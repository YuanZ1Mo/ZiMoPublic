#ifndef NOMINMAX   // windows.h 的 min/max 宏会破坏 std::min/max;须在一切 include 前定义
#define NOMINMAX
#endif
#include "zm_net_http_server.h"

#include <drogon/DrClassMap.h>
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>
#include <drogon/utils/HttpConstraint.h>
#include <drogon/utils/Utilities.h>   // getHttpDate(条件请求 If-Modified-Since 解析)
#include <drogon/RequestStream.h>

#include <trantor/net/TcpConnection.h>
#include <trantor/net/EventLoop.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <cctype>
#include <deque>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <windows.h>

#include <zm_util_logger.h>
#include <zm_util_thread.h>
#include <zm_util_json.h>

using namespace drogon;
using std::string;
using std::vector;
using std::pair;

// ============================================================================
// 静态生命周期状态机(FR-01/02/04,设计 §2.2) —— 实现见文件后部
//   (Init/Open/Close 需 NowMs、s_workPoolSize、JsonResponse 等已定义,故置于后部)
// ============================================================================

// 匿名命名空间:文件内私有工具(Filter 适配 / 流式发送状态机 / WS 控制器等)
namespace
{
// ----------------------------------------------------------------------------
// std::function 适配为 Drogon HttpFilter(按名注册,构造时注入函数;设计 §4.2)
// ----------------------------------------------------------------------------
class ZmFuncFilter : public drogon::HttpFilterBase
{
public:
    explicit ZmFuncFilter(
        std::function<bool(const HttpRequestPtr&, HttpResponsePtr&)> fn)
        : m_fn(std::move(fn))
    {
    }

    void doFilter(const HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                  drogon::FilterChainCallback&& fccb) override
    {
        HttpResponsePtr resp;
        if (m_fn(req, resp))
        {
            fccb();
        }
        else
        {
            if (!resp)
                resp = ZmHttpServer::ErrorResponse(500, "filter denied");
            fcb(resp);
        }
    }

private:
    std::function<bool(const HttpRequestPtr&, HttpResponsePtr&)> m_fn;
};

// ----------------------------------------------------------------------------
// 方案乙流式发送状态机(定时器链;设计 §6.1 v2.1)
// ----------------------------------------------------------------------------
/// 文件传输专用 I/O 线程池:磁盘读/写绝不占用事件循环线程(NFR)。
/// 独立于全局池(避免与其它任务抢线程),按需自动扩容。
ZmThreadPool& HttpIoPool()
{
    static ZmThreadPool pool(2, "ZmHttpIo");
    return pool;
}

class ZmStreamLoopState : public std::enable_shared_from_this<ZmStreamLoopState>
{
public:
    string path;
    uint64_t offset = 0;      // 发送起点(Range 续传定位;全文件 = 0)
    uint64_t remaining = 0;
    uint64_t total = 0;
    uint64_t sent = 0;
    int64_t lastSentMs = 0;
    int64_t abortMs = 120000;
    ZmHttpSendFileOptions opts;
    drogon::ResponseStreamPtr stream;

    void Run();   // 打开文件并驱动第一块
    void Next();  // 提交异步读 → I/O 线程读盘 → queueInLoop 回执发送(事件循环不阻塞)
    void OnReadDone(std::shared_ptr<std::string> buf, bool ok);
    void Finish();
    void DoClose();

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    bool m_fini = false;
    int m_inFlight = 0;   // 在途异步读计数(仅事件循环线程访问)
    bool m_closed = false;
};

static int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ── Run:打开文件句柄并驱动第一块(事件循环线程;收到流对象时立即调用) ──
void ZmStreamLoopState::Run()
{
    // 路径为内部构造的 ASCII 路径;File 已按 UTF-8(A:\ZiMo\...)
    std::filesystem::path fsPath(path);
    m_handle = CreateFileW(fsPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE)
    {
        PUBLIC_LOG_ERROR("SendFileStreamCoro 打不开文件: {}", path);
        Finish();
        return;
    }
    // Range 续传定位:从 offset 起读(全文件 offset=0);定位失败按错误收尾
    if (offset > 0)
    {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN))
        {
            PUBLIC_LOG_ERROR("SendFileStreamCoro 定位失败(offset={}): {}", offset, path);
            Finish();
            return;
        }
    }
    lastSentMs = NowMs();
    Next();
}

// ── Next:调度一次异步读(块粒度 chunkSize,预读窗口=1 块) ──
//    在途计数 m_inFlight 保护:读回执到达前不再叠加下一块;所有状态只在事件循环线程变更。
void ZmStreamLoopState::Next()
{
    if (m_fini)
        return;

    // 停止判定:连接关闭或已发完
    if (!stream || remaining == 0)
    {
        Finish();
        return;
    }
    // 停滞判定:距上次成功 send 超阈值且连接未关闭(慢客户端兜底,可 Range 续传)
    if (NowMs() - lastSentMs > abortMs)
    {
        PUBLIC_LOG_WARN("SendFileStreamCoro 停滞放弃: {}", path);
        Finish();
        return;
    }

    size_t want = static_cast<size_t>(
        std::min<uint64_t>(remaining, opts.chunkSize ? opts.chunkSize : 1));
    ++m_inFlight;   // 缓冲已就位,回执前不再叠加(预读窗口 = 1 块,内存有界)
    HttpIoPool().Submit(
        [st = shared_from_this(), want]() {
            // 读盘在专用 I/O 线程执行,绝不阻塞事件循环(NFR)
            DWORD rd = 0;
            auto buf = std::make_shared<std::string>();
            buf->resize(want);
            if (::ReadFile(st->m_handle, buf->data(), static_cast<DWORD>(want),
                           &rd, nullptr) &&
                rd > 0)
                buf->resize(static_cast<size_t>(rd));
            drogon::app().getLoop()->queueInLoop(
                [st, buf = std::move(buf), ok = (rd > 0)]() mutable {
                    st->OnReadDone(std::move(buf), ok);
                });
        },
        "ZmHttpRead");
}

// ── OnReadDone:读回执(事件循环线程) → 发送 + 进度 + 决定继续/收尾 ──
void ZmStreamLoopState::OnReadDone(std::shared_ptr<std::string> buf, bool ok)
{
    // 统一在事件循环线程执行(读线程仅回执,不碰 stream)
    --m_inFlight;
    if (m_fini)
    {
        if (m_inFlight == 0)
            DoClose();
        return;
    }
    if (!ok || !stream)
    {
        Finish();
        return;
    }

    if (!stream->send(*buf))
    {
        // trantor 契约:send 返回 false = 连接已关闭
        Finish();
        return;
    }
    remaining -= buf->size();
    sent += buf->size();
    lastSentMs = NowMs();
    if (opts.onProgress && sent <= total)
        opts.onProgress(sent, total);

    if (remaining == 0)
    {
        Finish();
        return;
    }
    // 定时器节流:事件循环处理发送、缓冲自然排水(内存有界:在途预读 = 1 块)
    // interBlockMs == 0 → 发完即调度(runAfter(0) 下一轮立即跑,无节流,对应头文件契约)
    drogon::app().getLoop()->runAfter(opts.interBlockMs / 1000.0,
                                      [st = shared_from_this()]() { st->Next(); });
}

// ── Finish:请求收尾(幂等)——有待在途读时延迟关句柄,由最后一笔回执补关(防竞态) ──
void ZmStreamLoopState::Finish()
{
    if (m_fini)
        return;
    m_fini = true;
    // 有待在途读时缓至关句柄,由最后一笔回执补关,避免与 I/O 线程竞态
    if (m_inFlight == 0)
        DoClose();
}

// ── DoClose:真正释放(句柄 + 流)——在途计数为 0 才执行,与 I/O 线程无竞态 ──
void ZmStreamLoopState::DoClose()
{
    if (m_closed)
        return;
    m_closed = true;
    if (m_handle != INVALID_HANDLE_VALUE)
    {
        // 在途读计数为 0 才关句柄 → 与 I/O 线程的 ReadFile 无竞态
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    if (stream)
    {
        // ResponseStream::close() 发送终止分块并关闭(线程安全)
        stream->close();
        stream.reset();
    }
}

// ----------------------------------------------------------------------------
// WebSocket 注册表(FR-16;设计 §9)
// ----------------------------------------------------------------------------
class ZmWsController : public drogon::WebSocketControllerBase
{
public:
    explicit ZmWsController(string regName) : m_regName(std::move(regName)) {}

    void handleNewConnection(const HttpRequestPtr& req,
                             const WebSocketConnectionPtr& conn) override
    {
        const auto& cb = GetWsCallbacks(m_regName);
        bool ok = cb.onAuth ? cb.onAuth(req) : true;
        if (!ok)
        {
            // 设计 §9:无"接受前鉴权"钩子,只能升级后拒绝 → 连接关闭语义
            conn->shutdown(drogon::CloseCode::kEndpointGone, "auth failed");
            return;
        }
        if (cb.onOpen)
            cb.onOpen(conn, req);
    }

    void handleNewMessage(const WebSocketConnectionPtr& conn, string&& msg,
                          const WebSocketMessageType& type) override
    {
        const auto& cb = GetWsCallbacks(m_regName);
        if (cb.onMessage)
            cb.onMessage(conn, std::move(msg), type);
    }

    void handleConnectionClosed(const WebSocketConnectionPtr& conn) override
    {
        const auto& cb = GetWsCallbacks(m_regName);
        if (cb.onClose)
            cb.onClose(conn);
    }

    static const ZmHttpServer::WsCallbacks& GetWsCallbacks(const string& name)
    {
        std::lock_guard lock(s_mtx);
        auto it = s_cbs.find(name);
        static ZmHttpServer::WsCallbacks empty;
        return it == s_cbs.end() ? empty : it->second;
    }

    static void SetCallbacks(const string& name, ZmHttpServer::WsCallbacks cb)
    {
        std::lock_guard lock(s_mtx);
        s_cbs[name] = std::move(cb);
    }

private:
    string m_regName;
    static std::mutex s_mtx;
    static std::map<string, ZmHttpServer::WsCallbacks> s_cbs;
};

std::mutex ZmWsController::s_mtx;
std::map<string, ZmHttpServer::WsCallbacks> ZmWsController::s_cbs;

string BuildWsRegName()
{
    static std::atomic<uint32_t> n{0};
    return "ZmWsCtrl" + std::to_string(n.fetch_add(1));
}

// ----------------------------------------------------------------------------
// 文件条件请求(Last-Modified/ETag → 304;RFC 7232;文件传输三条路径共用)
// ----------------------------------------------------------------------------
/// RFC1123 HTTP 日期输出("Thu, 03 Sep 2026 10:00:00 GMT")
static string HttpDateStr(int64_t t)
{
    std::time_t et = static_cast<std::time_t>(t);
    std::tm tmv{};
    if (gmtime_s(&tmv, &et) != 0)
        return "";
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tmv) == 0)
        return "";
    return buf;
}

