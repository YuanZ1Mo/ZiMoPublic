/**
 * @file zm_net_http.cpp
 * @brief 基于 libevent 的多线程 HTTP 服务器实现
 */

#include "zm_net_http.h"

#include "zm_net_websocket_server.h"   // m_wsServer 成员完整定义(new/Close/delete)
#include "zm_net_req_loop.h"   // close 通知器投递 CLOSE 需要 ZmReqLoop 完整定义
#include "zm_net_req_loop_pool.h"   // m_reqLoopPool 完整定义(new/Init/Acquire/Shutdown)
#include "zm_net_req_loop_protocol.h"   // ZmReqLoopJrpc(信封存储/static_cast 依赖)
#include "zm_net_socket.h"
#include "zm_net_ip.h"
#include "../util/zm_util_thread.h"
#include "../util/zm_util_libevent.h"
#include "../util/zm_util_logger.h"
#include "../ssl/zm_ssl_ctx.h"
#include "../define/zm_version_define.h"

#include <../libevent/include/event2/bufferevent.h>
#include <../libevent/include/event2/bufferevent_ssl.h>
#include <../libevent/include/event2/buffer.h>
#include <../openssl/include/openssl/ssl.h>
#include <../openssl/include/openssl/err.h>

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

bool ZmHttpdTask::IsHttps()
{
    // 请求可能为 nullptr（对象池预创建场景，见构造器注释）
    if (!m_request)
        return false;

    struct evhttp_connection* con = evhttp_request_get_connection(m_request);
    if (!con)
        return false;

    struct bufferevent* bev = evhttp_connection_get_bufferevent(con);
    if (!bev)
        return false;

    // 非 SSL bufferevent 返回 nullptr（libevent 源码 bufferevent_openssl.c:
    // bufferevent_openssl_get_ssl → bufferevent_ssl_upcast 失败即 NULL）
    return bufferevent_openssl_get_ssl(bev) != nullptr;
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
    if (fd < 0 || length < 0)
        return -1;

    // ★ 分段零拷贝:Windows 下 evbuffer_file_segment 的 MapViewOfFile 每视图长度
    //   受 32 位 DWORD 限制(buffer.c 转交长度参数),>4GB 整段映射失败 → 500。
    //   拆为 ≤2GB 的多视图拼接:每段一个独立映射窗口,>4GB 文件仍零拷贝。
    //   (Windows 原生 TransmitFile 不支持 TLS,本服务全 HTTPS,不可用;文件映射
    //   页缓存归系统管理可回收,慢客户端不产生用户态无界堆积。)
    // 段所有权:仅最后一个段挂 CLOSE_ON_FREE(fd 在全部数据发出后关闭一次);
    //   中间段共享同一 fd,若提前关 fd 会破坏后续段读取。
    //   契约:失败返回 -1 时不接管 fd(调用方负责关闭);成功时 fd 随响应释放。
    static constexpr ev_off_t kFileSegmentBytes = (ev_off_t)2 * 1024 * 1024 * 1024;

    for (ev_off_t off = 0; off < length; )
    {
        ev_off_t chunk = (length - off > kFileSegmentBytes) ? kFileSegmentBytes : (length - off);
        bool last = (off + chunk >= length);
        // 使用 evbuffer_file_segment 替代已废弃的 evbuffer_add_file
        // EVBUF_FS_DISABLE_SENDFILE: 禁用 Windows TransmitFile，只用 mmap(已在库内定义)
        struct evbuffer_file_segment* seg = evbuffer_file_segment_new(
            fd, offset + off, chunk,
            last ? EVBUF_FS_CLOSE_ON_FREE : 0);
        if (!seg)
            return -1;

        // 添加到响应缓冲区（evbuffer 增加段引用计数）
        if (evbuffer_add_file_segment(m_reply_buf, seg, 0, chunk) != 0)
        {
            // 失败仅释放未采用的段;已加入的段不回退(病态路径,映射失败集中在首段)
            evbuffer_file_segment_free(seg);
            return -1;
        }
        // 释放本地引用（evbuffer 仍持有引用，fd 不会在此处关闭）
        evbuffer_file_segment_free(seg);
        off += chunk;
    }
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

bool ZmHttpdTask::SetRateLimit(size_t download_bps, size_t upload_bps)
{
    if (download_bps == 0 && upload_bps == 0)
        return false;

    struct evhttp_connection* conn = evhttp_request_get_connection(m_request);
    if (!conn)
        return false;

    struct bufferevent* bev = evhttp_connection_get_bufferevent(conn);
    if (!bev)
        return false;

    // 令牌桶参数：libevent 要求 rate >= 1 且 burst >= rate
    // 为 0 的方向填入 EV_RATE_LIMIT_MAX，相当于不限速
    size_t read_rate   = upload_bps   ? upload_bps   : (size_t)EV_RATE_LIMIT_MAX;
    size_t read_burst  = upload_bps   ? upload_bps   : (size_t)EV_RATE_LIMIT_MAX;
    size_t write_rate  = download_bps ? download_bps : (size_t)EV_RATE_LIMIT_MAX;
    size_t write_burst = download_bps ? download_bps : (size_t)EV_RATE_LIMIT_MAX;

    struct ev_token_bucket_cfg* cfg = ev_token_bucket_cfg_new(
        read_rate,  read_burst,
        write_rate, write_burst,
        nullptr
    );

    if (!cfg)
        return false;

    int ret = bufferevent_set_rate_limit(bev, cfg);
    ev_token_bucket_cfg_free(cfg);  // bev 已持有自身引用，释放本地引用

    return ret == 0;
}

bool ZmHttpdTask::JoinRateLimitGroup(struct bufferevent_rate_limit_group* group)
{
    if (!group)
        return false;

    struct evhttp_connection* conn = evhttp_request_get_connection(m_request);
    if (!conn)
        return false;

    struct bufferevent* bev = evhttp_connection_get_bufferevent(conn);
    if (!bev)
        return false;

    return bufferevent_add_to_rate_limit_group(bev, group) == 0;
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
    // 注:close 通知由 ZmHttpServer 的 per-connection 通知器统一广播(方案 3),
    // 此处不再注册单槽 closecb(单槽会被同连接后续请求覆盖,且 doer 回收后悬垂)
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
        m_connClosed = false;
        m_boundLoop = nullptr;   // ★ 清旧请求的 A 绑定,防池复用后 close 投递到已释放的 loop
        m_userData.reset();      // ★ 清旧请求的用户数据(如 JRPC 响应信封),防池复用污染下一请求
        m_recycled = false;      // ★ 清回收标记:本次请求结束后才可再次 RecycleDoer 入池
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
        // CORS:浏览器带 Origin 时回显精确值并允许凭据(cookie 跨端口同站场景,
        // 如页面 443 → REST 39441);无 Origin(非浏览器/curl)保持通配 * 兼容。
        // 注:Allow-Origin 通配 * 与 Allow-Credentials 同时出现会被浏览器拒绝,故二选一。
        const char* reqOrigin = m_request
            ? evhttp_find_header(evhttp_request_get_input_headers(m_request), "Origin")
            : nullptr;
        if (reqOrigin && *reqOrigin)
        {
            PutReplyHeader("Access-Control-Allow-Origin", reqOrigin);
            PutReplyHeader("Access-Control-Allow-Credentials", "true");
            // 凭据模式下 Allow-Headers 通配符 * 不生效(Fetch 规范),须显式列出;
            // 优先回显预检请求声明的头列表,兜底 Content-Type(前端仅用此头)
            const char* reqHeaders = m_request
                ? evhttp_find_header(evhttp_request_get_input_headers(m_request),
                    "Access-Control-Request-Headers")
                : nullptr;
            if (reqHeaders && *reqHeaders)
                PutReplyHeader("Access-Control-Allow-Headers", reqHeaders);
            else
                PutReplyHeader("Access-Control-Allow-Headers", "Content-Type");
        }
        else
        {
            PutReplyHeader("Access-Control-Allow-Origin", "*");
            PutReplyHeader("Access-Control-Allow-Headers", "*");
        }
        PutReplyHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        PutReplyHeader("ZmHttpServer-Version", ZIMO_SERVER_VERSION);

        // ★ HSTS：仅在 HTTPS 模式下启用，告知浏览器"永远用 HTTPS 访问本站"
        // 后续可通过 OnConfigureSSL 的 mTLS 扩展点进一步定制安全头
        if (m_httpd->IsHttps())
            PutReplyHeader("Strict-Transport-Security", "max-age=31536000");

        for (auto it = m_reply_headers.begin(); it != m_reply_headers.end(); it++)
        {
            evhttp_add_header(evhttp_request_get_output_headers(m_request),
                it->first.c_str(), it->second.c_str());
        }
    }

    /** @brief 发送完整 HTTP 响应（一次性），由 ZM_HTTPD_CONTROL_REPLY 触发 */
    void SendReply()
    {
        // ★ 防御 doer 生命周期竞态(实测 _free_base/WriteResponseHeaders 崩溃):
        //   - m_connClosed:连接已关闭(closecb 置位),evhttp 已释放 request
        //   - m_recycled:doer 已回收入池后的残留 REPLY 信号(keep-alive 下 evhttp
        //     已释放 request 但连接还开着,m_connClosed=false 挡不住)
        //   两种情况都跳过发送,doer 回收由调用方 OnEventControl 的 RecycleDoer 完成。
        if (m_connClosed.load() || m_recycled)
            return;
        WriteResponseHeaders();
        evhttp_send_reply(m_request, m_status_code,
            m_reason.empty() ? nullptr : m_reason.c_str(), m_reply_buf);

        // ★ 生命周期由调用方 OnEventControl 管理，此处不回收
    }

    /** @brief 开始流式响应：写响应头，进入 chunked 传输模式 */
    void SendReplyStart()
    {
        WriteResponseHeaders();
        // 连接已先于流开始断开 → 标记关闭,
        // 发送线程将据此尽快退出,避免向已释放连接空发
        if (!evhttp_request_get_connection(m_request))
        {
            m_connClosed.store(true);
            return;
        }
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
    bool m_recycled = false;      ///< 已回收标记(REPLY|STREAM_END 双驱动兜底,防双入池;Reset 时清)

    friend class ZmHttpServer;   // RecycleDoer 访问 m_recycled(双驱动防重入护栏)
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
// 它们会在后续 REPLY 回调到达时由 RecycleDoer 回收(池满或 shutdown 时多余 doer 被 delete)。
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
        //    这些 doer 的 event 尚未触发 REPLY,由 RecycleDoer 回收(池满或 shutdown 时多余 doer 被 delete)
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

ZmHttpServer::ZmHttpServer(struct event_base* evbase, uint16_t local_port,
                         const char* certFile, const char* keyFile,
                         uint16_t redirect_from_port,
                         uint32_t sessionCacheSize,
                         const char* sessionContext)
    : m_evbase(evbase), m_evhttpd(nullptr), m_threadPoolName(), m_threadPool(nullptr),
      m_reqLoopPool(nullptr), m_loopPoolPrealloc(0), m_loopPoolMax(0),
      m_loopPoolBudgetMs(0), m_loopPoolEnabled(false),
      m_local_port(local_port), m_port_bind_failed(false), m_ssl_ctx(nullptr),
      m_oldCtx(nullptr), m_ctxCleanupTimer(nullptr),
      m_redirect_from_port(redirect_from_port), m_redirectEvhttp(nullptr),
      m_httpdDoerPool(nullptr), m_hasTicketKeys(false), m_sessionCacheSize(sessionCacheSize),
      m_sessionContext(sessionContext ? sessionContext : "")
{
    memset(m_ticketKeys, 0, sizeof(m_ticketKeys));

    // ★ WebSocket 组件在构造时创建(而非 Init):业务回调须在 Init() 之前注册,
    //    GetWebSocketServer() 构造后即有效;m_wsServer->Init()(心跳定时器)仍由服务器 Init 托管
    m_wsServer = new ZmWebSocketServer(this);

    if (certFile && certFile[0] && keyFile && keyFile[0])
    {
        m_ssl_ctx = ZmSSLContext::MakeServerCTX(certFile, keyFile,
                                                 sessionCacheSize, sessionContext);
        if (m_ssl_ctx)
            OnConfigureSSL(m_ssl_ctx);  // ★ 扩展点：子类可覆写以实现 mTLS 等
    }
}

ZmHttpServer::~ZmHttpServer()
{
    Close();
    // 防御:Close() 未调用(如 Init 失败路径)时,成员仍在 —— 析构兜底释放
    if (m_wsServer)
    {
        delete m_wsServer;
        m_wsServer = nullptr;
    }
}

void ZmHttpServer::SetWebSocketCallbacks(const ZmWebSocketCallbacks& cbs)
{
    if (m_wsServer)
        m_wsServer->SetCallbacks(cbs);
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

    // 创建业务 ZmReqLoopPool(EnableLoopPool 启用后;JRPC/RESTful 派生类构造时默认启用)
    // 预创建/上限/预算默认值(hw / hw*4 / 5000ms)
    if (m_loopPoolEnabled)
    {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        m_reqLoopPool = new ZmReqLoopPool();
        // loop 工厂(SetLoopPoolFactory):JRPC 需产出 ZmReqLoopJrpc 承载信封
        if (m_loopPoolFactory)
            m_reqLoopPool->SetLoopFactory(m_loopPoolFactory);
        if (!m_reqLoopPool->Init(
                m_loopPoolPrealloc > 0 ? m_loopPoolPrealloc : (int)hw,
                m_loopPoolMax > 0 ? m_loopPoolMax : (int)hw * 40,
                m_loopPoolBudgetMs > 0 ? m_loopPoolBudgetMs : 5000))
        {
            DEFAULT_LOG_ERROR("业务 ZmReqLoopPool初始化失败");
            delete m_reqLoopPool;
            m_reqLoopPool = nullptr;
            return false;
        }
    }

    // 创建 doer 对象池（预创建 = CPU 核数，与线程池规模匹配，免去首批请求的分配延迟）
    if (!m_httpdDoerPool)
        m_httpdDoerPool = new ZmHttpdDoerPool(this, std::thread::hardware_concurrency());

    // 初始化 WebSocket 组件(对象已在构造函数创建;此处仅建心跳定时器等)
    if (m_wsServer)
        m_wsServer->Init();

    // WS 回包投递事件(event_new/event_add 线程安全;回调在事件循环线程执行,ctx=this)
    m_wsReplyEvent = event_new(m_evbase, -1, 0, ZmHttpServer::OnEventControl, this);
    event_add(m_wsReplyEvent, nullptr);

    return true;
}

void ZmHttpServer::EnableLoopPool(int prealloc, int maxLoops, uint32_t budgetMs)
{
    m_loopPoolPrealloc = prealloc;
    m_loopPoolMax = maxLoops;
    m_loopPoolBudgetMs = budgetMs;
    m_loopPoolEnabled = true;
}

void ZmHttpServer::SetLoopPoolFactory(std::function<ZmReqLoop*()> factory)
{
    m_loopPoolFactory = std::move(factory);
}

ZmReqLoop* ZmHttpServer::AcquireLoop(ZmHttpdTask* task)
{
    if (!m_reqLoopPool)
        return nullptr;
    // 排队上限 = 剩余预算;客户端已断则提前放弃
    int64_t remainMs = task->ArriveMs() + (int64_t)m_reqLoopPool->BudgetMs()
                     - (int64_t)::GetTickCount64();
    if (remainMs <= 0) remainMs = 1;
    return m_reqLoopPool->Acquire((int)remainMs, &task->ConnClosedFlag());
}

void ZmHttpServer::DispatchLoop(ZmHttpdTask* task, ZmReqLoop* loop,
    std::function<void(ZmReqLoop*)> onStart)
{
    task->BindLoop(loop);
    auto* ctx = new ZmReqLoop::StartCtx();
    ctx->task = task;
    ctx->deadlineMs = task->ArriveMs() + (int64_t)m_reqLoopPool->BudgetMs();
    ctx->handlers.onStart = std::move(onStart);
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_START, ctx,
        [](void* p) { delete static_cast<ZmReqLoop::StartCtx*>(p); });
}

void ZmHttpServer::DrainWorkers()
{
    if (m_threadPool)
    {
        delete m_threadPool;
        m_threadPool = nullptr;
    }
}

void ZmHttpServer::Close()
{
    m_closing.store(true);   // ★ 通知器 map 进入"仅 ClearAll 可碰"状态,防主线程与循环线程并发

    // ★ 先停线程池（join 所有 worker，确保不再有新 doer 进入处理流程）
    if (m_threadPool)
    {
        delete m_threadPool;
        m_threadPool = nullptr;
    }

    // ★ 停业务 ZmReqLoopPool(join 全部 ZmReqLoop 线程;在飞业务完成,其回复仍可投到存活的循环/doer池/evhttp)
    if (m_reqLoopPool)
    {
        m_reqLoopPool->Shutdown();
        delete m_reqLoopPool;
        m_reqLoopPool = nullptr;
    }

    // ★ 关闭 WebSocket 组件:置 closing 标志 + 断开全部会话 + 清活跃表
    //    (会话包装对象与回包事件的释放放在 evhttp_free 之后,见下)
    if (m_wsServer)
        m_wsServer->Close();

    // ★ 销毁 doer 对象池（线程池已停，worker 不再引用 doer，安全释放）
    if (m_httpdDoerPool)
    {
        delete m_httpdDoerPool;
        m_httpdDoerPool = nullptr;
    }

    // ★ 先释放 HTTP→HTTPS 重定向服务器（如有）
    if (m_redirectEvhttp)
    {
        evhttp_free(m_redirectEvhttp);
        m_redirectEvhttp = nullptr;
    }

    // 释放 evhttp（停止接受新连接；同时物理释放 evws 连接,其 closecb 命中
    // ZmWebSocketServer 的 closing 分支,仅置位原子标志,不触碰会话对象）
    if (m_evhttpd)
    {
        evhttp_free(m_evhttpd);
        m_evhttpd = nullptr;
    }

    // ★ 销毁 WebSocket 组件(顺序约束):
    //   m_wsReplyEvent 先释放 → 此后 PostWsReply 直接丢弃,不再产生引用会话的闭包;
    //   再排空残留闭包(可能引用会话)→ 析构释放会话包装对象(僵尸表),无 use-after-free
    if (m_wsReplyEvent)
    {
        event_free(m_wsReplyEvent);
        m_wsReplyEvent = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_wsReplyMutex);
        m_wsReplyQueue.clear();
    }
    if (m_wsServer)
    {
        delete m_wsServer;
        m_wsServer = nullptr;
    }

    // m_evbase 由外部管理生命周期，不在此释放

    // 释放证书热加载残留的旧 ctx（如果有）
    if (m_ctxCleanupTimer)
    {
        event_free(m_ctxCleanupTimer);
        m_ctxCleanupTimer = nullptr;
    }
    if (m_oldCtx)
    {
        SSL_CTX_free((SSL_CTX*)m_oldCtx);
        m_oldCtx = nullptr;
    }

    // 释放 SSL 上下文（如果有）
    if (m_ssl_ctx)
    {
        SSL_CTX_free((SSL_CTX*)m_ssl_ctx);
        m_ssl_ctx = nullptr;
    }

    NotifierClearAll();     // 清理残留通知器(fired 通知器按设计留在 map 中等此清理;
                            // evhttp_connection_free 会无条件触发 closecb,不能依赖它自清理)
}

