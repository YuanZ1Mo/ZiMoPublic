#ifndef NOMINMAX   // windows.h 的 min/max 宏会破坏 std::min/max;须在一切 include 前定义
#define NOMINMAX
#endif
#include "zm_net_http_client.h"
#include "zm_net_http_client_download.h"

#include "../util/zm_util_logger.h"

#include <../drogon/include/drogon/HttpTypes.h>
#include <../drogon/include/drogon/utils/coroutine.h>

#include <../drogon/include/trantor/net/EventLoop.h>
#include <../drogon/include/trantor/net/EventLoopThreadPool.h>

#include <windows.h>  // MultiByteToWideChar(LoadUploadFile UTF-8 路径契约);trantor 先行铁律

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace drogon;
using std::string;
using std::to_string;

// ============================================================================
// 出站 HTTP/HTTPS 客户端门面 普通 lane:HttpClient-Lane(EventLoopThreadPool) + per-target 池 + 三形态 +
//             超时/重试/重定向/日志统计. 下载通道:见 zm_net_http_client_download.*.
// ============================================================================
namespace
{
enum class ZmClientState
{
    Uninit,       // 进程启动默认态
    Initialized,  // Init 完成,可发送
    Closed        // Close 完成,终态
};

std::mutex s_stateMtx;
ZmClientState s_state = ZmClientState::Uninit;
ZmHttpClient::Options s_opts;

// 已登记 loop(客户端自身 lane + 服务器各 loop;供 SendSync 拒绝/Close 自锁检查)
std::mutex s_loopMtx;
std::unordered_set<trantor::EventLoop*> s_loops;

// ----------------------------------------------------------------------------
// 连接池(key = scheme://host:port)
// ----------------------------------------------------------------------------
struct ZmPoolEntry
{
    string key;
    string host;   // 小写(不含 [])
    uint16_t port = 0;
    bool ssl = false;
    trantor::EventLoop* loop = nullptr;
    std::vector<drogon::HttpClientPtr> clients;  // ≤ maxConnPerHost
    uint64_t createSeq = 0;
};

std::mutex s_poolMtx;
std::unordered_map<string, std::shared_ptr<ZmPoolEntry>> s_pools;
uint64_t s_poolCreateSeq = 0;
trantor::EventLoopThreadPool* s_lanePool = nullptr;
// 客户端自持阻塞工作池(multipart 组装/上传读盘/下载通道的磁盘段).
// 原子指针:Close 置空与各提交点读取跨线程并发,普通指针是未同步读写.
// 对象本身有意不析构,故读到旧的非空指针仍指向存活对象,仅"读到空"表示已停.
std::atomic<ZmThreadPool*> s_workPool{nullptr};

// 统计(全局计数;per-target 明细未提供)
std::atomic<uint64_t> s_statRequests{0};
std::atomic<uint64_t> s_statOk{0};
std::atomic<uint64_t> s_statStatus4xx{0};  // 传输成功但 4xx(ok 语义 = 传输成功)
std::atomic<uint64_t> s_statBadResponse{0};
std::atomic<uint64_t> s_statTimeout{0};
std::atomic<uint64_t> s_statNetworkErr{0};
std::atomic<uint64_t> s_statBytesIn{0};

// ----------------------------------------------------------------------------
// 基元
// ----------------------------------------------------------------------------

ZmHttpResult MakeErrorResult(drogon::ReqResult err, int status)
{
    ZmHttpResult r;
    r.err = err;
    r.status = status;
    return r;
}

/// @return 去掉首尾空白(空格/制表/CR/LF)的副本
string Trim(const string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// @return s 的 ASCII 小写副本
string ToLowerCopy(string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

/// 域名含 ':' 时为 IPv6,加 [] 返回
string BracketHost(const string& host)
{
    if (host.find(':') != string::npos)
        return "[" + host + "]";
    return host;
}

// ----------------------------------------------------------------------------
// URL 解析(key 规范化)
// ----------------------------------------------------------------------------
struct ZmUrlTarget
{
    bool ok = false;
    bool ssl = false;
    string host;       // 小写
    uint16_t port = 0; // 有效端口(缺省 80/443)
    string path;       // "/path?query"(无 fragment)
    string key;        // scheme://host:port
};

/// path 白名单:pathEncode(false) 后按原样上线,
/// 裸空格/控制字符/畸形 %XX 一律拒绝(UTF-8 字节 >=0x80 允许)
bool IsSafePath(const string& path)
{
    for (size_t i = 0; i < path.size(); ++i)
    {
        unsigned char c = (unsigned char)path[i];
        if (c <= 0x20 || c == 0x7F)
            return false;
        if (c == '%')
        {
            if (i + 2 >= path.size() || !isxdigit((unsigned char)path[i + 1]) ||
                !isxdigit((unsigned char)path[i + 2]))
                return false;
            i += 2;
        }
    }
    return true;
}

/**
 * @brief 解析绝对 URL 并规范化连接池键
 *
 * path 走白名单校验:裸空格/控制字符/畸形 %XX 一律判非法.
 *
 * @param raw 原始 URL(可含 userinfo 与 fragment;fragment 忽略)
 * @return 解析结果;ok=false 表示 scheme/主机/端口/path 不合法
 */
ZmUrlTarget ParseUrl(const string& raw)
{
    ZmUrlTarget t;
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

    size_t slashPos = rest.find('/');
    string authority = (slashPos == string::npos) ? rest : rest.substr(0, slashPos);
    t.path = (slashPos == string::npos) ? "/" : rest.substr(slashPos);

    // userinfo 仅剥离(暂不做 Basic Auth 自动头)
    size_t at = authority.find_last_of('@');
    if (at != string::npos)
        authority = authority.substr(at + 1);

    string host;
    string portStr;
    if (!authority.empty() && authority[0] == '[')  // IPv6:[::1]:port
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
            if (!std::isdigit((unsigned char)c))
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
    t.key = scheme + "://" + host + ":" + to_string(p);
    if (!IsSafePath(t.path))
        return t;  // path 白名单不过 → BadServerAddress
    t.ok = true;
    return t;
}

/// RFC 3986 简化解析(相对/绝对/协议相对);无效返回空串
string ResolveRedirect(const string& baseUrl, const string& loc)
{
    string l = Trim(loc);
    if (l.empty())
        return "";
    if (l.find("http://") == 0 || l.find("https://") == 0)
        return l;
    ZmUrlTarget base = ParseUrl(baseUrl);
    if (!base.ok)
        return "";
    if (l.find("//") == 0)  // scheme-relative
    {
        size_t end = baseUrl.find("://");
        return baseUrl.substr(0, end) + ":" + l;  // scheme: + "//host..."
    }
    // relative:以基址目录合并,规范化 '.'/'..'
    size_t schemeEnd = baseUrl.find("://") + 3;
    size_t authEnd = baseUrl.find('/', schemeEnd);
    if (authEnd == string::npos)
        authEnd = baseUrl.size();
    string dir = base.path;
    size_t lastSlash = dir.find_last_of('/');
    dir = (lastSlash == string::npos || lastSlash == 0) ? "/" : dir.substr(0, lastSlash + 1);
    string join = (l[0] == '/') ? l : dir + l;

    std::vector<string> segs;
    {
        size_t i = 0;
        while (i <= join.size())
        {
            size_t ns = join.find('/', i);
            string seg = (ns == string::npos) ? join.substr(i) : join.substr(i, ns - i);
            if (seg == ".")
            {
                // 跳过
            }
            else if (seg == "..")
            {
                if (!segs.empty())
                    segs.pop_back();
            }
            else if (!seg.empty())
            {
                segs.push_back(seg);
            }
            if (ns == string::npos)
                break;
            i = ns + 1;
        }
    }
    string norm = "/";
    for (size_t i = 0; i < segs.size(); ++i)
    {
        if (i > 0)
            norm += "/";
        norm += segs[i];
    }
    return baseUrl.substr(0, authEnd) + norm;
}

// ----------------------------------------------------------------------------
// 超时/幂等/退避
// ----------------------------------------------------------------------------

double ResolveTimeout(const ZmHttpRequestOptions& o)
{
    double t = o.timeoutSec;
    if (t == 0)
        t = s_opts.defaultTimeoutSec;  // 0 = 全局默认
    if (t < 0)
        t = 0;  // -1 = 不超时(drogon 0 = 禁用超时)
    return t;
}

/// @return 该方法是否天然幂等(可安全重试)
bool IsIdempotentMethod(drogon::HttpMethod m)
{
    switch (m)
    {
        case drogon::HttpMethod::Get:
        case drogon::HttpMethod::Head:
        case drogon::HttpMethod::Put:
        case drogon::HttpMethod::Options:
        case drogon::HttpMethod::Delete:
            return true;
        default:
            return false;
    }
}

/**
 * @brief 计算下一次重试前的等待毫秒数
 *
 * Retry-After 优先(整秒或 HTTP-date 两种格式,与 capMs 取 min);
 * 无该头或格式不可解析时用 base·2^n 加 ±jitter,封顶 capMs.
 *
 * @param resp       上一次响应(可空)
 * @param retryIndex 已重试次数(0 起)
 * @param baseMs     指数退避基数(毫秒)
 * @param capMs      退避上限(毫秒)
 * @param jitter     抖动幅度(0 = 不加抖动)
 * @return 等待毫秒数
 */
double RetryDelayMs(const drogon::HttpResponsePtr& resp, int retryIndex,
                    double baseMs, double capMs, double jitter)
{
    if (resp)
    {
        string ra = Trim(string(resp->getHeader("Retry-After")));
        if (!ra.empty())
        {
            char* end = nullptr;
            double v = std::strtod(ra.c_str(), &end);
            if (end != ra.c_str() && v > 0)
                return std::min(v * 1000.0, capMs);
            // HTTP-date 形态(RFC 7231 IMF-fixdate):"Sun, 06 Nov 1994 08:49:37 GMT"
            std::tm tm{};
            std::istringstream ss(ra);
            ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
            if (!ss.fail())
            {
                time_t target = _mkgmtime(&tm);
                if (target > 0)
                {
                    double ms = (double)(target - std::time(nullptr)) * 1000.0;
                    if (ms > 0)
                        return std::min(ms, capMs);
                }
            }
        }
    }
    double d = std::min(baseMs * (double)(1ULL << std::min(retryIndex, 30)), capMs);
    if (jitter > 0)
    {
        static thread_local std::mt19937 rng(std::random_device{}());  // 各线程独立,免数据竞争
        std::uniform_real_distribution<double> u(1.0 - jitter, 1.0 + jitter);
        d *= u(rng);
    }
    return d;
}

// ----------------------------------------------------------------------------
// 敏感头(跨域跳转时剥除)
// ----------------------------------------------------------------------------
bool IsSensitiveHeader(const string& name)
{
    string l = ToLowerCopy(name);
    return l == "authorization" || l == "cookie" || l == "proxy-authorization";
}

// 状态机前向声明(定义见下方"连接池实现"/"请求组装"小节;供内联成员函数体使用)
using ZmHeaderList = std::vector<std::pair<string, string>>;  // 与下方定义同类型(重复声明合法)
/// 取/建池并择路(实现见"连接池实现"小节)
drogon::HttpClientPtr GetPoolClient(const ZmUrlTarget& t,
                                    const drogon::HttpClientPtr& avoid = nullptr);
/// 把连接层失败过的 client 移出池(池空则新建替换)
void RemovePoolClient(const string& key, const drogon::HttpClientPtr& bad);
/// 组装请求(方法/path/头/body;实现见"请求组装"小节)
void PrepareRequest(drogon::HttpRequestPtr& req, drogon::HttpMethod m, const ZMJSON& jsonBody,
                    const string* rawBody, const string& rawContentType,
                    const ZmHeaderList& headers, const ZmUrlTarget& tgt, bool stripSensitive);

// ----------------------------------------------------------------------------
// 编排状态机(本文件唯一实现)
//
// 为何用回调状态机而非协程循环(设计约束):"协程帧内持有复杂值对象(map/string)被
// 网络回调线程跨线程 resume"形态,在本工具链(MSVC 14.44 /std:c++20)下会稳定触发
// 访问违例(崩在帧内容器拷贝构造).本实现从结构上避免该形态--
// ① 重试/重定向/多尝试循环 = drogon 响应回调驱动的**堆上状态机**(shared_ptr 持有,
//    map/string/vector 归属堆对象),方法在"发起线程/回调线程/lane loop"间以
//    回调链自我推进,任何时刻不存在"跨线程 resume 帧内含复杂对象的协程";
// ② 协程侧仅 ZmMachineAwaiter 薄桥(帧内指针对齐成员;值对象堆化 ZmMachineCtx);
// ③ 收尾回调严格一次(per-attempt 原子 once + Finish 终态).
// ----------------------------------------------------------------------------
class ZmSendMachine;  // fwd

// 状态机主体的对外完成通道:恰好一次回调(线程 = 响应回调线程/lane loop)
using ZmMachineDoneFn = std::function<void(ZmHttpResult&&)>;

class ZmSendMachine : public std::enable_shared_from_this<ZmSendMachine>
{
  public:
    using RequestOptionsPtr = ZmHttpClient::ZmHttpRequestOptionsPtr;
    // 可选的离核预组装(上传读盘+multipart 组装;在工作池执行;成功须回填 m_curRaw/ m_curContentType)
    using PreludeFn = std::function<bool(ZmSendMachine&, std::string& errMsg)>;

    /// 注:所有大容器(method/url/body/raw/contentType/m_opts/headers 快照)均归本堆对象;
    /// 协程帧与回调 lambda 只持 shared_ptr/裸指针.
    ZmSendMachine(drogon::HttpMethod m0, std::string url0, ZMJSON body0, std::string raw0,
                  std::string contentType0, bool rawProvided0, RequestOptionsPtr opts0,
                  PreludeFn prelude0, ZmMachineDoneFn done0)
       : m_originalMethod(m0), m_originalUrl(std::move(url0)), m_curMethod(m0),
          m_currentUrl(m_originalUrl), m_lastUrl(m_currentUrl), m_curBody(std::move(body0)),
          m_curRaw(std::move(raw0)), m_curContentType(std::move(contentType0)),
          m_rawProvided(rawProvided0), m_opts(std::move(opts0)),
          m_prelude(std::move(prelude0)), m_onDone(std::move(done0))
    {
    }

    /// 在调用方线程启动(main/业务线程或上游回调线程均可;m_prelude 存在时先离核)
    void Start()
    {
        const auto& def = ZmHttpClient::GetOptions();
        m_retryBudget = (m_opts->retryCount >= 0) ? m_opts->retryCount: def.retryMax;
        m_totalBudget = def.maxTotalAttempts > 0 ? def.maxTotalAttempts: 8;
        m_headerSnapshot.clear();
        m_headerSnapshot.reserve(def.commonHeaders.size() + m_opts->headers.size());
        for (const auto& kv : def.commonHeaders)
            m_headerSnapshot.push_back(kv);
        for (const auto& kv: m_opts->headers)
            m_headerSnapshot.push_back(kv);
        m_t0 = std::chrono::steady_clock::now();

        if (m_prelude)
        {
            ZmThreadPool* pool = s_workPool.load();  // 取一次即用,避免"查后再取"读两遍
            if (!pool)
            {
                FinishErr(drogon::ReqResult::NetworkFailure, 0);  // Close 竞态窄窗(工作池已停)
                return;
            }
            auto self = shared_from_this();
            pool->Submit([self]() { self->RunPreludeOnPool(); });
        }
        else
        {
            // 首跳异步化:await_suspend 栈上不存在同步 Finish
            trantor::EventLoop* disp =
                (s_lanePool && ZmHttpClient::IsReady()) ? s_lanePool->getLoop(0) : nullptr;
            if (!disp)
            {
                FinishErr(drogon::ReqResult::NetworkFailure, 0);  // Close 竞态窄窗
                return;
            }
            auto self = shared_from_this();
            disp->queueInLoop([self]() { self->NextAttempt(); });
        }
    }

    // ── 预组装产物(上传通道工作池回填;其余为空) ──
    std::string m_curRaw;
    std::string m_curContentType;

  private:
    /// 工作池线程:执行离核预组装并接续首跳;异常一律按网络失败终结
    void RunPreludeOnPool()
    {
        try
        {
            std::string err;
            if (!m_prelude(*this, err))
            {
                PUBLIC_LOG_WARN("ZmHttpClient 离核预组装失败: {}", err);
                FinishErr(drogon::ReqResult::NetworkFailure, 0);
            }
            else
                NextAttempt();  // 线程安全:状态机仅经 drogon queueInLoop 与池快照序列化触达共享面
        }
        catch (...)
        {
            PUBLIC_LOG_WARN("ZmHttpClient 离核预组装异常");
            FinishErr(drogon::ReqResult::NetworkFailure, 0);
        }
    }

    // -- 一次尝试(组装 → 发起;回调 = 本机自我推进) --
    void NextAttempt()
    {
        if (m_finished)
            return;
        if (!ZmHttpClient::IsReady())
        {
            // Close 竞态:拒绝在关闭后建池/回退 app loop
            FinishErr(drogon::ReqResult::NetworkFailure, 0);
            return;
        }
        if (m_attemptUsed >= m_totalBudget)  // 预算耗尽:返回上一次结果(与旧循环出口一致)
        {
            Finish(std::move(m_attemptResult));
            return;
        }
        ++m_attemptUsed;

        ZmUrlTarget tgt = ParseUrl(m_currentUrl);
        if (!tgt.ok)
        {
            FinishErr(drogon::ReqResult::BadServerAddress, 0);
            return;
        }
        m_lastTargetKey = tgt.key;
        // 避免上一条连接层失败过的将死连接(见 m_lastFailedClient 注);拿到健康连接后清除标记
        auto client = GetPoolClient(tgt, m_lastFailedClient);
        if (!client)
        {
            FinishErr(drogon::ReqResult::NetworkFailure, 0);
            return;
        }
        m_lastClient = client;    // 保持连接在退避/重定向等待期存活(防池逐出断链)
        m_lastLoop = client->getLoop();
        m_lastFailedClient.reset();

        auto req = drogon::HttpRequest::newHttpRequest();
        PrepareRequest(req, m_curMethod, m_curBody, m_rawProvided ? &m_curRaw: nullptr,
                       m_curContentType, m_headerSnapshot, tgt, m_stripSensitive);

        auto self = shared_from_this();
        auto armed = std::make_shared<std::atomic<bool>>(false);
        client->sendRequest(
            req,
            [self, armed](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
                self->OnAttemptResponse(armed, r, resp);
            },
            ResolveTimeout(*m_opts));
    }

    /// 单次尝试的响应入口:先过兜底 once,再进决策;异常按网络失败终结
    /// @param armed 本次尝试的一次性开关(库版本漂移下防双发)
    void OnAttemptResponse(const std::shared_ptr<std::atomic<bool>>& armed, drogon::ReqResult r,
                           const drogon::HttpResponsePtr& resp)
    {
        // drogon 1.9.13 官方已保证回调恰好一次(timeoutFlag),原子开关再防一层
        if (armed->exchange(true))
            return;
        try
        {
            OnResponse(r, resp);
        }
        catch (...)
        {
            FinishErr(drogon::ReqResult::NetworkFailure, 0);
        }
    }

    // -- 单次响应的决策(超限/重试/重定向/终结) --
    void OnResponse(drogon::ReqResult r, const drogon::HttpResponsePtr& resp)
    {
        const auto& def = ZmHttpClient::GetOptions();

        // 组装本次尝试结果
        if (r == drogon::ReqResult::Ok && resp)
        {
            size_t effMax = m_opts->maxBodyBytes > 0 ? m_opts->maxBodyBytes: def.maxBodyBytes;
            if (resp->getBody().size() > effMax)
            {
                PUBLIC_LOG_WARN("ZmHttpClient 响应体超限({} > {}),url={}", resp->getBody().size(),
                                effMax, m_currentUrl);
                m_attemptResult = MakeErrorResult(drogon::ReqResult::BadResponse,
                                                resp->getStatusCode());
            }
            else
            {
                m_attemptResult.err = drogon::ReqResult::Ok;
                m_attemptResult.status = resp->getStatusCode();
                m_attemptResult.resp = resp;
            }
        }
        else
        {
            m_attemptResult = MakeErrorResult(r, 0);
        }

        int status = m_attemptResult.status;
        bool netRetriable = (m_attemptResult.err == drogon::ReqResult::Timeout ||
                             m_attemptResult.err == drogon::ReqResult::NetworkFailure);
        // 5xx 与 429(限流)可重试;429 的 Retry-After 由 RetryDelayMs 优先尊重
        bool statusRetriable = (m_attemptResult.err == drogon::ReqResult::Ok &&
                                ((status >= 500 && status <= 599) || status == 429));
        bool methodRetriable = IsIdempotentMethod(m_curMethod) || m_opts->idempotent;
        bool handled = false;

        // 连接层失败处置:Timeout 慢≠坏,仅下一跳规避;
        // 其余无响应(NetworkFailure/BadServerAddress 等)→ 从池中移除,死连接不再留存
        if (!resp)
        {
            if (r == drogon::ReqResult::Timeout)
                m_lastFailedClient = m_lastClient;
            else
                RemovePoolClient(m_lastTargetKey, m_lastClient);
        }

        // -- 重试(指数退避 + Retry-After 优先;定时在客户端 lane loop,不睡业务线程) --
        if ((netRetriable || statusRetriable) && methodRetriable && m_retriesUsed < m_retryBudget)
        {
            double dMs = RetryDelayMs(resp, m_retriesUsed, def.retryBaseMs, def.retryCapMs,
                                      def.retryJitter);
            ++m_retriesUsed;
            auto self = shared_from_this();
            trantor::EventLoop* loop = m_lastLoop ? m_lastLoop
                                                : (s_lanePool ? s_lanePool->getLoop(0) : nullptr);
            if (!loop)
            {
                FinishErr(drogon::ReqResult::NetworkFailure, 0);
                return;
            }
            loop->runAfter(dMs / 1000.0, [self]() { self->NextAttempt(); });
            handled = true;  // 下一轮重试(同 URL;新 req)
        }
        else if (m_attemptResult.err == drogon::ReqResult::Ok && status >= 300 && status <= 399 &&
                 status != 300 && m_opts->followRedirect && m_redirectsUsed < def.maxRedirects)
        {
            // -- 重定向(300 不跟;301/302 非 GET/HEAD 转 GET;303 转 GET;307/308 保体) --
            string loc = Trim(string(resp->getHeader("Location")));
            string nextUrl = loc.empty() ? "": ResolveRedirect(m_currentUrl, loc);
            if (!nextUrl.empty())
            {
                ZmUrlTarget nt = ParseUrl(nextUrl);
                if (nt.ok && nt.key != m_lastTargetKey)
                    m_stripSensitive = true;  // 跨域剥头(此后保持)
                ++m_redirectsUsed;
                m_everRedirected = true;
                m_currentUrl = nextUrl;
                m_lastUrl = nextUrl;
                if (status == 303 ||
                    ((status == 301 || status == 302) && m_curMethod != drogon::HttpMethod::Get &&
                     m_curMethod != drogon::HttpMethod::Head))
                {
                    m_curMethod = drogon::HttpMethod::Get;
                    m_curBody = ZMJSON();
                    m_curRaw.clear();
                }
                // 调度下一跳(与重试分支同律:经 loop 排队,避免同栈递归;
                // 零延迟--窗口期由"连接层失败换连"兜底,见 m_lastFailedClient 注)
                auto redirectSelf = shared_from_this();
                trantor::EventLoop* redirectLoop = m_lastLoop ? m_lastLoop
                                                            : (s_lanePool ? s_lanePool->getLoop(0)
                                                                          : nullptr);
                if (!redirectLoop)
                {
                    FinishErr(drogon::ReqResult::NetworkFailure, 0);
                    return;
                }
                redirectLoop->queueInLoop(
                    [redirectSelf]() { redirectSelf->NextAttempt(); });
                handled = true;  // 已调度下一跳
            }
        }

        if (!handled)
            Finish(std::move(m_attemptResult));
    }

    // -- 终态(一次性):统计 + 出站日志 + 外部回调 --
    void Finish(ZmHttpResult&& lastResult)
    {
        if (m_finished)
            return;
        m_finished = true;   // 终态门:绝不允许二次回调(防外层抛弃后兜底路径误入)
        auto keep = shared_from_this();  // 本栈生命期兜底(回调链可即时清零引用)

        ZmHttpResult result = std::move(lastResult);
        result.retries = m_retriesUsed;
        result.followedRedirect = m_everRedirected;
        result.finalUrl = m_lastUrl;
        result.elapsedSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_t0).count();

        const auto& def = ZmHttpClient::GetOptions();
        s_statRequests.fetch_add(1);
        if (result.err == drogon::ReqResult::Ok)
        {
            s_statOk.fetch_add(1);
            if (result.status >= 400 && result.status <= 499)
                s_statStatus4xx.fetch_add(1);
        }
        else if (result.err == drogon::ReqResult::BadResponse)
            s_statBadResponse.fetch_add(1);
        else if (result.err == drogon::ReqResult::Timeout)
            s_statTimeout.fetch_add(1);
        else
            s_statNetworkErr.fetch_add(1);
        if (result.resp)
            s_statBytesIn.fetch_add(result.resp->getBody().size());

        if (def.outboundAccessLog)
        {
            string mStr(drogon::to_string_view(m_originalMethod));
            size_t bytes = result.resp ? (size_t)result.resp->getBody().size() : 0;
            string insecureFlag = def.validateCert ? "" : " insecure";
            PUBLIC_LOG_INFO("[ZmHttpClient][out] {} {} → {} {}ms {}B retries={} final={}{}",
                            mStr, m_originalUrl, result.status, (int)(result.elapsedSec * 1000),
                            bytes, result.retries, result.finalUrl, insecureFlag);
        }

        // 最后一次触碰外部(可能就此销毁本机;做完所有成员访问再调用)
        if (m_onDone)
        {
            ZmMachineDoneFn done = std::move(m_onDone);
            m_onDone = nullptr;
            done(std::move(result));
        }
    }

/// 直接以错误分类终结(错误码 + 状态码一并带回执)
    void FinishErr(drogon::ReqResult err, int status)
    {
        Finish(MakeErrorResult(err, status));
    }

    drogon::HttpMethod m_originalMethod;
    string m_originalUrl;
    drogon::HttpMethod m_curMethod;
    string m_currentUrl;
    string m_lastUrl;
    ZMJSON m_curBody;
    bool m_rawProvided = false;
    ZmHeaderList m_headerSnapshot;
    int m_retryBudget = 0;
    int m_totalBudget = 0;
    int m_retriesUsed = 0;
    int m_redirectsUsed = 0;
    int m_attemptUsed = 0;
    bool m_everRedirected = false;
    bool m_stripSensitive = false;
    bool m_finished = false;
    string m_lastTargetKey;
    drogon::HttpClientPtr m_lastClient;   // 与 m_lastLoop 同行;保持连接在退避等待期存活
    drogon::HttpClientPtr m_lastFailedClient;  // 上次连接层失败的 client(下一跳规避/替换)
    trantor::EventLoop* m_lastLoop = nullptr;
    ZmHttpResult m_attemptResult;         // 最后一次尝试结果
    RequestOptionsPtr m_opts;
    PreludeFn m_prelude;
    ZmMachineDoneFn m_onDone;
    std::chrono::steady_clock::time_point m_t0;
};

// 发起状态机(前置:调用方已确认 IsReady;not-ready 由各级包装同步处理).
// prelude 可空(JSON/表单走 nullptr,上传走离核预组装).
void RunSendMachine(drogon::HttpMethod m, std::string url, ZMJSON body, std::string raw,
                    std::string contentType, bool rawProvided,
                    ZmHttpClient::ZmHttpRequestOptionsPtr opts, ZmSendMachine::PreludeFn prelude,
                    ZmMachineDoneFn done)
{
    auto machine = std::make_shared<ZmSendMachine>(
        m, std::move(url), std::move(body), std::move(raw), std::move(contentType), rawProvided,
        std::move(opts), std::move(prelude), std::move(done));
    machine->Start();
}

// 协程薄桥:帧内成员仅指针对齐(shared_ptr<ZmMachineCtx>/EventLoop*/coroutine_handle);
// 全部值对象(url/body/raw/contentType/opts/prelude/result)移交堆上 ZmMachineCtx--
// 帧内不含任何复杂值对象(设计约束,见状态机节).
struct ZmMachineCtx
{
    drogon::HttpMethod m = drogon::HttpMethod::Get;
    std::string url;
    ZMJSON body;
    std::string raw;
    std::string contentType;
    bool rawProvided = false;
    ZmHttpClient::ZmHttpRequestOptionsPtr opts;
    ZmSendMachine::PreludeFn prelude;
    ZmHttpResult result;
    std::exception_ptr ex;
};

class ZmMachineAwaiter
{
  public:
    ZmMachineAwaiter(drogon::HttpMethod m, std::string url, ZMJSON body, std::string raw,
                     std::string contentType, bool rawProvided,
                     ZmHttpClient::ZmHttpRequestOptionsPtr opts,
                     ZmSendMachine::PreludeFn prelude = {})
       : m_resumeLoop(opts ? opts->resumeLoop: nullptr)
    {
        m_ctx = std::make_shared<ZmMachineCtx>();
        m_ctx->m = m;
        m_ctx->url = std::move(url);
        m_ctx->body = std::move(body);
        m_ctx->raw = std::move(raw);
        m_ctx->contentType = std::move(contentType);
        m_ctx->rawProvided = rawProvided;
        m_ctx->opts = std::move(opts);
        m_ctx->prelude = std::move(prelude);
    }

    // 未 Init:不挂起,不发状态机,同步取错误结果(避免"await_suspend 栈上重入 resume")
    bool await_ready() noexcept
    {
        if (!ZmHttpClient::IsReady())
        {
            m_ctx->result = MakeErrorResult(drogon::ReqResult::BadServerAddress, 0);
            return true;
        }
        return false;
    }

/// 移交值对象并启动状态机;完成回调恰一次唤醒本协程
    void await_suspend(std::coroutine_handle<> h)
    {
        m_resumeH = h;
        // 移交:值对象随 ctx 离开帧(此后帧内仅 3 个指针成员;resume 前不触碰 ctx 之外内容)
        auto ctx = m_ctx;
        RunSendMachine(ctx->m, std::move(ctx->url), std::move(ctx->body), std::move(ctx->raw),
                       std::move(ctx->contentType), ctx->rawProvided, std::move(ctx->opts),
                       std::move(ctx->prelude),
                       [this, ctx](ZmHttpResult&& r) { Deliver(ctx, std::move(r)); });
    }

/// @return 状态机结果(异常按原样重抛)
    ZmHttpResult await_resume()
    {
        if (m_ctx->ex)
            std::rethrow_exception(m_ctx->ex);
        return std::move(m_ctx->result);
    }

  private:
    /// 恰一次唤醒:resumeLoop 指定时回环,否则就地 resume(之后不得触碰本桥成员)
    void Deliver(const std::shared_ptr<ZmMachineCtx>& ctx, ZmHttpResult&& r)
    {
        ctx->result = std::move(r);
        // resume 线程语义:默认 = lane loop 回调线程;resumeLoop 指定时回环.
        // 注意:resume 是 Deliver 的最后一条语句,不得在 resume 后触碰本桥成员.
        if (m_resumeLoop && !m_resumeLoop->isInLoopThread())
            m_resumeLoop->queueInLoop([h = m_resumeH]() { h.resume(); });
        else
            m_resumeH.resume();
    }

    std::shared_ptr<ZmMachineCtx> m_ctx;   // 值对象唯一次元(堆)
    trantor::EventLoop* m_resumeLoop = nullptr;
    std::coroutine_handle<> m_resumeH{};
};
// ----------------------------------------------------------------------------
// 连接池实现(loop 按目标哈希绑定;创建时 TLS 固化)
// ----------------------------------------------------------------------------

drogon::HttpClientPtr CreatePoolClient(ZmPoolEntry& e)
{
    const auto& o = ZmHttpClient::GetOptions();
    string uri = (e.ssl ? "https://" : "http://") + BracketHost(e.host) + ":" + to_string(e.port);
    auto client = drogon::HttpClient::newHttpClient(uri, e.loop, false /*useOldTLS*/,
                                                    o.validateCert);
    client->setUserAgent(o.userAgent);
    if (!o.clientCert.empty() && !o.clientKey.empty())
        client->setCertPath(o.clientCert, o.clientKey);
    // trustCA:本捆绑 drogon client 的 addSSLConfigs 注入 CAfile 未生效
    // (validateCert 校验会拒绝自签;"VerifyMode" 变体直接触发 trantor FATAL)--
    // 普通 lane 不做注入,仅告警;内网自签请用全局 validateCert=false 应急
    // (出站访问日志带 insecure 标记)或下载通道 TLSPolicy::setCaPath
    if (!o.trustCA.empty())
        PUBLIC_LOG_WARN(
            "ZmHttpClient::Options.trustCA 在本捆绑 drogon client 未生效;"
            "内网自签请用全局 validateCert=false 应急或下载通道 TLSPolicy.setCaPath");
    return client;
}

/// @return 该目标哈希绑定的 lane loop(池未建 → nullptr)
trantor::EventLoop* ChooseLaneLoop(const string& key)
{
    size_t n = s_lanePool ? s_lanePool->size() : 0;
    if (n <= 1)
        return s_lanePool ? s_lanePool->getLoop(0) : nullptr;
    return s_lanePool->getLoop(std::hash<string>()(key) % n);
}

/// 取/建池 + 择路(least-busy;全忙时惰性补建至 maxConnPerHost);返回持有引用(防逐出悬空)
/// 双纪律(缺一即崩):
///   ① `requestsBufferSize()` 在非 loop 线程是 queueInLoop + future.get() **整线程阻塞
///      等彼 loop**;若持 s_poolMtx 调用,彼 loop 线程恰等本锁 → 两条 lane 互等死锁.
///      → 一律**锁外**评估.
///   ② 评估与补建不得并发触碰同一容器:锁内**拷贝 clients 快照**(shared_ptr 列表拷贝,
///      不触碰 client 内部),锁外对**快照**只读求值;补建(write entry->clients)回锁,
///      返回也取自快照--杜绝 vector 并发读写.
drogon::HttpClientPtr GetPoolClient(const ZmUrlTarget& t, const drogon::HttpClientPtr& avoid)
{
    if (!ZmHttpClient::IsReady())
        return nullptr;  // Close 竞态:拒绝在关闭后新建池
    std::shared_ptr<ZmPoolEntry> entry;
    std::vector<drogon::HttpClientPtr> snap;
    {
        std::lock_guard lock(s_poolMtx);
        auto it = s_pools.find(t.key);
        if (it == s_pools.end())
        {
            auto e = std::make_shared<ZmPoolEntry>();
            e->key = t.key;
            e->host = t.host;
            e->port = t.port;
            e->ssl = t.ssl;
            e->createSeq = ++s_poolCreateSeq;
            e->loop = ChooseLaneLoop(t.key);
            e->clients.push_back(CreatePoolClient(*e));
            s_pools.emplace(t.key, e);
            entry = e;

            // 容量守护(rare:targets 超限):逐出"创建序最旧"者(近 LRU).纯结构操作,
            // 不复查 idle(持锁查 requestsBufferSize 会跨 lane 死锁,见上注释);
            // 逐出安全性由 shared_ptr 全员持有保证:drogon 内部以 shared_from_this
            // 维持回调链,状态机亦持 lastClient,在飞请求不会被断链(drogn 1.9.13).
            size_t maxT = s_opts.maxTargets > 0 ? s_opts.maxTargets : 256;
            while (s_pools.size() > maxT)
            {
                uint64_t bestSeq = UINT64_MAX;
                string bestKey;
                for (const auto& kv : s_pools)
                {
                    if (kv.first == t.key)
                        continue;  // 不驱逐本次目标
                    if (kv.second->createSeq < bestSeq)
                    {
                        bestSeq = kv.second->createSeq;
                        bestKey = kv.first;
                    }
                }
                if (bestKey.empty())
                    break;
                s_pools.erase(bestKey);
            }
        }
        else
        {
            entry = it->second;
        }
        snap = entry->clients;  // 快照(shared_ptr 列表,锁内短拷贝;后序评估锁外只读快照)
    }

    // 候选集剔除 avoid(连接层失败过的将死连接;策略见状态机 lastFailedClient 注)
    std::vector<drogon::HttpClientPtr> cands;
    cands.reserve(snap.size());
    for (const auto& c : snap)
    {
        if (avoid && c == avoid)
            continue;
        cands.push_back(c);
    }
    if (cands.empty())
    {
        // 唯一候选即 avoid(单连接目标):移除将死连接,新建一条替换(避免死循环撞同一连接)
        std::lock_guard lock(s_poolMtx);
        auto& v = entry->clients;
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (v[i] == avoid)
            {
                v.erase(v.begin() + i);
                break;
            }
        }
        auto nc = CreatePoolClient(*entry);
        v.push_back(nc);
        return nc;
    }

    // -- 锁外评估忙闲(只读快照;跨 loop 时会整线程等彼 loop,但彼 loop 无需本锁 → 不构成死锁) --
    size_t bestIdx = 0;
    size_t bestBuf = SIZE_MAX;
    bool allBusy = true;
    for (size_t i = 0; i < cands.size(); ++i)
    {
        size_t b = cands[i]->requestsBufferSize();
        if (b < bestBuf)
        {
            bestBuf = b;
            bestIdx = i;
        }
        if (b == 0)
            allBusy = false;
    }

    // 懒补建:全忙且容量未满 → 补 1(评估在锁外;补建写结构须回锁.锁内不再读 busy,
    // 并发下至多互见一次,结果仍 ≤ maxConnPerHost,良性竞态)
    if (allBusy)
    {
        std::lock_guard lock(s_poolMtx);
        if (entry->clients.size() < (size_t)s_opts.maxConnPerHost)
            entry->clients.push_back(CreatePoolClient(*entry));
    }
    return cands[bestIdx];
}

/// 连接层失败(无响应)的 client 从池中移除:死连接 busy=0 反被
/// least-busy 优先选中,留存会令后续请求反复首撞失败;池空则新建替换.
/// 纯结构操作(锁内),CreatePoolClient 非阻塞;在飞引用由 shared_ptr 保持安全.
void RemovePoolClient(const string& key, const drogon::HttpClientPtr& bad)
{
    if (!bad)
        return;
    std::lock_guard lock(s_poolMtx);
    auto it = s_pools.find(key);
    if (it == s_pools.end())
        return;
    auto& v = it->second->clients;
    v.erase(std::remove(v.begin(), v.end(), bad), v.end());
    if (v.empty())
        v.push_back(CreatePoolClient(*it->second));
}

// ----------------------------------------------------------------------------
// 表单编码(application/x-www-form-urlencoded,UTF-8 百分号编码,RFC3986)
// ----------------------------------------------------------------------------
string UrlEncode(const string& s)
{
    static const char* hex = "0123456789ABCDEF";
    string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += (char)c;
        }
        else if (c == ' ')
        {
            out += '+';
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

/// 组装 x-www-form-urlencoded body:key=val&...(键值均 URL 编码;空 body 返回空串)
string BuildFormBody(const std::map<string, string>& fields)
{
    string out;
    for (const auto& kv : fields)
    {
        if (!out.empty())
            out += "&";
        out += UrlEncode(kv.first);
        out += "=";
        out += UrlEncode(kv.second);
    }
    return out;
}

// ----------------------------------------------------------------------------
// multipart 手拼(捆绑版无客户端 Multipart API)
// ----------------------------------------------------------------------------
/// 生成 boundary:与最终 body 组成内容(fileData/field/fileName)做无碰撞校验(最多 8 次)
string MakeMultipartBoundary(const string& fileData, const string& field, const string& fileName)
{
    static thread_local std::mt19937 rng(std::random_device{}());  // 各线程独立,免数据竞争
    for (int i = 0; i < 8; ++i)
    {
        char b[32];
        snprintf(b, sizeof(b), "ZiMoFormBoundary%08x%08x", (unsigned)rng(), (unsigned)rng());
        if (fileData.find(b) == string::npos && field.find(b) == string::npos &&
            fileName.find(b) == string::npos)
            return b;
    }
    return "ZiMoFormBoundaryFailback";  // 理论不可达;兜底仍可写
}

/**
 * @brief quoted-string 转义:剔除控制字符,转义 " 与 \
 *
 * 表单字段名与文件名共用(与服务端 MakeContentDisposition 同源姿势);
 * 不转义则字段名里的 CRLF 或引号能伪造出额外的 part 头.
 *
 * @param s 原值(UTF-8,非 ASCII 原样保留)
 * @return 可安全放进 name="..."/filename="..." 的字符串
 */
string EscapeMultipartQuoted(const string& s)
{
    string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        if (c < 0x20 || c == 0x7F)
            continue;
        if (c == '"' || c == '\\')
            out += '\\';
        out += (char)c;
    }
    return out;
}

/// 读盘 + 尺寸护栏(在客户端自持工作池执行;文件 ≤ maxBytes 才允许)
/// 路径 UTF-8 契约:窄串路径经 CP_UTF8 转宽后打开(平台铁律,禁窄串直接进 fstream)
bool LoadUploadFile(const string& filePath, size_t maxBytes, string& outData, string& errMsg)
{
    int wn = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0);
    if (wn <= 0)
    {
        errMsg = "path 编码非法: " + filePath;
        return false;
    }
    std::wstring wpath((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, &wpath[0], wn);
    if (!wpath.empty() && wpath.back() == L'\0')
        wpath.pop_back();
    std::ifstream f(wpath.c_str(), std::ios::binary);  // MSVC 扩展:ifstream 接受宽路径
    if (!f)
    {
        errMsg = "open file failed: " + filePath;
        return false;
    }
    f.seekg(0, std::ios::end);
    size_t size = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    if (size > maxBytes)
    {
        errMsg = "file too large: " + filePath + " (" + std::to_string(size) + " > " +
                 std::to_string(maxBytes) + ")";
        return false;
    }
    outData.resize(size);
    if (size > 0)
        f.read(&outData[0], (std::streamsize)size);
    if (size > 0 && !f)
    {
        errMsg = "read file failed: " + filePath;
        return false;
    }
    return true;
}

/**
 * @brief 组装单文件 multipart body(内存内拼装;按文件扩展名推断 MIME)
 *
 * @param fileData 文件内容
 * @param fileName 已转义的文件名(EscapeMultipartQuoted)
 * @param field    已转义的表单字段名(EscapeMultipartQuoted)
 * @param boundary 分隔串(须已校验与 body 无碰撞)
 * @return multipart body 字节
 */
string AssembleMultipart(const string& fileData, const string& fileName, const string& field,
                         const string& boundary)
{
    string mime = "application/octet-stream";
    {
        size_t dot = fileName.find_last_of('.');
        if (dot != string::npos)
        {
            static const std::map<string, string> exts = {
                {"txt", "text/plain"},   {"json", "application/json"},
                {"html", "text/html"},   {"xml", "application/xml"},
                {"png", "image/png"},    {"jpg", "image/jpeg"},
                {"jpeg", "image/jpeg"},  {"gif", "image/gif"},
                {"mp3", "audio/mpeg"},   {"mp4", "video/mp4"},
                {"zip", "application/zip"},
            };
            string ext = ToLowerCopy(fileName.substr(dot + 1));
            auto mit = exts.find(ext);
            if (mit != exts.end())
                mime = mit->second;
        }
    }

    string out;
    out += "--" + boundary + "\r\n";
    out += "Content-Disposition: form-data; name=\"" + field + "\"; filename=\"" + fileName +
           "\"\r\n";
    out += "Content-Type: " + mime + "\r\n\r\n";
    out += fileData;
    out += "\r\n--" + boundary + "--\r\n";
    return out;
}

// ----------------------------------------------------------------------------
// 请求组装(JSON dump 直写 / 表单 / multipart;头注入;跨域剥敏感头)
// ----------------------------------------------------------------------------
/// 头容器遍历一律用出站前的短命快照(防止在回调栈上遍历长命引用容器);
/// ZmHeaderList/PrepareRequest 声明见状态机前向声明节
void PrepareRequest(drogon::HttpRequestPtr& req, drogon::HttpMethod m, const ZMJSON& jsonBody,
                    const string* rawBody, const string& rawContentType,
                    const ZmHeaderList& headers, const ZmUrlTarget& tgt, bool stripSensitive)
{
    req->setMethod(m);
    req->setPathEncode(false);  // drogon urlEncode 不保留 %,默认开启会双重编码
    req->setPath(tgt.path);

    for (const auto& kv : headers)
    {
        if (stripSensitive && IsSensitiveHeader(kv.first))
            continue;
        req->addHeader(kv.first, kv.second);
    }

    if (m == drogon::HttpMethod::Get || m == drogon::HttpMethod::Head)
        return;  // 无 body

    if (rawBody)
    {
        req->setBody(*rawBody);
        req->setContentTypeString(rawContentType);
    }
    else if (!jsonBody.is_null())
    {
        req->setBody(jsonBody.dump());  // ZMJSON(ordered_json).dump():键序=构造序
        req->setContentTypeCode(CT_APPLICATION_JSON);
    }
}
}  // namespace