/// 文件元信息(存在性/大小/最后写入秒;单次 stat 提供,条件请求与 Range 共用)
struct ZmFileMeta
{
    bool found = false;       // 文件存在(否则 → 404)
    bool sizeFailed = false;  // 存在但取大小失败(否则 → 500)
    size_t size = 0;
    int64_t mtimeSec = 0;     // last_write_time → epoch 秒
};

static ZmFileMeta FetchFileMeta(const string& path)
{
    ZmFileMeta m;
    std::error_code ec;
    m.found = std::filesystem::exists(path, ec) && !ec;
    if (!m.found)
        return m;
    m.size = static_cast<size_t>(std::filesystem::file_size(path, ec));
    if (ec)
    {
        m.sizeFailed = true;
        return m;
    }
    std::error_code ec2;
    auto ft = std::filesystem::last_write_time(path, ec2);
    if (!ec2)
    {
        // file_time(文件时钟) → system_clock(epoch 秒):以两个时钟的 now 为桥换算
        auto fileNow = std::filesystem::file_time_type::clock::now();
        auto sysNow = std::chrono::system_clock::now();
        auto sysT = std::chrono::time_point_cast<std::chrono::seconds>(
            sysNow - (fileNow - ft));
        m.mtimeSec =
            static_cast<int64_t>(std::chrono::system_clock::to_time_t(sysT));
    }
    return m;
}

/// 缓存头(200/206/304 共用;强 ETag = size-mtime)
static pair<string, string> CacheHeaders(const ZmFileMeta& m)
{
    return {HttpDateStr(m.mtimeSec),
            "\"" + std::to_string(m.size) + "-" + std::to_string(m.mtimeSec) + "\""};
}

/// 条件请求判定:If-None-Match 优先(命中 → 304;未命中跳过 If-Modified-Since),
/// If-Modified-Since 兜底(文件未改 → 304)。日期非法视为未提供。
/// @return 非空 = 304 响应(调用方直接返回);nullptr = 继续正常 200/206 流程。
static HttpResponsePtr Maybe304(const HttpRequestPtr& req, const ZmFileMeta& m,
                                const pair<string, string>& cacheHeaders)
{
    const string& lastMod = cacheHeaders.first;
    const string& etag = cacheHeaders.second;
    string inm = req->getHeader("If-None-Match");
    if (!inm.empty())
    {
        // RFC 7232:If-None-Match 弱比较(可带 W/ 前缀/逗号列表),用子串命中已够
        if (inm == "*" || inm.find(etag) != string::npos)
        {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k304NotModified);
            resp->addHeader("Last-Modified", lastMod);
            resp->addHeader("ETag", etag);
            return resp;
        }
        return nullptr;
    }
    string ims = req->getHeader("If-Modified-Since");
    if (!ims.empty())
    {
        try
        {
            trantor::Date d = drogon::utils::getHttpDate(ims);
            if (d.microSecondsSinceEpoch() / 1000000 >= m.mtimeSec)
            {
                auto resp = HttpResponse::newHttpResponse();
                resp->setStatusCode(k304NotModified);
                resp->addHeader("Last-Modified", lastMod);
                resp->addHeader("ETag", etag);
                return resp;
            }
        }
        catch (...)
        {
            // 日期非法 → 当作未提供
        }
    }
    return nullptr;
}
}  // namespace

// ============================================================================
// 静态成员(工作池大小 / filter 注册表 / 全局 once)
// ============================================================================
std::mutex ZmHttpServer::s_filterMtx;
std::map<string, std::function<bool(const drogon::HttpRequestPtr&,
                                    drogon::HttpResponsePtr&)>>
    ZmHttpServer::s_filters;

static std::atomic<size_t> s_workPoolSize{8};
std::atomic<bool> ZmHttpServer::s_autoJsonp{true};   // 自动 JSONP 默认开(对齐主流)
std::atomic<size_t> ZmHttpServer::s_nonStreamBodyLimit{0};  // Init 注入;0 = 关闭预检
std::vector<string> ZmHttpServer::s_corsOrigins;            // Init 注入(启动后只读)

/// 三面共享的静态阻塞工作池(FR-19):RunOnPool 的唯一目的地。
/// 容量为进程级全局(SetWorkPoolSize 注入),首次使用即定型;
/// 同一进程内所有 HTTP 面共用,不与业务自建线程池混用。
ZmThreadPool& ZmHttpServer::WorkPool()
{
    static ZmThreadPool pool(static_cast<uint16_t>(s_workPoolSize.load()),
                             "DrogonHttp-Worker");
    return pool;
}

/// 业务阻塞工作池线程数(三面共享静态池;DB/磁盘/CPU 型 handler 在此排队执行)。
/// 须在首次 RunOnPool 前调用(静态池首次使用即定型);高并发可调大。
/// 切勿设 0:自研 ZmThreadPool 按该数创建工作线程,0 = 任务永不执行、业务卡死。
void ZmHttpServer::SetWorkPoolSize(size_t n)
{
    s_workPoolSize.store(n);
}

size_t ZmHttpServer::GetWorkPoolSize()
{
    return s_workPoolSize.load();
}

// ============================================================================
// 静态生命周期状态机(FR-01/02/04,设计 §2.2)
//   drogon app() 为全局单例且 run() 只能跑一次 → 生命周期为进程级一次:
//     Uninit → Initialized(Init) → Opened(Open) → Closed(Close, 终态)。
//   相位契约:AddListener/RegisterRoutes/RegisterCoro 全部须在 Open 前完成;
//   Open() 起 run 线程,绑定失败(端口占用等)在 300ms 内探测并 fail-fast。
//   运行期唯一可热更新能力:ReloadCertificates()(reloadSSLFiles)。
// ============================================================================
namespace
{
enum class ZmRuntimeState
{
    Uninit,       // 进程启动默认态;Init 前唯一可做的是 Init
    Initialized,  // Init 完成:全局参数/证书/全局 advice 已应用,可登记监听与路由
    Opened,       // 事件循环运行中;只能 Close(或证书热重载)
    Closed        // 已 quit+join,终态;不能再 Open/Init/AddListener
};

std::atomic<ZmRuntimeState> s_state{ZmRuntimeState::Uninit};
std::atomic<bool> s_hasListener{false};
std::mutex s_stateMtx;
std::condition_variable s_stateCv;
std::unique_ptr<ZmThread> s_runThread;      // app().run() 承载线程(统一 ZmThread 模型)
std::atomic<bool> s_runError{false};
std::string s_runErrorMsg;
std::atomic<bool> s_hasCert{false};

// 启动成功信号:registerBeginningAdvice 在事件循环真正跑起来时置值,
// Open() 用 future 等待,替代"300ms 没报错就认为成功"的盲猜。
// 用 unique_ptr 管理:Open 失败回退后可重建以支持重试。
std::mutex s_startMtx;
std::unique_ptr<std::promise<void>> s_startPromise;
std::atomic<bool> s_startReady{false};
}  // namespace

/// 进程级状态查询:是否已完成 Init(当前可登记监听/路由,启动与否均可)
bool ZmHttpServer::IsInitialized()
{
    return s_state.load() >= ZmRuntimeState::Initialized;
}

/// 进程级状态查询:事件循环是否运行中(运行期只能 Close/证书热重载)
bool ZmHttpServer::IsOpened()
{
    return s_state.load() == ZmRuntimeState::Opened;
}

