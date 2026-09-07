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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

using std::string;

// ============================================================================
// 流式下载通道(设计二期 §15)
//  单 EventLoopThread(自启自停)+ 每会话写线程 + 有界指令队列;dlLoop 零磁盘。
//  会话状态机全堆对象,回调链推进;连接代数(epoch)守卫重连窗口。
//  续传:.part 大小即起点;If-Range 变更检测;.part.meta 仅 {etag,lastModified} 一次写。
//  线程纪律:除 Done(写线程)与 Shutdown 外的触碰均在 dlLoop 线程;
//            文件打开/侧车读取在 StartDownload 提交线程(调用方)同步完成。
// ============================================================================
namespace
{
trantor::EventLoopThread* s_dlThread = nullptr;
trantor::EventLoop* s_dlLoop = nullptr;
std::mutex s_dlMtx;
bool s_dlRunning = false;

// 在飞会话登记(Shutdown 快照/中止;Deliver 时自摘除)
std::mutex s_sessMtx;
std::set<std::shared_ptr<class ZmDownloadSession>> s_sessions;

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

/// 域名含 ':' 时为 IPv6,加 [] 返回(请求 Host 头用)
string BracketHost(const string& host)
{
    if (host.find(':') != string::npos)
        return "[" + host + "]";
    return host;
}

// ----------------------------------------------------------------------------
// 极简 URL 解析(仅 http/https)
// ----------------------------------------------------------------------------
struct ZmDownloadTarget
{
    bool ok = false;
    bool ssl = false;
    string host;
    uint16_t port = 0;
    string pathQuery;  // 恒以 '/' 开头;无 fragment
};

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

/// 请求行/头注入 fail-closed:拒绝裸空格与控制字符(防请求行/头注入)
bool IsSafeRequestBytes(const string& s)
{
    for (unsigned char c : s)
    {
        if (c <= 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// 单次下载会话(设计二期 §15)
//   dlLoop:解析/编排/指令入队;写线程:唯一盘上执行者;Done 恰一次(写线程)。
// ----------------------------------------------------------------------------
class ZmDownloadSession : public std::enable_shared_from_this<ZmDownloadSession>
{
  public:
    using ZmDoneFn = std::function<void(ZmHttpClient::ZmDownloadResult&&)>;

    /// 提交线程构造:解析 URL → 打开 .part(定起点/截断)→ 读侧车 → 起写线程。
    /// 失败返回 nullptr 并回填 err(不注册、不启动写线程)。
    static std::shared_ptr<ZmDownloadSession> Create(const string& url, const string& destPath,
                                                     ZmHttpClient::ZmHttpRequestOptionsPtr opts,
                                                     ZmDoneFn done, string& err)
    {
        const auto& def = ZmHttpClient::GetOptions();
        auto s = std::shared_ptr<ZmDownloadSession>(
            new ZmDownloadSession(url, destPath, std::move(opts), std::move(done)));
        s->chunkBytes_ = def.downloadChunkBytes > 0 ? def.downloadChunkBytes : 1024 * 1024;
        s->stallAbortMs_ = def.downloadStallAbortMs > 0 ? (int64_t)def.downloadStallAbortMs : 120000;
        s->qCap_ = def.downloadQueueMaxBytes > 0 ? def.downloadQueueMaxBytes : 64ULL * 1024 * 1024;

        s->tgt_ = ParseDownloadUrl(url);
        if (!s->tgt_.ok)
        {
            err = "url 解析失败";
            return nullptr;
        }
        if (!IsSafeRequestBytes(s->tgt_.pathQuery))
        {
            err = "path 含裸空格/控制字符";
            return nullptr;
        }
        for (const auto& kv : def.commonHeaders)
            if (!IsSafeRequestBytes(kv.first) || !IsSafeRequestBytes(kv.second))
            {
                err = "公共头含裸空格/控制字符: " + kv.first;
                return nullptr;
            }
        if (s->opts_)
        {
            const auto& optHeaders = s->opts_->headers;
            for (const auto& kv : optHeaders)
            {
                if (!IsSafeRequestBytes(kv.first) || !IsSafeRequestBytes(kv.second))
                {
                    err = "请求头含裸空格/控制字符: " + kv.first;
                    return nullptr;
                }
            }
        }

        // 打开 .part:N = 现有大小(唯一事实源);N=0 截断
        std::wstring part = ToW(destPath + ".part");
        s->file_ = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s->file_ == INVALID_HANDLE_VALUE)
        {
            err = "打开 .part 失败: " + destPath + ".part";
            return nullptr;
        }
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(s->file_, &sz) || sz.QuadPart < 0)
        {
            CloseHandle(s->file_);
            s->file_ = INVALID_HANDLE_VALUE;
            err = "查询 .part 大小失败: " + destPath + ".part";
            return nullptr;
        }
        if (sz.QuadPart > 0)
        {
            // 续传:起点 = 现有大小;读侧车(If-Range 校验值)
            LARGE_INTEGER li;
            li.QuadPart = sz.QuadPart;
            if (!SetFilePointerEx(s->file_, li, nullptr, FILE_BEGIN))
            {
                CloseHandle(s->file_);
                s->file_ = INVALID_HANDLE_VALUE;
                err = "定位 .part 续写点失败: " + destPath + ".part";
                return nullptr;
            }
            s->resumedFrom_ = (uint64_t)sz.QuadPart;
            s->cursor_ = s->resumedFrom_;
            s->ReadSidecar();
        }
        else
        {
            SetFilePointer(s->file_, 0, nullptr, FILE_BEGIN);
            SetEndOfFile(s->file_);
        }
        s->lastActivityMs_ = NowMs();

        // 写线程(成员声明序保证:writer_ 为最后成员,~session 先于其析构 join)
        s->writer_ = std::thread([raw = s.get()]() { raw->WriterLoop(); });
        return s;
    }

    /// dlLoop 入口:发起解析/连接
    void StartOnLoop()
    {
        BeginConnect();
    }

    /// Shutdown 请求(任意线程):唤醒/解除写线程阻塞;写线程自行收尾并 Done
    void RequestShutdown()
    {
        {
            std::lock_guard lk(qMtx_);
            writerStop_ = true;
        }
        qCv_.notify_all();
        CancelPendingFileIo();
    }

    /// 析构上下文自检:写线程自毁路径(hold 析构)detach 自己;其余线程等写线程退出。
    /// 写线程终态(Deliver)先摘登记再回执,故正常路径下此处 joinable 已为 false。
    /// (dtor 必须 public:shared_ptr 删除器需可 delete)
    ~ZmDownloadSession()
    {
        if (writer_.joinable())
        {
            if (writer_.get_id() == std::this_thread::get_id())
                writer_.detach();  // 线程即将自然退出,仅释放句柄
            else
                writer_.join();
        }
    }

  private:
    // 指令(设计 §15.2)
    struct ZmDlAction
    {
        enum class Kind
        {
            Write,      // data = 落盘字节
            Truncate,   // 就地截断至 0
            WriteMeta,  // data = 侧车 JSON
            Finish,     // number = 最终文件大小
            Abort       // 终止(保留 .part/.meta)
        };
        Kind kind = Kind::Write;
        string data;
        uint64_t number = 0;
    };

    ZmDownloadSession(string url, const string destPath,
                      ZmHttpClient::ZmHttpRequestOptionsPtr opts, ZmDoneFn done)
        : url_(std::move(url)), destPath_(destPath), opts_(std::move(opts)), done_(std::move(done))
    {
    }

    // —— 状态(dlLoop 侧) ——
    string url_;
    string destPath_;
    ZmHttpClient::ZmHttpRequestOptionsPtr opts_;
    ZmDoneFn done_;

    ZmDownloadTarget tgt_;
    std::shared_ptr<trantor::Resolver> resolver_;
    std::shared_ptr<trantor::TcpClient> client_;
    trantor::TcpConnectionPtr conn_;
    uint64_t epoch_ = 0;         // 连接代数:重连窗口内旧连接回调一律忽略
    bool guardStarted_ = false;  // 停滞看护链只起一次

    size_t chunkBytes_ = 1024 * 1024;
    int64_t stallAbortMs_ = 120000;
    int64_t lastActivityMs_ = 0;
    uint64_t qCap_ = 64ULL * 1024 * 1024;

    // 续传/游标
    uint64_t resumedFrom_ = 0;  // 本次起点(0 = 全新)
    uint64_t cursor_ = 0;       // 当前响应已交付写线程的落盘偏移
    string metaEtag_, metaLm_;  // 请求侧车值(发 If-Range)
    string respEtag_, respLm_;  // 当前响应头值(写侧车)
    bool restartOnceUsed_ = false;

    // 响应解析(fail-closed)
    bool headerDone_ = false;
    int statusCode_ = 0;
    bool chunked_ = false;
    bool hasContentLength_ = false;
    uint64_t contentLength_ = 0;
    bool hasContentRange_ = false;
    uint64_t contentRangeFirst_ = 0;
    uint64_t recvBody_ = 0;
    uint64_t chunkPendingSize_ = UINT64_MAX;
    bool chunkTerminated_ = false;
    string pendingBuf_;

    std::atomic<bool> finished_{false};  // 终态门:此后 dlLoop 不再入队/回调

    // —— 写线程侧 ——
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::thread writer_;          // 最后成员(先于其余成员析构)
    std::mutex qMtx_;
    std::condition_variable qCv_;
    std::deque<ZmDlAction> q_;
    uint64_t qBytes_ = 0;
    bool writerStop_ = false;

    // -------------------------------------------------- dlLoop 流程
    void BeginConnect()
    {
        if (finished_.load())
            return;
        ++epoch_;
        const uint64_t ep = epoch_;
        if (!resolver_)
            resolver_ = trantor::Resolver::newResolver(s_dlLoop, 30);
        auto self = shared_from_this();
        resolver_->resolve(tgt_.host, [self, ep](const trantor::InetAddress& addr) {
            if (self->epoch_ != ep || self->finished_.load())
                return;
            trantor::InetAddress a = addr;
            a.setPortNetEndian(htons(self->tgt_.port));  // Resolver 回传端口恒 0,须回填
            self->OnResolved(a, ep);
        });
    }

    void OnResolved(const trantor::InetAddress& addr, uint64_t ep)
    {
        auto self = shared_from_this();
        client_ = std::make_shared<trantor::TcpClient>(s_dlLoop, addr, "zm-download");
        client_->setMessageCallback([self, ep](const trantor::TcpConnectionPtr& c,
                                               trantor::MsgBuffer* buf) {
            if (self->epoch_ != ep || self->finished_.load())
                return;
            self->OnRecv(c, buf);
        });
        client_->setConnectionCallback([self, ep](const trantor::TcpConnectionPtr& conn) {
            if (self->epoch_ != ep || self->finished_.load())
                return;
            if (conn->connected())
                self->OnConnected(conn);
            else
                self->OnDisconnect();
        });
        client_->setConnectionErrorCallback([self, ep]() {
            if (self->epoch_ != ep || self->finished_.load())
                return;
            self->OnConnectError();
        });
        client_->setSSLErrorCallback([self, ep](trantor::SSLError) {
            if (self->epoch_ != ep || self->finished_.load())
                return;
            self->OnSslError();
        });

        // TLS 先于 connect;策略与普通 lane 同源(全局 Options)
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

    void OnConnected(const trantor::TcpConnectionPtr& conn)
    {
        conn_ = conn;
        lastActivityMs_ = NowMs();
        const auto& def = ZmHttpClient::GetOptions();

        string req = "GET " + tgt_.pathQuery + " HTTP/1.1\r\n";
        req += "Host: " + BracketHost(tgt_.host) +
               (tgt_.port == (tgt_.ssl ? 443 : 80) ? "" : (":" + std::to_string(tgt_.port))) + "\r\n";
        req += "User-Agent: " + def.userAgent + "\r\n";
        if (resumedFrom_ > 0)
            req += "Range: bytes=" + std::to_string(resumedFrom_) + "-\r\n";
        if (resumedFrom_ > 0 && !metaEtag_.empty())
            req += "If-Range: " + metaEtag_ + "\r\n";
        else if (resumedFrom_ > 0 && metaEtag_.empty() && !metaLm_.empty())
            req += "If-Range: " + metaLm_ + "\r\n";
        req += "Connection: close\r\n";
        req += "Accept-Encoding: identity\r\n";
        for (const auto& kv : def.commonHeaders)
            req += kv.first + ": " + kv.second + "\r\n";
        if (opts_)
            for (const auto& kv : opts_->headers)
                req += kv.first + ": " + kv.second + "\r\n";
        req += "\r\n";
        conn->send(req);

        // 停滞看护(仅一次;自链 timer,finished_ 后自止)
        if (!guardStarted_)
        {
            guardStarted_ = true;
            auto self = shared_from_this();
            s_dlLoop->runAfter(0.25, [self]() { self->OnGuard(); });
        }
    }

    void OnDisconnect()
    {
        conn_.reset();
        if (finished_.load())
            return;
        if (!headerDone_)
        {
            Fail(drogon::ReqResult::NetworkFailure, 0, "连接中断(未收到响应头)");
            return;
        }
        if (chunked_ && !chunkTerminated_)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, "chunked 未终结即断开");
            return;
        }
        if (hasContentLength_ && recvBody_ < contentLength_)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, "响应体未收全即断开");
            return;
        }
        Complete();  // Connection: close 定界完成
    }

    void OnConnectError()
    {
        Fail(drogon::ReqResult::BadServerAddress, 0, "连接失败/拒绝");
    }

    void OnSslError()
    {
        Fail(drogon::ReqResult::HandshakeError, 0, "TLS 握手/证书失败");
    }

    // —— 停滞看护:读侧停滞(无收包且写队列已空)→ 放弃 ——
    void OnGuard()
    {
        if (finished_.load())
            return;
        int64_t now = NowMs();
        uint64_t qb = 0;
        {
            std::lock_guard lk(qMtx_);
            qb = qBytes_;
        }
        if (now - lastActivityMs_ > stallAbortMs_ && qb == 0)
        {
            Fail(drogon::ReqResult::Timeout, statusCode_, "对端停滞(超时无进展)");
            return;
        }
        auto self = shared_from_this();
        s_dlLoop->runAfter(0.25, [self]() { self->OnGuard(); });
    }

    // —— 接收:头部累积 + fail-closed 解析 + body 切块入队 ——
    void OnRecv(const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* msg)
    {
        lastActivityMs_ = NowMs();
        size_t len = msg->readableBytes();
        if (len == 0)
            return;

        if (!headerDone_)
        {
            const char* data = msg->peek();
            string entire(data, len);
            size_t headerEnd = entire.find("\r\n\r\n");
            if (headerEnd == string::npos)
            {
                if (len > 256 * 1024)
                    Fail(drogon::ReqResult::BadResponse, 0, "响应头过长/畸形");
                return;  // 继续等(不 retrieve 即保留)
            }
            ProcessHeaderData(data, headerEnd + 4);
            msg->retrieve(headerEnd + 4);
            if (finished_.load())
                return;
            if (!headerDone_)
                return;  // 1xx 跳过,重入等待
            size_t rest = msg->readableBytes();
            if (rest > 0)
            {
                HandleBodyData(msg->peek(), rest);
                msg->retrieveAll();
            }
        }
        else
        {
            HandleBodyData(msg->peek(), len);
            msg->retrieveAll();
        }
        if (finished_.load())
            return;
        CheckComplete();
    }

    // 状态行严格解析:"HTTP/<digit>.<digit> <3位数字> ..."
    bool ParseStatusLine(const string& statusLine)
    {
        if (statusLine.compare(0, 5, "HTTP/") != 0)
            return false;
        size_t sp = statusLine.find(' ');
        if (sp == string::npos || sp < 8 || statusLine[6] != '.')
            return false;
        if (!isdigit((unsigned char)statusLine[5]) || !isdigit((unsigned char)statusLine[7]))
            return false;
        if (sp + 4 > statusLine.size())
            return false;
        for (int i = 1; i <= 3; ++i)
        {
            if (!isdigit((unsigned char)statusLine[sp + i]))
                return false;
        }
        statusCode_ = (statusLine[sp + 1] - '0') * 100 + (statusLine[sp + 2] - '0') * 10 +
                      (statusLine[sp + 3] - '0');
        return true;
    }

    static bool AllDigits(const string& s)
    {
        if (s.empty())
            return false;
        for (char c : s)
        {
            if (!isdigit((unsigned char)c))
                return false;
        }
        return true;
    }

    /// Content-Range:"bytes <first>-<last>/<total>"(first 必须可解析;last/total 不使用)
    bool ParseContentRangeFirst(const string& val, uint64_t& first)
    {
        const string prefix = "bytes ";
        if (val.compare(0, prefix.size(), prefix) != 0)
            return false;
        size_t dash = val.find('-', prefix.size());
        if (dash == string::npos)
            return false;
        string num = Trim(val.substr(prefix.size(), dash - prefix.size()));
        if (!AllDigits(num))
            return false;
        first = strtoull(num.c_str(), nullptr, 10);
        return true;
    }

    void ProcessHeaderData(const char* data, size_t len)
    {
        string head(data, len);
        size_t eol = head.find("\r\n");
        string statusLine = head.substr(0, eol == string::npos ? head.size() : eol);
        if (!ParseStatusLine(statusLine))
        {
            Fail(drogon::ReqResult::BadResponse, 0, "状态行畸形");
            return;
        }
        if (statusCode_ == 100)
            return;  // 1xx:消费后继续等正式头(headerDone_ 仍 false)

        // 逐行解析(fail-closed:冲突即终结)
        int clCount = 0;
        bool tePresent = false;
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
                    ++clCount;
                    if (!AllDigits(val))
                    {
                        Fail(drogon::ReqResult::BadResponse, 0, "Content-Length 非法");
                        return;
                    }
                    contentLength_ = strtoull(val.c_str(), nullptr, 10);
                    hasContentLength_ = true;
                }
                else if (key == "transfer-encoding")
                {
                    tePresent = true;
                    chunked_ = (ToLowerCopy(val).find("chunked") != string::npos);
                }
                else if (key == "content-range")
                {
                    uint64_t f = 0;
                    hasContentRange_ = ParseContentRangeFirst(val, f);
                    contentRangeFirst_ = f;
                }
                else if (key == "etag")
                {
                    respEtag_ = val;
                }
                else if (key == "last-modified")
                {
                    respLm_ = val;
                }
            }
            pos = lineEnd + 2;
        }
        if (clCount > 1)
        {
            Fail(drogon::ReqResult::BadResponse, 0, "重复 Content-Length");
            return;
        }
        if (hasContentLength_ && chunked_)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, "Content-Length 与 Transfer-Encoding 冲突");
            return;
        }
        headerDone_ = true;

        // 分支表(设计 §15.1):206 校验首字节;200 原连接续读+就地截断;异常重连一次
        if (statusCode_ == 200)
        {
            if (resumedFrom_ > 0)
            {
                // 200 = 全量表示:原连接续读,写线程按序就地截断(不重连)
                EnqueueTruncate();
                cursor_ = 0;
            }
            EnqueueWriteMeta();
        }
        else if (statusCode_ == 206)
        {
            if (!hasContentRange_ || !hasContentLength_)
            {
                Fail(drogon::ReqResult::BadResponse, statusCode_, "206 缺 Content-Range/Content-Length");
                return;
            }
            if (contentRangeFirst_ == resumedFrom_)
            {
                EnqueueWriteMeta();
                cursor_ = resumedFrom_;
            }
            else
            {
                RestartOnce("Content-Range 首字节不符");
                return;
            }
        }
        else if (statusCode_ == 412 || statusCode_ == 416)
        {
            RestartOnce("Range/If 冲突(" + std::to_string(statusCode_) + ")");
            return;
        }
        else if (statusCode_ < 200 || statusCode_ >= 300)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, "非 2xx 响应");
            return;
        }
    }

    /// 200 截断从头 / 206 不符、412、416:断开 → 截断 → 重连一次(发普通 GET)
    void RestartOnce(const string& why)
    {
        if (restartOnceUsed_)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, why + "(二次不符)");
            return;
        }
        restartOnceUsed_ = true;
        headerDone_ = false;
        chunked_ = false;
        hasContentLength_ = false;
        contentLength_ = 0;
        hasContentRange_ = false;
        recvBody_ = 0;
        chunkPendingSize_ = UINT64_MAX;
        chunkTerminated_ = false;
        pendingBuf_.clear();
        respEtag_.clear();
        respLm_.clear();
        metaEtag_.clear();  // 重连发普通 GET,不带 Range/If-Range
        metaLm_.clear();
        EnqueueTruncate();
        resumedFrom_ = 0;
        cursor_ = 0;
        if (client_)
            client_->disconnect();  // 旧连接回调被 epoch 守卫忽略
        conn_.reset();
        BeginConnect();
    }

    void HandleBodyData(const char* data, size_t len)
    {
        if (len == 0 || finished_.load())
            return;
        if (chunked_)
        {
            // 严格 chunked:size 行全 hex(§15.3);数据切块入队;0 终结
            pendingBuf_.append(data, len);
            for (;;)
            {
                if (chunkPendingSize_ == UINT64_MAX)
                {
                    size_t lineEnd = pendingBuf_.find("\r\n");
                    if (lineEnd == string::npos)
                    {
                        if (pendingBuf_.size() > 128)
                            Fail(drogon::ReqResult::BadResponse, statusCode_, "chunk size 行畸形");
                        return;
                    }
                    string sizeLine = pendingBuf_.substr(0, lineEnd);
                    size_t semi = sizeLine.find(';');
                    if (semi != string::npos)
                        sizeLine = sizeLine.substr(0, semi);  // 去扩展
                    if (sizeLine.empty())
                    {
                        Fail(drogon::ReqResult::BadResponse, statusCode_, "chunk size 为空");
                        return;
                    }
                    for (char c : sizeLine)
                    {
                        if (!isdigit((unsigned char)c) && !isxdigit((unsigned char)c))
                        {
                            Fail(drogon::ReqResult::BadResponse, statusCode_, "chunk size 非 hex");
                            return;
                        }
                    }
                    uint64_t sz = strtoull(sizeLine.c_str(), nullptr, 16);
                    if (sz > qCap_)
                    {
                        Fail(drogon::ReqResult::BadResponse, statusCode_, "chunk 超过队列上限");
                        return;
                    }
                    chunkPendingSize_ = sz;
                    pendingBuf_.erase(0, lineEnd + 2);
                }
                if (chunkPendingSize_ == 0)
                {
                    pendingBuf_.clear();  // 忽略 trailer
                    chunkTerminated_ = true;
                    return;
                }
                if (pendingBuf_.size() < (size_t)chunkPendingSize_ + 2)
                    return;  // 等 CRLF+数据
                {
                    size_t remain = (size_t)chunkPendingSize_;
                    size_t off = 0;
                    while (remain > 0)
                    {
                        size_t n = std::min(remain, chunkBytes_);
                        if (!EnqueueWrite(pendingBuf_.data() + off, n))
                            return;
                        if (finished_.load())
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
            recvBody_ += len;
            if (hasContentLength_ && recvBody_ > contentLength_)
            {
                Fail(drogon::ReqResult::BadResponse, statusCode_, "响应体超出 Content-Length");
                return;
            }
            // 块粒度聚合入队
            pendingBuf_.append(data, len);
            while (pendingBuf_.size() >= chunkBytes_)
            {
                if (!EnqueueWrite(pendingBuf_.data(), chunkBytes_))
                    return;
                if (finished_.load())
                    return;
                pendingBuf_.erase(0, chunkBytes_);
            }
        }
    }

    void CheckComplete()
    {
        if (finished_.load())
            return;
        if (chunked_)
        {
            if (chunkTerminated_)
                Complete();
        }
        else if (hasContentLength_)
        {
            if (recvBody_ >= contentLength_)
                Complete();
        }
        // 无 CL:等连接关闭定界(Connection: close 已声明)
    }

    void Complete()
    {
        if (finished_.load())
            return;
        // 收尾:残余未达块粒度的字节先入队(杜绝"残余未落盘")。
        // ⚠ 必须在置位 finished_ 之前入队——终态门会拒绝非终结指令。
        if (!pendingBuf_.empty())
        {
            cursor_ += pendingBuf_.size();  // 残余计入游标(result.written 准确性)
            ZmDlAction w;
            w.kind = ZmDlAction::Kind::Write;
            w.data = std::move(pendingBuf_);
            pendingBuf_.clear();
            EnqueueAction(std::move(w), true);
        }
        if (finished_.exchange(true))
            return;
        if (client_)
            client_->disconnect();
        ZmDlAction fin;
        fin.kind = ZmDlAction::Kind::Finish;
        fin.number = cursor_;
        EnqueueAction(std::move(fin), true);
    }

    void Fail(drogon::ReqResult err, int status, const string& why)
    {
        if (finished_.exchange(true))
            return;
        PUBLIC_LOG_WARN("ZmHttpClient 下载失败(err={},status={},url={},why={})", (int)err, status,
                        url_, why);
        if (client_)
            client_->disconnect();
        CancelPendingFileIo();  // 解除写线程慢盘阻塞(否则 ABORT 永不执行)
        failStatus_ = status;
        failWhy_ = why;  // 先回填再入队(qMtx_ 提供 happens-before)
        ZmDlAction ab;
        ab.kind = ZmDlAction::Kind::Abort;
        EnqueueAction(std::move(ab), true);
    }

    // —— 指令入队(dlLoop 专用) ——
    // force=true:绕过队列上限;Abort/Finish 恒绕过 finished_ 终态门(Fail/Complete 刚置位)。
    // writerStop_(通道关闭)期间一切入队被拒,写线程以"通道关闭"回执。
    bool EnqueueAction(ZmDlAction&& a, bool force)
    {
        size_t sz = a.data.size();
        {
            std::lock_guard lk(qMtx_);
            if (writerStop_)
                return false;
            const bool terminal = (a.kind == ZmDlAction::Kind::Abort ||
                                   a.kind == ZmDlAction::Kind::Finish);
            if (finished_.load() && !terminal)
                return false;
            if (!force && !terminal && qBytes_ + sz > qCap_)
                return false;
            qBytes_ += sz;
            q_.push_back(std::move(a));
        }
        qCv_.notify_one();
        lastActivityMs_ = NowMs();
        return true;
    }

    bool EnqueueWrite(const char* data, size_t n)
    {
        ZmDlAction a;
        a.kind = ZmDlAction::Kind::Write;
        a.data.assign(data, n);
        if (!EnqueueAction(std::move(a), false))
        {
            if (!finished_.load())
                Fail(drogon::ReqResult::NetworkFailure, 0, "写侧积压超限");
            return false;
        }
        cursor_ += n;
        return true;
    }

    bool EnqueueTruncate()
    {
        ZmDlAction a;
        a.kind = ZmDlAction::Kind::Truncate;
        return EnqueueAction(std::move(a), true);
    }

    /// 侧车一次写:仅 {etag,lastModified},无 offset(设计 §15.1)
    void EnqueueWriteMeta()
    {
        ZMJSON j;
        j["etag"] = respEtag_;
        j["lastModified"] = respLm_;
        ZmDlAction a;
        a.kind = ZmDlAction::Kind::WriteMeta;
        a.data = j.dump();
        EnqueueAction(std::move(a), true);
    }

    void ReadSidecar()
    {
        std::ifstream f(destPath_ + ".part.meta", std::ios::binary);
        if (!f)
            return;
        string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        string err;
        ZMJSON j = zm_json_parse(content, err);
        if (!err.empty())
            return;
        if (j.contains("etag") && j["etag"].is_string())
            metaEtag_ = j["etag"].get<string>();
        if (j.contains("lastModified") && j["lastModified"].is_string())
            metaLm_ = j["lastModified"].get<string>();
    }

    // -------------------------------------------------- 写线程(唯一盘上执行者)
    void WriterLoop()
    {
        for (;;)
        {
            ZmDlAction a;
            {
                std::unique_lock lk(qMtx_);
                qCv_.wait(lk, [&] { return writerStop_ || !q_.empty(); });
                if (writerStop_)
                    break;
                qBytes_ -= q_.front().data.size();
                a = std::move(q_.front());
                q_.pop_front();
            }
            switch (a.kind)
            {
                case ZmDlAction::Kind::Write:
                {
                    DWORD written = 0;
                    BOOL ok = WriteFile(file_, a.data.data(), (DWORD)a.data.size(), &written,
                                        nullptr);
                    if (!ok || written != a.data.size())
                    {
                        WriterFail("写盘失败");
                        return;
                    }
                    break;
                }
                case ZmDlAction::Kind::Truncate:
                {
                    LARGE_INTEGER zero;
                    zero.QuadPart = 0;
                    if (!SetFilePointerEx(file_, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(file_))
                    {
                        WriterFail("就地截断失败");
                        return;
                    }
                    break;
                }
                case ZmDlAction::Kind::WriteMeta:
                {
                    // 侧车写失败不中断下载(仅丧失下次续传能力)
                    std::wstring mp = ToW(destPath_ + ".part.meta");
                    HANDLE mf = CreateFileW(mp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (mf != INVALID_HANDLE_VALUE)
                    {
                        DWORD written = 0;
                        WriteFile(mf, a.data.data(), (DWORD)a.data.size(), &written, nullptr);
                        CloseHandle(mf);
                    }
                    else
                    {
                        PUBLIC_LOG_WARN("ZmHttpClient 下载侧车写入失败(续传能力丧失): {}",
                                        destPath_ + ".part.meta");
                    }
                    break;
                }
                case ZmDlAction::Kind::Finish:
                {
                    FinishOk(a.number);
                    return;
                }
                case ZmDlAction::Kind::Abort:
                {
                    AbortPath(failWhy_.empty() ? "下载中止" : failWhy_);
                    return;
                }
            }
        }
        // writerStop_ 退出:中止并回执(在飞数据丢弃,.part 保留)
        WriterFail("通道关闭");
    }

    /// 终态收尾:摘除登记 → 回执。hold 保活至本函数退出(~session 若在此触发,
    /// 析构自检会 detach 写线程自身,线程随后自然退出)。
    void Deliver(ZmHttpClient::ZmDownloadResult&& r)
    {
        std::shared_ptr<ZmDownloadSession> hold;
        {
            std::lock_guard lk(s_sessMtx);
            auto it = s_sessions.find(shared_from_this());
            if (it != s_sessions.end())
            {
                hold = *it;
                s_sessions.erase(it);
            }
        }
        if (done_)
        {
            ZmDoneFn d = std::move(done_);
            done_ = nullptr;
            d(std::move(r));  // 最后触碰外部;hold 保活至本函数退出
        }
    }

    void FinishOk(uint64_t finalWritten)
    {
        finished_.store(true);  // writer 侧终态先置位:dlLoop 侧 Fail 不再入队/取消(句柄随即将关)
        ClosePartFile();
        // 覆写目标:Windows 用 MoveFileExW(REPLACE;.part 与目标同目录保证同卷)
        std::wstring part = ToW(destPath_ + ".part");
        std::wstring dst = ToW(destPath_);
        bool renamed = MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (!renamed)
        {
            DeleteFileW(dst.c_str());  // 退路:删目标重试一次
            renamed = MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }
        if (!renamed)
        {
            PUBLIC_LOG_WARN("ZmHttpClient 下载改名失败(.part→目标),url={}", url_);
            ZmHttpClient::ZmDownloadResult r;
            r.ok = false;
            r.status = statusCode_;
            r.resumedFrom = resumedFrom_;
            r.error = "最后改名失败(.part→目标)";
            Deliver(std::move(r));  // .part 保留
            return;
        }
        DeleteFileW(ToW(destPath_ + ".part.meta").c_str());
        ZmHttpClient::ZmDownloadResult r;
        r.ok = true;
        r.status = statusCode_;
        r.written = finalWritten;
        r.resumedFrom = resumedFrom_;
        Deliver(std::move(r));
    }

    void AbortPath(const string& why)
    {
        finished_.store(true);
        ClosePartFile();  // abort 语义:保留 .part/.meta 供续传(设计 §15.2)
        ZmHttpClient::ZmDownloadResult r;
        r.ok = false;
        r.status = failStatus_;
        r.resumedFrom = resumedFrom_;
        r.error = why;
        Deliver(std::move(r));
    }

    void WriterFail(const string& why)
    {
        finished_.store(true);
        ClosePartFile();  // 保留 .part/.meta
        ZmHttpClient::ZmDownloadResult r;
        r.ok = false;
        r.status = statusCode_;
        r.resumedFrom = resumedFrom_;
        r.error = why;
        Deliver(std::move(r));
    }

    /// 关闭 .part 句柄(写线程终态)。qMtx_ 与 CancelPendingFileIo 互斥,
    /// 保证"读句柄→CancelIoEx"期间句柄不可能被并发关闭/复用。
    void ClosePartFile()
    {
        std::lock_guard lk(qMtx_);
        if (file_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }

    /// 取消句柄上全部未决 I/O(含写线程阻塞中的同步 WriteFile)。
    /// 持 qMtx_ 横跨取消调用:彻底封死"句柄已关/已被复用"竞态
    /// (仅锁内取值或原子化 HANDLE 均不充分——读与取消之间句柄仍可被关)。
    /// 写线程从不持 qMtx_ 阻塞在 WriteFile 上(出锁后执行动作),此处等待有界。
    void CancelPendingFileIo()
    {
        std::lock_guard lk(qMtx_);
        if (file_ != INVALID_HANDLE_VALUE)
            CancelIoEx(file_, nullptr);
    }

    // Fail 的回执补充(入队 ABORT 后写线程读取;mutex 提供可见性)
    int failStatus_ = 0;
    string failWhy_;
};

}  // namespace

// ============================================================================
// 通道静态(设计二期 §15.4;Start/Shutdown 幂等)
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
    std::shared_ptr<trantor::EventLoopThread> t;
    trantor::EventLoop* loop = nullptr;
    {
        std::lock_guard lock(s_dlMtx);
        if (!s_dlRunning)
            return;
        s_dlRunning = false;
        loop = s_dlLoop;
        t.reset(s_dlThread);
        s_dlThread = nullptr;
        s_dlLoop = nullptr;
    }
    // ① 快照在飞会话并请求中止(写线程自行收尾并 Done,线程 = 写线程)
    std::vector<std::shared_ptr<class ZmDownloadSession>> list;
    {
        std::lock_guard lk(s_sessMtx);
        list.assign(s_sessions.begin(), s_sessions.end());
    }
    for (auto& s : list)
        s->RequestShutdown();
    // ② 等登记表排空(写线程 Deliver 自摘;取消不了的写阻塞最多等 3s 后放行)
    for (int spin = 0; spin < 300; ++spin)
    {
        bool empty = false;
        {
            std::lock_guard lk(s_sessMtx);
            empty = s_sessions.empty();
        }
        if (empty)
            break;
        Sleep(10);
    }
    // ③ 退出 dlLoop
    if (loop)
        loop->quit();
    if (t)
        t->wait();
    // ④ 清理残留登记(理论上 Deliver 已自摘)
    {
        std::lock_guard lk(s_sessMtx);
        s_sessions.clear();
    }
}

bool ZmHttpDownloadChannel::IsRunning()
{
    std::lock_guard lock(s_dlMtx);
    return s_dlRunning;
}

bool ZmHttpDownloadChannel::StartDownload(const std::string& url, const std::string& destPath,
                                          ZmHttpClient::ZmHttpRequestOptionsPtr opts, DoneFn done,
                                          std::string* err)
{
    trantor::EventLoop* loop;
    {
        std::lock_guard lock(s_dlMtx);
        if (!s_dlRunning || !s_dlLoop)
        {
            if (err)
                *err = "下载通道未运行";
            return false;
        }
        loop = s_dlLoop;
    }
    string localErr;
    auto session =
        ZmDownloadSession::Create(url, destPath, std::move(opts), std::move(done), localErr);
    if (!session)
    {
        if (err)
            *err = localErr;
        return false;
    }
    {
        std::lock_guard lk(s_sessMtx);
        s_sessions.insert(session);
    }
    loop->runInLoop([session]() { session->StartOnLoop(); });
    return true;
}

// ============================================================================
// DownloadCoro 薄桥(帧内仅指针对齐,值对象堆化)
// ============================================================================
struct ZmDownloadCtx
{
    ZmHttpClient::ZmDownloadResult result;
    bool delivered = false;
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
        string err;
        bool accepted = ZmHttpDownloadChannel::StartDownload(
            ctx->url, ctx->destPath, ctx->opts,
            [this, ctx](ZmHttpClient::ZmDownloadResult&& r) {
                ctx->result = std::move(r);
                Deliver(ctx);
            },
            &err);
        if (!accepted)
        {
            // 通道竞停/文件打开失败:同步收尾(resume 保持为语句块最后一步)
            ctx->result.ok = false;
            ctx->result.status = 0;
            ctx->result.error = err.empty() ? "下载未受理" : err;
            Deliver(ctx);
        }
    }

    ZmHttpClient::ZmDownloadResult await_resume()
    {
        return std::move(ctx_->result);
    }

  private:
    void Deliver(const std::shared_ptr<ZmDownloadCtx>& ctx)
    {
        if (ctx->delivered)
            return;
        ctx->delivered = true;
        // resume 为该语句块最后一步(不得在 resume 后触碰本桥成员)
        if (resumeLoop_ && !resumeLoop_->isInLoopThread())
            resumeLoop_->queueInLoop([h = resumeH_]() { h.resume(); });
        else
            resumeH_.resume();
    }

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