// ----------------------------------------------------------------------------
// 生命周期
// ----------------------------------------------------------------------------

/// 一次性初始化:注入全局参数并创建双 lane(普通请求池 + 下载通道)
/// @param opts 全局运行参数(校验 lane 数 1..8,工作池非 0)
/// @return true 初始化成功;false 重复 Init 或参数非法
bool ZmHttpClient::Init(const Options& opts)
{
    {
        std::lock_guard lock(s_stateMtx);
        if (s_state != ZmClientState::Uninit)
        {
            PUBLIC_LOG_WARN("ZmHttpClient::Init 已初始化过(重复 Init 被拒绝)");
            return false;
        }
        // 护栏:与服务器 SetWorkPoolSize 同纪律,0 = 任务永不执行
        if (opts.normalLoopThreads == 0 || opts.normalLoopThreads > 8 ||
            opts.workPoolSize == 0 || opts.maxConnPerHost == 0)
        {
            PUBLIC_LOG_ERROR("ZmHttpClient::Init 参数非法: normLoopThreads={}, workPoolSize={}, "
                             "maxConnPerHost={}",
                             opts.normalLoopThreads, opts.workPoolSize, opts.maxConnPerHost);
            return false;
        }
        s_opts = opts;
        s_state = ZmClientState::Initialized;
    }

    // 普通 lane:自建事件循环池(不依赖 app())并登记(供 SendSync 拒绝/Close 保护).
    // EventLoopThreadPool::start() 非阻塞,getLoop() 在池线程建好 loop 前返回 nullptr,
    // 而 newHttpClient(uri, nullptr) 会回退 app() 事件循环并跨线程二次构造 EventLoop
    // (trantor FATAL)--故 start() 后轮询等待全部 loop 就位再暴露使用.
    s_lanePool = new trantor::EventLoopThreadPool(opts.normalLoopThreads, "ZmHttpClient-Lane");
    s_lanePool->start();
    for (size_t i = 0; i < s_lanePool->size(); ++i)
    {
        trantor::EventLoop* lp = nullptr;
        for (int spin = 0; spin < 500 && !(lp = s_lanePool->getLoop(i)); ++spin)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!lp)
        {
            PUBLIC_LOG_ERROR("ZmHttpClient::Init 事件循环池启动超时");
            // 失败回滚:拒绝"假 Initialized"砖化,允许重新 Init.
            // 已登记的 loop 随池销毁即成悬空指针,必须先出表(否则 IsLoopThread 解引用悬空)
            for (auto* registered : s_lanePool->getLoops())
                UnregisterLoop(registered);
            delete s_lanePool;
            s_lanePool = nullptr;
            std::lock_guard lock(s_stateMtx);
            s_state = ZmClientState::Uninit;
            return false;
        }
        RegisterLoop(lp);
    }

    // 客户端自持阻塞工作池(multipart 组装/上传读盘/下载受理的磁盘段;不依赖服务器 RunOnPool)
    s_workPool = new ZmThreadPool(static_cast<uint16_t>(opts.workPoolSize), "ZmHttpClient-Worker");

    // HttpClient-DL 下载通道(启失败仅告警--普通 lane 不受影响)
    if (opts.enableDownload)
    {
        if (!ZmHttpDownloadChannel::Start())
            PUBLIC_LOG_ERROR("ZmHttpClient 下载通道启动失败(enableDownload=true)");
    }
    PUBLIC_LOG_INFO(
        "ZmHttpClient::Init ok: normalLane={}, workPool={}, maxConn={}/host, maxTargets={}, "
        "retryMax={}, budget={}, validateCert={}, download={}",
        opts.normalLoopThreads, opts.workPoolSize, opts.maxConnPerHost, opts.maxTargets,
        opts.retryMax, opts.maxTotalAttempts, opts.validateCert ? 1 : 0,
        opts.enableDownload ? 1 : 0);
    return true;
}