bool ZmHttpServer::IsOpen() const
{
    return m_evhttpd != nullptr;
}

// ============================================================================
// 证书热加载
// ============================================================================

bool ZmHttpServer::ReloadCertificate(const char* certFile, const char* keyFile)
{
    if (!certFile || !keyFile || !m_evbase)
        return false;

    // ① 创建新 SSL_CTX（沿用当前 session cache 配置 + ticket 密钥）
    SSL_CTX* newCtx = ZmSSLContext::MakeServerCTX(certFile, keyFile,
                                                   m_sessionCacheSize,
                                                   m_sessionContext.empty()
                                                       ? nullptr
                                                       : m_sessionContext.c_str());
    if (!newCtx)
    {
        PUBLIC_LOG_WARN("ReloadCertificate: new cert/key load failed, keeping existing cert (port {})", m_local_port);
        return false;
    }
    if (m_hasTicketKeys)
    {
        // 补设 ticket 密钥:新 ctx 默认使用 OpenSSL 内部随机密钥,
        // 不补设会与其余服务器密钥不一致,导致共享的 ticket 无法恢复
        SSL_CTX_set_tlsext_ticket_keys(newCtx, m_ticketKeys, ZM_TICKET_KEYS_LEN);
    }

    // ② 原子替换（事件循环线程操作，无竞态）
    //     新连接 → OnSSLBuffereventCB 读 m_ssl_ctx → 拿到 newCtx
    //     旧连接 → 已有独立的 SSL*，不受影响
    SSL_CTX* old = (SSL_CTX*)m_ssl_ctx;
    m_ssl_ctx = newCtx;

    // ③ 清理上一次热加载残留的旧 ctx 和定时器
    if (m_ctxCleanupTimer)
    {
        event_free(m_ctxCleanupTimer);
        m_ctxCleanupTimer = nullptr;
    }
    if (m_oldCtx)
    {
        SSL_CTX_free((SSL_CTX*)m_oldCtx);
        m_oldCtx = nullptr;
    }

    // ④ 保留当前的旧 ctx，延时 5 分钟后释放（等现有 TLS 连接完成或超时）
    if (old)
    {
        m_oldCtx = old;
        m_ctxCleanupTimer = event_new(m_evbase, -1, 0,
            [](evutil_socket_t, short, void* arg) {
                SSL_CTX_free((SSL_CTX*)arg);
            }, old);
        if (m_ctxCleanupTimer)
        {
            struct timeval tv = {300, 0};  // 5 minutes
            event_add(m_ctxCleanupTimer, &tv);
        }
    }

    PUBLIC_LOG_INFO("Certificate reloaded successfully (port {})", m_local_port);
    return true;
}

