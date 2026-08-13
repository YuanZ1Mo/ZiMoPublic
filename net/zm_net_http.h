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

#include "../util/zm_util_json.h"
#include "../util/zm_util_str.h"
// 需要 ZM_TICKET_KEYS_LEN(ticket 密钥长度宏),仅引入 evp.h 等轻量头,不含 openssl/ssl.h
#include "../ssl/zm_ssl_ctx.h"

#include <../libevent/include/event2/http.h>
#include <../libevent/include/event2/keyvalq_struct.h>
#include <../libevent/include/event2/event.h>

#include <stdint.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string_view>
#include <unordered_map>

// SSL_CTX 前向声明（仅在启用 HTTPS 时需要，避免头文件全量引入 openssl/ssl.h）
struct ssl_ctx_st;

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
class ZmWebSocketServer;   // WebSocket 组件(内部成员,实现见 zm_net_websocket_server.h)
struct ZmWebSocketCallbacks;   // WebSocket 业务回调(SetWebSocketCallbacks 形参,完整定义在 cpp)
class ZmHttpdDoer;
class ZmHttpdDoerPool;
class ZmReqLoopPool;
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
    /** Convert HTTP method enum to string, like as EVHTTP_REQ_GET->"GET" */
    static const char*  VerbToString(evhttp_cmd_type verb);
    /** Get MIME type string from file extension */
    static const char*  GetMimeType(const std::string& path);
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




