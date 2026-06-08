/**
 * @file zm_net_http.cpp
 * @brief 基于 libevent 的多线程 HTTP 服务器实现
 */

#include "zm_net_http.h"
#include "zm_net_socket.h"
#include "zm_net_ip.h"
#include "../util/zm_util_libevent.h"
#include "../define/zm_version_define.h"
#include "../spdlog/zm_logger.h"

#include <event2/bufferevent.h>



 ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
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

int ZmHttpUtil::ParseVerb(const char* method)
{
    // GET POST CONNECT PUT DELETE OPTIONS PATCH TRACE HEAD
    if (strcmp(method, "GET") == 0) { return ZM_HTTP_VERB_GET; }
    else if (strcmp(method, "POST") == 0) { return ZM_HTTP_VERB_POST; }
    else if (strcmp(method, "CONNECT") == 0) { return ZM_HTTP_VERB_CONNECT; }
    else if (strcmp(method, "PUT") == 0) { return ZM_HTTP_VERB_PUT; }
    else if (strcmp(method, "DELETE") == 0) { return ZM_HTTP_VERB_DELETE; }
    else if (strcmp(method, "OPTIONS") == 0) { return ZM_HTTP_VERB_OPTIONS; }
    else if (strcmp(method, "PATCH") == 0) { return ZM_HTTP_VERB_PATCH; }
    else if (strcmp(method, "TRACE") == 0) { return ZM_HTTP_VERB_TRACE; }
    else if (strcmp(method, "HEAD") == 0) { return ZM_HTTP_VERB_HEAD; }
    return ZM_HTTP_VERB_NONE;
}

int ZmHttpUtil::StartWithVerbs(const char* buf)
{
    // GET POST CONNECT PUT DELETE OPTIONS PATCH TRACE HEAD
    if (strncmp(buf, "GET ", 4) == 0) { return ZM_HTTP_VERB_GET; }
    else if (strncmp(buf, "POST ", 5) == 0) { return ZM_HTTP_VERB_POST; }
    else if (strncmp(buf, "CONNECT ", 8) == 0) { return ZM_HTTP_VERB_CONNECT; }
    else if (strncmp(buf, "PUT ", 4) == 0) { return ZM_HTTP_VERB_PUT; }
    else if (strncmp(buf, "DELETE ", 7) == 0) { return ZM_HTTP_VERB_DELETE; }
    else if (strncmp(buf, "OPTIONS ", 8) == 0) { return ZM_HTTP_VERB_OPTIONS; }
    else if (strncmp(buf, "PATCH ", 6) == 0) { return ZM_HTTP_VERB_PATCH; }
    else if (strncmp(buf, "TRACE ", 6) == 0) { return ZM_HTTP_VERB_TRACE; }
    else if (strncmp(buf, "HEAD ", 5) == 0) { return ZM_HTTP_VERB_HEAD; }
    return ZM_HTTP_VERB_NONE;
}

// code referers libevent source http.c evhttp_parse_request_line()
// GET scheme:[//[user:password@]host[:port]][/]path[?query][#fragment] HTTP/1.1
bool ZmHttpUtil::ParseRequest(ZM_HTTP_REQ* req, const char* line, int verb)
{
    char* method;
    char* uri;
    char* version;

    ZmByteBuffer  heap(strlen(line), line);

    char* reqline = heap.Str();
    /* Parse the request line */
    method = zm_strsep(&reqline, " ");
    if (reqline == NULL)
    {
        return false;
    }
    uri = zm_strsep(&reqline, " ");
    if (reqline == NULL)
    {
        return false;
    }
    version = zm_strsep(&reqline, " ");
    if (reqline != NULL)
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
void ZmHttpUtil::ParseUri(ZM_HTTP_REQ* req, const char* uri_)
{
    if (req == NULL || uri_ == NULL)
    {
        return;
    }

    if (req->host) { free(req->host); }
    if (req->userinfo) { free(req->userinfo); }
    if (req->path) { free(req->path); }
    if (req->useragent) { free(req->useragent); }
    req->host = NULL;
    req->userinfo = NULL;
    req->path = NULL;
    req->useragent = NULL;

    ZmByteBuffer heap(strlen(uri_), uri_);
    char* uri = heap.Str();

    // scheme
    char* scheme = strstr(uri, "://");
    if (NULL != scheme)
    {
        snprintf(req->scheme, sizeof(req->scheme), "%s", zm_strsep(&uri, "://"));
        uri += 2;
    }

    // path
    char* path = strchr(uri, '/');
    req->path = _strdup(NULL != path ? path : "/");

    // host and port
    char* host = (NULL != path) ? zm_strsep(&uri, "/") : uri;

    // userinfo
    if (NULL != strchr(host, '@'))
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
         * +- 2019.06.20
         *  兼容类似这样的 IPv6 代理请求: CONNECT fec0::2:0:0:1092:285:443 HTTP/1.1
         *  因此修改为从右搜索冒号
         */
         // -- req->host = y_strdup(y_strsep(&host, ":"));
         // -- if ( NULL!=host )
         // -- {
         // --     req->port = (uint16_t)atoi(host);
         // -- }
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
    char* next_token = NULL;
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
        pstr = strtok_s(NULL, delims, &next_token);
    }
}