// ============================================================================
// TLS session ticket 密钥(方案 B:OpenSSL 内部密钥)
// ============================================================================

namespace
{
// 投递参数打包:服务器指针 + 密钥拷贝(event_base_once 回调用)
struct ZmPostTicketKeyArg
{
    ZmHttpServer*    server;
    unsigned char    keys[ZM_TICKET_KEYS_LEN];
};

void OnPostSetTicketKeys(evutil_socket_t, short, void* arg)
{
    ZmPostTicketKeyArg* p = (ZmPostTicketKeyArg*)arg;
    p->server->SetTicketKeys(p->keys, sizeof(p->keys));  // 事件循环线程内执行
    delete p;
}
}

void ZmHttpServer::SetTicketKeys(const unsigned char* keys, size_t len)
{
    if (!keys || len != ZM_TICKET_KEYS_LEN)
        return;

    memcpy(m_ticketKeys, keys, ZM_TICKET_KEYS_LEN);
    m_hasTicketKeys = true;

    if (m_ssl_ctx)
    {
        // 事件循环线程内调用:启动路径无并发;轮换路径经 PostSetTicketKeys 投递
        // 宏展开为 SSL_CTX_ctrl(ctx, ..., void*),C++ 下需显式去 const(OpenSSL 只拷贝不修改)
        SSL_CTX_set_tlsext_ticket_keys((SSL_CTX*)m_ssl_ctx, (void*)keys, (long)len);
    }
}

