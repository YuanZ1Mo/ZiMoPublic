#include "zm_net_http_client.h"

#include <../openssl/include/openssl/evp.h>   // Basic 认证(C9):EVP_EncodeBlock(base64)
#include <zlib.h>   // C13:gzip/deflate 自动解压(inflate 系;zlib 1.3.1 已在 C1 接入:include 路径 + zlibstatic.lib)

ZmHttpClientRequest::ZmHttpClientRequest()
{
}

ZmHttpClientRequest& ZmHttpClientRequest::SetMethod(const char* method)
{
    if (method) m_method = method;
    return *this;
}

ZmHttpClientRequest& ZmHttpClientRequest::SetUrl(const char* url)
{
    if (url) m_url = url;
    return *this;
}

ZmHttpClientRequest& ZmHttpClientRequest::SetHeader(const char* name, const char* value)
{
    if (name && value) m_headers[name] = value;
    return *this;
}

void ZmHttpClientRequest::RemoveHeader(const char* name)
{
    if (name) m_headers.erase(name);
}

ZmHttpClientRequest& ZmHttpClientRequest::SetBody(const void* data, size_t len)
{
    if (!data || len == 0)   // 空指针/零长:清空三形态(等价空 body)
    {
        m_body.clear();
        m_bodyFile.clear();
        m_uploadChunk = {};
        return *this;
    }
    m_body.assign((const BYTE*)data, (const BYTE*)data + len);
    m_bodyFile.clear();
    m_uploadChunk = {};
    return *this;
}

ZmHttpClientRequest& ZmHttpClientRequest::SetBodyJson(const ZMJSON& json)
{
    std::string s = json.dump();
    SetBody(s.data(), s.size());
    m_headers["Content-Type"] = "application/json; charset=utf-8";
    return *this;
}

ZmHttpClientRequest& ZmHttpClientRequest::SetBodyFile(const char* path)
{
    m_body.clear();
    m_uploadChunk = {};
    m_bodyFile = path ? path : "";
    return *this;
}

ZmHttpClientRequest& ZmHttpClientRequest::SetUploadStream(std::function<std::string()> cb)
{
    m_body.clear();
    m_bodyFile.clear();
    m_uploadChunk = std::move(cb);
    return *this;
}