const char* ZmHttpUtil::GetQuery(const ZM_HTTP_URI* uri, const char* name)
{
    for (size_t i = 0; i < uri->qcnt; i++)
    {
        if (strcmp(uri->query[i].name, name) == 0)
        {
            return uri->query[i].value ? uri->query[i].value : "";
        }
    }
    return "";
}

std::string ZmHttpUtil::HeaderGetValue(struct evkeyvalq* headers, const char* key, const char* defv)
{
    const char* val = evhttp_find_header(headers, key);
    return std::string(val ? val : defv);
}

/** 不破坏数据的情况下提取 Status-Code */
int ZmHttpUtil::ParseStatusCode(const char* statusLine, const char* limit)
{
    int statusCode = 0;
    // Status-Line = HTTP-Version SP Status-Code SP Reason-Phrase CRLF
    const char* ptr = strchr(statusLine, ' ');
    if (ptr && ptr < limit)
    {
        ptr++;
        const char* endptr = strchr(ptr, ' ');
        if (endptr && endptr < limit)
        {
            // The Status-Code element is a 3-digit integer result code
            while (ptr < endptr)
            {
                if (*ptr < '0' || *ptr>'9' || statusCode > 1000) { return 0; }
                statusCode = statusCode * 10 + (*ptr - '0');
                ptr++;
            }
        }
    }
    return statusCode;
}


















// ============================ ZmHttpdTask ============================

ZmHttpdTask::ZmHttpdTask(struct evhttp_request* request) : m_request(request), m_status_code(0)
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
    // 创建用于存储响应体的 evbuffer，后续通过 SetReplyData 写入
    m_reply_buf = evbuffer_new();
}