void ZmHttpServer::PostSetTicketKeys(const unsigned char* keys, size_t len)
{
    if (!keys || len != ZM_TICKET_KEYS_LEN || !m_evbase)
        return;

    ZmPostTicketKeyArg* p = new ZmPostTicketKeyArg;
    p->server = this;
    memcpy(p->keys, keys, ZM_TICKET_KEYS_LEN);

    // event_base_once 线程安全,回调仅在事件循环线程执行。
    // 注意:本方法不保证回调在服务器析构前执行 —— 调用方必须在销毁
    // ZmHttpServer 之前先停止并释放事件循环(残留 once 事件在
    // event_base_free 时被丢弃,不会在析构后执行);否则存在
    // 回调访问已释放对象的窗口(轮换 12h 一次,实际概率极低)。
    if (event_base_once(m_evbase, -1, EV_TIMEOUT, OnPostSetTicketKeys, p, NULL) == -1)
        delete p;   // 投递失败,释放已分配的参数,避免泄漏
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
    if (doer->m_recycled)   // 已回收(REPLY|STREAM_END 合并等双驱动路径),防双入池
        return;
    doer->m_recycled = true;
    NotifierRemove(doer);   // 摘除通知器,防 close 广播到已回收 doer(UAF)
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

// ============================================================================
// close 通知器(方案 3):per-connection 广播,替换单槽 closecb(流式路径)
// 事件循环线程独享(m_closeNotifiers 无锁):Add/Remove 均在循环线程执行(Close 期间由主线程 ClearAll 独占清理,Add/Remove 以 m_closing 跳过);
// closecb 在循环线程触发,Close 的 evhttp_free 中由主线程触发(只读不删)
// ============================================================================

void ZmHttpServer::OnConnCloseNotifierCB(struct evhttp_connection* /*conn*/, void* arg)
{
    auto* notifier = static_cast<ZmHttpServer::ZmConnCloseNotifier*>(arg);
    if (!notifier || notifier->fired)
        return;

    if (notifier->closing && notifier->closing->load())
        return;   // 服务器 Close 中:ZmReqLoopPool已销毁,不再投 CLOSE(doer 正在整体拆除)

    notifier->fired = true;

    for (ZmHttpdTask* task : notifier->members)
    {
        task->MarkConnClosed();
        // 已绑定 A → 投 CLOSE 到 A 的 loop;★ ctx 必须为 task(ProcessClose 身份校验契约)
        // 未绑定(或已解绑):业务入口查 m_connClosed 快路径兜底,投递会被身份校验丢弃
        if (ZmReqLoop* loop = task->BoundLoop())
            loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_CLOSE, task);
    }
    // 注:notifier 的删除由 NotifierRemove / NotifierClearAll 完成,closecb 只置 fired
}