/// 进程级一次性初始化(FR-01/03/10,设计 §2.2):应用全局运行参数 + 证书 +
/// 全局 advice(/ping、访问日志、自动 JSONP)。只允许 Uninit → Initialized 一次;
/// 之后到 Open 前可自由 AddListener/Setup/RegisterCoro(Phase1)。
/// @return false = 状态非法(重复 Init/已启动)或参数校验失败。
bool ZmHttpServer::Init(const Options& opts)
{
    ZmRuntimeState cur = s_state.load();
    if (cur != ZmRuntimeState::Uninit)
    {
        PUBLIC_LOG_ERROR("ZmHttpServer::Init 只可调用一次(当前状态已非 Uninit)");
        return false;
    }

    // ── 全局运行参数(FR-03;均为 drogon app() 全局量,运行期不可改) ──
    app().setThreadNum(opts.threadNum);            // 0 = 自动 = CPU 核数
    app().setMaxConnectionNum(opts.maxConnections);
    app().setMaxConnectionNumPerIP(opts.maxConnectionsPerIP);  // per-IP 护栏(0=不限;单机压测慎设)
    app().setClientMaxBodySize(opts.clientMaxBodySize);
    app().setClientMaxMemoryBodySize(opts.clientMaxMemoryBodySize);  // 内存闸:超限落临时文件
    s_nonStreamBodyLimit.store(opts.nonStreamBodyLimit, std::memory_order_relaxed);
    s_corsOrigins = opts.corsAllowedOrigins;       // CORS 白名单(启动前注入,运行期只读)
    app().setIdleConnectionTimeout(opts.idleTimeoutSec);
    app().setKeepaliveRequestsNumber(opts.keepaliveRequests);   // 0 = 不限次数回收
    app().enableRequestStream(opts.enableRequestStream);
    app().enableGzip(opts.gzip);
    app().enableBrotli(opts.brotli);
    app().setGzipStatic(opts.gzipStatic);
    app().setBrStatic(opts.brotliStatic);   // 捆绑 1.9.13 已含 setBrStatic 且 lib 已链接 brotli(USE_BROTLI,已实测符号在)
    s_workPoolSize.store(opts.workPoolSize);       // 首次 RunOnPool 前定型

    // ── TLS(FR-10/11;证书全局,保证热加载) ──
    s_hasCert.store(!opts.certFile.empty(), std::memory_order_relaxed);
    if (!opts.certFile.empty())
    {
        app().setSSLFiles(opts.certFile, opts.keyFile);
        if (opts.ticketDisabled)
            app().setSSLConfigCommands({ { "Options", "-SessionTicket" } });
    }

    // ── 启动成功信号(FR-01):事件循环真正跑起来时置值,供 Open() 确定性等待 ──
    // 绑定失败会在事件循环启动前抛异常,此时 BeginningAdvice 不触发 → 由 run 异常兜底。
    app().registerBeginningAdvice([] {
        {
            std::lock_guard lk(s_startMtx);
            if (s_startPromise && !s_startReady.load())
            {
                s_startPromise->set_value();
                s_startReady.store(true);
            }
        }
        s_stateCv.notify_all();   // 唤醒 Open() 的 wait_for(依赖 s_startReady 判定)
    });

    // ── 全局 advice + /ping(FR-17/23/24;经 Options 一次性注册,不再依赖"首个 Open"去重) ──
    // 注:本捆绑 drogon 的 FunctionTraits 协程特化要求 handler 首参为按值 HttpRequestPtr
    app().registerHandler("/ping",
        [](HttpRequestPtr req) -> drogon::Task<HttpResponsePtr> {
            ZMJSON data;
            data["pong"] = true;
            co_return JsonResponse(200, data);
        },
        { HttpMethod::Get });

    // 请求访问记录(FR-17,用户决策:不用 AccessLogger/access.log,
    // 格式化后经公共库日志 PUBLIC_LOG_* 承载,与运行日志同文件按标签区分)。
    // 起始时间存 req attributes,PostHandling 观察点结算。
    app().registerPreRoutingAdvice([](const HttpRequestPtr& req) {
        req->getAttributes()->insert("ZmAccessStartMs", std::any(int64_t(NowMs())));
    });
    app().registerPostHandlingAdvice([](const HttpRequestPtr& req,
                                        const HttpResponsePtr& resp) {
        int64_t cost = NowMs() - req->getAttributes()->get<int64_t>("ZmAccessStartMs");
        string query = req->getQuery();
        // 字节数口径:流式上传的 body 已被消费(净余≈0)、流式/文件响应的 body 为空,
        //   此二者优先取 Content-Length 头(带 "~" 前缀)近似;chunked 流(方案乙)无该头 → 记 0。
        string reqBytes = std::to_string(req->getBody().size());
        if (req->getBody().empty())
        {
            string cl = req->getHeader("Content-Length");
            if (!cl.empty())
                reqBytes = "~" + cl;
        }
        string respBytes = std::to_string(resp->getBody().size());
        if (resp->getBody().empty())
        {
            string cl = resp->getHeader("Content-Length");
            if (!cl.empty())
                respBytes = "~" + cl;
        }
        PUBLIC_LOG_INFO(
            "{} {} {} [{}] ({} - {}) {} {} {}ms",
            req->isOnSecureConnection() ? "https" : "http",
            req->getMethodString(),
            query.empty() ? string(req->path()) : string(req->path()) + "?" + query,
            reqBytes, req->getPeerAddr().toIpPort(), req->getLocalAddr().toIpPort(),
            static_cast<int>(resp->getStatusCode()), respBytes, cost);
    });

    // 非流式请求体闸门(header 阶段预检):1.9.13 无 per-route 上限 API,
    // Content-Length 声明超限 → 提前 413,防恶意大 body 落临时文件/磁盘耗尽。
    // 豁免:带 X-File-Size 者视为流式大上传(RegisterStreamCoro 业务,其 maxBytes 兜底);
    //   ⚠ 该豁免可被伪造,防御定位为"默认配置收口",非完备防护(全局 clientMaxBodySize 兜底)。
    app().registerPreRoutingAdvice([](const HttpRequestPtr& req, AdviceCallback&& cb,
                                      AdviceChainCallback&& cc) {
        size_t limit = s_nonStreamBodyLimit.load();
        if (limit > 0)
        {
            string cl = req->getHeader("Content-Length");
            if (!cl.empty())
            {
                try
                {
                    if (std::stoull(cl) > limit && req->getHeader("X-File-Size").empty())
                    {
                        cb(ErrorResponse(413, "body too large"));
                        return;
                    }
                }
                catch (const std::exception&) { /* 非法长度头,交给框架解析 */ }
            }
        }
        cc();
    });

    // 安全响应头(全局 PreSending;含 80→443 重定向与全部分支):
    //   X-Content-Type-Options / X-Frame-Options / HTTPS 面 HSTS。
    //   CSP 不做默认(需按页面约定配置,见 Options 备注)。
    app().registerPreSendingAdvice([](const HttpRequestPtr& req,
                                      const HttpResponsePtr& resp) {
        resp->addHeader("X-Content-Type-Options", "nosniff");
        resp->addHeader("X-Frame-Options", "SAMEORIGIN");
        if (req->isOnSecureConnection())
        {
            resp->addHeader("Strict-Transport-Security",
                            "max-age=31536000; includeSubDomains");
        }
    });

    // 自动 JSONP(FR-24,主流中间件语义):GET + 合法 callback + JSON 响应 → 包装。
    // 规则收敛:不污染普通 REST/前端静态/WS 升级;三面共享(全局 advice)。
    // 注:在 PostHandling 之后执行,访问日志记录的字节数为包装前的 JSON 大小。
    app().registerPreSendingAdvice([](const HttpRequestPtr& req,
                                      const HttpResponsePtr& resp) {
        if (!ZmHttpServer::IsAutoJsonpEnabled())
            return;
        if (req->method() != Get)
            return;
        string cb = req->getParameter("callback");
        if (cb.empty() || !ZmHttpServer::IsValidJsonpCallback(cb))
            return;
        auto ct = resp->contentType();
        if (ct != CT_APPLICATION_JSON && ct != CT_TEXT_JAVASCRIPT)
            return;
        string body = cb + "(" + std::string(resp->getBody()) + ");";
        resp->setBody(body);
        resp->setContentTypeCode(CT_TEXT_JAVASCRIPT);
    });

    s_state.store(ZmRuntimeState::Initialized);
    PUBLIC_LOG_INFO("ZmHttpServer::Init 完成(全局参数/证书/advice 已应用)");
    return true;
}

/// 启动服务器(FR-01/04,设计 §2.2 Phase2):后台线程跑 app().run()。
/// 前置:已 Init 且已登记至少一个监听;启动成功以"事件循环就绪信号"
/// (registerBeginningAdvice)为准,绑定失败(端口占用等)经 run 线程异常
/// fail-fast 返回 false。
bool ZmHttpServer::Open()
{
    ZmRuntimeState cur = s_state.load();
    if (cur != ZmRuntimeState::Initialized)
    {
        PUBLIC_LOG_ERROR("ZmHttpServer::Open: 必须先 Init(当前状态非 Initialized)");
        return false;
    }
    if (!s_hasListener.load())
    {
        PUBLIC_LOG_ERROR("ZmHttpServer::Open: 未登记任何监听(AddListener 必须先于 Open)");
        return false;
    }

    s_runError.store(false);
    s_state.store(ZmRuntimeState::Opened);

    // 启动信号重置:advice 已注册,每次 Open 重建 promise 供本次等待
    // (s_startReady 原子标志为实际判定依据,advice 置值 + notify s_stateCv)
    {
        std::lock_guard lk(s_startMtx);
        s_startPromise = std::make_unique<std::promise<void>>();
        s_startReady.store(false);
    }

    // 后台线程跑 app().run()(ZmThread 模型;Stop 靠 request_stop + join,
    // 实际退出由 app().quit() 驱动,run() 不查 stop_token)。
    // 绑定失败(端口占用等)会在事件循环启动前抛异常 → s_runError 捕获兜底。
    s_runThread = std::make_unique<ZmThread>(
        "DrogonHttpRun", [](std::stop_token) {
            try
            {
                app().run();
            }
            catch (const std::exception& e)
            {
                {
                    std::lock_guard lk(s_stateMtx);
                    s_runErrorMsg = e.what();
                }
                s_runError.store(true);
                s_stateCv.notify_all();
            }
            catch (...)
            {
                {
                    std::lock_guard lk(s_stateMtx);
                    s_runErrorMsg = "unknown";
                }
                s_runError.store(true);
                s_stateCv.notify_all();
            }
        });
    if (!s_runThread->Start())
    {
        PUBLIC_LOG_ERROR("ZmHttpServer::Open: DrogonHttpRun 线程启动失败");
        s_runThread.reset();
        s_state.store(ZmRuntimeState::Initialized);
        return false;
    }

    // 纯事件等待"启动结果":两个信号必然有一个到达并 notify ——
    //   BeginningAdvice 触发(事件循环已跑)→ 成功;run 抛异常(绑定失败等)→ s_runError。
    // 无需超时:绑定失败必抛异常、启动成功必触发 advice,二者必有其一。
    std::unique_lock lock(s_stateMtx);
    s_stateCv.wait(lock, [&] { return s_runError.load() || s_startReady.load(); });

    if (s_runError.load())
    {
        PUBLIC_LOG_ERROR("app().run() 启动异常: {}", s_runErrorMsg);
        // 让 run 线程收尾退出,再回退状态允许重试
        app().quit();
        s_runThread->Stop();
        s_runThread.reset();
        s_state.store(ZmRuntimeState::Initialized);
        return false;
    }
    PUBLIC_LOG_INFO("ZmHttpServer::Open 完成(app().run 运行中)");
    return true;
}