ZmHttpdTask::~ZmHttpdTask()
{
    evhttp_clear_headers(&m_query);
    if (m_reply_buf)
    {
        evbuffer_free(m_reply_buf);
    }
    m_reply_buf = NULL;
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

const char* ZmHttpdTask::GetQueryValue(const char* name, const char* defv)
{
    const char* val = evhttp_find_header(&m_query, name);
    return val ? val : defv;
}

void ZmHttpdTask::GetRequestHeaders(nlohmann::json::object_t& headersObj)
{
    struct evkeyvalq* headerKeyVals = evhttp_request_get_input_headers(const_cast<struct evhttp_request*>(m_request));
    if (headerKeyVals)
    {
        // 遍历 libevent 内部的 tailq 链表（tqh_first / tqe_next）读取所有请求头
        for (struct evkeyval* h = headerKeyVals->tqh_first; h; h = h->next.tqe_next)
        {
            headersObj[h->key] = h->value;
        }
    }
}

const char* ZmHttpdTask::GetRequestHeader(const char* name, const char* defv)
{
    const struct evkeyvalq* headers = evhttp_request_get_input_headers(m_request);
    const char* val = evhttp_find_header(headers, name);
    return val ? val : defv;
}

void ZmHttpdTask::PutReplyHeader(const char* name, const char* val)
{
    // val 为 NULL 时存储空字符串，防止后续构造 string 时崩溃
    m_reply_headers[std::string(name)] = std::string(val ? val : "");
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




// ============================ ZmHttpServer internals ============================

/**
 * @brief HTTP 请求处理任务，由线程池调度执行（不再继承 ZmThread）
 *
 * 生命周期:
 *   1. 事件循环线程创建 ZmHttpdDoer 并启动工作线程
 *   2. 工作线程执行 Run() → Perform() → event_active(REPLY)
 *   3. 事件循环线程收到 REPLY 信号 → SendReply() → 启动 1 秒定时器
 *   4. 定时器触发 → SendReplyEnd() → delete this
 *
 * @note 此类仅在 cpp 内部使用，不对外暴露
 */
class ZmHttpdDoer : public ZmHttpdTask
{
public:
    /**
     * @brief 构造请求处理任务（不再继承 ZmThread，由线程池调度）
     * @param httpd    所属的 HTTP 服务器实例
     * @param request  libevent HTTP 请求对象
     */
    ZmHttpdDoer(ZmHttpServer* httpd, struct evhttp_request* request)
        : ZmHttpdTask(request), m_httpd(httpd)
    {
        m_remove_event = NULL;
        m_reply_event = event_new(m_httpd->EventBase(), -1, EV_PERSIST | EV_READ,
            ZmHttpServer::OnEvent_Control, this);
        event_add(m_reply_event, NULL);
    }

    /**
     * @brief 析构，释放回复事件和延迟释放事件
     */
    ~ZmHttpdDoer()
    {
        if (m_reply_event)
        {
            event_free(m_reply_event);
            m_reply_event = NULL;
        }
        if (m_remove_event)
        {
            event_free(m_remove_event);
            m_remove_event = NULL;
        }
    }

    /**
     * @brief 在事件循环线程中发送 HTTP 响应，并启动延迟释放定时器
     *
     * 执行流程:
     *   1. 释放 m_reply_event（已不需要）
     *   2. 将 m_reply_headers 中的响应头写入 evhttp_request
     *   3. 调用 evhttp_send_reply 发送响应
     *   4. 创建 1 秒定时器，到期后执行 SendReplyEnd 释放自身
     *
     * @note 延迟 1 秒释放是为了确保 libevent 已完成响应数据的网络发送
     */
    void SendReply()
    {
        if (m_reply_event)
        {
            event_free(m_reply_event);
            m_reply_event = NULL;
        }
        // 将收集到的响应头统一写入 evhttp_request 的输出头
        // 此处必须在事件循环线程执行，因为 evhttp_add_header 操作 libevent 内部结构
        for (auto it = m_reply_headers.begin(); it != m_reply_headers.end(); it++)
        {
            evhttp_add_header(evhttp_request_get_output_headers(m_request), it->first.c_str(), it->second.c_str());
        }
        evhttp_send_reply(m_request, m_status_code, m_reason.empty() ? NULL : m_reason.c_str(), m_reply_buf);

        // 创建 1 秒一次性定时器，到期后由 OnEvent_Timer 回调触发 SendReplyEnd
        struct timeval tv = { 1, 0 };
        if (m_remove_event)
        {
            event_free(m_remove_event);
            m_remove_event = NULL;
        }
        m_remove_event = event_new(m_httpd->EventBase(), -1, EV_TIMEOUT, ZmHttpServer::OnEvent_Timer, this);
        evtimer_add(m_remove_event, &tv);
    }

    /**
     * @brief 释放定时器事件并销毁自身（delete this）
     *
     * @note 由 1 秒定时器触发，确保响应已发送完毕后才释放资源
     */
    void SendReplyEnd()
    {
        if (m_remove_event)
        {
            event_free(m_remove_event);
            m_remove_event = NULL;
        }
        delete this;
    }

    /**
     * @brief 由线程池调用的处理入口，执行请求处理并通知事件循环线程
     */
public:
    void Process()
    {
        m_httpd->Perform(this);
        event_active(m_reply_event, ZmHttpServer::ZM_HTTPD_CONTROL_REPLY, 0);
    }

private:
    /** @brief 所属的 HTTP 服务器实例 */
    ZmHttpServer* m_httpd;

    /** @brief 用于接收工作线程"回复就绪"信号的事件 */
    struct event* m_reply_event;

    /** @brief 响应发送后的延迟释放定时器事件 */
    struct event* m_remove_event;
};

/**
 * @brief 将 HTTP 请求信息格式化输出到日志（当前日志输出已注释）
 * @param req  libevent HTTP 请求对象
 *
 * @note 此函数为 cpp 内部使用的调试辅助函数
 */
static void YDumpHttpRequest(const struct evhttp_request* req)
{
    const char* method = NULL;
    switch (evhttp_request_get_command(req))
    {
    case EVHTTP_REQ_GET:        method = "GET";        break;
    case EVHTTP_REQ_POST:       method = "POST";       break;
    case EVHTTP_REQ_HEAD:       method = "HEAD";       break;
    case EVHTTP_REQ_PUT:        method = "PUT";        break;
    case EVHTTP_REQ_DELETE:     method = "DELETE";     break;
    case EVHTTP_REQ_OPTIONS:    method = "OPTIONS";    break;
    case EVHTTP_REQ_TRACE:      method = "TRACE";      break;
    case EVHTTP_REQ_CONNECT:    method = "CONNECT";    break;
    case EVHTTP_REQ_PATCH:      method = "PATCH";      break;
    default:                    method = "unknown";    break;
    }

    struct evkeyvalq* headers = evhttp_request_get_input_headers(const_cast<struct evhttp_request*>(req));
    int i = 1;
    for (struct evkeyval* header = headers->tqh_first; header; header = header->next.tqe_next, i++)
    {
    }

    struct evbuffer* inbuf = evhttp_request_get_input_buffer((evhttp_request*)req);
    size_t           dlen = evbuffer_get_length(inbuf);
    ZmByteBuffer buf(dlen, evbuffer_pullup(inbuf, dlen));
}


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

void ZmHttpHead::Parse(const char* buf, size_t len, bool hasReqLine)
{
    _entries.Clear();
    const char* hstr = NULL;
    if (hasReqLine)
    {
        hstr = strstr(buf, "\r\n");
        _status_code = ZmHttpUtil::ParseStatusCode(buf, hstr);
        len = len ? (len - (hstr - buf - 2)) : strlen(hstr);
    }
    else
    {
        hstr = buf;
        len = len ? len : strlen(hstr);
    }
    ZmByteBuffer str(len, hstr);
    char  delims[] = "\r\n";
    char* next_token = NULL;
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

        pstr = strtok_s(NULL, delims, &next_token);
    }
}

void ZmHttpHead::Build(ZmByteBuffer& output)
{
    ZmByteBuffer tmp(4096);
    size_t offset = 0;
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        _ENTRY* e = _entries.At(i);
        // SP_LOGT("[%ld], %s: %s", i, e->name, e->value);
        offset += snprintf(tmp.Str() + offset, tmp.Size() - offset, "%s: %s\r\n", e->name, e->value);
    }
    output.Reset(offset, tmp.Head());
}