void ZmHttpServer::NotifierAdd(ZmHttpdTask* task)
{
    if (m_closing.load())
        return;   // Close 期间不再登记

    struct evhttp_connection* conn = evhttp_request_get_connection(task->Request());
    if (!conn)
        return;   // 连接已死(客户端断开与请求处理竞争):由 m_connClosed 兜底

    auto it = m_closeNotifiers.find(conn);
    if (it != m_closeNotifiers.end() && it->second->fired)
    {
        // 旧通知器已触发且连接地址被新连接复用:换新通知器并重新注册 closecb,
        // 否则新连接断开时零广播(旧成员后续 Remove 对新条目是良性 no-op)
        delete it->second;
        m_closeNotifiers.erase(it);
        it = m_closeNotifiers.end();
    }

    ZmConnCloseNotifier* n;
    if (it == m_closeNotifiers.end())
    {
        n = new ZmConnCloseNotifier();
        n->closing = &m_closing;   // closecb 门:Close 期间跳过 CLOSE 投递(ZmReqLoopPool已先停)
        m_closeNotifiers[conn] = n;
        evhttp_connection_set_closecb(conn, OnConnCloseNotifierCB, n);
    }
    else
    {
        n = it->second;
    }
    n->members.push_back(task);
}

void ZmHttpServer::NotifierRemove(ZmHttpdTask* task)
{
    if (m_closing.load())
        return;   // Close 期间不触碰 map(NotifierClearAll 统一清理)

    struct evhttp_connection* conn = evhttp_request_get_connection(task->Request());
    if (!conn)
        return;   // 连接已释放:doer 残留于 fired 通知器 members 属良性(closecb 不会重触发,由 ClearAll 统一清理)
    auto it = m_closeNotifiers.find(conn);
    if (it == m_closeNotifiers.end())
        return;
    ZmConnCloseNotifier* n = it->second;
    auto& members = n->members;
    members.erase(std::remove(members.begin(), members.end(), task), members.end());
    if (members.empty() || n->fired)
    {
        if (!n->fired)
            evhttp_connection_set_closecb(conn, nullptr, nullptr);
        m_closeNotifiers.erase(it);
        delete n;
    }
}

