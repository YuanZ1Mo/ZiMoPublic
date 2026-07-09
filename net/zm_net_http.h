/**
 * @file zm_net_http.h
 * @brief 基于 libevent 的多线程 HTTP 服务器，支持通用 HTTP 请求和 JSON-RPC 2.0 协议
 *
 * 提供三层能力:
 *   - ZmHttpdTask:     封装单个 HTTP 请求的读写操作（URI 解析、请求头/响应头、响应体）
 *   - ZmHttpServer:    多线程 HTTP 服务器，每个请求由独立工作线程处理，不阻塞事件循环
 *   - ZmJsonRpcServer: 在 ZmHttpServer 基础上实现 JSON-RPC 2.0 协议解析与分发
 *
 * 线程模型:
 *   主线程运行 libevent 事件循环接收请求，每个新请求创建一个 ZmHttpdDoer 工作线程，
 *   工作线程处理完毕后通过 event_active 通知事件循环线程发送响应。
 */

#ifndef ZM_NET_HTTP_H
#define ZM_NET_HTTP_H

#include "../json/zm_json.h"
#include "../util/zm_util_str.h"

#include <../libevent/include/event2/http.h>
#include <../libevent/include/event2/keyvalq_struct.h>
#include <../libevent/include/event2/event.h>

#include <stdint.h>
#include <string_view>

#define JRPC_VERSION "2.0"

// 2xx 成功
#define ZM_HTTP_STATUS_CODE_OK                    200  // 请求成功
#define ZM_HTTP_STATUS_CODE_CREATED               201  // 已创建（资源新建成功）
#define ZM_HTTP_STATUS_CODE_ACCEPTED              202  // 已接受（异步处理中）
#define ZM_HTTP_STATUS_CODE_NO_CONTENT            204  // 无内容（操作成功，无返回体）
#define ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT       206  // 部分内容（断点续传 / Range 响应）
                                                   
// 3xx 重定向                                      
#define ZM_HTTP_STATUS_CODE_MOVED_PERMANENTLY     301  // 永久重定向
#define ZM_HTTP_STATUS_CODE_FOUND                 302  // 临时重定向
#define ZM_HTTP_STATUS_CODE_NOT_MODIFIED          304  // 未修改（缓存命中）
#define ZM_HTTP_STATUS_CODE_TEMPORARY_REDIRECT    307  // 临时重定向（保持原方法）
                                                   
// 4xx 客户端错误                                  
#define ZM_HTTP_STATUS_CODE_BAD_REQUEST           400  // 请求无效（参数格式/必填字段缺失）
#define ZM_HTTP_STATUS_CODE_UNAUTHORIZED          401  // 未授权（需认证）
#define ZM_HTTP_STATUS_CODE_FORBIDDEN             403  // 禁止访问（已认证但无权限）
#define ZM_HTTP_STATUS_CODE_NOT_FOUND             404  // 资源不存在
#define ZM_HTTP_STATUS_CODE_METHOD_NOT_ALLOWED    405  // 方法不允许（如 PUT 访问只读接口）
#define ZM_HTTP_STATUS_CODE_REQUEST_TIMEOUT       408  // 请求超时
#define ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE 416 // 请求范围无法满足（Range 头非法）
#define ZM_HTTP_STATUS_CODE_TOO_MANY_REQUESTS     429  // 请求过于频繁（频率限制）
                                                   
// 5xx 服务端错误                                  
#define ZM_HTTP_STATUS_CODE_INTERNAL_ERROR        500  // 服务器内部错误
#define ZM_HTTP_STATUS_CODE_NOT_IMPLEMENTED       501  // 功能未实现
#define ZM_HTTP_STATUS_CODE_BAD_GATEWAY           502  // 网关错误（上游返回无效响应）
#define ZM_HTTP_STATUS_CODE_SERVICE_UNAVAILABLE   503  // 服务不可用（过载/维护中）
#define ZM_HTTP_STATUS_CODE_GATEWAY_TIMEOUT       504  // 网关超时（上游响应超时）

// 前向声明（头文件中仅通过指针使用）
class ZmThreadPool;
class ZmHttpdDoer;
class ZmHttpdDoerPool;
struct evbuffer;


