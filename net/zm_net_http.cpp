/**
 * @file zm_net_http.cpp
 * @brief 基于 libevent 的多线程 HTTP 服务器实现
 */

#include "zm_net_http.h"

#include "zm_net_socket.h"
#include "zm_net_ip.h"
#include "../util/zm_util_thread.h"
#include "../util/zm_util_libevent.h"
#include "../define/zm_version_define.h"
#include "../spdlog/zm_logger.h"

#include <../libevent/include/event2/bufferevent.h>
#include <../libevent/include/event2/buffer.h>

#include <algorithm>
#include <atomic>
#include <vector>

 // ============================================================================
 // ZmHttpUtil
ZM_HTTP_REQ* ZmHttpUtil::CreateRequest()
{
    ZM_HTTP_REQ* req = (ZM_HTTP_REQ*)malloc(sizeof(ZM_HTTP_REQ));
    req->Init();
    req->major = 1;
    req->minor = 1;
    return req;
}

void ZmHttpUtil::FreeRequest(ZM_HTTP_REQ* req)
{

    if (req)
    {
#define ZM_APP_REQ_FREE_STR(f) if (nullptr != req->f) { free(req->f); }
        ZM_APP_REQ_FREE_STR(host);
        ZM_APP_REQ_FREE_STR(userinfo);
        ZM_APP_REQ_FREE_STR(path);
        ZM_APP_REQ_FREE_STR(useragent);
        free(req);
        req = nullptr;
#undef ZM_APP_REQ_FREE_STR
    }
}

int ZmHttpUtil::ParseVerb(std::string_view method)
{
    // GET POST CONNECT PUT DELETE OPTIONS PATCH TRACE HEAD
    if (method == "GET") { return ZM_HTTP_VERB_GET; }
    else if (method == "POST") { return ZM_HTTP_VERB_POST; }
    else if (method == "CONNECT") { return ZM_HTTP_VERB_CONNECT; }
    else if (method == "PUT") { return ZM_HTTP_VERB_PUT; }
    else if (method == "DELETE") { return ZM_HTTP_VERB_DELETE; }
    else if (method == "OPTIONS") { return ZM_HTTP_VERB_OPTIONS; }
    else if (method == "PATCH") { return ZM_HTTP_VERB_PATCH; }
    else if (method == "TRACE") { return ZM_HTTP_VERB_TRACE; }
    else if (method == "HEAD") { return ZM_HTTP_VERB_HEAD; }
    return ZM_HTTP_VERB_NONE;
}

const char* ZmHttpUtil::VerbToString(evhttp_cmd_type verb)
{
    switch (verb)
    {
    case EVHTTP_REQ_GET:     return "GET";
    case EVHTTP_REQ_POST:    return "POST";
    case EVHTTP_REQ_CONNECT: return "CONNECT";
    case EVHTTP_REQ_PUT:     return "PUT";
    case EVHTTP_REQ_DELETE:  return "DELETE";
    case EVHTTP_REQ_OPTIONS: return "OPTIONS";
    case EVHTTP_REQ_PATCH:    return "PATCH";
    case EVHTTP_REQ_TRACE:    return "TRACE";
    case EVHTTP_REQ_HEAD:     return "HEAD";
    case EVHTTP_REQ_PROPFIND: return "PROPFIND";
    case EVHTTP_REQ_PROPPATCH:return "PROPPATCH";
    case EVHTTP_REQ_MKCOL:    return "MKCOL";
    case EVHTTP_REQ_LOCK:     return "LOCK";
    case EVHTTP_REQ_UNLOCK:   return "UNLOCK";
    case EVHTTP_REQ_COPY:     return "COPY";
    case EVHTTP_REQ_MOVE:     return "MOVE";
    default:                  return "UNKNOWN";
    }
}

const char* ZmHttpUtil::GetMimeType(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";

    std::string ext = path.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);

    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css")                    return "text/css; charset=utf-8";
    if (ext == ".js")                     return "application/javascript; charset=utf-8";
    if (ext == ".json")                   return "application/json; charset=utf-8";
    if (ext == ".png")                    return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")  return "image/jpeg";
    if (ext == ".gif")                    return "image/gif";
    if (ext == ".svg")                    return "image/svg+xml";
    if (ext == ".ico")                    return "image/x-icon";
    if (ext == ".woff")                   return "font/woff";
    if (ext == ".woff2")                  return "font/woff2";
    if (ext == ".ttf")                    return "font/ttf";
    if (ext == ".txt")                    return "text/plain; charset=utf-8";
    if (ext == ".xml")                    return "application/xml; charset=utf-8";
    if (ext == ".pdf")                    return "application/pdf";
    if (ext == ".zip")                    return "application/zip";

    return "application/octet-stream";
}

int ZmHttpUtil::StartWithVerbs(std::string_view buf)
{
    // GET POST CONNECT PUT DELETE OPTIONS PATCH TRACE HEAD
    if (buf.substr(0, 4) == "GET ") { return ZM_HTTP_VERB_GET; }
    else if (buf.substr(0, 5) == "POST ") { return ZM_HTTP_VERB_POST; }
    else if (buf.substr(0, 8) == "CONNECT ") { return ZM_HTTP_VERB_CONNECT; }
    else if (buf.substr(0, 4) == "PUT ") { return ZM_HTTP_VERB_PUT; }
    else if (buf.substr(0, 7) == "DELETE ") { return ZM_HTTP_VERB_DELETE; }
    else if (buf.substr(0, 8) == "OPTIONS ") { return ZM_HTTP_VERB_OPTIONS; }
    else if (buf.substr(0, 6) == "PATCH ") { return ZM_HTTP_VERB_PATCH; }
    else if (buf.substr(0, 6) == "TRACE ") { return ZM_HTTP_VERB_TRACE; }
    else if (buf.substr(0, 5) == "HEAD ") { return ZM_HTTP_VERB_HEAD; }
    return ZM_HTTP_VERB_NONE;
}

// code referers libevent source http.c evhttp_parse_request_line()
// GET scheme:[//[user:password@]host[:port]][/]path[?query][#fragment] HTTP/1.1
bool ZmHttpUtil::ParseRequest(ZM_HTTP_REQ* req, std::string_view line, int verb)
{
    char* method;
    char* uri;
    char* version;

    ZmByteBuffer  heap(line.size(), line.data());

    char* reqline = heap.Str();
    /* Parse the request line */
    method = zm_strsep(&reqline, " ");
    if (reqline == nullptr)
    {
        return false;
    }
    uri = zm_strsep(&reqline, " ");
    if (reqline == nullptr)
    {
        return false;
    }
    version = zm_strsep(&reqline, " ");
    if (reqline != nullptr)
    {
        return false;
    }

    // method
    snprintf(req->method, sizeof(req->method), "%s", method);
    req->verb = (verb > 0) ? verb : ParseVerb(method);

    // version : evhttp_parse_http_version
    int  major;
    int  minor;
    char ch;
    if (sscanf_s(version, "HTTP/%d.%d%c", &major, &minor, &ch, 1) == 2)
    {
        req->major = (char)(major & 0xFF);
        req->minor = (char)(minor & 0xFF);
    }
    // req->uri = y_strdup(uri);
    ParseUri(req, uri);

    return true;
}

/**
 * scheme:[//[user:password@]host[:port]][/]path[?query][#fragment]
 *
 * Format for Literal IPv6 Addresses in URL's
 * https://www.ietf.org/rfc/rfc2732.txt
 * http://[FEDC:BA98:7654:3210:FEDC:BA98:7654:3210]:80/index.html
 * http://[1080:0:0:0:8:800:200C:417A]/index.html
 * http://[3ffe:2a00:100:7031::1]
 * http://[1080::8:800:200C:417A]/foo
 * http://[::192.9.5.5]/ipng
 * http://[::FFFF:129.144.52.38]:80/index.html
 * http://[2010:836B:4179::836B:4179]
 */