void ZmHttpServer::NotifierClearAll()
{
    for (auto& kv : m_closeNotifiers)
        delete kv.second;
    m_closeNotifiers.clear();
}

void ZmHttpServer::OnHttpRequestCB(struct evhttp_request* request, void* arg)
{
    ZmHttpServer* server = (ZmHttpServer*)arg;
    // ★ WebSocket 升级分流(必须在 AcquireDoer 之前,事件循环线程):
    // TryUpgrade 内部:Upgrade 头识别 → 回调注册检查 → 路径/鉴权 → evws_new_session;
    // 成功接管返回 true(request 已释放,不得再进入 doer 流程)
    if (server->m_wsServer && server->m_wsServer->TryUpgrade(request))
        return;

    const char* uri = evhttp_request_get_uri(request);
    if (uri && arg)
    {
        ZmHttpServer* server = (ZmHttpServer*)arg;
        if (!server->m_threadPool)
        {
            // 关闭进行中(worker 已排空):直接拒绝,防空指针解引用
            // (覆盖 DrainWorkers→evhttp_free 整个关闭窗口的新请求)
            evhttp_send_error(request, ZM_HTTP_STATUS_CODE_SERVICE_UNAVAILABLE, nullptr);
            return;
        }
        ZmHttpdDoer* doer = server->AcquireDoer(request);
        doer->SetArriveTime((int64_t)::GetTickCount64());   // 请求到达时间戳(deadline 起点)
        server->NotifierAdd(doer);                          // 登记 close 通知器(循环线程,无竞态)
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
        if (ZmHttpServer::ZM_HTTPD_CONTROL_WS_REPLY & what)
        {
            // WS 回包闭包(服务器级事件,ctx = ZmHttpServer,区别于 doer 级控制信号)
            ZmHttpServer* server = (ZmHttpServer*)ctx;
            server->RunWsReplies();
        }
    }
}

void ZmHttpServer::PostWsReply(std::function<void()> fn)
{
    if (!m_wsReplyEvent || !fn)
        return;
    {
        std::lock_guard<std::mutex> lock(m_wsReplyMutex);
        m_wsReplyQueue.push_back(std::move(fn));
    }
    event_active(m_wsReplyEvent, ZmHttpServer::ZM_HTTPD_CONTROL_WS_REPLY, 0);
}

void ZmHttpServer::RunWsReplies()
{
    std::deque<std::function<void()>> queue;
    {
        std::lock_guard<std::mutex> lock(m_wsReplyMutex);
        queue.swap(m_wsReplyQueue);
    }
    for (auto& fn : queue)
    {
        if (fn)
            fn();
    }
}

// ============================================================================
// HTTPS SSL bufferevent 工厂（由 evhttp_set_bevcb 注册，每个新连接调用一次）
// ============================================================================

struct bufferevent* ZmHttpServer::OnSSLBuffereventCB(struct event_base* base, void* arg)
{
    SSL_CTX* ctx = (SSL_CTX*)arg;
    SSL* ssl = SSL_new(ctx);
    if (!ssl)
        return nullptr;