/*
* HTTP Status Codes:
* 100 Continue, 101 Switching Protocols
* 200 OK, 201 Created, 202 Accepted, 204 No Content, 206 Partial Content
* 301 Moved Permanently, 302 Found, 304 Not Modified, 307 Temporary Redirect
* 400 Bad Request, 401 Unauthorized, 403 Forbidden, 404 Not Found,
* 405 Method Not Allowed, 408 Request Timeout, 409 Conflict, 410 Gone
* 500 Internal Server Error, 501 Not Implemented, 502 Bad Gateway,
* 503 Service Unavailable, 504 Gateway Timeout
*/

typedef enum
{
    ZM_HTTP_VERB_NONE = 0x00,
    ZM_HTTP_VERB_GET = 0x01,
    ZM_HTTP_VERB_POST = 0x02,
    ZM_HTTP_VERB_CONNECT = 0x03,
    ZM_HTTP_VERB_PUT = 0x04,
    ZM_HTTP_VERB_DELETE = 0x05,
    ZM_HTTP_VERB_HEAD = 0x06,
    ZM_HTTP_VERB_OPTIONS = 0x07,
    ZM_HTTP_VERB_TRACE = 0x08,
    ZM_HTTP_VERB_PATCH = 0x09,
}ZM_HTTP_VERB;

//
// https://www.ietf.org/rfc/rfc2732.txt
// Format for Literal IPv6 Addresses in URL's
// host          = hostname | IPv4address | IPv6reference
// ipv6reference = "[" IPv6address "]"
//
// SHOULD enclose IPv6 address in square brackets.
//
typedef struct strust_http_req
{
    char                    method[16];     /* GET POST HEAD PUT DELETE OPTIONS TRACE CONNECT PATCH SPCONN ...  */
    uint8_t                 verb;           /* method / verbs */
    char                    major;			/* HTTP Major number */
    char                    minor;			/* HTTP Minor number */
    char                    scheme[16];     /* http https */
    char* host;           /* www.astraliser.net */
    uint16_t                port;           /* 80 443 ... */
    uint16_t                websocket;      /* Is websocket request */
    char* userinfo;       /*  */
    char* path;           /* /index.php?name=kp */
    char* useragent;       /* such as curl/7.64.1 */
    /* char* query; char* fragment; */

    uint32_t				pid = 0;		/* #19684 */

    // 默认构造函数
    strust_http_req() {
        Init();
    }

    // 清除
    void Init() {

        memset(method, 0, sizeof(method));
        memset(scheme, 0, sizeof(scheme));

        verb = 0;
        major = 0;
        minor = 0;
        host = nullptr;
        port = 0;
        websocket = 0;
        userinfo = nullptr;
        path = nullptr;
        useragent = nullptr;
        pid = 0;
    }

}ZM_HTTP_REQ;

typedef struct
{
    char* path;
    size_t      qcnt;
    struct
    {
        char* name;
        char* value;
    }query[16];
    char* fragment;
}ZM_HTTP_URI;

class ZmHttpUtil
{
private:
    ZmHttpUtil() {}

public:
    ~ZmHttpUtil() {}

    /** Parse the HTTP verbs by the method name, like as 'GET'->ZM_HTTP_VERB_GET */
    static int          ParseVerb(std::string_view method);
    /** Parse the HTTP request first line and return the verbs type */
    static int          StartWithVerbs(std::string_view buf);
    /** Parse the HTTP request first line */
    static bool         ParseRequest(ZM_HTTP_REQ* req, std::string_view line, int verb = 0);
    /** Parse a HTTP request url */
    static void         ParseUri(ZM_HTTP_REQ* req, std::string_view uri);
    /** Parse Path part of HTTP request url */
    static void         ParseUriPath(ZM_HTTP_URI* uri, char* path);
    /** Get query value by name from Query part of URL */
    static const char* GetQuery(const ZM_HTTP_URI* uri, std::string_view name);

    static ZM_HTTP_REQ* CreateRequest();
    static void         FreeRequest(ZM_HTTP_REQ* req);

    static std::string  HeaderGetValue(struct evkeyvalq* headers, std::string_view key, std::string_view defv = "");

    /** Parse Status-Code from Status-Line */
    static int          ParseStatusCode(std::string_view statusLine, std::string_view limit);
};