class ZmReqLoop;   // 前向声明(per-request 事件循环,见 zm_net_req_loop.h)

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

    /** @brief 获取请求的路径部分（不含 query string），例如 "/api/users" */
    const char* Path();

    /** @brief 获取请求的 query string，例如 "foo=bar&page=1"（无参数时返回空字符串） */
    const char* QueryStr();

    const char* Ip();
    ev_uint16_t Port();

    /**
     * @brief 查询当前请求连接是否为 HTTPS(TLS)
     * @return true 该请求经由 TLS 连接到达(底层为 SSL bufferevent)
     * @note 遍历 evhttp_connection → bufferevent 判定,与 Ip()/Port() 同模式;
     *       纯 HTTP 服务器实例(如 HTTPS 模式下的 80 端口重定向服务器)恒为 false
     */
    bool IsHttps();

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
     * @brief 设置当前连接的收发限速（单连接独立限速）
     *
     * 对底层 bufferevent 设置读写速率令牌桶，需在响应数据发送前调用。
     * 仅限制响应体数据的传输速率，不影响请求头解析和响应头发送。
     *
     * @param download_bps 每秒最大发送字节数（服务器→客户端），0 表示不限速
     * @param upload_bps   每秒最大接收字节数（客户端→服务器），0 表示不限速
     * @return true 成功，false 失败（连接无效或 bufferevent 不支持限速）
     *
     * @note 基于 libevent bufferevent_set_rate_limit，不会丢失零拷贝特性。
     *       每个连接独立限速，互不影响。可在传输过程中重复调用以动态调速。
     */
    bool SetRateLimit(size_t download_bps = 0, size_t upload_bps = 0);

    /**
     * @brief 将当前连接加入共享带宽池
     *
     * 同一组内所有连接共享组的总带宽。用于按用户/IP 聚合限速，
     * 防止多线程下载器绕过单连接限速。
     *
     * @param group  共享带宽池指针（由 bufferevent_rate_limit_group_new 创建）
     * @return true 成功，false 失败（连接无效或 group 为 nullptr）
     *
     * @note 需在 SetReplyFile / SetReplyData 之前调用。
     *       可与 SetRateLimit 叠加使用：连接实际速度取两者的最小值。
     */
    bool JoinRateLimitGroup(struct bufferevent_rate_limit_group* group);

    // ---- 流式响应（Chunked Transfer-Encoding） ----

    /**
     * @brief 开始流式响应，仅发送 HTTP 响应头（Transfer-Encoding: chunked）
     *
     * 调用后连接进入分块传输模式，后续通过 SendReplyChunk() 发送数据块，
     * 最后通过 EndStreamReply() 结束流式响应。
     *
     * 适用于 SSE 推送、大文件下载、实时日志推送等需要边产出边发送的场景。
     * 可在任意线程调用（通过 event_active 投递到事件循环线程发送响应头）。
     *
     * @param code    HTTP 状态码，如 200
     * @param reason  原因短语，如 "OK"，传入 nullptr 时由 libevent 自动填充
     *
     * @note 调用前需通过 PutReplyHeader / SetReply 设置好响应头和状态码
     * @note handler 应返回 -1（异步模式）阻止框架自动发送完整响应
     */
    void StartStreamReply(int code, const char* reason = nullptr);

    /**
     * @brief 发送一个流式数据块
     *
     * 数据块通过线程安全的内部缓冲区传递到事件循环线程，由事件循环调用
     * evhttp_send_reply_chunk 发送。频繁小块写入时，事件循环可能将多个块
     * 合并为一次发送（由 libevent 事件合并行为决定，对客户端透明）。
     *
     * 可在任意线程调用，不需要等待前一个块发送完成。
     *
     * @param data  数据块字节指针
     * @param dlen  数据块长度（字节数）
     */
    void SendReplyChunk(const BYTE* data, size_t dlen);

    /**
     * @brief 结束流式响应，发送终止块并释放连接
     *
     * 若内部缓冲区还有未发送的数据块，会先刷出再发送终止块。
     * 调用后 doer 被回收，task 不可再使用。
     *
     * 可在任意线程调用。
     */
    void EndStreamReply();

    /** @brief 查询当前是否处于流式响应模式 */
    bool IsStreaming() const { return m_streaming; }

    /**
     * @brief 查询连接是否已关闭（客户端断开/连接释放）
     */
    bool IsConnClosed() const { return m_connClosed.load(); }

    /** @brief 请求到达时间戳(毫秒,GetTickCount64),OnHttpRequestCB 记录,供业务预算/超时计算 */
    void SetArriveTime(int64_t ms) { m_arriveMs = ms; }
    int64_t ArriveMs() const { return m_arriveMs; }

    /** @brief 标记连接已关闭(close 通知器调用,任意线程安全) */
    void MarkConnClosed() { m_connClosed.store(true); }

    /** @brief 暴露连接关闭原子标志(供 ZmReqLoopPool 排队等待轮询) */
    std::atomic<bool>& ConnClosedFlag() { return m_connClosed; }

    /** @brief 绑定/解绑当前请求的 A(ZmReqLoop),close 通知器经此投递 CLOSE(原子) */
    void BindLoop(ZmReqLoop* l) { m_boundLoop.store(l); }
    ZmReqLoop* BoundLoop() const { return m_boundLoop.load(); }

    /**
     * @brief 暂存请求级用户数据(通用槽位,协议上下文可放这里)
     *
     * @note 生命周期同 task:doer 复用(Reset)时自动清空,任意线程可读写
     *       (业务线程写,回复路径读;读写均不跨请求,无竞态)
     */
    void SetUserData(std::shared_ptr<void> data) { m_userData = std::move(data); }
    std::shared_ptr<void> UserData() const { return m_userData; }

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

    /** @brief 设置流式响应回调（由 ZmHttpdDoer 在构造时调用） */
    void SetStreamStartCallback(std::function<void()> cb);
    void SetStreamChunkCallback(std::function<void()> cb);
    void SetStreamEndCallback(std::function<void()> cb);

