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
#include "../util/zm_util_thread.h"

#include <stdint.h>

#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/event.h>
#include <event2/buffer.h>


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
        host = NULL;
        port = 0;
        websocket = 0;
        userinfo = NULL;
        path = NULL;
        useragent = NULL;
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

    /** Parse the HTTP verbs by the method name, like as 'GET'->SP_HTTP_VERB_GET */
    static int          ParseVerb(const char* method);
    /** Parse the HTTP request first line and return the verbs type */
    static int          StartWithVerbs(const char* buf);
    /** Parse the HTTP request first line */
    static bool         ParseRequest(ZM_HTTP_REQ* req, const char* line, int verb = 0);
    /** Parse a HTTP request url */
    static void         ParseUri(ZM_HTTP_REQ* req, const char* uri);
    /** Parse Path part of HTTP request url */
    static void         ParseUriPath(ZM_HTTP_URI* uri, char* path);
    /** Get query value by name from Query part of URL */
    static const char* GetQuery(const ZM_HTTP_URI* uri, const char* name);

    static ZM_HTTP_REQ* CreateRequest();
    static void         FreeRequest(ZM_HTTP_REQ* req);

    static std::string  HeaderGetValue(struct evkeyvalq* headers, const char* key, const char* defv = "");

    /** Parse Status-Code from Status-Line */
    static int          ParseStatusCode(const char* statusLine, const char* limit);
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
    evhttp_cmd_type        Method();

    /** @brief 获取请求的 URI 路径（含 query string），例如 "/api?foo=bar" */
    const char*            Uri();

    const char* Ip();
    ev_uint16_t Port();

    /** @brief 获取请求的唯一追踪 ID（自增序号，从 1 开始，进程内唯一） */
    uint64_t    Id();

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
    const char*            GetQueryValue(const char* name, const char* defv = "");

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
    void                   GetRequestHeaders(nlohmann::json::object_t& headersObj);

    /**
     * @brief 获取指定请求头的值
     * @param name   请求头名称（不区分大小写）
     * @param defv   请求头不存在时的默认返回值
     * @return       请求头值字符串指针，内部缓冲区
     */
    const char*            GetRequestHeader(const char* name, const char* defv = "");

    /**
     * @brief 设置响应头（延迟到发送响应时写入）
     * @param name  响应头名称
     * @param val   响应头值，传入 NULL 时存储为空字符串
     *
     * @note 可在 SendReply 之前多次调用，同名字段会被覆盖
     */
    void PutReplyHeader(const char* name, const char* val);

    /**
     * @brief 设置响应状态码和原因短语
     * @param code    HTTP 状态码，如 200、404、500
     * @param reason  原因短语，如 "OK"、"Not Found"，传入 NULL 时由 libevent 自动填充
     */
    void SetReply(int code, const char* reason = NULL);

    /**
     * @brief 设置响应体数据（追加到响应缓冲区）
     * @param data  响应体数据的字节指针
     * @param dlen  响应体数据长度（字节数）
     *
     * @note 传入 NULL 或 dlen 为 0 时不执行任何操作
     */
    void SetReplyData(const BYTE* data = NULL, size_t dlen = 0);

    /**
     * @brief 将一个 evbuffer 的全部内容追加到响应缓冲区
     * @param buf  源 evbuffer，传输完成后源 buffer 内容被消费
     */
    void SetReplyBuf(struct evbuffer* buf = NULL);

    /**
     * @brief 清空已写入的响应体数据（保留状态码和响应头）
     *
     * 适用于异常恢复场景：handler 部分写入后抛异常，
     * 中间件可清空脏数据后重写错误响应。
     */
    void ClearReplyBody()
    {
        if (m_reply_buf)
            evbuffer_drain(m_reply_buf, evbuffer_get_length(m_reply_buf));
    }

protected:
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

    /** @brief 待写入的响应头集合，在事件循环线程发送响应时统一写入 */
    std::map<std::string, std::string>  m_reply_headers;

    /** @brief 响应体缓冲区 */
    struct evbuffer*                    m_reply_buf;
};

class ZmHttpHead
{
public:
    ZmHttpHead();
    ~ZmHttpHead();

    inline int  StatusCode() { return _status_code; }
    inline int  ContentLength()
    {
        const char* value = Value("Content-Length");
        return value ? atoi(value) : 0;
    }