/**
 * @brief 封装单个 HTTP 请求的上下文，提供请求读取和响应写入的抽象接口
 *
 * 在工作线程中使用。工作线程通过此对象读取请求信息（URI、方法、请求头），
 * 并写入响应（状态码、响应头、响应体）。实际的响应发送在事件循环线程中完成。
 *
 * @example 基本响应流程
 * @code
 *   task->PutReplyHeader("Content-type", "text/plain");
 *   task->SetReply(200, "OK");
 *   task->SetReplyData((const BYTE*)"hello", 5);
 * @endcode
 */
class ZmHttpdTask
{
public:
    /**
     * @brief 构造请求上下文，解析 URI 中的 query 参数并创建响应缓冲区
     * @param request  libevent 的 HTTP 请求对象，由事件循环传入
     */
    ZmHttpdTask(struct evhttp_request* request);

    /**
     * @brief 析构，释放 query 参数表和响应缓冲区
     */
    ~ZmHttpdTask();

    /** @brief 获取底层的 libevent 请求对象 */
    struct evhttp_request* Request();

    /** @brief 获取请求的 HTTP 方法（GET/POST/PUT 等） */
    evhttp_cmd_type Method();

    /** @brief 获取请求的 URI 路径（含 query string），例如 "/api?foo=bar" */
    const char* Uri();

    const char* Ip();
    ev_uint16_t Port();

    /** @brief 获取请求的唯一追踪 ID（自增序号，从 1 开始，进程内唯一） */
    uint64_t Id();

    /**
     * @brief 获取 URI query string 中指定参数的值
     * @param name   参数名
     * @param defv   参数不存在时的默认返回值，默认为空字符串
     * @return       参数值字符串指针，内部缓冲区，不需要调用者释放
     *
     * @example
     * @code
     *   // URI 为 /api?token=abc123
     *   const char* token = task->GetQueryValue("token");       // "abc123"
     *   const char* page  = task->GetQueryValue("page", "1");   // "1"（未传时使用默认值）
     * @endcode
     */
    const char* GetQueryValue(std::string_view name, std::string_view defv = "");

    /**
     * @brief 将所有请求头导出为 JSON 对象
     * @param headersObj  输出的 JSON 对象，键为请求头名称，值为请求头内容
     *
     * @example
     * @code
     *   nlohmann::json::object_t headers;
     *   task->GetRequestHeaders(headers);
     *   // headers["User-Agent"] == "Mozilla/5.0 ..."
     * @endcode
     */
    void GetRequestHeaders(nlohmann::json::object_t& headersObj);

    /**
     * @brief 获取指定请求头的值
     * @param name   请求头名称（不区分大小写）
     * @param defv   请求头不存在时的默认返回值
     * @return       请求头值字符串指针，内部缓冲区
     */
    const char* GetRequestHeader(std::string_view name, std::string_view defv = "");

    /**
     * @brief 设置响应头（延迟到发送响应时写入）
     * @param name  响应头名称
     * @param val   响应头值，传入 nullptr 时存储为空字符串
     *
     * @note 可在 SendReply 之前多次调用，同名字段会被覆盖
     */
    void PutReplyHeader(std::string_view name, std::string_view val);

    /**
     * @brief 设置响应状态码和原因短语
     * @param code    HTTP 状态码，如 200、404、500
     * @param reason  原因短语，如 "OK"、"Not Found"，传入 nullptr 时由 libevent 自动填充
     */
    void SetReply(int code, const char* reason = nullptr);

    /**
     * @brief 设置响应体数据（追加到响应缓冲区）
     * @param data  响应体数据的字节指针
     * @param dlen  响应体数据长度（字节数）
     *
     * @note 传入 nullptr 或 dlen 为 0 时不执行任何操作
     */
    void SetReplyData(const BYTE* data = nullptr, size_t dlen = 0);

    /**
     * @brief 将一个 evbuffer 的全部内容追加到响应缓冲区
     * @param buf  源 evbuffer，传输完成后源 buffer 内容被消费
     */
    void SetReplyBuf(struct evbuffer* buf = nullptr);

    /**
     * @brief 零拷贝方式向响应缓冲区添加文件内容
     *
     * 使用 evbuffer_file_segment 替代已废弃的 evbuffer_add_file，
     * 支持 mmap/sendfile 零拷贝。函数接管 fd 所有权，
     * 传输完成后由 libevent 自动关闭 fd。
     *
     * @param fd      文件描述符（函数接管所有权，成功后调用者不应再操作 fd）
     * @param offset  文件读取起始偏移量（字节）
     * @param length  读取长度（字节数）
     * @return 0 成功，-1 失败（调用者需自行 close(fd)）
     */
    int SetReplyFile(int fd, ev_off_t offset, ev_off_t length);