bool ZmHttpClient::IsReady()
{
    std::lock_guard lock(s_stateMtx);
    return s_state == ZmClientState::Initialized;
}

/// 全局唯一关闭(三步序:下载通道 → 普通 lane → 置 Closed);幂等;终态.
/// 严禁在任一已登记 loop 线程内调用(自锁);业务须在调用前停发新请求
void ZmHttpClient::Close()
{
    {
        std::lock_guard lock(s_stateMtx);
        if (s_state != ZmClientState::Initialized)
            return;  // Uninit/Closed:幂等
        s_state = ZmClientState::Closed;
    }

    // 三步序(不可颠倒):
    //   ① 下载通道:停止(在飞会话随 loop 退出终结;.part/.meta 保留可续传)→ quit + join
    //   ② 普通 lane:清池 → 登记表注销 → quit → wait
    ZmHttpDownloadChannel::Shutdown();
    {
        std::lock_guard lock(s_poolMtx);
        s_pools.clear();  // 在飞请求按取消语义终止(回调随 loop 退出丢弃,进程退出兜底)
    }
    if (s_workPool.load())
    {
        // s_workPool 有意不 delete:析构会丢弃未执行任务并使在飞
        // Submit 构成 UAF;进程级终态对象随进程回收,指针置空令后续 Submit 走拒绝路径
        s_workPool = nullptr;
    }
    if (s_lanePool)
    {
        for (auto* lp : s_lanePool->getLoops())
            UnregisterLoop(lp);
        for (auto* lp : s_lanePool->getLoops())
            lp->quit();
        s_lanePool->wait();
        delete s_lanePool;
        s_lanePool = nullptr;
    }
    PUBLIC_LOG_INFO("ZmHttpClient::Close ok(终态,closed)");
}

