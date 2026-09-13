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
#include <vector>

using std::string;

// ============================================================================
// 流式下载通道(设计二期 §15)
//  单 EventLoopThread(自启自停)+ 每会话写线程 + 有界指令队列;dlLoop 零磁盘。
//  会话状态机全堆对象,回调链推进;连接代数(epoch)守卫重连窗口。
//  续传:.part 大小即起点;If-Range 变更检测;.part.meta 仅 {etag,lastModified} 一次写。
//  线程纪律:除 Done(写线程)与 Shutdown 外的触碰均在 dlLoop 线程;
//            文件打开/侧车读取提交客户端工作池执行(不占调用方线程)。
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

/// 敏感头判定:跨目标重定向时剥除(名单与普通通道 zm_net_http_client.cpp 一致)
bool IsSensitiveHeaderName(const string& name)
{
    string l = ToLowerCopy(Trim(name));
    return l == "authorization" || l == "cookie" || l == "proxy-authorization";
}

/**
 * @brief 解析重定向位置,得到可再次请求的绝对 URL
 *
 * 支持绝对 URL、协议相对(`//host/path`)、根相对(`/path`)与普通相对路径(含 `.`/`..` 规范化);
 * 相对形态以 baseUrl 的目录部分为参照。
 *
 * @param baseUrl 当前请求的绝对 URL
 * @param loc     Location 头原值(前后空白自动裁掉)
 * @return 绝对 URL;空值或基址非法时返回空串
 */
string ResolveRedirectUrl(const string& baseUrl, const string& loc)
{
    string l = Trim(loc);
    if (l.empty())
        return "";
    if (l.find("http://") == 0 || l.find("https://") == 0)
        return l;
    ZmDownloadTarget base = ParseDownloadUrl(baseUrl);
    if (!base.ok)
        return "";
    string origin = baseUrl.substr(0, baseUrl.find("://") + 3);  // scheme://
    size_t authEnd = baseUrl.find('/', origin.size());
    string authority = (authEnd == string::npos)
                           ? baseUrl.substr(origin.size())
                           : baseUrl.substr(origin.size(), authEnd - origin.size());
    if (l.find("//") == 0)
        return origin + l.substr(2);  // 协议相对:换主机,沿用当前 scheme
    string path = base.pathQuery;
    size_t q = path.find('?');
    if (q != string::npos)
        path = path.substr(0, q);
    size_t lastSlash = path.find_last_of('/');
    string dir = (lastSlash == string::npos || lastSlash == 0) ? "/" : path.substr(0, lastSlash + 1);
    string join = (l[0] == '/') ? l : dir + l;

    // 逐段规范化:丢弃空段与 "."、遇 ".." 回退一段
    std::vector<string> segs;
    size_t i = 0;
    while (i <= join.size())
    {
        size_t ns = join.find('/', i);
        string seg = (ns == string::npos) ? join.substr(i) : join.substr(i, ns - i);
        if (seg == "..")
        {
            if (!segs.empty())
                segs.pop_back();
        }
        else if (!seg.empty() && seg != ".")
        {
            segs.push_back(seg);
        }
        if (ns == string::npos)
            break;
        i = ns + 1;
    }
    string norm = "/";
    for (size_t k = 0; k < segs.size(); ++k)
    {
        if (k > 0)
            norm += "/";
        norm += segs[k];
    }
    return origin + authority + norm;
}