/// 全局唯一关闭(FR-04,设计 §2.2 Phase3):quit() + join run 线程;幂等。
/// 状态分支:未启动/已关闭 → 直接返回;Initialized 未 Open → 仅回退状态;
/// Opened → 先 app().quit() 停事件循环,再 Stop(join)。
/// ⚠ 在飞 HTTP 语义(v2.7):quit 后挂起协程不再调度,业务层自保障
///    (守护线程 join/断点),本函数不等待在飞业务。
void ZmHttpServer::Close()
{
    ZmRuntimeState cur = s_state.load();
    if (cur == ZmRuntimeState::Uninit || cur == ZmRuntimeState::Closed)
    {
        return;   // 幂等:未启动 / 已关闭
    }
    if (cur == ZmRuntimeState::Initialized)
    {
        // 未 Open 就直接 Close:仅回退状态
        s_state.store(ZmRuntimeState::Closed);
        return;
    }
    // Opened:先 quit 让 run() 返回,再 Stop(join) — 顺序必须保持
    if (s_runThread)
    {
        app().quit();
        s_runThread->Stop();
        s_runThread.reset();
    }
    s_state.store(ZmRuntimeState::Closed);
    PUBLIC_LOG_INFO("ZmHttpServer::Close 完成(已 quit+Stop,终态)");
}

/// 本面是否 HTTPS(一对象一端口:监听 useSSL 即 HTTPS 模式;
/// 证书为进程级全局,各面判定结果一致)
bool ZmHttpServer::IsHttps() const
{
    // 一对象一端口(v2.5):本面监听启用 useSSL 即 HTTPS 模式
    return m_listenerSet && m_listener.useSSL;
}

// ============================================================================
// 结构路由注册(幂等;manager 在 Open 前调用)
// ============================================================================
void ZmHttpServer::Setup()
{
    if (m_setupDone)
        return;
    m_setupDone = true;
    RegisterRoutes();
}

// ============================================================================
// 监听配置(FR-02;证书全局经 Init 的 Options,per-listener 证书为空保证热加载)
// ============================================================================
void ZmHttpServer::AddListener(uint16_t port, bool useSSL, const string& ip,
                                   bool useOldTLS,
                                   const vector<pair<string, string>>& sslConfCmds)
{
    // 一对象一端口(v2.5):仅允许设置一次;重复设置报错并忽略(多端口面用多个实例)
    if (m_listenerSet)
    {
        PUBLIC_LOG_ERROR("AddListener 重复设置已忽略(一对象一端口): 已有 port={}, 忽略 port={}",
                         m_listener.port, port);
        return;
    }
    if (port == 0)
    {
        PUBLIC_LOG_ERROR("AddListener 端口非法(port=0),忽略");
        return;
    }
    if (IsOpened())
    {
        PUBLIC_LOG_ERROR("AddListener 已跳过(run 后不可再添加监听): port={}", port);
        return;
    }
    // certFile/keyFile 传空 → 使用全局 setSSLFiles 配置(头文件契约,保证热加载)
    app().addListener(ip, port, useSSL, "", "", useOldTLS, sslConfCmds);
    m_listener.port = port;
    m_listener.useSSL = useSSL;
    m_listener.ip = ip;
    m_listener.useOldTLS = useOldTLS;
    m_listener.sslConfCmds = sslConfCmds;
    m_listenerSet = true;
    s_hasListener.store(true, std::memory_order_relaxed);
}

bool ZmHttpServer::ReloadCertificates()
{
    if (!s_hasCert.load(std::memory_order_relaxed))
        return false;
    app().reloadSSLFiles();   // 热加载:换内容不换路径(运行期唯一可热更新能力)
    return true;
}

// ============================================================================
// 流式接收(FR-15,路径 B;与 Options.enableRequestStream 配套)
//   drogon stream 契约:registerHandler 底层 FunctionTraits 匹配三参形态
//   (req, RequestStreamPtr&&, callback) → isStreamHandler = true,框架自动经
//   internal::createRequestStream 注入流对象;数据经 RequestStreamReader 逐块交付。
//   RegisterStreamCoro:三参流式回调(事件循环) → async_run 桥接业务协程。
//   SaveStreamToFile:块到即写 + maxBytes 兜底,完成回调恢复协程。
// ============================================================================
namespace
{
/// 上传落盘状态机:网络块经有界队列交专用写线程顺序落盘,
/// 事件循环只入队与收结果回执(写盘绝不占用事件循环线程 — NFR)
/// 流式上游(FR-15)写入执行器:事件循环接收数据块(fifo 入队)→ 专用写线程落盘;
/// 处理 停写(超限/用户取消/网络中断)、进度节流、结束/失败回执,半成品一致性。
class ZmUploadSink : public std::enable_shared_from_this<ZmUploadSink>
{
public:
    static std::shared_ptr<ZmUploadSink>
    Create(drogon::RequestStreamPtr stream, std::string destPath,
           ZmHttpUploadFileOptions opts, bool* tooLarge,
           std::function<void(bool)> done)
    {
        auto p = std::shared_ptr<ZmUploadSink>(new ZmUploadSink(
            std::move(stream), std::move(destPath), std::move(opts),
            tooLarge, std::move(done)));
        p->Attach();
        return p;
    }

    ~ZmUploadSink()
    {
        // 写线程退出前持有自身引用,文件清理由写线程完成;析构只停线程(安全 join)
        if (m_writer && (m_writer->IsRunning() || m_writer->IsStopping()))
        {
            {
                std::lock_guard lk(m_wmtx);
                m_wStop = true;
                m_wcv.notify_all();
            }
            m_writer->Stop();
        }
    }

private:
    /// 写积压上限(条);超出即中止上传,保证缓冲内存有界。
    /// 注:须 ≥ 单次同步灌入的最大块数(读回调挂上前 drogon 会同步吐出已缓冲块)
    static constexpr size_t kUploadQueueCap = 128;

    ZmUploadSink(drogon::RequestStreamPtr stream, std::string destPath,
                 ZmHttpUploadFileOptions opts, bool* tooLarge,
                 std::function<void(bool)> done)
        : m_stream(std::move(stream)), m_dest(std::move(destPath)),
          m_opts(std::move(opts)), m_tooLarge(tooLarge), m_done(std::move(done))
    {
    }

    void Attach()
    {
        // Create 的返回值被 awaiter 丢弃(见 ZmSinkAwaiter),此处自引用持有自身,
        // 直到 Done 完成回调才释放 —— 保证 sink 存活至收尾(done 必达)
        m_self = shared_from_this();
        // 写线程负责打开/写入/收尾(句柄生命周期全在 I/O 线程)。
        // 必须先于 setStreamReader 启动:挂读回调时 drogon 会同步灌入已缓冲的
        // 请求体,写线程先就位才能即时消费,避免初始突发撞上积压上限。
        m_writer = std::make_unique<ZmThread>("ZmUploadWrite",
            [wp = weak_from_this()]() {
                if (auto self = wp.lock())   // 强引用保证 sink 存活至写线程退出
                    self->WriterMain(self);
            });
        if (!m_writer->Start())
        {
            PUBLIC_LOG_ERROR("ZmUploadSink 写线程启动失败: {}", m_dest);
            m_stream->setStreamReader(drogon::RequestStreamReader::newNullReader());
            Finish(false, true);
            return;
        }
        m_stream->setStreamReader(drogon::RequestStreamReader::newReader(
            [wp = weak_from_this()](const char* buf, size_t len) {
                if (auto p = wp.lock())
                    p->OnData(buf, len);
            },
            [wp = weak_from_this()](std::exception_ptr e) {
                if (auto p = wp.lock())
                    p->OnFinish(e);
            }));
    }

    // ---- 以下仅由专用写线程调用(句柄/文件生命周期) ----

    bool OpenFile()
    {
        wchar_t wpath[2048];
        if (MultiByteToWideChar(CP_UTF8, 0, m_dest.c_str(), -1, wpath, 2048) <= 0)
            return false;
        m_handle = ::CreateFileW(wpath, GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
    }

    bool WriteAll(const char* buf, size_t len)
    {
        size_t off = 0;
        while (off < len)
        {
            DWORD wrote = 0;
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - off, (1u << 26)));
            if (!::WriteFile(m_handle, buf + off, chunk, &wrote, nullptr) || wrote == 0)
                return false;
            off += wrote;
        }
        return true;
    }

    void CloseFile()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    void Cleanup(bool deleteFile)   // 失败/中止/超限 → 删半成品
    {
        CloseFile();
        if (deleteFile)
        {
            wchar_t wpath[2048];
            if (MultiByteToWideChar(CP_UTF8, 0, m_dest.c_str(), -1, wpath, 2048) > 0)
                ::DeleteFileW(wpath);
        }
    }

    void Done(bool ok)
    {
        if (m_done)
        {
            auto d = std::move(m_done);
            m_done = nullptr;
            d(ok);
        }
        // 完成收尾:释放自引用;此后若写线程已退出,剩余引用在事件循环上归零 → 析构
        m_self.reset();
    }