void ZmHttpUtil::ParseUri(ZM_HTTP_REQ* req, std::string_view uri_)
{
    if (req == nullptr || uri_.empty())
    {
        return;
    }

    if (req->host) { free(req->host); }
    if (req->userinfo) { free(req->userinfo); }
    if (req->path) { free(req->path); }
    if (req->useragent) { free(req->useragent); }
    req->host = nullptr;
    req->userinfo = nullptr;
    req->path = nullptr;
    req->useragent = nullptr;

    ZmByteBuffer heap(uri_.size(), uri_.data());
    char* uri = heap.Str();

    // scheme
    char* scheme = strstr(uri, "://");
    if (nullptr != scheme)
    {
        snprintf(req->scheme, sizeof(req->scheme), "%s", zm_strsep(&uri, "://"));
        uri += 2;
    }

    // path
    char* path = strchr(uri, '/');
    req->path = _strdup(nullptr != path ? path : "/");

    // host and port
    char* host = (nullptr != path) ? zm_strsep(&uri, "/") : uri;

    // userinfo
    if (nullptr != strchr(host, '@'))
    {
        req->userinfo = _strdup(zm_strsep(&host, "@"));
    }

    // IPv6  square brakcet
    const char* qbracket_l = strchr(host, '[');
    const char* qbracket_r = strchr(host, ']');
    if (qbracket_l && qbracket_r && qbracket_l < qbracket_r)
    {
        req->host = zm_strndup(qbracket_l + 1, qbracket_r - qbracket_l - 1);
        const char* port = strchr(qbracket_r, ':');
        if (port)
        {
            req->port = (uint16_t)atoi(port + 1);
        }

        const char* pid = strchr(qbracket_r, '#');
        if (pid)
        {
            req->pid = (uint32_t)atol(pid + 1);
        }
    }
    else
    {
        /**
         *  兼容类似这样的 IPv6 代理请求: CONNECT fec0::2:0:0:1092:285:443 HTTP/1.1
         *  因此修改为从右搜索冒号
         */
        char* port = strrchr(host, ':');
        if (port)
        {
            *port = '\0';
            port++;
            if (port)
            {
                req->port = (uint16_t)atoi(port);
            }

            char* pid = strrchr(port, '#');
            if (pid)
            {
                *pid = '\0';
                pid++;
                if (pid)
                {
                    req->pid = (uint32_t)atol(pid);
                }
            }
        }
        req->host = _strdup(host);
        // +- end
    }
    if (req->port == 0)
    {
        if (_stricmp(req->scheme, "http") == 0)
        {
            req->port = 80;
        }
        else if (_stricmp(req->scheme, "https") == 0 || _stricmp(req->scheme, "ssl") == 0)
        {
            req->port = 443;
        }
    }
}

// [/]path[?query][#fragment]
void ZmHttpUtil::ParseUriPath(ZM_HTTP_URI* uri, char* path)
{
    memset(uri, 0, sizeof(ZM_HTTP_URI));
    uri->path = path;
    char* fragment = strchr(path, '#');
    if (fragment)
    {
        uri->fragment = fragment + 1;
        *fragment = '\0';
    }
    char* query = strchr(path, '?');
    if (query)
    {
        *query = '\0';
        query++;
    }
    else
    {
        query = path;
    }

    char  delims[] = "&";
    char* next_token = nullptr;
    char* pstr = strtok_s(query, delims, &next_token);
    while (pstr && uri->qcnt < 16)
    {
        size_t len = strlen(pstr);
        if (strlen(pstr) > 0)
        {
            uri->query[uri->qcnt].name = pstr;
            char* value = (char*)memchr(pstr, '=', len);
            if (value)
            {
                uri->query[uri->qcnt].value = value + 1;
                *value = '\0';
            }
            uri->qcnt++;
        }
        pstr = strtok_s(nullptr, delims, &next_token);
    }
}

const char* ZmHttpUtil::GetQuery(const ZM_HTTP_URI* uri, std::string_view name)
{
    for (size_t i = 0; i < uri->qcnt; i++)
    {
        if (name.size() == strlen(uri->query[i].name) &&
            strncmp(uri->query[i].name, name.data(), name.size()) == 0)
        {
            return uri->query[i].value ? uri->query[i].value : "";
        }
    }
    return "";
}

std::string ZmHttpUtil::HeaderGetValue(struct evkeyvalq* headers, std::string_view key, std::string_view defv)
{
    std::string keyStr(key);
    const char* val = evhttp_find_header(headers, keyStr.c_str());
    return std::string(val ? val : defv);
}

/** 不破坏数据的情况下提取 Status-Code */
int ZmHttpUtil::ParseStatusCode(std::string_view statusLine, std::string_view /*limit*/)
{
    int statusCode = 0;
    // Status-Line = HTTP-Version SP Status-Code SP Reason-Phrase CRLF
    auto pos1 = statusLine.find(' ');
    if (pos1 != std::string_view::npos)
    {
        auto rest = statusLine.substr(pos1 + 1);
        auto pos2 = rest.find(' ');
        if (pos2 != std::string_view::npos)
        {
            auto codeStr = rest.substr(0, pos2);
            for (char c : codeStr)
            {
                if (c < '0' || c > '9' || statusCode > 1000) { return 0; }
                statusCode = statusCode * 10 + (c - '0');
            }
        }
    }
    return statusCode;
}

// ============================ ZmHttpdTask ============================

/** @brief 全局请求 ID 计数器，线程安全自增 */
static std::atomic<uint64_t> g_httpd_task_id{0};

ZmHttpdTask::ZmHttpdTask(struct evhttp_request* request) : m_request(request), m_status_code(0), m_id(++g_httpd_task_id)
{
    // request 可能为 nullptr（对象池预创建场景），仅跳过 URI 解析，其余资源正常分配
    if (request)
    {
        const struct evhttp_uri* uri = evhttp_request_get_evhttp_uri(request);
        if (uri)
        {
            // 解析 URI 的 query string 部分为键值对，存入 m_query
            // 例如 "/api?foo=bar&name=test" 解析为 {foo=bar, name=test}
            if (-1 == evhttp_parse_query_str(evhttp_uri_get_query(uri), &m_query))
            {
            }
        }
    }
    // 创建用于存储响应体的 evbuffer，后续通过 SetReplyData 写入
    m_reply_buf = evbuffer_new();
    m_input_buf = nullptr;

    // 创建流式响应数据块缓冲区（启用线程安全锁，工作线程写入，事件循环线程读取）
    m_chunk_buf = evbuffer_new();
    evbuffer_enable_locking(m_chunk_buf, nullptr);
    m_streaming = false;
}

ZmHttpdTask::~ZmHttpdTask()
{
    evhttp_clear_headers(&m_query);
    if (m_reply_buf)
    {
        evbuffer_free(m_reply_buf);
    }
    m_reply_buf = nullptr;

    if (m_chunk_buf)
    {
        evbuffer_free(m_chunk_buf);
    }
    m_chunk_buf = nullptr;
}

struct evhttp_request* ZmHttpdTask::Request()
{
    return m_request;
}

evhttp_cmd_type ZmHttpdTask::Method()
{
    return evhttp_request_get_command(m_request);
}

const char* ZmHttpdTask::Uri()
{
    return evhttp_request_get_uri(m_request);
}

const char* ZmHttpdTask::Path()
{
    const struct evhttp_uri* uri = evhttp_request_get_evhttp_uri(m_request);
    return uri ? evhttp_uri_get_path(uri) : "/";
}