protected:
    friend class ZmHttpdDoer;

    std::function<void()> m_on_reply;                ///< 回复信号回调

    /** @brief 流式响应回调（由 ZmHttpdDoer 设置，触发 event_active 投递到事件循环） */
    std::function<void()> m_on_stream_start;
    std::function<void()> m_on_stream_chunk;
    std::function<void()> m_on_stream_end;
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

    /** @brief 流式响应数据块累积缓冲区（线程安全锁，工作线程写入，事件循环线程读取并发送） */
    struct evbuffer*                    m_chunk_buf;

    /** @brief 是否处于流式响应模式 */
    bool                                m_streaming;

    /** @brief 连接关闭标志（close 通知器置位；doer 池复用时在 Reset 中清零） */
    std::atomic<bool>                   m_connClosed {false};

    /** @brief 请求到达时间戳(毫秒),OnHttpRequestCB 设置 */
    int64_t m_arriveMs = 0;
    /** @brief 绑定的 A(ZmReqLoop),close 通知器读取投递 CLOSE;原子防跨线程竞态 */
    std::atomic<ZmReqLoop*> m_boundLoop{nullptr};

    /** @brief 请求级用户数据(子类/业务层暂存协议上下文,doer Reset 时清空) */
    std::shared_ptr<void> m_userData;

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
        ZM_HTTPD_CONTROL_REPLY        = 0x0200,   ///< 工作线程请求发送完整响应
        ZM_HTTPD_CONTROL_STREAM_START = 0x0400,   ///< 工作线程请求开始流式响应（发响应头）
        ZM_HTTPD_CONTROL_STREAM_CHUNK = 0x0800,   ///< 工作线程请求发送流式数据块
        ZM_HTTPD_CONTROL_STREAM_END   = 0x1000,   ///< 工作线程请求结束流式响应
        ZM_HTTPD_CONTROL_WS_REPLY     = 0x2000,   ///< 任意线程投递 WS 回包闭包(事件循环线程执行)
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
     * @brief 构造 HTTP/HTTPS 服务器
     * @param evbase              外部 libevent 事件循环对象（不由此类接管生命周期）
     * @param local_port          监听端口号
     * @param certFile            证书 PEM 文件路径，非空启用 HTTPS
     * @param keyFile             私钥 PEM 文件路径，非空启用 HTTPS
     * @param redirect_from_port  当 HTTPS 启用时，在此端口创建 301 重定向服务器；0 表示不启用
     * @param sessionCacheSize    TLS 会话缓存容量，0=不启用
     * @param sessionContext      会话上下文标识，sessionCacheSize>0 时必填
     */
    ZmHttpServer(struct event_base* evbase, uint16_t local_port,
                 const char* certFile = nullptr,
                 const char* keyFile = nullptr,
                 uint16_t redirect_from_port = 0,
                 uint32_t sessionCacheSize = 0,
                 const char* sessionContext = nullptr);

    /** @brief 析构，释放 evhttp 和控制事件（不释放外部 event_base） */
    virtual ~ZmHttpServer();

    /**
     * @brief 初始化 HTTP 服务器：绑定端口、创建工作线程池和控制事件
     * @return true 初始化成功
     */
    bool Init();

    /** @brief 关闭 HTTP 服务器：停止线程池、释放控制事件和 evhttp（不释放外部 event_base） */
    void Close();

    /** @brief 武装关闭门(m_closing=true):通知器 Add/Remove/closecb 投递全部跳过。
     *  Close() 内部先调用;ZmReqLoopPool 销毁后 closecb 不得再触碰 loop。幂等。 */
    void BeginClose() { m_closing.store(true); }

    /** @brief 排空 HTTP worker 线程池(join):调用后不再有 doer 进入处理流程。
     *  Close() 内部先于 ZmReqLoopPool 停止调用,消除 doer 与 ZmReqLoopPool 销毁的竞态。幂等。 */
    void DrainWorkers();

    /**
     * @brief 启用业务 ZmReqLoopPool(ZmReqLoopPool,按需获取/预算约束/断连放弃)
     *
     * 必须在 Init() 之前调用;池的实际创建在 Init() 中完成。
     * ZmJsonRpcServer / ZmRESTfulServer 派生类构造时自动启用,裸 ZmHttpServer 默认不启用。
     *
     * @param prealloc  预创建 loop 线程数,0 = 默认(硬件并发)
     * @param maxLoops  池容量上限,0 = 默认(预创建 x4)
     * @param budgetMs  业务预算毫秒(deadline = 请求到达 + 预算),0 = 默认(5000)
     */
    void EnableLoopPool(int prealloc = 0, int maxLoops = 0, uint32_t budgetMs = 0);

    /** @brief 查询业务 ZmReqLoopPool是否已启用(Init 成功后生效) */
    bool LoopPoolEnabled() const { return m_reqLoopPool != nullptr; }

    /**
     * @brief 设置 ZmReqLoopPool 的 loop 工厂(默认 new ZmReqLoop())
     *
     * 派生类需在 loop 上承载 per-request 状态时调用,如
     * ZmJsonRpcServer → ZmReqLoopJrpc(存储 JRPC 响应信封)。
     * 必须在 Init() 之前调用(Init 预创建时即用工厂)。
     *
     * @note 工厂返回的实例类型必须与派生类中 static_cast 的类型一致,
     *       否则转换是未定义行为。
     */
    void SetLoopPoolFactory(std::function<ZmReqLoop*()> factory);

    /** @brief 查询服务器是否已初始化 */
    bool IsOpen() const;

    /**
     * @brief 获取本服务器的 WebSocket 组件(Init 后有效,始终非空)
     * @note 业务回调注册须在服务器 Init() 之前:
     *       GetWebSocketServer()->SetCallbacks({...}) → 服务器 Init()。
     *       未注册 onMessage 的服务器不接受 WebSocket 升级(走现有流程)。
     */
    ZmWebSocketServer* GetWebSocketServer() { return m_wsServer; }

    /**
     * @brief 设置 WebSocket 业务回调(转发内部组件,等价 GetWebSocketServer()->SetCallbacks)
     * @param cbs 业务回调(onMessage 为空 = 不接受升级);须在服务器 Init() 之前调用
     * @note 仿 SetRequestReadCB 直连接口:派生类(ZmRESTfulServer 等)直接可用
     */
    void SetWebSocketCallbacks(const ZmWebSocketCallbacks& cbs);

    /** @brief 查询是否已启用 HTTPS（m_ssl_ctx 非空） */
    bool IsHttps() const { return m_ssl_ctx != nullptr; }

    /**
     * @brief 热加载 SSL 证书（无需重启服务）
     *
     * 创建新 SSL_CTX，原子替换 m_ssl_ctx。新旧 ctx 在事件循环线程操作（无竞态）。
     * 旧 ctx 延时 5 分钟后释放，确保现有 TLS 连接完成或超时。
     *
     * @param certFile  新证书 PEM 文件路径
     * @param keyFile   新私钥 PEM 文件路径
     * @return true 加载并替换成功，false 证书无效（旧证书继续有效）
     */
    bool ReloadCertificate(const char* certFile, const char* keyFile);

    /**
     * @brief 设置 TLS session ticket 密钥(80 字节,ZM_TICKET_KEYS_LEN)
     *
     * 须在服务器事件循环线程内调用(唯一调用路径为 PostSetTicketKeys 投递回调)。
     * 内部保存拷贝,证书热加载后自动补设到新 SSL_CTX。
     */
    void SetTicketKeys(const unsigned char* keys, size_t len);

    /**
     * @brief 将 ticket 密钥投递到本服务器事件循环线程设置(线程安全)
     *
     * 轮换场景使用:任意线程可调用,实际在事件循环线程内执行
     * SSL_CTX_set_tlsext_ticket_keys,避免与并发握手竞争。
     * 残留未执行的投递在 event_base 释放时被丢弃,不会在服务器析构后执行。
     */
    void PostSetTicketKeys(const unsigned char* keys, size_t len);

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

    /**
     * @brief HTTPS SSL bufferevent 工厂回调（由 evhttp_set_bevcb 注册）
     *
     * 每个新连接到来时，evhttp 调用此函数创建 SSL 加密的 bufferevent。
     * 创建 bufferevent_openssl_socket_new 并设置为 ACCEPTING（服务端）模式，
     * SSL 握手由 libevent 自动完成，对上层 HTTP 协议解析完全透明。
     *
     * @param base  libevent event_base
     * @param arg   SSL_CTX 指针，在 evhttp_set_bevcb 中作为 cbarg 传入
     * @return      SSL bufferevent 指针，evhttp 随后通过 bufferevent_setfd 绑定 socket
     */
    static struct bufferevent* OnSSLBuffereventCB(struct event_base* base, void* arg);

    /**
     * @brief HTTP→HTTPS 301 全量重定向回调（轻量级，不走 ZmHttpdDoer / Router 管线）
     *
     * redirect_from_port 上收到任意请求时，构造 https://host:port/uri 并返回 301。
     * 在事件循环线程直接调用 evhttp_send_reply，零线程切换。
     *
     * 【限制】不按路径区分、不经中间件、固定 301（POST→GET）。仅适用于通用 HTTP 端口，
     * 不适合 API 端口（POST 丢 body、客户端应直接配置 https://）。
     *
     * @param req  原始 HTTP 请求
     * @param arg  ZmHttpServer 实例指针（用于获取 HTTPS 端口号）
     */
    static void OnRedirectRequestCB(struct evhttp_request* req, void* arg);