    /**
     * @brief 清空已写入的响应体数据（保留状态码和响应头）
     *
     * 适用于异常恢复场景：handler 部分写入后抛异常，
     * 中间件可清空脏数据后重写错误响应。
     */
    void ClearReplyBody();

    /**
     * @brief 发送被延迟的 HTTP 响应
     *
     * 通过 event_active 将 REPLY 信号投递到 HTTP 服务器的 event loop 线程，
     * 由 event loop 调用 SendReply() 实际发送 evhttp_send_reply。
     *
     * @note 可在任意线程调用（event_active 线程安全）。
     *       调用前需确保已通过 SetReply / SetReplyData / PutReplyHeader 设置好响应内容。
     */
    void TriggerReply();

protected:
    /** @brief 设置延迟回复回调（由 ZmHttpdDoer 在构造时调用） */
    void SetReplyCallback(std::function<void()> cb);

protected:
    friend class ZmHttpdDoer;

    std::function<void()> m_on_reply;                ///< 回复信号回调
    /** @brief 底层 libevent HTTP 请求对象 */
    struct evhttp_request*              m_request;

    /** @brief 请求追踪 ID（构造时自增分配，进程内唯一，用于日志关联请求和响应） */
    uint64_t                            m_id;

    /** @brief URI query string 解析后的键值对表（由 evhttp_parse_query_str 填充） */
    struct evkeyvalq                    m_query;

    /** @brief HTTP 响应状态码 */
    int                                 m_status_code;

    /** @brief HTTP 响应原因短语 */
    std::string                         m_reason;

    /** @brief 待写入的响应头集合（支持同名重复，如多个 Set-Cookie），在事件循环线程发送响应时统一写入 */
    std::vector<std::pair<std::string, std::string>>  m_reply_headers;

    /** @brief 响应体缓冲区 */
    struct evbuffer*                    m_reply_buf;

    /** @brief 请求体原始 evbuffer（由 Perform 设置，用于分块读取大文件） */
    struct evbuffer*                    m_input_buf;

public:
    /**
     * @brief 获取请求体原始 evbuffer，用于分块读取大文件上传
     * @return 请求体缓冲区指针，无请求体时返回 nullptr
     *
     * @note 配合 DrainInputBody() 使用：evbuffer_copyout 读取一块 → 处理 → DrainInputBody 排空已处理数据
     * @example
     *   struct evbuffer* in = task->GetInputBuffer();
     *   while (evbuffer_get_length(in) > 0) {
     *       BYTE buf[262144];
     *       size_t n = evbuffer_copyout(in, buf, sizeof(buf));
     *       _write(fd, buf, n);
     *       task->DrainInputBody(n);
     *   }
     */
    struct evbuffer* GetInputBuffer() const;

    /**
     * @brief 设置请求体原始 evbuffer 引用（由 ZmHttpServer::Perform 调用）
     * @param buf 请求体缓冲区指针
     */
    void SetInputBuffer(struct evbuffer* buf);

    /**
     * @brief 从请求体缓冲区头部排空指定字节数
     * @param len 要排空的字节数
     *
     * @note 与 GetInputBuffer() 配合使用，每次处理完一块数据后调用
     */
    void DrainInputBody(size_t len);
};

class ZmHttpHead
{
public:
    ZmHttpHead();
    ~ZmHttpHead();

    int         StatusCode();
    int         ContentLength();

    void        Parse(std::string_view buf, size_t len, bool hasReqLine = false);
    void        Build(ZmByteBuffer& output);
    void        BuildToBuffer(struct evbuffer* buf);
    void        PutAll(ZmHttpHead* other);

    const char* PutValue(const char* name, const char* fmt, ...);
    const char* Value(const char* name, const char* value = nullptr);
    void        Remove(std::string_view name);
    void        SetHostField(std::string_view scheme, std::string_view host, uint16_t port);

    bool        IsEmpty();

private:
    typedef struct
    {
        char* name;
        char* value;
    }_ENTRY;
    ZmHttpHead::_ENTRY* QueryEntry(std::string_view name);

private:
    int                 _status_code;
    ZmArrayList<_ENTRY> _entries;
};