const char* ZmHttpdTask::QueryStr()
{
    const struct evhttp_uri* uri = evhttp_request_get_evhttp_uri(m_request);
    return uri ? evhttp_uri_get_query(uri) : "";
}

const char* ZmHttpdTask::Ip()
{
    const char* address = nullptr;

    struct evhttp_connection* con = evhttp_request_get_connection(m_request);
    if (con)
    {

        ev_uint16_t port;
        // 获取对端 IP 和端口（可用于访问控制或日志记录）
        evhttp_connection_get_peer(con, &address, &port);
    }

    return address;
}

ev_uint16_t ZmHttpdTask::Port()
{
    ev_uint16_t port = 0;
    struct evhttp_connection* con = evhttp_request_get_connection(m_request);
    if (con)
    {
        const char* address;

        // 获取对端 IP 和端口（可用于访问控制或日志记录）
        evhttp_connection_get_peer(con, &address, &port);
    }
    return port;
}

uint64_t ZmHttpdTask::Id()
{
    return m_id;
}

const char* ZmHttpdTask::GetQueryValue(std::string_view name, std::string_view defv)
{
    std::string nameStr(name);
    const char* val = evhttp_find_header(&m_query, nameStr.c_str());
    return val ? val : defv.data();
}

void ZmHttpdTask::GetRequestHeaders(nlohmann::json::object_t& headersObj)
{
    struct evkeyvalq* headerKeyVals = evhttp_request_get_input_headers(const_cast<struct evhttp_request*>(m_request));
    if (headerKeyVals)
    {
        // 遍历 libevent 内部的 tailq 链表（tqh_first / tqe_next）读取所有请求头
        for (struct evkeyval* h = headerKeyVals->tqh_first; h; h = h->next.tqe_next)
        {
            auto it = headersObj.find(h->key);
            if (it == headersObj.end())
            {
                headersObj[h->key] = h->value;
            }
            else if (it->second.is_array())
            {
                it->second.push_back(h->value);
            }
            else
            {
                // 已有单值 → 转为数组
                nlohmann::json arr = nlohmann::json::array();
                arr.push_back(std::move(it->second));
                arr.push_back(h->value);
                it->second = std::move(arr);
            }
        }
    }
}

const char* ZmHttpdTask::GetRequestHeader(std::string_view name, std::string_view defv)
{
    const struct evkeyvalq* headers = evhttp_request_get_input_headers(m_request);
    std::string nameStr(name);
    const char* val = evhttp_find_header(headers, nameStr.c_str());
    return val ? val : defv.data();
}

void ZmHttpdTask::PutReplyHeader(std::string_view name, std::string_view val)
{
    // val 为空时存储空字符串，防止后续构造 string 时崩溃
    std::string key(name);
    std::string value(val);

    // 去重：key 和 value 都相同则跳过
    for (auto& [k, v] : m_reply_headers)
    {
        if (k == key && v == value)
            return;
    }

    m_reply_headers.emplace_back(std::move(key), std::move(value));
}

void ZmHttpdTask::SetReply(int code, const char* reason)
{
    m_status_code = code;
    m_reason = std::string(reason ? reason : "");
}

void ZmHttpdTask::SetReplyData(const BYTE* data, size_t dlen)
{
    if (data && dlen > 0)
    {
        // 将数据追加到响应缓冲区，可多次调用以拼接响应体
        evbuffer_add(m_reply_buf, data, dlen);
    }
}

void ZmHttpdTask::SetReplyBuf(struct evbuffer* buf)
{
    if (buf)
    {
        // 将源 buffer 的全部内容移动到响应缓冲区（源 buffer 被消费）
        evbuffer_remove_buffer(buf, m_reply_buf, evbuffer_get_length(buf));
    }
}

int ZmHttpdTask::SetReplyFile(int fd, ev_off_t offset, ev_off_t length)
{
    if (fd < 0)
        return -1;

    // 使用 evbuffer_file_segment 替代已废弃的 evbuffer_add_file
    // EVBUF_FS_CLOSE_ON_FREE: 段释放时自动 close(fd)
    // EVBUF_FS_DISABLE_SENDFILE: 禁用 Windows TransmitFile，只用 mmap
    struct evbuffer_file_segment* seg = evbuffer_file_segment_new(
        fd, offset, length,
        EVBUF_FS_CLOSE_ON_FREE);
    if (!seg)
        return -1;

    // 添加到响应缓冲区（evbuffer 增加段引用计数）
    if (evbuffer_add_file_segment(m_reply_buf, seg, 0, length) != 0)
    {
        // 失败时释放段，EVBUF_FS_CLOSE_ON_FREE 会触发 close(fd)
        evbuffer_file_segment_free(seg);
        return -1;
    }

    // 释放本地引用（evbuffer 仍持有引用，fd 不会在此处关闭）
    evbuffer_file_segment_free(seg);
    return 0;
}

void ZmHttpdTask::SetInputBuffer(struct evbuffer* buf)
{
    m_input_buf = buf;
}

struct evbuffer* ZmHttpdTask::GetInputBuffer() const
{
    return m_input_buf;
}

void ZmHttpdTask::DrainInputBody(size_t len)
{
    if (m_input_buf && len > 0)
        evbuffer_drain(m_input_buf, len);
}

void ZmHttpdTask::ClearReplyBody()
{
    if (m_reply_buf)
        evbuffer_drain(m_reply_buf, evbuffer_get_length(m_reply_buf));
}

void ZmHttpdTask::TriggerReply()
{
    if (m_on_reply)
        m_on_reply();
}

void ZmHttpdTask::SetReplyCallback(std::function<void()> cb)
{
    m_on_reply = std::move(cb);
}

// ============================ 流式响应 ============================

void ZmHttpdTask::StartStreamReply(int code, const char* reason)
{
    m_status_code = code;
    m_reason = std::string(reason ? reason : "");
    m_streaming = true;
    if (m_on_stream_start)
        m_on_stream_start();
}

void ZmHttpdTask::SendReplyChunk(const BYTE* data, size_t dlen)
{
    if (!data || dlen == 0)
        return;
    if (!m_streaming)
        return;

    // evbuffer_add 在 evbuffer_enable_locking 后线程安全
    evbuffer_add(m_chunk_buf, data, dlen);
    if (m_on_stream_chunk)
        m_on_stream_chunk();
}

void ZmHttpdTask::EndStreamReply()
{
    if (!m_streaming)
        return;
    m_streaming = false;
    if (m_on_stream_end)
        m_on_stream_end();
}

void ZmHttpdTask::SetStreamStartCallback(std::function<void()> cb)
{
    m_on_stream_start = std::move(cb);
}

void ZmHttpdTask::SetStreamChunkCallback(std::function<void()> cb)
{
    m_on_stream_chunk = std::move(cb);
}

void ZmHttpdTask::SetStreamEndCallback(std::function<void()> cb)
{
    m_on_stream_end = std::move(cb);
}

// ============================ ZmHttpServer internals ============================

/**
 * @brief HTTP 请求处理任务，由线程池调度执行
 *
 * 生命周期（对象池模式）:
 *   1. 事件循环线程从对象池 Acquire 或 new 创建，提交到线程池
 *   2. 线程池执行 Process() → Perform() → 若未 DeferReply 则自动 event_active(REPLY)
 *   3. 事件循环线程收到 REPLY 信号 → SendReply() → 回收至对象池
 *   4. 对象池满或 shutdown 时，多余的 doer 被 delete
 *   5. Perform() 异常时兜底设置 500 并触发 REPLY
 *
 * @note 此类仅在 cpp 内部使用，不对外暴露
 * @note event/m_reply_buf/m_on_deferred_reply 构造后不再释放，复用时通过 Reset() 清状态
 */