protected:
    /**
     * @brief SSL 上下文配置完成后的扩展点（子类可覆写以实现 mTLS、自定义校验等）
     *
     * 在 MakeServerCTX 成功之后、BindEventBase 之前调用。默认实现为空（单向认证）。
     *
     * @param ctx  已加载服务器证书和私钥的 SSL_CTX，可在此基础上继续配置
     *
     * @example mTLS 双向认证
     * @code
     *   void OnConfigureSSL(SSL_CTX* ctx) override {
     *       SSL_CTX_load_verify_locations(ctx, "client-ca.crt", nullptr);
     *       SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER |
     *                          SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
     *   }
     * @endcode
     */
    virtual void OnConfigureSSL(struct ssl_ctx_st* ctx) {}

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

protected:
    /**
     * @brief 从业务 ZmReqLoopPool取一个 loop(预算内排队,客户端断连提前放弃)
     * @param task 请求上下文(ArriveMs/ConnClosedFlag 参与排队计算)
     * @return 可用的 ZmReqLoop,或 nullptr(池未启用/排队超时/断连)
     */
    ZmReqLoop* AcquireLoop(ZmHttpdTask* task);

    /**
     * @brief 绑定 task 并投递 START 到 ZmReqLoop 线程(业务回调在其上执行)
     *
     * @param task    请求上下文(内部 BindLoop)
     * @param loop    AcquireLoop 返回的实例
     * @param onStart ZmReqLoop 线程执行的续体(闭包捕获业务回调与请求数据;task 内数据
     *                指针仅回复发送前有效,勿跨请求保存)
     */
    void DispatchLoop(ZmHttpdTask* task, ZmReqLoop* loop,
        std::function<void(ZmReqLoop*)> onStart);