    /// 写线程 → 事件循环 的结果回执;强引用保证残局(清理/done)必执行
    void PostFinish(std::shared_ptr<ZmUploadSink> self, bool ok, bool tooLarge)
    {
        auto loop = drogon::app().getLoop();
        loop->queueInLoop([self, ok, tooLarge]() { self->Finish(ok, tooLarge); });
    }

    /// 专用写线程主循环:开文件 → 顺序消费队列落盘 → 结果回执到事件循环
    void WriterMain(std::shared_ptr<ZmUploadSink> keepAlive)
    {
        if (!OpenFile())
        {
            // 开文件失败:语义与现状一致(置 tooLarge 标记),删半成品
            m_writerDead.store(true);
            PostFinish(std::move(keepAlive), false, true);
            return;
        }
        uint64_t written = 0;
        for (;;)
        {
            std::string item;
            bool drain = false;
            {
                std::unique_lock lk(m_wmtx);
                m_wcv.wait(lk, [this] {
                    return m_wStop.load() || m_streamEnded.load() ||
                           !m_pending.empty();
                });
                if (!m_pending.empty())
                {
                    item = std::move(m_pending.front());
                    m_pending.pop_front();
                }
                else
                {
                    drain = true;   // 队列空 → 流已结束或要求停止
                }
            }
            if (drain)
                break;
            if (m_wStop.load())
                break;   // 中止:丢弃残余块,不再落盘

            if (!WriteAll(item.data(), item.size()))
            {
                PUBLIC_LOG_ERROR("ZmUploadSink 写盘失败: {}", m_dest);
                Cleanup(true);
                m_writerDead.store(true);
                PostFinish(std::move(keepAlive), false, false);
                return;
            }
            written += item.size();
            m_written.store(written, std::memory_order_relaxed);
            if (m_opts.maxBytes > 0 && written > m_opts.maxBytes)
            {
                {   // 超限:清空残余并中止
                    std::lock_guard g(m_wmtx);
                    m_pending.clear();
                    m_wStop = true;
                }
                Cleanup(true);   // 超限 → 删半成品
                m_writerDead.store(true);
                PostFinish(std::move(keepAlive), false, true);
                return;
            }
        }
        // 正常收尾(流结束):成功关句柄;中止路径删半成品
        bool ok = !m_wStop.load();
        if (ok)
            CloseFile();
        else
            Cleanup(true);
        m_writerDead.store(true);
        PostFinish(std::move(keepAlive), ok, false);
    }

    void Finish(bool ok, bool tooLarge)
    {
        if (m_fini)
            return;
        m_fini = true;
        if (m_tooLarge)
            *m_tooLarge = tooLarge;
        if (!ok || tooLarge)
            m_stream->setStreamReader(drogon::RequestStreamReader::newNullReader());
        Done(ok);
    }

    void OnData(const char* buf, size_t len)
    {
        if (m_fini || m_writerDead.load())
            return;
        bool overflow = false;
        {
            std::lock_guard lk(m_wmtx);
            if (m_pending.size() >= kUploadQueueCap)
                overflow = true;   // 写积压超限 → 中止,内存有界
            else
            {
                m_pending.emplace_back(buf, len);   // 一次拷贝(网络块 → 写队)
                m_wcv.notify_all();                 // 唤醒写线程消费
            }
        }
        if (overflow)
        {
            PUBLIC_LOG_WARN("ZmUploadSink 写积压超限,中止上传: {}", m_dest);
            {
                std::lock_guard lk(m_wmtx);
                m_wStop = true;
                m_pending.clear();
                m_wcv.notify_all();
            }
            Finish(false, false);
            return;
        }
        if (m_opts.onProgress)
        {
            int64_t now = NowMs();
            uint64_t written = m_written.load(std::memory_order_relaxed);
            if (written == len || now - m_lastProgressMs >= (int64_t)m_opts.progressIntervalMs)
            {
                m_lastProgressMs = now;
                m_opts.onProgress(written, m_opts.maxBytes);
            }
        }
    }

    void OnFinish(std::exception_ptr e)
    {
        if (m_fini)
            return;
        {
            std::lock_guard lk(m_wmtx);
            if (e)
            {
                m_wStop = true;     // 网络中断 → 半成品作废
                m_pending.clear();
            }
            m_streamEnded.store(true);
            m_wcv.notify_all();
        }
        if (!e)
            return;   // 正常结束:等写线程把队列落完再回执成功
        Finish(false, false);
    }

    drogon::RequestStreamPtr m_stream;
    std::string m_dest;
    ZmHttpUploadFileOptions m_opts;
    bool* m_tooLarge = nullptr;
    std::function<void(bool)> m_done;
    std::shared_ptr<ZmUploadSink> m_self;   ///< 自引用:Create 返回值被丢弃,靠它撑生命周期

    // 文件句柄生命周期归属写线程;事件循环不得触碰
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    std::unique_ptr<ZmThread> m_writer;
    std::mutex m_wmtx;
    std::condition_variable m_wcv;
    std::deque<std::string> m_pending;          ///< FIFO 写队列(事件循环入队/写线程消费)
    std::atomic<bool> m_wStop{false};           ///< 停止请求(中止/超限/网络中断)
    std::atomic<bool> m_streamEnded{false};     ///< 请求体已结束(写线程排空后收尾)
    std::atomic<bool> m_writerDead{false};      ///< 写线程已退出(防入队到无人消费)
    std::atomic<uint64_t> m_written{0};         ///< 已落盘字节(写线程更新,事件循环读进度)
    int64_t m_lastProgressMs = 0;               ///< 仅事件循环线程访问
    bool m_fini = false;                        ///< 仅事件循环线程访问
};

/// SaveStreamToFile 的恢复器:完成回调(CallbackAwaiter 子类) → 值 + resume
struct ZmSinkAwaiter : drogon::CallbackAwaiter<bool>
{
    drogon::RequestStreamPtr stream;
    std::string dest;
    ZmHttpUploadFileOptions opts;
    bool* tooLarge = nullptr;

    void await_suspend(std::coroutine_handle<> h)
    {
        ZmUploadSink::Create(std::move(stream), std::move(dest), std::move(opts),
                             tooLarge, [this, h](bool ok) {
            this->setValue(ok);
            h.resume();
        });
    }
};

}  // namespace

/// 流式接收落盘助手(FR-15):把 RequestStream(块到达即回调)写入 destPath,
/// 全程经 ZmUploadSink(事件循环入队 + 专用写线程,磁盘 I/O 不占事件循环)。
/// 失败(网络中断/写盘错误/超限)会清理半成品并置 *tooLarge(超限语义)。
/// @return true=完整落盘成功;false=失败(经 tooLarge 区分超限/其他)。
drogon::Task<bool> ZmHttpServer::SaveStreamToFile(drogon::RequestStreamPtr stream,
                                                  const std::string& destPath,
                                                  const ZmHttpUploadFileOptions& opts,
                                                  bool* tooLarge)
{
    ZmSinkAwaiter a;
    a.stream = std::move(stream);
    a.dest = destPath;
    a.opts = opts;
    a.tooLarge = tooLarge;
    co_return co_await a;
}

/// 注册"流式接收"路由(FR-15 路径 B):handler 形参带 RequestStreamPtr,
/// drogon FunctionTraits 判定为 stream-handler,框架注入流对象并逐块回调。
/// @param maxBytes 路由级上传上限(0=不限):X-File-Size 声明超限 → 丢剩余并
///                 413(newNullReader),并把上限写 req attributes("ZmStreamMaxBytes")
///                 供业务 SaveStreamToFile 兜底取用。
void ZmHttpServer::RegisterStreamCoro(const string& path, drogon::HttpMethod m,
                                      ZmHttpStreamHandler h, const vector<string>& filters,
                                      uint64_t maxBytes)
{
    vector<internal::HttpConstraint> cons;
    cons.emplace_back(m);
    for (const auto& fn : filters)
    {
        if (!CheckFilterRegistered(fn))
            PUBLIC_LOG_ERROR("RegisterStreamCoro[{}]: filter 未注册: {}", path, fn);
        cons.emplace_back(fn);
    }

    // 三参流式回调(FunctionTraits 匹配 (req, stream, cb) → isStreamHandler):
    // 数据逐块交付;内部 bridge 到业务协程(异步回调 → 协程,FR-05 协程形态统一)
    auto fn = [h = std::move(h), maxBytes](
                  const HttpRequestPtr& req, drogon::RequestStreamPtr&& streamCtx,
                  std::function<void(const HttpResponsePtr&)>&& cb) {
        // 路由级上限(注册参数):X-File-Size 声明超限 → 丢弃剩余并 413,不进入业务
        if (maxBytes > 0)
        {
            uint64_t declared = 0;
            string xfs = req->getHeader("X-File-Size");
            if (!xfs.empty())
            {
                try { declared = std::stoull(xfs); }
                catch (const std::exception&) {}
            }
            if (declared > maxBytes)
            {
                streamCtx->setStreamReader(drogon::RequestStreamReader::newNullReader());
                cb(ZmHttpServer::ErrorResponse(413, "file too large"));
                return;
            }
            // 上限透传进 req attributes,供业务 SaveStreamToFile 兜底取用
            req->getAttributes()->insert("ZmStreamMaxBytes",
                                         std::any(uint64_t(maxBytes)));
        }

        drogon::async_run([h, req, streamCtx = std::move(streamCtx),
                           cb = std::move(cb)]() -> drogon::Task<> {
            try
            {
                auto resp = co_await h(req, std::move(streamCtx));
                if (resp)
                    cb(resp);
                else
                    cb(ZmHttpServer::ErrorResponse(500, "stream handler: null response"));
            }
            catch (const std::exception& e)
            {
                PUBLIC_LOG_ERROR("RegisterStreamCoro 业务异常: {}", e.what());
                cb(ZmHttpServer::ErrorResponse(500, "stream handler: internal error"));
            }
            catch (...)
            {
                cb(ZmHttpServer::ErrorResponse(500, "stream handler: internal error"));
            }
        });
    };

    if (path.find('{') == string::npos)
        app().registerHandler(path, std::move(fn), cons);
    else
        app().registerHandlerViaRegex(PathPatternToRegex(path), std::move(fn), cons);
}