class ZmHttpdDoer : public ZmHttpdTask
{
public:
    ZmHttpdDoer(ZmHttpServer* httpd, struct evhttp_request* request)
        : ZmHttpdTask(request), m_httpd(httpd)
    {
        // request 为 nullptr 时仅分配持久资源（预创建场景），URI 解析在 Reset() 中补齐
        m_reply_event = event_new(m_httpd->EventBase(), -1, 0,
            ZmHttpServer::OnEventControl, this);
        event_add(m_reply_event, nullptr);

        // 延迟回复回调：this 不变，整个生命周期只设置一次
        SetReplyCallback([this] {
            event_active(m_reply_event, ZmHttpServer::ZM_HTTPD_CONTROL_REPLY, 0);
        });

        // 流式响应回调：将工作线程的请求投递到事件循环线程执行
        SetStreamStartCallback([this] {
            event_active(m_reply_event, ZmHttpServer::ZM_HTTPD_CONTROL_STREAM_START, 0);
        });
        SetStreamChunkCallback([this] {
            event_active(m_reply_event, ZmHttpServer::ZM_HTTPD_CONTROL_STREAM_CHUNK, 0);
        });
        SetStreamEndCallback([this] {
            event_active(m_reply_event, ZmHttpServer::ZM_HTTPD_CONTROL_STREAM_END, 0);
        });
    }

    ~ZmHttpdDoer()
    {
        // ★ 仅在对象真正销毁时释放资源（不是回收时）
        if (m_reply_event)
        {
            event_free(m_reply_event);
            m_reply_event = nullptr;
        }
    }

    // ========================================================================
    // 对象池复用接口
    // ========================================================================

    /**
     * @brief 重置内部状态以绑定新请求（从对象池取出时调用）
     *
     * 只清零瞬态字段；持久资源（event / evbuffer / callback）保持复用。
     *
     * @param request  新的 libevent 请求对象
     */
    void Reset(struct evhttp_request* request)
    {
        // ① 清空上一个请求的 query 参数
        evhttp_clear_headers(&m_query);

        // ② 绑定新请求并重新解析 URI query string
        m_request = request;
        const struct evhttp_uri* uri = evhttp_request_get_evhttp_uri(request);
        if (uri)
        {
            evhttp_parse_query_str(evhttp_uri_get_query(uri), &m_query);
        }

        // ③ 清空响应状态（evbuffer 只 drain，不 free）
        evbuffer_drain(m_reply_buf, evbuffer_get_length(m_reply_buf));
        evbuffer_drain(m_chunk_buf, evbuffer_get_length(m_chunk_buf));
        m_reply_headers.clear();
        m_reason.clear();
        m_status_code = 0;
        m_input_buf = nullptr;
        m_streaming = false;
        m_id = ++g_httpd_task_id;

        // ★ 以下成员保持不变（生命周期 = doer 对象生命周期）:
        //   m_reply_event — 仍挂在 event_base 上，下次 event_active 即可触发
        //   m_reply_buf   — evbuffer 仅 drain，结构体复用
        //   m_on_reply    — lambda 捕获 this 不变，仍然正确
        //   m_httpd       — 同一 server 实例
    }

    // ========================================================================
    // 响应发送
    // ========================================================================

    /** @brief 将默认响应头和业务层自定义响应头写入 libevent 请求（SendReply / SendReplyStart 共用） */
    void WriteResponseHeaders()
    {
        PutReplyHeader("Access-Control-Allow-Origin", "*");
        PutReplyHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        PutReplyHeader("Access-Control-Allow-Headers", "*");
        PutReplyHeader("ZmHttpServer-Version", ZIMO_SERVER_VERSION);

        for (auto it = m_reply_headers.begin(); it != m_reply_headers.end(); it++)
        {
            evhttp_add_header(evhttp_request_get_output_headers(m_request),
                it->first.c_str(), it->second.c_str());
        }
    }

    /** @brief 发送完整 HTTP 响应（一次性），由 ZM_HTTPD_CONTROL_REPLY 触发 */
    void SendReply()
    {
        WriteResponseHeaders();
        evhttp_send_reply(m_request, m_status_code,
            m_reason.empty() ? nullptr : m_reason.c_str(), m_reply_buf);

        // ★ 生命周期由调用方 OnEventControl 管理，此处不回收
    }

    /** @brief 开始流式响应：写响应头，进入 chunked 传输模式 */
    void SendReplyStart()
    {
        WriteResponseHeaders();
        evhttp_send_reply_start(m_request, m_status_code,
            m_reason.empty() ? nullptr : m_reason.c_str());
    }

    /** @brief 将 m_chunk_buf 中的累积数据通过 evhttp_send_reply_chunk 发送（事件循环线程调用） */
    void SendReplyChunkToClient()
    {
        size_t len = evbuffer_get_length(m_chunk_buf);
        if (len == 0)
            return;

        struct evbuffer* tmp = evbuffer_new();
        // evbuffer_remove_buffer: 将 m_chunk_buf 的全部数据移动到 tmp（不拷贝）
        evbuffer_remove_buffer(m_chunk_buf, tmp, len);
        evhttp_send_reply_chunk(m_request, tmp);
        evbuffer_free(tmp);
    }

    /** @brief 结束流式响应：刷出剩余数据块并发送终止块，doer 由调用方回收 */
    void SendReplyEnd()
    {
        // 先刷出所有未发送的数据块
        SendReplyChunkToClient();
        evhttp_send_reply_end(m_request);
    }

    // ========================================================================
    // 请求处理入口（线程池调用）
    // ========================================================================

    void Process()
    {
        try
        {
            m_httpd->Perform(this);
        }
        catch (const std::exception& e)
        {
            PUBLIC_LOG_ERROR("[请求#{}] Perform 异常: {}", m_id, e.what());
            if (m_streaming)
            {
                // 流式响应已开始，通过 event_active 结束（事件循环线程负责 evhttp_send_reply_end + 回收）
                EndStreamReply();
            }
            else
            {
                SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR, "Internal Server Error");
                TriggerReply();
            }
        }
        catch (...)
        {
            PUBLIC_LOG_ERROR("[请求#{}] Perform 未知异常", m_id);
            if (m_streaming)
            {
                EndStreamReply();
            }
            else
            {
                SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR, "Internal Server Error");
                TriggerReply();
            }
        }
    }

    ZmHttpServer* HttpServer() { return m_httpd; }

private:
    ZmHttpServer* m_httpd;
    struct event* m_reply_event;  ///< "响应就绪"信号事件（挂在 httpd 的 event_base 上）
};

// ============================================================================
// ZmHttpdDoer 对象池（事件循环线程独享，无锁，自动扩容）
// ============================================================================
//
// Acquire / Recycle 均在事件循环线程调用，不需要同步原语。
// 无硬上限，池大小随峰值并发自然增长。
//
// 维护两份列表：
//   m_allDoers  — 池创建过的所有 doer（全集，析构时统一释放）
//   m_freelist  — 空闲待复用的 doer（m_allDoers 的子集）
//
// 在飞 doer（m_allDoers 中但不在 m_freelist 中）不在析构时强制释放，
// 它们会在后续 REPLY 回调到达时通过 RecycleDoer(nullptr 兜底) 自删除。
//
class ZmHttpdDoerPool
{
public:
    explicit ZmHttpdDoerPool(ZmHttpServer* server, size_t initCount = 0)
        : m_server(server), m_peakSize(0)
    {
        if (initCount > 0)
            PreAlloc(initCount);
    }