private:
    /** @brief WebSocket 组件(ZmWebSocketServer 直访本类私有成员,如 m_reqLoopPool/m_threadPool/PostWsReply) */
    friend class ZmWebSocketServer;
    /** @brief WebSocket 会话(PostSendText 内部直调 PostWsReply 投递回包,友元不传递故单独声明) */
    friend class ZmWebSocketSession;

    /** @brief libevent 事件循环对象（外部传入，不由此类释放） */
    struct event_base* m_evbase;

    /** @brief libevent HTTP 服务器对象 */
    struct evhttp*     m_evhttpd;

    /** @brief 期望的线程池名称（Init 前设置则用此名，否则默认 "Http-{port}"） */
    std::string        m_threadPoolName;

    /** @brief 工作线程池（复用线程处理请求，替代 thread-per-request） */
    ZmThreadPool*      m_threadPool;

    /** @brief 业务 ZmReqLoopPool(EnableLoopPool 启用后由 Init 创建;nullptr = 未启用) */
    ZmReqLoopPool*     m_reqLoopPool;
    /** @brief ZmReqLoopPool配置(EnableLoopPool 保存,Init 时生效) */
    int                m_loopPoolPrealloc;
    int                m_loopPoolMax;
    uint32_t           m_loopPoolBudgetMs;
    bool               m_loopPoolEnabled;
    /** @brief loop 工厂(SetLoopPoolFactory 设置,Init 创建池时透传;默认 new ZmReqLoop()) */
    std::function<ZmReqLoop*()> m_loopPoolFactory;

    /** @brief 监听端口号 */
    uint16_t           m_local_port;

    /** @brief 端口绑定是否失败（BindEventBase 中设置） */
    bool               m_port_bind_failed;

    /** @brief SSL 上下文指针（nullptr = HTTP，非空 = HTTPS） */
    struct ssl_ctx_st* m_ssl_ctx;

    /** @brief 证书热加载时保留的旧 SSL_CTX（延时释放，给现有连接缓冲时间） */
    struct ssl_ctx_st* m_oldCtx;

    /** @brief 旧 SSL_CTX 延时清理定时器 */
    struct event*      m_ctxCleanupTimer;

    /** @brief ticket 密钥拷贝（未设置时为 0） */
    unsigned char      m_ticketKeys[ZM_TICKET_KEYS_LEN];

    /** @brief 是否已设置 ticket 密钥 */
    bool               m_hasTicketKeys;

    /** @brief session cache 容量（热加载沿用） */
    uint32_t           m_sessionCacheSize;

    /** @brief session cache 上下文（热加载沿用） */
    std::string        m_sessionContext;

    /** @brief HTTP→HTTPS 重定向服务器端口（0 = 不启用） */
    uint16_t           m_redirect_from_port;

    /**
     * @brief HTTP→HTTPS 重定向服务器（轻量级 evhttp，不经 ZmHttpdDoer/Router 管线）
     *
     * 仅在 m_redirect_from_port > 0 时创建。每个请求直接在事件循环线程处理：
     * 提取 Host 头构造 https:// URL → evhttp_send_reply(301)。
     * 适用浏览器全量跳转，不适合 API（POST→GET、无路由匹配、无中间件）。
     */
    struct evhttp*     m_redirectEvhttp;

    /** @brief 通用 HTTP 请求处理回调 */
    OnHttpdRequestCB   m_on_request;

    /** @brief ZmHttpdDoer 对象池（事件循环线程独享，无需锁） */
    ZmHttpdDoerPool*   m_httpdDoerPool;

    /** @brief per-connection close 通知器(方案 3):连接上所有在飞 doer 共享,close 时广播
     *  @note 单槽 closecb 覆盖问题的修复:closecb 的 ctx 是通知器,而非单个 doer */
    struct ZmConnCloseNotifier
    {
        std::vector<ZmHttpdTask*> members;   ///< 该连接所有在飞请求
        bool fired = false;                  ///< 已触发(防 reset/free 双触发)
        std::atomic<bool>* closing = nullptr;   ///< 指向服务器的 m_closing(Close 期间 closecb 跳过投递)
    };

    std::unordered_map<struct evhttp_connection*, ZmConnCloseNotifier*> m_closeNotifiers;  ///< 循环线程独享,无锁(Close 期间由主线程 ClearAll 独占清理,Add/Remove 以 m_closing 跳过)

    /** @brief Close 进行中:通知器 Add/Remove 跳过 map 操作(Close 由非循环线程调用) */
    std::atomic<bool> m_closing {false};

    /** @brief WebSocket 组件(Init 创建,Close 销毁;生命周期由本类托管) */
    ZmWebSocketServer* m_wsServer = nullptr;

    /** @brief WS 回包投递队列(任意线程 push,事件循环线程 RunWsReplies 排空) */
    std::deque<std::function<void()>> m_wsReplyQueue;
    std::mutex                        m_wsReplyMutex;
    struct event*                     m_wsReplyEvent = nullptr;

    /** @brief 连接关闭回调(closecb):广播到该连接所有在飞 doer */
    static void OnConnCloseNotifierCB(struct evhttp_connection* conn, void* arg);
    /** @brief 将 doer 登记进连接的通知器(请求到达时调用,循环线程) */
    void NotifierAdd(ZmHttpdTask* task);
    /** @brief 从通知器摘除 doer;空则注销 closecb 并删除通知器(doer 回收时调用) */
    void NotifierRemove(ZmHttpdTask* task);
    /** @brief 清理全部通知器(服务器 Close/析构时调用) */
    void NotifierClearAll();

    /**
     * @brief 投递回包闭包到本服务器事件循环线程执行(任意线程调用,线程安全)
     * @note 仅用于 evws 收发(evws_send_text 等,严格在循环线程执行);
     *       由 ZmWebSocketServer(友元)使用,不对外公开。
     */
    void PostWsReply(std::function<void()> fn);
    /** @brief 事件循环线程执行所有待处理回包闭包(ZM_HTTPD_CONTROL_WS_REPLY 触发) */
    void RunWsReplies();
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
 *   server.SetRequestReadCB([](ZmReqLoop* loop, const char* reqData) {
 *       // ZmReqLoopPool 线程执行;reqData 为请求 JSON 字符串(method/params)
 *       std::string err;
 *       ZMJSON req = zm_json_parse(reqData, err);
 *       ZMJSON rsp;
 *       if (req["method"] == "add")
 *           rsp["result"] = {{"sum", req["params"]["a"].get<int>()
 *               + req["params"]["b"].get<int>()}};
 *       else
 *           rsp["error"] = ZmJsonRpcServer::MakeError(-32601, "Method not found");
 *       ZmReqLoopJrpc::ResponseJson(loop, rsp);   // 任意线程可调,框架自动构造响应并发送
 *   });
 * @endcode
 */