// ============================================================================
// 路由与 Filter(FR-05/06)
// ============================================================================
bool ZmHttpServer::CheckFilterRegistered(const string& name)
{
    std::lock_guard lock(s_filterMtx);
    return s_filters.count(name) != 0;
}

void ZmHttpServer::AddFilter(const string& name,
                                 const std::function<bool(const HttpRequestPtr&,
                                                          HttpResponsePtr&)>& f)
{
    {
        std::lock_guard lock(s_filterMtx);
        s_filters[name] = f;
    }
    // 按名注册:自定义名 + 共享工厂(经注册表按名取回函数,设计 §4.2)
    DrClassMap::registerClass(name, nullptr, [name]() -> std::shared_ptr<DrObjectBase> {
        std::function<bool(const HttpRequestPtr&, HttpResponsePtr&)> fn;
        {
            std::lock_guard lock(s_filterMtx);
            auto it = s_filters.find(name);
            if (it == s_filters.end())
                return std::shared_ptr<DrObjectBase>(nullptr);
            fn = it->second;
        }
        return std::make_shared<ZmFuncFilter>(fn);
    });
    // 强制物化单例,保障框架按名解析(与 Api 契约一致)
    DrClassMap::getSingleInstance(name);
}

void ZmHttpServer::RegisterCoro(const string& path, drogon::HttpMethod m,
                                    ZmHttpCoroHandler h, const vector<string>& filters)
{
    vector<internal::HttpConstraint> cons;
    cons.emplace_back(m);
    for (const auto& fn : filters)
    {
        if (!CheckFilterRegistered(fn))
            PUBLIC_LOG_ERROR("RegisterCoro[{}]: filter 未注册: {}", path, fn);
        cons.emplace_back(fn);
    }
    // 适配:本捆绑 drogon 的 FunctionTraits 协程特化(zig 形态 HttpRequestPtr 按值);
    // ZmHttpCoroHandler 对外仍用 const 引用(业务友好),此处包一层。
    // ZmHttpCoroHandler 形参按值(HttpRequestPtr),FunctionTraits 协程特化可直接匹配;
    // 直接注册 std::function,不套中间协程(套层会在 Windows 上崩溃,实测)。
    if (path.find('{') == string::npos)
    {
        app().registerHandler(path, std::move(h), cons);
    }
    else
    {
        // {N} 占位符:本捆绑 drogon 的 registerHandler 转换路径在 Windows 崩溃,
        // 改经 registerHandlerViaRegex 手动转换(转义 + {N}→([^/]+))
        app().registerHandlerViaRegex(PathPatternToRegex(path), std::move(h), cons);
    }
}

string ZmHttpServer::PathPatternToRegex(const string& path)
{
    string out;
    out.reserve(path.size() + 16);
    for (size_t i = 0; i < path.size();)
    {
        char c = path[i];
        if (c == '{')
        {
            size_t end = path.find('}', i);
            if (end != string::npos)
            {
                out += "([^/]+)";
                i = end + 1;
                continue;
            }
        }
        // 转义正则元字符
        switch (c)
        {
        case '.': case '+': case '*': case '?': case '(': case ')':
        case '[': case ']': case '^': case '$': case '|': case '\\':
            out += '\\';
            break;
        default:
            break;
        }
        out += c;
        ++i;
    }
    return out;
}

/// 注册带业务级 deadline 的协程路由(FR-14):到期由事件循环定时器发 504,
/// 原子门(TryReply)保证只回一次;业务晚到结果经"弱引用 + connected()"安全丢弃。
/// 定时器注册在连接所属 loop;流式/下载端点不要用本接口。
void ZmHttpServer::RegisterCoroWithDeadline(const string& path, drogon::HttpMethod m,
                                                ZmHttpCoroHandler h, size_t deadlineMs,
                                                const vector<string>& filters)
{
    vector<internal::HttpConstraint> cons;
    cons.emplace_back(m);
    for (const auto& fn : filters)
    {
        if (!CheckFilterRegistered(fn))
            PUBLIC_LOG_ERROR("RegisterCoroWithDeadline[{}]: filter 未注册: {}", path, fn);
        cons.emplace_back(fn);
    }

    // callback-form 注册:框架回调仅能发一次;超时定时器放连接所属 loop(设计 §7 v2.1);
    // 业务晚到结果经"原子门 + 弱引用 + connected()"安全丢弃(FR-14)
    // 注:本捆绑 drogon 的协程函数指针特化要求首参按值 HttpRequestPtr、回调整参按值传递
    app().registerHandler(path,
        [h, deadlineMs](HttpRequestPtr req,
                        std::function<void(const HttpResponsePtr&)> cb) -> Task<> {
            auto gate = std::make_shared<std::atomic<bool>>(false);
            auto connWk = req->getConnectionPtr();

            trantor::EventLoop* loop = nullptr;
            if (auto c = connWk.lock())
                loop = c->getLoop();
            if (!loop)
                loop = app().getLoop();
            loop->runAfter(deadlineMs / 1000.0,
                [gate, cb, connWk]() {
                    if (gate->exchange(true))
                        return;
                    auto c = connWk.lock();
                    if (!c || !c->connected())
                        return;
                    ZMJSON data;
                    data["error"]["code"] = 504;
                    data["error"]["message"] = "deadline exceeded";
                    cb(JsonResponse(504, data));
                });

            try
            {
                auto resp = co_await h(req);
                if (gate->exchange(true))
                    co_return;                      // 已被超时占位
                auto c = connWk.lock();
                if (!c || !c->connected())
                    co_return;                      // 连接已关,丢弃
                if (resp)
                    cb(resp);
                else
                    cb(ErrorResponse(500, "handler returned null response"));
            }
            catch (const std::exception& e)
            {
                PUBLIC_LOG_ERROR("RegisterCoroWithDeadline 业务异常: {}", e.what());
                if (!gate->exchange(true))
                    cb(ErrorResponse(500, "internal error"));
            }
            catch (...)
            {
                if (!gate->exchange(true))
                    cb(ErrorResponse(500, "internal error"));
            }
        },
        cons);
}

// ============================================================================
// advice 挂点(FR-07)
//   透传注册。纪律:同一 advice 只由归属面注册一次(门禁/SPA 归前端面、
//   CORS 与 OPTIONS 预检归 RESTful 面);advice 组合(如复用 PreRouting 挂多个)
//   允许按序注册。基类内置项(/ping、AccessLogger)仍经 once_flag 去重。
// ============================================================================
void ZmHttpServer::RegisterPreRouting(std::function<void(const HttpRequestPtr&,
                                                             AdviceCallback&&,
                                                             AdviceChainCallback&&)> a)
{
    app().registerPreRoutingAdvice(std::move(a));
}

void ZmHttpServer::RegisterPostRouting(std::function<void(const HttpRequestPtr&,
                                                              AdviceCallback&&,
                                                              AdviceChainCallback&&)> a)
{
    app().registerPostRoutingAdvice(std::move(a));
}

void ZmHttpServer::RegisterPostHandling(std::function<void(const HttpRequestPtr&,
                                                               const HttpResponsePtr&)> a)
{
    app().registerPostHandlingAdvice(std::move(a));
}

void ZmHttpServer::RegisterPreSending(std::function<void(const HttpRequestPtr&,
                                                             const HttpResponsePtr&)> a)
{
    app().registerPreSendingAdvice(std::move(a));
}

// ============================================================================
// 文件传输(FR-12):Range 解析 + 方案甲/乙 + Hybrid
// ============================================================================
// ----------------------------------------------------------------------------
// ParseRange —— 请求 Range 头解析(方案甲/乙共用)
//   支持:bytes=a-b(闭区间)、bytes=a-(开终点,到文件尾)、bytes=-N(后缀 N 字节)
//   规则:仅单段;多段/非数字/起点越界/终点小于起点 → valid=false
//        (present=true 且 !valid 时调用方按 RFC 7233 返回 416);
//        无 Range 头 → present=false(调用方走全文件 200);
//   注:drogon 的 newFileResponse 不解析 Range 头,故所有范围语义在此统一实现。
// ----------------------------------------------------------------------------
ZmHttpServer::RangeInfo ZmHttpServer::ParseRange(const HttpRequestPtr& req,
                                                         size_t fileSize)
{
    RangeInfo r;
    string range = req->getHeader("Range");
    if (range.empty())
        return r;                                    // 无 Range → 全文件(present=false)
    r.present = true;

    if (range.rfind("bytes=", 0) != 0)
    {
        r.valid = false;
        return r;
    }
    string body = range.substr(6);
    if (body.find(',') != string::npos)
    {
        r.valid = false;                             // 多段不支持
        return r;
    }
    size_t dash = body.find('-');
    if (dash == string::npos)
    {
        r.valid = false;
        return r;
    }
    string startS = body.substr(0, dash);
    string endS = body.substr(dash + 1);

    if (startS.empty() && endS.empty())
    {
        r.valid = false;
        return r;
    }
    if (!startS.empty() && !std::all_of(startS.begin(), startS.end(),
                                        [](char c) { return std::isdigit((unsigned char)c); }))
    {
        r.valid = false;
        return r;
    }
    if (!endS.empty() && !std::all_of(endS.begin(), endS.end(),
                                      [](char c) { return std::isdigit((unsigned char)c); }))
    {
        r.valid = false;
        return r;
    }

    r.partial = true;
    try
    {
        if (startS.empty())
        {
            // 后缀范围:bytes=-N(最后 N 字节)
            size_t n = std::stoull(endS);
            if (n == 0)
            {
                r.valid = false;
                return r;
            }
            n = std::min(n, fileSize);
            r.offset = fileSize - n;
            r.length = n;
        }
        else
        {
            size_t start = std::stoull(startS);
            if (start >= fileSize)
            {
                r.valid = false;                     // 起点越界 → 416
                return r;
            }
            size_t end = endS.empty() ? fileSize - 1 : std::stoull(endS);
            if (end < start)
            {
                r.valid = false;
                return r;
            }
            end = std::min(end, fileSize - 1);
            r.offset = start;
            r.length = end - start + 1;
        }
        r.valid = true;
    }
    catch (...)
    {
        r.valid = false;
    }
    return r;
}