ZmHttpClientRequest& ZmHttpClientRequest::SetConnectTimeout(int s) { m_connectTimeout = s; return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetTotalTimeout(int s)   { m_totalTimeout = s;   return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetFollowRedirect(bool on, int max) { m_followRedirect = on; m_redirectMax = max; return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetProxy(const char* host, uint16_t port) { m_proxyHost = host ? host : ""; m_proxyPort = port; return *this; }

ZmHttpClientRequest& ZmHttpClientRequest::SetBasicAuth(const char* user, const char* pass)
{
    // Basic 认证(C9):user:pass → base64 → "Authorization: Basic <b64>"(null 参数按空串)
    std::string raw = std::string(user ? user : "") + ":" + (pass ? pass : "");
    size_t outLen = ((raw.size() + 2) / 3) * 4;
    std::vector<char> out(outLen + 1);
    int n = EVP_EncodeBlock((unsigned char*)out.data(), (const unsigned char*)raw.data(), (int)raw.size());
    out[n] = 0;
    m_headers["Authorization"] = std::string("Basic ") + out.data();
    return *this;
}

ZmHttpClientRequest& ZmHttpClientRequest::SetBearerToken(const char* token)
{
    if (token) m_headers["Authorization"] = std::string("Bearer ") + token;
    return *this;
}

ZmHttpClientRequest& ZmHttpClientRequest::SetUseCookieJar(bool on) { m_useCookieJar = on; return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetGzip(bool on)         { m_gzip = on; return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetRetry(int count, int baseDelayMs) { m_retryCount = count; m_retryBaseDelayMs = baseDelayMs; return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetOnDataChunk(std::function<void(const BYTE*, size_t)> cb) { m_onDataChunk = std::move(cb); return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetOnSseEvent(std::function<void(const std::string&)> cb) { m_onSseEvent = std::move(cb); return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetProgressCallback(std::function<void(int64_t, int64_t)> cb) { m_progress = std::move(cb); return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetOutputFile(const char* path) { m_outputFile = path ? path : ""; return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetRange(int64_t offset) { m_range = offset; return *this; }
ZmHttpClientRequest& ZmHttpClientRequest::SetClientCert(const char* certFile, const char* keyFile)
{
    m_clientCertFile = certFile ? certFile : "";
    m_clientKeyFile = keyFile ? keyFile : "";
    return *this;
}

const std::string& ZmHttpClientResponse::Header(const char* name) const
{
    static const std::string s_empty;
    if (!name)
        return s_empty;
    for (const auto& kv : m_headers)
        if (_stricmp(kv.first.c_str(), name) == 0)
            return kv.second;
    return s_empty;
}

#include "zm_net_http_client_loop.h"
#include "zm_logger.h"

#include "../util/zm_util_str.h"     // C18:zm_strsep(SetDnsServers 手动 DNS 列表解析)

#include <../libevent/include/event2/dns.h>   // C18:evdns_base_clear_nameservers_and_suspend/nameserver_ip_add/resume
#include <../libevent/include/event2/event.h>
#include <../libevent/include/event2/buffer.h>
#include <../libevent/include/event2/bufferevent.h>
#include <../libevent/include/event2/bufferevent_ssl.h>  // TLS(C8)
#include <../libevent/include/event2/http.h>
#include <../libevent/include/event2/keyvalq_struct.h>   // evkeyvalq 完整定义(头遍历)
#include <../libevent/include/event2/util.h>

#include "../ssl/zm_ssl_ctx.h"          // TLS(C8):ZmSSLContext::MakeClientCTX
#include <../openssl/include/openssl/ssl.h>   // TLS(C8):SSL_new/SSL_CTX_*

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <future>
#include <map>
#include <stdio.h>     // C14:sscanf(Expires 属性解析)
#include <string.h>
#include <time.h>      // C14:time/_mkgmtime(cookie 过期与 Expires 属性解析)
#include <vector>

// ============================================================================
// TLS(C8):HTTPS 支持(复用 ZmSSLContext)
//
// 实现说明:本仓库 libevent 2.2.1 无 evhttp_connection_set_bevcb(bevcb 仅服务端
// evhttp_set_bevcb / evhttp_bound_set_bevcb 存在,evhttp_connection 结构无 bevcb
// 字段;已核对 http.c / http-internal.h / http.h)。HTTPS 客户端按 libevent 官方
// 模式(regress_http.c 的 https 客户端测试)实现:
//   bufferevent_openssl_socket_new(base, -1, ssl, CONNECTING, ...) 创建 SSL bev
//   (bev 持有 SSL 对象,bev 释放时 SSL_free)→ evhttp_connection_base_bufferevent_new
//   挂到 evhttp 连接(连接持有 bev,evhttp_connection_free 时 bufferevent_free)。
// 超时顺序(C7 前瞻项,已核对):bev 在连接创建时即存在,evhttp_connection_set_timeout /
// set_connect_timeout_tv 直接作用于 con->bufev(= 我们的 SSL bev),连接建立时
// (evhttp_connection_cb)再按连接超时/读写超时重设 → 无需在 bevcb 内重设(亦无 bevcb)。
// SSL_CTX 生命周期:OpenSSL SSL 对象持有 SSL_CTX 引用,请求级 SSL_CTX 在请求收口时
// SSL_CTX_free 安全(仅减引用;连接即使回池,ctx 存活至该连接的 SSL 释放)。
// 连接池交互:请求级证书覆盖的连接绕过池获取(不复用旧身份)、收口时不回池;
// 客户端级配置变更(dirty 重建)时同步清空 https 空闲池(旧身份连接全部作废)。
// ============================================================================

// 构建客户端 SSL_CTX(复用 ZmSSLContext):
//  verifyPeer=true : SSL_VERIFY_PEER + 默认 CA(或 caFile 自定义 CA)
//  verifyPeer=false: SSL_VERIFY_NONE(测试用)
//  certFile/keyFile 非空:MakeClientCTX(certFile, keyFile)(mTLS)
static SSL_CTX* BuildClientSSLCTX(bool verifyPeer, const char* caFile,
                                  const std::string& certFile, const std::string& keyFile)
{
    SSL_CTX* ctx = nullptr;
    if (!certFile.empty() && !keyFile.empty())
        ctx = ZmSSLContext::MakeClientCTX(certFile.c_str(), keyFile.c_str());
    else
        ctx = ZmSSLContext::MakeClientCTX();
    if (!ctx) return nullptr;

    if (verifyPeer)
    {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        if (caFile && *caFile)
            SSL_CTX_load_verify_locations(ctx, caFile, nullptr);
        else
            SSL_CTX_set_default_verify_paths(ctx);
    }
    else
    {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }
    return ctx;
}

// ============================================================================
// 内部实现(私有类,封装循环线程内的全部状态与逻辑)
// ============================================================================

// C13:释放解压流(inflateEnd 释放内部状态 + 释放 z_stream 对象;置空)。
// 仅需 zlib 类型,置于结构之前,析构/header_cb 重置/GzipFinish 三处复用。
static void GzipDestroyStream(void*& stream)
{
    if (!stream)
        return;
    inflateEnd(static_cast<z_stream*>(stream));
    delete static_cast<z_stream*>(stream);
    stream = nullptr;
}

// ── 单请求状态(循环线程独占;同步模式的 future 跨线程)──

struct ZmHttpClientReqState
{
    ZmHttpClientReqState(ZmHttpClientPrivate* owner, const ZmHttpClientRequest& req,
                         uint64_t id, void* params, ZmHttpClientCallback cb)
        : priv(owner), req(req), id(id), params(params), cb(std::move(cb))
    {
    }

    ~ZmHttpClientReqState()
    {
        if (outFile) { fclose(outFile); outFile = nullptr; }   // 兜底(正常路径 FinishRequest 已关)
        // 兜底(正常路径 FinishRequest 已 event_del+event_free 并置空)
        if (totalTimeoutEvent) { event_free(totalTimeoutEvent); totalTimeoutEvent = nullptr; }
        // C15 兜底:重试退避事件(正常路径 RetryCB 已释放置空 / FinishRequest 已清理)
        if (retryEvent) { event_free(retryEvent); retryEvent = nullptr; }
        // C12 兜底:上传泵回调(正常路径 FinishRequest 已移除;防收口遗漏致 bev 回调悬垂)
        // 与上传文件(正常路径 FinishRequest 已关闭)
        if (uploadCb && con)
        {
            struct bufferevent* bev = evhttp_connection_get_bufferevent(con);
            struct evbuffer* out = bev ? bufferevent_get_output(bev) : nullptr;
            if (out)
                evbuffer_remove_cb_entry(out, uploadCb);
            uploadCb = nullptr;
        }
        if (uploadFile) { fclose(uploadFile); uploadFile = nullptr; }
        // 兜底(正常路径 FinishRequest 已释放并置空;SSL 持有 ctx 引用,释放安全)
        if (perReqSslCtx) { SSL_CTX_free(perReqSslCtx); perReqSslCtx = nullptr; }
        // C16 兜底:CONNECT 隧道 bev(正常路径 FinishRequest 阶段①已释放置空;防收口
        // 遗漏路径泄漏 socket)
        if (tunnelBev) { bufferevent_free(tunnelBev); tunnelBev = nullptr; }
        // C16 兜底:隧道 SSL 对象(正常路径 FinishRequest 阶段①已 SSL_free 置空)
        if (tunnelSsl) { SSL_free(tunnelSsl); tunnelSsl = nullptr; }
        // C13 兜底:解压流(正常路径 FinishRequest 已 GzipFinish 释放并置空)
        GzipDestroyStream(gzipStream);
    }

    ZmHttpClientPrivate* priv = nullptr;    ///< 所属客户端(循环线程内使用)

    // 请求数据
    ZmHttpClientRequest  req;
    ZmHttpClientRequest  redirectReq;   ///< 重定向目标请求副本(C9;仅重定向路径使用)
    uint64_t             id;
    void*                params;
    ZmHttpClientCallback cb;

    // URL 解析结果
    std::string  scheme;       ///< http/https
    std::string  host;
    uint16_t     port = 0;
    std::string  path;         ///< 含 query

    // 状态
    int          attempts = 0;          ///< 已发起次数(整条链计数:初始+重定向+重试;重定向预算判定用 attempts-retryCount)
    int          retryCount = 0;        ///< C15:已重试次数(与 attempts 分离:重定向不消耗重试预算,重试不消耗重定向预算)
    bool         cancelled = false;
    bool         finished = false;      ///< 已收口(回调已触发/同步已置值)
    bool         timeoutByDeadline = false;   ///< 总超时回调已触发(error_cb 的 CANCEL 分支据此收口 TIMEOUT 而非 CANCELLED)

    // 总超时(deadline)事件(StartRequest 创建于循环 base;完成/取消/超时经 FinishRequest 统一清理)
    struct event* totalTimeoutEvent = nullptr;
    // C15 重试退避定时器(MaybeRetry 创建于循环 base;触发 = RetryCB 自行释放置空;收口清理仍挂起者)
    struct event* retryEvent = nullptr;

    // 同步模式
    std::promise<std::unique_ptr<ZmHttpClientResult>>* syncPromise = nullptr;

    // 响应组装
    ZmHttpClientResult result;
    FILE* outFile = nullptr;            ///< SetOutputFile 时打开
    void* gzipStream = nullptr;         ///< z_stream*(C13:Content-Encoding gzip/deflate 的解压流;GzipInit 创建,FinishRequest 收尾/析构兜底)
    bool  gzipDone = false;             ///< C13:解压流已见 Z_STREAM_END(完整结束;GzipFinish 据此跳过 Z_FINISH 冲刷)
    std::string sseBuffer;              ///< SSE 帧缓冲(C11:SseFeed 增量帧解析)

    // ── C11 真流式(增量钩子)状态:header_cb/chunk_cb 记录,循环线程独占 ──
    bool streamingBody = false;     ///< header_cb:2xx → 流式接管(体增量分发;总超时已取消)
    bool non2xxResponse = false;    ///< header_cb:非 2xx → 按普通响应收口(体累积,总超时保持)
    bool eofOk = false;             ///< error_cb:无 Content-Length 流式流 EOF = 正常流结束
    int  abortError = ZM_HTTPC_OK;  ///< chunk_cb 内部中止(写盘失败):取消时按此错误收口
    std::string abortErrorText;
    std::vector<BYTE> non2xxBody;   ///< 非 2xx 响应体累积(收口移交 m_body)
    // C13 全量模式解压暂存:本结构不是 ZmHttpClientResult 的友元,解压产出不可直写 m_body
    // —— 先入此暂存,由 done_cb 全量 gzip 路径(std::move)移交 m_body
    std::vector<BYTE> gzipFullBody;

    // ── 流式下载(C10)/SSE(C11):进度与流式状态(循环线程独占)──
    uint64_t bytesReceived = 0;         ///< 已接收字节数(进度)
    int64_t  contentLengthTotal = -1;   ///< Content-Length 响应头;无 = -1(进度 total 未知)

    // ── C12 流式上传(chunked)泵状态(循环线程独占;泵 = bev 输出缓冲回调,见
    // ZmHttpClientUploadOutCB;无 uploadPumpEvent —— 定时事件泵无法保证时序,见泵注释)──
    struct evbuffer_cb_entry* uploadCb = nullptr;  ///< bev 输出缓冲回调(泵;FinishRequest 移除)
    FILE* uploadFile = nullptr;                    ///< BodyFile 时打开(FinishRequest 关闭)
    bool uploadDone = false;                       ///< 已写终止块(0\r\n\r\n)
    bool uploadHeaderReady = false;                ///< 头部块已完整写入且自动 Content-Length 已清除
    uint64_t bytesSent = 0;                        ///< 已发送字节数(上传进度 sent)
    static constexpr size_t UPLOAD_CHUNK_SIZE = 64 * 1024;   ///< 泵单块大小(内存有界 ≤ 2 块)

    // 流式消费判定(C10/C11 收敛):OnDataChunk 逐块回调 / OnSseEvent 逐事件 / OutputFile 落盘
    // 任一启用即走真流式路径(chunk_cb 增量分发)。C7 的"流式取消总超时"判定与此合并为单一判定:
    // 流式请求的总超时在响应头到达时取消(header_cb,2xx),与 ZmReqLoop::CancelDeadline 语义对齐;
    // 全量请求总超时覆盖至收口(FinishRequest 清理),维持现状。
    bool IsStreamingConsumer() const
    {
        return req.OnDataChunk() || req.OnSseEvent() || !req.OutputFile().empty();
    }

    // 数据分发(循环线程):OnDataChunk 逐块回调;OutputFile 逐块写盘;两者可并存(先回调后写盘);
    // 随后更新进度(Progress(已收, total);total = Content-Length,未知为 -1)。
    // 返回 false = 写盘失败(OnDataChunk 回调已触发;调用方收口 ERR_FILE_IO,进度不含本块)
    bool DispatchData(const BYTE* data, size_t len)
    {
        if (req.OnDataChunk())
            req.OnDataChunk()(data, len);
        if (!WriteToFile(data, len))
            return false;
        bytesReceived += len;
        if (req.Progress())
            req.Progress()((int64_t)bytesReceived, contentLengthTotal);
        return true;
    }

    // C13 解压后字节公共分发(chunk_cb 原始分流与 GzipFeed 产出共用):
    //   全量(非流式消费)      → 追加 m_body(仅 done_cb 的 GzipFeed 可达:chunk_cb 只注册于流式请求);
    //   非 2xx 流式            → 累积 non2xxBody(与 chunk_cb 原始语义一致:不触发数据回调/不落盘);
    //   2xx 流式:SSE          → SseFeed 逐事件(SSE_MAX_FRAME 上限作用于解压后,挂载点在解压后);
    //   2xx 流式:OnDataChunk/落盘 → DispatchData(回调+写盘+进度);
    //   2xx 流式:SSE 仅消费    → 计数+进度。
    // 失败时已置 abortError/abortErrorText(SSE 帧超上限 / 写盘失败),返回 false;调用方取消流按它收口
    bool DispatchDecoded(const BYTE* data, size_t len)
    {
        if (!IsStreamingConsumer())
        {
            // 全量模式:解压产出暂存 gzipFullBody(done_cb 全量 gzip 路径移交 m_body;
            // 本分支仅 done_cb 的 GzipFeed 可达:chunk_cb 只注册于流式请求;bytesReceived 照计)
            size_t old = gzipFullBody.size();
            gzipFullBody.resize(old + len);
            memcpy(gzipFullBody.data() + old, data, len);
            bytesReceived += len;
            return true;
        }
        if (!streamingBody)
        {
            non2xxBody.insert(non2xxBody.end(), data, data + len);
            return true;
        }
        if (req.OnSseEvent() && !SseFeed(data, len))
        {
            abortError = ZM_HTTPC_ERR_PARSE;
            abortErrorText = "sse frame too large";
            return false;
        }
        if (req.OnDataChunk() || !req.OutputFile().empty())
        {
            if (!DispatchData(data, len))
            {
                abortError = ZM_HTTPC_ERR_FILE_IO;
                abortErrorText = "write output file failed";
                return false;
            }
        }
        else if (req.OnSseEvent())
        {
            // SSE 仅消费(无数据回调/落盘):计数与进度(与 chunk_cb 原分支一致)
            bytesReceived += len;
            if (req.Progress())
                req.Progress()((int64_t)bytesReceived, contentLengthTotal);
        }
        return true;
    }

    // 独立写盘步骤:返回 false = 写盘失败(短写/磁盘错误;已写部分由 FinishRequest 关闭)
    bool WriteToFile(const BYTE* data, size_t len)
    {
        if (!outFile) return true;
        return fwrite(data, 1, len, outFile) == len;
    }

    // ── SSE 帧解析(C11):追加增量字节,按空行分帧,逐帧解析 data: 行并回调 OnSseEvent。
    // SSE 规范:行以 \n 结尾(容忍 \r\n);帧 = 若干行 + 空行;仅解析 data: 前缀行,
    // 多行 data 以 \n 连接(行首单个空格剥离);comment(:)/event:/id:/retry: 行忽略;
    // 残留半帧(无结尾空行)在流结束时丢弃。
    // 帧缓冲上限:无界增长防护(半帧永不完成时防内存膨胀)。C13 gzip 挂载点在
    // chunk_cb 内、SseFeed 前 —— 解压后字节再进本函数,上限自然作用于解压后。
    // 返回 false = 帧缓冲超上限(帧损坏),调用方取消流并按 ERR_PARSE 收口。
    static constexpr size_t SSE_MAX_FRAME = 1024 * 1024;
    bool SseFeed(const BYTE* data, size_t len)
    {
        sseBuffer.append((const char*)data, len);
        // 行尾归一化:"\r\n" → "\n"(规范禁止 data 载荷含裸 CR,替换无损)
        for (size_t p = sseBuffer.find("\r\n"); p != std::string::npos; p = sseBuffer.find("\r\n", p))
        {
            sseBuffer.erase(p, 1);
            p += 1;
        }
        if (sseBuffer.size() > SSE_MAX_FRAME)
            return false;   // 帧损坏(超过上限):调用方取消流
        for (;;)
        {
            size_t end = sseBuffer.find("\n\n");
            if (end == std::string::npos)
                break;
            DispatchSseFrame(sseBuffer.substr(0, end));
            sseBuffer.erase(0, end + 2);
        }
        return true;
    }

    void DispatchSseFrame(const std::string& frame)
    {
        std::string data;   // 累积 data: 行值(多行以 \n 连接)
        size_t pos = 0;
        while (pos <= frame.size())
        {
            size_t nl = frame.find('\n', pos);
            std::string line = (nl == std::string::npos) ? frame.substr(pos) : frame.substr(pos, nl - pos);
            if (line.compare(0, 5, "data:") == 0)
            {
                std::string v = line.substr(5);
                if (!v.empty() && v[0] == ' ')
                    v.erase(0, 1);   // 行首单个空格剥离
                if (!data.empty())
                    data += '\n';
                data += v;
            }
            // 其余行(comment/event/id/retry 等)忽略
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
        if (!data.empty() && req.OnSseEvent())
            req.OnSseEvent()(data);
    }

    // evhttp 句柄(StartRequest 赋值;完成/错误/取消后 evhttp 已释放,不再使用)
    struct evhttp_request*    hreq = nullptr;
    struct evhttp_connection* con  = nullptr;

    // HTTPS:请求级 SSL_CTX(请求级证书覆盖时非空,收口时 SSL_CTX_free;
    // SSL 对象持有 ctx 引用,连接即使回池,ctx 存活至该连接 SSL 释放,释放安全)
    SSL_CTX* perReqSslCtx = nullptr;

    // 错误回调先行记录的待定错误(done_cb(NULL) 跟进收口时使用)
    int         pendingError = ZM_HTTPC_OK;
    std::string pendingErrorText;

    // ── C16 代理 HTTPS CONNECT 隧道状态(循环线程独占)──
    // tunnelBev:CONNECT 阶段的原始 socket bev(连接代理、收发 CONNECT 请求/响应)。
    // 隧道建立后作为 SSL filter bev 的底层(bufferevent_openssl_filter_new 内部 incref)。
    // SSL bev 不带 BEV_OPT_CLOSE_ON_FREE(带该标志时底层被 decref 两次:be_ssl_unlink
    // 同步一次 + bufferevent_finalize_cb_ 一次,再加阶段①的释放 = 引用计数击穿,已核对
    // bufferevent_ssl.c / bufferevent.c)—— 不带时 unlink 仅摘底层回调,底层引用由
    // finalize 释放一次(2→1),我方引用由 FinishRequest 阶段①在 con 释放之后释放
    // (1→0 → fd 恰关一次)。CONNECT 阶段失败/取消:阶段①直接释放本 bev(连带关闭 socket)。
    struct bufferevent* tunnelBev = nullptr;
    std::string tunnelBuf;   ///< CONNECT 响应头累积(读回调;解析到 "\r\n\r\n" 后移交/清空)
    // 隧道 SSL 对象:SSL bev 不带 CLOSE_ON_FREE → be_ssl_destruct 不释放它
    // (SSL_context_free 按标志门控,已核对 bufferevent_openssl.c)→ 归属我方,
    // 阶段①在 con 释放后 SSL_free;析构兜底(正常路径已释放置空)
    SSL* tunnelSsl = nullptr;
};

// ============================================================================
// C13 gzip/deflate 自动解压(zlib)
// 请求侧:SetGzip(true)(默认开)自动加 "Accept-Encoding: gzip, deflate"(用户显式设置时尊重,
// 不覆盖,见 StartRequest)。响应侧:Content-Encoding 为 gzip/deflate 且请求允许解压 →
// header_cb(流式)/done_cb(全量)调 GzipInit,此后增量字节先 inflate 再分发(挂载点即 C11
// 注释预定位置:chunk_cb 内、SseFeed 前 —— SSE_MAX_FRAME 上限自然作用于解压后字节)。
// 其他编码(如 br)不解压,原样透传(调用方自行处理)。
// ============================================================================

// 初始化解压流:encoding 为 "gzip" → inflateInit2(47)(gzip wrapper);"deflate" → inflateInit2(15)
// (zlib wrapper;raw deflate(-15)不识别 —— 与 curl 等主流实现口径一致,待 C18 行为验证)。
// 失败返回 false(仅 inflateInit2 内存错误;调用方按 ERR_PARSE 收口)
static bool GzipInit(ZmHttpClientReqState* st, const char* encoding)
{
    int windowBits;
    if (_stricmp(encoding, "gzip") == 0)
        windowBits = 47;
    else if (_stricmp(encoding, "deflate") == 0)
        windowBits = 15;
    else
        return false;   // 未知编码:不解压(调用方已前置判断,正常不可达)
    GzipDestroyStream(st->gzipStream);   // 防御:旧流作废(正常路径 header_cb 重置已销毁)
    auto* zs = new z_stream();
    if (inflateInit2(zs, windowBits) != Z_OK)
    {
        delete zs;
        return false;
    }
    st->gzipStream = zs;
    st->gzipDone = false;
    return true;
}

// 喂数据:边 inflate 边产出(有界 64KB 输出缓冲,防解压炸弹瞬时大分配);产出经
// st->DispatchDecoded 走消费路径(流式 SSE/回调/落盘;非 2xx 累积;全量累积 m_body)。
// 返回 false = inflate 错误(已置 abortError = ERR_PARSE "gzip inflate failed")或分发失败(已置)
static bool GzipFeed(ZmHttpClientReqState* st, const BYTE* data, size_t len)
{
    auto* zs = static_cast<z_stream*>(st->gzipStream);
    if (st->gzipDone)
        return true;   // 流已完整结束:忽略尾部多余字节(单流编码,尾随数据非本流内容)
    zs->next_in = (Bytef*)data;
    zs->avail_in = (uInt)len;
    BYTE out[64 * 1024];
    while (zs->avail_in > 0 || zs->avail_out == 0)   // 直到输入耗尽且输出缓冲未满
    {
        zs->next_out = out;
        zs->avail_out = (uInt)sizeof(out);
        int rc = inflate(zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END)
        {
            st->gzipDone = true;
            size_t produced = sizeof(out) - zs->avail_out;
            if (produced && !st->DispatchDecoded(out, produced))
                return false;
            return true;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR)
        {
            // Z_DATA_ERROR(损坏)/Z_STREAM_ERROR(状态错)/Z_MEM_ERROR 等:解压失败
            st->abortError = ZM_HTTPC_ERR_PARSE;
            st->abortErrorText = "gzip inflate failed";
            return false;
        }
        size_t produced = sizeof(out) - zs->avail_out;
        if (produced && !st->DispatchDecoded(out, produced))
            return false;
        if (rc == Z_BUF_ERROR)
            break;   // 无进展 = 输入耗尽(本调用 avail_out 恒为 64KB,排除输出缓冲不足):下轮数据到达再喂
    }
    return true;
}

// 收尾:EOF 时 inflate(Z_FINISH) 冲刷残留输出(完整流在 GzipFeed 已见 Z_STREAM_END 即 gzipDone,
// 无残留;截断流 Z_FINISH 因缺输入返回 Z_BUF_ERROR,产出亦已尽数分发);随后释放解压流。
// 截断(缺 gzip 结束标记)不在此报错 —— 传输层 EOF 语义由 error_cb 按 C11 判定。
static void GzipFinish(ZmHttpClientReqState* st)
{
    if (!st->gzipStream)
        return;
    auto* zs = static_cast<z_stream*>(st->gzipStream);
    if (!st->gzipDone)
    {
        BYTE out[64 * 1024];
        for (;;)
        {
            zs->next_out = out;
            zs->avail_out = (uInt)sizeof(out);
            int rc = inflate(zs, Z_FINISH);
            size_t produced = sizeof(out) - zs->avail_out;
            if (produced && !st->DispatchDecoded(out, produced))
                break;
            if (rc == Z_STREAM_END || rc != Z_OK)
                break;   // 完成 / 截断(Z_BUF_ERROR)/ 损坏:冲刷结束
        }
    }
    GzipDestroyStream(st->gzipStream);
}

class ZmHttpClientPrivate
{
public:
    explicit ZmHttpClientPrivate(ZmHttpClient* owner);
    ~ZmHttpClientPrivate();

    bool Start(const char* name);              // 创建并启动 m_loop
    void Stop();                               // 停 m_loop(析构与 CloseAll 用)
    bool SetDnsServers(const char* servers);   // 记录手动 DNS;循环已运行时即时投递应用
    void ApplyDnsServers();                    // C18:手动 DNS 应用(循环线程;空 = 保持系统默认)
    bool SetClientCert(const char* certFile, const char* keyFile);   // 配置 mTLS 证书(置脏懒重建)
    void SetVerifyMode(bool verifyPeer, const char* caFile);         // 配置校验模式(置脏懒重建)
    bool IsLooped() const { return m_loop && m_loop->IsRunning(); }
    event_base* LoopBase() const { return m_loop ? m_loop->EventBase() : nullptr; }
    evdns_base* LoopDnsBase() const { return m_loop ? m_loop->EventDnsBase() : nullptr; }

    // 公开方法委托入口(任意线程可调;内部经 event_base_once 投递到循环线程)
    void PostStartRequest(const ZmHttpClientRequest& req, uint64_t id, void* params,
                          ZmHttpClientCallback cb,
                          std::promise<std::unique_ptr<ZmHttpClientResult>>* syncP = nullptr);
    void PostCancel(uint64_t id);
    void PostCancelAll();
    void PostCloseAll();

    // 引擎函数(静态成员:需访问 Result/Response 私有字段,故不入匿名函数)
    static void FinishRequest(ZmHttpClientReqState* st, int error, const char* errText,
                              bool deferConFree = false);
    static void StartRequest(ZmHttpClientReqState* st);
    static void ZmHttpClientOnDoneCB(struct evhttp_request* hreq, void* arg);
    static void ZmHttpClientOnErrorCB(enum evhttp_request_error err, void* arg);
    static int  ZmHttpClientOnHeaderCB(struct evhttp_request* hreq, void* arg);   // C11:响应头到达(流式)
    static void ZmHttpClientOnChunkCB(struct evhttp_request* hreq, void* arg);    // C11:响应体增量(流式)
    static void FailRequestDirect(ZmHttpClientReqState* st, const char* text);
    // C16 辅助:HTTPS SSL_CTX 获取(直连/代理隧道共用;见 cpp)
    static SSL_CTX* HttpGetUseCtx(ZmHttpClientPrivate* priv, ZmHttpClientReqState* st, bool usePerReqCert);
    // C16:CookieBuildHeader 供自由函数 DispatchOnConnection(请求头组装)调用,置于公开区
    static std::string CookieBuildHeader(ZmHttpClientPrivate* priv, const std::string& scheme,
                                         const std::string& host, const std::string& path);

    // 引擎状态(循环线程独占)
    std::vector<ZmHttpClientReqState*> m_inflight;   ///< 在飞请求

private:
    ZmHttpClient*     m_owner;
    ZmHttpClientLoop* m_loop;
    std::string m_dnsServers;        ///< 手动 DNS(空 = 系统默认)
    std::string m_clientCertFile;
    std::string m_clientKeyFile;
    bool        m_verifyPeer = true;
    std::string m_caFile;

    // ── TLS(C8):客户端级缓存 SSL_CTX(懒构建;循环线程独占访问)──
    SSL_CTX* m_sslCtx = nullptr;        ///< 客户端级缓存 SSL_CTX(懒构建)
    bool     m_sslCtxDirty = false;     ///< 配置变更后需重建(下次 HTTPS 请求重建)

    // ── 连接池(C6)──
    std::map<std::string, std::vector<struct evhttp_connection*>> m_idleConns;  ///< key=scheme:host:port,空闲连接缓存
    static constexpr int ZM_HTTPC_MAX_IDLE_PER_HOST = 4;                        ///< 每主机空闲连接上限

    // 连接池/取消辅助(私有静态成员,见 cpp)
    static std::string HttpClientPoolKey(const std::string& scheme, const std::string& host, uint16_t port);
    static struct evhttp_connection* HttpClientPoolAcquire(ZmHttpClientPrivate* priv,
        const std::string& scheme, const std::string& host, uint16_t port);
    static void HttpClientPoolRelease(ZmHttpClientPrivate* priv, struct evhttp_connection* con,
        const std::string& scheme, const std::string& host, uint16_t port);
    static void HttpClientPoolClearAll(ZmHttpClientPrivate* priv);
    static void CancelInflightAll(ZmHttpClientPrivate* priv);

    // ── C14 cookie jar(进程内,不持久化磁盘 —— spec §6 已知限制;循环线程独占,无锁)──
    struct ZmCookie
    {
        std::string name;
        std::string value;
        std::string path;        ///< 空 = 未指定(存储时按请求路径目录 default-path)
        std::string domain;      ///< 小写;空 = 仅主机域
        bool        secure = false;
        int64_t     expires = 0; ///< 绝对时间戳(秒);0 = 会话 cookie(不过期)
    };
    std::map<std::string, std::vector<ZmCookie>> m_cookieJar;  ///< key = domain(小写)

    // cookie jar 辅助(静态成员,见 cpp;C16:CookieBuildHeader 供自由函数
    // DispatchOnConnection 调用,故声明置于公开区,见类首部)
    static bool        CookieParseSetCookie(const std::string& setCookie, ZmCookie& out);
    static void        CookieStore(ZmHttpClientPrivate* priv, const std::string& host,
                                   const std::string& path, const std::string& setCookie);
};

// ============================================================================
// 连接池(C6)
// ============================================================================

std::string ZmHttpClientPrivate::HttpClientPoolKey(const std::string& scheme, const std::string& host, uint16_t port)
{
    // string 拼接:避免固定缓冲截断长主机名(253 字符)导致 key 碰撞错配
    return scheme + ":" + host + ":" + std::to_string(port);
}

// 获取连接:有空闲则复用(校验 bufferevent 仍有效,否则丢弃重建);无空闲返回 nullptr(调用方新建)
struct evhttp_connection* ZmHttpClientPrivate::HttpClientPoolAcquire(ZmHttpClientPrivate* priv,
    const std::string& scheme, const std::string& host, uint16_t port)
{
    const std::string key = HttpClientPoolKey(scheme, host, port);
    auto it = priv->m_idleConns.find(key);
    if (it == priv->m_idleConns.end())
        return nullptr;
    auto& idle = it->second;
    while (!idle.empty())
    {
        struct evhttp_connection* con = idle.back();
        idle.pop_back();
        struct bufferevent* bev = evhttp_connection_get_bufferevent(con);
        if (bev && (bufferevent_get_enabled(bev) & EV_READ))
            return con;   // 有效,复用
        evhttp_connection_free(con);   // 已失效,丢弃
    }
    if (idle.empty())
        priv->m_idleConns.erase(it);
    return nullptr;
}

// 归还连接:keep-alive 且空闲未满 → 缓存;否则销毁
void ZmHttpClientPrivate::HttpClientPoolRelease(ZmHttpClientPrivate* priv, struct evhttp_connection* con,
    const std::string& scheme, const std::string& host, uint16_t port)
{
    if (!con) return;
    const std::string key = HttpClientPoolKey(scheme, host, port);
    auto& idle = priv->m_idleConns[key];
    if ((int)idle.size() < ZM_HTTPC_MAX_IDLE_PER_HOST)
    {
        idle.push_back(con);   // keep-alive 复用
        return;
    }
    evhttp_connection_free(con);
}

// 清空全部(CloseAll / 析构调用)
void ZmHttpClientPrivate::HttpClientPoolClearAll(ZmHttpClientPrivate* priv)
{
    for (auto& kv : priv->m_idleConns)
        for (auto* con : kv.second)
            evhttp_connection_free(con);
    priv->m_idleConns.clear();
}

// 取消全部在飞请求(PostCancelAll / PostCloseAll 公共逻辑;循环线程内调用)
void ZmHttpClientPrivate::CancelInflightAll(ZmHttpClientPrivate* priv)
{
    auto inflight = priv->m_inflight;   // 拷贝:取消回调(收口)会修改 m_inflight
    for (auto* st : inflight)
    {
        st->cancelled = true;
        if (st->hreq)
            evhttp_cancel_request(st->hreq);
        else if (st->retryEvent)
        {
            // C15 退避窗口:无在飞请求(收口前置清理已释放 hreq),仅退避定时器挂起。
            // 仅置 cancelled 的话,定时器到点前循环若停止(CloseAll/析构)永不触发 →
            // st 泄漏且回调永不触发(违反"恰好一次"契约,st 含 sync promise/outFile)。
            // 取消定时器并立即收口(CANCELLED)。退避窗口无 hreq,error_cb 不会同步
            // 触发,无双交付;FinishRequest 阶段③对已置空的 retryEvent 幂等。
            event_del(st->retryEvent);
            event_free(st->retryEvent);
            st->retryEvent = nullptr;
            FinishRequest(st, ZM_HTTPC_ERR_CANCELLED, "cancelled");
        }
        else if (st->tunnelBev)
        {
            // C16 CONNECT 隧道阶段:无 hreq(未发起 make_request)、无退避定时器,仅
            // 隧道 bev 在飞。仅置 cancelled 的话本分支将收不到任何后续回调(隧道 bev
            // 由本函数释放,readcb/eventcb 不再触发)→ st 泄漏且回调永不触发。
            // 与退避窗口同款处理:直接收口(CANCELLED),FinishRequest 阶段①释放隧道 bev。
            FinishRequest(st, ZM_HTTPC_ERR_CANCELLED, "cancelled");
        }
    }
}

// ============================================================================
// C14 cookie jar(进程内,不持久化磁盘 —— spec §6 已知限制;循环线程独占,无锁)
// 请求侧:UseCookieJar(默认开)且用户未显式设置 Cookie 头 → 按域后缀 + 路径前缀匹配
//   附加(跨主机重定向的显式 Cookie 已由 C9 从请求副本剥离;jar 按新主机重新匹配);
// 响应侧:Set-Cookie(可多个)解析存储 —— header_cb(流式)/done_cb(全量)两挂点均在
//   重定向早退之前(C13 审查登记),重定向链中间响应与非 2xx 响应的 Set-Cookie 亦存储;
// 简化(spec §6 已知限制):HttpOnly/SameSite 不解析(客户端 jar 无脚本侧)、
//   Expires 仅 IMF-fixdate 格式、name/value 不做引号展开、jar key 不区分
//   "仅主机域 cookie"与"Domain 属性 = 该主机 的 cookie"(同名同路径互相覆盖)。
// ============================================================================

// 段首尾空白裁剪(RFC 6265 §4.1.1:仅 SP/HTAB)
static std::string CookieTrim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return std::string();
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// 请求路径的目录部分(RFC 6265 §5.1.4 default-path:去 query 后取最右 '/' 之前)
// 空 / 不以 '/' 开头 / 仅一个 '/' → "/"
static std::string CookiePathDir(const std::string& path)
{
    std::string p = path;
    size_t q = p.find_first_of("?#");
    if (q != std::string::npos)
        p.erase(q);
    if (p.empty() || p[0] != '/')
        return "/";
    size_t slash = p.rfind('/');
    if (slash == 0)
        return "/";
    return p.substr(0, slash);
}

// cookie path 匹配(RFC 6265 §5.1.4):完整相等 / cookie path 为前缀且以 '/' 结尾 /
// 前缀且请求路径下一字符为 '/'(防 "/a" 误配 "/ab")
static bool CookiePathMatch(const std::string& cookiePath, const std::string& reqPath)
{
    if (cookiePath == reqPath)
        return true;
    if (reqPath.compare(0, cookiePath.size(), cookiePath) != 0)
        return false;
    if (!cookiePath.empty() && cookiePath.back() == '/')
        return true;
    return reqPath.size() > cookiePath.size() && reqPath[cookiePath.size()] == '/';
}

// HTTP-date 解析(Expires 属性;RFC 7231 IMF-fixdate "Sun, 06 Nov 1994 08:49:37 GMT")
// 简化:仅 IMF-fixdate 格式;解析失败返回 0(调用方按会话 cookie 处理)
static time_t CookieParseHttpDate(const std::string& s)
{
    static const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = { 0 };
    if (sscanf_s(s.c_str(), "%*3s, %d %3s %d %d:%d:%d GMT", &day, mon, (unsigned)sizeof(mon),
                 &year, &hh, &mm, &ss) != 6)
        return 0;
    int month = -1;
    for (int i = 0; i < 12; ++i)
        if (_strnicmp(mon, kMonths[i], 3) == 0) { month = i; break; }
    if (month < 0 || day < 1 || day > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60)
        return 0;
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month;
    t.tm_mday = day;
    t.tm_hour = hh;
    t.tm_min = mm;
    t.tm_sec = ss;
    t.tm_isdst = 0;
    return _mkgmtime(&t);   // UTC → time_t(失败返回 (time_t)-1,调用方判 >0)
}

// 解析单个 Set-Cookie 头(RFC 6265 §4.1 子集):
//   name=value 必填;Path/Domain/Max-Age/Secure 解析;Expires 解析失败按会话 cookie;
//   Max-Age=0 → 立即过期(删除判定见 CookieStore);Domain 规范化(去前导点、转小写);
//   Path 属性缺省或空值 → out.path 留空(CookieStore 按请求路径目录补齐 = RFC 6265 default-path);
//   name/value 含 CR/LF → 拒绝(防注入);Max-Age 优先于 Expires(RFC 6265 §4.1.2.2)
bool ZmHttpClientPrivate::CookieParseSetCookie(const std::string& setCookie, ZmCookie& out)
{
    out = ZmCookie();   // 默认:path 空(未指定 → 存储时按请求路径目录 default-path),domain 空,secure=false,expires=0(会话)
    size_t semi = setCookie.find(';');
    std::string first = (semi == std::string::npos) ? setCookie : setCookie.substr(0, semi);
    size_t eq = first.find('=');
    if (eq == std::string::npos || eq == 0)
        return false;   // 无 name=value:非法 Set-Cookie
    out.name = CookieTrim(first.substr(0, eq));
    out.value = CookieTrim(first.substr(eq + 1));
    if (out.name.empty())
        return false;
    // 防注入:name/value 含 CR/LF 拒绝该 cookie(不经 evhttp_add_header 回显)
    if (out.name.find_first_of("\r\n") != std::string::npos ||
        out.value.find_first_of("\r\n") != std::string::npos)
        return false;

    bool hasMaxAge = false;
    int64_t maxAgeExpires = 0;
    size_t pos = (semi == std::string::npos) ? std::string::npos : semi + 1;
    while (pos != std::string::npos)
    {
        size_t nextSemi = setCookie.find(';', pos);
        std::string attr = (nextSemi == std::string::npos) ? setCookie.substr(pos)
                                                           : setCookie.substr(pos, nextSemi - pos);
        attr = CookieTrim(attr);
        if (!attr.empty())
        {
            size_t ae = attr.find('=');
            std::string aname = CookieTrim(ae == std::string::npos ? attr : attr.substr(0, ae));
            std::string aval  = (ae == std::string::npos) ? std::string() : CookieTrim(attr.substr(ae + 1));
            if (_stricmp(aname.c_str(), "Path") == 0)
            {
                if (!aval.empty())
                {
                    out.path = aval;
                    if (out.path[0] != '/')
                        out.path = "/" + out.path;   // 简化:不以 '/' 开头补齐(而非按 RFC 忽略该属性)
                }
                // Path= 空值:与缺省相同(RFC 6265 §5.3.2)→ 保持空,由 CookieStore default-path 处理
            }
            else if (_stricmp(aname.c_str(), "Domain") == 0)
            {
                if (!aval.empty())
                {
                    out.domain = aval;
                    if (out.domain[0] == '.')
                        out.domain.erase(0, 1);   // 去前导点
                    for (char& c : out.domain)
                        c = (char)tolower((unsigned char)c);
                }
                // 空 Domain 属性:忽略(RFC 6265 §5.3:按缺省 → 仅主机域)
            }
            else if (_stricmp(aname.c_str(), "Max-Age") == 0)
            {
                if (!aval.empty())
                {
                    char* endp = nullptr;
                    long v = strtol(aval.c_str(), &endp, 10);
                    if (endp && *endp == '\0')
                    {
                        hasMaxAge = true;
                        maxAgeExpires = (int64_t)time(nullptr) + v;   // v=0 → now(立即过期 → 删除)
                    }
                }
            }
            else if (_stricmp(aname.c_str(), "Expires") == 0)
            {
                if (!aval.empty())
                {
                    time_t t = CookieParseHttpDate(aval);
                    if (t > 0)
                        out.expires = (int64_t)t;   // 解析失败维持会话 cookie(expires=0)
                }
            }
            else if (_stricmp(aname.c_str(), "Secure") == 0)
            {
                out.secure = true;   // 仅 https 发送(匹配侧判定)
            }
            // 其余属性(HttpOnly/SameSite 等)忽略
        }
        pos = (nextSemi == std::string::npos) ? std::string::npos : nextSemi + 1;
    }
    if (hasMaxAge)
        out.expires = maxAgeExpires;   // Max-Age 优先于 Expires(RFC 6265 §4.1.2.2)
    return true;
}

// 存储(循环线程):Domain 属性存在 → 存到该域(小写);缺省 → 存到请求 host 域;
// 请求 host 为 IP 字面量 → 忽略 Domain 属性(host-only,RFC 6265 §5.3 step 5);
// Path 缺省或空值 → cookie path 按请求路径目录(调用方已传目录部分);
// 同名同域同路径 → 覆盖;expires<=now(Max-Age=0 / Expires 已过)→ 删除;
// 安全:RFC 6265 §5.3 step 6 —— Domain 属性与请求 host 域不匹配时拒绝存储
// (防跨域投毒;设计简化项,此处按规范收紧,匹配规则与 CookieBuildHeader 一致)
void ZmHttpClientPrivate::CookieStore(ZmHttpClientPrivate* priv, const std::string& host,
                                      const std::string& path, const std::string& setCookie)
{
    if (!priv)
        return;
    ZmCookie c;
    if (!CookieParseSetCookie(setCookie, c))
        return;   // 非法 Set-Cookie(缺 name=value):忽略

    // RFC 6265 §5.3 step 5:请求 host 为 IP 字面量(含 ':' 的 IPv6 / 全数字与点的 IPv4)
    // 时忽略 Domain 属性(cookie 变 host-only —— 防 192.168.1.5 设 Domain=168.1.5 后
    // 泄漏给 *.168.1.5 下任意主机)
    bool hostIsIp = (host.find(':') != std::string::npos);
    if (!hostIsIp && !host.empty())
    {
        hostIsIp = true;
        for (char ch : host)
            if (!isdigit((unsigned char)ch) && ch != '.') { hostIsIp = false; break; }
    }
    if (hostIsIp)
        c.domain.clear();

    std::string domain = c.domain.empty() ? host : c.domain;
    for (char& ch : domain)
        ch = (char)tolower((unsigned char)ch);
    if (domain.empty())
        return;

    if (!c.domain.empty())
    {
        // Domain 属性存在:须与请求 host 域匹配(host == domain 或 host 以 "."+domain 结尾)
        std::string h = host;
        for (char& ch : h)
            ch = (char)tolower((unsigned char)ch);
        bool ok = (h == domain);
        if (!ok && h.size() > domain.size() &&
            h[h.size() - domain.size() - 1] == '.' &&
            h.compare(h.size() - domain.size(), domain.size(), domain) == 0)
            ok = true;
        if (!ok)
            return;   // 跨域 Domain 属性:拒绝存储(RFC 6265 §5.3)
    }

    if (c.path.empty())
        c.path = (path.empty() || path[0] != '/') ? "/" : path;   // Path 缺省 → 请求路径目录

    if (c.expires != 0 && c.expires <= (int64_t)time(nullptr))
    {
        // 已过期(Max-Age=0 / Expires 在过去):删除
        auto mit = priv->m_cookieJar.find(domain);
        if (mit != priv->m_cookieJar.end())
        {
            auto& vec = mit->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                [&](const ZmCookie& e) { return e.name == c.name && e.path == c.path; }),
                vec.end());
            if (vec.empty())
                priv->m_cookieJar.erase(mit);   // 顺手清空空域条目
        }
        return;
    }

    auto& vec = priv->m_cookieJar[domain];
    auto it = std::find_if(vec.begin(), vec.end(),
        [&](const ZmCookie& e) { return e.name == c.name && e.path == c.path; });
    if (it != vec.end())
        *it = c;   // 同名同域同路径 → 覆盖
    else
        vec.push_back(c);
}

// 构建请求 Cookie 头:域后缀匹配(host == domain 或 host 以 "."+domain 结尾)+
// path 前缀匹配(RFC 6265 §5.1.4,见 CookiePathMatch)+ secure 仅 https 发送 +
// 未过期(expires==0 会话 cookie 不过期;顺带清理过期项)+ 仅主机域 cookie 只发
// 请求 host 本身(不发给子域,RFC 6265 §5.1.3 host-only)。
// 同 name 多 path 并发项按 path 长度降序输出(RFC 6265 §5.4)。无匹配返回空串
std::string ZmHttpClientPrivate::CookieBuildHeader(ZmHttpClientPrivate* priv,
                                                   const std::string& scheme,
                                                   const std::string& host,
                                                   const std::string& path)
{
    std::string out;
    if (!priv)
        return out;
    std::string h = host;
    for (char& c : h)
        c = (char)tolower((unsigned char)c);
    std::string reqPath = path;
    size_t q = reqPath.find_first_of("?#");
    if (q != std::string::npos)
        reqPath.erase(q);   // 匹配用 request-uri path(RFC 6265:不含 query)
    if (reqPath.empty())
        reqPath = "/";
    const bool isSecure = (scheme == "https");
    const int64_t now = (int64_t)time(nullptr);

    std::vector<const ZmCookie*> matched;
    for (auto& kv : priv->m_cookieJar)
    {
        const std::string& domain = kv.first;
        bool dm = (h == domain);
        if (!dm && h.size() > domain.size() &&
            h[h.size() - domain.size() - 1] == '.' &&
            h.compare(h.size() - domain.size(), domain.size(), domain) == 0)
            dm = true;
        if (!dm)
            continue;
        auto& vec = kv.second;
        for (size_t i = 0; i < vec.size(); )
        {
            const ZmCookie& c = vec[i];
            if (c.expires != 0 && c.expires <= now)
            {
                vec.erase(vec.begin() + i);   // 过期:顺带清理(erase 不影响此前收集的指针)
                continue;
            }
            if (c.domain.empty() && h != domain)
            {
                ++i;   // 仅主机域 cookie:只发请求 host 本身,不发给子域
                continue;
            }
            if (c.secure && !isSecure)
            {
                ++i;   // secure cookie 仅 https 发送(不清理)
                continue;
            }
            if (!CookiePathMatch(c.path, reqPath))
            {
                ++i;
                continue;
            }
            matched.push_back(&c);
            ++i;
        }
    }
    std::sort(matched.begin(), matched.end(),
        [](const ZmCookie* a, const ZmCookie* b) { return a->path.size() > b->path.size(); });
    for (const ZmCookie* c : matched)
    {
        if (!out.empty())
            out += "; ";
        out += c->name + "=" + c->value;
    }
    return out;
}

// ============================================================================
// 投递机制:任意线程 → 循环线程(event_base_once;EV_TIMEOUT + NULL timeout =
// "success immediately",即下一轮循环执行;base 已启用线程锁,线程安全)
// ============================================================================

struct ZmHttpClientPostArg
{
    std::function<void()> fn;
};

static void ZmHttpClientPostCB(evutil_socket_t /*fd*/, short /*what*/, void* arg)
{
    auto* a = static_cast<ZmHttpClientPostArg*>(arg);
    a->fn();
    delete a;
}

// 返回 false = 投递失败(循环已不可用),调用方需自行兜底
static bool PostToLoop(event_base* base, std::function<void()> fn)
{
    if (!base)
        return false;
    auto* arg = new ZmHttpClientPostArg{ std::move(fn) };
    if (event_base_once(base, -1, EV_TIMEOUT, ZmHttpClientPostCB, arg, nullptr) != 0)
    {
        delete arg;
        return false;
    }
    return true;
}

// ============================================================================
// 请求总超时(deadline):超时取消请求,收口为 TIMEOUT(区别于主动取消的 CANCELLED)
// ============================================================================

static void ZmHttpClientTotalTimeoutCB(evutil_socket_t /*fd*/, short /*what*/, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);

    st->timeoutByDeadline = true;
    st->cancelled = true;
    if (st->hreq)
    {
        // 取消会同步触发 error_cb(CANCEL)(已核对 libevent 2.2.1:evhttp_cancel_request →
        // evhttp_connection_fail_ → error_cb,且无 done_cb 跟进)→ 收口(经 timeoutByDeadline
        // 区分 TIMEOUT)。收口含 delete st 与总超时事件清理(本回调内 event_free 自身安全,
        // 事件已在派发前出队),故此处取用 hreq 后不得再访问 st。
        evhttp_cancel_request(st->hreq);
        return;
    }
    // 未发起请求(罕见):直接收口
    ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_TIMEOUT, "total timeout");
}

// ============================================================================
// URL 解析:http(s)://host[:port]/path
// 规则:默认端口 http=80/https=443;path 为空 → "/";query 原样保留
// ============================================================================

static bool ParseHttpUrl(const std::string& url, std::string& scheme, std::string& host,
                         uint16_t& port, std::string& path)
{
    scheme.clear();
    host.clear();
    port = 0;
    path.clear();
    if (url.empty())
        return false;

    size_t p = url.find("://");
    if (p == std::string::npos || p == 0)
        return false;

    scheme = url.substr(0, p);
    for (char& c : scheme)
        c = (char)tolower((unsigned char)c);   // scheme 大小写不敏感
    if (scheme == "http")
        port = 80;
    else if (scheme == "https")
        port = 443;
    else
        return false;   // 非 http/https(调用方按 ERR_UNSUPPORTED 收口)

    std::string rest = url.substr(p + 3);
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    // query/fragment 若出现在 host 段内(路径缺失,如 http://host?x=1):截断 host 到 ?/# 之前,
    // 并把 ?/# 之后的内容并入 path(RFC 3986:无路径时 query/fragment 直接跟在 authority 后)
    size_t qf = hostport.find_first_of("?#");
    if (qf != std::string::npos)
    {
        path = hostport.substr(qf) + (slash == std::string::npos ? std::string() : path);
        hostport = hostport.substr(0, qf);
    }
    // 请求目标须以 '/' 开头(RFC 3986 request-target);无路径或未以 '/' 开头时补齐
    if (path.empty() || path[0] != '/')
        path = "/" + path;
    if (hostport.empty())
        return false;

    // IPv6 字面量:[::1]:8080(端口须为 1-5 位全数字且 1-65535,否则非法)
    if (hostport[0] == '[')
    {
        size_t close = hostport.find(']');
        if (close == std::string::npos)
            return false;
        host = hostport.substr(1, close - 1);
        std::string portPart = hostport.substr(close + 1);
        if (!portPart.empty())
        {
            if (portPart[0] != ':')
                return false;
            portPart = portPart.substr(1);
            if (portPart.empty() || portPart.size() > 5)
                return false;
            for (char c : portPart)
                if (!isdigit((unsigned char)c))
                    return false;
            long v = strtol(portPart.c_str(), nullptr, 10);
            if (v <= 0 || v > 65535)
                return false;
            port = (uint16_t)v;
        }
        return !host.empty();
    }

    // host[:port](主机名不含 ':',冒号后必须为 1-5 位全数字端口且 1-65535,否则非法)
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos)
    {
        std::string portPart = hostport.substr(colon + 1);
        if (portPart.empty() || portPart.size() > 5)
            return false;
        for (char c : portPart)
            if (!isdigit((unsigned char)c))
                return false;
        long v = strtol(portPart.c_str(), nullptr, 10);
        if (v <= 0 || v > 65535)
            return false;
        port = (uint16_t)v;
        host = hostport.substr(0, colon);
        return !host.empty();
    }

    host = hostport;
    return !host.empty();
}

// ============================================================================
// 重定向 Location 解析(C9):绝对 URL(http:// / https:// 开头,大小写不敏感)原样
// 使用;相对路径(/foo 或 foo)以原请求 scheme://host[:port] 为基拼接(默认端口省略)。
// 返回空串 = 无法解析(调用方按普通响应处理,不跟随,避免死循环)。
// 注:不做 RFC 3986 相对路径归一化(../ 等),Location 均按主机根解析;协议相对
// URL(//host/path)亦按路径处理(超出设计范围,服务端不常见)。
// ============================================================================

static std::string ResolveRedirectUrl(const std::string& loc,
                                      const std::string& scheme, const std::string& host, uint16_t port)
{
    std::string url;
    if (_strnicmp(loc.c_str(), "http://", 7) == 0 || _strnicmp(loc.c_str(), "https://", 8) == 0)
    {
        url = loc;   // 绝对 URL(scheme/格式合法性交由 ParseHttpUrl 下放校验)
    }
    else if (loc[0] == '#')
    {
        return std::string();   // 纯 fragment:无实际目标,拒绝跟随
    }
    else
    {
        url = scheme + "://" + host;
        if (!((scheme == "http" && port == 80) || (scheme == "https" && port == 443)))
            url += ":" + std::to_string(port);
        url += (loc[0] == '/') ? loc : std::string("/") + loc;
    }
    std::string s; std::string h; uint16_t p = 0; std::string path;
    if (!ParseHttpUrl(url, s, h, p, path))
        return std::string();   // 非法 Location:拒绝跟随
    return url;
}

// ============================================================================
// C15 重试(指数退避):幂等方法(GET/HEAD/PUT/DELETE)+ 连接类错误 → 自动重试
//   预算分离(C9 审查登记):retryCount 独立于 attempts(重定向预算) —— 重定向不消耗
//   重试预算(retryCount 仅 MaybeRetry 递增),重试不消耗重定向预算(OnDoneCB 的跳转
//   上限用 attempts - retryCount 判定);两者合计仍受总超时约束(deadline 不重臂,
//   覆盖整条链含重试)。取消置位(cancelled)后任何时刻不再重试(本函数前置检查 +
//   StartRequest 开头检查兜底);重试耗尽时透出最后一次错误(返回 false,调用方收口)。
//   退避定时用 event_new 定时器而非 event_base_once:event_base_once(含超时版本)
//   无法取消 —— 退避窗口内总超时/收口会删除 st,迟到的回调将悬垂访问;event_new
//   事件可由 FinishRequest event_del+event_free 取消,delete st 前必已清理。
// ============================================================================

static void ZmHttpClientRetryCB(evutil_socket_t /*fd*/, short /*what*/, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);
    // 事件已触发(派发前出队,非活动):自行释放并置空 —— FinishRequest 的清理只处理
    // 仍挂起的事件(退避窗口内被收口),二者分工保证恰好释放一次
    struct event* ev = st->retryEvent;
    st->retryEvent = nullptr;
    if (ev)
    {
        event_del(ev);   // 幂等空操作(非活动;与总超时回调内释放自身同一安全依据)
        event_free(ev);
    }
    ZmHttpClientPrivate::StartRequest(st);   // 重入(attempts 递增;cancelled 检查在其开头)
}

// 返回 true = 已安排重试(调用方不再收口:不 set_value/不回调/不 delete st);false = 正常收口
static bool MaybeRetry(ZmHttpClientPrivate* priv, ZmHttpClientReqState* st, int error)
{
    if (st->cancelled || st->finished)
        return false;
    if (st->req.RetryCount() <= 0 || st->retryCount >= st->req.RetryCount())
        return false;
    // chunked 上传不重试(C12 审查登记):上传重试需重置泵状态(输出缓冲残留帧/终止块/
    // 进度),复杂且语义差 —— 文件/流式上传请求跳过重试
    if (st->req.UploadChunk() || !st->req.BodyFile().empty())
        return false;
    // 幂等方法才重试(POST/PATCH 非幂等,重放有重复提交风险;调用方自行处理)
    const std::string& m = st->req.Method();
    if (!(m == "GET" || m == "HEAD" || m == "PUT" || m == "DELETE"))
        return false;
    // 连接类错误才重试(响应/解析/本地 IO 等错误不重试)
    if (error != ZM_HTTPC_ERR_CONNECT && error != ZM_HTTPC_ERR_CONNECT_TIMEOUT
        && error != ZM_HTTPC_ERR_TIMEOUT && error != ZM_HTTPC_ERR_STREAM_BROKEN)
        return false;

    st->retryCount++;

    // 指数退避:500/1000/2000...(base * 2^(retryCount-1));shift 封顶防 1<<n 溢出(UB),
    // 时长超剩余 deadline 时由总超时兜底终止整条链(退避窗口内收口会取消本定时器)
    int shift = st->retryCount - 1;
    if (shift > 20) shift = 20;
    int64_t delayMs = (int64_t)st->req.RetryBaseDelayMs() * ((int64_t)1 << shift);
    if (delayMs > 0x7FFFFFFF) delayMs = 0x7FFFFFFF;   // 封顶(极端退避参数防溢出)

    // 重入守卫(防御,当前不可达:retryEvent 已触发时 RetryCB 置空,收口时 FinishRequest
    // 清理;若未来引入新的收口前路径,防止覆盖在飞定时器导致泄漏/悬垂 → 按本次错误收口)
    if (st->retryEvent)
    {
        st->retryCount--;
        return false;
    }
    // 退避定时器(可取消;见上方 event_base_once 不用的原因)
    st->retryEvent = event_new(priv->LoopBase(), -1, EV_TIMEOUT, ZmHttpClientRetryCB, st);
    if (!st->retryEvent)
    {
        st->retryCount--;
        return false;   // 无法调度重试(内存错误):按本次错误收口
    }
    struct timeval tv = { (long)(delayMs / 1000), (int)(delayMs % 1000) * 1000 };
    if (event_add(st->retryEvent, &tv) != 0)
    {
        event_free(st->retryEvent);
        st->retryEvent = nullptr;
        st->retryCount--;
        return false;
    }

    // ── 状态重置(调度成功后才执行:C15 关键 —— 重试 = 同一请求的新一次尝试,
    // 旧尝试的响应/流状态全部作废;调度失败路径保持 result 的最后错误透出)──
    st->result = ZmHttpClientResult();   // 清错误/响应(m_error/m_errorText/m_status/m_headers/m_body/m_contentLength)
    // gzip 状态重置(C13 审查登记):GzipDestroyStream 直接作废残留解压流 —— 不能用
    // GzipFinish(Z_FINISH 冲刷会把旧尝试的残留解压字节冲进新尝试的响应体)
    GzipDestroyStream(st->gzipStream);
    st->gzipDone = false;
    // 流式/SSE 标志复位(C11):旧尝试的流状态不延续
    st->streamingBody = false;
    st->non2xxResponse = false;
    st->eofOk = false;
    st->abortError = ZM_HTTPC_OK;
    st->abortErrorText.clear();
    st->non2xxBody.clear();
    st->gzipFullBody.clear();
    st->sseBuffer.clear();
    st->pendingError = ZM_HTTPC_OK;   // 防御:旧尝试的待定错误不延续
    st->pendingErrorText.clear();
    // 进度计数复位(progress total 未知 = -1)
    st->bytesReceived = 0;
    st->contentLengthTotal = -1;
    // 上传状态(C12 登记):chunked 上传不重试(上方门),此处不可能有 uploadCb/uploadFile
    // 残留;内存体无状态(evhttp_make_request 已拷贝),无需处理
    // outFile(C10):重试保留已开文件并从头重发 —— fseek(0)(断点续传由调用方用 Range 实现)
    if (st->outFile)
        fseek(st->outFile, 0, SEEK_SET);

    return true;
}

// ============================================================================
// 收口:任何路径(成功/错误/取消/超时)只经此函数一次(循环线程)
// 三段式(C15):① 前置清理(上传泵/连接/句柄/请求级 SSL_CTX —— 收口与重试共用) →
// ② MaybeRetry 判定(命中 → 请求延续,跳过最终收口)→ ③ 最终收口
// (事件清理/解压流收尾/落盘文件/在飞移除/交付)。outFile 与 totalTimeoutEvent
// 不在前置清理处置:重试需要它们(outFile 从头重写;deadline 不重臂覆盖整条链含重试)。
// deferConFree:libevent 回调返回后仍会引用连接时(连接超时 cleanup 路径)置真,
// 连接改由 event_base_once 延后释放(下一轮循环),避免悬垂访问。
// ============================================================================

void ZmHttpClientPrivate::FinishRequest(ZmHttpClientReqState* st,
                                        int error, const char* errText, bool deferConFree)
{
    if (st->finished)
        return;
    st->result.m_error = error;
    if (errText) st->result.m_errorText = errText;

    // ── 阶段①:前置清理(收口与重试共用;重试路径经阶段②返回,st 保留、不交付)──
    // C12 上传泵/文件体收尾:先移除输出缓冲回调(防 bev 回池复用后旧回调悬垂触发、
    // 亦防悬垂 st),再关闭上传文件;随后连接释放/回池(回调与连接生命周期解耦)。
    // 注意:取消路径(st 经 error_cb 删除)可能在泵回调栈内触发本函数 —— 泵回调
    // 在触发取消后不再访问 st,此处清理与其顺序安全(详见泵的 ferror 分支注释)。
    // (C15:重试请求必无上传状态 —— MaybeRetry 的 chunked 上传门;此块仅收口路径可达)
    if (st->uploadCb)
    {
        if (st->con)
        {
            struct bufferevent* bev = evhttp_connection_get_bufferevent(st->con);
            struct evbuffer* out = bev ? bufferevent_get_output(bev) : nullptr;
            if (out)
                evbuffer_remove_cb_entry(out, st->uploadCb);
        }
        st->uploadCb = nullptr;   // 连接已随错误释放时回调条目随缓冲释放,置空即可
    }
    if (st->uploadFile)
    {
        fclose(st->uploadFile);
        st->uploadFile = nullptr;
    }

    // 连接收尾:成功且响应可 keep-alive → 归还连接池;错误/不可复用 → 直接释放
    // (C15:重试路径 error 恒非 OK,此块对重试恒走释放分支 —— 重试的新连接由 StartRequest 重建)
    if (st->con)
    {
        struct evhttp_connection* con = st->con;
        st->con = nullptr;
        if (deferConFree)
        {
            // 当前 evhttp 回调栈返回后仍会读连接(evhttp_connection_cb_cleanup):延后释放
            // (该路径连接状态不可靠,不归还池,直接 free)
            PostToLoop(st->priv->LoopBase(), [con]() { evhttp_connection_free(con); });
        }
        else if (error == ZM_HTTPC_OK &&
                 !st->IsStreamingConsumer() &&   // C11:流式连接不复用(流状态不可靠,EOF/结束后即释放)
                 !st->req.UploadChunk() && st->req.BodyFile().empty() &&  // C12:上传连接不复用(服务端提前响应未消费体时,输出缓冲可能残留未发送块帧,复用会污染下个请求)
                 _stricmp(st->result.m_response.Header("Connection").c_str(), "close") != 0 &&
                 !st->perReqSslCtx &&
                 !st->req.HasProxy())   // C16:代理连接不回池(连接目标对后续请求不透明 ——
                                        // 池 key 按目标主机组织,代理连接复归直连请求会错配;直接释放)
        {
            // 成功且响应头无 Connection: close → 空闲连接归还池(keep-alive 复用;
            // 请求级证书覆盖的连接携带临时身份,不回池直接释放,防污染后续请求)
            HttpClientPoolRelease(st->priv, con, st->scheme, st->host, st->port);
        }
        else
        {
            evhttp_connection_free(con);   // 错误路径/Connection: close:不归还
        }
    }
    // C16 隧道 SSL 对象:SSL bev 不带 CLOSE_ON_FREE → be_ssl_destruct 不释放它
    // (SSL_context_free 按标志门控,已核对)—— 须在 con(SSL bev)释放后再 SSL_free;
    // SSL_free 经 BIO 不触碰底层(bio_bufferevent_free 的 shutdown 未置位,已核对)。
    // 重试(C15)/重定向(C9)重走代理路径时旧对象不延续,故在此统一释放置空。
    if (st->tunnelSsl)
    {
        SSL_free(st->tunnelSsl);
        st->tunnelSsl = nullptr;
    }
    // C16 CONNECT 隧道 bev:隧道建立后 SSL bev(filter)经 finalize 对底层恰 decref
    // 一次(不带 CLOSE_ON_FREE),须在 con(SSL bev)释放之后再释放我方引用 —— 引用
    // 归零、fd 恰好关闭一次(be_socket_destruct 按引用归零关 fd);deferConFree 时
    // 一并延后(经两次 PostToLoop,先 con 后 bev,顺序保持)。
    // CONNECT 阶段(con 为空)失败/取消:此处直接释放并关闭 socket。
    if (st->tunnelBev)
    {
        struct bufferevent* tb = st->tunnelBev;
        st->tunnelBev = nullptr;
        if (deferConFree)
            PostToLoop(st->priv->LoopBase(), [tb]() { bufferevent_free(tb); });
        else
            bufferevent_free(tb);
        // 旧尝试的 CONNECT 响应残留不延续:重试(C15)/重定向(C9)会开新隧道,
        // 残留部分响应头与新响应拼接会解析错乱
        st->tunnelBuf.clear();
    }
    st->hreq = nullptr;   // evhttp 已释放请求对象

    // 请求级 SSL_CTX 释放(HTTPS;SSL 对象持有 ctx 引用,连接即使已回池亦安全;
    // C15 重试路径:下一尝试经 StartRequest 重建)
    if (st->perReqSslCtx)
    {
        SSL_CTX_free(st->perReqSslCtx);
        st->perReqSslCtx = nullptr;
    }

    // ── 阶段②:重试判定(C15)。命中 → 状态重置 + 退避调度,请求延续(不交付)。
    // st 保持挂在 m_inflight —— 退避窗口内 Cancel/CancelAll 仍可命中置 cancelled
    // (取消即止),重试重入 StartRequest 时经其开头检查收口;find 守卫防重复入列 ──
    if (MaybeRetry(st->priv, st, error))
        return;

    // ── 阶段③:最终收口 ──
    st->finished = true;

    // C15 重试退避事件清理(重试已确定不再发生;已触发时 RetryCB 已置空,此处仅清理
    // 仍挂起者 —— 如退避窗口内被总超时/取消收口,防迟到回调访问已删 st)
    if (st->retryEvent)
    {
        event_del(st->retryEvent);
        event_free(st->retryEvent);
        st->retryEvent = nullptr;
    }

    // 总超时事件清理(收口必经路径;总超时回调内已触发时,事件已出队,event_del 为幂等空操作)
    if (st->totalTimeoutEvent)
    {
        event_del(st->totalTimeoutEvent);
        event_free(st->totalTimeoutEvent);
        st->totalTimeoutEvent = nullptr;
    }

    // C13:解压流收尾(EOF 冲刷残留输出 + 释放 z_stream;完整流在 GzipFeed 已见 Z_STREAM_END,无残留)
    GzipFinish(st);

    // 落盘文件收尾(流式模式):主错误为 OK 且 fclose 失败(缓冲刷盘错误)→ 改报 FILE_IO。
    // 连接处置已在阶段①按 error 参数判定,不受影响(连接本身健康,照常归还/释放)
    if (st->outFile)
    {
        bool flushFail = (fclose(st->outFile) != 0);
        st->outFile = nullptr;
        if (flushFail && st->result.m_error == ZM_HTTPC_OK)
        {
            st->result.m_error = ZM_HTTPC_ERR_FILE_IO;
            st->result.m_errorText = "output file flush failed";
        }
    }

    // 从在飞列表移除(后续 Cancel 不再命中;C15:重试路径 st 始终在列,收口才出列)
    auto& inflight = st->priv->m_inflight;
    inflight.erase(std::remove(inflight.begin(), inflight.end(), st), inflight.end());

    if (st->syncPromise)
    {
        auto p = std::unique_ptr<ZmHttpClientResult>(new ZmHttpClientResult(std::move(st->result)));
        st->syncPromise->set_value(std::move(p));   // 同步:唤醒阻塞线程
        st->syncPromise = nullptr;
        delete st;
    }
    else if (st->cb)
    {
        st->cb(&st->result, st->id, st->params);    // 异步:回调在循环线程执行
        st->cb = {};
        delete st;
    }
    else
    {
        delete st;   // 无回调无 promise(罕见):直接释放
    }
}

// ============================================================================
// evhttp 回调(循环线程)
// 回调形态(已核对 2.2.1 源码,libevent 无 EVREQ_HTTP_CONNECT_FAILED 枚举):
//   正常响应            → done_cb(req, response_code != 0)
//   传输层错误          → error_cb(err) 先行,然后 done_cb(NULL) 跟进
//   连接建立失败/超时   → 仅 done_cb(req, response_code == 0)(cleanup 路径,无 error_cb)
//   主动取消            → 仅 error_cb(EVREQ_HTTP_REQUEST_CANCEL)
// ============================================================================

void ZmHttpClientPrivate::ZmHttpClientOnErrorCB(enum evhttp_request_error err, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);

    int code;
    const char* text;
    switch (err)
    {
    case EVREQ_HTTP_TIMEOUT:         code = ZM_HTTPC_ERR_TIMEOUT;      text = "timeout"; break;
    case EVREQ_HTTP_EOF:
        // C11:无 Content-Length 的流式流(SSE 等无限流)EOF = 对端正常结束流 → 收口 OK;
        // 有 Content-Length 未收满 / 头部阶段 EOF → STREAM_BROKEN(判定见 done_cb(NULL))
        // C13:header_cb 中止(如 gzip init 失败,返回 -1 触发本回调)不视为正常结束;
        // 已 init 但未见 Z_STREAM_END(截断 gzip 流)同样不视为正常结束 → STREAM_BROKEN
        if (st->abortError == ZM_HTTPC_OK &&
            (!st->gzipStream || st->gzipDone) &&
            st->IsStreamingConsumer() && st->result.m_response.m_status != 0 &&
            st->contentLengthTotal < 0)
        {
            st->eofOk = true;
        }
        // C18 实测修正:Windows 上连接被拒/握手阶段失败(如 127.0.0.1:9 拒绝连接)
        // 经 error_cb(EVREQ_HTTP_EOF) 先行 + done_cb(NULL) 跟进收口,而非本文件
        // 上方注释所述的"仅 done_cb(response_code==0)"路径 —— 无响应头(status==0)
        // 即连接/握手阶段失败,按 CONNECT 报(与"连不存在端口 → ERR_CONNECT"契约一致;
        // 响应头已到达后的 EOF 仍是流中断语义,不受影响)
        if (st->result.m_response.m_status == 0)
        {
            code = ZM_HTTPC_ERR_CONNECT;
            text = "connect failed";
        }
        else
        {
            code = ZM_HTTPC_ERR_STREAM_BROKEN;
            text = (st->gzipStream && !st->gzipDone) ? "gzip stream truncated" : "connection closed";
        }
        break;
    case EVREQ_HTTP_INVALID_HEADER:  code = ZM_HTTPC_ERR_PARSE;         text = "invalid header"; break;
    case EVREQ_HTTP_BUFFER_ERROR:    code = ZM_HTTPC_ERR_PARSE;         text = "buffer error"; break;
    case EVREQ_HTTP_REQUEST_CANCEL:  code = ZM_HTTPC_ERR_CANCELLED;     text = "cancelled"; break;
    case EVREQ_HTTP_DATA_TOO_LONG:   code = ZM_HTTPC_ERR_PARSE;         text = "data too long"; break;
    default:                         code = ZM_HTTPC_ERR_CONNECT;       text = "http error"; break;
    }
    // 注:连接建立失败/超时经 done_cb(response_code == 0) 路径收口 CONNECT(见 OnDoneCB);
    // 传输层错误(EOF/BUFFER_ERROR)经本回调先行记录,由 done_cb(NULL) 统一收口。

    if (err == EVREQ_HTTP_REQUEST_CANCEL)
    {
        // 取消:无完成回调跟进,立即收口。总超时触发的取消收口 TIMEOUT;chunk_cb 内部中止
        // (写盘失败)触发的取消按 abortError 收口;主动取消收口 CANCELLED
        if (st->timeoutByDeadline)
            FinishRequest(st, ZM_HTTPC_ERR_TIMEOUT, "total timeout");
        else if (st->abortError != ZM_HTTPC_OK)
            FinishRequest(st, st->abortError, st->abortErrorText.c_str());
        else
            FinishRequest(st, code, text);
    }
    else
    {
        // 完成回调将(以 NULL 请求)跟进;先记录错误,由跟进方统一收口(避免悬垂删除)
        // C13:header_cb 已预置 abortError(如 gzip init 失败,经返回 -1 触发本回调)时优先按它收口
        if (st->abortError != ZM_HTTPC_OK)
        {
            st->pendingError = st->abortError;
            st->pendingErrorText = st->abortErrorText;
        }
        else
        {
            st->pendingError = code;
            st->pendingErrorText = text ? text : "";
        }
    }
}

void ZmHttpClientPrivate::ZmHttpClientOnDoneCB(struct evhttp_request* hreq, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);

    if (!hreq)
    {
        // C11:无 Content-Length 流式流的 EOF 经 eofOk 收口 OK(正常流结束;error_cb 已判定)
        if (st->eofOk)
        {
            FinishRequest(st, ZM_HTTPC_OK, nullptr);
            return;
        }
        // 传输层错误:error_cb 已先行记录(防御:未记录时按连接失败处理)
        if (st->pendingError == ZM_HTTPC_OK)
        {
            st->pendingError = ZM_HTTPC_ERR_CONNECT;
            st->pendingErrorText = "transport error";
        }
        FinishRequest(st, st->pendingError, st->pendingErrorText.c_str());
        return;
    }

    if (evhttp_request_get_response_code(hreq) == 0)
    {
        // 连接建立失败/超时路径(cleanup,无 error_cb;错误细化在 C7;
        // 连接在回调返回后仍被 libevent 引用 → 延后释放)
        FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "connect failed", true /* deferConFree */);
        return;
    }

    // ── 重定向跟随(C9):301/302/303/307/308 + FollowRedirect + Location 有效 → 重新发起。
    // 插入点在响应组装之前:重定向响应不回调数据回调/不组装,直接丢弃;
    // 总超时 deadline 保持原定时(StartRequest 防重建),覆盖整条重定向链。
    // 重定向跟随:SSE(无限流)不跟随(SSE 端点重定向罕见;FollowRedirect 对 SSE 请求忽略,
    // 按 3xx 普通响应收口 —— header_cb 置 non2xxResponse,体累积移交 m_body);
    // OnDataChunk/OutputFile 流式下载保持跟随(CDN/签名 URL 的 302 流程)。
    int code = evhttp_request_get_response_code(hreq);
    // ── C14 cookie jar:Set-Cookie 存储(全量路径;流式请求已在 header_cb 存储)。
    // 挂点须在重定向早退之前(C13 审查登记):重定向链中间响应的 Set-Cookie 同样存储;
    // 非 2xx 响应也存储(Set-Cookie 语义与状态码无关)──
    if (st->req.UseCookieJar() && !st->IsStreamingConsumer())
    {
        struct evkeyvalq* hdrs = evhttp_request_get_input_headers(hreq);
        if (hdrs)
        {
            std::string pathDir = CookiePathDir(st->path);
            for (struct evkeyval* h = hdrs->tqh_first; h; h = h->next.tqe_next)
                if (_stricmp(h->key, "Set-Cookie") == 0)
                    CookieStore(st->priv, st->host, pathDir, h->value);
        }
    }
    // C12:文件/流式上传不跟随重定向(上传重定向语义复杂:307/308 保留体需泵在新连接
    // 重启、302/303 转 GET 丢体;交由调用方直接处理 3xx)。内存体上传保持现有行为
    // (307/308 保留体重发 —— 体在 st->req 中,StartRequest 重发即可)
    bool redirectFollow = st->req.FollowRedirect() && !st->req.OnSseEvent() &&
                          !st->req.UploadChunk() && st->req.BodyFile().empty();
    if (redirectFollow &&
        (code == 301 || code == 302 || code == 303 || code == 307 || code == 308))
    {
        if (st->attempts - st->retryCount >= st->req.RedirectMax())
        {
            // 超限:不组装响应,直接收口(整条链共发起 RedirectMax 次请求,最多跟随 RedirectMax-1 次
            // 跳转;attempts-retryCount 已达上限仍收到重定向状态 → 错误收口。
            // C15 预算分离:attempts 含重试发起,减 retryCount 后仅计"初始+重定向",
            // 重试不消耗重定向预算 —— 反之重定向亦不消耗重试预算(retryCount 仅 MaybeRetry 递增))
            FinishRequest(st, ZM_HTTPC_ERR_REDIRECT, "redirect limit exceeded");
            return;
        }
        const char* loc = evhttp_find_header(evhttp_request_get_input_headers(hreq), "Location");
        if (loc && *loc)
        {
            std::string newUrl = ResolveRedirectUrl(loc, st->scheme, st->host, st->port);
            if (!newUrl.empty())
            {
                // 302/303(HEAD 除外)与 301 的 POST → GET 并清空请求体(RFC 7231/浏览器语义);
                // 307/308 保留方法/体(重放原请求);301 其他方法亦保留
                st->redirectReq = st->req;
                st->redirectReq.SetUrl(newUrl.c_str());
                // 跨主机重定向:剥离 Authorization/Cookie(防凭据泄漏;浏览器/curl/Go 同行为;
                // origin = scheme+host+port,任一不同即视为跨域,保守剥离)
                std::string ns; std::string nh; uint16_t np = 0; std::string npath;
                if (ParseHttpUrl(newUrl, ns, nh, np, npath) &&
                    (ns != st->scheme || nh != st->host || np != st->port))
                {
                    st->redirectReq.RemoveHeader("Authorization");
                    st->redirectReq.RemoveHeader("Cookie");
                }
                if (((code == 302 || code == 303) && _stricmp(st->redirectReq.Method().c_str(), "HEAD") != 0) ||
                    (code == 301 && _stricmp(st->redirectReq.Method().c_str(), "POST") == 0))
                {
                    st->redirectReq.SetMethod("GET");
                    st->redirectReq.SetBody(nullptr, 0);   // 清空内存体/文件体/流式上传
                }
                st->req = st->redirectReq;   // 整体替换:后续 StartRequest 使用新请求(TLS/证书/超时/回调自动继承)

                // 旧连接不归还池(重定向即新请求;跨主机/跨协议自然新建),直接释放。
                // 安全:本仓库 libevent 的客户端连接无 EVHTTP_CON_AUTOFREE 标记,
                // done_cb 返回后 libevent 仅释放请求对象、不再触碰连接(已核对 http.c)。
                if (st->con) { evhttp_connection_free(st->con); st->con = nullptr; }
                // C16:隧道 SSL 对象(bev 不带 CLOSE_ON_FREE,归属我方)与代理隧道的
                // 原始 bev 均在 con 之后释放(释放顺序与 FinishRequest 阶段①一致 ——
                // 不释放则重定向后旧隧道 fd 泄漏/SSL 泄漏)
                if (st->tunnelSsl) { SSL_free(st->tunnelSsl); st->tunnelSsl = nullptr; }
                if (st->tunnelBev) { bufferevent_free(st->tunnelBev); st->tunnelBev = nullptr; }
                st->hreq = nullptr;   // 旧请求句柄由 libevent 在回调返回后释放
                // 旧 per-req SSL_CTX:连接已释放(其 SSL 随之释放,ctx 引用归零),安全释放防泄漏
                if (st->perReqSslCtx) { SSL_CTX_free(st->perReqSslCtx); st->perReqSslCtx = nullptr; }

                StartRequest(st);   // 重新发起(attempts 于 StartRequest 内递增;cancelled 检查在其开头)
                return;
            }
        }
        // Location 缺失或解析失败 → 按普通响应处理(不跟随)
    }

    // C11:流式请求(2xx)的总超时已前移至响应头到达时取消(header_cb);此处不再处理。
    // 非 2xx 流式响应与全量请求一致:总超时覆盖至收口(FinishRequest 统一清理)。

    // 正常响应:状态码/头/体
    if (!st->IsStreamingConsumer())
    {
        // 全量模式:状态/头在 done 组装(流式模式已在 header_cb 组装,防重复)
        st->result.m_response.m_status = evhttp_request_get_response_code(hreq);
        struct evkeyvalq* hdrs = evhttp_request_get_input_headers(hreq);
        if (hdrs)
        {
            for (struct evkeyval* h = hdrs->tqh_first; h; h = h->next.tqe_next)
                st->result.m_response.m_headers.emplace_back(h->key, h->value);
        }
        // C13:全量模式无 header_cb(增量钩子仅流式请求注册),Content-Encoding 在此识别。
        // gzip/deflate 且请求允许解压(SetGzip,默认开)→ 初始化解压流,体读出后经 GzipFeed 解压;
        // identity/空/未知编码(如 br)→ 不解压,原样透传
        const char* ce = hdrs ? evhttp_find_header(hdrs, "Content-Encoding") : nullptr;
        if (ce && *ce && _stricmp(ce, "identity") != 0 && st->req.Gzip() &&
            (_stricmp(ce, "gzip") == 0 || _stricmp(ce, "deflate") == 0))
        {
            if (!GzipInit(st, ce))
            {
                FinishRequest(st, ZM_HTTPC_ERR_PARSE, "gzip init failed");
                return;
            }
        }
    }
    struct evbuffer* in = evhttp_request_get_input_buffer(hreq);
    size_t len = in ? evbuffer_get_length(in) : 0;
    if (st->IsStreamingConsumer())
    {
        // ── 流式模式(C11):响应体已由 chunk_cb 增量分发(2xx)或累积(非 2xx);此处仅收口 ──
        if (st->non2xxResponse)
        {
            // 非 2xx:按普通响应收口 —— 累积体移交 m_body(不触发数据回调/不落盘;
            // SSE 端点的错误响应(401/500 等)错误详情由此可见)
            // 兜底:chunk_cb 未覆盖的残留数据(库行为与预期不符时)先并入 non2xxBody
            // (再随移交进 m_body;gzip 响应则经解压后并入,产出走 DispatchDecoded 非 2xx 分支)
            while (in && evbuffer_get_length(in) > 0)
            {
                size_t n = evbuffer_get_length(in);
                if (st->gzipStream)
                {
                    std::vector<BYTE> buf(n);
                    evbuffer_remove(in, buf.data(), n);
                    if (!GzipFeed(st, buf.data(), n))
                    {
                        FinishRequest(st, ZM_HTTPC_ERR_PARSE, "gzip inflate failed");
                        return;
                    }
                }
                else
                {
                    size_t old = st->non2xxBody.size();
                    st->non2xxBody.resize(old + n);
                    evbuffer_remove(in, st->non2xxBody.data() + old, n);
                }
            }
            st->result.m_response.m_body = std::move(st->non2xxBody);
            st->result.m_response.m_contentLength = st->result.m_response.m_body.size();
        }
        else
        {
            st->result.m_response.m_contentLength = st->bytesReceived;   // 实际收到字节数
            // 兜底:chunk_cb 未覆盖的残留数据(库行为与预期不符时)按 C10 口径 64KB 分块分发
            // (gzip 响应:压缩残留字节经 GzipFeed 解压后走 DispatchDecoded 公共分发)
            const size_t CHUNK = 64 * 1024;
            while (in && evbuffer_get_length(in) > 0)
            {
                size_t take = evbuffer_get_length(in) > CHUNK ? CHUNK : evbuffer_get_length(in);
                std::vector<BYTE> buf(take);
                evbuffer_remove(in, buf.data(), take);
                if (st->gzipStream)
                {
                    if (!GzipFeed(st, buf.data(), take))
                    {
                        FinishRequest(st, ZM_HTTPC_ERR_PARSE, "gzip inflate failed");
                        return;
                    }
                }
                else if (!st->DispatchData(buf.data(), take))
                {
                    // 写盘失败:中断分发并收口(文件由 FinishRequest 关闭;收口后 st 已释放,不得再访问)
                    FinishRequest(st, ZM_HTTPC_ERR_FILE_IO, "write output file failed");
                    return;
                }
            }
        }
    }
    else
    {
        if (st->gzipStream)
        {
            // 全量模式 + C13:压缩体 64KB 分块喂入解压流,随喂随 evbuffer_remove 释放压缩拷贝
            // (避免整包 pullup 的第三份驻留;产出经 DispatchDecoded 全量分支暂存 gzipFullBody)。
            // GzipFeed 失败 = inflate 错误 → 收口 ERR_PARSE
            const size_t CHUNK = 64 * 1024;
            while (in && evbuffer_get_length(in) > 0)
            {
                size_t take = evbuffer_get_length(in) > CHUNK ? CHUNK : evbuffer_get_length(in);
                std::vector<BYTE> buf(take);
                evbuffer_remove(in, buf.data(), take);
                if (!GzipFeed(st, buf.data(), take))
                {
                    FinishRequest(st, ZM_HTTPC_ERR_PARSE, "gzip inflate failed");
                    return;
                }
            }
            // 解压产出由 DispatchDecoded 暂存于 gzipFullBody,此处移交 m_body(结构非友元不可直写)
            st->result.m_response.m_body = std::move(st->gzipFullBody);
            st->result.m_response.m_contentLength = st->result.m_response.m_body.size();
        }
        else
        {
            // 全量模式:现有逻辑不变
            st->result.m_response.m_body.resize(len);
            if (len && in)
                evbuffer_remove(in, st->result.m_response.m_body.data(), len);
            st->result.m_response.m_contentLength = len;
        }
    }

    FinishRequest(st, ZM_HTTPC_OK, nullptr);
}

// ============================================================================
// C11 真流式:增量钩子(仅流式请求注册;循环线程)
// 核实结论(vendored libevent 2.2.1-alpha-dev,http.c/http.h 源码核对):
//   evhttp_request_set_header_cb —— 响应头完整解析后、读体前触发,客户端
//     (EVHTTP_RESPONSE)路径同样调用(evhttp_read_header);返回 <0 中止连接。
//   evhttp_request_set_chunked_cb —— 增量读取时触发(全部帧形态:chunked 每完成块 /
//     Content-Length / read-until-close 每次读),数据在 req->input_buffer,回调返回后
//     libevent 自动清空(evhttp_read_body / evhttp_handle_chunked_read);DEFER_FREE
//     机制下回调内可安全 evhttp_cancel_request(不悬垂)。
//   done_cb 仍是唯一完成钩子:无限流(无 Content-Length / 无结束块)不触发,流式结束
//     经取消 / 对端关闭(EOF)触发 —— 故 C10 的"done_cb 整包分块"无法承载 SSE,本任务
//     以 header_cb + chunk_cb 实现"响应头到达即接管、增量推送"。手动 bev 路径(C11 预案)
//     因上述钩子存在而不需要,普通请求路径不注册钩子,行为零变化。
// ============================================================================

// header_cb:响应头完整到达 —— 组装状态/头(取代 done_cb 组装),2xx → 流式接管(取消
// 总超时,此后由读写超时(connectTimeout)接管空闲保护);非 2xx → 按普通响应收口(体累积,
// 总超时保持至收口)。只记录状态、不触发收口(evhttp 在读头回调栈内,收口须经完成/错误回调)。
int ZmHttpClientPrivate::ZmHttpClientOnHeaderCB(struct evhttp_request* hreq, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);

    int code = evhttp_request_get_response_code(hreq);
    if (code == 100)
        return 0;   // 100-continue:等待最终响应头(最终头到达时再次进入,幂等)

    // 每个新响应(重定向链 302 → 最终 200 等)重置流式状态,防链上中间响应状态泄漏
    st->streamingBody = false;
    st->non2xxResponse = false;
    st->non2xxBody.clear();
    // C13:逐响应重置 abortError(防御未来路径;当前所有置位路径均终止请求,无可达残留)
    st->abortError = ZM_HTTPC_OK;
    st->abortErrorText.clear();
    // C13:上个响应的解压流作废(重定向链中间响应的 Content-Encoding 不延续;新响应按需重建)
    GzipDestroyStream(st->gzipStream);
    st->gzipDone = false;

    st->result.m_response.m_status = code;
    st->result.m_response.m_headers.clear();   // 重入防重复
    struct evkeyvalq* hdrs = evhttp_request_get_input_headers(hreq);
    if (hdrs)
    {
        // C14:Set-Cookie 存储与头组装同循环 —— Set-Cookie 可多个,须遍历取全部同名项
        // (evhttp_find_header 只返回首个);挂点在重定向早退之前(done_cb 的重定向分支
        // 在其后,已覆盖;C13 审查登记);非 2xx 响应亦存储(语义与状态码无关)
        const bool jarOn = st->req.UseCookieJar();
        const std::string pathDir = CookiePathDir(st->path);   // Path 缺省时的 default-path
        for (struct evkeyval* h = hdrs->tqh_first; h; h = h->next.tqe_next)
        {
            st->result.m_response.m_headers.emplace_back(h->key, h->value);
            if (jarOn && _stricmp(h->key, "Set-Cookie") == 0)
                CookieStore(st->priv, st->host, pathDir, h->value);
        }
    }
    const char* cl = hdrs ? evhttp_find_header(hdrs, "Content-Length") : nullptr;
    st->contentLengthTotal = (cl && *cl) ? atoll(cl) : -1;   // 无 CL = -1(进度 total 未知)

    // C13:Content-Encoding 识别 —— gzip/deflate 且请求允许解压(SetGzip,默认开)时初始化
    // 解压流,此后 chunk_cb 增量字节先 inflate 再分发(挂载点即 C11 注释预定位置);
    // identity/空/未知编码(如 br)→ 不解压,原样透传
    const char* ce = hdrs ? evhttp_find_header(hdrs, "Content-Encoding") : nullptr;
    if (ce && *ce && _stricmp(ce, "identity") != 0 && st->req.Gzip() &&
        (_stricmp(ce, "gzip") == 0 || _stricmp(ce, "deflate") == 0))
    {
        if (!GzipInit(st, ce))
        {
            // init 失败(仅 inflateInit2 内存错误):中止请求。header_cb 内不可取消
            // (取消即释放连接,而 evhttp_read_header 返回后仍会用连接读体,已核对 http.c)
            // —— 预置 abortError 后返回 -1,libevent 以 EOF 失败连接,error_cb 按 abortError 收口
            st->abortError = ZM_HTTPC_ERR_PARSE;
            st->abortErrorText = "gzip init failed";
            return -1;
        }
        // gzip 解压时:Content-Length 是压缩字节数,与解压后 received 不可比,进度 >100% 是错误
        // 语义 → total 置 -1(未知)。进度口径:received = 解压后字节,total = -1 表示未知。
        // (未知编码如 br 不解压,received = 线上字节,与 CL 可比,保持原值)
        st->contentLengthTotal = -1;
    }

    if (code >= 200 && code < 300)
    {
        // 2xx:流式接管 —— 取消总超时(响应头到达即接管;与 ZmReqLoop::CancelDeadline 语义对齐)
        st->streamingBody = true;
        if (st->totalTimeoutEvent)
        {
            event_del(st->totalTimeoutEvent);
            event_free(st->totalTimeoutEvent);
            st->totalTimeoutEvent = nullptr;   // FinishRequest 不再清理
        }
    }
    else
    {
        st->non2xxResponse = true;   // 非 2xx(含流式请求的 3xx):按普通响应收口
    }
    return 0;   // <0 会中止连接(本实现不需要)
}

// chunk_cb:响应体增量 —— 2xx 分发(SSE 帧解析 → OnSseEvent 逐事件;OnDataChunk/OutputFile
// → DispatchData 64KB 分块回调/落盘/进度);非 2xx 累积(non2xxBody,收口移交 m_body)。
void ZmHttpClientPrivate::ZmHttpClientOnChunkCB(struct evhttp_request* hreq, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);

    struct evbuffer* in = evhttp_request_get_input_buffer(hreq);
    size_t len = in ? evbuffer_get_length(in) : 0;
    if (len == 0)
        return;

    const BYTE* p = evbuffer_pullup(in, len);
    if (!p)
        return;   // 线性化失败(内存错误):放弃本块(回调后 libevent 清空,无残留)

    if (!st->streamingBody)
    {
        // 非 2xx:累积,收口移交 m_body(不触发数据回调/不落盘)
        if (st->gzipStream)
        {
            // C13:非 2xx 的 gzip 响应体同样解压(产出经 DispatchDecoded 并入 non2xxBody)
            if (!GzipFeed(st, p, len))
            {
                if (st->hreq)
                    evhttp_cancel_request(st->hreq);   // → error_cb(CANCEL) 按 abortError 收口
                return;
            }
        }
        else
        {
            st->non2xxBody.insert(st->non2xxBody.end(), p, p + len);
        }
        return;
    }

    if (st->gzipStream)
    {
        // C13:解压挂载点(chunk_cb 内、消费分发前;产出经 DispatchDecoded 公共分流
        // SSE→SseFeed / OnDataChunk+落盘→DispatchData —— SSE_MAX_FRAME 上限作用于解压后)
        if (!GzipFeed(st, p, len))
        {
            // inflate 错误/分发失败:abortError 已置 → 取消流(error_cb(CANCEL) 按它收口;
            // chunk_cb 内取消安全:libevent DEFER_FREE 机制,已核对 http.c)
            if (st->hreq)
                evhttp_cancel_request(st->hreq);
            return;
        }
        return;
    }

    if (st->req.OnSseEvent())
    {
        if (!st->SseFeed(p, len))   // SSE:帧解析 → OnSseEvent(逐事件)
        {
            // 帧缓冲超上限(帧损坏):中止流(取消 → error_cb(CANCEL) 按 abortError 收口)
            st->abortError = ZM_HTTPC_ERR_PARSE;
            st->abortErrorText = "sse frame too large";
            if (st->hreq)
                evhttp_cancel_request(st->hreq);
            return;
        }
    }

    if (st->req.OnDataChunk() || !st->req.OutputFile().empty())
    {
        // 原始字节分发:64KB 分块(与 C10 口径一致)→ OnDataChunk 回调/落盘/进度
        const size_t CHUNK = 64 * 1024;
        size_t off = 0;
        while (off < len)
        {
            size_t take = (len - off > CHUNK) ? CHUNK : (len - off);
            if (!st->DispatchData(p + off, take))
            {
                // 写盘失败:中止流(取消 → error_cb(CANCEL) 按 abortError 收口;
                // chunk_cb 内取消安全:libevent DEFER_FREE 机制,已核对 http.c)
                st->abortError = ZM_HTTPC_ERR_FILE_IO;
                st->abortErrorText = "write output file failed";
                if (st->hreq)
                    evhttp_cancel_request(st->hreq);
                return;
            }
            off += take;
        }
    }
    else if (st->req.OnSseEvent())
    {
        // SSE 仅消费(无数据回调/落盘):计数与进度(总超时已取消;total 无 Content-Length 为 -1)
        st->bytesReceived += len;
        if (st->req.Progress())
            st->req.Progress()((int64_t)st->bytesReceived, st->contentLengthTotal);
    }
}