    ~ZmHttpdDoerPool()
    {
        // ① 删除所有空闲 doer
        for (ZmHttpdDoer* doer : m_freelist)
            delete doer;

        // ② 统计在飞 doer（m_allDoers 中有但 m_freelist 中已无）
        //    这些 doer 的 event 尚未触发 REPLY，由 RecycleDoer nullptr 兜底自删除
        size_t inflight = m_allDoers.size() - m_freelist.size();
        if (inflight > 0)
        {
            PUBLIC_LOG_WARN("DoerPool 析构: {} 个 doer 仍在飞（等待异步响应），将由兜底逻辑自删除", inflight);
        }

        m_freelist.clear();
        m_allDoers.clear();
    }

    /**
     * @brief 从池中取出一个已重置的 doer，池空时新建（自动扩容）
     */
    /**
     * @brief 预创建 count 个 doer 放入池中（初始化时调用，避免首批请求的分配延迟）
     * @param count  预创建数量
     */
    void PreAlloc(size_t count)
    {
        m_freelist.reserve(m_freelist.size() + count);
        m_allDoers.reserve(m_allDoers.size() + count);
        for (size_t i = 0; i < count; ++i)
        {
            auto* doer = new ZmHttpdDoer(m_server, nullptr);
            m_allDoers.push_back(doer);
            m_freelist.push_back(doer);
        }
        RefreshPeak();
    }

    ZmHttpdDoer* Acquire(struct evhttp_request* request)
    {
        if (!m_freelist.empty())
        {
            ZmHttpdDoer* doer = m_freelist.back();
            m_freelist.pop_back();
            doer->Reset(request);
            return doer;
        }
        // 池空 → 新建（自动扩容），记入全集
        auto* doer = new ZmHttpdDoer(m_server, request);
        m_allDoers.push_back(doer);
        return doer;
    }

    /**
     * @brief 回收 doer，放回空闲列表
     */
    void Recycle(ZmHttpdDoer* doer)
    {
        m_freelist.push_back(doer);
        RefreshPeak();
    }

    /**
     * @brief 收缩池容量，保留最近使用的 keep 个 doer，其余释放
     * @param keep  保留数量，0 表示全部释放
     */
    void Shrink(size_t keep = 0)
    {
        while (m_freelist.size() > keep)
        {
            ZmHttpdDoer* doer = m_freelist.back();
            m_freelist.pop_back();
            // 从全集中移除
            auto it = std::find(m_allDoers.begin(), m_allDoers.end(), doer);
            if (it != m_allDoers.end())
                m_allDoers.erase(it);
            delete doer;
        }
    }

    size_t Size()       const { return m_freelist.size(); }
    size_t TotalSize()  const { return m_allDoers.size(); }  ///< 含在飞 doer
    size_t PeakSize()   const { return m_peakSize; }

private:
    void RefreshPeak()
    {
        if (m_freelist.size() > m_peakSize)
            m_peakSize = m_freelist.size();
    }

    ZmHttpServer* m_server;
    std::vector<ZmHttpdDoer*> m_freelist;   ///< 空闲待复用
    std::vector<ZmHttpdDoer*> m_allDoers;   ///< 全集（含在飞 doer）
    size_t m_peakSize;                       ///< 空闲列表历史峰值
};

ZmHttpHead::ZmHttpHead() : _entries(16)
{
}

ZmHttpHead::~ZmHttpHead()
{
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        _ENTRY* entry = _entries.At(i);
        if (entry->name)
        {
            free(entry->name);
        }
        if (entry->value)
        {
            free(entry->value);
        }
    }
    _entries.Clear();
}

int ZmHttpHead::StatusCode()
{
    return _status_code;
}

int ZmHttpHead::ContentLength()
{
    const char* value = Value("Content-Length");
    return value ? atoi(value) : 0;
}

void ZmHttpHead::Parse(std::string_view buf, size_t len, bool hasReqLine)
{
    _entries.Clear();
    const char* hstr = nullptr;
    if (hasReqLine)
    {
        hstr = strstr(buf.data(), "\r\n");
        _status_code = ZmHttpUtil::ParseStatusCode(buf, hstr);
        len = len ? (len - (hstr - buf.data() - 2)) : strlen(hstr);
    }
    else
    {
        hstr = buf.data();
        len = len ? len : buf.size();
    }
    ZmByteBuffer str(len, hstr);
    char  delims[] = "\r\n";
    char* next_token = nullptr;
    char* pstr = strtok_s(str.Str(), delims, &next_token);
    while (pstr)
    {
        if (strlen(pstr) < 1)
        {
            break;
        }
        char* vstr = strchr(pstr, ':');
        if (vstr)
        {
            *vstr = '\0';
            vstr++;
            /** 去掉value左边的空白 */
            while (*vstr == ' ' || *vstr == '\t')
            {
                vstr++;
            }
        }
        /** 去掉name右边的空白 */
        char* rstr = pstr + strlen(pstr) - 1;
        while (rstr > pstr && (*rstr == ' ' || *rstr == '\t'))
        {
            *rstr = '\0';
            rstr--;
        }

        _ENTRY* entry = _entries.Add();
        entry->name = _strdup(pstr);
        entry->value = _strdup(vstr ? vstr : "");

        pstr = strtok_s(nullptr, delims, &next_token);
    }
}

void ZmHttpHead::Build(ZmByteBuffer& output)
{
    ZmByteBuffer tmp(4096);
    size_t offset = 0;
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        _ENTRY* e = _entries.At(i);
        offset += snprintf(tmp.Str() + offset, tmp.Size() - offset, "%s: %s\r\n", e->name, e->value);
    }
    output.Reset(offset, tmp.Head());
}

void ZmHttpHead::BuildToBuffer(struct evbuffer* buf)
{
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        _ENTRY* e = _entries.At(i);
        evbuffer_add_printf(buf, "%s: %s\r\n", e->name, e->value);
    }
}

void ZmHttpHead::PutAll(ZmHttpHead* other)
{
    for (size_t i = 0; i < other->_entries.Count(); i++)
    {
        Value(other->_entries[i]->name, other->_entries[i]->value);
    }
}

ZmHttpHead::_ENTRY* ZmHttpHead::QueryEntry(std::string_view name)
{
    std::string nameStr(name);
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        if (_stricmp(_entries.At(i)->name, nameStr.c_str()) == 0)
        {
            return _entries.At(i);
        }
    }
    return nullptr;
}

const char* ZmHttpHead::PutValue(const char* name, const char* fmt, ...)
{
    ZmByteBuffer temp(256);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(temp.Str(), temp.Size(), fmt, ap);
    va_end(ap);
    return Value(name, temp.Str());
}

const char* ZmHttpHead::Value(const char* name, const char* value)
{
    _ENTRY* entry = QueryEntry(name);
    if (value)
    {
        if (!entry)
        {
            entry = _entries.Add();
            entry->name = _strdup(name);
        }
        else if (entry->value)
        {
            free(entry->value);
        }
        entry->value = _strdup(value);
    }
    return entry ? entry->value : nullptr;
}

void ZmHttpHead::Remove(std::string_view name)
{
    std::string nameStr(name);
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        if (_stricmp(_entries.At(i)->name, nameStr.c_str()) == 0)
        {
            _entries.Remove(i);
            return;
        }
    }
}