// ----------------------------------------------------------------------------
// 协程编排(统一核心:池 → 单尝试 → 重试/重定向 → 预算)
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// 编排 = 薄协程壳 + 堆上状态机(见匿名字空间 ZmSendMachine/ZmMachineAwaiter)
//   所有包装形态统一 co_await 薄桥;重试/重定向/多尝试循环在状态机内以回调链推进,协程帧不持有任何复杂值对象(设计约束,见状态机节).
// ----------------------------------------------------------------------------

/// 编排核心:薄壳--把入参移交状态机;帧内仅 POD 与 shared_ptr.
/// opts 以 shared_ptr 飞行(帧内仅指针计数拷贝;见头文件注),调用方不得传空
drogon::Task<ZmHttpResult> ZmHttpClient::SendPayload(drogon::HttpMethod m,
                                                     const std::string& url, ZMJSON jsonBody,
                                                     const string* rawBody,
                                                     const string& rawContentType,
                                                     ZmHttpRequestOptionsPtr opts)
{
    assert(opts);  // 状态机全程按非空解引用;三种公开形态均以 make_shared 构造,空指针属调用错误
    co_return co_await ZmMachineAwaiter(
        m, url, std::move(jsonBody), rawBody ? *rawBody : string(), rawContentType,
        rawBody != nullptr, std::move(opts));
}