// ============================================================================
// C12 流式上传(chunked)泵:evbuffer 输出回调驱动的手动 chunk 分帧
//
// 第 0 步核实结论(vendored libevent 2.2.1-alpha-dev = 上游 master 2026-06-16,
// http.c / http.h / bufferevent_sock.c / buffer.c 源码核对):
//   1) 客户端请求路径无原生 chunked 上传 —— 无 evhttp_request_set_chunked(公共头
//      仅 evhttp_request_set_chunked_cb = 响应体增量读回调);chunk 分帧 API
//      (evhttp_send_reply_start/chunk/end)仅服务端路径存在(req->chunked 仅在服务端
//      evhttp_send_reply_start 置位);
//   2) 客户端写路径(evhttp_make_request → dispatch → evhttp_make_header →
//      evhttp_write_buffer)对 POST/PUT 自动追加 "Content-Length: 0"
//      (make_header_request 仅检查 Content-Length 头是否存在,不检查
//      Transfer-Encoding;值为输出缓冲长度 = 0) —— 设 TE 头不能阻止,须从线路清除;
//   3) evhttp_write_buffer 只启 EV_WRITE 由 bev 写回调 flush,无任何分帧;
//   4) bev 输出缓冲每次变更内联触发注册的回调(bev 缓冲未启用延迟回调队列)。
// 故按计划采用"手动泵",但泵的驱动机制与计划差异(实现修正):
//   计划原以 loop 定时事件驱动,核实发现无法保证正确性 ——
//   a) 单轮循环内 fd 事件先于超时事件出队(event.c:evsel->dispatch 先收 fd 事件、
//      timeout_process 随后),头部写入后的首次 flush 必然先于定时泵触发,泵无法在
//      "头部已写入输出缓冲但尚未 flush"的窗口内清除自动 Content-Length;
//   b) 泵的背压延迟窗口内输出缓冲可能被对端完全消费 → bev 写回调触发
//      evhttp_write_connectioncb 见空缓冲即转入 EVCON_READING 并 disable EV_WRITE
//      (bev 输出回调仅在 EV_WRITE 启用时重新挂写事件),泵后续写入永久滞留 → 死锁。
// 本实现以 evbuffer 输出缓冲回调为泵(缓冲变更点内联执行,不经事件队列):
//   * 阶段 1:头部块完整写入时(以 "\r\n\r\n" 判定;evhttp 头部值禁 CR/LF 无误配)
//     内联清除自动 "Content-Length: 0\r\n"(它是 evhttp 追加的最后一行头部);
//   * 阶段 2:对端消费(evbuffer_drain → 回调,先于 bev 用户写回调
//     evhttp_write_connectioncb 的空缓冲判定)后补一块 —— 输出缓冲不空转,
//     evhttp 不会提前转 READING,终止块前的任何时刻都安全;
//   * 背压:仅当缓冲长度低于一块(64KB)时补块,内存有界(≤ 2 块),无需定时器;
//   * 全部在循环线程执行(bev 写路径内联),与现有回调并发模型一致。
//   * 回调执行位置:UploadChunk/Progress 在 bev 输出缓冲变更点内联执行(深度栈,
//     可能位于 socket/SSL 写路径内);回调内同步重入本客户端会死锁循环线程,
//     阻塞回调会卡住同客户端全部请求 —— 与 OnDataChunk 同级,勿在回调中做重活。
// ============================================================================

