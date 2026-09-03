#ifndef NOMINMAX
#define NOMINMAX
#endif
// 头顺序铁律:trantor 先行,后 windows.h(否则 winsock.h 先于 winsock2.h 报重定义)
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/net/InetAddress.h>
#include <trantor/net/Resolver.h>
#include <trantor/net/TcpClient.h>
#include <trantor/net/TcpConnection.h>
#include <trantor/net/TLSPolicy.h>
#include <trantor/utils/MsgBuffer.h>
#include <drogon/utils/coroutine.h>

#include "zm_net_http_client.h"
#include "zm_net_http_client_download.h"

#include <zm_util_json.h>
#include <zm_util_logger.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

using std::string;

// ============================================================================
// 流式下载通道(设计 §10)
//  单 EventLoopThread(自启自停)+ trantor::TcpClient 直写盘;与 drogon app()/服务器
//  三面完全隔离。会话状态机全堆对象,回调链推进——无"跨线程 resume 协程帧"模式。
//  线程纪律:除 DoneFn/Shutdown 外的触碰均在 dlLoop 线程(连接/解析/写盘/meta 豁免)。
// ============================================================================
namespace
{
trantor::EventLoopThread* s_dlThread = nullptr;
trantor::EventLoop* s_dlLoop = nullptr;
std::mutex s_dlMtx;
bool s_dlRunning = false;

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

string ToLowerCopy(string s)
{
    for (auto& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

string Trim(const string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring ToW(const string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0')
        w.pop_back();
    return w;
}

// ----------------------------------------------------------------------------
// 极简 URL 解析(与普通 lane 同规则;仅 http/https)
// ----------------------------------------------------------------------------
struct ZmDownloadTarget
{
    bool ok = false;
    bool ssl = false;
    string host;
    uint16_t port = 0;
    string pathQuery;  // 恒以 '/' 开头;无 fragment
};

size_t FindFirstOffset(const string& s, size_t from, char c)
{
    size_t p = s.find(c, from);
    return p;
}

ZmDownloadTarget ParseDownloadUrl(const string& raw)
{
    ZmDownloadTarget t;
    size_t pos = raw.find("://");
    if (pos == string::npos)
        return t;
    string scheme = ToLowerCopy(raw.substr(0, pos));
    if (scheme != "http" && scheme != "https")
        return t;
    t.ssl = (scheme == "https");

    size_t start = pos + 3;
    size_t frag = raw.find('#', start);
    string rest = (frag == string::npos) ? raw.substr(start) : raw.substr(start, frag - start);
    size_t slash = rest.find('/');
    string authority = (slash == string::npos) ? rest : rest.substr(0, slash);
    t.pathQuery = (slash == string::npos) ? "/" : rest.substr(slash);

    size_t at = authority.find_last_of('@');
    if (at != string::npos)
        authority = authority.substr(at + 1);

    string host, portStr;
    if (!authority.empty() && authority[0] == '[')
    {
        size_t cb = authority.find(']');
        if (cb == string::npos)
            return t;
        host = authority.substr(1, cb - 1);
        if (cb + 1 < authority.size())
        {
            if (authority[cb + 1] != ':')
                return t;
            portStr = authority.substr(cb + 2);
        }
    }
    else
    {
        size_t colon = authority.rfind(':');
        if (colon != string::npos)
        {
            host = authority.substr(0, colon);
            portStr = authority.substr(colon + 1);
        }
        else
            host = authority;
    }
    if (host.empty())
        return t;
    host = ToLowerCopy(host);

    uint32_t p = t.ssl ? 443 : 80;
    if (!portStr.empty())
    {
        uint32_t acc = 0;
        for (char c : portStr)
        {
            if (!isdigit((unsigned char)c))
                return t;
            acc = acc * 10 + (uint32_t)(c - '0');
            if (acc > 65535)
                return t;
        }
        p = acc;
    }
    if (p == 0)
        return t;
    t.host = host;
    t.port = (uint16_t)p;
    t.ok = true;
    return t;
}

// ----------------------------------------------------------------------------
// 单次下载会话:状态机(回调推进;除 Finish 的 done 外全在 dlLoop 线程)
// ----------------------------------------------------------------------------
class ZmDownloadSession;

using ZmDoneFn = std::function<void(ZmHttpClient::ZmDownloadResult&&)>;

class ZmDownloadSession : public std::enable_shared_from_this<ZmDownloadSession>
{
  public:
    ZmDownloadSession(string url, const string destPath,
                      ZmHttpClient::ZmHttpRequestOptionsPtr opts, ZmDoneFn done)
        : url_(std::move(url)), destPath_(destPath), opts_(std::move(opts)), done_(std::move(done))
    {
    }

    void Start();

  private:
    // —— 状态 ——
    string url_;
    string destPath_;
    ZmHttpClient::ZmHttpRequestOptionsPtr opts_;
    ZmDoneFn done_;

    ZmDownloadTarget tgt_;
    std::shared_ptr<trantor::Resolver> resolver_;
    std::shared_ptr<trantor::TcpClient> client_;
    trantor::TcpConnectionPtr conn_;

    size_t chunkBytes_ = 1024 * 1024;
    int64_t stallAbortMs_ = 120 * 1000;
    int64_t writeMaxMs_ = 200;
    int64_t startMs_ = 0;
    int64_t lastActivityMs_ = 0;

    // 响应解析
    bool headerDone_ = false;
    int statusCode_ = 0;
    bool chunked_ = false;
    bool hasContentLength_ = false;
    uint64_t contentLength_ = 0;
    bool hasContentRange_ = false;
    uint64_t contentRangeFirst_ = 0;
    uint64_t recvBody_ = 0;
    uint64_t chunkPendingSize_ = UINT64_MAX;  // 当前 chunk 帧剩余(hex);UINT64_MAX = 等待 size 行
    bool chunkTerminated_ = false;
    string pendingBuf_;

    // 续传
    uint64_t wantedOffset_ = 0;
    string metaEtag_, metaLastModified_;
    bool restartOnceUsed_ = false;

    // 落盘
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t written_ = 0;

    bool finished_ = false;

    // -------------------------------------------------- 流程
    void StartAt(uint64_t offset, const string& etag, const string& lastModified);
    void LoadMeta(uint64_t& offset, string& etag, string& lastModified);
    void OpenPartFile();   // wantedOffset>0 定位续写,否则截断
    void ClosePartFile();

    void OnResolved(const trantor::InetAddress& addr);
    void OnConnected(const trantor::TcpConnectionPtr& conn);
    void OnConnectError();
    void OnSslError();
    void OnRecv(const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* msg);
    void OnDisconnect();

    void ProcessHeaderData(const char* data, size_t len);
    void HandleBodyData(const char* data, size_t len);
    void AppendToFile(const char* data, size_t len);
    bool FlushPending();   // 把未达块粒度的残余字节落盘(完成前必调;失败已内部 FinalizeErr)
    void CheckComplete();
    void LastActivity();
    void OnGuard();   // 停滞/写超时看护(自链 timer)

    bool WriteToFile(const char* data, size_t len);  // 单块;内部计耗时
    void RestartFromZero();
    void FinalizeOk();
    void FinalizeErr(drogon::ReqResult err, int status, const std::string& why);
    void Done(ZmHttpClient::ZmDownloadResult&& r);
};

// ============================================================================
// ZmDownloadSession 方法实现
// ============================================================================

void ZmDownloadSession::Start()
{
    const auto& def = ZmHttpClient::GetOptions();
    chunkBytes_ = def.downloadChunkBytes > 0 ? def.downloadChunkBytes : 1 * 1024 * 1024;
    stallAbortMs_ = def.downloadStallAbortMs > 0 ? def.downloadStallAbortMs : 120 * 1000;
    writeMaxMs_ = def.downloadWriteMaxMs > 0 ? def.downloadWriteMaxMs : 200;
    startMs_ = NowMs();
    lastActivityMs_ = startMs_;

    tgt_ = ParseDownloadUrl(url_);
    if (!tgt_.ok)
    {
        FinalizeErr(drogon::ReqResult::BadServerAddress, 0, "url 解析失败");
        return;
    }

    uint64_t offset = 0;
    string etag, lm;
    LoadMeta(offset, etag, lm);
    StartAt(offset, etag, lm);
}

void ZmDownloadSession::LoadMeta(uint64_t& offset, string& etag, string& lastModified)
{
    offset = 0;
    std::ifstream f(destPath_ + ".part.meta", std::ios::binary);
    if (!f)
        return;
    string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    string err;
    ZMJSON j = zm_json_parse(content, err);
    if (!err.empty())
        return;
    if (j.contains("url") && j["url"] == url_)
    {
        if (j.contains("offset") && j["offset"].is_number_unsigned())
            offset = j["offset"].get<uint64_t>();
        if (j.contains("etag"))
            etag = j["etag"].get<string>();
        if (j.contains("lastModified"))
            lastModified = j["lastModified"].get<string>();
    }
    metaEtag_ = etag;
    metaLastModified_ = lastModified;
}

void ZmDownloadSession::StartAt(uint64_t offset, const string& etag, const string& lastModified)
{
    wantedOffset_ = offset;
    metaEtag_ = etag;
    metaLastModified_ = lastModified;

    // Resolver 异步解析(TcpClient 只收 InetAddress;回传端口恒 0 需回填)
    if (!resolver_)
        resolver_ = trantor::Resolver::newResolver(s_dlLoop, 30);
    auto self = shared_from_this();
    resolver_->resolve(tgt_.host, [self](const trantor::InetAddress& addr) {
        trantor::InetAddress a = addr;
        a.setPortNetEndian(htons(self->tgt_.port));
        self->OnResolved(a);
    });
}

void ZmDownloadSession::OnResolved(const trantor::InetAddress& addr)
{
    if (finished_)
        return;
    auto self = shared_from_this();
    client_ = std::make_shared<trantor::TcpClient>(s_dlLoop, addr, "zm-download");

    client_->setMessageCallback(
        [self](const trantor::TcpConnectionPtr& c, trantor::MsgBuffer* buf) {
            self->OnRecv(c, buf);
        });
    client_->setConnectionCallback(
        [self](const trantor::TcpConnectionPtr& conn) {
            if (conn->connected())
                self->OnConnected(conn);
            else
                self->OnDisconnect();
        });
    client_->setConnectionErrorCallback([self]() { self->OnConnectError(); });
    client_->setSSLErrorCallback([self](trantor::SSLError) { self->OnSslError(); });

    // TLS 先于 connect(捆绑 API 事实);服务证书校验/CA 与普通 lane 同源(全局 Options),
    // 但注入路径确证可用(TLSPolicy.setCaPath;普通 lane 的 addSSLConfigs 未兑现,见 §8)
    const auto& def = ZmHttpClient::GetOptions();
    if (tgt_.ssl)
    {
        auto policy = trantor::TLSPolicy::defaultClientPolicy(tgt_.host);
        policy->setValidate(def.validateCert);
        if (!def.trustCA.empty())
            policy->setCaPath(def.trustCA);
        if (!def.clientCert.empty() && !def.clientKey.empty())
            policy->setCertPath(def.clientCert).setKeyPath(def.clientKey);
        client_->enableSSL(std::move(policy));
    }
    client_->connect();
}

void ZmDownloadSession::OnConnected(const trantor::TcpConnectionPtr& conn)
{
    if (finished_)
        return;
    conn_ = conn;
    lastActivityMs_ = NowMs();
    const auto& def = ZmHttpClient::GetOptions();

    // 打开 .part(续传定位或截断)
    OpenPartFile();
    if (file_ == INVALID_HANDLE_VALUE)
    {
        FinalizeErr(drogon::ReqResult::NetworkFailure, 0, "打开 .part 失败: " + destPath_ + ".part");
        return;
    }

    // 手写 HTTP/1.1 GET(设计 §10.2)
    string req = "GET " + tgt_.pathQuery + " HTTP/1.1\r\n";
    req += "Host: " + tgt_.host + (tgt_.port == (tgt_.ssl ? 443 : 80) ? "" : (":" + std::to_string(tgt_.port))) + "\r\n";
    req += "User-Agent: " + def.userAgent + "\r\n";
    if (wantedOffset_ > 0)
        req += "Range: bytes=" + std::to_string(wantedOffset_) + "-\r\n";
    if (!metaEtag_.empty())
        req += "If-Match: " + metaEtag_ + "\r\n";
    req += "Connection: close\r\n";
    req += "Accept-Encoding: identity\r\n";
    for (const auto& kv : def.commonHeaders)
        req += kv.first + ": " + kv.second + "\r\n";
    for (const auto& kv : opts_->headers)
        req += kv.first + ": " + kv.second + "\r\n";
    req += "\r\n";
    conn->send(req);
    LastActivity();

    // 启动停滞看护(自链 timer;会话结束 finished_ 后自止)
    auto self = shared_from_this();
    s_dlLoop->runAfter(0.25, [self]() { self->OnGuard(); });
}

void ZmDownloadSession::OnConnectError()
{
    if (finished_)
        return;
    FinalizeErr(drogon::ReqResult::BadServerAddress, 0, "连接失败/拒绝");
}

void ZmDownloadSession::OnSslError()
{
    if (finished_)
        return;
    FinalizeErr(drogon::ReqResult::HandshakeError, 0, "TLS 握手/证书失败");
}

// ----------------------------------------------------------------------------
// 极简响应解析(仅所需字段;100 跳过;块粒度直写盘)
// ----------------------------------------------------------------------------
void ZmDownloadSession::OnRecv(const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* msg)
{
    if (finished_)
        return;
    LastActivity();

    size_t len = msg->readableBytes();
    if (len == 0)
        return;

    if (!headerDone_)
    {
        // 找头结束 \r\n\r\n
        const char* data = msg->peek();
        string entire(data, len);
        size_t headerEnd = entire.find("\r\n\r\n");
        if (headerEnd == string::npos)
        {
            if (len > 256 * 1024)
            {
                FinalizeErr(drogon::ReqResult::BadResponse, 0, "响应头过长/畸形");
                return;
            }
            return;  // 继续等(缓冲区自然累积;不 retrieve 即可保留)
        }
        // 解析头块
        ProcessHeaderData(data, headerEnd + 4);
        msg->retrieve(headerEnd + 4);
        if (finished_)
            return;
        if (!headerDone_)
            return;  // 100 Continue → 重置继续等
        // 头解析完成;脱落 body 剩余处理
        len = msg->readableBytes();
        if (len > 0)
        {
            HandleBodyData(msg->peek(), len);
            msg->retrieveAll();
        }
    }
    else
    {
        HandleBodyData(msg->peek(), len);
        msg->retrieveAll();
    }
    CheckComplete();
}

void ZmDownloadSession::ProcessHeaderData(const char* data, size_t len)
{
    string head(data, len);
    size_t eol = head.find("\r\n");
    string statusLine = head.substr(0, eol);
    // "HTTP/1.1 200 OK" / "HTTP/1.1 206 Partial Content"
    size_t sp1 = statusLine.find(' ');
    if (sp1 == string::npos)
    {
        FinalizeErr(drogon::ReqResult::BadResponse, 0, "状态行畸形");
        return;
    }
    statusCode_ = atoi(statusLine.c_str() + sp1 + 1);
    if (statusCode_ == 100)
        return;  // 继续找正式头(headerDone_ 仍 false,缓冲区已消费,重入上层等待)

    chunked_ = false;
    hasContentLength_ = false;
    contentLength_ = 0;
    // 逐行遍历头字段
    size_t pos = eol + 2;
    while (pos < head.size())
    {
        if (head[pos] == '\r' && pos + 1 < head.size() && head[pos + 1] == '\n')
            break;
        size_t lineEnd = head.find("\r\n", pos);
        if (lineEnd == string::npos)
            break;
        string line = head.substr(pos, lineEnd - pos);
        size_t colon = line.find(':');
        if (colon != string::npos)
        {
            string key = ToLowerCopy(Trim(line.substr(0, colon)));
            string val = Trim(line.substr(colon + 1));
            if (key == "content-length")
            {
                contentLength_ = strtoull(val.c_str(), nullptr, 10);
                hasContentLength_ = true;
            }
            else if (key == "transfer-encoding")
            {
                chunked_ = (val.find("chunked") != string::npos);
            }
            else if (key == "content-range")
            {
                // "bytes 123-456/789"
                long long b = -1;
                sscanf_s(val.c_str(), "bytes %lld-", &b);
                contentRangeFirst_ = (uint64_t)(b >= 0 ? b : UINT64_MAX);
                hasContentRange_ = (b >= 0);
            }
            else if (key == "etag")
            {
                metaEtag_ = val;
            }
            else if (key == "last-modified")
            {
                metaLastModified_ = val;
            }
        }
        pos = lineEnd + 2;
    }
    headerDone_ = true;  // 正式响应头完成

    // 分支表(设计 §10.3):206 校验首字节;200 忽略 Range 截断从头;不符 → 截断从头(有限次)
    if (statusCode_ == 200)
    {
        if (wantedOffset_ > 0 && !restartOnceUsed_)
        {
            restartOnceUsed_ = true;
            headerDone_ = false;
            pendingBuf_.clear();
            RestartFromZero();
            return;
        }
    }
    else if (statusCode_ == 206 && hasContentRange_ && hasContentLength_)
    {
        if (contentRangeFirst_ != wantedOffset_)
        {
            if (!restartOnceUsed_)
            {
                restartOnceUsed_ = true;
                headerDone_ = false;
                pendingBuf_.clear();
                RestartFromZero();
                return;
            }
            // 已自动重开一次仍不符(服务端异常)→ 按错误终结(设计 §10.3"拒绝续传")
            FinalizeErr(drogon::ReqResult::BadResponse, statusCode_, "Content-Range 二次不符");
            return;
        }
    }
    else if (statusCode_ == 412 || statusCode_ == 416)
    {
        FinalizeErr(drogon::ReqResult::BadResponse, statusCode_, "Range/If-Match 冲突(" + std::to_string(statusCode_) + ")");
        return;
    }
    if (statusCode_ < 200 || statusCode_ >= 300)
    {
        FinalizeErr(drogon::ReqResult::BadResponse, statusCode_, "非 2xx 响应");
        return;
    }
}

void ZmDownloadSession::RestartFromZero()
{
    // 从头重下:截断 .part、清除 meta 起点、重发(应满足 Range 语义约束)
    wantedOffset_ = 0;
    metaEtag_.clear();
    metaLastModified_.clear();
    written_ = 0;
    pendingBuf_.clear();
    recvBody_ = 0;
    chunkPendingSize_ = UINT64_MAX;
    chunkTerminated_ = false;
    headerDone_ = false;
    if (file_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    if (client_)
        client_->disconnect();
    // 直接重新开连(同一会话续跑;conn_ 由断开回调清理)
    StartAt(0, "", "");
}

void ZmDownloadSession::HandleBodyData(const char* data, size_t len)
{
    if (len == 0 || finished_)
        return;
    if (chunked_)
    {
        // 极简 chunked:按 \r\n 分帧;chunk-size 行(hex);累积数据;0 终结
        pendingBuf_.append(data, len);
        for (;;)
        {
            if (chunkPendingSize_ == UINT64_MAX)
            {
                size_t lineEnd = pendingBuf_.find("\r\n");
                if (lineEnd == string::npos)
                    return;
                string sizeLine = pendingBuf_.substr(0, lineEnd);
                size_t semi = sizeLine.find(';');
                if (semi != string::npos)
                    sizeLine = sizeLine.substr(0, semi);
                chunkPendingSize_ = strtoull(sizeLine.c_str(), nullptr, 16);
                pendingBuf_.erase(0, lineEnd + 2);
            }
            if (chunkPendingSize_ == 0)
            {
                pendingBuf_.clear();  // 忽略 trailer
                chunkTerminated_ = true;
                CheckComplete();
                return;
            }
            if (pendingBuf_.size() < (size_t)chunkPendingSize_ + 2)
                return;
            // 修复(2026-09-04):chunk 数据已在 pendingBuf_ 前端,直接按块写盘。
            // 此前经 AppendToFile(pendingBuf_.data(), ...) 会触发 std::string 自追加
            // (把 pendingBuf_ 追加进自身,UB),实测 20MB 源被写成 40MB(两半相同)。
            {
                size_t remain = (size_t)chunkPendingSize_;
                size_t off = 0;
                while (remain > 0)
                {
                    size_t n = std::min(remain, chunkBytes_);
                    if (!WriteToFile(pendingBuf_.data() + off, n))
                        return;
                    if (finished_)
                        return;
                    off += n;
                    remain -= n;
                }
                recvBody_ += (size_t)chunkPendingSize_;
            }
            pendingBuf_.erase(0, (size_t)chunkPendingSize_ + 2);  // + CRLF
            chunkPendingSize_ = UINT64_MAX;
        }
    }
    else
    {
        AppendToFile(data, len);
    }
}

void ZmDownloadSession::CheckComplete()
{
    if (finished_)
        return;
    if (chunked_)
    {
        if (!chunkTerminated_)
            return;
    }
    else
    {
        if (hasContentLength_)
        {
            if (recvBody_ < contentLength_)
                return;
        }
        else
        {
            // 无 CL:等待连接关闭定界(Connection: close 已声明)
            return;
        }
    }
    FinalizeOk();
}

void ZmDownloadSession::AppendToFile(const char* data, size_t len)
{
    if (len == 0)
        return;
    recvBody_ += len;
    // 按 chunkBytes 粒度直写
    pendingBuf_.append(data, len);
    while (pendingBuf_.size() >= chunkBytes_)
    {
        if (!WriteToFile(pendingBuf_.data(), chunkBytes_))
            return;
        pendingBuf_.erase(0, chunkBytes_);
    }
}

bool ZmDownloadSession::FlushPending()
{
    if (pendingBuf_.empty())
        return true;
    bool ok = WriteToFile(pendingBuf_.data(), pendingBuf_.size());
    if (ok)
        pendingBuf_.clear();
    return ok;
}

bool ZmDownloadSession::WriteToFile(const char* data, size_t len)
{
    if (file_ == INVALID_HANDLE_VALUE)
        return false;
    auto wb = std::chrono::steady_clock::now();
    DWORD written = 0;
    BOOL ok = WriteFile(file_, data, (DWORD)len, &written, nullptr);
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wb)
            .count();
    if (!ok || written != len)
    {
        FinalizeErr(drogon::ReqResult::NetworkFailure, 0, "写盘失败");
        return false;
    }
    if (elapsed > writeMaxMs_)
    {
        PUBLIC_LOG_WARN("ZmHttpClient 下载写盘超限({}ms > {}ms),url={}", elapsed, writeMaxMs_, url_);
        FinalizeErr(drogon::ReqResult::NetworkFailure, 0, "写盘耗时超限(慢盘)");
        return false;
    }
    written_ += len;
    lastActivityMs_ = NowMs();
    return true;
}

void ZmDownloadSession::LastActivity()
{
    lastActivityMs_ = NowMs();
}

void ZmDownloadSession::OnDisconnect()
{
    if (finished_)
        return;
    conn_.reset();
    if (headerDone_ && !chunked_ && !hasContentLength_)
    {
        // Connection: close 定界完成
        FinalizeOk();
        return;
    }
    if (!headerDone_)
    {
        FinalizeErr(drogon::ReqResult::NetworkFailure, 0, "连接中断(未收到响应头)");
        return;
    }
    if (chunked_ && !chunkTerminated_)
    {
        FinalizeErr(drogon::ReqResult::BadResponse, 0, "chunked 未终结即断开");
        return;
    }
    if (hasContentLength_ && recvBody_ < contentLength_)
    {
        FinalizeErr(drogon::ReqResult::BadResponse, 0, "响应体未收全即断开");
        return;
    }
    FinalizeOk();
}

void ZmDownloadSession::FinalizeOk()
{
    if (finished_)
        return;
    if (!FlushPending())
        return;  // 写盘失败已走 FinalizeErr
    finished_ = true;
    if (client_)
        client_->disconnect();
    ClosePartFile();

    // 覆写目标:Windows 用 MoveFileExW(REPLACE;std::rename 失败)
    std::wstring part = ToW(destPath_ + ".part");
    std::wstring dst = ToW(destPath_);
    bool renamed = MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!renamed)
    {
        // 退路:删目标重试一次
        DeleteFileW(dst.c_str());
        renamed = MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!renamed)
    {
        PUBLIC_LOG_WARN("ZmHttpClient 下载改名失败(.part→目标),url={}", url_);
        ZmHttpClient::ZmDownloadResult r;
        r.ok = false;
        r.status = statusCode_;
        r.error = "最后改名失败(.part→目标)";
        Done(std::move(r));
        return;
    }
    // 完成:清理 meta
    DeleteFileW(ToW(destPath_ + ".part.meta").c_str());

    ZmHttpClient::ZmDownloadResult r;
    r.ok = true;
    r.status = statusCode_;
    r.written = written_;
    Done(std::move(r));
}

void ZmDownloadSession::FinalizeErr(drogon::ReqResult err, int status, const std::string& why)
{
    if (finished_)
        return;
    // abort 语义:保留 .part/.meta 供续传(设计 §10.2;仅"从头重下"路径截断)
    finished_ = true;
    if (client_)
        client_->disconnect();
    ClosePartFile();
    PUBLIC_LOG_WARN("ZmHttpClient 下载失败(err={},status={},url={},why={})", (int)err, status,
                    url_, why);

    ZmHttpClient::ZmDownloadResult r;
    r.ok = false;
    r.status = status;
    if (err == drogon::ReqResult::Timeout)
        r.error = "timeout";
    else if (err == drogon::ReqResult::BadResponse)
        r.error = why;
    else if (err == drogon::ReqResult::HandshakeError)
        r.error = "tls";
    else
        r.error = why;
    Done(std::move(r));
}

void ZmDownloadSession::ClosePartFile()
{
    if (file_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

void ZmDownloadSession::OpenPartFile()
{
    std::wstring path = ToW(destPath_ + ".part");
    // 续传:wantedOffset_ > 0 → 打开(不截断)并推进写指针;否则截断
    DWORD access = GENERIC_WRITE;
    HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    file_ = h;
    if (wantedOffset_ > 0)
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)wantedOffset_;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
        {
            ClosePartFile();
            return;
        }
        written_ = wantedOffset_;
    }
    else
    {
        // 从头:截断
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
        written_ = 0;
    }
}

void ZmDownloadSession::Done(ZmHttpClient::ZmDownloadResult&& r)
{
    // 最后触碰外部(可能销毁本机)
    if (done_)
    {
        ZmDoneFn d = std::move(done_);
        done_ = nullptr;
        d(std::move(r));
    }
}

void ZmDownloadSession::OnGuard()
{
    if (finished_)
        return;
    int64_t now = NowMs();
    if (now - lastActivityMs_ > stallAbortMs_)
    {
        FinalizeErr(drogon::ReqResult::Timeout, 0, "对端停滞(超时无进展)");
        return;
    }
    // 自链心跳
    auto self = shared_from_this();
    s_dlLoop->runAfter(0.25, [self]() { self->OnGuard(); });
}

}  // namespace

// ============================================================================
// 通道静态(设计 §4.1 三步序 ①;Start/Shutdown 阶幂等)
// ============================================================================

bool ZmHttpDownloadChannel::Start()
{
    std::lock_guard lock(s_dlMtx);
    if (s_dlRunning)
        return true;
    auto t = std::make_unique<trantor::EventLoopThread>("ZmHttpClient-DL");
    t->run();
    trantor::EventLoop* loop = nullptr;
    for (int spin = 0; spin < 500 && !(loop = t->getLoop()); ++spin)
        Sleep(10);  // 同 lane 纪律:start 非阻塞,getLoop 就绪前可能为 null
    if (!loop)
        return false;
    s_dlThread = t.release();
    s_dlLoop = loop;
    s_dlRunning = true;
    return true;
}

void ZmHttpDownloadChannel::Shutdown()
{
    std::lock_guard lock(s_dlMtx);
    if (!s_dlRunning)
        return;
    s_dlRunning = false;
    auto* t = s_dlThread;
    auto* loop = s_dlLoop;
    s_dlThread = nullptr;
    s_dlLoop = nullptr;
    if (loop)
        loop->quit();
    if (t)
        t->wait();
    delete t;
}

bool ZmHttpDownloadChannel::IsRunning()
{
    std::lock_guard lock(s_dlMtx);
    return s_dlRunning;
}

bool ZmHttpDownloadChannel::StartDownload(const std::string& url, const std::string& destPath,
                                          ZmHttpClient::ZmHttpRequestOptionsPtr opts,
                                          DoneFn done)
{
    trantor::EventLoop* loop;
    {
        std::lock_guard lock(s_dlMtx);
        if (!s_dlRunning || !s_dlLoop)
            return false;
        loop = s_dlLoop;
    }
    auto session =
        std::make_shared<ZmDownloadSession>(url, destPath, std::move(opts), std::move(done));
    loop->runInLoop([session]() { session->Start(); });
    return true;
}

// ============================================================================
// DownloadCoro 薄桥(与普通 lane 同纪律:帧内仅指针对齐,值对象堆化)
// ============================================================================
struct ZmDownloadCtx
{
    ZmHttpClient::ZmDownloadResult result;
    string url;
    string destPath;
    ZmHttpClient::ZmHttpRequestOptionsPtr opts;
};

class ZmDownloadAwaiter
{
  public:
    ZmDownloadAwaiter(string url, string destPath, ZmHttpClient::ZmHttpRequestOptionsPtr opts)
        : ctx_(std::make_shared<ZmDownloadCtx>()),
          resumeLoop_(opts ? opts->resumeLoop : nullptr)
    {
        ctx_->url = std::move(url);
        ctx_->destPath = std::move(destPath);
        ctx_->opts = std::move(opts);
    }

    bool await_ready() noexcept
    {
        if (!ZmHttpClient::IsReady())
        {
            ctx_->result.error = "ZmHttpClient 未初始化";
            return true;
        }
        if (!ZmHttpDownloadChannel::IsRunning())
        {
            ctx_->result.error = "下载通道未启用(Options.enableDownload)";
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        resumeH_ = h;
        auto ctx = ctx_;
        ZmHttpDownloadChannel::StartDownload(
            ctx->url, ctx->destPath, ctx->opts, [this, ctx](ZmHttpClient::ZmDownloadResult&& r) {
                ctx->result = std::move(r);
                if (resumeLoop_ && !resumeLoop_->isInLoopThread())
                    resumeLoop_->queueInLoop([h = resumeH_]() { h.resume(); });
                else
                    resumeH_.resume();
            });
    }

    ZmHttpClient::ZmDownloadResult await_resume()
    {
        return std::move(ctx_->result);
    }

  private:
    std::shared_ptr<ZmDownloadCtx> ctx_;   // 值对象唯一次元(堆;帧内仅指针对齐)
    trantor::EventLoop* resumeLoop_ = nullptr;
    std::coroutine_handle<> resumeH_{};
};

// ----------------------------------------------------------------------------
// 公共入口(下载通道)
// ----------------------------------------------------------------------------

drogon::Task<ZmHttpClient::ZmDownloadResult> ZmHttpClient::DownloadCoro(
    const std::string& url, const std::string& destPath, const ZmHttpRequestOptions& opts)
{
    co_return co_await ZmDownloadAwaiter(url, destPath,
                                         std::make_shared<const ZmHttpRequestOptions>(opts));
}