void ZmHttpHead::BuildToBuffer(struct evbuffer* buf)
{
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        _ENTRY* e = _entries.At(i);
        // SP_LOGT("[%ld], %s: %s", i, e->name, e->value);
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

ZmHttpHead::_ENTRY* ZmHttpHead::QueryEntry(const char* name)
{
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        if (0 == _stricmp(_entries.At(i)->name, name))
        {
            return _entries.At(i);
        }
    }
    return NULL;
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
    //SP_LOGT("ZmHttpHead::Value count=%ld, %s=%s", (long)_entries.Count(), name, value);
    return entry ? entry->value : NULL;
}

void ZmHttpHead::Remove(const char* name)
{
    //SP_LOGT("ZmHttpHead::Remove count=%ld, name=%s", (long)_entries.Count(), name);
    for (size_t i = 0; i < _entries.Count(); i++)
    {
        if (0 == _stricmp(_entries.At(i)->name, name))
        {
            _entries.Remove(i);
            return;
        }
    }
}

void ZmHttpHead::SetHostField(const char* scheme, const char* host, uint16_t port)
{
    ZmByteBuffer temp(256);
    bool isIPv6 = (AF_INET6 == ZmNetIP::Validate(host));
    if ((strcmp(scheme, "http") == 0 && port != 80)
        || (strcmp(scheme, "https") == 0 && port != 443)
        || (strcmp(scheme, "ssl") == 0 && port != 443))
    {
        if (isIPv6) { temp.Sprintf("[%s]:%d", host, port); }
        else { temp.Sprintf("%s:%d", host, port); }
    }
    else
    {
        if (isIPv6) { temp.Sprintf("[%s]", host); }
        else { temp.Sprintf("%s", host); }
    }
    Value("Host", temp.Str());
}

// ============================ ZmHttpServer ============================