/**
 * @brief 多线程 HTTP 服务器，基于外部 libevent 事件循环驱动
 *
 * 不再继承 ZmThread。构造函数接受外部 event_base，不创建独立事件循环线程。
 * 每个进入的 HTTP 请求会被分配给线程池中的工作线程处理，不阻塞事件循环接收新请求。
 *
 * 线程交互:
 *   - 事件循环线程: 接收请求 → 创建 ZmHttpdDoer → 提交到线程池
 *   - 工作线程:     执行 Perform() → 通过 event_active 通知事件循环线程发送响应
 *   - 事件循环线程: 收到通知 → SendReply() → 启动 1 秒定时器 → 延迟释放资源
 *
 * @note 事件循环由外部（如 ZmEvBaseRunLoop）管理，ZmHttpServer 不负责事件循环的启停
 *
 * @example 基本使用
 * @code
 *   ZmHttpServer server(evbase, 8080);
 *   server.SetRequestCallback([](ZmHttpdTask* task, const BYTE* data, size_t dlen) -> int {
 *       task->PutReplyHeader("Content-type", "text/plain");
 *       task->SetReplyData((const BYTE*)"hello", 5);
 *       return 200;
 *   });
 *   server.Init();
 * @endcode
 */
class ZmHttpServer
{
public:
    /**
     * @brief 事件循环内部控制事件类型，用于线程间通信
     *
     * 通过 event_active 的 what 参数传递，事件循环线程根据类型执行对应操作:
     *   - ZM_HTTPD_CONTROL_REPLY:     工作线程请求发送 HTTP 响应
     *   - ZM_HTTPD_CONTROL_REPLY_END: 定时器到期，释放 ZmHttpdDoer 资源
     */
    enum
    {
        ZM_HTTPD_CONTROL_REPLY     = 0x0200,   ///< 工作线程请求发送响应
    };

    /**
     * @brief HTTP 请求处理回调函数类型
     * @param task  请求上下文对象
     * @param data  请求体原始字节
     * @param dlen  请求体长度
     * @return      HTTP 状态码，返回 0 表示此路径不支持（将被覆盖为 404）
     */
    typedef std::function<int(ZmHttpdTask*, const BYTE*, size_t)> OnHttpdRequestCB;

    /**
     * @brief 构造 HTTP 服务器
     * @param evbase     外部 libevent 事件循环对象（不由此类接管生命周期）
     * @param local_port 监听端口号
     */
    ZmHttpServer(struct event_base* evbase, uint16_t local_port);

    /** @brief 析构，释放 evhttp 和控制事件（不释放外部 event_base） */
    virtual ~ZmHttpServer();

    /**
     * @brief 初始化 HTTP 服务器：绑定端口、创建工作线程池和控制事件
     * @return true 初始化成功
     */
    bool Init();

    /** @brief 关闭 HTTP 服务器：停止线程池、释放控制事件和 evhttp（不释放外部 event_base） */
    void Close();

    /** @brief 查询服务器是否已初始化 */
    bool IsOpen() const;

    /**
     * @brief 设置内部线程池名称（调试时 VS 线程列表可见）
     * @param name  新名称，如 "JRPC-39440"
     */
    void SetPoolName(const std::string& name);

    /** @brief 获取监听端口号 */
    uint16_t           LocalPort();

    /** @brief 获取底层 libevent 事件循环对象 */
    struct event_base* EventBase();

    /**
     * @brief 从对象池获取或新建 ZmHttpdDoer（事件循环线程调用）
     * @param request  libevent 请求对象
     * @return         ZmHttpdDoer 实例指针
     */
    ZmHttpdDoer* AcquireDoer(struct evhttp_request* request);

    /**
     * @brief 回收 ZmHttpdDoer 到对象池（事件循环线程调用）
     *
     * 若对象池未初始化（shutdown 后兜底），直接 delete。
     *
     * @param doer  待回收的 ZmHttpdDoer 实例
     */
    void RecycleDoer(ZmHttpdDoer* doer);

    /**
     * @brief 设置通用 HTTP 请求处理回调
     * @param onreq  回调函数，处理非 JSON-RPC 路径的 HTTP 请求
     */
    void               SetRequestCallback(OnHttpdRequestCB onreq);