class ZmJsonRpcServer : public ZmHttpServer
{
public:
    /**
     * @brief JRPC 业务请求回调（在 ZmReqLoop 线程执行，与 ZmReqLoopJrpcRequestCB 同签名）
     * @param loop    本请求的 ZmReqLoop 实例（回复经 ZmReqLoopJrpc::ResponseJson(loop, rsp)）
     * @param reqData 请求 JSON 字符串（含 method / params；仅在回调期间有效）
     *
     * 服务器收到匹配 root_uri 的请求后，解析校验、响应信封(id/jsonrpc/method)存入 ZmReqLoopJrpc，
     * 经内部 ZmReqLoopPool 分发，在 ZmReqLoop 线程调用本回调。业务层异步处理完成后
     * 调用 ZmReqLoopJrpc::ResponseJson(loop, rsp) 回复（与 ZmReqLoopRest 同构：
     * TryReply 门 → 服务器组装信封 → 发送 → DONE 收尾）。
     *
     * @note 回复可在任意线程调用（内部通过 event_active 安全投递到 HTTP event loop）。
     */
    typedef std::function<void(ZmReqLoop* loop, const char* reqData)>
        OnRequestReadCB;

    /**
     * @brief 构造 JSON-RPC 服务器
     * @param evbase      外部 libevent 事件循环对象
     * @param root_uri    RPC 请求的 URI 前缀，仅匹配此前缀的请求走 RPC 流程，为空或 nullptr 时所有请求走 RPC
     * @param local_port  监听端口号
     */
    ZmJsonRpcServer(struct event_base* evbase, std::string_view root_uri, uint16_t local_port,
                    const char* certFile = nullptr, const char* keyFile = nullptr,
                    uint32_t sessionCacheSize = 0, const char* sessionContext = nullptr);

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
     * @brief 设置 JRPC 业务请求回调（业务层直接调用）
     * @param oncall_async 业务回调(OnRequestReadCB,在 ZmReqLoopPool 线程执行)
     *
     * @note 设置后所有匹配 root_uri 的请求经内部 ZmReqLoopPool 分发到本回调;
     *       未设置时返回 PORTAL_NOJRPC 错误信封。
     *       业务层经 ZmReqLoopJrpc::ResponseJson(loop, rsp) 回复,
     *       框架自动构造 JSON-RPC 2.0 标准响应信封并发送。
     */
    void SetRequestReadCB(OnRequestReadCB oncall_async);