static void ZmHttpClientUploadOutCB(struct evbuffer* buf, const struct evbuffer_cb_info* info, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);
    if (st->cancelled || st->finished)
        return;

    // ── 阶段 1(一次性):等头部块完整,清除自动 Content-Length: 0 ──
    if (!st->uploadHeaderReady)
    {
        // 头部块以 "\r\n\r\n" 结束(evhttp 头部值禁 CR/LF,无误配;最终 "\r\n" 在
        // make_header 的最后一次追加中写入)。本回调在该次追加时触发,此刻缓冲
        // 恰好 = 整个头部块(输出缓冲为空 → 自动 CL 为 0;chunked 体不写 output_buffer,
        // 后续数据仅经本泵追加)。
        struct evbuffer_ptr end = evbuffer_search_range(buf, "\r\n\r\n", 4, nullptr, nullptr);
        if (end.pos < 0)
            return;   // 头部未完整:等待下一次缓冲变更
        // 匹配以 "\r\n" 锚定行首(21 字节;自动 CL 恒为最后一行 —— make_header_request
        // 在全部用户头之后追加),避免用户头值含 "Content-Length: 0" 子串
        // (如 X-Content-Length: 0)时误剔
        struct evbuffer_ptr cl = evbuffer_search_range(buf, "\r\nContent-Length: 0\r\n", 21, nullptr, nullptr);
        const size_t hdrLen = (size_t)end.pos + 4;
        if (cl.pos >= 0 && (size_t)cl.pos + 21 <= hdrLen && hdrLen == evbuffer_get_length(buf))
        {
            // 移除 CL 行:整个头部块移出 → 从内存副本剔除 CL 行 → 整体放回(放回
            // 触发回调重入:阶段 1 完成,阶段 2 需 n_deleted>0 不触发,无副作用)。
            // 前部冻结:仅 socket bev 创建时 evbuffer_freeze(buf,1)(写路径每次写前
            // 解冻/写后重冻结,不依赖先前状态);SSL bev 输出端从未被冻结
            // (bufferevent_ssl.c 零 freeze 调用)。此处解冻后【不】重冻结 ——
            // 重冻结会令 SSL 写路径的 evbuffer_drain 因 freeze_start 返回 -1,
            // 已写出字节永不排空 → 头部块被反复 SSL_write(线上垃圾)
            std::vector<char> head(hdrLen);
            evbuffer_unfreeze(buf, 1);
            evbuffer_remove(buf, head.data(), hdrLen);
            size_t clPos = (size_t)cl.pos + 1;   // 锚定符 "\r\n" 之后即 CL 行本体(19 字节)
            head.erase(head.begin() + clPos, head.begin() + clPos + 19);
            evbuffer_add(buf, head.data(), head.size());
        }
        st->uploadHeaderReady = true;
        // 首块不在此补入:头部写入是"追加"而非"消费",统一由阶段 2 的首次消费驱动,
        // 路径单一(字节序天然正确:头部先于一切块数据)
    }

    // ── 阶段 2:对端消费后补一块(泵)──
    if (st->uploadDone)
        return;
    if (info->n_deleted == 0)
        return;   // 仅水位下降(对端消费)后补块;自身追加触发的回调(纯 n_added)不补
    if (evbuffer_get_length(buf) >= (size_t)ZmHttpClientReqState::UPLOAD_CHUNK_SIZE)
        return;   // 上一块未被消费完:背压(内存有界 ≤ 2 块)

    // 拉取一块(64KB;流式回调返回空串 = 结束)
    std::string chunk;
    if (st->req.UploadChunk())
        chunk = st->req.UploadChunk()();
    else if (st->uploadFile)
    {
        char raw[ZmHttpClientReqState::UPLOAD_CHUNK_SIZE];
        size_t n = fread(raw, 1, sizeof(raw), st->uploadFile);
        if (n > 0)
            chunk.assign(raw, n);
        if (n == 0 && ferror(st->uploadFile))
        {
            // 文件读取失败:中止上传(取消 → error_cb(CANCEL) 按 abortError 收口)。
            // 本回调处于 bev 写路径内,直接取消的安全依据(已核对):bev 写回调持
            // bev 引用;evhttp_cancel_request → connection_fail_ 先释放请求、reset
            // 连接(清空输出缓冲 → 本回调重入,此时 cancelled 已置位即返回)、最后
            // 才触发 error_cb(收口删除 st) —— 取消调用后不得再访问 st,与
            // chunk_cb 写盘失败的中止模式一致。循环投递的取消/超时(PostCancel/
            // 总超时)在输出缓冲处于冻结状态下 reset(evhttp_connection_reset_hard_
            // 的 evbuffer_drain 因 freeze_start 失败)时:Release 构建 EVUTIL_ASSERT
            // 为 no-op,静默跳过(残留字节随连接释放),Debug 构建可能断言 ——
            // 项目仅 Release 配置,可接受;C18 覆盖上传中取消用例
            st->cancelled = true;
            st->abortError = ZM_HTTPC_ERR_FILE_IO;
            st->abortErrorText = "read upload file failed";
            if (st->hreq)
                evhttp_cancel_request(st->hreq);
            return;   // st 已收口删除,不得再访问
        }
    }

    if (chunk.empty())
    {
        // 终止块:chunked 结束标记;此后缓冲被消费空时 evhttp_write_connectioncb
        // 转入 EVCON_READING,响应由现有 header_cb/chunk_cb/done_cb 处理
        evbuffer_add(buf, "0\r\n\r\n", 5);
        st->uploadDone = true;
        if (st->req.Progress())
            st->req.Progress()((int64_t)st->bytesSent, -1);   // 最后一次回调(总字节数,total=-1)
        return;
    }

    // chunk 帧:"<十六进制长度>\r\n<数据>\r\n"
    char head[32];
    int hl = evutil_snprintf(head, sizeof(head), "%zx\r\n", chunk.size());
    evbuffer_add(buf, head, (size_t)hl);
    evbuffer_add(buf, chunk.data(), chunk.size());
    evbuffer_add(buf, "\r\n", 2);

    st->bytesSent += chunk.size();
    if (st->req.Progress())
        st->req.Progress()((int64_t)st->bytesSent, -1);
}