ZmHttpServer::ZmHttpServer(uint16_t local_port) : ZmThread("HTTPD"),
    m_evbase(NULL), m_evhttpd(NULL), m_ctrl_event(NULL), m_pool(nullptr),
    m_local_port(local_port), m_port_bind_failed(false), m_shutdown_requested(false)
{}

ZmHttpServer::~ZmHttpServer()
{
    FreeEventObjects();
}

void ZmHttpServer::OnStopping()
{
    m_shutdown_requested = true;
    if (ZmThread::CurrentThreadID() == ThreadID())
    {
        // 同线程直接打断事件循环
        OnControlClose();
    }
    else if (m_ctrl_event)
    {
        // 跨线程通过事件通知，安全地由事件循环线程执行关闭
        event_active(m_ctrl_event, ZM_HTTPD_CONTROL_CLOSE, 0);
    }
    else if (m_evbase)
    {
        // 控制事件尚未创建（BindEventBase 未完成），直接打断事件循环
        event_base_loopbreak(m_evbase);
    }
}

uint16_t ZmHttpServer::LocalPort()
{
    return m_local_port;
}

struct event_base* ZmHttpServer::EventBase()
{
    return m_evbase;
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

    YDumpHttpRequest(task->Request());

    // 自动添加 CORS 跨域响应头，允许所有来源访问
    task->PutReplyHeader("Access-Control-Allow-Origin", "*");
    task->PutReplyHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    task->PutReplyHeader("Access-Control-Allow-Headers", "*");

    // 浏览器预检请求（OPTIONS）直接返回 200，无需进入业务逻辑
    if (EVHTTP_REQ_OPTIONS == task->Method())
    {
        task->SetReply(200);
        return;
    }

    struct evbuffer* inbuf = evhttp_request_get_input_buffer(task->Request());
    size_t           dlen = evbuffer_get_length(inbuf);
    int              code = OnHttpdRequest(task, evbuffer_pullup(inbuf, dlen), dlen);
    // OnHttpdRequest 返回 0 表示该路径未被任何回调处理，默认 404
    if (0 == code)
    {
        code = 404;
    }
    task->SetReply(code);
}

void ZmHttpServer::OnHttp_RequestCB(struct evhttp_request* request, void* arg)
{
    const char* uri = evhttp_request_get_uri(request);
    if (uri && arg)
    {
        ZmHttpServer* server = (ZmHttpServer*)arg;
        ZmHttpdDoer* doer = new ZmHttpdDoer(server, request);
        server->m_pool->Submit([doer]() { doer->Process(); });
    }
    else
    {
        evhttp_send_error(request, 500, NULL);
    }
}

void ZmHttpServer::OnEvent_Control(evutil_socket_t fd, short what, void* ctx)
{
    if (ctx)
    {
        // 通过位与判断触发了哪种控制信号
        if (ZmHttpServer::ZM_HTTPD_CONTROL_REPLY & what)
        {
            // 工作线程请求发送响应
            ZmHttpdDoer* doer = (ZmHttpdDoer*)ctx;
            doer->SendReply();
        }
        else if (ZmHttpServer::ZM_HTTPD_CONTROL_REPLY_END & what)
        {
            // 延迟释放工作线程资源
            ZmHttpdDoer* doer = (ZmHttpdDoer*)ctx;
            doer->SendReplyEnd();
        }
        else if (ZmHttpServer::ZM_HTTPD_CONTROL_CLOSE & what)
        {
            // 外部线程请求关闭服务器
            ZmHttpServer* hs = (ZmHttpServer*)ctx;
            hs->OnControlClose();
        }
    }
}

void ZmHttpServer::OnEvent_Timer(evutil_socket_t fd, short event, void* arg)
{
    ZmHttpdDoer* doer = (ZmHttpdDoer*)arg;
    if (doer)
    {
        doer->SendReplyEnd();
    }
}

int ZmHttpServer::OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
    // 返回 0 表示此路径不支持，Perform 中会将 0 覆盖为 404
    return m_on_request ? m_on_request(task, data, dlen) : 0;
}