    /**
    * @brief 构造 JSON-RPC HTTP 响应并写入 task（异步路径使用）
    *
    * 根据请求中是否携带 callback 参数自动选择响应格式：
    * - 无 callback → 标准 JSON 响应，Content-Type: application/json
    * - 有 callback → JSONP 格式 callback(json)，Content-Type: application/javascript
    *
    * 同时设置 Server 响应头（含服务端版本号）、HTTP 状态码 200 和响应体。
    * 调用者负责触发实际发送（异步路径需调用 SendDeferredReply）。
    *
    * @param task         请求上下文对象（从中读取 callback 参数）
    * @param rsp_envelope 已填充 jsonrpc/id/method/result/error 的响应 JSON 对象
    */
    static void BuildJsonRpcResponse(ZmHttpdTask* task, ZMJSON& reply, const ZMJSON& response);

protected:
    /**
     * @brief 处理 HTTP 请求，解析 JSON-RPC 协议并构造响应（重写父类虚函数）
     *
     * 处理流程:
     *   1. URI 不匹配 root_uri 时交给父类处理
     *   2. GET 请求从 query string 读取 Base64 编码的 jsonbody 并解码
     *   3. 解析 JSON 请求体，校验 method/params 字段
     *   4. 响应信封(id/jsonrpc/method)存入 ZmReqLoopJrpc，经内部 ZmReqLoopPool 分发到 m_on_request_read
     *   5. 业务层经静态 ZmReqLoopJrpc::ResponseJson(loop, rsp) 回复，
     *      BuildJsonRpcResponse 按 JSON-RPC 2.0 规范组装（result 和 error 二选一）
     *   6. 支持 JSONP 模式（callback 参数非空时包裹为 callback(json)）
     *
     * @param task  请求上下文对象
     * @param data  请求体原始字节
     * @param dlen  请求体长度
     * @return      始终返回 200（错误信息在响应体的 JSON error 字段中）
     */
    virtual int OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen);