// ============================================================================
// C16 代理(HTTP absolute-form + HTTPS CONNECT 隧道)
//
// 第 0 步核实结论(vendored libevent 2.2.1-alpha-dev,http.c / bufferevent_sock.c /
// bufferevent_openssl.c / bufferevent_ssl.c 源码核对):
//   1) evhttp_make_request 对 uri 原样 strdup(http.c:2948),evhttp_make_header_request
//      把 req->uri 原样写入请求行(http.c:522-524,"%s %s HTTP/%d.%d\r\n"),客户端
//      路径对 uri 不做任何解析 —— 传完整绝对 URL 即天然发出 absolute-form 请求行
//      ("GET http://host/path HTTP/1.1"),HTTP 代理路径无需手动拼请求行;
//   2) 本库 libevent 客户端不自动加 Host 头(make_header_request 仅追加
//      Content-Length),RFC 7230 §5.4 要求 absolute-form 请求的 Host 头为目标主机
//      —— 代理路径由 DispatchOnConnection 的 hostHeader 参数补发(用户显式设置尊重);
//   3) HTTPS 隧道接管:evhttp_connection_base_bufferevent_new + make_request 会再次
//      bufferevent_socket_connect_hostname(evhttp_connection_connect_,http.c:2913),
//      对已连接 fd 的 connect 返回 EISCONN(evutil_socket_connect_,evutil.c:716)
//      → 连接失败 —— 必须用 evhttp_connection_base_bufferevent_reuse_new
//      (http.c:2603,置连接状态 EVCON_IDLE → make_request 直接 dispatch 不再 connect);
//   4) 隧道建立时代理可能把 200 响应与目标首包 TLS 数据(ServerHello)同段转发:
//      SSL bev 用 bufferevent_openssl_filter_new(底层 = 隧道 bev,BIO 经底层 bev
//      输入缓冲取数,残留字节可注入)而非 openssl_socket_new(直读 fd,残留字节丢失
//      即握手卡死;已核对 bufferevent_openssl.c be_openssl_bio_set_fd 两形态差异);
//   5) filter 形态下 evhttp_connection_set_timeout 对 SSL bev 的读写超时无效
//      (be_ssl_adj_timeouts → generic → SSL bev 自身事件未挂 fd,event_add 失败,
//      已核对 bufferevent_ssl.c)—— 超时在 CONNECT 阶段直设于隧道 bev(socket bev,
//      读/写事件携带超时);隧道 bev 超时经 be_ssl_eventcb(BEV_EVENT_TIMEOUT)透传回
//      SSL bev 用户事件回调 → evhttp_error_cb → EVREQ_HTTP_TIMEOUT,与直连同语义
//      (口径:ConnectTimeout 同时作用于连接与读写);
//   6) 引用计数模型(评审修正):SSL filter bev 不带 BEV_OPT_CLOSE_ON_FREE —— 带该
//      标志时底层被 decref 两次(be_ssl_unlink 同步一次 + bufferevent_finalize_cb_
//      一次),再加阶段①的释放会在引用归零后再次 decref(Debug 构建断言退出,已核对
//      bufferevent.c:718 / finalize_cb_ 的 underlying decref / be_ssl_unlink);
//      不带时 be_ssl_unlink 仅摘底层回调,底层引用由 finalize 释放一次(2→1),
//      阶段①释放我方引用(1→0 → fd 恰关一次)。连带:be_ssl_destruct 不释放 SSL 对象
//      (SSL_context_free 按标志门控)→ SSL 归属我方(st->tunnelSsl,阶段① SSL_free);
//   7) 代理连接不回池:池 key 按目标主机组织,代理连接入库会错配给后续直连请求
//      (FinishRequest 阶段①按 req.HasProxy() 排除);重试(C15)/重定向(C9)对代理
//      请求同样适用 —— 均重新进入 StartRequest 按代理路径发起。
// ============================================================================