    void        Parse(const char* buf, size_t len, bool hasReqLine = false);
    void        Build(ZmByteBuffer& output);
    void        BuildToBuffer(struct evbuffer* buf);
    void        PutAll(ZmHttpHead* other);

    const char* PutValue(const char* name, const char* fmt, ...);
    const char* Value(const char* name, const char* value = NULL);
    void        Remove(const char* name);
    void        SetHostField(const char* scheme, const char* host, uint16_t port);

    inline bool IsEmpty() { return (0 == _entries.Count()); }

private:
    typedef struct
    {
        char* name;
        char* value;
    }_ENTRY;
    ZmHttpHead::_ENTRY* QueryEntry(const char* name);

private:
    int                 _status_code;
    ZmArrayList<_ENTRY> _entries;
};


/**
 * @brief 多线程 HTTP 服务器，基于 libevent 事件循环驱动
 *
 * 继承自 ZmThread，在独立线程中运行 libevent 的事件循环。每个进入的 HTTP 请求
 * 会被分配给一个 ZmHttpdDoer 工作线程处理，不会阻塞事件循环接收新请求。
 *
 * 线程交互:
 *   - 事件循环线程: 接收请求 → 创建 ZmHttpdDoer → 启动工作线程
 *   - 工作线程:     执行 Perform() → 通过 event_active 通知事件循环线程发送响应
 *   - 事件循环线程: 收到通知 → SendReply() → 启动 1 秒定时器 → 延迟释放资源
 *
 * @example 基本使用
 * @code
 *   ZmHttpServer server(8080);
 *   server.SetRequestCallback([](ZmHttpdTask* task, const BYTE* data, size_t dlen) -> int {
 *       task->PutReplyHeader("Content-type", "text/plain");
 *       task->SetReplyData((const BYTE*)"hello", 5);
 *       return 200;
 *   });
 *   server.Startup();
 * @endcode
 */
class ZmHttpServer : public ZmThread
{
public:
    /**
     * @brief 事件循环内部控制事件类型，用于线程间通信
     *
     * 通过 event_active 的 what 参数传递，事件循环线程根据类型执行对应操作:
     *   - ZM_HTTPD_CONTROL_CLOSE:     通知事件循环退出（Shutdown 调用）
     *   - ZM_HTTPD_CONTROL_REPLY:     工作线程请求发送 HTTP 响应
     *   - ZM_HTTPD_CONTROL_REPLY_END: 定时器到期，释放 ZmHttpdDoer 资源
     */
    enum
    {
        ZM_HTTPD_CONTROL_CLOSE     = 0x0100,   ///< 通知事件循环退出
        ZM_HTTPD_CONTROL_REPLY     = 0x0200,   ///< 工作线程请求发送响应
        ZM_HTTPD_CONTROL_REPLY_END = 0x0400    ///< 响应发送完成，延迟释放资源
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
     * @param local_port  监听端口号
     */
    ZmHttpServer(uint16_t local_port);

    /** @brief 析构，释放所有 libevent 资源 */
    virtual ~ZmHttpServer();

    /** @brief 获取监听端口号 */
    uint16_t           LocalPort();

    /** @brief 获取底层 libevent 事件循环对象 */
    struct event_base* EventBase();

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
     * @note 创建 ZmHttpdDoer 工作线程处理请求，线程启动失败时返回 503
     */
    static void OnHttp_RequestCB(struct evhttp_request* request, void* arg);

    /**
     * @brief 事件循环内部控制事件回调，处理线程间通信信号
     * @param fd    未使用（事件无 socket）
     * @param what  触发的事件标志位，与 ZM_HTTPD_CONTROL_* 枚举按位与判断
     * @param ctx   事件关联的对象指针（ZmHttpdDoer 或 ZmHttpServer）
     */
    static void OnEvent_Control(evutil_socket_t fd, short what, void* ctx);

    /**
     * @brief 延迟释放定时器回调，在响应发送 1 秒后触发以销毁 ZmHttpdDoer
     * @param fd      未使用
     * @param event   未使用
     * @param arg     ZmHttpdDoer 实例指针
     */
    static void OnEvent_Timer(evutil_socket_t fd, short event, void* arg);

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
     * @brief 线程主函数（ZmThread 虚函数重写）
     *
     * 执行流程:
     *   1. 初始化 libevent 线程支持
     *   2. 创建 event_base 和 evhttp 服务器
     *   3. 绑定端口并进入事件循环（阻塞）
     *   4. 事件循环退出后释放资源
     */
    virtual void Run();