private:
    /** @brief JRPC 业务请求回调(经内部 ZmReqLoopPool 分发到 ZmReqLoop 线程执行) */
    OnRequestReadCB m_on_request_read;

    /** @brief RPC 请求的 URI 前缀，匹配此前缀的请求走 RPC 流程 */
    char                    m_root_uri[128];
};


/**
 * @brief RESTful HTTP 服务器，在 ZmHttpServer 基础上增加 RESTful 路由与分发
 *
 * 业务处理统一走异步回调:请求按 root_uri 前缀过滤命中后，
 * 经 ZmReqLoopPool 投递到业务线程(ZmReqLoop)，
 * 业务层通过 ZmReqLoopRest::Response* 回复 helper 直接回写 HTTP 响应。
 *
 * @example 异步模式
 * @code
 *   server.SetRequestReadCB([](ZmReqLoop* loop, const BYTE* body, size_t len) {
 *       // ZmReqLoopPool 线程执行 → ZmReqLoopRest::Response* 回复
 *   });
 * @endcode
 */
class ZmRESTfulServer : public ZmHttpServer
{
public:
    /**
     * @brief RESTful 业务请求回调（在 ZmReqLoopPool 线程执行，与 ZmReqLoopRestfulRequestCB 同签名）
     * @param loop     本请求的 ZmReqLoop 实例（经 ZmReqLoopRest::Response* 回复）
     * @param body     请求体字节指针（指向请求 evbuffer，回复发送前有效，勿保存跨请求使用）
     * @param body_len 请求体长度
     */
    using OnRequestReadCB = std::function<void(
        ZmReqLoop* loop, const BYTE* body, size_t body_len)>;

    ZmRESTfulServer(struct event_base* evbase, std::string_view root_uri, uint16_t local_port,
                    const char* certFile = nullptr, const char* keyFile = nullptr,
                    uint32_t sessionCacheSize = 0, const char* sessionContext = nullptr);
    virtual ~ZmRESTfulServer();

    /**
     * @brief 设置 RESTful 业务请求回调（业务层直接调用）
     * @param oncall 业务回调(OnRequestReadCB,在 ZmReqLoopPool 线程执行)
     *
     * @note 设置后所有匹配 root_uri 的请求经内部 ZmReqLoopPool 分发到本回调;
     *       未设置时返回 500,池满/排队超时返回 503。
     */
    void SetRequestReadCB(OnRequestReadCB oncall);

protected:
    /**
     * @brief 处理 HTTP 请求（重写 ZmHttpServer 虚函数）
     *
     * 前缀匹配 → 异步分发（全部请求投递到异步回调）
     * @return -1 已投递异步处理，0 未匹配/未设置回调
     */
    virtual int OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen) override;

private:
    /** @brief RESTful 业务请求回调(经内部 ZmReqLoopPool 分发到 ZmReqLoop 线程执行) */
    OnRequestReadCB m_on_request_read;

    /** @brief URI 前缀（仅匹配此前缀的请求走 RESTful 流程） */
    char m_root_uri[128];
};


#endif /* ZM_NET_HTTP_H */