// CONNECT 响应头上限(恶意/损坏代理无限响应头防内存膨胀)
static constexpr size_t ZM_HTTPC_PROXY_CONNECT_MAX_RESP = 64 * 1024;

// HTTPS SSL_CTX 获取(直连/代理隧道共用;循环线程):客户端级懒构建/脏重建(配置变更
// 时顺带清空 https 空闲池 —— 旧身份连接全部作废),请求级证书覆盖时按请求临时构建
// (挂 st->perReqSslCtx,收口释放)。返回 nullptr = 构建失败(调用方按 ERR_SSL 收口)。
SSL_CTX* ZmHttpClientPrivate::HttpGetUseCtx(ZmHttpClientPrivate* priv,
                                            ZmHttpClientReqState* st, bool usePerReqCert)
{
    // 客户端级 SSL_CTX 懒构建/重建(循环线程独占;配置变更经 SetClientCert/SetVerifyMode 置脏)
    if (!priv->m_sslCtx || priv->m_sslCtxDirty)
    {
        if (priv->m_sslCtx) SSL_CTX_free(priv->m_sslCtx);
        priv->m_sslCtx = BuildClientSSLCTX(priv->m_verifyPeer, priv->m_caFile.c_str(),
                                           priv->m_clientCertFile, priv->m_clientKeyFile);
        priv->m_sslCtxDirty = false;
        // 配置变更:https 空闲连接以旧 SSL_CTX 建立身份,全部作废(逐条释放,同一轮次内完成)
        for (auto it = priv->m_idleConns.begin(); it != priv->m_idleConns.end(); )
        {
            if (it->first.compare(0, 6, "https:") == 0)
            {
                for (auto* c : it->second)
                    evhttp_connection_free(c);
                it = priv->m_idleConns.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    // 请求级证书覆盖:按请求临时构建(请求收口时释放)
    SSL_CTX* useCtx = priv->m_sslCtx;
    if (usePerReqCert)
    {
        useCtx = BuildClientSSLCTX(priv->m_verifyPeer, priv->m_caFile.c_str(),
                                   st->req.ClientCertFile(), st->req.ClientKeyFile());
        st->perReqSslCtx = useCtx;   // 收口时 SSL_CTX_free(SSL 持有引用;连接不回池)
    }
    return useCtx;
}

// 前置声明:在已就绪连接上组装请求并发出(直连/HTTP 代理/HTTPS 隧道共用;定义见下)
static void DispatchOnConnection(ZmHttpClientReqState* st, struct evhttp_connection* con,
                                 const char* requestUri, const std::string* hostHeader);

// 目标主机 Host 头值(host[:port],非默认端口带端口;IPv6 字面量回括)—— C16 代理路径用
static std::string HttpTargetHostHeader(ZmHttpClientReqState* st)
{
    std::string h = st->host;
    if (st->host.find(':') != std::string::npos)
        h = "[" + st->host + "]";
    if (!((st->scheme == "http" && st->port == 80) || (st->scheme == "https" && st->port == 443)))
        h += ":" + std::to_string(st->port);
    return h;
}

// ── CONNECT 阶段回调(循环线程;隧道 bev 专属 —— 隧道建立后其回调被 SSL bev 覆盖,
//    此后不再触发;st 失效前 bufferevent_free 已置空回调,无悬垂)──

// 事件:CONNECTED → 发送 "CONNECT host:port HTTP/1.1"(Host 头 = 目标)并开读;
// TIMEOUT → 收口 CONNECT_TIMEOUT;EOF/ERROR → 收口 CONNECT(连接代理失败)
static void ZmHttpClientTunnelEventCB(struct bufferevent* bev, short what, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);
    if (st->cancelled || st->finished)
        return;

    if (what & BEV_EVENT_CONNECTED)
    {
        // CONNECT 行恒带端口(RFC 7231 §4.3.6:authority-form = host:port,严格代理
        // 要求;仅此行 —— Host 头省略默认端口合法);IPv6 字面量回括
        std::string target = st->host;
        if (st->host.find(':') != std::string::npos)
            target = "[" + st->host + "]";
        std::string connectReq = "CONNECT " + target + ":" + std::to_string(st->port) +
                                 " HTTP/1.1\r\nHost: " + HttpTargetHostHeader(st) + "\r\n\r\n";
        bufferevent_write(bev, connectReq.data(), connectReq.size());
        bufferevent_enable(bev, EV_READ);
        return;
    }
    if (what & BEV_EVENT_TIMEOUT)
    {
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT_TIMEOUT, "proxy connect timeout");
        return;
    }
    // EOF / 错误:连接代理失败(拒绝连接/中途断开)
    ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "proxy connect failed");
}