void ZmHttpServer::Run()
{
    // 初始化 libevent 线程支持（Windows 下调用 evthread_use_windows_threads）
    // 必须在 event_base_new 之前调用，全局只需调用一次
    zm_util_eventbase_init();
    FreeEventObjects();

    m_evbase = event_base_new();
    bool bindRet = BindEventBase(m_evbase);
    // 检查 m_shutdown_requested 防止在 BindEventBase 完成前收到 Shutdown 导致进入事件循环
    if (bindRet && !m_shutdown_requested)
    {
        // 创建工作线程池（线程复用，替代 thread-per-request）
        m_pool = new ZmThreadPool(
            (uint16_t)std::thread::hardware_concurrency());
        event_base_dispatch(m_evbase);
    }

    FreeEventObjects();
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
    evhttp_set_gencb(m_evhttpd, ZmHttpServer::OnHttp_RequestCB, this);

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
        // 创建控制事件用于接收外部线程的信号（Shutdown、Reply 等）
        // fd=-1 表示不关联 socket，EV_PERSIST 允许重复触发
        m_ctrl_event = event_new(evbase, -1, EV_PERSIST | EV_READ, ZmHttpServer::OnEvent_Control, (void*)this);
        event_add(m_ctrl_event, NULL);
    }
    else
    {
        m_port_bind_failed = true;
        // 绑定失败时释放已创建的 evhttp 对象，避免资源泄漏
        // evbase 由 FreeEventObjects 在 Run() 中释放
        if (m_evhttpd)
        {
            evhttp_free(m_evhttpd);
            m_evhttpd = NULL;
        }
    }

    return ret;
}

void ZmHttpServer::OnControlClose()
{
    if (m_evbase)
    {
        // 打断事件循环，使 event_base_dispatch 返回
        event_base_loopbreak(m_evbase);
    }
}

void ZmHttpServer::FreeEventObjects()
{
    // 先停线程池（join 所有 worker，确保不再有任务访问 evbase）
    if (m_pool)
    {
        delete m_pool;
        m_pool = nullptr;
    }

    // 释放顺序: ctrl_event → evhttpd → evbase
    if (m_ctrl_event)
    {
        event_free(m_ctrl_event);
        m_ctrl_event = NULL;
    }
    if (m_evhttpd)
    {
        evhttp_free(m_evhttpd);
        m_evhttpd = NULL;
    }
    if (m_evbase)
    {
        event_base_free(m_evbase);
        m_evbase = NULL;
    }
}


// ============================ ZmJsonRpcServer ============================
/*
* JSON-RPC 2.0 Error Codes:
* -32700   Parse error         Invalid JSON was received by the server.
* -32600   Invalid Request     The JSON sent is not a valid Request object.
* -32601   Method not found    The method does not exist / is not available.
* -32602   Invalid params      Invalid method parameter(s).
* -32603   Internal error      Internal JSON-RPC error.
* -32000 to -32099             Server error (Reserved for implementation-defined server-errors).
*/

ZmJsonRpcServer::ZmJsonRpcServer(const char* root_uri, uint16_t local_port)
    : ZmHttpServer(local_port)
{
    if (root_uri)
    {
        snprintf(m_root_uri, sizeof(m_root_uri), "%s", root_uri);
    }
    else
    {
        // root_uri 为 NULL 时清零，后续 ZmString::IsEmpty 返回 true，
        // 使 OnHttpdRequest 中所有请求都走 RPC 流程
        memset(m_root_uri, 0, sizeof(m_root_uri));
    }
}

ZmJsonRpcServer::~ZmJsonRpcServer()
{}

ZMJSON ZmJsonRpcServer::MakeError(int code, const char* message)
{
    return ZMJSON{ {"code", code}, {"message", message} };
}

void ZmJsonRpcServer::SetJsonRpcCB(OnJsonRpcRequestCB oncall)
{
    m_on_jsonrpc_call = oncall;
}

void ZmJsonRpcServer::SetJsonRpcCBEx(OnJsonRpcRequestCBEx oncall_ex)
{
    m_on_jsonrpc_call_ex = oncall_ex;
}

int ZmJsonRpcServer::OnJsonRpcRequest(ZmHttpdTask* task, const char* method, const ZMJSON& params,
    ZMJSON& result, ZMJSON& error)
{
    // 优先使用带 task 参数的 CBEx，其次使用不带 task 参数的 CB，都没有则返回 -1
    return m_on_jsonrpc_call_ex ? m_on_jsonrpc_call_ex(task, method, params, result, error)
        : (m_on_jsonrpc_call ? m_on_jsonrpc_call(method, params, result, error) : -1);
}