/// 任意方法请求(JSON body 可空);协程形态
drogon::Task<ZmHttpResult> ZmHttpClient::SendCoro(drogon::HttpMethod m, const std::string& url,
                                                  const ZMJSON& body,
                                                  const ZmHttpRequestOptions& opts)
{
    co_return co_await ZmMachineAwaiter(m, url, body, string(), string(), false,
                                        std::make_shared<const ZmHttpRequestOptions>(opts));
}

/// GET(无 body)
drogon::Task<ZmHttpResult> ZmHttpClient::GetCoro(const std::string& url,
                                                 const ZmHttpRequestOptions& opts)
{
    co_return co_await ZmMachineAwaiter(drogon::Get, url, ZMJSON(), string(), string(), false,
                                        std::make_shared<const ZmHttpRequestOptions>(opts));
}

/// POST JSON body
drogon::Task<ZmHttpResult> ZmHttpClient::PostJsonCoro(const std::string& url,
                                                      const ZMJSON& body,
                                                      const ZmHttpRequestOptions& opts)
{
    co_return co_await ZmMachineAwaiter(drogon::Post, url, body, string(), string(), false,
                                        std::make_shared<const ZmHttpRequestOptions>(opts));
}

/// POST 表单(字段做 UTF-8 百分号编码)
drogon::Task<ZmHttpResult> ZmHttpClient::PostFormCoro(const std::string& url,
                                                      const std::map<string, string>& fields,
                                                      const ZmHttpRequestOptions& opts)
{
    // 表单:application/x-www-form-urlencoded(UTF-8 百分号编码);
    // body 移交 ZmMachineAwaiter→ZmMachineCtx(堆),帧内不滞留值对象
    string body = BuildFormBody(fields);
    co_return co_await ZmMachineAwaiter(drogon::Post, url, ZMJSON(), std::move(body),
                                        "application/x-www-form-urlencoded", true,
                                        std::make_shared<const ZmHttpRequestOptions>(opts));
}