    /**
     * @brief 执行 HTTP 请求处理流程（由工作线程调用）
     *
     * 处理流程:
     *   1. 获取客户端 IP 并忽略 SIGPIPE
     *   2. 自动添加跨域（CORS）响应头
     *   3. OPTIONS 请求直接返回 200
     *   4. 调用 OnHttpdRequest 执行业务逻辑
     *   5. 业务逻辑返回 0 时设置 404
     *
     * @param task  请求上下文对象
     */
    void               Perform(ZmHttpdTask* task);

    /**
     * @brief libevent 通用请求回调，每个新请求触发（事件循环线程执行）
     * @param request  libevent 请求对象
     * @param arg      ZmHttpServer 实例指针
     *
     * @note 创建 ZmHttpdDoer 并提交到线程池处理请求
     */
    static void OnHttpRequestCB(struct evhttp_request* request, void* arg);

    /**
     * @brief 事件循环内部控制事件回调，处理线程间通信信号
     * @param fd    未使用（事件无 socket）
     * @param what  触发的事件标志位，与 ZM_HTTPD_CONTROL_* 枚举按位与判断
     * @param ctx   事件关联的对象指针（ZmHttpdDoer 或 ZmHttpServer）
     */
    static void OnEventControl(evutil_socket_t fd, short what, void* ctx);

protected:
    /**
     * @brief 处理 HTTP 请求的业务逻辑（虚函数，子类可重写）
     * @param task  请求上下文对象
     * @param data  请求体原始字节
     * @param dlen  请求体长度
     * @return      HTTP 状态码，0 表示此路径不支持
     *
     * @note 默认实现调用 m_on_request 回调；若未设置回调则返回 0
     */
    virtual int  OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen);

    /**
     * @brief 将 libevent HTTP 服务器绑定到指定 event_base
     * @param evbase  libevent 事件循环对象
     * @return        true 绑定成功，false 端口绑定失败
     */
    bool BindEventBase(struct event_base* evbase);

private:
    /** @brief libevent 事件循环对象（外部传入，不由此类释放） */
    struct event_base* m_evbase;

    /** @brief libevent HTTP 服务器对象 */
    struct evhttp*     m_evhttpd;

    /** @brief 期望的线程池名称（Init 前设置则用此名，否则默认 "Http-{port}"） */
    std::string        m_threadPoolName;

    /** @brief 工作线程池（复用线程处理请求，替代 thread-per-request） */
    ZmThreadPool*      m_threadPool;

    /** @brief 监听端口号 */
    uint16_t           m_local_port;

    /** @brief 端口绑定是否失败（BindEventBase 中设置） */
    bool               m_port_bind_failed;

    /** @brief 通用 HTTP 请求处理回调 */
    OnHttpdRequestCB   m_on_request;

    /** @brief ZmHttpdDoer 对象池（事件循环线程独享，无需锁） */
    ZmHttpdDoerPool*   m_httpdDoerPool;
};


/*
* JSON-RPC 2.0 标准错误码（https://www.jsonrpc.org/specification）
* -32600	Invalid Request	不是有效的 JSON-RPC 2.0 请求
* -32601	Method not found	请求的方法不存在
* -32602	Invalid params	参数无效或不正确
* -32603	Internal error	服务器内部错误
* -32700	Parse error	JSON 解析错误
* -32000 to -32099	Server error	服务器定义的错误（保留范围）
*
* -32000    Internal portal does not have JRPC processing channel set up 门户未设置jrpcReq回调函数
*/
#define ZM_JRPC_ERR_INVALID_REQUEST  -32600  // 无效请求（不是有效的 JSON-RPC 2.0 请求）
#define ZM_JRPC_ERR_METHOD_NOT_FOUND -32601  // 方法不存在
#define ZM_JRPC_ERR_INVALID_PARAMS   -32602  // 参数无效（params 格式/类型不符）
#define ZM_JRPC_ERR_INTERNAL         -32603  // 内部错误（服务端处理异常）
#define ZM_JRPC_ERR_PARSE            -32700  // 解析错误（JSON 格式非法）
// 自定义错误码（-32000 ~ -32099 为 JSON-RPC 2.0 保留的服务端自定义范围）
#define ZM_JRPC_ERR_PORTAL_NOJRPC    -32000  // 门户未设置 JRPC 回调
#define ZM_JRPC_ERR_EMPTY_RSP        -32001  // 响应为空（上游回复空内容）
#define ZM_JRPC_ERR_FORMAT           -32002  // 响应格式错误（上游 JSON 解析失败）
#define ZM_JRPC_ERR_DROPPED          -32003  // 响应被丢弃（通道关闭/超时）