/*
* -32700	Parse error	JSON 解析错误
* -32600	Invalid Request	不是有效的 JSON-RPC 2.0 请求
* -32601	Method not found	请求的方法不存在
* -32602	Invalid params	参数无效或不正确
* -32603	Internal error	服务器内部错误
* -32000 to -32099	Server error	服务器定义的错误（保留范围）
* -32000    Internal portal does not have JRPC processing channel set up 门户未设置jrpcReq回调函数
*/
int ZmJsonRpcServer::OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
    // URI 不以 root_uri 开头时，回退到父类的通用 HTTP 请求处理
    if (ZmString::IsEmpty(m_root_uri) || !ZmString::StartsWith(task->Uri(), m_root_uri))
    {
        return ZmHttpServer::OnHttpdRequest(task, data, dlen);
    }

    std::string  errmsg;
    int          errcode = 0;

    ZMJSON       rsp_reply;
    ZMJSON       rsp_result;
    ZMJSON       rsp_error;
    std::string  callback;
    std::string  rsp_content_type;
    std::string  rsp_server;
    std::string  rsp_body;
    std::string  jsonbody;
    ZmByteBuffer buf(dlen, data);

    // 获取 JSONP 回调函数名（非空时响应格式为 callback(json)）
    callback = task->GetQueryValue("callback");

    // GET 请求通过 query string 的 jsonbody 参数传递 Base64 编码的 JSON 请求体
    // 例如: GET /rpc?callback=auth&jsonbody=eyJtZXRob2QiOiJsb2dpbiJ9
    if (EVHTTP_REQ_GET == task->Method())
    {
        jsonbody = task->GetQueryValue("jsonbody");
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
        // 构造 JSON-RPC 2.0 标准响应
        rsp_reply["jsonrpc"] = "2.0";

        //id 原样回传，便于客户端匹配响应
        if (!request["id"].is_null())
        {
            rsp_reply["id"] = request["id"];
        }

        std::string method = zm_json_get_str(request, "method");
        if (!method.empty())
        {
            ZMJSON params = request["params"];
            if (!params.is_object())
            {
                // JSON-RPC 2.0: params 必须是对象
                errcode = -32602;
                errmsg = "Invalid params";
            }
            else if (OnJsonRpcRequest(task, method.c_str(), params, rsp_result, rsp_error) < 0)
            {
                // 回调返回 < 0 表示该 method 不存在
                errcode = -32601;
                errmsg = "Method not found";
            }

            // 将请求中的 method 原样回传，便于客户端匹配响应
            rsp_reply["method"] = method;
        }
        else
        {
            // 缺少 method 字段
            errcode = -32600;
            errmsg = "Invalid Request";
        }
    }
    else
    {
        // JSON 解析失败
        errcode = -32700;
        errmsg = "Parse error";
    }

    if (errcode)
    {
        rsp_error.clear();
        rsp_error["code"] = errcode;
        rsp_error["message"] = errmsg;
    }

    // JSON-RPC 2.0 规范: 响应中 result 和 error 二选一
    if (!rsp_error.empty())
    {
        rsp_reply["error"] = rsp_error;
    }
    else
    {
        rsp_reply["result"] = rsp_result;
    }

    // 根据是否有 JSONP callback 选择响应格式
    if (callback.empty())
    {
        // 标准 JSON 响应
        rsp_body = ZMJSON(rsp_reply).dump();
        rsp_content_type = "application/json; charset=UTF-8; callback=empty";
    }
    else
    {
        // JSONP 响应格式: callback(jsonString)
        rsp_body.erase();
        rsp_body.append(callback);
        rsp_body.append("(");
        rsp_body.append(ZMJSON(rsp_reply).dump());
        rsp_body.append(")");
        rsp_content_type = "application/javascript";
    }

    // 在 Server 响应头中附带服务端版本号
    rsp_server = "zimo_serrver_version=";
    rsp_server.append(ZIMO_SERVER_VERSION);

    task->PutReplyHeader("Content-type", rsp_content_type.c_str());
    task->PutReplyHeader("Server", rsp_server.c_str());
    task->SetReplyData((const BYTE*)rsp_body.data(), rsp_body.size());

    // JSON-RPC 层面的错误通过响应体中的 error 字段传达，HTTP 层面始终返回 200
    return 200;
}