// 读回调:累积 CONNECT 响应头,解析到 "\r\n\r\n":
//   2xx → 隧道建立 → TLS 接管(SSL filter bev + reuse_new)→ DispatchOnConnection;
//   非 2xx → 代理拒绝(确定性失败,不重试)→ 收口 ERR_PROXY;
//   超上限 → 收口 ERR_PROXY。
// 隧道建立后本回调不再触发(SSL bev 已覆盖底层回调),残留 TLS 字节注入底层 bev 输入。
static void ZmHttpClientTunnelReadCB(struct bufferevent* bev, void* arg)
{
    auto* st = static_cast<ZmHttpClientReqState*>(arg);
    if (st->cancelled || st->finished)
        return;

    struct evbuffer* in = bufferevent_get_input(bev);
    size_t len = evbuffer_get_length(in);
    if (len == 0)
        return;
    const char* p = (const char*)evbuffer_pullup(in, len);
    if (!p)
        return;   // 线性化失败(内存错误):放弃本块,下轮数据到达再处理(无残留)

    if (st->tunnelBuf.size() + len > ZM_HTTPC_PROXY_CONNECT_MAX_RESP)
    {
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_PROXY, "proxy connect response too large");
        return;
    }
    st->tunnelBuf.append(p, len);
    evbuffer_drain(in, len);

    size_t hdrEnd = st->tunnelBuf.find("\r\n\r\n");
    if (hdrEnd == std::string::npos)
        return;   // 响应头未完整:等待更多数据

    // 状态行 "HTTP/x.y <code> ...":首个空格后即状态码
    size_t lineEnd = st->tunnelBuf.find("\r\n");
    int code = 0;
    if (lineEnd != std::string::npos)
    {
        std::string line = st->tunnelBuf.substr(0, lineEnd);
        const char* sp = strchr(line.c_str(), ' ');
        if (sp)
            code = atoi(sp + 1);
    }
    if (code < 200 || code >= 300)
    {
        char text[96];
        evutil_snprintf(text, sizeof(text), "proxy connect rejected (status %d)", code);
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_PROXY, text);
        return;
    }

    // ── 隧道建立(2xx):TLS 接管,后续请求流程与直连共用 ──
    // 1) 残留字节:代理与目标首包 TLS 数据(ServerHello 等)同段转发时,头部结束标记
    //    之后的字节即 TLS 握手数据 —— 注入隧道 bev 输入缓冲(SSL filter bev 经 BIO
    //    从底层 bev 输入取数,可正确消费;见上方第 0 步结论 4)
    if (hdrEnd + 4 < st->tunnelBuf.size())
    {
        // 注:socket bev 输入缓冲为 freeze_end 冻结(bufferevent_sock.c:375,读回调
        // 每次读前解冻/读后重冻结),直接 evbuffer_add 会失败 —— 先解冻(不重冻结,
        // 与 C12 泵同款处理;后续读回调自会重冻结)
        struct evbuffer* bin = bufferevent_get_input(st->tunnelBev);
        evbuffer_unfreeze(bin, 0);
        evbuffer_add(bin, st->tunnelBuf.data() + hdrEnd + 4, st->tunnelBuf.size() - hdrEnd - 4);
    }
    st->tunnelBuf.clear();
    bufferevent_disable(st->tunnelBev, EV_READ | EV_WRITE);   // 底层回调即将移交给 SSL bev

    // 2) SSL_CTX(客户端级/请求级,同直连路径)
    const bool usePerReqCert = !st->req.ClientCertFile().empty() && !st->req.ClientKeyFile().empty();
    SSL_CTX* useCtx = ZmHttpClientPrivate::HttpGetUseCtx(st->priv, st, usePerReqCert);
    if (!useCtx)
    {
        st->perReqSslCtx = nullptr;
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_SSL, "ssl context build failed");
        return;
    }
    // 3) SSL 对象 + SNI(IP 字面量不发送 SNI,RFC 6066;SNI = 目标主机,非代理)
    SSL* ssl = SSL_new(useCtx);
    if (!ssl)
    {
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_SSL, "ssl object create failed");
        return;
    }
    if (st->host.find(':') == std::string::npos && !isdigit((unsigned char)st->host[0]))
        SSL_set_tlsext_host_name(ssl, st->host.c_str());
    // 4) SSL filter bev:底层 = 隧道 bev(内部 incref)。【不带 BEV_OPT_CLOSE_ON_FREE】
    //    —— 带该标志时底层引用被 decref 两次(be_ssl_unlink 同步一次 +
    //    bufferevent_finalize_cb_ 一次),再加阶段①的释放 = 引用计数击穿(Debug 构建
    //    EVUTIL_ASSERT(refcnt>0) 退出,已核对 bufferevent.c:718 / bufferevent.c
    //    finalize_cb_ 的 underlying decref / bufferevent_ssl.c be_ssl_unlink);
    //    不带时 be_ssl_unlink 走 else 分支仅摘底层回调,finalize 对底层 decref 一次
    //    (2→1),阶段①释放我方引用(1→0 → fd 恰关一次)。
    //    连带影响:be_ssl_destruct 不释放 SSL 对象(SSL_context_free 按 CLOSE_ON_FREE
    //    门控,已核对 bufferevent_openssl.c)→ SSL 对象归属我方(st->tunnelSsl,
    //    阶段①在 con 释放后 SSL_free)
    struct bufferevent* sslBev = bufferevent_openssl_filter_new(
        st->priv->LoopBase(), st->tunnelBev, ssl, BUFFEREVENT_SSL_CONNECTING,
        BEV_OPT_DEFER_CALLBACKS);
    if (!sslBev)
    {
        // bev 创建失败:不带 CLOSE_ON_FREE,libevent 未释放 ssl —— 我方释放
        SSL_free(ssl);
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_SSL, "ssl bufferevent create failed");
        return;
    }
    st->tunnelSsl = ssl;   // SSL 对象归属我方(bev 不释放它;阶段①收口时 SSL_free)
    bufferevent_openssl_set_allow_dirty_shutdown(sslBev, 1);   // 对端不送 close_notify 按干净 EOF 处理
    // 5) 已连连接接管:reuse_new 置 EVCON_IDLE → make_request 直接 dispatch,不再
    //    connect(已连 fd 会 EISCONN;见上方第 0 步结论 3)
    struct evhttp_connection* con = evhttp_connection_base_bufferevent_reuse_new(
        st->priv->LoopBase(), st->priv->LoopDnsBase(), sslBev);
    if (!con)
    {
        // 不带 CLOSE_ON_FREE:SSL bev 释放对底层仅 decref(经 finalize),不释放 ssl;
        // fd 仍归隧道 bev(我方引用在 FinishRequest 阶段①释放) —— 此处释放 SSL bev
        // 与我方持有的 SSL 对象
        bufferevent_free(sslBev);
        SSL_free(st->tunnelSsl);
        st->tunnelSsl = nullptr;
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_SSL, "ssl connection create failed");
        return;
    }
    // 6) 请求组装与发出:目标 = 隧道内 origin-form;Host 头 = 目标主机(与 CONNECT 一致)
    std::string hostHdr = HttpTargetHostHeader(st);
    DispatchOnConnection(st, con, st->path.c_str(), &hostHdr);
}

// C16 HTTP 代理路径:连接代理 + absolute-form 请求行(见上方第 0 步结论 1 ——
// evhttp_make_request 原样使用传入 uri,传完整绝对 URL 即发 absolute-form;
// 连接对象目标 = 代理;响应为代理直通的目标响应,evhttp 正常解析,无特殊处理)。
// Host 头 = 目标主机(经 DispatchOnConnection 的 hostHeader 参数)。
// 代理连接不回池(FinishRequest 阶段①按 req.HasProxy() 排除);重试/重定向经
// StartRequest 重新分派到本路径。
static void StartViaHttpProxy(ZmHttpClientReqState* st)
{
    // 请求总超时(deadline):代理路径在此创建(直连在 DispatchOnConnection;共用
    // !totalTimeoutEvent 守卫 —— 重定向/重试不重建)
    if (st->req.TotalTimeout() > 0 && !st->totalTimeoutEvent)
    {
        st->totalTimeoutEvent = event_new(st->priv->LoopBase(), -1, EV_TIMEOUT,
                                          ZmHttpClientTotalTimeoutCB, st);
        if (st->totalTimeoutEvent)
        {
            struct timeval tv = { st->req.TotalTimeout(), 0 };
            event_add(st->totalTimeoutEvent, &tv);
        }
    }

    struct evhttp_connection* con = evhttp_connection_base_new(
        st->priv->LoopBase(), st->priv->LoopDnsBase(),
        st->req.ProxyHost().c_str(), st->req.ProxyPort());
    if (!con)
    {
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "proxy connection create failed");
        return;
    }

    // absolute-form 目标:scheme://host[:port] + 路径(ParseHttpUrl 已归一化路径以
    // '/' 开头;IPv6 字面量回括 —— 代理需可解析的绝对 URL)
    std::string absUrl = st->scheme + "://" + HttpTargetHostHeader(st) + st->path;
    std::string hostHdr = HttpTargetHostHeader(st);
    DispatchOnConnection(st, con, absUrl.c_str(), &hostHdr);
}

// C16 HTTPS 代理路径:CONNECT 隧道(流程见上方 C16 总注释与隧道回调)。
// 取消适配:CONNECT 阶段 hreq 未创建 —— PostCancel/CancelInflightAll 对
// "hreq==null && retryEvent==null && tunnelBev!=null"(本阶段)直接收口 CANCELLED,
// FinishRequest 阶段①释放隧道 bev 并关 socket(已核对两处取消分支,含 CloseAll)。
// 总超时:deadline 在进入本函数时即创建(无 hreq 期间触发 → TotalTimeoutCB 的
// else 分支直接 FinishRequest(TIMEOUT)),覆盖 CONNECT + 整条请求链。
// 失败路径(连接失败/CONNECT 非 2xx/SSL 建失败)→ FinishRequest(ERR_CONNECT/
// ERR_CONNECT_TIMEOUT/ERR_PROXY/ERR_SSL);连接类错误可经 C15 重试(重走本路径)。
static void StartViaHttpsProxy(ZmHttpClientReqState* st)
{
    // 请求总超时(deadline):CONNECT 阶段即生效(无 hreq,触发经 FinishRequest 直收)
    if (st->req.TotalTimeout() > 0 && !st->totalTimeoutEvent)
    {
        st->totalTimeoutEvent = event_new(st->priv->LoopBase(), -1, EV_TIMEOUT,
                                          ZmHttpClientTotalTimeoutCB, st);
        if (st->totalTimeoutEvent)
        {
            struct timeval tv = { st->req.TotalTimeout(), 0 };
            event_add(st->totalTimeoutEvent, &tv);
        }
    }

    // 连接代理(不经连接池;代理连接不回池,见 FinishRequest)
    st->tunnelBev = bufferevent_socket_new(st->priv->LoopBase(), -1,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    if (!st->tunnelBev)
    {
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "proxy socket create failed");
        return;
    }
    // 连接/读写超时(口径与直连一致:ConnectTimeout 同时作用于连接与读写)。filter
    // 形态下 evhttp_connection_set_timeout 对 SSL bev 无效(见第 0 步结论 5),故在此
    // 直设于隧道 bev(底层 socket bev 读写事件携带超时);隧道 bev 超时经 be_ssl_eventcb
    // 透传回 SSL bev 用户回调 → evhttp_error_cb(EVREQ_HTTP_TIMEOUT),与直连同语义
    if (st->req.ConnectTimeout() > 0)
    {
        struct timeval tv = { st->req.ConnectTimeout(), 0 };
        bufferevent_set_timeouts(st->tunnelBev, &tv, &tv);
    }
    bufferevent_setcb(st->tunnelBev, ZmHttpClientTunnelReadCB, nullptr,
                      ZmHttpClientTunnelEventCB, st);
    if (bufferevent_socket_connect_hostname(st->tunnelBev, st->priv->LoopDnsBase(),
                                            AF_UNSPEC, st->req.ProxyHost().c_str(),
                                            st->req.ProxyPort()) != 0)
    {
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "proxy connect failed");   // 阶段①释放隧道 bev
        return;
    }
    // 后续:CONNECTED 事件 → 发 CONNECT → 读回调解析 2xx → 隧道接管(均在循环线程)
}

// 在已就绪的连接上组装请求并发出(直连/HTTP 代理/HTTPS 隧道共用;循环线程)。
// requestUri:直连/隧道 = origin-form 路径;HTTP 代理 = 完整绝对 URL(absolute-form)。
// hostHeader:非空且用户未显式设置 Host 头时补发(代理路径 —— 本库 libevent 客户端
// 不自动加 Host 头,已核对;RFC 7230 §5.4:absolute-form 请求的 Host 头为目标主机)。
// 直连路径传 nullptr(维持 C1-C15 既有行为,不加 Host)。
static void DispatchOnConnection(ZmHttpClientReqState* st, struct evhttp_connection* con,
                                 const char* requestUri, const std::string* hostHeader)
{
    // 连接超时(0 = 不限制:不设置,用 libevent 默认 45s;libevent 的 0 表示立即超时,故 0 时不调用)
    if (st->req.ConnectTimeout() > 0)
    {
        struct timeval ctv = { st->req.ConnectTimeout(), 0 };
        evhttp_connection_set_connect_timeout_tv(con, &ctv);
    }
    // 读写超时(0 = 不限制:不设置,用 libevent 默认 50s)
    if (st->req.ConnectTimeout() > 0)
        evhttp_connection_set_timeout(con, st->req.ConnectTimeout());

    // 请求总超时(deadline;0 = 不限制;流式请求在响应头到达(header_cb,2xx)时自动取消,见 OnHeaderCB)
    // 重定向链/重试(C15)不重建事件:保持原 deadline 覆盖整条链含重试(重建会覆盖并泄漏在飞
    // 事件;FinishRequest 前置清理亦保留该事件)。流式请求 2xx 已取消事件的场景(header_cb)
    // 重试时按守卫重建 —— 新链的 deadline,与 C11 语义一致
    // (C16:代理路径在 StartViaHttpProxy/StartViaHttpsProxy 创建 —— CONNECT 阶段即须
    // 覆盖;此处守卫对两处幂等)
    if (st->req.TotalTimeout() > 0 && !st->totalTimeoutEvent)
    {
        st->totalTimeoutEvent = event_new(st->priv->LoopBase(), -1, EV_TIMEOUT,
                                          ZmHttpClientTotalTimeoutCB, st);
        if (st->totalTimeoutEvent)
        {
            struct timeval tv = { st->req.TotalTimeout(), 0 };
            event_add(st->totalTimeoutEvent, &tv);
        }
    }

    struct evhttp_request* hreq = evhttp_request_new(&ZmHttpClientPrivate::ZmHttpClientOnDoneCB, st);
    if (!hreq)
    {
        evhttp_connection_free(con);
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "request create failed");
        return;
    }
    evhttp_request_set_error_cb(hreq, &ZmHttpClientPrivate::ZmHttpClientOnErrorCB);

    // 方法映射(未知方法退化为 GET)
    static const struct { const char* n; evhttp_cmd_type t; } kMethods[] = {
        {"GET", EVHTTP_REQ_GET}, {"POST", EVHTTP_REQ_POST}, {"PUT", EVHTTP_REQ_PUT},
        {"DELETE", EVHTTP_REQ_DELETE}, {"HEAD", EVHTTP_REQ_HEAD},
        {"OPTIONS", EVHTTP_REQ_OPTIONS}, {"PATCH", EVHTTP_REQ_PATCH},
    };
    evhttp_cmd_type mtype = EVHTTP_REQ_GET;
    for (const auto& km : kMethods)
        if (_stricmp(st->req.Method().c_str(), km.n) == 0) { mtype = km.t; break; }

    // 请求头
    for (const auto& kv : st->req.Headers())
        evhttp_add_header(evhttp_request_get_output_headers(hreq), kv.first.c_str(), kv.second.c_str());

    // Host 头补发(C18 审查修正):HTTP/1.1 强制 Host(RFC 7230 §5.4),vendored
    // libevent 客户端路径不自动生成 Host(已核对 http.c:make_header_request 仅追加
    // Content-Length)—— 直连与代理(absolute-form / CONNECT 隧道内)均须补发目标
    // 主机 Host(host[:port],非默认端口带端口;IPv6 字面量回括);用户显式设置时
    // 尊重,不覆盖。
    {
        bool hasHost = false;
        for (const auto& kv : st->req.Headers())
            if (_stricmp(kv.first.c_str(), "Host") == 0) { hasHost = true; break; }
        if (!hasHost)
        {
            std::string hostHdr = hostHeader ? *hostHeader : HttpTargetHostHeader(st);
            evhttp_add_header(evhttp_request_get_output_headers(hreq), "Host", hostHdr.c_str());
        }
    }

    // ── C13 gzip:请求允许解压(Gzip 默认开)且用户未显式设置 Accept-Encoding 时自动补
    // "Accept-Encoding: gzip, deflate"(用户显式设置时尊重,不覆盖;与 SSE 的 Accept 补全同模式)──
    if (st->req.Gzip())
    {
        bool hasAcceptEncoding = false;
        for (const auto& kv : st->req.Headers())
            if (_stricmp(kv.first.c_str(), "Accept-Encoding") == 0) { hasAcceptEncoding = true; break; }
        if (!hasAcceptEncoding)
            evhttp_add_header(evhttp_request_get_output_headers(hreq), "Accept-Encoding", "gzip, deflate");
    }

    // ── C14 cookie jar:自动附加 Cookie 头(UseCookieJar 默认开;用户显式设置的
    // "Cookie" 头尊重,不覆盖)。匹配:域后缀 + 路径前缀 + secure(仅 https)+ 未过期;
    // 跨主机重定向的显式 Cookie 已由 C9 从请求副本剥离,jar 附加按新主机重新匹配 ──
    if (st->req.UseCookieJar())
    {
        bool hasCookieHeader = false;
        for (const auto& kv : st->req.Headers())
            if (_stricmp(kv.first.c_str(), "Cookie") == 0) { hasCookieHeader = true; break; }
        if (!hasCookieHeader)
        {
            std::string cookie = ZmHttpClientPrivate::CookieBuildHeader(st->priv, st->scheme, st->host, st->path);
            if (!cookie.empty())
                evhttp_add_header(evhttp_request_get_output_headers(hreq), "Cookie", cookie.c_str());
        }
    }

    // ── C12 流式上传(chunked):文件/流式体统一 chunked 传输 ──
    // 头:Transfer-Encoding: chunked(不设 Content-Length;evhttp 对 POST/PUT 自动追加
    // "Content-Length: 0" —— 上传泵回调在头部写入输出缓冲时内联清除,见泵注释;
    // 用户显式 Content-Length 与 chunked 冲突(RFC 7230 3.3.2 禁止共存),剔除)
    const bool chunkedUpload = st->req.UploadChunk() || !st->req.BodyFile().empty();
    if (chunkedUpload)
    {
        evhttp_add_header(evhttp_request_get_output_headers(hreq), "Transfer-Encoding", "chunked");
        evhttp_remove_header(evhttp_request_get_output_headers(hreq), "Content-Length");

        // 上传泵接线:在连接 bev 输出缓冲注册回调(缓冲每次变更内联触发)。须在
        // make_request 之前注册 —— 头部写入(dispatch)发生在 make_request 内,
        // 回调要在头部写入期间清除自动 Content-Length(见泵阶段 1)
        struct bufferevent* bev = evhttp_connection_get_bufferevent(con);
        struct evbuffer* out = bev ? bufferevent_get_output(bev) : nullptr;
        if (!out)
        {
            evhttp_request_free(hreq);   // 未挂接连接,手动释放防泄漏
            // 局部 con 尚未赋给 st->con(赋值较晚),FinishRequest 的连接处置看不到
            // 它 → 须在此释放(池取出的连接不释放即永久缩水)
            evhttp_connection_free(con);
            ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "upload pump attach failed");
            return;
        }
        st->uploadCb = evbuffer_add_cb(out, ZmHttpClientUploadOutCB, st);
        if (!st->uploadCb)
        {
            evhttp_request_free(hreq);
            evhttp_connection_free(con);   // 同上:FinishRequest 看不到局部 con
            ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "upload pump attach failed");
            return;
        }
        // EVBUFFER_CB_NODEFER = 2(evbuffer-internal.h;公共头未导出该常量;libevent
        // 自身 bufferevent_setwatermark 亦经 set_flags 设置此值):回调在缓冲变更点
        // 内联执行 —— 泵必须在 evhttp_write_connectioncb"输出已空 → 转读响应"判定
        // 之前补块(时序依据见泵注释)
        evbuffer_cb_set_flags(out, st->uploadCb, 2);
    }

    // ── C11 真流式:流式消费请求注册增量钩子(核实结论见 OnHeaderCB 上方注释)──
    // SSE(OnSseEvent)自动补 Accept: text/event-stream(用户显式设置时尊重)
    if (st->IsStreamingConsumer())
    {
        evhttp_request_set_header_cb(hreq, &ZmHttpClientPrivate::ZmHttpClientOnHeaderCB);
        evhttp_request_set_chunked_cb(hreq, &ZmHttpClientPrivate::ZmHttpClientOnChunkCB);
        if (st->req.OnSseEvent())
        {
            bool hasAccept = false;
            for (const auto& kv : st->req.Headers())
                if (_stricmp(kv.first.c_str(), "Accept") == 0) { hasAccept = true; break; }
            if (!hasAccept)
                evhttp_add_header(evhttp_request_get_output_headers(hreq), "Accept", "text/event-stream");
        }
    }

    // ── Range(C9):bytes=offset-(evhttp_add_header 内部拷贝,局部变量保持清晰)──
    if (st->req.Range() >= 0)
    {
        std::string range = std::string("bytes=") + std::to_string(st->req.Range()) + "-";
        evhttp_add_header(evhttp_request_get_output_headers(hreq), "Range", range.c_str());
    }

    // 请求体:内存体写入 output_buffer(Content-Length 自动);文件/流式上传(C12)
    // 不写 output_buffer(保持空 → 自动 CL 为 0,由泵清除),体经上传泵 chunked 写入
    const auto& body = st->req.Body();
    if (!body.empty())
        evbuffer_add(evhttp_request_get_output_buffer(hreq), body.data(), body.size());

    st->con = con;
    st->hreq = hreq;
    st->attempts++;
    // 重定向/重试可能再次进入本函数:在飞列表防重复入列(须唯一,否则取消会重复命中)
    if (std::find(st->priv->m_inflight.begin(), st->priv->m_inflight.end(), st) ==
        st->priv->m_inflight.end())
    {
        st->priv->m_inflight.push_back(st);   // 先入列再 make_request:失败路径经收口出列
    }

    int rc = evhttp_make_request(con, hreq, mtype, requestUri);
    if (rc != 0)
    {
        // 失败:evhttp 已释放 hreq;连接仍归我方(收口统一释放)
        ZmHttpClientPrivate::FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "make_request failed");
    }
}