/**
 * @brief JSON-RPC 2.0 协议服务器，在 ZmHttpServer 基础上增加 RPC 解析与分发
 *
 * 仅处理 URI 以 root_uri 开头的请求（如 "/rpc"），其他请求交给父类处理。
 * 支持 GET（通过 query string 传参，jsonbody 为 Base64 编码）和 POST（请求体为 JSON）。
 * 支持 JSONP 回调（通过 query string 的 callback 参数）。
 *
 * @example 使用方式
 * @code
 *   ZmJsonRpcServer server("/rpc", 39440);
 *   server.SetJsonRpcCBEx([](ZmHttpdTask* task, const std::string& method,
 *       const ZMJSON& params, ZMJSON& result, ZMJSON& error) -> int {
 *       if (method == "add") {
 *           result["sum"] = params["a"].get<int>() + params["b"].get<int>();
 *           return 0;
 *       }
 *       return -1;  // method not found
 *   });
 *   server.Startup();
 * @endcode
 */
class ZmJsonRpcServer : public ZmHttpServer
{
public:
    /**
     * @brief JSON-RPC 请求处理回调（带 task 参数），优先级高于 OnJsonRpcRequestCB
     * @param  task    请求上下文对象
     * @param  method  RPC 方法名
     * @param  params  RPC 参数对象
     * @param  result  输出结果对象
     * @param  error   输出错误对象
     * @header header  请求头设置
     * @return        >= 0 表示方法已处理，< 0 表示方法未找到
     */
    typedef std::function<int(ZmHttpdTask* task, const ZMJSON& request, ZMJSON& response)> OnJsonRpcRequestCB;

    /**
     * @brief JSON-RPC 异步请求处理回调（优先级最高）
     * @param task    请求上下文对象
     * @param method  RPC 方法名
     * @param params  RPC 参数对象
     * @param reply   响应回调，业务层处理完成后调用 reply(result, error, ) 发送响应
     *
     * 与同步回调不同，本回调立即返回（不阻塞 Worker 线程）。业务层在异步处理
     * 完成后调用 reply 函数，reply 内部构造 JSON-RPC 2.0 响应并通过
     * SendDeferredReply 发送回 HTTP 客户端。
     *
     * @note 设置了异步回调后，同步回调被忽略。
     * @note reply 可在任意线程调用（内部通过 event_active 安全投递到 HTTP event loop）。
     */
    typedef std::function<void(ZmHttpdTask* task, const ZMJSON& request,
        std::function<void(const ZMJSON& response)> replyCB)>
        OnJsonRpcRequestCBAsync;

    /**
     * @brief 构造 JSON-RPC 服务器
     * @param evbase      外部 libevent 事件循环对象
     * @param root_uri    RPC 请求的 URI 前缀，仅匹配此前缀的请求走 RPC 流程，为空或 nullptr 时所有请求走 RPC
     * @param local_port  监听端口号
     */
    ZmJsonRpcServer(struct event_base* evbase, std::string_view root_uri, uint16_t local_port);

    /** @brief 析构 */
    virtual ~ZmJsonRpcServer();

    /**
     * @brief 快速构造一个 JSON-RPC 错误响应对象
     * @param code     错误码（遵循 JSON-RPC 2.0 规范）
     * @param message  错误描述
     * @return         形如 {"code": code, "message": message} 的 JSON 对象
     *
     * @example
     * @code
     *   error = ZmJsonRpcServer::MakeError(-32601, "Method not found");
     * @endcode
     */
    static ZMJSON MakeError(int code, std::string_view message);

    /**
     * @brief 设置 JSON-RPC 同步请求回调
     * @param oncall  回调函数
     *
     * @note 回调函数在业务层不要返回0, 因为返回0表示没设置同步回调
     *       回调函数返回 < 0 表示该 method 不被业务层接收
     */
    void SetJsonRpcCB(OnJsonRpcRequestCB oncall);