HttpResponsePtr ZmHttpServer::Range416Response(bool hasRange, size_t fileSize)
{
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k416RequestedRangeNotSatisfiable);
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody("{\"error\":{\"code\":416,\"message\":\"Range not satisfiable\"}}");
    if (hasRange)
        resp->addHeader("Content-Range", "bytes */" + std::to_string(fileSize));
    return resp;
}

const string& ZmHttpServer::MimeForExt(const string& path)
{
    static const std::map<string, string> m = {
        { ".html", "text/html; charset=utf-8" },  { ".htm", "text/html; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" },    { ".js", "application/javascript; charset=utf-8" },
        { ".json", "application/json; charset=utf-8" },
        { ".xml", "application/xml; charset=utf-8" },
        { ".png", "image/png" },                  { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" },                { ".gif", "image/gif" },
        { ".ico", "image/x-icon" },               { ".svg", "image/svg+xml" },
        { ".woff", "font/woff" },                 { ".woff2", "font/woff2" },
        { ".ttf", "font/ttf" },                   { ".mp3", "audio/mpeg" },
        { ".mp4", "video/mp4" },                  { ".opus", "audio/ogg" },
        { ".ogg", "audio/ogg" },                  { ".wav", "audio/wav" },
        { ".aac", "audio/aac" },                  { ".zip", "application/zip" },
        { ".doc", "application/msword" },         { ".pdf", "application/pdf" },
        { ".txt", "text/plain; charset=utf-8" },
        { ".csv", "text/csv; charset=utf-8" },    { ".md", "text/markdown; charset=utf-8" },
        { ".webp", "image/webp" },                { ".bmp", "image/bmp" },
        { ".avif", "image/avif" },                { ".tif", "image/tiff" },
        { ".tiff", "image/tiff" },
        { ".webm", "video/webm" },                { ".mkv", "video/x-matroska" },
        { ".mov", "video/quicktime" },
        { ".flac", "audio/flac" },                { ".m4a", "audio/mp4" },
        { ".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
        { ".ppt", "application/vnd.ms-powerpoint" },
        { ".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
        { ".xls", "application/vnd.ms-excel" },
        { ".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
        { ".rar", "application/vnd.rar" },        { ".7z", "application/x-7z-compressed" },
        { ".tar", "application/x-tar" },          { ".gz", "application/gzip" },
        { ".eot", "application/vnd.ms-fontobject" }, { ".otf", "font/otf" },
    };
    static const string noMime = "application/octet-stream";
    size_t p = path.find_last_of('.');
    if (p == string::npos)
        return noMime;
    auto it = m.find(path.substr(p));
    return it == m.end() ? noMime : it->second;
}

// ============================================================================
// SendFileCoro —— 文件下载"方案甲":(FR-12)
//   直接使用 drogon::HttpResponse::newFileResponse(内部为 trantor sendFile,
//   零拷贝分段读盘入发送缓冲),Range/206/Content-Length/Accept-Ranges 语义由框架兜底。
//   适用:常规文件(< Hybrid 阈值);要求简洁、带宽内无额外延迟。
//   边界:无字节级水位(慢客户端背压依赖 trantor 输出缓冲,见设计 §6.1 O1 待实测)。
//   条件请求:Last-Modified(mtime)+ 强 ETag(size-mtime) → If-None-Match 优先、
//   If-Modified-Since 兜底 → 304(缓存节流,配合前端静态资源显著省流量)。
// ============================================================================
drogon::Task<HttpResponsePtr> ZmHttpServer::SendFileCoro(const HttpRequestPtr& req,
                                                         const string& path,
                                                         const string& attachmentName)
{
    // 单次 stat(存在性/大小/mtime;失败 404/500),转 Impl 复用元信息
    ZmFileMeta m = FetchFileMeta(path);
    if (!m.found)
        co_return ErrorResponse(404, "file not found");
    if (m.sizeFailed)
        co_return ErrorResponse(500, "file stat failed");
    co_return co_await SendFileCoroImpl(req, path, attachmentName, m.size, m.mtimeSec);
}

drogon::Task<HttpResponsePtr> ZmHttpServer::SendFileCoroImpl(const HttpRequestPtr& req,
                                                             const string& path,
                                                             const string& attachmentName,
                                                             size_t fileSize,
                                                             int64_t mtimeSec)
{
    ZmFileMeta m;                       // 元信息由公开入口/Hybrid 已取,此处只组装
    m.size = fileSize;
    m.mtimeSec = mtimeSec;

    // ① 条件请求(304 无 body);命中即返回,不再走 Range/200
    auto cacheHeaders = CacheHeaders(m);
    if (auto notMod = Maybe304(req, m, cacheHeaders))
        co_return notMod;

    // ② 解析 Range 头(共用解析器):present=带 Range;valid=单段合法(多段/非法 → 无效)
    RangeInfo r = ParseRange(req, fileSize);
    if (r.present && !r.valid)
    {
        // Range 头存在但不可满足 → 413/416 语义:416 + Content-Range: bytes */size
        co_return Range416Response(true, fileSize);
    }
    // ③ "bytes=a-" 这类开放终点:length=0 表示"到文件尾",这里把语义具体化
    if (r.partial && r.length == 0)
        r.length = fileSize - r.offset;

    // ④ 构造文件响应:
    //    partial(有合法 Range)→ offset/length 只发区间,setContentRange 由下面手动加头;
    //    全文件 → offset=0,length=fileSize;
    //    attachmentName 非空 → 框架自动写 Content-Disposition: attachment。
    //    ⚠ setContentRange 传 false:统一由本函数显式写 Content-Range,避免框架/手动双写
    HttpResponsePtr resp = HttpResponse::newFileResponse(
        path, r.offset, r.partial ? r.length : fileSize,
        /*setContentRange=*/false, attachmentName, CT_NONE, "", req);

    // ⑤ 缓存头(与 304 一致:客户端代理/浏览器核对条件)
    resp->addHeader("Last-Modified", cacheHeaders.first);
    resp->addHeader("ETag", cacheHeaders.second);
    // ⑥ 告知客户端支持断点续传(Range 请求有效依据)
    resp->addHeader("Accept-Ranges", "bytes");
    // ⑦ 部分内容:206 + Content-Range: bytes {offset}-{offset+length-1}/{fileSize}
    if (r.partial)
    {
        resp->setStatusCode(k206PartialContent);
        resp->addHeader("Content-Range",
                        "bytes " + std::to_string(r.offset) + "-" +
                            std::to_string(r.offset + r.length - 1) + "/" +
                            std::to_string(fileSize));
    }
    co_return resp;
}

// ============================================================================
// SendFileStreamCoro —— 文件下载"方案乙":(FR-12)
//   newAsyncStreamResponse 分块流式(Transfer-Encoding: chunked,不设 Content-Length),
//   块间定时器节流:预读窗口 = 1 块 → 内存有界(慢客户端缓冲不随时长线性涨)。
//   读盘走专用 I/O 线程池(HttpIoPool),事件循环线程绝不阻塞(NFR);
//   支持停滞放弃(stallAbortMs,客户端 Range 续传)与进度回调(onProgress)。
//   适用:≥Hybrid 阈值的大文件、需要进度回调/长连接稳定性控制的场景。
//   条件请求:同方案甲(304 为普通响应,不在流内)。
// ============================================================================
drogon::Task<HttpResponsePtr> ZmHttpServer::SendFileStreamCoro(const HttpRequestPtr& req,
                                                               const string& path,
                                                               const string& attachmentName,
                                                               const ZmHttpSendFileOptions& opts)
{
    // 单次 stat(存在性/大小/mtime;失败 404/500),转 Impl 复用元信息
    ZmFileMeta m = FetchFileMeta(path);
    if (!m.found)
        co_return ErrorResponse(404, "file not found");
    if (m.sizeFailed)
        co_return ErrorResponse(500, "file stat failed");
    co_return co_await SendFileStreamCoroImpl(req, path, attachmentName, opts,
                                              m.size, m.mtimeSec);
}

drogon::Task<HttpResponsePtr> ZmHttpServer::SendFileStreamCoroImpl(
    const HttpRequestPtr& req, const string& path, const string& attachmentName,
    const ZmHttpSendFileOptions& opts, size_t fileSize, int64_t mtimeSec)
{
    // ① 条件请求(304 无 body);命中即返回,不再走流式
    ZmFileMeta m;
    m.size = fileSize;
    m.mtimeSec = mtimeSec;
    auto cacheHeaders = CacheHeaders(m);
    if (auto notMod = Maybe304(req, m, cacheHeaders))
        co_return notMod;

    // ② Range 解析(同方案甲共用):合法单段 → partial 区间;非法 → 416
    RangeInfo r = ParseRange(req, fileSize);
    if (r.present && !r.valid)
    {
        co_return Range416Response(true, fileSize);
    }
    if (r.partial && r.length == 0)
        r.length = fileSize - r.offset;

    // ③ 流式响应工厂:回调在发送启动时(事件循环线程)被框架调用,
    //    在此把 文件路径/区间/行为参数 注入发送状态机,并立即 Run() 驱动第一块;
    //    true = disableKickoffTimeout:禁用 trantor 默认启动超时(大文件/长流不被误杀)
    HttpResponsePtr resp = HttpResponse::newAsyncStreamResponse(
        [path, fileSize, r, opts, attachmentName](ResponseStreamPtr stream) mutable {
            auto st = std::make_shared<ZmStreamLoopState>();   // 状态机对象(持所有权)
            st->path = path;
            st->offset = r.offset;                              // Range 起点(续传定位)
            st->total = r.partial ? r.length : fileSize;       // 本次发送总量(区间或全文件)
            st->remaining = st->total;                          // 剩余待发字节
            st->abortMs = opts.stallAbortMs;                    // 停滞放弃阈值(默认 120s)
            st->opts = opts;
            st->stream = std::move(stream);                     // 排他持有流(close 由状态机负责)
            st->Run();                                          // 打开文件句柄并调度第一块
        },
        true);

    // ④ 响应头:断点续传声明 + MIME(方案乙无 Content-Length,chunked 编码)
    resp->addHeader("Accept-Ranges", "bytes");
    resp->addHeader("Content-Type", MimeForExt(path));
    // ⑤ 缓存头(与 304 一致;大文件下载客户端亦可条件续用)
    resp->addHeader("Last-Modified", cacheHeaders.first);
    resp->addHeader("ETag", cacheHeaders.second);
    // ⑥ 部分内容 → 206 + Content-Range(格式同方案甲;长度由 chunked 流承载)
    if (r.partial)
    {
        resp->setStatusCode(k206PartialContent);
        resp->addHeader("Content-Range",
                        "bytes " + std::to_string(r.offset) + "-" +
                            std::to_string(r.offset + r.length - 1) + "/" +
                            std::to_string(fileSize));
    }
    // ⑦ 下载文件名(浏览器另存为)
    if (!attachmentName.empty())
    {
        resp->addHeader("Content-Disposition",
                        "attachment; filename=\"" + attachmentName + "\"");
    }
    co_return resp;
}

// ============================================================================
// SendFileHybridCoro —— 文件下载"便捷入口":(FR-12)
//   按文件大小自动路由:fileSize < threshold → 方案甲 SendFileCoro(常规,零拷贝)
//                        fileSize ≥ threshold → 方案乙 SendFileStreamCoro(流式,内存有界)
//   阈值默认 2GB(调用方可覆盖;0 = 恒乙);不关心细节的业务层直接用本入口。
// ============================================================================
drogon::Task<HttpResponsePtr> ZmHttpServer::SendFileHybridCoro(const HttpRequestPtr& req,
                                                               const string& path,
                                                               const string& attachmentName,
                                                               size_t threshold,
                                                               const ZmHttpSendFileOptions& streamOpts)
{
    // 单次 stat(存在性/大小/mtime)后按阈值路由到对应内部实现,不重复取元信息
    ZmFileMeta m = FetchFileMeta(path);
    if (!m.found)
        co_return ErrorResponse(404, "file not found");
    if (m.sizeFailed)
        co_return ErrorResponse(500, "file stat failed");
    if (m.size < threshold)
        co_return co_await SendFileCoroImpl(req, path, attachmentName, m.size, m.mtimeSec);
    co_return co_await SendFileStreamCoroImpl(req, path, attachmentName, streamOpts,
                                              m.size, m.mtimeSec);
}

// ============================================================================
// 流式工厂(FR-13)
// ============================================================================
/// 流式响应工厂(FR-13):返回 newAsyncStreamResponse 响应,调用方自行设头;
/// disableKickoff=true 关闭 trantor 默认启动超时(长流/业务线程先启动场景必备)。
HttpResponsePtr ZmHttpServer::MakeStreamResponse(StreamCb cb, bool disableKickoff)
{
    return HttpResponse::newAsyncStreamResponse(std::move(cb), disableKickoff);
}

// ============================================================================
// WebSocket(FR-16;设计 §9)
// ============================================================================
/// 注册 WebSocket 路由(FR-16):每个 path 生成唯一注册名,经 DrClassMap 工厂
/// 实例化通用 ZmWsController,回调按注册名存入全局表(onAuth 拒绝语义见 §9)。
void ZmHttpServer::RegisterWebSocket(const string& path, const WsCallbacks& cb)
{
    string regName = BuildWsRegName();
    // factory 实例化全新 controller,回调经注册表按注册名取回
    DrClassMap::registerClass(regName,
        [regName]() -> DrObjectBase* { return new ZmWsController(regName); },
        [regName]() -> std::shared_ptr<DrObjectBase> {
            return std::make_shared<ZmWsController>(regName);
        });
    ZmWsController::SetCallbacks(regName, cb);
    app().registerWebSocketController(path, regName);
}

// ============================================================================
// 响应助手(FR-08/09/24;业务层使用 ZMJSON,输出为构造序)
// ============================================================================

/// drogon(Json::Value)→ 业务 ZMJSON(递归)。⚠ 对象键序按 Json::Value 内部
/// map 字典序;需要构造序时请业务侧直接用 ZMJSON 构造,勿经本转换。
ZMJSON ZmHttpServer::FromDrogonJson(const Json::Value& v)
{
    switch (v.type())
    {
    case Json::nullValue: return ZMJSON(nullptr);
    case Json::booleanValue: return ZMJSON(v.asBool());
    case Json::intValue: return ZMJSON(v.asInt64());
    case Json::uintValue: return ZMJSON(v.asUInt64());
    case Json::realValue: return ZMJSON(v.asDouble());
    case Json::stringValue:
        return ZMJSON(v.asString());
    case Json::arrayValue:
    {
        ZMJSON arr = ZMJSON::array();
        for (const auto& e : v)
            arr.push_back(FromDrogonJson(e));
        return arr;
    }
    case Json::objectValue:
    {
        // 对象键序按 Json::Value 内部(map 字典序);业务需要构造序时用 ZMJSON 直接构造
        ZMJSON obj = ZMJSON::object();
        for (auto it = v.begin(); it != v.end(); ++it)
            obj[it.name()] = FromDrogonJson(*it);
        return obj;
    }
    default:
        return ZMJSON(nullptr);
    }
}

/// 业务 ZMJSON → drogon(Json::Value)(递归)。仅 drogon API 要求处使用
/// (如 loadConfigJson 等价场景);业务链路无需接触 jsoncpp。
Json::Value ZmHttpServer::ToDrogonJson(const ZMJSON& v)
{
    if (v.is_null())
        return Json::Value(Json::nullValue);
    if (v.is_boolean())
        return Json::Value(v.get<bool>());
    if (v.is_number_integer())
        return Json::Value(v.get<int64_t>());
    if (v.is_number_unsigned())
        return Json::Value(v.get<uint64_t>());
    if (v.is_number_float())
        return Json::Value(v.get<double>());
    if (v.is_string())
        return Json::Value(v.get<string>());
    if (v.is_array())
    {
        Json::Value arr(Json::arrayValue);
        for (const auto& e : v)
            arr.append(ToDrogonJson(e));
        return arr;
    }
    if (v.is_object())
    {
        Json::Value obj;
        for (auto it = v.begin(); it != v.end(); ++it)
            obj[it.key()] = ToDrogonJson(it.value());
        return obj;
    }
    return Json::Value(Json::nullValue);
}

/// 统一 JSON 响应(FR-09):裸 JSON 体(业务语义),状态码由参数指定;
/// ZMJSON 直序列化 → 输出为构造序(键序可控)。
HttpResponsePtr ZmHttpServer::JsonResponse(int status, const ZMJSON& data)
{
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<HttpStatusCode>(status));
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(data.dump());
    return resp;
}

/// 统一错误响应(FR-08):{error:{code,message}} 错误包(与前端 auth.js 约定一致)
HttpResponsePtr ZmHttpServer::ErrorResponse(int status, const string& msg)
{
    ZMJSON error;
    error["error"]["code"] = status;
    error["error"]["message"] = msg;
    return JsonResponse(status, error);
}

bool ZmHttpServer::IsValidJsonpCallback(const std::string& cb)
{
    // 白名单 [A-Za-z0-9_.](FR-24,防 XSS 反射):长度 ≤128 且全字符合法
    if (cb.empty() || cb.size() > 128)
        return false;
    return std::all_of(cb.begin(), cb.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
    });
}

/// CORS 白名单查询(Init 注入 Options.corsAllowedOrigins,运行期只读):
/// 空名单 → 一律拒绝(不回显 CORS 头,浏览器判跨域失败);命中才放行凭据回显。
bool ZmHttpServer::IsCorsOriginAllowed(const std::string& origin)
{
    if (origin.empty())
        return false;
    for (const auto& o : s_corsOrigins)
    {
        if (o == origin)
            return true;
    }
    return false;
}

/// 显式 JSONP 响应(FR-24):有合法 callback → cb(json);(application/javascript);
/// 无 callback → 常规 JSON;非法 callback 名 → 400。
/// (自动 JSONP 为全局开关 SetAutoJsonp,经 PreSending advice 兜底包装)
HttpResponsePtr ZmHttpServer::JsonpResponse(const HttpRequestPtr& req, const ZMJSON& data)
{
    string cb = req->getParameter("callback");
    if (cb.empty())
        return JsonResponse(200, data);

    if (!IsValidJsonpCallback(cb))
        return ErrorResponse(400, "invalid callback name");

    string body = cb + "(" + data.dump() + ");";
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(CT_TEXT_JAVASCRIPT);
    resp->setBody(body);
    return resp;
}