/// multipart 上传的离核预组装(读盘 → 名称转义 → boundary 无碰撞 → 拼 body)
struct ZmUploadPrelude
{
    std::string m_filePath;  ///< 待上传文件路径(UTF-8)
    std::string m_field;     ///< 表单字段名(未转义)
    size_t m_maxBytes = 0;   ///< 单文件大小上限(字节)

    /// @param mach   状态机(成功时回填 m_curRaw / m_curContentType)
    /// @param errMsg 失败原因回填
    /// @return true 组装完成;false 读盘失败或文件超限
    bool operator()(ZmSendMachine& mach, std::string& errMsg) const;
};

bool ZmUploadPrelude::operator()(ZmSendMachine& mach, std::string& errMsg) const
{
    string fileData;
    if (!LoadUploadFile(m_filePath, m_maxBytes, fileData, errMsg))
        return false;
    string fileName = m_filePath.substr(m_filePath.find_last_of("/\\") + 1);
    // 先转义再查碰撞:boundary 校验必须针对最终上线的字节
    string fieldEsc = EscapeMultipartQuoted(m_field);
    string fileEsc = EscapeMultipartQuoted(fileName);
    string boundary = MakeMultipartBoundary(fileData, fieldEsc, fileEsc);
    mach.m_curRaw = AssembleMultipart(fileData, fileEsc, fieldEsc, boundary);
    mach.m_curContentType = "multipart/form-data; boundary=" + boundary;
    return true;
}