    // fd 传 -1：evhttp 随后通过 bufferevent_setfd() 绑定实际的 socket fd
    // BUFFEREVENT_SSL_ACCEPTING：服务端模式，自动调用 SSL_accept()
    // BEV_OPT_CLOSE_ON_FREE：bev 释放时自动关闭底层 socket
    struct bufferevent* bev = bufferevent_openssl_socket_new(
        base, -1, ssl,
        BUFFEREVENT_SSL_ACCEPTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);

    return bev;
}

// ============================================================================
// HTTP→HTTPS 301 重定向回调（轻量级）
//
// 【设计意图】通用 HTTP 端口（80）仅做全量 301 → HTTPS，语义极简，无需经
// ZmHttpdDoer / ZmHttpdTask / Router / 中间件 等完整管线。直接在事件循环线程调用
// evhttp_send_reply，零线程切换、零内存分配(除临时 evbuffer)。
//
// 【适用场景】
//   - 浏览器用户习惯性输入 http://ip/ → 自动跳 https://ip/
//   - 搜索引擎索引了旧 HTTP 链接 → 301 通知其更新
//
// 【不适合的场景与权衡】
//   - 不按路径做差异化重定向（未走 Router，无路由匹配）
//   - 不经过中间件（日志/限流等通用逻辑需另外实现）
//   - 固定 301（POST→GET），如需保留 HTTP 方法用 307/308 需改造
//   - API 端点（JRPC/RESTful）不应启用此重定向（POST 丢 body、客户端应直接配 https://）
//
// 后续如需增强：可行方案是将 redirect_from_port 上的请求也纳入 ZmHttpdDoer 管线，
// 由 Router 根据路径决定 301/307/或不重定向。
// ============================================================================