/// 请求行/头名 fail-closed:拒绝裸空格与控制字符(防请求行与头名注入)
bool IsSafeRequestBytes(const string& s)
{
    for (unsigned char c : s)
    {
        if (c <= 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

/**
 * @brief 头值白名单:仅拒绝能造成请求注入的字符
 *
 * 空格与水平制表符是合法头值内容(Accept: text/html、charset=utf-8 等),
 * 误拒会让同一份头配置"普通通道能发、下载通道全挂";高位字节按 obs-text 放行。
 *
 * @param s 待校验的头值
 * @return true 可安全拼进请求;false 含 CR/LF/NUL 等控制字符
 */
bool IsSafeHeaderValue(const string& s)
{
    for (unsigned char c : s)
    {
        if ((c < 0x20 && c != 0x09) || c == 0x7F)
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

    /**
     * @brief 构造会话:解析 URL → 打开 .part(定起点/截断)→ 读侧车 → 起写线程
     *
     * 全程磁盘操作,须在客户端工作池线程执行(不得占用事件循环线程)。
     *
     * @param url      下载源(仅 http/https)
     * @param destPath 目标文件路径(过程文件为 destPath + ".part")
     * @param opts     逐请求选项(可空 = 取全局默认)
     * @param done     终态回调(写线程调用,恰一次)
     * @param loop     本会话所在的下载通道 loop(固化到会话,不读后续全局状态)
     * @param err      失败原因回填
     * @return 会话对象;失败返回 nullptr(不注册、不起写线程)
     */
    static std::shared_ptr<ZmDownloadSession> Create(const string& url, const string& destPath,
                                                     ZmHttpClient::ZmHttpRequestOptionsPtr opts,
                                                     ZmDoneFn done, trantor::EventLoop* loop,
                                                     string& err)
    {
        const auto& def = ZmHttpClient::GetOptions();
        auto s = std::shared_ptr<ZmDownloadSession>(
            new ZmDownloadSession(url, destPath, std::move(opts), std::move(done)));
        s->loop_ = loop;
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
        {
            if (!IsSafeRequestBytes(kv.first))
            {
                err = "公共头名含裸空格/控制字符: " + kv.first;
                return nullptr;
            }
            if (!IsSafeHeaderValue(kv.second))
            {
                err = "公共头值含控制字符: " + kv.first;
                return nullptr;
            }
        }
        if (s->opts_)
        {
            for (const auto& kv : s->opts_->headers)
            {
                if (!IsSafeRequestBytes(kv.first))
                {
                    err = "请求头名含裸空格/控制字符: " + kv.first;
                    return nullptr;
                }
                if (!IsSafeHeaderValue(kv.second))
                {
                    err = "请求头值含控制字符: " + kv.first;
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
        s->lastWriteMs_ = s->lastActivityMs_;

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
    trantor::EventLoop* loop_ = nullptr;  // 本会话的下载 loop(创建时固化,不读全局)
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
    int redirectsUsed_ = 0;        // 已跟随的重定向次数(上限走全局 maxRedirects)
    bool stripSensitive_ = false;  // 跨目标跳转后剥除 Authorization/Cookie

    // 响应解析(fail-closed)
    bool headerDone_ = false;
    int statusCode_ = 0;
    bool chunked_ = false;
    bool hasContentLength_ = false;
    uint64_t contentLength_ = 0;
    bool hasContentRange_ = false;
    uint64_t contentRangeFirst_ = 0;
    string respLocation_;  // Location 头(3xx 跟随用;随响应解析重置)
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
    int64_t lastWriteMs_ = 0;  // 写线程最后取走指令的时刻(停滞看护的写侧判据;受 qMtx_ 保护)

    // -------------------------------------------------- dlLoop 流程
    void BeginConnect()
    {
        if (finished_.load())
            return;
        ++epoch_;
        const uint64_t ep = epoch_;
        if (!resolver_)
            resolver_ = trantor::Resolver::newResolver(loop_, 30);
        std::weak_ptr<ZmDownloadSession> weak = shared_from_this();
        resolver_->resolve(tgt_.host, [weak, ep](const trantor::InetAddress& addr) {
            auto self = weak.lock();
            if (!self || self->epoch_ != ep || self->finished_.load())
                return;
            trantor::InetAddress a = addr;
            a.setPortNetEndian(htons(self->tgt_.port));  // Resolver 回传端口恒 0,须回填
            self->OnResolved(a, ep);
        });
    }

    void OnResolved(const trantor::InetAddress& addr, uint64_t ep)
    {
        // 四个回调一律持弱引用:锁不上即会话已终结,直接丢弃。
        // (强引用会与 client_ 成员形成引用环,令会话连同写线程永久泄漏)
        std::weak_ptr<ZmDownloadSession> weak = shared_from_this();
        client_ = std::make_shared<trantor::TcpClient>(loop_, addr, "zm-download");
        client_->setMessageCallback([weak, ep](const trantor::TcpConnectionPtr& c,
                                               trantor::MsgBuffer* buf) {
            auto self = weak.lock();
            if (!self || self->epoch_ != ep || self->finished_.load())
                return;
            self->OnRecv(c, buf);
        });
        client_->setConnectionCallback([weak, ep](const trantor::TcpConnectionPtr& conn) {
            auto self = weak.lock();
            if (!self || self->epoch_ != ep || self->finished_.load())
                return;
            if (conn->connected())
                self->OnConnected(conn);
            else
                self->OnDisconnect();
        });
        client_->setConnectionErrorCallback([weak, ep]() {
            auto self = weak.lock();
            if (!self || self->epoch_ != ep || self->finished_.load())
                return;
            self->OnConnectError();
        });
        client_->setSSLErrorCallback([weak, ep](trantor::SSLError) {
            auto self = weak.lock();
            if (!self || self->epoch_ != ep || self->finished_.load())
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
        {
            req += "Range: bytes=" + std::to_string(resumedFrom_) + "-\r\n";
            if (!metaEtag_.empty())
                req += "If-Range: " + metaEtag_ + "\r\n";
            else if (!metaLm_.empty())
                req += "If-Range: " + metaLm_ + "\r\n";
            else
                PUBLIC_LOG_WARN("ZmHttpClient 下载续传无 If-Range 校验值(降级为仅 "
                                "Content-Range 起点校验): {}",
                                destPath_);
        }
        req += "Connection: close\r\n";
        req += "Accept-Encoding: identity\r\n";
        for (const auto& kv : def.commonHeaders)
        {
            if (stripSensitive_ && IsSensitiveHeaderName(kv.first))
                continue;
            req += kv.first + ": " + kv.second + "\r\n";
        }
        if (opts_)
            for (const auto& kv : opts_->headers)
            {
                if (stripSensitive_ && IsSensitiveHeaderName(kv.first))
                    continue;
                req += kv.first + ": " + kv.second + "\r\n";
            }
        req += "\r\n";
        conn->send(req);

        // 停滞看护(仅一次;自链 timer,finished_ 后自止)
        if (!guardStarted_)
        {
            guardStarted_ = true;
            std::weak_ptr<ZmDownloadSession> weak = shared_from_this();
            loop_->runAfter(0.25, [weak]() {
                if (auto self = weak.lock())
                    self->OnGuard();
            });
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

    // —— 停滞看护:读侧无收包且写侧无进展(队列空,或写线程久未取走指令)→ 放弃 ——
    // 写侧判据须能独立成立:写线程挂在网络盘上时队列恒非空,只看"队列空"会让会话永挂。
    void OnGuard()
    {
        if (finished_.load())
            return;
        int64_t now = NowMs();
        uint64_t qb = 0;
        int64_t lastWrite = 0;
        {
            std::lock_guard lk(qMtx_);
            qb = qBytes_;
            lastWrite = lastWriteMs_;
        }
        bool readStalled = (now - lastActivityMs_ > stallAbortMs_);
        bool writeStalled = (qb == 0) || (now - lastWrite > stallAbortMs_);
        if (readStalled && writeStalled)
        {
            Fail(drogon::ReqResult::Timeout, statusCode_, "对端停滞(超时无进展)");
            return;
        }
        std::weak_ptr<ZmDownloadSession> weak = shared_from_this();
        loop_->runAfter(0.25, [weak]() {
            if (auto self = weak.lock())
                self->OnGuard();
        });
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
                else if (key == "content-encoding")
                {
                    // 请求侧已声明 identity:对端仍压缩 → 落盘即坏字节,当场终结
                    string enc = ToLowerCopy(val);
                    if (!enc.empty() && enc != "identity")
                    {
                        Fail(drogon::ReqResult::BadResponse, statusCode_,
                             "响应体被压缩(Content-Encoding: " + val + ")");
                        return;
                    }
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
                else if (key == "location")
                {
                    respLocation_ = val;
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

        // 分支表:206 校验首字节;200 原连接续读+就地截断;3xx 换目标重连;异常重连一次
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
        else if (statusCode_ >= 300 && statusCode_ < 400)
        {
            FollowRedirect();
            return;
        }
        else if (statusCode_ < 200 || statusCode_ >= 300)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, "非 2xx 响应");
            return;
        }
    }

    /// 重置"当前响应"的解析状态(续传基线 resumedFrom_/cursor_ 不在此列)
    void ResetResponseState()
    {
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
        respLocation_.clear();
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
        ResetResponseState();
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

    /**
     * @brief 跟随 3xx:解析 Location → 换目标重连(仅 dlLoop 线程调用)
     *
     * 续传语义保留:Range/If-Range 照发,由对端以 200(全量,就地截断)或 206 收敛。
     * 跨目标跳转后剥除敏感头;`opts.followRedirect=false`、Location 非法或次数超限时按失败终结。
     */
    void FollowRedirect()
    {
        if (statusCode_ == 304)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, "304 无响应体(未发条件请求)");
            return;
        }
        if (opts_ && !opts_->followRedirect)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_,
                 std::to_string(statusCode_) + " 重定向未跟随(followRedirect=false)");
            return;
        }
        string nextUrl = ResolveRedirectUrl(url_, respLocation_);
        ZmDownloadTarget next = nextUrl.empty() ? ZmDownloadTarget() : ParseDownloadUrl(nextUrl);
        if (!next.ok)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, "重定向 Location 缺失或非法");
            return;
        }
        const auto& def = ZmHttpClient::GetOptions();
        int maxRed = def.maxRedirects > 0 ? def.maxRedirects : 5;
        if (redirectsUsed_ >= maxRed)
        {
            Fail(drogon::ReqResult::BadResponse, statusCode_, "重定向次数超限");
            return;
        }
        ++redirectsUsed_;
        if (next.ssl != tgt_.ssl || next.host != tgt_.host || next.port != tgt_.port)
            stripSensitive_ = true;  // 跨目标:此后不再外发 Authorization/Cookie
        tgt_ = next;
        url_ = nextUrl;
        ResetResponseState();
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
                if (pendingBuf_[(size_t)chunkPendingSize_] != '\r' ||
                    pendingBuf_[(size_t)chunkPendingSize_ + 1] != '\n')
                {
                    Fail(drogon::ReqResult::BadResponse, statusCode_, "chunk 数据块后无 CRLF");
                    return;
                }
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
        // 回填须早于解除写阻塞:被 CancelIoEx 唤醒的写线程会立刻读 failWhy_ 回执,
        // 晚于取消就只剩"下载中止"。赋值放进锁内,读取方(写线程)也在同一把锁上取
        {
            std::lock_guard lk(qMtx_);
            failStatus_ = status;
            failWhy_ = why;
        }
        CancelPendingFileIo();  // 解除写线程慢盘阻塞(否则 ABORT 永不执行)
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
        // 路径 UTF-8 契约:窄串直接进 fstream 在中文路径下静默失败(丢失 If-Range 校验值)
        std::ifstream f(ToW(destPath_ + ".part.meta").c_str(), std::ios::binary);
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
                lastWriteMs_ = NowMs();  // 写线程存活证据(停滞看护据此判写侧无进展)
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
                        // CancelIoEx 解除的阻塞写不是磁盘故障:按通道的真实原因回执,
                        // 否则 Fail 报出的原因(对端停滞/积压超限等)会被"写盘失败"顶掉
                        if (!ok && GetLastError() == ERROR_OPERATION_ABORTED)
                        {
                            bool stopping = false;
                            string why;
                            {
                                std::lock_guard lk(qMtx_);
                                stopping = writerStop_;  // 通道关闭取消的写
                                why = failWhy_;          // Fail 已回填的中止原因
                            }
                            if (stopping)
                                WriterFail("通道关闭");
                            else
                                AbortPath(why.empty() ? "下载中止" : why);
                            return;
                        }
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
        // 不做"删目标重试"的退路:删成功而改名再失败,会同时丢掉用户既有文件与本次结果
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

// ----------------------------------------------------------------------------
// 通道级会话收尾(Shutdown 用)
// ----------------------------------------------------------------------------

/// 快照在飞会话并逐一请求中止(任意线程;写线程自行收尾并 Done)
void RequestShutdownAllSessions()
{
    std::vector<std::shared_ptr<ZmDownloadSession>> list;
    {
        std::lock_guard lk(s_sessMtx);
        list.assign(s_sessions.begin(), s_sessions.end());
    }
    for (auto& s : list)
        s->RequestShutdown();
}

/**
 * @brief 等会话登记表排空
 *
 * @param spinMax 轮询次数(每轮 10ms)
 * @return true 已排空;false 轮询耗尽仍有在飞会话(写盘阻塞等),由调用方放行
 */
bool WaitSessionsDrained(int spinMax)
{
    for (int spin = 0; spin < spinMax; ++spin)
    {
        {
            std::lock_guard lk(s_sessMtx);
            if (s_sessions.empty())
                return true;
        }
        Sleep(10);
    }
    return false;
}

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
    RequestShutdownAllSessions();
    // ② 等登记表排空(写线程 Deliver 自摘;取消不了的写阻塞最多等 3s 后放行)
    WaitSessionsDrained(300);
    // ③ 退出 dlLoop
    if (loop)
        loop->quit();
    if (t)
        t->wait();
    // ④ 兜底:① 快照之后才完成登记的会话在此收敛(受理与快照不同锁域),随后清表
    RequestShutdownAllSessions();
    WaitSessionsDrained(300);
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
    // 打开 .part/读侧车/起写线程都是磁盘操作:提交客户端工作池执行,不占调用方线程
    // (调用方通常是服务器事件循环线程)。受理结果同步返回;会话级失败经 done 回执。
    auto optsHold = std::move(opts);
    bool submitted = ZmHttpClient::SubmitBlockingTask([=]() {
        string localErr;
        auto session = ZmDownloadSession::Create(url, destPath, optsHold, done, loop, localErr);
        if (!session)
        {
            if (done)
            {
                ZmHttpClient::ZmDownloadResult r;
                r.ok = false;
                r.error = localErr.empty() ? "下载未受理" : localErr;
                done(std::move(r));
            }
            return;
        }
        {
            std::lock_guard lk(s_sessMtx);
            s_sessions.insert(session);
        }
        // 受理后通道已停(与 Shutdown 的竞态窗口):自行叫停,写线程醒来走失败路径
        {
            std::lock_guard lock(s_dlMtx);
            if (!s_dlRunning)
            {
                session->RequestShutdown();
                return;
            }
            // 入队须与 s_dlRunning 检查同锁域:出锁后 Shutdown 可能已把 loop 连同线程销毁
            loop->runInLoop([session]() { session->StartOnLoop(); });
        }
    });
    if (!submitted && err)
        *err = "下载通道未运行(工作池已停)";
    return submitted;
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