drogon::Task<ZmHttpResult> ZmHttpClient::UploadCoro(const std::string& url,
                                                    const std::string& filePath,
                                                    const std::string& field,
                                                    const ZmHttpRequestOptions& opts)
{
    // multipart:读盘 + boundary 无碰撞 + 组装 = 离核预组装(在客户端自持工作池执行)
    const size_t effMax = opts.maxBodyBytes > 0 ? opts.maxBodyBytes: GetOptions().maxBodyBytes;
    co_return co_await ZmMachineAwaiter(drogon::Post, url, ZMJSON(), string(), string(), true,
                                        std::make_shared<const ZmHttpRequestOptions>(opts),
                                        ZmUploadPrelude{filePath, field, effMax});
}

// ----------------------------------------------------------------------------
// 回调 / 同步(与协程形态共用同一状态机)
// ----------------------------------------------------------------------------

/// 异步回调形态(完成回调线程 = lane loop)
void ZmHttpClient::SendAsync(drogon::HttpMethod m, const std::string& url, const ZMJSON& body,
                             std::function<void(ZmHttpResult)> cb,
                             const ZmHttpRequestOptions& opts)
{
    if (!cb)
        return;
    if (!IsReady())
    {
        cb(MakeErrorResult(drogon::ReqResult::BadServerAddress, 0));
        return;
    }
    // 直接发状态机,完成回调线程 = lane loop
    RunSendMachine(m, url, body, string(), string(), false,
                   std::make_shared<const ZmHttpRequestOptions>(opts), {},
                   [cb = std::move(cb)](ZmHttpResult&& r) { cb(std::move(r)); });
}