void ZmHttpHead::SetHostField(std::string_view scheme, std::string_view host, uint16_t port)
{
    ZmByteBuffer temp(256);
    std::string hostStr(host);
    bool isIPv6 = (AF_INET6 == ZmNetIP::Validate(hostStr.c_str()));
    if ((scheme == "http" && port != 80)
        || (scheme == "https" && port != 443)
        || (scheme == "ssl" && port != 443))
    {
        if (isIPv6) { temp.Sprintf("[%s]:%d", hostStr.c_str(), port); }
        else { temp.Sprintf("%s:%d", hostStr.c_str(), port); }
    }
    else
    {
        if (isIPv6) { temp.Sprintf("[%s]", hostStr.c_str()); }
        else { temp.Sprintf("%s", hostStr.c_str()); }
    }
    Value("Host", temp.Str());
}

bool ZmHttpHead::IsEmpty()
{
    return (_entries.Count() == 0);
}

// ============================ ZmHttpServer ============================

ZmHttpServer::ZmHttpServer(struct event_base* evbase, uint16_t local_port)
    : m_evbase(evbase), m_evhttpd(nullptr), m_threadPoolName(), m_threadPool(nullptr),
      m_local_port(local_port), m_port_bind_failed(false), m_httpdDoerPool(nullptr)
{}

ZmHttpServer::~ZmHttpServer()
{
    Close();
}

bool ZmHttpServer::Init()
{
    // 初始化 libevent 线程支持（Windows 下调用 evthread_use_windows_threads）
    // 必须在 event_base_new 之前调用，全局只需调用一次
    zm_util_eventbase_init();

    if (!m_evbase)
        return false;

    if (!BindEventBase(m_evbase))
        return false;

    // 创建工作线程池（线程复用，替代 thread-per-request）
    {
        std::string poolName = m_threadPoolName.empty()
            ? "ZmHttpServer:" + std::to_string(m_local_port)
            : m_threadPoolName;
        m_threadPool = new ZmThreadPool(
            (uint16_t)std::thread::hardware_concurrency(), poolName);
    }

    // 创建 doer 对象池（预创建 = CPU 核数，与线程池规模匹配，免去首批请求的分配延迟）
    if (!m_httpdDoerPool)
        m_httpdDoerPool = new ZmHttpdDoerPool(this, std::thread::hardware_concurrency());

    return true;
}

void ZmHttpServer::Close()
{
    // ★ 先停线程池（join 所有 worker，确保不再有新 doer 进入处理流程）
    if (m_threadPool)
    {
        delete m_threadPool;
        m_threadPool = nullptr;
    }

    // ★ 销毁 doer 对象池（线程池已停，worker 不再引用 doer，安全释放）
    if (m_httpdDoerPool)
    {
        delete m_httpdDoerPool;
        m_httpdDoerPool = nullptr;
    }

    // 释放 evhttp（停止接受新连接）
    if (m_evhttpd)
    {
        evhttp_free(m_evhttpd);
        m_evhttpd = nullptr;
    }

    // m_evbase 由外部管理生命周期，不在此释放
}

bool ZmHttpServer::IsOpen() const
{
    return m_evhttpd != nullptr;
}

void ZmHttpServer::SetPoolName(const std::string& name)
{
    m_threadPoolName = name;
    if (m_threadPool)
        m_threadPool->SetPoolName(name);
}

uint16_t ZmHttpServer::LocalPort()
{
    return m_local_port;
}

struct event_base* ZmHttpServer::EventBase()
{
    return m_evbase;
}

ZmHttpdDoer* ZmHttpServer::AcquireDoer(struct evhttp_request* request)
{
    return m_httpdDoerPool ? m_httpdDoerPool->Acquire(request)
                      : new ZmHttpdDoer(this, request);
}

void ZmHttpServer::RecycleDoer(ZmHttpdDoer* doer)
{
    if (m_httpdDoerPool)
        m_httpdDoerPool->Recycle(doer);
    else
        delete doer;  // pool 已销毁时兜底
}

void ZmHttpServer::SetRequestCallback(OnHttpdRequestCB onreq)
{
    m_on_request = onreq;
}

void ZmHttpServer::Perform(ZmHttpdTask* task)
{
    struct evhttp_connection* con = evhttp_request_get_connection(task->Request());
    if (con)
    {
        struct bufferevent* bev = evhttp_connection_get_bufferevent(con);
        if (bev)
        {
            // 忽略 SIGPIPE 信号，防止对端已关闭时 write 触发进程终止
            evutil_socket_t fd = bufferevent_getfd(bev);
            ZmNetSocketBase::IgnoreSignalPipe(fd);
        }
    }

    //// 打印请求接收日志，包含追踪 ID 便于关联请求和响应
    //{
    //    const char* method = nullptr;
    //    switch (task->Method())
    //    {
    //    case EVHTTP_REQ_GET:     method = "GET";     break;
    //    case EVHTTP_REQ_POST:    method = "POST";    break;
    //    case EVHTTP_REQ_PUT:     method = "PUT";     break;
    //    case EVHTTP_REQ_DELETE:  method = "DELETE";  break;
    //    case EVHTTP_REQ_OPTIONS: method = "OPTIONS"; break;
    //    case EVHTTP_REQ_PATCH:   method = "PATCH";   break;
    //    case EVHTTP_REQ_HEAD:    method = "HEAD";    break;
    //    default:                 method = "UNKNOWN"; break;
    //    }
    //    PUBLIC_LOG_INFO("[请求#{}] → {} {} 来自 {}", task->Id(), method,
    //        task->Uri() ? task->Uri() : "(null)", task->Ip() ? task->Ip() : "(null)");
    //}

    // 浏览器预检请求（OPTIONS）直接返回 200，无需进入业务逻辑
    if (EVHTTP_REQ_OPTIONS == task->Method())
    {
        task->SetReply(ZM_HTTP_STATUS_CODE_OK);
        task->TriggerReply();
        return;
    }

    struct evbuffer* inbuf = evhttp_request_get_input_buffer(task->Request());
    size_t           dlen = evbuffer_get_length(inbuf);
    task->SetInputBuffer(inbuf);
    int              code = OnHttpdRequest(task, evbuffer_pullup(inbuf, dlen), dlen);

    if (code == -1)
    {
        //-1表示异步回调,这边不做处理
    }
    else
    {
        task->SetReply(code);
        task->TriggerReply();
    }
}

void ZmHttpServer::OnHttpRequestCB(struct evhttp_request* request, void* arg)
{
    const char* uri = evhttp_request_get_uri(request);
    if (uri && arg)
    {
        ZmHttpServer* server = (ZmHttpServer*)arg;
        ZmHttpdDoer* doer = server->AcquireDoer(request);
        server->m_threadPool->Submit([doer]() { doer->Process(); }, "Doer");
    }
    else
    {
        evhttp_send_error(request, 500, nullptr);
    }
}

void ZmHttpServer::OnEventControl(evutil_socket_t fd, short what, void* ctx)
{
    if (ctx)
    {
        // 通过位与判断触发了哪种控制信号
        if (ZmHttpServer::ZM_HTTPD_CONTROL_REPLY & what)
        {
            // 工作线程请求发送完整响应
            ZmHttpdDoer* doer = (ZmHttpdDoer*)ctx;
            doer->SendReply();
            // ★ 回收至对象池（替代 delete this）
            doer->HttpServer()->RecycleDoer(doer);
        }
        if (ZmHttpServer::ZM_HTTPD_CONTROL_STREAM_START & what)
        {
            // 工作线程请求开始流式响应（仅发送响应头）
            ZmHttpdDoer* doer = (ZmHttpdDoer*)ctx;
            doer->SendReplyStart();
            // ★ 不回收：流式响应进行中，等待 CHUNK / END 信号
        }
        if (ZmHttpServer::ZM_HTTPD_CONTROL_STREAM_CHUNK & what)
        {
            // 工作线程请求发送一个流式数据块
            ZmHttpdDoer* doer = (ZmHttpdDoer*)ctx;
            doer->SendReplyChunkToClient();
            // ★ 不回收：流式响应进行中
        }
        if (ZmHttpServer::ZM_HTTPD_CONTROL_STREAM_END & what)
        {
            // 工作线程请求结束流式响应
            ZmHttpdDoer* doer = (ZmHttpdDoer*)ctx;
            doer->SendReplyEnd();
            // ★ 流式响应结束，回收至对象池
            doer->HttpServer()->RecycleDoer(doer);
        }
    }
}