    /**
    * @brief 关闭 HTTP 服务器，安全终止事件循环
    *
    * 根据调用者所在线程采取不同策略:
    *   - 事件循环线程内: 直接调用 event_base_loopbreak
    *   - 其他线程且控制事件已创建: 通过 event_active 发送关闭信号
    *   - 其他线程且事件循环尚未初始化: 通过 event_base_loopbreak 直接打断
    */
    virtual void OnStopping() override;

    /**
     * @brief 将 libevent HTTP 服务器绑定到指定 event_base
     * @param evbase  libevent 事件循环对象
     * @return        true 绑定成功，false 端口绑定失败
     */
    bool BindEventBase(struct event_base* evbase);

    /** @brief 通过 event_base_loopbreak 终止事件循环 */
    void OnControlClose();

    /** @brief 按顺序释放 ctrl_event → evhttpd → evbase，全部置 NULL */
    void FreeEventObjects();

private:
    /** @brief libevent 事件循环对象 */
    struct event_base* m_evbase;

    /** @brief libevent HTTP 服务器对象 */
    struct evhttp*     m_evhttpd;

    /** @brief 用于接收外部线程控制信号的事件（关闭、响应等） */
    struct event*      m_ctrl_event;

    /** @brief 工作线程池（复用线程处理请求，替代 thread-per-request） */
    ZmThreadPool*      m_pool;

    /** @brief 监听端口号 */
    uint16_t           m_local_port;

    /** @brief 端口绑定是否失败（BindEventBase 中设置） */
    bool               m_port_bind_failed;

    /** @brief 是否已请求关闭（防止 Shutdown 后仍进入事件循环） */
    bool               m_shutdown_requested;

    /** @brief 通用 HTTP 请求处理回调 */
    OnHttpdRequestCB   m_on_request;
};


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
     * @param task    请求上下文对象
     * @param method  RPC 方法名
     * @param params  RPC 参数对象
     * @param result  输出结果对象
     * @param error   输出错误对象
     * @return        >= 0 表示方法已处理，< 0 表示方法未找到
     */
    typedef std::function<int(ZmHttpdTask*, const std::string&, const ZMJSON&,
        ZMJSON&, ZMJSON&)> OnJsonRpcRequestCBEx;

    /**
     * @brief JSON-RPC 请求处理回调（不带 task 参数）
     * @param method  RPC 方法名
     * @param params  RPC 参数对象
     * @param result  输出结果对象
     * @param error   输出错误对象
     * @return        >= 0 表示方法已处理，< 0 表示方法未找到
     */
    typedef std::function<int(const std::string&, const ZMJSON&,
        ZMJSON&, ZMJSON&)> OnJsonRpcRequestCB;

    /**
     * @brief 构造 JSON-RPC 服务器
     * @param root_uri     RPC 请求的 URI 前缀，仅匹配此前缀的请求走 RPC 流程，为空或 NULL 时所有请求走 RPC
     * @param local_port   监听端口号
     */
    ZmJsonRpcServer(const char* root_uri, uint16_t local_port);

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
    static ZMJSON MakeError(int code, const char* message);

    /**
     * @brief 设置 JSON-RPC 请求回调（不带 task 参数）
     * @param oncall  回调函数
     */
    void SetJsonRpcCB(OnJsonRpcRequestCB oncall);

    /**
     * @brief 设置 JSON-RPC 请求回调（带 task 参数，优先级更高）
     * @param oncall_ex  回调函数
     *
     * @note 同时设置了 CBEx 和 CB 时，优先使用 CBEx
     */
    void SetJsonRpcCBEx(OnJsonRpcRequestCBEx oncall_ex);

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
    virtual int OnJsonRpcRequest(ZmHttpdTask* task, const char* method, const ZMJSON& params,
        ZMJSON& result, ZMJSON& error);

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

private:
    /** @brief JSON-RPC 请求回调（不带 task 参数） */
    OnJsonRpcRequestCB      m_on_jsonrpc_call;

    /** @brief JSON-RPC 请求回调（带 task 参数，优先使用） */
    OnJsonRpcRequestCBEx    m_on_jsonrpc_call_ex;

    /** @brief RPC 请求的 URI 前缀，匹配此前缀的请求走 RPC 流程 */
    char                    m_root_uri[128];
};


#endif /* ZM_NET_HTTP_H */