// ============================================================================
// 发起请求(循环线程):连接池获取空闲连接,无则新建;每连接单请求(归还即空闲)
// ============================================================================

void ZmHttpClientPrivate::StartRequest(ZmHttpClientReqState* st)
{
    if (st->cancelled)
    {
        FinishRequest(st, ZM_HTTPC_ERR_CANCELLED, "cancelled");
        return;
    }
    // 在飞注册(C16:提前到最前 —— CONNECT 隧道阶段无 hreq,取消/收口须能命中 st;
    // DispatchOnConnection 尾部仍有 find 守卫,重定向/重试再入不重复入列)
    if (std::find(st->priv->m_inflight.begin(), st->priv->m_inflight.end(), st) ==
        st->priv->m_inflight.end())
    {
        st->priv->m_inflight.push_back(st);
    }

    // URL 解析(失败按不支持协议收口)
    if (!ParseHttpUrl(st->req.Url(), st->scheme, st->host, st->port, st->path))
    {
        FinishRequest(st, ZM_HTTPC_ERR_UNSUPPORTED, "unsupported url");
        return;
    }

    // ── 输出文件(C10):发起前打开(wb 覆盖写;失败即收口,连接尚未创建无需释放)。
    // 重定向链再入本函数时已打开,跳过(文件跨整条链只开一次,FinishRequest 统一关闭;
    // 重定向响应的体不写盘,仅最终响应的体落盘)。
    if (!st->outFile && !st->req.OutputFile().empty())
    {
        // fopen_s(MSVC 安全形式;项目未开 _CRT_SECURE_NO_WARNINGS)
        errno_t fe = fopen_s(&st->outFile, st->req.OutputFile().c_str(), "wb");
        if (fe != 0 || !st->outFile)
        {
            st->outFile = nullptr;
            FinishRequest(st, ZM_HTTPC_ERR_FILE_IO, "open output file failed");
            return;
        }
    }

    // ── 文件上传(C12):发起前打开(rb;失败即收口,连接尚未创建无需释放)。
    // 流式体(UploadChunk)不在此打开;两形态均走 chunked 传输,泵见 ZmHttpClientUploadOutCB
    const bool chunkedUpload = st->req.UploadChunk() || !st->req.BodyFile().empty();
    if (chunkedUpload && !st->req.BodyFile().empty())
    {
        errno_t fe = fopen_s(&st->uploadFile, st->req.BodyFile().c_str(), "rb");
        if (fe != 0 || !st->uploadFile)
        {
            st->uploadFile = nullptr;
            FinishRequest(st, ZM_HTTPC_ERR_FILE_IO, "open upload file failed");
            return;
        }
    }

    // ── C16 代理路径分派:SetProxy 后所有请求先连代理,按目标 scheme 分两条路径 ──
    // (http = absolute-form;https = CONNECT 隧道;代理连接不回池,见 FinishRequest;
    //  重定向/重试再入本函数时按新 URL 重新分派,仍走代理)
    if (st->req.HasProxy())
    {
        if (st->scheme == "http")
        {
            StartViaHttpProxy(st);
            return;
        }
        if (st->scheme == "https")
        {
            StartViaHttpsProxy(st);
            return;
        }
    }

    // 请求级证书覆盖:绕过连接池直接新建(池内连接以旧身份握手,不得复用于新身份)
    const bool usePerReqCert = !st->req.ClientCertFile().empty() && !st->req.ClientKeyFile().empty();
    // 配置变更未重建时:https 请求亦跳过池复用,强制走 TLS 块 → 重建 + 清空旧 https 池
    // (否则 dirty 清空在 acquire 命中时不可达,校验模式/证书变更将无限期不生效)
    const bool skipPool = usePerReqCert || (st->scheme == "https" && st->priv->m_sslCtxDirty);
    struct bufferevent* bev = nullptr;   // HTTPS:自建 SSL bev(连接未接管时归我方释放)
    struct evhttp_connection* con = nullptr;
    if (!skipPool)
        con = HttpClientPoolAcquire(st->priv, st->scheme, st->host, st->port);
    if (!con)
    {
        if (st->scheme == "https")
        {
            // ── TLS(C8):SSL_CTX 构建 + SSL bev 挂载(连接创建即完成,后续 set_timeout 直接作用于 bev)──
            // (C16:SSL_CTX 获取逻辑已提取为 HttpGetUseCtx,直连/代理隧道共用)
            SSL_CTX* useCtx = ZmHttpClientPrivate::HttpGetUseCtx(st->priv, st, usePerReqCert);
            if (!useCtx)
            {
                st->perReqSslCtx = nullptr;
                FinishRequest(st, ZM_HTTPC_ERR_SSL, "ssl context build failed");
                return;
            }
            // SSL 对象 + SNI(IP 字面量不发送 SNI,RFC 6066);bev 持有 SSL(bev 释放时 SSL_free),连接持有 bev
            SSL* ssl = SSL_new(useCtx);
            if (!ssl)
            {
                FinishRequest(st, ZM_HTTPC_ERR_SSL, "ssl object create failed");
                return;
            }
            if (st->host.find(':') == std::string::npos &&
                !isdigit((unsigned char)st->host[0]))
            {
                SSL_set_tlsext_host_name(ssl, st->host.c_str());
            }
            bev = bufferevent_openssl_socket_new(
                st->priv->LoopBase(), -1, ssl, BUFFEREVENT_SSL_CONNECTING,
                BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
            if (!bev)
            {
                // bev 创建失败:libevent 已按 BEV_OPT_CLOSE_ON_FREE 释放 ssl
                FinishRequest(st, ZM_HTTPC_ERR_SSL, "ssl bufferevent create failed");
                return;
            }
            bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);   // 对端不送 close_notify 按干净 EOF 处理
            con = evhttp_connection_base_bufferevent_new(st->priv->LoopBase(), st->priv->LoopDnsBase(),
                                                         bev, st->host.c_str(), st->port);
        }
        else
        {
            con = evhttp_connection_base_new(st->priv->LoopBase(), st->priv->LoopDnsBase(),
                                             st->host.c_str(), st->port);
        }
    }
    if (!con)
    {
        if (bev)
        {
            // HTTPS:base_bufferevent_new 失败路径 bev 归属不一致 —— calloc 失败未接管(须释放);
            // address strdup 失败已随连接释放(此处会双释放,仅存于 calloc 成功+strdup 失败的双重
            // OOM 级联,接受此权衡;详见 libevent http.c evhttp_connection_base_bufferevent_new)
            bufferevent_free(bev);
            FinishRequest(st, ZM_HTTPC_ERR_SSL, "ssl connection create failed");
        }
        else
        {
            FinishRequest(st, ZM_HTTPC_ERR_CONNECT, "connection create failed");
        }
        return;
    }
    // 直连路径:连接就绪,请求组装与发出(头/体/超时/钩子/泵)在 DispatchOnConnection
    DispatchOnConnection(st, con, st->path.c_str(), nullptr);
}

// ============================================================================
// ZmHttpClientPrivate 完整实现
// ============================================================================

ZmHttpClientPrivate::ZmHttpClientPrivate(ZmHttpClient* owner)
    : m_owner(owner), m_loop(nullptr)
{
}

ZmHttpClientPrivate::~ZmHttpClientPrivate()
{
    Stop();   // 兜底(公开析构已先 PostCloseAll + Stop)
    // 客户端级 SSL_CTX:循环线程已停止(在飞已取消/连接已清理),直接释放;
    // 若仍有连接持有 SSL 对象,OpenSSL 引用计数保证 ctx 存活至 SSL 释放
    if (m_sslCtx)
    {
        SSL_CTX_free(m_sslCtx);
        m_sslCtx = nullptr;
    }
}

bool ZmHttpClientPrivate::Start(const char* name)
{
    if (m_loop)
        return m_loop->IsRunning();
    m_loop = new ZmHttpClientLoop(std::string(name ? name : "ZmHttpClient") + "Loop");
    if (!m_loop->Start())
    {
        delete m_loop;
        m_loop = nullptr;
        return false;
    }
    // C18 审查修正:手动 DNS 落实 —— 循环就绪后投递应用(SetDnsServers 已设置的;
    // 未设置/空 → 保持系统默认,evdns_base 已由循环按系统 DNS 配置)
    if (!m_dnsServers.empty())
        PostToLoop(m_loop->EventBase(), [this]() { ApplyDnsServers(); });
    return true;
}

void ZmHttpClientPrivate::Stop()
{
    if (m_loop)
    {
        m_loop->Stop();
        delete m_loop;
        m_loop = nullptr;
    }
}

// C18 审查修正(spec §2-2 落实):手动 DNS 覆写循环的 evdns_base —— 清空系统默认
// nameserver(挂起在途解析),逐条添加手动配置(逗号分隔,与 GetDNSAddresses 同格式),
// 最后恢复解析。循环线程内执行(经 PostToLoop 投递;m_dnsServers 空 = 不动)。
void ZmHttpClientPrivate::ApplyDnsServers()
{
    if (m_dnsServers.empty())
        return;
    evdns_base* db = m_loop ? m_loop->EventDnsBase() : nullptr;
    if (!db)
        return;
    evdns_base_clear_nameservers_and_suspend(db);
    char* buf = _strdup(m_dnsServers.c_str());
    if (buf)
    {
        char* cursor = buf;
        char* token = zm_strsep(&cursor, ",");
        while (token)
        {
            while (*token == ' ') token++;
            if (*token)
                evdns_base_nameserver_ip_add(db, token);
            token = zm_strsep(&cursor, ",");
        }
        free(buf);
    }
    evdns_base_resume(db);
}

bool ZmHttpClientPrivate::SetDnsServers(const char* servers)
{
    if (servers)
        m_dnsServers = servers;
    // C18 审查修正:循环已运行时即时投递应用(Start 路径在 Start 内应用;两条路径
    // 均在循环线程执行,避免与在飞解析并发;调用方须在 Start 后、发请求前配置)
    if (m_loop && m_loop->IsRunning() && !m_dnsServers.empty())
        PostToLoop(m_loop->EventBase(), [this]() { ApplyDnsServers(); });
    return true;
}

bool ZmHttpClientPrivate::SetClientCert(const char* certFile, const char* keyFile)
{
    m_clientCertFile = certFile ? certFile : "";
    m_clientKeyFile = keyFile ? keyFile : "";
    m_sslCtxDirty = true;   // 下次 HTTPS 请求重建(懒重建;调用方须在请求前配置,不并发)
    return true;
}

void ZmHttpClientPrivate::SetVerifyMode(bool verifyPeer, const char* caFile)
{
    m_verifyPeer = verifyPeer;
    m_caFile = caFile ? caFile : "";
    m_sslCtxDirty = true;   // 下次 HTTPS 请求重建
}

// 未启动/投递失败:直接(在调用线程)返回错误,保证"恰好一次"
void ZmHttpClientPrivate::FailRequestDirect(ZmHttpClientReqState* st, const char* text)
{
    ZmHttpClientResult r;
    r.m_error = ZM_HTTPC_ERR_CONNECT;
    r.m_errorText = text ? text : "client not started";
    if (st->syncPromise)
    {
        auto p = std::unique_ptr<ZmHttpClientResult>(new ZmHttpClientResult(std::move(r)));
        st->syncPromise->set_value(std::move(p));
    }
    else if (st->cb)
    {
        st->cb(&r, st->id, st->params);
    }
    delete st;
}

void ZmHttpClientPrivate::PostStartRequest(const ZmHttpClientRequest& req, uint64_t id,
                                           void* params, ZmHttpClientCallback cb,
                                           std::promise<std::unique_ptr<ZmHttpClientResult>>* syncP)
{
    auto* st = new ZmHttpClientReqState(this, req, id, params, cb);
    st->syncPromise = syncP;

    if (!m_loop)
    {
        FailRequestDirect(st, "client not started");
        return;
    }
    if (!PostToLoop(m_loop->EventBase(), [this, st]() { StartRequest(st); }))
        FailRequestDirect(st, "loop unavailable");
}

void ZmHttpClientPrivate::PostCancel(uint64_t id)
{
    if (!m_loop)
        return;
    PostToLoop(m_loop->EventBase(), [this, id]()
    {
        for (auto* st : m_inflight)
        {
            if (st->id == id)
            {
                st->cancelled = true;
                if (st->hreq)
                    evhttp_cancel_request(st->hreq);   // → error_cb(CANCEL) → 收口
                else if (st->retryEvent)
                {
                    // C15 退避窗口(无在飞请求,仅退避定时器挂起):立即收口 ——
                    // 否则要等定时器到点(延迟 ≤ 退避时长,最坏无界)才经 StartRequest
                    // 的 cancelled 检查收口;直接取消定时器并收口,语义与 hreq 分支一致
                    event_del(st->retryEvent);
                    event_free(st->retryEvent);
                    st->retryEvent = nullptr;
                    FinishRequest(st, ZM_HTTPC_ERR_CANCELLED, "cancelled");
                }
                else if (st->tunnelBev)
                {
                    // C16 CONNECT 隧道阶段:无 hreq/无退避,仅隧道 bev 在飞 —— 与
                    // 退避窗口同款处理:立即收口(CANCELLED;阶段①释放隧道 bev),
                    // 否则取消将永久悬置(隧道 bev 由收口释放,不再有回调触发收口)
                    FinishRequest(st, ZM_HTTPC_ERR_CANCELLED, "cancelled");
                }
                break;   // id 唯一(同步请求 id 来自原子计数器;异步 id 由调用方保证)
            }
        }
    });
}

void ZmHttpClientPrivate::PostCancelAll()
{
    if (!m_loop)
        return;
    PostToLoop(m_loop->EventBase(), [this]() { CancelInflightAll(this); });
}

void ZmHttpClientPrivate::PostCloseAll()
{
    if (!m_loop)
        return;
    PostToLoop(m_loop->EventBase(), [this]()
    {
        HttpClientPoolClearAll(this);   // 清空空闲连接缓存(SSL bev 随之释放)
        CancelInflightAll(this);        // 取消全部在飞
        if (m_sslCtx) { SSL_CTX_free(m_sslCtx); m_sslCtx = nullptr; }   // 客户端级 SSL_CTX(下次请求懒重建)
    });
}

// ============================================================================
// ZmHttpClient 公开接口(委托私有类;私有类负责循环与线程安全投递)
// ============================================================================

ZmHttpClient::ZmHttpClient(const std::string& name)
    : m_priv(nullptr), m_name(name)
{
}

ZmHttpClient::~ZmHttpClient()
{
    if (m_priv)
    {
        m_priv->PostCloseAll();   // 取消全部在飞(投递;循环线程执行)
        m_priv->Stop();           // 停循环并 join(在飞收口完成后退出)
        delete m_priv;
        m_priv = nullptr;
    }
}

bool ZmHttpClient::Start()
{
    if (!m_priv)
        m_priv = new ZmHttpClientPrivate(this);
    return m_priv->Start(m_name.c_str());
}

bool ZmHttpClient::SetDnsServers(const char* servers)
{
    return m_priv && m_priv->SetDnsServers(servers);
}

bool ZmHttpClient::SetClientCert(const char* certFile, const char* keyFile)
{
    return m_priv && m_priv->SetClientCert(certFile, keyFile);
}

void ZmHttpClient::SetVerifyMode(bool verifyPeer, const char* caFile)
{
    if (m_priv)
        m_priv->SetVerifyMode(verifyPeer, caFile);
}

bool ZmHttpClient::IsLooped() const
{
    return m_priv && m_priv->IsLooped();
}

ZmHttpClientResult* ZmHttpClient::Send(const ZmHttpClientRequest& req)
{
    if (!m_priv)
    {
        // 未启动:返回错误对象(遵守头文件"失败亦返回对象"契约,不返回 nullptr)
        auto* r = new ZmHttpClientResult();
        r->m_error = ZM_HTTPC_ERR_CONNECT;
        r->m_errorText = "client not started";
        return r;
    }

    static std::atomic<uint64_t> s_syncId{ 1 };
    // C18 审查修正:同步请求 id 加高位偏移(0x8000...),与调用方自选异步 id
    // (低 63 位)隔离 —— PostCancel 按 id 扫 m_inflight,两区间永不重叠,防误杀
    uint64_t id = 0x8000000000000000ULL | s_syncId.fetch_add(1);   // 同步请求也占独立 id,支持按 id 取消

    auto promise = std::make_shared<std::promise<std::unique_ptr<ZmHttpClientResult>>>();
    auto fut = promise->get_future();
    m_priv->PostStartRequest(req, id, nullptr, {}, promise.get());

    // 阻塞等待:TotalTimeout 秒(0 = 不限制)
    std::future_status st = std::future_status::ready;
    if (req.TotalTimeout() > 0)
        st = fut.wait_for(std::chrono::seconds(req.TotalTimeout()));
    else
        fut.wait();   // 不限制:无限等待

    if (st == std::future_status::timeout)
    {
        m_priv->PostCancel(id);   // 循环线程内取消 → 收口置值
        if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
        {
            // 极端兜底:循环线程失联(卡死)。promise 转入孤儿池保活,
            // 防止循环线程后续 set_value 访问已析构 promise(结果无人读取,无害)。
            static std::mutex s_orphanMutex;
            static std::vector<std::shared_ptr<std::promise<std::unique_ptr<ZmHttpClientResult>>>> s_orphans;
            {
                std::lock_guard<std::mutex> lock(s_orphanMutex);
                s_orphans.push_back(std::move(promise));
            }
            auto* r = new ZmHttpClientResult;
            r->m_error = ZM_HTTPC_ERR_TIMEOUT;
            r->m_errorText = "loop unresponsive";
            return r;
        }
        auto* r = fut.get().release();
        r->m_error = ZM_HTTPC_ERR_TIMEOUT;   // 同步超时对外报 TIMEOUT(而非 CANCELLED)
        r->m_errorText = "total timeout";
        return r;
    }
    return fut.get().release();   // 调用方 delete
}

void ZmHttpClient::SendAsync(const ZmHttpClientRequest& req, uint64_t id, void* params, ZmHttpClientCallback cb)
{
    if (!m_priv)
    {
        // 未启动:以错误结果直接调用回调(遵守头契约"回调保证恰好触发一次";
        // 仅此退化场景在调用方线程执行回调)
        if (cb)
        {
            ZmHttpClientResult r;
            r.m_error = ZM_HTTPC_ERR_CONNECT;
            r.m_errorText = "client not started";
            cb(&r, id, params);
        }
        return;
    }
    m_priv->PostStartRequest(req, id, params, cb);
}

void ZmHttpClient::Cancel(uint64_t id)
{
    if (m_priv)
        m_priv->PostCancel(id);
}

void ZmHttpClient::CancelAll()
{
    if (m_priv)
        m_priv->PostCancelAll();
}

void ZmHttpClient::CloseAll()
{
    if (m_priv)
        m_priv->PostCloseAll();
}