int ZmHttpServer::OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
    return m_on_request ? m_on_request(task, data, dlen) : ZM_HTTP_STATUS_CODE_NOT_FOUND;
}

bool ZmHttpServer::BindEventBase(struct event_base* evbase)
{
    bool ret = false;
    m_port_bind_failed = false;
    m_evhttpd = evhttp_new(evbase);

    // 设置服务器支持的 HTTP 方法
    evhttp_set_allowed_methods(m_evhttpd, EVHTTP_REQ_GET |
                                         EVHTTP_REQ_POST |
                                         EVHTTP_REQ_HEAD |
                                         EVHTTP_REQ_PUT |
                                         EVHTTP_REQ_DELETE |
                                         EVHTTP_REQ_OPTIONS |
                                         EVHTTP_REQ_TRACE |
                                         EVHTTP_REQ_CONNECT |
                                         EVHTTP_REQ_PATCH |
                                         EVHTTP_REQ_PROPFIND |
                                         EVHTTP_REQ_PROPPATCH |
                                         EVHTTP_REQ_MKCOL |
                                         EVHTTP_REQ_LOCK |
                                         EVHTTP_REQ_UNLOCK |
                                         EVHTTP_REQ_COPY |
                                         EVHTTP_REQ_MOVE);

    // 设置通用请求回调，所有进入的请求都走 OnHttp_RequestCB
    evhttp_set_gencb(m_evhttpd, ZmHttpServer::OnHttpRequestCB, this);

    // 不限制最大并发连接数（0 表示禁用限制），实际上限由线程池和系统资源决定
    evhttp_set_max_connections(m_evhttpd, 0);

    do
    {
        if (m_local_port)
        {
            // 绑定到 0.0.0.0 表示接受所有网卡的请求（包括 127.0.0.1 和物理网卡 IP）
            struct evhttp_bound_socket* handle = evhttp_bind_socket_with_handle(m_evhttpd, "0.0.0.0", m_local_port);
            if (!handle)
            {
                const char* errmsg = evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
                PUBLIC_LOG_ERROR("Bind port {} on HTTPD server failed:{}, errMsg: {}", m_local_port, EVUTIL_SOCKET_ERROR(), errmsg);
                break;
            }
        }
        ret = true;
    } while (false);

    if (ret)
    {
        // 端口绑定成功
    }
    else
    {
        m_port_bind_failed = true;
        // 绑定失败时释放已创建的 evhttp 对象，避免资源泄漏
        // evbase 由外部管理生命周期
        if (m_evhttpd)
        {
            evhttp_free(m_evhttpd);
            m_evhttpd = nullptr;
        }
    }

    return ret;
}

// ============================ ZmJsonRpcServer ============================

ZmJsonRpcServer::ZmJsonRpcServer(struct event_base* evbase, std::string_view root_uri, uint16_t local_port)
    : ZmHttpServer(evbase, local_port)
{
    if (!root_uri.empty())
    {
        snprintf(m_root_uri, sizeof(m_root_uri), "%.*s", (int)root_uri.size(), root_uri.data());
    }
    else
    {
        // root_uri 为空时清零，后续 ZmString::IsEmpty 返回 404，
        // 使 OnHttpdRequest 中所有请求都走 JRPC 流程
        memset(m_root_uri, 0, sizeof(m_root_uri));
        PUBLIC_LOG_INFO("You haven't set the root_uri for HTTP_SERVER, so all requests will return a 404 error");
    }

    std::string poolName = "ZmJsonRpcServer:" + std::to_string(local_port);
    SetPoolName(poolName);
}

ZmJsonRpcServer::~ZmJsonRpcServer()
{}

ZMJSON ZmJsonRpcServer::MakeError(int code, std::string_view message)
{
    return ZMJSON{ {"code", code}, {"message", message} };
}

void ZmJsonRpcServer::SetJsonRpcCB(OnJsonRpcRequestCB oncall)
{
    m_on_jsonrpc_call = oncall;
}

void ZmJsonRpcServer::SetJsonRpcCBAsync(OnJsonRpcRequestCBAsync oncall_async)
{
    m_on_jsonrpc_call_async = oncall_async;
}

int ZmJsonRpcServer::OnJsonRpcRequest(ZmHttpdTask* task, const ZMJSON& request, ZMJSON& response)
{
    return m_on_jsonrpc_call ? m_on_jsonrpc_call(task, request, response) : -1;
}

bool ZmJsonRpcServer::OnJsonRpcRequestAsync(ZmHttpdTask* task, const ZMJSON& request, ZMJSON& reply)
{
    if (m_on_jsonrpc_call_async)
    {
        // 异步回调：reply 拷贝到堆上（shared_ptr），确保 OnHttpdRequest 返回后仍然存活
        // reply 是 ZMJSON& 引用，按值捕获只拷贝引用本身，必须显式拷对象到堆
        auto replyPtr = std::make_shared<ZMJSON>(reply);
        m_on_jsonrpc_call_async(task, request,
            [this, task, replyPtr](const ZMJSON& response) mutable
            {
                OnJsonRpcAsyncReply(this, task, *replyPtr, response);
            });

        return true; // 异步处理中，响应稍后到达
    }

    return  false;
}

void ZmJsonRpcServer::OnJsonRpcAsyncReply(ZmJsonRpcServer* server, ZmHttpdTask* task, ZMJSON& reply, const ZMJSON& response)
{
    // 通过共用方法构造响应（自动处理 JSONP callback 和 Server 头）
    server->BuildJsonRpcResponse(task, reply, response);
    // 投递 REPLY 信号到 HTTP 服务器的 event loop → SendReply → evhttp_send_reply
    task->TriggerReply();
}

void ZmJsonRpcServer::BuildJsonRpcResponse(ZmHttpdTask* task, ZMJSON& reply, const ZMJSON& response)
{
    ZMJSON error   = response.value("error",   ZMJSON());
    ZMJSON result  = response.value("result",  ZMJSON());
    ZMJSON headers = response.value("headers", ZMJSON());

    // JSON-RPC 2.0 规范: 响应中 result 和 error 二选一
    if (!error.empty())
    {
        reply["error"] = error;
    }
    else if (!result.empty())
    {
        reply["result"] = result;
    }
    else
    {
        reply["error"] = MakeError(ZM_JRPC_ERR_INTERNAL, "Invalid Response, the response must contain either error or result");
    }

    // 从 task 中获取 callback 参数（JSONP 支持），无需外部捕获
    std::string callback = task->GetQueryValue("callback");
    std::string body;
    std::string content_type;

    if (callback.empty())
    {
        // 标准 JSON 响应
        body = reply.dump();
        content_type = "application/json; charset=UTF-8";
    }
    else
    {
        // JSONP 响应格式: callback(jsonString)
        body = callback + "(" + reply.dump() + ")";
        content_type = "application/javascript";
    }

    // 业务层传入的自定义响应头（如 Set-Cookie 等）
    // 支持 string/number/bool 单值 和 array 多值（同名 header 发多次）
    if (headers.is_object())
    {
        auto putValue = [&](const char* key, const ZMJSON& v)
        {
            if (v.is_string())
                task->PutReplyHeader(key, v.get_ref<const std::string&>().c_str());
            else if (v.is_number() || v.is_boolean())
                task->PutReplyHeader(key, v.dump().c_str());
        };

        for (auto& [key, val] : headers.items())
        {
            if (val.is_array())
            {
                for (auto& item : val)
                    putValue(key.c_str(), item);
            }
            else
            {
                putValue(key.c_str(), val);
            }
        }
    }

    task->PutReplyHeader("Content-type", content_type.c_str());
    task->SetReply(ZM_HTTP_STATUS_CODE_OK);
    task->SetReplyData((const BYTE*)body.data(), body.size());
}