void ZmHttpServer::OnRedirectRequestCB(struct evhttp_request* req, void* arg)
{
    if (!req)
        return;

    ZmHttpServer* server = (ZmHttpServer*)arg;
    uint16_t httpsPort = server ? server->m_local_port : 443;

    // 安全获取 URI（null 时回退为 "/"）
    const char* uri = evhttp_request_get_uri(req);
    if (!uri)
        uri = "/";

    // 从请求头获取 Host（用于构造重定向 URL），安全处理 null
    const char* host = nullptr;
    struct evkeyvalq* inHeaders = evhttp_request_get_input_headers(req);
    if (inHeaders)
        host = evhttp_find_header(inHeaders, "Host");

    // 构造目标 URL
    char redirectUrl[2048];
    if (host)
    {
        // 去掉 Host 头中可能带有的端口号（如 Host: example.com:80）
        std::string hostOnly(host);
        size_t colonPos = hostOnly.find(':');
        if (colonPos != std::string::npos)
            hostOnly = hostOnly.substr(0, colonPos);

        snprintf(redirectUrl, sizeof(redirectUrl), "https://%s:%d%s",
            hostOnly.c_str(), httpsPort, uri);
    }
    else
    {
        snprintf(redirectUrl, sizeof(redirectUrl), "https://localhost:%d%s",
            httpsPort, uri);
    }

    // 发送 301 响应
    struct evbuffer* body = evbuffer_new();
    if (body)
    {
        evbuffer_add_printf(body,
            "<html><body>Redirecting to <a href=\"%s\">%s</a></body></html>",
            redirectUrl, redirectUrl);

        struct evkeyvalq* outHeaders = evhttp_request_get_output_headers(req);
        if (outHeaders)
        {
            evhttp_add_header(outHeaders, "Location", redirectUrl);
            evhttp_add_header(outHeaders, "Content-Type", "text/html; charset=utf-8");
        }
        evhttp_send_reply(req, 301, "Moved Permanently", body);
        evbuffer_free(body);
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

    // ★ HTTPS: 如果提供了 SSL_CTX，注入 SSL bufferevent 工厂
    // evhttp 在每次 accept 新连接时调用此工厂创建 bufferevent，
    // 返回的 bufferevent_openssl 自动完成 SSL 握手和加解密，对上层透明
    if (m_ssl_ctx)
    {
        evhttp_set_bevcb(m_evhttpd, ZmHttpServer::OnSSLBuffereventCB,
                         m_ssl_ctx);
        PUBLIC_LOG_INFO("HTTPS mode enabled on port {}", m_local_port);
    }

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

        // ★ HTTPS 模式 + 指定 redirect_from_port：创建轻量 HTTP→HTTPS 301 重定向服务器
        if (m_ssl_ctx && m_redirect_from_port > 0)
        {
            m_redirectEvhttp = evhttp_new(evbase);
            if (m_redirectEvhttp)
            {
                evhttp_set_gencb(m_redirectEvhttp, ZmHttpServer::OnRedirectRequestCB, this);
                if (evhttp_bind_socket_with_handle(m_redirectEvhttp, "0.0.0.0", m_redirect_from_port))
                {
                    PUBLIC_LOG_INFO("HTTP→HTTPS redirect on port {} → {}", m_redirect_from_port, m_local_port);
                }
                else
                {
                    PUBLIC_LOG_WARN("HTTP→HTTPS redirect bind port {} failed", m_redirect_from_port);
                    evhttp_free(m_redirectEvhttp);
                    m_redirectEvhttp = nullptr;
                }
            }
        }
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

ZmJsonRpcServer::ZmJsonRpcServer(struct event_base* evbase, std::string_view root_uri, uint16_t local_port,
                               const char* certFile, const char* keyFile,
                               uint32_t sessionCacheSize, const char* sessionContext)
    : ZmHttpServer(evbase, local_port, certFile, keyFile, 0,
                   sessionCacheSize, sessionContext)
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

    // 自治业务 ZmReqLoopPool(默认参数:预创建=硬件并发,上限=x4,预算=5000ms)
    EnableLoopPool();
    // ★ loop 工厂必须产出 ZmReqLoopJrpc(承载响应信封;OnHttpdRequest 内 static_cast)
    SetLoopPoolFactory([]() { return new ZmReqLoopJrpc(); });
}

ZmJsonRpcServer::~ZmJsonRpcServer()
{}

ZMJSON ZmJsonRpcServer::MakeError(int code, std::string_view message)
{
    return ZMJSON{ {"code", code}, {"message", message} };
}

void ZmJsonRpcServer::SetRequestReadCB(OnRequestReadCB oncall_async)
{
    m_on_request_read = oncall_async;
}

void ZmJsonRpcServer::BuildJsonRpcResponse(ZmHttpdTask* task, ZMJSON& reply, const ZMJSON& response)
{
    ZMJSON error   = response.value("error",   ZMJSON());
    ZMJSON result  = response.value("result",  ZMJSON());

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

    task->PutReplyHeader("Content-type", content_type.c_str());
    task->SetReply(ZM_HTTP_STATUS_CODE_OK);
    task->SetReplyData((const BYTE*)body.data(), body.size());
}

int ZmJsonRpcServer::OnHttpdRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
    // 暂定JRPC请求的uri一律使用完全匹配
    // 用 Path() 而非 Uri():Path() 剥离 query string,支持 GET JSONP(?callback=&jsonbody=)
    if (ZmString::IsEmpty(m_root_uri) || !ZmString::Equals(task->Path(), m_root_uri, true))
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
                    request.clear();
                    request["method"] = method;
                    request["params"] = params;

                    // 仅异步业务回调：未设置则报 PORTAL_NOJRPC
                    if (m_on_request_read)
                    {
                        // ★ 内部 ZmReqLoopPool 分发:Acquire(预算内排队/断连放弃)→ START → 业务回调
                        ZmReqLoop* loop = AcquireLoop(task);
                        if (!loop)
                        {
                            // 池满/排队超时/断连:回 DROPPED 错误信封(带 id,客户端可匹配请求;
                            // 显式传信封:此时无 loop,信封无法存 loop 内部)
                            ZMJSON err = { {"error",
                                MakeError(ZM_JRPC_ERR_DROPPED, "No worker available")} };
                            BuildJsonRpcResponse(task, err, reply);
                            // 投递 REPLY 信号到 HTTP event loop 实际发送(任意线程可调)
                            task->TriggerReply();
                            return -1;
                        }

                        // ★ 响应信封(id/jsonrpc/method)存入 ZmReqLoopJrpc:
                        //   业务层 Response 时取出组装(与 ZmReqLoopRest per-request 状态同模式)
                        static_cast<ZmReqLoopJrpc*>(loop)->SetEnvelope(reply);

                        // 请求 JSON 拷贝进闭包(request 是服务器栈上对象,必须拷贝)
                        std::string reqJson = request.dump();
                        DispatchLoop(task, loop,
                            [this, reqJson = std::move(reqJson)](ZmReqLoop* l) {
                                m_on_request_read(l, reqJson.c_str());
                            });

                        return -1; // 异步处理中，响应稍后到达
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

ZmRESTfulServer::ZmRESTfulServer(struct event_base* evbase, std::string_view root_uri, uint16_t local_port,
                               const char* certFile, const char* keyFile,
                               uint32_t sessionCacheSize, const char* sessionContext)
    : ZmHttpServer(evbase, local_port, certFile, keyFile, 0,
                   sessionCacheSize, sessionContext)
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

    // 自治业务 ZmReqLoopPool(默认参数:预创建=硬件并发,上限=x4,预算=5000ms)
    EnableLoopPool();
}

ZmRESTfulServer::~ZmRESTfulServer()
{}

void ZmRESTfulServer::SetRequestReadCB(OnRequestReadCB oncall)
{
    m_on_request_read = oncall;
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

    // ② 内部 ZmReqLoopPool 分发 — 业务回调在 ZmReqLoop 线程执行
    if (m_on_request_read)
    {
        // 池满/排队超时/断连:回 503(TriggerReply 驱动 doer 回收)
        ZmReqLoop* loop = AcquireLoop(task);
        if (!loop)
        {
            task->SetReply(ZM_HTTP_STATUS_CODE_SERVICE_UNAVAILABLE, "Service Unavailable");
            task->TriggerReply();
            return -1;
        }
        // body 零拷贝:data 指向请求 evbuffer,回复发送前有效(与旧 manager 侧契约一致)
        DispatchLoop(task, loop,
            [this, data, dlen](ZmReqLoop* l) {
                m_on_request_read(l, data, dlen);
            });
        return -1;
    }
    return 0;  // 没设置任何回调
}