/// 同步形态:仅限业务线程(已登记 loop 线程一律拒绝)
/// @return 结果;兜底超时按 Timeout 返回(状态机未回调时不让业务线程久挂)
ZmHttpResult ZmHttpClient::SendSync(drogon::HttpMethod m, const std::string& url,
                                    const ZMJSON& body, const ZmHttpRequestOptions& opts)
{
    if (IsLoopThread())
    {
        // 所有已登记 loop(客户端 lane + 服务器各 loop)一律拒绝,
        // 防"服务器 loop 线程被同步调用卡死"(drogon 自身断言只保护客户端自身 loop)
        PUBLIC_LOG_ERROR("ZmHttpClient::SendSync 禁止在已登记 loop 线程调用,url={}", url);
        return MakeErrorResult(drogon::ReqResult::NetworkFailure, 0);
    }
    if (!IsReady())
        return MakeErrorResult(drogon::ReqResult::BadServerAddress, 0);

    // 直接发状态机,promise/future 桥接(完成回调线程 = lane loop;业务线程阻塞取)
    // 注:promise 不可拷贝,经 shared_ptr 包装以适配 std::function 的拷贝要求
    auto promPtr = std::make_shared<std::promise<ZmHttpResult>>();
    auto fut = promPtr->get_future();
    RunSendMachine(m, url, body, string(), string(), false,
                   std::make_shared<const ZmHttpRequestOptions>(opts), {},
                   [promPtr](ZmHttpResult&& r) { promPtr->set_value(std::move(r)); });

    // 兜底超时:Close()/异常下状态机可能永不回调(在飞被丢弃语义)--业务线程绝不能因此永挂.
    // 上限必须覆盖真实最坏耗时(尝试数 × 单次超时 + 退避),否则同步形态会比协程/异步更早
    // 报超时,同一请求三种形态结论不一致.
    const auto& def = ZmHttpClient::GetOptions();
    int retryBudget = (opts.retryCount >= 0) ? opts.retryCount : def.retryMax;
    int attempts = 1 + (retryBudget > 0 ? retryBudget : 0);
    int attemptCap = def.maxTotalAttempts > 0 ? def.maxTotalAttempts : 8;
    if (attempts > attemptCap)
        attempts = attemptCap;  // 重试与重定向共用同一尝试预算
    double t = ResolveTimeout(opts);
    double hardLimit =
        (t > 0 ? t : 60.0) * attempts + (def.retryCapMs / 1000.0) * attempts + 10.0;
    if (fut.wait_for(std::chrono::duration<double>(hardLimit)) != std::future_status::ready)
    {
        PUBLIC_LOG_ERROR("ZmHttpClient::SendSync 等待超时({}s),url={} —— 状态机未回调(坠飞/Close)",
                         hardLimit, url);
        return MakeErrorResult(drogon::ReqResult::Timeout, 0);
    }
    return fut.get();
}

// ----------------------------------------------------------------------------
// 流式下载:实现位于 zm_net_http_client_download.cpp
// (DownloadCoro = 薄桥 + ZmHttpDownloadChannel;与普通 lane 同"帧内无复杂值对象"纪律)
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// 诊断/统计
// ----------------------------------------------------------------------------

/// @return 统计 JSON(请求数/错误分类/字节/4xx)
string ZmHttpClient::DumpStats()
{
    return string("{") + "\"requests\":" + to_string(s_statRequests.load()) +
           ",\"ok\":" + to_string(s_statOk.load()) +
           ",\"status4xx\":" + to_string(s_statStatus4xx.load()) +
           ",\"badResponse\":" + to_string(s_statBadResponse.load()) +
           ",\"timeout\":" + to_string(s_statTimeout.load()) +
           ",\"networkErr\":" + to_string(s_statNetworkErr.load()) +
           ",\"bytesIn\":" + to_string(s_statBytesIn.load()) + "}";
}

void ZmHttpClient::ResetStats()
{
    s_statRequests.store(0);
    s_statOk.store(0);
    s_statStatus4xx.store(0);
    s_statBadResponse.store(0);
    s_statTimeout.store(0);
    s_statNetworkErr.store(0);
    s_statBytesIn.store(0);
}

// ----------------------------------------------------------------------------
// loop 登记表
// ----------------------------------------------------------------------------

void ZmHttpClient::RegisterLoop(trantor::EventLoop* loop)
{
    if (!loop)
        return;
    std::lock_guard lock(s_loopMtx);
    s_loops.insert(loop);
}

void ZmHttpClient::UnregisterLoop(trantor::EventLoop* loop)
{
    std::lock_guard lock(s_loopMtx);
    s_loops.erase(loop);
}

bool ZmHttpClient::IsLoopThread()
{
    std::lock_guard lock(s_loopMtx);
    for (auto* lp : s_loops)
    {
        if (lp && lp->isInLoopThread())
            return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// 离核任务(客户端自持工作池)
// ----------------------------------------------------------------------------

/**
 * @brief 提交可能阻塞的任务到客户端工作池
 *
 * 磁盘 IO 等阻塞任务不得占用事件循环线程(服务器 loop 被拖住会连带停服).
 * 工作池对象是进程级终态(Close 只置空指针,不析构),故已受理的任务必然执行.
 *
 * @param task 任务体(工作池线程执行;须自持所需状态,不得捕获栈上引用)
 * @return true 已受理;false 未就绪或工作池已停(Close 竞态,任务不执行)
 */
bool ZmHttpClient::SubmitBlockingTask(std::function<void()> task)
{
    if (!task || !IsReady())
        return false;
    ZmThreadPool* pool = s_workPool.load();  // Close 竞态:读到旧指针仍可提交,读到空则拒绝
    if (!pool)
        return false;
    pool->Submit(std::move(task));
    return true;
}

// ----------------------------------------------------------------------------
// 访问器
// ----------------------------------------------------------------------------

const ZmHttpClient::Options& ZmHttpClient::GetOptions()
{
    return s_opts;
}

// ----------------------------------------------------------------------------
// ZmHttpResult helper
// ----------------------------------------------------------------------------

ZMJSON ZmHttpResult::Json() const
{
    if (!resp)
        return ZMJSON();
    // 仅 Content-Type 语义为 JSON(application/json 或 application/*+json,含 charset 参数) 时解析
    {
        string ct = ToLowerCopy(string(resp->getHeader("Content-Type")));
        if (ct.find("json") == string::npos)
            return ZMJSON();
    }
    string err;
    ZMJSON v = zm_json_parse(string(resp->getBody()), err);
    if (!err.empty())
    {
        PUBLIC_LOG_WARN("ZmHttpClient 响应 JSON 解析失败: {}", err);
        return ZMJSON();
    }
    return v;
}

string ZmHttpResult::Body() const
{
    return resp ? string(resp->getBody()) : string();
}