int ZmJsonRpcServer::OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
    // 暂定JRPC请求的rui一律使用完全匹配
    if (ZmString::IsEmpty(m_root_uri) || !ZmString::Equals(task->Uri(), m_root_uri, true))
    {
        return ZM_HTTP_STATUS_CODE_NOT_FOUND;
    }

    std::string  errmsg;
    int          errcode = 0;

    ZMJSON       reply;
    ZMJSON       response;
    ZmByteBuffer buf(dlen, data);

    // GET 请求通过 query string 的 jsonbody 参数传递 Base64 编码的 JSON 请求体
    // 例如: GET /rpc?callback=auth&jsonbody=eyJtZXRob2QiOiJsb2dpbiJ9
    if (EVHTTP_REQ_GET == task->Method())
    {
        std::string jsonbody = task->GetQueryValue("jsonbody");
        if (!jsonbody.empty())
        {
            // Base64Decode 内部会重新分配 buf 大小为解码后的预期长度
            ZmString::Base64Decode(buf, jsonbody.data(), jsonbody.size());
        }
    }

    // 解析 JSON 请求体，errmsg 非空表示解析失败
    ZMJSON request = zm_json_parse(buf.Str(), errmsg);
    if (errmsg.empty())
    {
        // 构造响应信封的基础部分（id / jsonrpc / method），捕获给 reply 闭包
        //id 原样回传，便于客户端匹配响应
        if (!request["id"].is_null())
        {
            reply["id"] = request["id"];
        }

        // 校验 JSON-RPC 版本号，必须为 JRPC_VERSION
        std::string jsonrpc_ver = zm_json_get_str(request, "jsonrpc");
        if (jsonrpc_ver != JRPC_VERSION)
        {
            errcode = ZM_JRPC_ERR_INVALID_REQUEST;
            errmsg = "Invalid Request: jsonrpc must be " JRPC_VERSION;
        }
        else
        {
            // 构造 JSON-RPC 2.0 标准响应
            reply["jsonrpc"] = jsonrpc_ver;

            std::string method = zm_json_get_str(request, "method");
            if (!method.empty())
            {
                // 将请求中的 method 原样回传，便于客户端匹配响应
                reply["method"] = method;

                ZMJSON params = request["params"];
                if (!params.is_object())
                {
                    // JSON-RPC 2.0: params 必须是对象
                    errcode = ZM_JRPC_ERR_INVALID_PARAMS;
                    errmsg = "Invalid params";
                }
                else
                {
                    // 将 task 中存储的请求头添加到 request 中
                    nlohmann::json::object_t headers;
                    task->GetRequestHeaders(headers);

                    request.clear();
                    request["method"] = method;
                    request["params"] = params;
                    request["headers"] = headers;

                    // 异步路径优先：设置了异步回调则忽略同步回调
                    if (OnJsonRpcRequestAsync(task, request, reply))
                    {
                        return -1; // 异步处理中，响应稍后到达
                    }
                    // 回调返回 < 0 表示该 method 不存在, 等于0表示没设置同步回调, 所以业务层不要返回0
                    else if (int ret = OnJsonRpcRequest(task, request, response))
                    {
                        if (ret < 0)
                        {
                            errcode = ZM_JRPC_ERR_METHOD_NOT_FOUND;
                            errmsg = "Method not found";
                        }
                    }
                    else
                    {
                        errcode = ZM_JRPC_ERR_PORTAL_NOJRPC;
                        errmsg = "Internal portal does not have JRPC processing channel set up";
                    }
                }
            }
            else
            {
                // 缺少 method 字段
                errcode = ZM_JRPC_ERR_INVALID_REQUEST;
                errmsg = "Invalid Request, Method field is required";
            }
        }
    }
    else
    {
        // JSON 解析失败
        errcode = ZM_JRPC_ERR_PARSE;
        errmsg = "Parse request json error";
    }

    if (errcode)
    {
        response.clear();
        response["error"] = MakeError(errcode, errmsg.c_str());
    }

    BuildJsonRpcResponse(task, reply, response);

    // JSON-RPC 层面的错误通过响应体中的 error 字段传达，HTTP 层面始终返回 200
    return ZM_HTTP_STATUS_CODE_OK;
}

// ============================================================================
// ZmRESTfulServer — RESTful HTTP 服务器实现
// ============================================================================

ZmRESTfulServer::ZmRESTfulServer(struct event_base* evbase, std::string_view root_uri, uint16_t local_port)
    : ZmHttpServer(evbase, local_port)
{
    if (!root_uri.empty())
    {
        size_t len = root_uri.size() < sizeof(m_root_uri) - 1 ? root_uri.size() : sizeof(m_root_uri) - 1;
        memcpy(m_root_uri, root_uri.data(), len);
        m_root_uri[len] = '\0';
    }
    else
    {
        memset(m_root_uri, 0, sizeof(m_root_uri));
    }

    std::string poolName = "ZmRESTfulServer:" + std::to_string(local_port);
    SetPoolName(poolName);
}

ZmRESTfulServer::~ZmRESTfulServer()
{}

void ZmRESTfulServer::SetRESTfulCB(OnRESTfulRequestCB oncall)
{
    m_on_restful_call = oncall;
}

void ZmRESTfulServer::SetRESTfulCBAsync(OnRESTfulRequestCBAsync oncall)
{
    m_on_restful_async = oncall;
}

int ZmRESTfulServer::OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
    // ① URI 前缀过滤
    if (m_root_uri[0])
    {
        const char* uri = task->Uri();
        if (!uri || strncmp(uri, m_root_uri, strlen(m_root_uri)) != 0)
            return 0;  // 不匹配前缀，返回 404
    }

    // ② 异步优先（和 JRPC 一样）— 全部请求打包走 pair → Hub → delegate
    if (m_on_restful_async)
    {
        m_on_restful_async(task, data, dlen);
        return -1;
    }

    // ③ 同步兜底
    if (m_on_restful_call)
    {
        int code = m_on_restful_call(task, data, dlen);
        return code;
    }

    return 0;  // 没设置任何回调
}

// ============================================================================
// 工具方法
// ============================================================================

void ZmRESTfulServer::ReplyJson(ZmHttpdTask* task, int code, const ZMJSON& data)
{
    std::string json = data.dump();
    task->PutReplyHeader("Content-Type", "application/json; charset=utf-8");
    task->SetReply(code);
    task->SetReplyData((const BYTE*)json.c_str(), json.size());
    task->TriggerReply();
}

void ZmRESTfulServer::ReplyError(ZmHttpdTask* task, int code, std::string_view msg)
{
    ZMJSON err = {{"error", {{"code", code}, {"message", msg}}}};
    ReplyJson(task, code, err);
}

void ZmRESTfulServer::ReplyEmpty(ZmHttpdTask* task, int code, const char* reason)
{
    task->SetReply(code, reason);
    task->TriggerReply();
}

void ZmRESTfulServer::ReplyRedirect(ZmHttpdTask* task, const char* location, int code)
{
    task->PutReplyHeader("Location", location);
    task->SetReply(code);
    task->TriggerReply();
}