    /**
     * @brief 设置 JSON-RPC 异步请求回调（优先级最高）
     * @param oncall_async  异步回调函数
     *
     * @note 设置后同步回调被忽略，所有匹配 root_uri 的请求走异步路径。
     *       异步回调中业务层调用 reply(result, error) 发送响应，框架自动构造
     *       JSON-RPC 2.0 标准响应信封并通过 task->SendDeferredReply() 发送。
     */
    void SetJsonRpcCBAsync(OnJsonRpcRequestCBAsync oncall_async);

protected:
    /**
     * @brief 分发 JSON-RPC 请求到注册的回调（虚函数，子类可重写）
     * @param task    请求上下文对象
     * @param method  RPC 方法名
     * @param params  RPC 参数对象
     * @param result  输出结果对象
     * @param error   输出错误对象
     * @return        >= 0 表示方法已处理，< 0 表示方法未找到
     *
     * @note 分发优先级: CBEx > CB > 返回 -1
     */
    virtual int OnJsonRpcRequest(ZmHttpdTask* task, const ZMJSON& request, ZMJSON& response);

    /**
    * @brief 分发 JSON-RPC 请求到注册的异步回调（虚函数，子类可重写）
    * @param task    请求上下文对象
    * @param method  RPC 方法名
    * @param params  RPC 参数对象
    * @param reply   输出结果对象
    * @return        true 表示方法已回调到异步函数，false 表示没设置
    *
    * @note 分发优先级: CBEx > CB > 返回 -1
    */
    virtual bool OnJsonRpcRequestAsync(ZmHttpdTask* task, const ZMJSON& request, ZMJSON& reply);

    /**
     * @brief 处理 HTTP 请求，解析 JSON-RPC 协议并构造响应（重写父类虚函数）
     *
     * 处理流程:
     *   1. URI 不匹配 root_uri 时交给父类处理
     *   2. GET 请求从 query string 读取 Base64 编码的 jsonbody 并解码
     *   3. 解析 JSON 请求体，校验 method/params 字段
     *   4. 调用 OnJsonRpcRequest 分发到业务回调
     *   5. 按 JSON-RPC 2.0 规范构造响应（result 和 error 二选一）
     *   6. 支持 JSONP 模式（callback 参数非空时包裹为 callback(json)）
     *
     * @param task  请求上下文对象
     * @param data  请求体原始字节
     * @param dlen  请求体长度
     * @return      始终返回 200（错误信息在响应体的 JSON error 字段中）
     */
    virtual int OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen);

    /**
     * @brief 构造 JSON-RPC HTTP 响应并写入 task（同步和异步路径共用）
     *
     * 根据请求中是否携带 callback 参数自动选择响应格式：
     * - 无 callback → 标准 JSON 响应，Content-Type: application/json
     * - 有 callback → JSONP 格式 callback(json)，Content-Type: application/javascript
     *
     * 同时设置 Server 响应头（含服务端版本号）、HTTP 状态码 200 和响应体。
     * 调用者负责触发实际发送（同步路径由框架自动发送，异步路径需调用 SendDeferredReply）。
     *
     * @param task         请求上下文对象（从中读取 callback 参数）
     * @param rsp_envelope 已填充 jsonrpc/id/method/result/error 的响应 JSON 对象
     */
    void BuildJsonRpcResponse(ZmHttpdTask* task, ZMJSON& reply, const ZMJSON& response);

    /**
     * @brief 异步 JRPC 请求的回复回调（静态成员函数，供 OnJsonRpcRequestAsync 中 std::bind 使用）
     * @param server  ZmJsonRpcServer 实例指针
     * @param task    请求上下文对象
     * @param reply   响应信封 JSON 对象引用
     * @param result  业务层返回的结果对象（is_null() 表示无结果）
     * @param error   业务层返回的错误对象（is_null() 表示无错误）
     */
    static void OnJsonRpcAsyncReply(ZmJsonRpcServer* server, ZmHttpdTask* task, ZMJSON& reply, const ZMJSON& response);

private:
    /** @brief JSON-RPC 请求回调, 返回 < 0 表示该 method 不存在, 所以业务层不要返回0,因为返回0表示没设置同步回调 */
    OnJsonRpcRequestCB    m_on_jsonrpc_call;

    /** @brief JSON-RPC 异步请求回调（优先级最高，设置后忽略同步回调） */
    OnJsonRpcRequestCBAsync m_on_jsonrpc_call_async;

    /** @brief RPC 请求的 URI 前缀，匹配此前缀的请求走 RPC 流程 */
    char                    m_root_uri[128];
};


#endif /* ZM_NET_HTTP_H */
