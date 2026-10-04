#ifndef NOMINMAX   // windows.h 的 min/max 宏会破坏 std::min/max;须在一切 include 前定义
#define NOMINMAX
#endif
#include "zm_net_http_server.h"
#include "zm_net_http_client.h"   // 服务器 loop 登记(SendSync 拒绝面)

#include "../util/zm_util_logger.h"
#include "../util/zm_util_thread.h"
#include "../util/zm_util_json.h"
#include "../util/zm_util_str.h"   // ZmString::UTF8_To_Unicode(路径 UTF-8 → wide 转换)

#include <../drogon/include/drogon/DrClassMap.h>
#include <../drogon/include/drogon/HttpAppFramework.h>
#include <../drogon/include/drogon/HttpFilter.h>
#include <../drogon/include/drogon/WebSocketController.h>
#include <../drogon/include/drogon/utils/HttpConstraint.h>
#include <../drogon/include/drogon/utils/Utilities.h>   // getHttpDate(条件请求 If-Modified-Since 解析)
#include <../drogon/include/drogon/RequestStream.h>

#include <../drogon/include/trantor/net/TcpConnection.h>
#include <../drogon/include/trantor/net/EventLoop.h>

#include <windows.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <cctype>
#include <deque>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

using namespace drogon;
using std::string;
using std::vector;
using std::pair;


// ── 文件级私有状态（各节共用） ──
//   匿名命名空间内的工具与静态表:Filter 适配 / 流式发送状态机 / WS 控制器 /
//   归属表 / JSONP 表 / 工作池大小。被多个节引用(或须先于引用者定义),故集中于此。

// 匿名命名空间:文件内私有工具(Filter 适配 / 流式发送状态机 / WS 控制器等)
namespace
{
/**
 * @brief 把 std::function 适配为 drogon HttpFilter(按名注册,构造时注入函数)
 *
 * 供 AddFilter 注册的按名工厂使用:框架实例化时从注册表取回过滤函数注入本类。
 */
class ZmFuncFilter : public drogon::HttpFilterBase
{
public:
    /**
     * @brief 构造适配器并绑定过滤函数
     * @param fn  过滤函数:返回 false 即拒绝请求
     */
    explicit ZmFuncFilter(
        std::function<bool(const HttpRequestPtr&, HttpResponsePtr&)> fn)
        : m_fn(std::move(fn))
    {
    }

    /**
     * @brief 执行过滤:通过则 fccb() 放行到下一环,否则 fcb(resp) 回响应
     *
     * @param req   请求
     * @param fcb   拒绝回调(交出响应)
     * @param fccb  链式回调(放行)
     */
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
// 方案乙流式发送状态机(定时器链)
// ----------------------------------------------------------------------------
/**
 * @brief 取文件传输专用 I/O 线程池
 *
 * 磁盘读/写绝不占用事件循环线程（NFR）。独立于全局工作池，避免与其它任务抢线程；
 * 起始 2 个 worker，任务积压时由线程池自身按需扩容。
 *
 * @return 进程级唯一实例的引用（首次调用时惰性创建）
 */
ZmThreadPool& HttpIoPool()
{
    static ZmThreadPool pool(2, "ZmHttpIo");
    return pool;
}

/**
 * @brief 方案乙（流式分块发送）的单次发送状态机
 *
 * 由 newAsyncStreamResponse 的工厂回调创建，Run() 打开文件句柄并驱动第一块；
 * 此后由"定时器 → 异步读 → 回执发送"的链式循环推进。
 *
 * 状态机归属本连接的事件循环：工厂回调由框架在该 loop 上调用
 * （HttpServer::sendResponse 断言在 loop 内），Run() 据此定型 m_loop，此后全部
 * 定时器与读回执都投回该 loop —— 状态与 conn->bytesSent() 的读写因此只发生在
 * 单一线程上，分块发送也不占用主 loop。
 *
 * 读盘投递到 HttpIoPool 执行，事件循环永不阻塞（NFR）。
 * 收尾统一走 Finish()（幂等）→ DoClose()：句柄与流的释放在途读计数归零后才执行，
 * 与 I/O 线程无竞态。
 *
 * 读盘出错或提前读到文件尾时按"发完"收尾：流正常关闭，客户端只看到更短的 body。
 * 纯 200 响应无 Content-Length，客户端无从察觉（206 由 Content-Range 声明长度，
 * 客户端可判定），故两条异常路径都记日志留痕（见 ReadChunk）。
 */
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
    /// 连接弱引用（停滞判定用：bytesSent 差值才反映对端真实消费）
    std::weak_ptr<trantor::TcpConnection> connWk;
    uint64_t lastConnSent = 0;   // 上次观测的连接累计发送字节(仅事件循环线程访问)

    void Run();   // 打开文件并驱动第一块
    void Next();  // 提交异步读 → I/O 线程读盘 → queueInLoop 回执发送(事件循环不阻塞)
    void OnReadDone(std::shared_ptr<std::string> buf, bool ok);
    void Finish();
    void DoClose();

    /// @brief 读一块到缓冲并投回本连接的事件循环发送(I/O 线程执行,绝不阻塞事件循环)
    ///
    /// 读失败与提前读到文件尾都产出 ok=false,由事件循环侧按"发完"收尾 ——
    /// 两条异常路径在此留日志:客户端只看到短了的一坨字节,日志是唯一线索。
    ///
    /// @param want 本次期望读取的字节数(不超过 chunkSize)
    void ReadChunk(uint64_t want)
    {
        DWORD rd = 0;
        auto buf = std::make_shared<std::string>();
        buf->resize(want);
        const BOOL readOk =
            ::ReadFile(m_handle, buf->data(), static_cast<DWORD>(want), &rd, nullptr);
        if (readOk && rd > 0)
            buf->resize(static_cast<size_t>(rd));
        else if (!readOk)
            PUBLIC_LOG_ERROR("SendFileStreamCoro 读盘失败(本块期望 {} 字节,GetLastError={}): {}",
                             want, ::GetLastError(), path);
        else
            // 提交时恒有 remaining > 0(Next 判定),故读到 0 字节 = 文件比 stat 时短
            PUBLIC_LOG_WARN("SendFileStreamCoro 提前读到文件尾(本块期望 {} 字节): {}",
                            want, path);
        const bool ok = (rd > 0);
        // m_loop 由 Run() 在本任务提交前定型(经线程池入队同步),此处只读
        m_loop->queueInLoop(
            [st = shared_from_this(), buf = std::move(buf), ok]() mutable {
                st->OnReadDone(std::move(buf), ok);
            });
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    /// 状态机所属事件循环(连接 loop;Run() 时定型,此后所有回调与定时器都投这里)
    trantor::EventLoop* m_loop = nullptr;
    bool m_fini = false;
    int m_inFlight = 0;   // 在途异步读计数(仅事件循环线程访问)
    bool m_closed = false;
    /// 本响应起点的连接累计发送字节(水位判定的排水量基准)
    uint64_t m_baseConnSent = 0;
    /// 被水位挡住时的下一轮延后(毫秒;1 起步翻倍到 250 封顶,排水恢复即归零)
    int64_t m_idleMs = 0;
};

/**
 * @brief 流式发送的读盘任务:在专用 I/O 线程读取一块,再投回事件循环发送
 */
struct ZmStreamReadTask
{
    std::shared_ptr<ZmStreamLoopState> st;       ///< 发送状态机(保活)
    uint64_t                           want = 0;  ///< 本次期望读取的字节数

    void operator()() const { st->ReadChunk(want); }
};

/**
 * @brief 取单调时钟毫秒数
 *
 * 基于 steady_clock，不受系统时间调整影响，用于耗时统计与停滞判定基准。
 *
 * @return 自系统启动起算的毫秒数
 */
static int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ----------------------------------------------------------------------------
// 请求 ID 与指标计数器(原子,每请求 1-4 次操作)
// ----------------------------------------------------------------------------
/// 请求 ID 的进程内自增序号(原子;每生成一次 +1)
static std::atomic<uint64_t> s_reqIdSeq{1};

// 响应结算指标(原子;PreRouting 起算在飞,PreSending 结算总/状态段/延迟桶)
static std::atomic<uint64_t> s_m_total{0};
static std::atomic<uint64_t> s_m_s2xx{0};
static std::atomic<uint64_t> s_m_s3xx{0};
static std::atomic<uint64_t> s_m_s4xx{0};
static std::atomic<uint64_t> s_m_s5xx{0};
static std::atomic<uint64_t> s_m_inflight{0};
static std::atomic<uint64_t> s_m_latLe100{0};
static std::atomic<uint64_t> s_m_latLe500{0};
static std::atomic<uint64_t> s_m_latLe2s{0};
static std::atomic<uint64_t> s_m_latGt2s{0};

/**
 * @brief 生成请求 ID
 *
 * 形态 zm-<unix秒>-<进程内原子序>,合法字符 [A-Za-z0-9-_.]。
 * 秒段取 system_clock(wall time)而非单调时钟 —— ID 的时间语义与日志/world time 一致,
 * 否则秒段变成"开机起算",跨重启无法对时。
 *
 * @return 形如 "zm-1757654321-42" 的请求 ID
 */
static string MakeRequestId()
{
    auto now = std::chrono::system_clock::now();
    return "zm-" +
           std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                              now.time_since_epoch())
                              .count()) +
           "-" + std::to_string(s_reqIdSeq.fetch_add(1, std::memory_order_relaxed));
}

/**
 * @brief 校验请求 ID 是否合法
 *
 * 上游透传的 X-Request-Id 必须先过本校验才会被沿用,防止脏字符(换行/空格等)污染日志行。
 *
 * @param id  待校验的请求 ID
 * @return true 合法(非空、长度 ≤128 且仅含 [A-Za-z0-9-_.]);false 不合法
 */
static bool IsValidRequestId(const string& id)
{
    if (id.empty() || id.size() > 128)
        return false;
    for (char c : id)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
            return false;
    }
    return true;
}

/**
 * @brief 打开文件句柄并驱动第一块
 *
 * 在事件循环线程执行(收到流对象时立即调用)。路径按 UTF-8 契约显式转 wide;
 * 有 offset 时先定位(Range 续传起点);打开或定位失败一律走 Finish() 收尾,
 * 由客户端自行 Range 续传。
 */
void ZmStreamLoopState::Run()
{
    // 定型状态机所属事件循环:本回调由框架在连接 loop 上调用(工厂回调即在发送起点),
    // 故取连接 loop;取不到(异常态)退回主 loop,与 ZmDeadlineState 同款取值顺序
    if (auto c = connWk.lock())
        m_loop = c->getLoop();
    if (!m_loop)
        m_loop = drogon::app().getLoop();
    // 路径为 UTF-8 契约(与上传侧一致):显式 CP_UTF8 → wide,窄串直接进 filesystem 会被按 ANSI 解码
    std::wstring wpath = ZmString::UTF8_To_Unicode(path);
    m_handle = CreateFileW(wpath.c_str(), GENERIC_READ,
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
    // 停滞判定基准:以连接累计发送字节为"对端真实消费"信号
    if (auto c = connWk.lock())
    {
        lastConnSent  = c->bytesSent();
        m_baseConnSent = lastConnSent;   // 水位基准:本响应起点
    }
    lastSentMs = NowMs();
    Next();
}

/**
 * @brief 调度一次异步读(块粒度 chunkSize,预读窗口 = 1 块)
 *
 * 在途计数 m_inFlight 保证:读回执到达前不再叠加下一块;所有状态只在事件循环线程变更。
 * 停止判定有两类:连接已关或已发完(直接 Finish)、对端消费停滞(放弃,客户端可续传)。
 */
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
    // 停滞判定:send() 返回 true 只代表排入 trantor 输出缓冲,
    // 不反映对端消费——以 conn->bytesSent()(套接字实发)差值为准:
    // 字节持续增长 = 对端在消费;冻结超过 abortMs = 对端停滞 → 放弃(客户端可 Range 续传)。
    // 注:bytesSent 覆盖响应头+正文,慢但仍在消费的客户端不会误杀。
    if (auto c = connWk.lock())
    {
        uint64_t sentNow = c->bytesSent();
        if (sentNow != lastConnSent)
        {
            lastConnSent = sentNow;
            lastSentMs = NowMs();
        }
        else if (NowMs() - lastSentMs > abortMs)
        {
            PUBLIC_LOG_WARN("SendFileStreamCoro 对端消费停滞放弃: {}", path);
            Finish();
            return;
        }
        // 软件水位:trantor 不暴露输出缓冲长度,故用"连接已实发字节相对本响应起点的增量"
        // 近似排水量。已提交字节领先排水量超过水位 = 对端吃得慢,延后下一块 ——
        // 否则慢客户端会把整份文件堆进内存(实测 2GB 文件 + 3MB/s 客户端曾涨到 3.5GB)。
        if (opts.watermarkBytes > 0 && sent > (sentNow - m_baseConnSent) + opts.watermarkBytes)
        {
            m_idleMs = m_idleMs == 0 ? 1 : std::min<int64_t>(m_idleMs * 2, 250);
            m_loop->runAfter(static_cast<double>(m_idleMs) / 1000.0,
                             [st = shared_from_this()]() { st->Next(); });
            return;
        }
    }
    m_idleMs = 0;   // 排水跟得上:恢复常规调度

    size_t want = static_cast<size_t>(
        std::min<uint64_t>(remaining, opts.chunkSize ? opts.chunkSize : 1));
    ++m_inFlight;   // 缓冲已就位,回执前不再叠加(预读窗口 = 1 块,内存有界)
    HttpIoPool().Submit(ZmStreamReadTask{shared_from_this(), want}, "ZmHttpRead");
}

/**
 * @brief 处理一次异步读的回执:发送数据块、推进进度、决定继续或收尾
 *
 * 由 queueInLoop 投递回事件循环线程执行(读线程只回执,不触碰 stream)。
 *
 * @param buf  读出的数据块(ok 为 false 时内容无意义)
 * @param ok   true = 读盘成功且读到至少 1 字节;false = 读失败或已到文件末尾
 */
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

    // raw 模式:直接写原始字节(响应自带 Content-Length,不做 chunked 分帧);
    // 默认模式:交给 ResponseStream 做分块编码
    const bool sendOk = opts.raw ? stream->sendRaw(buf->data(), buf->size())
                                 : stream->send(*buf);
    if (!sendOk)
    {
        // trantor 契约:send 返回 false = 连接已关闭
        Finish();
        return;
    }
    remaining -= buf->size();
    sent += buf->size();
    // 注:此处不再刷新 lastSentMs——停滞判定只认 bytesSent 差值,
    //     send() 成功 ≠ 对端消费。
    if (opts.onProgress && sent <= total)
        opts.onProgress(sent, total);

    if (remaining == 0)
    {
        Finish();
        return;
    }
    // 定时器节流:事件循环处理发送、缓冲自然排水(内存有界:在途预读 = 1 块)
    // interBlockMs == 0 → 发完即调度(runAfter(0) 下一轮立即跑,无节流,对应头文件契约)
    m_loop->runAfter(opts.interBlockMs / 1000.0,
                     [st = shared_from_this()]() { st->Next(); });
}

/**
 * @brief 请求收尾(幂等)
 *
 * 以 m_fini 做一次性门;仍有在途异步读时只置标志,把关句柄/关流留给最后一笔回执
 * 补做,避免与 I/O 线程的 ReadFile 竞态。
 */
void ZmStreamLoopState::Finish()
{
    if (m_fini)
        return;
    m_fini = true;
    // 有待在途读时缓至关句柄,由最后一笔回执补关,避免与 I/O 线程竞态
    if (m_inFlight == 0)
        DoClose();
}

/**
 * @brief 真正释放资源(文件句柄 + 响应流)
 *
 * 仅在途读计数为 0 时执行,故与 I/O 线程的 ReadFile 无竞态;m_closed 保证只做一次。
 */
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
        // raw 模式:关闭流但不写 chunked 终止帧(Content-Length 已界定正文长度)
        if (opts.raw)
            stream->closeRaw();
        else
            stream->close();   // 发送终止分块并关闭(线程安全)
        stream.reset();
    }
    // 结束回调:发完 / 断连 / 读失败 / 停滞 四条路径都汇到 DoClose(m_closed 保证仅一次),
    // 调用方据此回收并发名额等资源
    if (opts.onFinish)
    {
        auto cb = opts.onFinish;   // 拷贝后调用:回调内可能析构本状态机
        cb();
    }
}

// ----------------------------------------------------------------------------
// WebSocket 注册表
// ----------------------------------------------------------------------------
/**
 * @brief 通用 WebSocket 控制器:把全局回调表按注册名桥接到 drogon 回调接口
 *
 * 每个 WS 路径经 BuildWsRegName 生成唯一注册名,由 DrClassMap 工厂实例化本类;
 * 构造时把该注册名的回调快照到成员(按值拷贝),此后消息路径零加锁。
 */
class ZmWsController : public drogon::WebSocketControllerBase
{
public:
    /**
     * @brief 构造控制器并快照该注册名的回调集合
     * @param regName  注册名(须已由 SetCallbacks 写入全局表)
     */
    explicit ZmWsController(string regName)
        : m_regName(std::move(regName)), m_cbs(GetCallbacksCopy(m_regName))
    {
    }

    /**
     * @brief 连接建立:先鉴权(onAuth),拒绝则关闭连接,通过则回调 onOpen
     *
     * @param req   升级请求(供 onAuth 读取 cookie/token 等)
     * @param conn  已升级的 WS 连接
     *
     * @example
     *   // 回调侧声明:onAuth 返回 false → 连接被以 kEndpointGone 关闭,
     *   // 业务侧不会再收到 onOpen/onMessage。
     */
    void handleNewConnection(const HttpRequestPtr& req,
                             const WebSocketConnectionPtr& conn) override
    {
        const auto& cb = m_cbs;   // 构造时快照,消息路径零锁
        bool ok = cb.onAuth ? cb.onAuth(req) : true;   // 未提供 onAuth → 默认放行
        if (!ok)
        {
            // 无"接受前鉴权"钩子,只能升级后拒绝 → 连接关闭语义
            conn->shutdown(drogon::CloseCode::kEndpointGone, "auth failed");
            return;
        }
        if (cb.onOpen)
            cb.onOpen(conn, req);
    }

    /**
     * @brief 收到一帧消息:转发到 onMessage(未注册则丢弃)
     *
     * @param conn  来源连接
     * @param msg   消息内容(移动传入,业务回调按值/移动接管)
     * @param type  帧类型(text/binary/ping/pong)
     */
    void handleNewMessage(const WebSocketConnectionPtr& conn, string&& msg,
                          const WebSocketMessageType& type) override
    {
        if (m_cbs.onMessage)
            m_cbs.onMessage(conn, std::move(msg), type);
    }

    /**
     * @brief 连接关闭:转发到 onClose(未注册则忽略)
     * @param conn  已关闭的连接
     */
    void handleConnectionClosed(const WebSocketConnectionPtr& conn) override
    {
        if (m_cbs.onClose)
            m_cbs.onClose(conn);
    }

    /**
     * @brief 按注册名取回回调集合的副本
     *
     * 工厂实例化先于任何回调,注册表在 Phase1 之后只读;按值返回使回调路径无需持锁。
     *
     * @param name  注册名
     * @return 回调集合副本;注册名不存在时返回空集合(所有回调为空)
     */
    static ZmHttpServer::WsCallbacks GetCallbacksCopy(const string& name)
    {
        std::lock_guard lock(s_mtx);
        auto it = s_cbs.find(name);
        return it == s_cbs.end() ? ZmHttpServer::WsCallbacks{} : it->second;
    }

    /**
     * @brief 登记某注册名的回调集合
     * @param name  注册名
     * @param cb    回调集合(移动存入全局表)
     */
    static void SetCallbacks(const string& name, ZmHttpServer::WsCallbacks cb)
    {
        std::lock_guard lock(s_mtx);
        s_cbs[name] = std::move(cb);
    }

private:
    string m_regName;
    ZmHttpServer::WsCallbacks m_cbs;   ///< 注册时快照(高比消息路径零锁)
    static std::mutex s_mtx;
    static std::map<string, ZmHttpServer::WsCallbacks> s_cbs;
};

std::mutex ZmWsController::s_mtx;
std::map<string, ZmHttpServer::WsCallbacks> ZmWsController::s_cbs;

/**
 * @brief 生成进程内唯一的 WebSocket 注册名
 * @return 形如 "ZmWsCtrl0" 的注册名(原子自增,进程内不重复)
 */
string BuildWsRegName()
{
    static std::atomic<uint32_t> n{0};
    return "ZmWsCtrl" + std::to_string(n.fetch_add(1));
}

}  // namespace

// 静态成员(工作池大小 / filter 注册表 / 全局 once)
namespace
{
/// filter 注册表(按名保存业务 filter 函数;Phase1 只写、运行期只读)
std::mutex s_filterMtx;
std::map<std::string, std::function<bool(const drogon::HttpRequestPtr&,
                                         drogon::HttpResponsePtr&)>>
    s_filters;
}  // namespace

static std::atomic<size_t> s_workPoolSize{8};
std::vector<string> ZmHttpServer::s_corsOrigins;            // SetCorsAllowedOrigins 注入(启动后只读)

// 路由归属登记
//   不变式:每条已注册路由都归属于某个声明了 root 的服务器面,或显式声明为平台共享。
//   目的:消灭"游离全局路由在公网端口直通"与"跨面注册绕过该面门禁"两类错误。
//   状态为进程级、只增不清(与静态生命周期一致);注册仅在 Phase1,启动后只读快照。
namespace
{
/// root 前缀 → 归属面(段感知前缀匹配;"/" = 兜底归属,优先级最低)
using ZmRootOwnerMap = std::map<std::string, const ZmHttpServer*, std::less<>>;

std::mutex s_ownerMtx;
ZmRootOwnerMap s_rootOwners;                                   // root → 面(由 s_rootClaims 推导)
std::set<std::string, std::less<>> s_sharedPaths;              // 平台共享路径(MarkShared)
std::map<uint16_t, const ZmHttpServer*> s_portFace;            // 端口 → 面(由 s_portClaims 推导)
std::vector<std::pair<const ZmHttpServer*, std::string>> s_routeLog;   // 已登记路由(Open 复检)
/// root 声明(一对象一条;换 root 即覆盖旧声明)——冲突在 Open 期按此表重算,故修正后自愈
std::vector<std::pair<const ZmHttpServer*, std::string>> s_rootClaims;
/// 端口登记(一对象一条;重登记即覆盖)——同上
std::vector<std::pair<const ZmHttpServer*, uint16_t>> s_portClaims;

/**
 * @brief 由声明表重建 root 归属表
 *
 * 声明表是唯一事实来源,归属表只是它的投影:同 root 被多个面声明时保留最早声明者,
 * 空声明表示本面不拥有任何前缀。整体重建而非增量修补,保证两者不会各自漂移。
 */
void ZmRebuildRootOwners()
{
    s_rootOwners.clear();
    for (const auto& [face, path] : s_rootClaims)
    {
        if (!path.empty())
            s_rootOwners.emplace(path, face);
    }
}

/**
 * @brief 由声明表重建端口归属表(同端口重复登记时保留最早登记者)
 */
void ZmRebuildPortFace()
{
    s_portFace.clear();
    for (const auto& [face, port] : s_portClaims)
        s_portFace.emplace(port, face);
}

/**
 * @brief 取出(或追加)某面的声明条目,返回可写引用
 *
 * 一对象一条:已存在则就地覆盖,否则追加到表尾(重建时按表序取先到者)。
 *
 * @param claims  声明表
 * @param face    服务器面
 * @return 该面的声明值引用(默认构造)
 */
template <typename T>
T& ZmClaimSlot(std::vector<std::pair<const ZmHttpServer*, T>>& claims,
               const ZmHttpServer* face)
{
    for (auto& c : claims)
    {
        if (c.first == face)
            return c.second;
    }
    claims.emplace_back(face, T{});
    return claims.back().second;
}

#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif

/**
 * @brief 解析 IPv4 点分十进制字面量
 *
 * 不用 inet_addr/inet_pton:前者被标记弃用(C4996),后者要引入 winsock2 头序约束,
 * 而这里只需要判"是不是 IPv4 字面量 + 取 4 字节网络序值"。
 *
 * @param s   待解析串
 * @param out 出参:网络序地址(可直接赋给 sockaddr_in::sin_addr.s_addr)
 * @return true 合法 IPv4 字面量;false 非 IPv4(含 IPv6/主机名/畸形)
 */
static bool ParseIPv4Literal(const string& s, unsigned long& out)
{
    unsigned long parts[4] = {0, 0, 0, 0};
    size_t idx = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '.')
        {
            if (i == 0 || s[i - 1] == '.' || ++idx > 3)
                return false;                      // 空段 / 段数超 4
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
        parts[idx] = parts[idx] * 10 + static_cast<unsigned long>(s[i] - '0');
        if (parts[idx] > 255)
            return false;
    }
    if (s.empty() || s.back() == '.' || idx != 3)
        return false;
    // 先拼成"点分顺序"的 32 位值,再转网络序 —— 直接赋值会大小端颠倒
    // (把 127.0.0.1 绑成 1.0.0.127,绑定必失败,预探测就会误报"端口被占用")
    out = ::htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
    return true;
}

/**
 * @brief 预探测各已登记监听端口能否绑定(绑得上立刻释放)
 *
 * 为什么必须预探测:捆抛的 trantor 在 bind 失败处直接 exit(1)(trantor Socket.cc:67),
 * 而该 exit 在"另一线程正阻塞于条件变量"时可能走不完 —— 进程既不服务也不退出。
 * 探测端必须带 SO_EXCLUSIVEADDRUSE:Windows 上裸绑定的端口会被带 SO_REUSEADDR 的
 * 后来者抢绑,不加独占就测不出"已被占用"。
 * 只探测 IPv4 字面量地址;IPv6/主机名不做预探测(避免误判),交回真实绑定路径。
 *
 * @param why 出参:失败原因(形如 "0.0.0.0:80 已被占用")
 * @return true 全部可绑定;false 至少一个不可绑定(原因已回填)
 */
static bool ProbeListenerPorts(string& why)
{
    std::vector<std::pair<const ZmHttpServer*, uint16_t>> claims;
    {
        std::lock_guard<std::mutex> lk(s_ownerMtx);
        claims = s_portClaims;
    }
    for (const auto& [face, port] : claims)
    {
        if (port == 0 || !face)
            continue;
        const string ip = face->GetBindIp();
        unsigned long addr = 0;
        if (ip.empty() || !ParseIPv4Literal(ip, addr))
            continue;                              // 未登记/非 IPv4 字面量:不预探测

        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            continue;                              // 探测本身不可用时不阻断
        BOOL excl = TRUE;
        ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&excl), sizeof(excl));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = addr;
        const bool ok = ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
        ::closesocket(s);
        if (!ok)
        {
            why = ip + ":" + std::to_string(port) + " 已被占用(或本进程无权绑定)";
            return false;
        }
    }
    return true;
}

/// 启动后只读的归属快照(热路径零锁:门禁一次查表、归属网一次查表)
struct ZmOwnerSnapshot
{
    ZmRootOwnerMap roots;
    std::set<std::string, std::less<>> shared;
    std::map<uint16_t, const ZmHttpServer*> portFace;
};
std::atomic<std::shared_ptr<const ZmOwnerSnapshot>> s_ownerSnap{};

/**
 * @brief 段感知前缀匹配:path 是否落在 root 下(root 自身或其子路径)
 *
 * 以 '/' 为段边界判定,故 root="/api" 不会误命中 "/apix"。
 *
 * @param path  请求路径
 * @param root  归属前缀("/" 表示兜底认领一切)
 * @return true 落在 root 之下;false 不在其下(root 为空时恒为 false)
 */
bool ZmPathUnderRoot(std::string_view path, const std::string& root)
{
    if (root.empty())
        return false;
    if (root == "/")
        return true;   // 兜底:认领一切
    if (path.size() < root.size())
        return false;
    if (path.compare(0, root.size(), root) != 0)
        return false;
    return path.size() == root.size() || path[root.size()] == '/';
}

/**
 * @brief 表内归属查询(最长前缀优先)
 *
 * 线性扫描 root 表:命中更长的前缀即替换结果;"/" 兜底项仅在无更具体命中时胜出。
 *
 * @param roots  归属表(root → 面)
 * @param path   请求路径
 * @return 归属面指针;无任何归属时为 nullptr
 */
const ZmHttpServer* ZmLookupInTable(const ZmRootOwnerMap& roots, std::string_view path)
{
    const ZmHttpServer* best = nullptr;
    size_t bestLen = 0;
    for (const auto& kv : roots)
    {
        if (kv.first == "/")
        {
            if (!best)
                best = kv.second;   // 兜底:仅当无更具体命中
            continue;
        }
        if (ZmPathUnderRoot(path, kv.first) && kv.first.size() > bestLen)
        {
            best = kv.second;
            bestLen = kv.first.size();
        }
    }
    return best;
}
}  // namespace

// 自动 JSONP:机制全局 + 授权逐路由 + 行为差量覆盖
//   机制(转换规则)与出口(PreSending)保持全局 —— 它是通用能力;
//   授权(哪些接口可被跨站 <script> 读取)逐路由声明 —— JSONP 天然绕过 CORS;
//   授权可反向:enabled=false 把该前缀记为例外(声明 ≠ 启用),供全局观察期下
//   单独收口 —— 否则只能把全局基线翻成 false 再逐条正面声明;
//   行为(参数名/错误包装/体积上限)逐路由差量覆盖,未设置项继承全局基线。
//   存储:Phase1 写入 → Open() 固化为只读快照 → 请求路径零锁查表(同归属表范式)。
namespace
{
std::mutex s_jsonpMtx;
ZmHttpServer::ZmJsonpOptions s_jsonpDefaults;                      // 全局基线(Init 前可改)
std::vector<std::pair<std::string, ZmHttpServer::ZmJsonpOptions>> s_jsonpRoutes;  // 前缀 → 合并后
std::set<std::string, std::less<>> s_jsonpWarned;                  // 观察期:未声明却被包装的路径
/// 观察期告警路径集合上限:路径由客户端可控,必须封顶(超限只静默,不再记录新路径)
constexpr size_t kJsonpWarnedMax = 4096;

struct ZmJsonpSnapshot
{
    ZmHttpServer::ZmJsonpOptions defaults;
    // 前缀声明(按前缀长度降序 → 首个命中即最长前缀)
    std::vector<std::pair<std::string, ZmHttpServer::ZmJsonpOptions>> routes;
};
std::atomic<std::shared_ptr<const ZmJsonpSnapshot>> s_jsonpSnap{};

/**
 * @brief JSONP 授权前缀匹配(段感知,含子路径)
 *
 * 与归属前缀同款判定:prefix 自身、其子路径、或以 '/' 结尾的声明都算命中。
 *
 * @param path    请求路径
 * @param prefix  声明的前缀
 * @return true 命中该前缀;false 未命中
 */
bool ZmJsonpPrefixMatch(const std::string& path, const std::string& prefix)
{
    if (prefix.empty() || path.size() < prefix.size())
        return false;
    if (path.compare(0, prefix.size(), prefix) != 0)
        return false;
    return path.size() == prefix.size() || prefix.back() == '/' || path[prefix.size()] == '/';
}

/**
 * @brief 固化 JSONP 授权快照(Open 期一次,幂等)
 *
 * 须在各面 Phase1 的 SetJsonp* 声明之后、run 之前调用。路由表按前缀长度降序排序,
 * 请求路径取首个命中即最长前缀;同时打印授权清单,并对全局 enabled=true 的
 * 观察期兼容模式给出告警。
 */
void ZmBuildJsonpSnapshot()
{
    auto snap = std::make_shared<ZmJsonpSnapshot>();
    {
        std::lock_guard<std::mutex> lk(s_jsonpMtx);
        snap->defaults = s_jsonpDefaults;
        snap->routes = s_jsonpRoutes;
    }
    std::sort(snap->routes.begin(), snap->routes.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& e : snap->routes)
    {
        PUBLIC_LOG_INFO("JSONP 声明: \"{}\" (enabled={}, paramNames=[{}], wrapErrors={}, "
                        "maxBodyBytes={})",
                        e.first, e.second.enabled,
                        e.second.paramNames.empty() ? "" : e.second.paramNames.front(),
                        e.second.wrapErrors, e.second.maxBodyBytes);
    }
    if (snap->defaults.enabled)
        PUBLIC_LOG_WARN("JSONP: 全局 enabled=true —— **未声明路由**的 JSON 响应仍会被自动包装"
                        "(观察期兼容模式);确认无外部调用方后请置 false 并逐路由 SetJsonpEnabled");
    s_jsonpSnap.store(std::move(snap), std::memory_order_release);
}
}  // namespace

// ── 静态生命周期 ──

// ── 静态生命周期状态机 ──
//   drogon app() 为全局单例且 run() 只能跑一次 → 生命周期为进程级一次:
//     Uninit → Initialized(Init) → Opened(Open) → Closed(Close, 终态)。
//   相位契约:AddListener/RegisterRoutes/RegisterCoro 全部须在 Open 前完成;
//   Open() 先预探测监听端口(占用则干净返回 false),再起 run 线程跑事件循环。
//   运行期唯一可热更新能力:ReloadCertificates()(reloadSSLFiles)。
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
// Open() 据此确定性等待"启动成功"(替代"300ms 没报错就认为成功"的盲猜)。
// ⚠ 该标志的读写与通知都在 s_stateMtx 内:等待方在持锁期间判定谓词,
//   通知方若不持同一把锁,"check 后 / block 前"到达的通知会丢(等待方永久阻塞)。
std::atomic<bool> s_startReady{false};

std::atomic<bool> s_initClaimed{false};   // Init 单次执行门(CAS 抢占,防并发双跑)
std::atomic<bool> s_trustProxy{false};    // 限流 key 是否取 X-Forwarded-For 首跳
std::atomic<bool> s_closing{false};       // Close 已接手收尾(锁外的 quit+join 进行中)

/**
 * @brief 把服务器事件循环登记进 ZmHttpClient 的已登记 loop 表
 *
 * 主 loop + 全部 IO loop 逐个登记;此后 SendSync 在这些线程上调用一律拒绝,
 * 防"服务器 loop 被同步出站请求卡死"(drogon 自身断言只保护客户端自身 loop)。
 * 失败仅告警不抛出 —— 登记为防御性能力,不应阻断启动。
 */
void RegisterServerLoops()
{
    try
    {
        size_t registered = 0;
        if (auto* lp = app().getLoop())
        {
            ZmHttpClient::RegisterLoop(lp);
            ++registered;
        }
        const size_t n = app().getThreadNum();
        for (size_t i = 0; i < n; ++i)
        {
            if (auto* lp = app().getIOLoop(i))
            {
                ZmHttpClient::RegisterLoop(lp);
                ++registered;
            }
        }
        PUBLIC_LOG_INFO("ZmHttpServer: 服务器事件循环已登记 ZmHttpClient(count={})", registered);
    }
    catch (...)
    {
        PUBLIC_LOG_WARN("ZmHttpServer: 服务器事件循环登记 ZmHttpClient 失败(忽略)");
    }
}

/**
 * @brief 注销服务器事件循环在 ZmHttpClient 中的登记(与 RegisterServerLoops 对称)
 *
 * 在 Close() 停事件循环之后调用;异常一律忽略(此时的 loop 可能已不可用)。
 */
void UnregisterServerLoops()
{
    try
    {
        if (auto* lp = app().getLoop())
            ZmHttpClient::UnregisterLoop(lp);
        const size_t n = app().getThreadNum();
        for (size_t i = 0; i < n; ++i)
        {
            if (auto* lp = app().getIOLoop(i))
                ZmHttpClient::UnregisterLoop(lp);
        }
    }
    catch (...)
    {
    }
}
}  // namespace

/**
 * @brief 进程级一次性初始化
 *
 * 应用全局运行参数 + 证书 + 全局 advice(/ping、访问日志、安全响应头、自动 JSONP)。
 * 只允许 Uninit → Initialized 一次;之后到 Open 前可自由 AddListener/Setup/RegisterCoro
 * (Phase1)。drogon app() 为全局单例,这些参数注入后运行期不可改。
 *
 * @param opts  全局运行参数(线程数/连接护栏/体积上限/证书/工作池等)
 * @return true 初始化成功;false 状态非法(重复 Init 或已启动)
 *
 * @example
 *   ZmHttpServer::Options o;
 *   o.threadNum = 4;
 *   o.certFile = "cert.pem";
 *   o.keyFile  = "key.pem";
 *   if (!ZmHttpServer::Init(o))
 *       return false;
 */
// ── 全局 advice / 平台路由处理函数(由 Init 注册) ──
//   均为文件内静态状态的消费者,无需捕获;Init 因此只保留"装配清单"。
namespace
{
/// 非流式请求体上限(由 Init 注入;0 = 关闭预检)
std::atomic<size_t> s_nonStreamBodyLimit{0};

/// JSONP 回调名白名单校验(定义在文件后部)
bool IsValidJsonpCallback(const std::string& cb);
/**
 * @brief 事件循环真正跑起来时置启动就绪信号
 *
 * 供 Open() 确定性等待"启动成功"。端口占用**不**走这里 —— 由 Open 的端口预探测
 * 在起线程前拦下(捆抛的 trantor 在 bind 失败处直接 exit(1),不会抛异常)。
 */
void NotifyServerStarted()
{
    // 通知必须在 s_stateMtx 内:等待方(Open/Close)持该锁判定谓词,通知方不持锁
    // 则存在"check 后 / block 前"的丢通知窗口 —— 丢了就永久阻塞(无人会再通知)。
    std::lock_guard lk(s_stateMtx);
    s_startReady.store(true);
    s_stateCv.notify_all();
}

/**
 * @brief 健康检查:返回 {"pong":true}
 *
 * 路径 /ping 已由 MarkShared 声明为平台共享路径,三个面均可达。
 *
 * @param req 请求(未使用)
 * @return JSON 响应
 */
drogon::Task<HttpResponsePtr> HandlePing(HttpRequestPtr req)
{

        ZMJSON data;
        data["pong"] = true;
        co_return ZmHttpServer::JsonResponse(200, data);
}

/**
 * @brief 记录访问起始时间与请求 ID(观察式 advice,不改流向)
 *
 * 起始时间与请求 ID 都写进 req attributes,供响应出口结算时取用;
 * 请求 ID 优先透传上游 X-Request-Id,非法或缺失时现场生成。
 *
 * @param req 请求
 */
void RecordAccessStart(const HttpRequestPtr& req)
{

    req->getAttributes()->insert("ZmAccessStartMs", std::any(int64_t(NowMs())));
    s_m_inflight.fetch_add(1, std::memory_order_relaxed);   // 与 FinalizeResponse 结算配对
    // 请求 ID:上游透传优先(合法字符校验),否则生成 zm-<秒>-<序>
    string rid = req->getHeader("X-Request-Id");
    if (rid.empty() || !IsValidRequestId(rid))
        rid = MakeRequestId();
    req->getAttributes()->insert("ZmRequestId", std::any(rid));
}

/**
 * @brief 非流式请求体闸门:声明超限 → 413
 *
 * 在 header 阶段按 Content-Length 预检,防止恶意大 body 落临时文件/耗尽磁盘。
 * 带 X-File-Size 的流式上传豁免(该豁免可被伪造,属默认收口而非完备防护)。
 *
 * @param req 请求(读 Content-Length / X-File-Size)
 * @param cb  短路回调(超限时直接回 413)
 * @param cc  放行回调
 */
void GateNonStreamBody(const HttpRequestPtr& req, AdviceCallback&& cb,
                       AdviceChainCallback&& cc)
{

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
                    cb(ZmHttpServer::ErrorResponse(413, "body too large"));
                    return;
                }
            }
            catch (const std::exception&) { /* 非法长度头,交给框架解析 */ }
        }
    }
    cc();
}

/**
 * @brief 响应统一出口:归属网检查 + 安全头 + 请求 ID 回写 + 访问日志
 *
 * 挂在 PreSending:静态文件、304、重定向、被 advice 拦截的响应都会经过这里,
 * 而 PostHandling 只覆盖 handler 路径,故结算点选在此处。
 *
 * @param req  请求
 * @param resp 即将发送的响应(就地追加头部)
 */
void FinalizeResponse(const HttpRequestPtr& req, const HttpResponsePtr& resp)
{

    // ── 运行时归属网:已服务响应的归属面 ≠ 端口所属面 → 门禁漏网告警。
    //    只在非错误态判定:门禁自身的 404 是"正确的拒绝",不算泄漏;
    //    共享路径(/ping)归属兜底面属正常,由 IsSharedPath 豁免;
    //    未声明 root 的纯 advice 垫片(前端重定向实例)不参与判定 —— 它"服务"的
    //    只是 301/302,不是路由(其上也注册不了业务路由)。
    //    快照在 Open 期固化,此处零锁两次查表(归属前缀 + 端口→面)。
    if (resp->getStatusCode() < 400)
    {
        if (auto snap = s_ownerSnap.load(std::memory_order_acquire))
        {
            const std::string& rpath = req->path();
            if (!snap->shared.count(rpath))
            {
                const ZmHttpServer* owner = ZmLookupInTable(snap->roots, rpath);
                auto fit = snap->portFace.find(ZmHttpLocalPort(req));
                if (owner && fit != snap->portFace.end() && fit->second &&
                    fit->second != owner && fit->second->HasRoot())
                {
                    PUBLIC_LOG_WARN(
                        "[ROUTE-LEAK] \"{}\" 在 {} 上被服务(归属 {})——per-port 门禁漏网,"
                        "请核对该路由的 root 归属",
                        rpath, fit->second->FaceDesc(), owner->FaceDesc());
                }
            }
        }
    }
    resp->addHeader("X-Content-Type-Options", "nosniff");
    resp->addHeader("X-Frame-Options", "SAMEORIGIN");
    if (req->isOnSecureConnection())
    {
        resp->addHeader("Strict-Transport-Security",
                        "max-age=31536000; includeSubDomains");
    }
    // 请求 ID:与 ZmAccessStartMs 同款防御 —— 未经历 PreRouting 时属性缺失
    // (drogon 对缺失键返回空串),给出可辨识占位而不是空头(见 RecordAccessStart
    // 的顺序不变式);正常路径下这里取到的是实际 ID
    string rid = req->getAttributes()->get<string>("ZmRequestId");
    if (rid.empty())
        rid = "zm-unknown";
    resp->addHeader("X-Request-Id", rid);

    // ── 观察结算(访问日志 + 指标;此处为唯一结算点) ──
    int64_t start = req->getAttributes()->get<int64_t>("ZmAccessStartMs");
    int64_t cost = start > 0 ? NowMs() - start : 0;  // 防御:未经历 PreRouting(前置解析拒绝)
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
        "[{}] {} {} {} [{}] ({} - {}) {} {} {}ms",
        rid,   // 与 X-Request-Id 回写同一个值(行首与响应头可对照)
        req->isOnSecureConnection() ? "https" : "http",
        req->getMethodString(),
        query.empty() ? string(req->path()) : string(req->path()) + "?" + query,
        reqBytes, req->getPeerAddr().toIpPort(), req->getLocalAddr().toIpPort(),
        static_cast<int>(resp->getStatusCode()), respBytes, cost);

    // ── 指标累加(与 RecordAccessStart 的 inflight+1 配对;start>0 才配得上) ──
    if (start > 0)
    {
        s_m_inflight.fetch_sub(1, std::memory_order_relaxed);
        const int code = static_cast<int>(resp->getStatusCode());
        s_m_total.fetch_add(1, std::memory_order_relaxed);
        if (code >= 500)
            s_m_s5xx.fetch_add(1, std::memory_order_relaxed);
        else if (code >= 400)
            s_m_s4xx.fetch_add(1, std::memory_order_relaxed);
        else if (code >= 300)
            s_m_s3xx.fetch_add(1, std::memory_order_relaxed);
        else
            s_m_s2xx.fetch_add(1, std::memory_order_relaxed);
        if (cost <= 100)
            s_m_latLe100.fetch_add(1, std::memory_order_relaxed);
        else if (cost <= 500)
            s_m_latLe500.fetch_add(1, std::memory_order_relaxed);
        else if (cost <= 2000)
            s_m_latLe2s.fetch_add(1, std::memory_order_relaxed);
        else
            s_m_latGt2s.fetch_add(1, std::memory_order_relaxed);
    }
}

/**
 * @brief 自动 JSONP:GET + 合法回调名 + JSON 响应 + 该路由已授权 → 包装
 *
 * 机制全局、授权逐路由(见 SetJsonpEnabled):声明的前缀可被 enabled=false 单独
 * 排除(例外不走"未声明"分支,故也不打观察期告警),未声明的前缀按全局基线定姿态。
 * 显式 JsonpResponse 的产物已是 JS,不会被二次包装;本函数在访问日志之后执行,
 * 日志记的是包装前的字节数。
 *
 * @param req  请求(读回调参数、方法)
 * @param resp 即将发送的响应(命中时改写 body 与 Content-Type)
 */
void ApplyAutoJsonp(const HttpRequestPtr& req, const HttpResponsePtr& resp)
{

    if (req->method() != Get)
        return;
    if (resp->contentType() != CT_APPLICATION_JSON)
        return;

    auto snap = s_jsonpSnap.load(std::memory_order_acquire);
    if (!snap)
        return;   // 未固化(未 Open,正常路径不会走到)
    const string path(req->path());
    const ZmHttpServer::ZmJsonpOptions* opt = nullptr;
    for (const auto& e : snap->routes)   // 已按前缀长度降序 → 首个命中即最长前缀
    {
        if (ZmJsonpPrefixMatch(path, e.first))
        {
            opt = &e.second;
            break;
        }
    }
    bool undeclared = false;
    if (!opt)
    {
        if (!snap->defaults.enabled)
            return;                      // 目标形态:未声明路由不包装
        opt = &snap->defaults;           // 观察期:兼容现状 + 告警(每路径一次)
        undeclared = true;
    }
    if (!opt->enabled)
        return;                          // 该前缀被显式排除(声明 ≠ 启用)
    if (!opt->wrapErrors && static_cast<int>(resp->getStatusCode()) >= 400)
        return;
    if (resp->getBody().size() > opt->maxBodyBytes)
        return;                          // 超限不包(避免整包拼接多一份拷贝)
    string cb;
    for (const auto& name : opt->paramNames)
    {
        cb = req->getParameter(name);
        if (!cb.empty())
            break;
    }
    if (cb.empty() || !IsValidJsonpCallback(cb))
        return;                          // 无合法回调名 = 不是 JSONP 请求:不包装,也不记录告警
    if (undeclared)
    {
        // 告警只在"确实包装了未声明路由"时记录,且有界(路径是客户端可控输入)
        std::lock_guard<std::mutex> lk(s_jsonpMtx);
        if (s_jsonpWarned.size() < kJsonpWarnedMax && s_jsonpWarned.insert(path).second)
        {
            PUBLIC_LOG_WARN("[JSONP] 未声明路由 \"{}\" 的 JSON 响应被自动包装(观察期);"
                            "确认无外部调用方后请 SetJsonpDefaults 置 enabled=false,"
                            "并对需要 JSONP 的路由 SetJsonpEnabled", path);
        }
    }
    string body = cb + "(" + std::string(resp->getBody()) + ");";
    resp->setBody(body);
    resp->setContentTypeCode(CT_TEXT_JAVASCRIPT);
}

}  // namespace
bool ZmHttpServer::Init(const Options& opts)
{
    // 单次执行门:CAS 抢占。只查状态存在 TOCTOU —— 两个线程可同时通过检查后各跑
    // 一遍(重复注册 advice/handler:轻则行为漂移,重则触发 drogon 断言)。
    // 注:本门只表达"有线程正在/曾经跑 Init",不表达"已初始化"(那是 s_state 的职责)——
    //     **尚未产生任何副作用**的失败分支必须归还本门,否则调用方修正后重试会被永久挡死;
    //     反之,一旦已改动 app() 全局状态就不得归还(再跑一遍会二次注册 advice/handler)。
    bool claimed = false;
    if (!s_initClaimed.compare_exchange_strong(claimed, true))
    {
        PUBLIC_LOG_ERROR("ZmHttpServer::Init 只可调用一次(已有并发 Init 或此前已 Init 过)");
        return false;
    }
    ZmRuntimeState cur = s_state.load();
    if (cur != ZmRuntimeState::Uninit)
    {
        PUBLIC_LOG_ERROR("ZmHttpServer::Init 只可调用一次(当前状态已非 Uninit)");
        // 归还本门:本次未做任何事,重复 Init 由状态判定负责拒绝(无副作用,安全可重试)
        s_initClaimed.store(false);
        return false;
    }

    // ── 全局运行参数(均为 drogon app 全局量,运行期不可改) ──
    app().setThreadNum(opts.threadNum);            // 0 = 自动 = CPU 核数
    app().setMaxConnectionNum(opts.maxConnections);
    app().setMaxConnectionNumPerIP(opts.maxConnectionsPerIP);  // per-IP 护栏(0=不限;单机压测慎设)
    app().setClientMaxBodySize(opts.clientMaxBodySize);
    app().setClientMaxMemoryBodySize(opts.clientMaxMemoryBodySize);  // 内存闸:超限落临时文件
    s_nonStreamBodyLimit.store(opts.nonStreamBodyLimit, std::memory_order_relaxed);
    app().setIdleConnectionTimeout(opts.idleTimeoutSec);
    app().setKeepaliveRequestsNumber(opts.keepaliveRequests);   // 0 = 不限次数回收
    app().enableRequestStream(opts.enableRequestStream);
    app().enableGzip(opts.gzip);
    app().enableBrotli(opts.brotli);
    app().setGzipStatic(opts.gzipStatic);
    // 捆绑 drogon 1.9.13 已含 setBrStatic,且 lib 已链接 brotli(USE_BROTLI)
    app().setBrStatic(opts.brotliStatic);
    s_workPoolSize.store(opts.workPoolSize);       // 首次 RunOnPool 前定型
    s_trustProxy.store(opts.trustProxyHeaders);    // 限流 key 来源(见 ZmIpRateLimiter::Check)

    // ── TLS(证书全局,保证热加载) ──
    s_hasCert.store(!opts.certFile.empty(), std::memory_order_relaxed);
    if (!opts.certFile.empty())
    {
        app().setSSLFiles(opts.certFile, opts.keyFile);
        if (opts.ticketDisabled)
            app().setSSLConfigCommands({ { "Options", "-SessionTicket" } });
    }

    // ── 启动成功信号:事件循环真正跑起来时置值,供 Open 确定性等待 ──
    // 注:端口占用由 Open 的预探测在起线程前拦下(捆抛的 trantor 在 bind 失败处
    //     直接 exit(1),不抛异常);其余启动/运行异常由 run 线程的异常路径兜底。
    app().registerBeginningAdvice(&NotifyServerStarted);

    // ── 全局 advice + /ping(经 Options 一次性注册,不再依赖"首个 Open"去重) ──
    // /ping 为平台共享路径(三面均可达, 旧语义):显式声明后各面门禁自动放行,
    // 归属网亦豁免(不再由各面门禁各自硬编码 "/ping")。
    MarkShared("/ping");
    // 注:本捆绑 drogon 的 FunctionTraits 协程特化要求 handler 首参为按值 HttpRequestPtr
    app().registerHandler("/ping", &HandlePing, { HttpMethod::Get });

    // 请求访问记录(:不用 AccessLogger/access.log,
    // 格式化后经公共库日志 PUBLIC_LOG_* 承载,与运行日志同文件按标签区分)。
    // 起始时间存 req attributes,PostHandling 观察点结算。
    // ⚠ 顺序不变式:本 advice 必须是**首个** PreRouting advice。请求 ID 在此写入,
    //   而后面的 advice 一旦短路(如 body 闸门 413),该响应仍会经 PreSending 出口
    //   结算 —— 若本 advice 被排到短路者之后,那些响应取不到 "ZmRequestId"
    //   (drogon 对缺失键返回空串),表现为空 X-Request-Id 头与日志行首 "[]"。
    app().registerPreRoutingAdvice(&RecordAccessStart);
    // ⚠ 观察结算不注册在此:drogon 1.9.13 的 PostHandling advice 只覆盖
    //    controller/binder 响应路径(HttpServer.cc:658/692/764),静态目录(含 304)、
    //    Range 响应、重定向等经 sendResponses 直达发送链,不经本 advice ——
    //    结算统一在 PreSending 出口(handleResponse:819 覆盖一切响应,见下)。

    // 非流式请求体闸门(header 阶段预检):1.9.13 无 per-route 上限 API,
    // Content-Length 声明超限 → 提前 413,防恶意大 body 落临时文件/磁盘耗尽。
    // 豁免:带 X-File-Size 者视为流式大上传(RegisterStreamCoro 业务,其 maxBytes 兜底);
    //   ⚠ 该豁免可被伪造,防御定位为"默认配置收口",非完备防护(全局 clientMaxBodySize 兜底)。
    app().registerPreRoutingAdvice(&GateNonStreamBody);

    // 安全响应头 + 观察结算(统一出口 = PreSending[handleResponse:819 覆盖
    // 一切响应:静态/304/Range/重定向/拦截等];PostHandling 仅覆盖 binder 路径,不作为结算点)。
    //   - X-Content-Type-Options / X-Frame-Options / HTTPS 面 HSTS(含 301 与全分支)
    //   - X-Request-Id 回写(与日志行首一致;含 304/重定向)
    //   - 访问日志 + 指标(总/状态段/inflight/延迟桶)
    //   - 注:连接已断(handleResponse 提前返回)时不结算,inflight 属测量性指标非精确。
    app().registerPreSendingAdvice(&FinalizeResponse);

    // 自动 JSONP:GET + 合法 callback + JSON 响应 → 包装。
    // 出口保持全局 advice(机制通用),**授权与行为改为逐路由**:
    //   · 命中 SetJsonpEnabled 声明的前缀 → 用该路由合并后的选项(最长前缀优先);
    //     该前缀 enabled=false(例外)则直接放行,且不打"未声明"的观察期告警;
    //   · 未命中 → 全局基线:enabled=true 时仍包装(观察期兼容)并**按路径告警一次**,
    //     enabled=false 时不包装(目标形态:只包装显式声明路由)。
    // 注①:仅包装纯 JSON(CT_APPLICATION_JSON)——显式 JsonpResponse 的产物已是
    //      CT_TEXT_JAVASCRIPT,纳入条件会二次包装。
    // 注②:在 PostHandling 之后执行,访问日志记录的字节数为包装前的 JSON 大小。
    app().registerPreSendingAdvice(&ApplyAutoJsonp);

    s_state.store(ZmRuntimeState::Initialized);
    PUBLIC_LOG_INFO("ZmHttpServer::Init 完成(全局参数/证书/advice 已应用)");
    return true;
}

/**
 * @brief 启动服务器(Phase2):后台线程跑 app.run
 *
 * 前置条件:已 Init 且已登记至少一个监听。启动成功以"事件循环就绪信号"
 * (registerBeginningAdvice)为准;端口占用经预探测 fail-fast,其余启动异常经 run 线程
 * 异常 fail-fast。并发纪律:状态迁移全程持 s_stateMtx —— Close 在 Open 的启动窗口/
 * 等待期插入时能看到一致状态并正常收管(run 线程由 Close join)。
 *
 * @return true 启动成功(app().run 运行中);false 前置不满足、归属校验失败、监听端口
 *         被占用(预探测)、run 线程启动失败、run 抛异常或等待就绪超时
 *         —— 除成功外状态均回退 Initialized,修正后可重试
 */
namespace
{
/**
 * @brief app().run() 的承载线程入口
 *
 * app().run() 抛出的异常在此捕获并置位错误信号,供 Open() 的等待逻辑 fail-fast;
 * 它同时是"运行期异常"对外可见的唯一信号 ——
 * s_state 不回退,由 IsOpened() 读它,避免"服务器已死但对外仍报运行中"。
 * 注:端口占用不走这里(trantor 在 bind 失败处直接 exit(1),trantor Socket.cc:67),
 * 该场景由 Open 的端口预探测拦下。
 * 置位与通知都在 s_stateMtx 内(与等待方共用一把锁,避免丢通知);
 * Close 的 join 已移出该锁(见 Close),故此处取锁不会与之互等。
 */
void RunServerLoop(std::stop_token)
{
    try
    {
        app().run();
    }
    catch (const std::exception& e)
    {
        s_runErrorMsg = e.what();
        std::lock_guard lk(s_stateMtx);
        s_runError.store(true, std::memory_order_release);
        PUBLIC_LOG_ERROR("ZmHttpServer app().run() 线程异常退出: {}", s_runErrorMsg);
        s_stateCv.notify_all();
    }
    catch (...)
    {
        s_runErrorMsg = "unknown";
        std::lock_guard lk(s_stateMtx);
        s_runError.store(true, std::memory_order_release);
        PUBLIC_LOG_ERROR("ZmHttpServer app().run() 线程异常退出(未知异常)");
        s_stateCv.notify_all();
    }
}
}  // namespace

bool ZmHttpServer::Open()
{
    std::unique_lock<std::mutex> lock(s_stateMtx);
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

    // 路由归属一致性校验(root 冲突 / 路由归属复检)+ 只读快照固化。
    // 校验不过 → 拒绝启动(不迁移状态,仍为 Initialized:修正配置后可重试 Open)。
    if (!ValidateRouteOwnership())
        return false;

    // 端口可用性预探测:必须在这里干净地失败 —— 捆抛的 trantor 在 bind 失败处直接
    // exit(1)(Socket.cc:67),且该 exit 在有线程阻塞于条件变量时走不完 → 进程卡死,
    // Open 既不返回也不报错。预探测通过后真实绑定仍可能被别进程抢走(TOCTOU),
    // 那由下面的有界等待兜底。
    {
        string why;
        if (!ProbeListenerPorts(why))
        {
            PUBLIC_LOG_ERROR("ZmHttpServer::Open: 监听端口不可用 —— {};修正后可直接重试 Open",
                             why);
            return false;   // 状态仍为 Initialized:允许修正后重试
        }
    }

    s_runError.store(false);
    s_state.store(ZmRuntimeState::Opened);

    // 启动信号复位:本函数全程持 s_stateMtx,故与通知方/等待方互斥(见 s_startReady 注)
    s_startReady.store(false);

    // 后台线程跑 app().run()(ZmThread 模型;Stop 靠 request_stop + join,
    // 实际退出由 app().quit() 驱动,run() 不查 stop_token)。
    // 注:run 线程异常路径在 s_stateMtx 内置 s_runError 并通知;Close 的 join 已移出
    //     该锁,故两者不会互等(见 Close 与 RunServerLoop)。
    s_runThread = std::make_unique<ZmThread>("DrogonHttpRun", &RunServerLoop);
    if (!s_runThread->Start())
    {
        PUBLIC_LOG_ERROR("ZmHttpServer::Open: DrogonHttpRun 线程启动失败");
        s_runThread.reset();
        s_state.store(ZmRuntimeState::Initialized);
        return false;
    }

    // 等待"启动结果":三个信号之一到达并 notify ——
    //   BeginningAdvice 触发(事件循环已跑)→ 成功;run 抛异常 → s_runError;
    //   并发 Close 收管 → s_state == Closed。
    // 有界兜底:进程内已无更强保证 —— trantor 在 bind 失败处 exit(1) 而 exit 可能
    // 走不完(不触发上面任何信号),此时调用方会永久阻塞。宁可超时报失败。
    constexpr int64_t kStartWaitMs = 15000;
    const auto startDeadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kStartWaitMs);
    auto startReady = [&] {
        return s_runError.load(std::memory_order_acquire) || s_startReady.load() ||
               s_state.load() == ZmRuntimeState::Closed;
    };
    while (!startReady())
    {
        if (s_stateCv.wait_until(lock, startDeadline) == std::cv_status::timeout && !startReady())
        {
            PUBLIC_LOG_ERROR("ZmHttpServer::Open: 等待事件循环就绪超时({}ms) —— run 线程"
                             "可能卡在第三方 exit 路径;按启动失败返回(状态回退 Initialized,"
                             "run 线程未 join,进程应尽快重启)",
                             kStartWaitMs);
            s_state.store(ZmRuntimeState::Initialized);
            return false;
        }
    }

    // 并发 Close 已收管(线程已 join、s_runThread 已复位):本次 Open 视为失败
    if (s_state.load() == ZmRuntimeState::Closed)
        return false;

    if (s_runError.load(std::memory_order_acquire))
    {
        PUBLIC_LOG_ERROR("app().run() 启动异常: {}", s_runErrorMsg);
        // 让 run 线程收尾退出,再回退状态允许重试
        // 此处持 s_stateMtx join 是安全的:s_runError 由 run 线程在该锁内置位,
        // 本函数能看到它为 true 即说明对方已离开临界区(且此后不会再取该锁)。
        app().quit();
        s_runThread->Stop();
        s_runThread.reset();
        s_state.store(ZmRuntimeState::Initialized);
        return false;
    }
    PUBLIC_LOG_INFO("ZmHttpServer::Open 完成(app().run 运行中)");
    RegisterServerLoops();  // :服务器 loop 登记(Open 成功后一次)
    return true;
}

/**
 * @brief 全局唯一关闭(Phase3):quit + join run 线程;幂等
 *
 * 状态分支:未启动/已关闭 → 直接返回;Initialized 未 Open → 仅回退状态;
 * Opened → 先 app().quit() 停事件循环,再 Stop(join) —— 顺序不可颠倒。
 * ⚠ 在飞 HTTP 语义:quit 后挂起的协程不再调度,业务层自保障(守护线程 join/断点),
 *    本函数不等待在飞业务。
 * 并发纪律(两条,缺一会挂):
 *   ① 启动窗口内 run 线程尚未进 loop,quit() 此时是 no-op,故先等"循环已起/run 抛
 *      异常"再 quit,否则 run() 永不返回,Stop() join 永久挂死;
 *   ② join 必须在 s_stateMtx 之外做:run 线程的异常收尾要取该锁才能通知(见
 *      RunServerLoop),持锁 join 会与之互等死锁。终态先落在锁内,故并发 Open 仍按
 *      "已收管"收场;s_closing 门保证并发 Close 等到本次收尾真正做完再返回。
 * ⚠ 严禁在 run 线程(服务器事件循环)内调用:那会 join 自身线程,标准库直接抛
 *    resource_deadlock_would_occur,且收尾无法完成。
 * Closed 为终态,之后不能再 Open/Init/AddListener。
 */
void ZmHttpServer::Close()
{
    std::unique_lock<std::mutex> lock(s_stateMtx);   // wait 需要可解锁的锁
    // 并发 Close:已有一次收尾在锁外 join,等它做完再返回(保证"返回即已停")
    s_stateCv.wait(lock, [] { return !s_closing.load(); });

    ZmRuntimeState cur = s_state.load();
    if (cur == ZmRuntimeState::Uninit || cur == ZmRuntimeState::Closed)
    {
        return;   // 幂等:未启动 / 已关闭
    }
    if (cur == ZmRuntimeState::Initialized)
    {
        // 未 Open 就直接 Close:仅回退状态
        s_state.store(ZmRuntimeState::Closed);
        s_stateCv.notify_all();
        return;
    }
    // Opened:先 quit 让 run() 返回,再 Stop(join) — 顺序必须保持
    std::unique_ptr<ZmThread> runThread;
    if (s_runThread)
    {
        // 启动窗口:run 线程还没进 loop,quit() 是 no-op(drogon 以
        // getLoop()->isRunning() 为前提,HttpAppFrameworkImpl.cc:1036)→ run() 永不返回
        // → Stop() join 永久阻塞。先等两个信号之一:事件循环已起(BeginningAdvice)
        // 或 run 已抛异常(此时线程已在收尾,join 立刻返回)。
        s_stateCv.wait(lock, [&] {
            return s_startReady.load() || s_runError.load(std::memory_order_acquire);
        });
        runThread = std::move(s_runThread);   // 所有权移出静态槽:join 放到锁外
    }
    s_state.store(ZmRuntimeState::Closed);   // 终态先落:并发 Open/Close 由此收场
    s_closing.store(true);
    app().quit();                            // 只入队不阻塞,保持"quit 先于 join"的顺序
    lock.unlock();

    if (runThread)
    {
        runThread->Stop();      // join(锁外;理由见 doc ②)
        runThread.reset();
    }

    lock.lock();
    s_closing.store(false);
    UnregisterServerLoops();  // :与 RegisterServerLoops 对称注销
    s_stateCv.notify_all();   // 唤醒并发 Open/Close 的等待
    PUBLIC_LOG_INFO("ZmHttpServer::Close 完成(已 quit+Stop,终态)");
}

/**
 * @brief 查询是否已完成 Init(可登记监听/路由,无论是否已启动)
 * @return true 状态为 Initialized/Opened;false 仍为 Uninit 或已 Closed
 *         (枚举序 Closed 在 Initialized 之后,"≥ Initialized" 会把它误判为可用)。
 *         注:不看 run 线程死活 —— Open 失败会回退到 Initialized 并允许"修正后重试",
 *         此时 Init 阶段确实已完成,与"当前是否有事件循环"是两件事(后者看 IsOpened)。
 */
bool ZmHttpServer::IsInitialized()
{
    ZmRuntimeState cur = s_state.load();
    return cur == ZmRuntimeState::Initialized || cur == ZmRuntimeState::Opened;
}

/**
 * @brief 查询事件循环是否正在运行
 * @return true 状态为 Opened 且 run 线程未抛异常(运行期抛异常 = 事件循环已死,
 *         s_state 不回退,故这里必须读 s_runError);false 其它状态
 */
bool ZmHttpServer::IsOpened()
{
    return s_state.load() == ZmRuntimeState::Opened &&
           !s_runError.load(std::memory_order_acquire);
}

/**
 * @brief 读取运行时指标快照
 *
 * 口径:total/状态段/延迟桶在响应出口(FinalizeResponse)累加,只统计经过 PreRouting
 * 的请求 —— 取不到起始时间戳的响应不结算(与访问日志同一判据);inflight 为
 * PreRouting 起算数减已结算数,属测量性指标,连接已断的响应不结算故非精确值。
 *
 * @return 各字段的原子读快照
 */
ZmHttpServer::ZmMetrics ZmHttpServer::GetMetricsSnapshot()
{
    ZmMetrics m;
    m.total = s_m_total.load(std::memory_order_relaxed);
    m.s2xx = s_m_s2xx.load(std::memory_order_relaxed);
    m.s3xx = s_m_s3xx.load(std::memory_order_relaxed);
    m.s4xx = s_m_s4xx.load(std::memory_order_relaxed);
    m.s5xx = s_m_s5xx.load(std::memory_order_relaxed);
    m.inflight = s_m_inflight.load(std::memory_order_relaxed);
    m.latencyLe100ms = s_m_latLe100.load(std::memory_order_relaxed);
    m.latencyLe500ms = s_m_latLe500.load(std::memory_order_relaxed);
    m.latencyLe2s = s_m_latLe2s.load(std::memory_order_relaxed);
    m.latencyGt2s = s_m_latGt2s.load(std::memory_order_relaxed);
    return m;
}

/**
 * @brief 查询本面监听是否启用 TLS
 *
 * 一对象一端口:监听登记时 useSSL 即决定本面模式。证书为进程级全局,
 * 故各面的判定结果一致。
 *
 * @return true 本面为 HTTPS;false 未登记监听或为纯 HTTP
 */
bool ZmHttpServer::IsHttps() const
{
    // 一对象一端口:本面监听启用 useSSL 即 HTTPS 模式
    return m_listenerSet && m_listener.useSSL;
}

// ── 监听与证书 ──

// ── 监听配置(证书全局经 Init 的 Options,per-listener 证书为空保证热加载) ──
/**
 * @brief 登记本面的监听端口(一对象一端口)
 *
 * 须在 Init 之后、Open 之前调用(全局参数先行注入);证书一律取 Init 传入的全局证书,
 * 此处不传 per-listener 证书,以保证 ReloadCertificates 热加载对所有面生效。
 * 同一端口被两个面登记会记入归属冲突,Open() 将拒绝启动。
 *
 * @param port         监听端口(0 非法,直接忽略)
 * @param useSSL       是否启用 TLS
 * @param ip           绑定地址("0.0.0.0"/"::" = 通配)
 * @param useOldTLS    是否允许旧版 TLS 协议
 * @param sslConfCmds  额外的 OpenSSL 配置指令(如禁用 SessionTicket)
 *
 * @example
 *   api.AddListener(8443, true);      // HTTPS 面
 *   front.AddListener(80, false);     // HTTP 面(用于 301 跳转)
 */
void ZmHttpServer::AddListener(uint16_t port, bool useSSL, const string& ip,
                                   bool useOldTLS,
                                   const vector<pair<string, string>>& sslConfCmds)
{
    // 一对象一端口:仅允许设置一次;重复设置报错并忽略(多端口面用多个实例)
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
    // 相位契约硬约束:监听须在 Init 之后(全局参数先行注入)登记
    if (!IsInitialized())
    {
        PUBLIC_LOG_ERROR("AddListener 须在 ZmHttpServer::Init 之后调用(当前未 Init),已忽略: port={}",
                         port);
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

    // 登记端口 → 面(运行时归属网判定"端口所属面";Open 期固化为只读快照)。
    // 同一端口被两个面登记 = 配置错误,Open() 期报冲突拒绝启动;
    // 一对象一端口:重登记即覆盖旧端口(归属表随之重建,修正配置后自愈)。
    {
        std::lock_guard<std::mutex> lk(s_ownerMtx);
        ZmClaimSlot(s_portClaims, this) = port;
        ZmRebuildPortFace();
        auto it = s_portFace.find(port);
        if (it != s_portFace.end() && it->second != this)
            PUBLIC_LOG_ERROR("AddListener: 端口 {} 被 {} 与 {} 同时登记"
                             " —— 端口冲突,Open() 将拒绝启动(保留先登记者)",
                             port, it->second->FaceDesc(), FaceDesc());
    }
}

/**
 * @brief 证书热重载(运行期唯一可热更新能力)
 *
 * 换内容不换路径:先替换磁盘上的证书文件,再调用本函数让 drogon 重新读取。
 *
 * @return true 已触发重载;false 未配置证书(纯 HTTP 面),无操作
 */
bool ZmHttpServer::ReloadCertificates()
{
    if (!s_hasCert.load(std::memory_order_relaxed))
        return false;
    app().reloadSSLFiles();   // 热加载:换内容不换路径
    return true;
}

// ── 路由注册 ──

// ── 结构路由注册(幂等;manager 在 Open 前调用) ──
/**
 * @brief 结构路由注册入口(幂等;manager 在 Open 前调用一次)
 *
 * 以 m_setupDone 做一次性门,重复调用直接返回;派生面在 RegisterRoutes()
 * 中注册自己的内置路由与 advice。
 */
void ZmHttpServer::Setup()
{
    if (m_setupDone)
        return;
    m_setupDone = true;
    RegisterRoutes();   // 派生面自行实现:注册路由与 advice
}

/**
 * @brief 注册协程路由(业务层最常用入口)
 *
 * handler 形参为按值 HttpRequestPtr:本捆绑 drogon 的 FunctionTraits 协程特化
 * 可直接匹配该形态,故直接注册 std::function,不套中间协程。路径含 {N} 占位符时
 * 改经 registerHandlerViaRegex 注册。
 *
 * @param path     路由路径,占位符写作 {id}(如 "/api/user/{id}")
 * @param m        HTTP 方法
 * @param h        业务协程 handler(返回空响应时回 500)
 * @param filters  filter 名列表(须已经 AddFilter 注册;未注册仅记错误日志)
 *
 * @example
 *   api.RegisterCoro("/api/user/{id}", Get,
 *       [](HttpRequestPtr req) -> drogon::Task<HttpResponsePtr> {
 *           ZMJSON d;
 *           d["id"] = req->getParameter("id");
 *           co_return ZmHttpServer::JsonResponse(200, d);
 *       });
 */
void ZmHttpServer::RegisterCoro(const string& path, drogon::HttpMethod m,
                                    ZmHttpCoroHandler h, const vector<string>& filters)
{
    // 归属校验 —— 跨面/游离注册会绕过该面的 per-port 门禁,一律拒绝
    if (!CheckRouteOwnership(path, "RegisterCoro"))
        return;
    vector<internal::HttpConstraint> cons;
    cons.emplace_back(m);
    for (const auto& fn : filters)
    {
        if (!CheckFilterRegistered(fn))
            PUBLIC_LOG_ERROR("RegisterCoro[{}]: filter 未注册: {}", path, fn);
        cons.emplace_back(fn);
    }
    // 适配:本捆绑 drogon 的 FunctionTraits 协程特化要求 HttpRequestPtr 按值;
    // ZmHttpCoroHandler 形参正是按值,可直接匹配。
    // 直接注册 std::function,不套中间协程(套层会在 Windows 上崩溃)。
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

/**
 * @brief 把 {N} 占位符路由模式转换为等价正则
 *
 * 逐个转义正则元字符,并把 {name} 整体替换为 ([^/]+)(段内匹配,不跨 '/')。
 * 用途:本捆绑 drogon 的 registerHandler 占位符转换路径在 Windows 会 exit(1),
 * 故含 {N} 的路由改经本函数转换后走 registerHandlerViaRegex。
 *
 * @param path  路由模式(如 "/api/user/{id}")
 * @return 等价正则串(如 "/api/user/([^/]+)")
 */
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

/**
 * @brief 校验路径占位符编号与形参数是否匹配
 *
 * 逐个取出 {…} 体：纯数字才当编号（与 drogon 的 \\{([^/]*)\\} 判定一致），
 * 编号须落在 1..arity 且互不重复 —— 越界与重复都会让 drogon 的 addHttpPath 直接
 * exit(1) 杀进程（HttpControllersRouter.cc:368-383），故在此提前拦下。
 *
 * @param path   路由模式（如 "/api/user/{1}"）
 * @param arity  handler 声明的路径参数个数（协程特化的 arity 不含 HttpRequestPtr）
 * @param what   调用方名称（错误日志前缀）
 * @return true 编号全部合法；false 非法（已打 ERROR，调用方须拒绝注册）
 */
bool ZmHttpServer::CheckPathParamPlaceholders(const string& path, size_t arity,
                                              const char* what)
{
    std::vector<size_t> places;   // 已出现的编号（形参数很少，线性查重足够）
    for (size_t i = 0; i < path.size();)
    {
        size_t open = path.find('{', i);
        if (open == string::npos)
            break;
        size_t close = path.find('}', open);
        if (close == string::npos)
            break;
        string body = path.substr(open + 1, close - open - 1);
        i = close + 1;
        // 非纯数字占位符（空体/命名形态）不属编号，交由 drogon 的其它分支处理
        if (body.empty() ||
            !std::all_of(body.begin(), body.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c)) != 0;
            }))
            continue;
        size_t place = 0;
        try
        {
            place = static_cast<size_t>(std::stoul(body));
        }
        catch (const std::exception&)
        {
            PUBLIC_LOG_ERROR("{}[{}]: 占位符编号 {} 超出可解析范围,拒绝注册", what, path, body);
            return false;
        }
        if (place == 0 || place > arity)
        {
            PUBLIC_LOG_ERROR("{}[{}]: 占位符编号 {} 越界(形参 {} 个,合法编号 1..{})"
                             "—— drogon 对该情形直接退出进程,故拒绝注册",
                             what, path, place, arity, arity);
            return false;
        }
        if (std::find(places.begin(), places.end(), place) != places.end())
        {
            PUBLIC_LOG_ERROR("{}[{}]: 占位符编号 {} 重复"
                             "—— drogon 对该情形直接退出进程,故拒绝注册",
                             what, path, place);
            return false;
        }
        places.push_back(place);
    }
    return true;
}

/**
 * @brief 注册带业务级 deadline 的协程路由
 *
 * 到期由事件循环定时器回 504,原子门保证只回一次;业务晚到结果经
 * "弱引用 + connected()"安全丢弃。定时器注册在连接所属 loop。
 * ⚠ 流式/下载端点不要用本接口:超时会在流中途插入 504。
 *
 * @param path        路由路径(含 {N} 占位符时自动改走 regex 注册)
 * @param m           HTTP 方法
 * @param h           业务协程 handler
 * @param deadlineMs  业务超时(毫秒);必须 > 0,无超时需求请改用 RegisterCoro
 * @param filters     filter 名列表(须已经 AddFilter 注册;未注册仅记错误日志)
 *
 * @example
 *   api.RegisterCoroWithDeadline("/api/heavy", Post, HandleHeavy, 30000);
 */
namespace
{
/// 单次请求的超时状态(定时器与协程共享;由"回答权"原子门保证只回一次)
struct ZmDeadlineState
{
    std::atomic<bool> answered{false};                // 原子门:只回一次
    std::function<void(const HttpResponsePtr&)> cb;   // 响应回调(完成即置空)
    std::weak_ptr<trantor::TcpConnection> connWk;     // 连接守卫
};

/**
 * @brief 超时到点:抢到"回答权"者回 504
 *
 * 抢不到说明业务已应答,或连接已关(此时不发);应答后立即释放响应回调。
 */
struct ZmDeadlineFire
{
    std::shared_ptr<ZmDeadlineState> st;

    void operator()() const
    {
        if (st->answered.exchange(true))
            return;                                       // 业务已回
        auto c = st->connWk.lock();
        if (!c || !c->connected())
            return;                                       // 连接已关,不发
        ZMJSON data;
        data["error"]["code"] = 504;
        data["error"]["message"] = "deadline exceeded";
        st->cb(ZmHttpServer::JsonResponse(504, data));
        st->cb = nullptr;                                 // 立即释放响应回调
    }
};

/**
 * @brief 业务完成后的统一收尾:占门 → 注销定时器 → 连接在则回响应 → 清空回调
 *
 * 成功与异常路径共用;`cb` 用完即置空,避免定时器把响应回调滞留到 deadlineMs。
 */
struct ZmDeadlineFinish
{
    std::shared_ptr<ZmDeadlineState> st;
    trantor::EventLoop*              loop = nullptr;
    trantor::TimerId                 tid  = 0;

    void operator()(HttpResponsePtr resp) const
    {
        if (st->answered.exchange(true))
            return;                                       // 已被超时占位
        loop->invalidateTimer(tid);                       // 幂等;内部 runInLoop → 跨线程安全
        auto c = st->connWk.lock();
        if (c && c->connected())
            st->cb(resp);
        else
            PUBLIC_LOG_DEBUG("RegisterCoroWithDeadline: 连接已关闭,响应丢弃");
        st->cb = nullptr;                                 // 不再滞留到 deadlineMs
    }
};
}  // namespace

void ZmHttpServer::RegisterCoroWithDeadline(const string& path, drogon::HttpMethod m,
                                                ZmHttpCoroHandler h, size_t deadlineMs,
                                                const vector<string>& filters)
{
    // 归属校验(同 RegisterCoro)
    if (!CheckRouteOwnership(path, "RegisterCoroWithDeadline"))
        return;
    // 守卫:deadlineMs=0 → runAfter(0) 会让每个请求立即 504(静默失效),
    // 属易踩的配置错误,故拒绝注册而非静默接受
    if (deadlineMs == 0)
    {
        PUBLIC_LOG_ERROR("RegisterCoroWithDeadline[{}]: deadlineMs=0 会立即 504;"
                         "无超时请改用 RegisterCoro —— 本次注册已忽略", path);
        return;
    }
    vector<internal::HttpConstraint> cons;
    cons.emplace_back(m);
    for (const auto& fn : filters)
    {
        if (!CheckFilterRegistered(fn))
            PUBLIC_LOG_ERROR("RegisterCoroWithDeadline[{}]: filter 未注册: {}", path, fn);
        cons.emplace_back(fn);
    }

    // callback-form 注册:框架回调仅能发一次;超时定时器放连接所属 loop;
    // 业务晚到结果经"原子门 + 弱引用 + connected"安全丢弃
    // 注:本捆绑 drogon 的协程函数指针特化要求首参按值 HttpRequestPtr、回调整参按值传递。
    // 注2:本捆绑 drogon 对协程 handler 的 paramCount() 恒为 0
    //   (FunctionTraits 只统计 req+callback 之外的额外参数),含 {N} 占位符路径若直接经
    //   app().registerHandler 注册,会命中 HttpControllersRouter::addHttpPath 的占位符校验
    //   (place > paramCount)并 exit(1) 杀进程(见 HttpControllersRouter::addHttpPath)。
    //   故与 RegisterCoro/RegisterStreamCoro 一致,{N} 路径必须改走
    //   registerHandlerViaRegex 绕开该校验。
    // 收尾优化:超时状态由定时器与协程共享,响应回调在"发出响应那一刻"立即清空 ——
    //   不再被定时器按值捕获而滞留到 deadlineMs(drogon 的响应回调持有
    //   TcpConnectionPtr + 请求对象**含 body**,大 body 请求下滞留成本可观);
    //   业务先完成时同时注销定时器(invalidateTimer 只删 id、线程安全、幂等;
    //   定时器对象本身仍留到到期,但已不再持有 cb)。
    auto handler = [h, deadlineMs](HttpRequestPtr req,
                                   std::function<void(const HttpResponsePtr&)> cb) -> Task<> {
        auto st = std::make_shared<ZmDeadlineState>();
        st->cb = std::move(cb);
        st->connWk = req->getConnectionPtr();

        trantor::EventLoop* loop = nullptr;
        if (auto c = st->connWk.lock())
            loop = c->getLoop();
        if (!loop)
            loop = app().getLoop();

        // 超时定时器:放连接所属 loop;id 保存供业务先完成时注销
        // 到期时以原子门抢占"回答权":抢到则校验连接仍在,再回 504
        const trantor::TimerId tid = loop->runAfter(deadlineMs / 1000.0, ZmDeadlineFire{st});

        // 统一收尾(业务成功/异常共用)
        const ZmDeadlineFinish finish{st, loop, tid};

        try
        {
            auto resp = co_await h(req);
            finish(resp ? std::move(resp)
                        : ErrorResponse(500, "handler returned null response"));
        }
        catch (const std::exception& e)
        {
            PUBLIC_LOG_ERROR("RegisterCoroWithDeadline 业务异常: {}", e.what());
            finish(ErrorResponse(500, "internal error"));
        }
        catch (...)
        {
            finish(ErrorResponse(500, "internal error"));
        }
    };

    if (path.find('{') == string::npos)
        app().registerHandler(path, std::move(handler), cons);
    else
        app().registerHandlerViaRegex(PathPatternToRegex(path), std::move(handler), cons);
}

// ── Multipart 表单与文件上传 ──
//   流式 newMultipartReader(MultipartHeader{name,filename,contentType} 已由框架
//   解析 Content-Disposition);部件间数据并入当前部件;字段值 ≤1MB、总量 maxBytes
//   超限 → 413(止损);完成/异常在 finish 回调统一桥接业务协程。
namespace
{
constexpr int64_t kMaxFieldBytes = 1 << 20;  // 单字段值上限(1MB)
/// 文件部件全量驻留内存的安全上限:调用方 maxBytes=0 且全局闸门也为 0 时的兜底
constexpr uint64_t kMultipartMemoryCeiling = 256ULL * 1024 * 1024;

/**
 * @brief 大小写不敏感的子串查找(RFC 规定 media type 与参数名大小写不敏感)
 *
 * @param hay     被查串(如 Content-Type 头值)
 * @param needle  待查串(须为小写字面量)
 * @return 首次出现的下标;未命中返回 string::npos
 */
size_t FindNoCase(const string& hay, const char* needle)
{
    const size_t n = std::strlen(needle);
    if (n == 0 || hay.size() < n)
        return string::npos;
    for (size_t i = 0; i + n <= hay.size(); ++i)
    {
        size_t j = 0;
        while (j < n &&
               std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                   static_cast<unsigned char>(needle[j]))
            ++j;
        if (j == n)
            return i;
    }
    return string::npos;
}

/// Multipart 收集器状态(全部在事件循环线程按序访问,无需锁)
struct ZmMultipartCollector
{
    ZmHttpServer::ZmMultipartResult result;
    bool fieldMode = true;        // 当前部件是字段(文件索引在 result.files)
    bool fieldActive = false;     // 已有部件开始(提交判断)
    string curFieldKey;
    string curField;
    int64_t curFieldSize = 0;
    uint64_t total = 0;
    uint64_t maxBytes = 0;        // 单请求总量上限(0 = 不限制)
    bool tooLarge = false;        // 累积超限/字段超限(交付 413)
};

/**
 * @brief 清洗上传文件名:只保留安全的文件名段
 *
 * 去掉路径成分(兼容 / 与 \),再剔除 Windows 非法字符与控制符,防路径穿越与注入。
 *
 * @param name  原始文件名(来自 Content-Disposition)
 * @return 清洗后的文件名;结果为空串表示名字不可用(".", ".." 或全为非法字符)
 */
std::string SanitizeFileName(std::string name)
{
    // 去掉路径成分(兼容 / 与 \)仅留文件名段
    size_t pos = name.find_last_of("\\/");
    if (pos != string::npos)
        name = name.substr(pos + 1);
    // 去 Windows 非法字符与控制符(防穿越/注入)
    std::string out;
    out.reserve(name.size());
    for (char c : name)
    {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            continue;
        out += c;
    }
    if (out == "." || out == "..")
        return "";
    return out;
}
}  // namespace

/**
 * @brief 注册 multipart 表单/文件上传路由(流式解析)
 *
 * 框架按 Content-Disposition 解析各部件头;字段值上限 1MB、单请求总量上限 maxBytes,
 * 超限即 413 并中途换 NullReader 丢弃剩余数据(止损,不整条消费)。解析完成后统一桥接
 * 到业务协程(handler 返回 null 或抛异常 → 500)。
 *
 * @param path      路由路径(含 {N} 占位符时自动改走 regex 注册)
 * @param m         HTTP 方法
 * @param h         业务协程 handler(收到收集完成的字段与文件)
 * @param filters   filter 名列表(须已经 AddFilter 注册;未注册仅记错误日志)
 * @param maxBytes  单请求总量上限(0 = 不限制,由全局配置兜底)
 */
/**
 * @brief multipart 部件头到达:按有无 filename 判定字段/文件部件
 */
struct ZmMultipartHeaderCb
{
    std::shared_ptr<ZmMultipartCollector> state;   ///< 收集状态

    void operator()(MultipartHeader header) const
{

    if (state->tooLarge)
        return;
    if (state->fieldActive && state->fieldMode)
        state->result.fields.emplace_back(state->curFieldKey, state->curField);
    state->fieldActive = true;
    if (header.filename.empty())
    {
        state->fieldMode = true;
        state->curFieldKey = header.name;
        state->curField.clear();
        state->curFieldSize = 0;
    }
    else
    {
        state->fieldMode = false;
        ZmHttpServer::ZmMultipartResult::File f;
        f.itemName = header.name;
        f.fileName = SanitizeFileName(header.filename);
        f.contentType = header.contentType;
        state->result.files.push_back(std::move(f));
    }
}
};

/**
 * @brief multipart 部件数据到达:字段按上限累积、文件追加到当前文件部件
 */
struct ZmMultipartDataCb
{
    std::shared_ptr<ZmMultipartCollector> state;    ///< 收集状态
    drogon::RequestStreamPtr              stream;   ///< 读流(达限时置空读者以丢弃剩余)

    void operator()(const char* buf, size_t len) const
{

    if (state->tooLarge)
        return;
    if (state->fieldMode)
    {
        if (state->curFieldSize + static_cast<int64_t>(len) > kMaxFieldBytes)
        {
            state->tooLarge = true;
            // 字段超限 → 中途换 NullReader 丢弃剩余网络数据,
            // 不再整条消费(与 RegisterStreamCoro 早拒语义对齐);正在执行的
            // 旧 multipart reader 由流内部在本次交付后释放,无悬空。
            stream->setStreamReader(drogon::RequestStreamReader::newNullReader());
            return;
        }
        state->curField.append(buf, len);
        state->curFieldSize += static_cast<int64_t>(len);
    }
    else
    {
        auto& f = state->result.files.back();
        f.data.append(buf, len);
        f.size += len;
    }
    state->total += len;
    if (state->maxBytes > 0 && state->total > state->maxBytes)
    {
        state->tooLarge = true;
        // 总量超限 → 同上,立即停止消费
        stream->setStreamReader(drogon::RequestStreamReader::newNullReader());
    }
}
};

/**
 * @brief 把 multipart 解析结果交付业务协程,并把返回值写回 drogon 回调
 */
struct ZmMultipartDispatch
{
    ZmHttpServer::ZmMultipartHandler                        h;     ///< 业务处理协程
    HttpRequestPtr                              req;   ///< 请求
    std::function<void(const HttpResponsePtr&)> cb;    ///< drogon 响应回调
    ZmHttpServer::ZmMultipartResult             res;   ///< 解析出的字段与文件

    drogon::Task<> operator()()
    {
        try
        {
            auto resp = co_await h(req, std::move(res));
            if (resp)
                cb(resp);
            else
                cb(ZmHttpServer::ErrorResponse(500, "multipart handler: null response"));
        }
        catch (const std::exception& ex)
        {
            PUBLIC_LOG_ERROR("multipart 业务异常: {}", ex.what());
            cb(ZmHttpServer::ErrorResponse(500, "multipart handler: internal error"));
        }
        catch (...)
        {
            cb(ZmHttpServer::ErrorResponse(500, "multipart handler: internal error"));
        }
    }
};

/**
 * @brief multipart 读流结束:超限/中断回 413/400,正常则把结果桥接给业务协程
 */
struct ZmMultipartFinishCb
{
    std::shared_ptr<ZmMultipartCollector>       state;   ///< 收集状态
    ZmHttpServer::ZmMultipartHandler                        h;       ///< 业务处理协程
    HttpRequestPtr                              req;     ///< 请求
    std::function<void(const HttpResponsePtr&)> cb;      ///< drogon 响应回调

    void operator()(std::exception_ptr e) const
{

    if (state->tooLarge)
    {
        cb(ZmHttpServer::ErrorResponse(413, "multipart too large"));
        return;
    }
    if (e)
    {
        PUBLIC_LOG_WARN("multipart 流中断(网络/客户端异常)");
        cb(ZmHttpServer::ErrorResponse(400, "multipart aborted"));
        return;
    }
    if (state->fieldActive && state->fieldMode)
        state->result.fields.emplace_back(state->curFieldKey, state->curField);
    drogon::async_run(ZmMultipartDispatch{h, req, cb, std::move(state->result)});
}
};

void ZmHttpServer::RegisterMultipartCoro(const string& path, drogon::HttpMethod m,
                                         ZmMultipartHandler h,
                                         const vector<string>& filters,
                                         uint64_t maxBytes)
{
    // 归属校验(同 RegisterCoro)
    if (!CheckRouteOwnership(path, "RegisterMultipartCoro"))
        return;
    vector<internal::HttpConstraint> cons;
    cons.emplace_back(m);
    for (const auto& fn : filters)
    {
        if (!CheckFilterRegistered(fn))
            PUBLIC_LOG_ERROR("RegisterMultipartCoro[{}]: filter 未注册: {}", path, fn);
        cons.emplace_back(fn);
    }

    // 三参流式形态(同 RegisterStreamCoro):框架注入 RequestStream
    auto fn = [h = std::move(h), maxBytes](
                  const HttpRequestPtr& req, drogon::RequestStreamPtr&& streamCtx,
                  std::function<void(const HttpResponsePtr&)>&& cb) {
        // ① Content-Type / boundary 预检(缺 → 400,不消费流)
        string ct = req->getHeader("Content-Type");
        // media type 与参数名大小写不敏感(RFC 7231/2045),不能按原样 find
        if (FindNoCase(ct, "multipart/form-data") == string::npos ||
            FindNoCase(ct, "boundary=") == string::npos)
        {
            cb(ZmHttpServer::ErrorResponse(400, "multipart form-data required"));
            return;
        }
        // 单请求总量上限:0 不再等于"无上限"(否则单请求可把内存吃满)——
        // 依次回落到非流式体量闸门、内置硬上限
        uint64_t effMax = maxBytes;
        if (effMax == 0)
            effMax = s_nonStreamBodyLimit.load(std::memory_order_relaxed);
        if (effMax == 0)
            effMax = kMultipartMemoryCeiling;
        // ② 声明超限预检(413;Content-Length 缺失(chunked)由运行时 total 兜底)
        {
            string cl = req->getHeader("Content-Length");
            if (!cl.empty())
            {
                try
                {
                    if (std::stoull(cl) > effMax)
                    {
                        streamCtx->setStreamReader(
                            drogon::RequestStreamReader::newNullReader());
                        cb(ZmHttpServer::ErrorResponse(413, "multipart too large"));
                        return;
                    }
                }
                catch (...) {}
            }
        }

        auto state = std::make_shared<ZmMultipartCollector>();
        state->maxBytes = effMax;
        // 超限后中途换 NullReader 用(拷贝持有流;见 dataCb)
        drogon::RequestStreamPtr stream = streamCtx;

        // 部件头到达:先提交上一个字段部件,再按 filename 有无判定本部件是字段还是文件
        ZmMultipartHeaderCb headerCb{state};
        // 部件间数据到达:字段按上限累积,文件追加到当前文件部件
        ZmMultipartDataCb dataCb{state, stream};
        // 流结束回调:超限/中断先回 413/400;正常则把收集结果桥接到业务协程
        ZmMultipartFinishCb finishCb{state, h, req, cb};
        streamCtx->setStreamReader(
            drogon::RequestStreamReader::newMultipartReader(req, headerCb, dataCb, finishCb));
    };

    if (path.find('{') == string::npos)
        app().registerHandler(path, std::move(fn), cons);
    else
        app().registerHandlerViaRegex(PathPatternToRegex(path), std::move(fn), cons);
}

/**
 * @brief 把 multipart 收集到的文件部件落盘
 *
 * 经 RunOnPool 投递到业务工作池执行(事件循环纪律:业务回调内禁止直接写文件);
 * 路径按 UTF-8 契约转 wide 后打开,按 64MB 分块循环写入以支持大文件。
 *
 * @param f         待落盘的文件部件(内容已在内存中)
 * @param destPath  目标文件路径(UTF-8 编码)
 * @return 写入的字节数;路径转换失败、创建文件失败或写盘失败时返回 -1
 *
 * @example
 *   int64_t n = co_await ZmHttpServer::SaveMultipartFile(res.files[0], "D:/up/a.bin");
 *   if (n < 0)
 *       co_return ZmHttpServer::ErrorResponse(500, "save failed");
 */
/**
 * @brief 多部件文件落盘任务:在阻塞工作池内分块写入(事件循环不得直接写文件)
 */
struct ZmMultipartWriteTask
{
    const ZmHttpServer::ZmMultipartResult::File* f = nullptr;   ///< 待落盘文件部件
    const std::string*                           destPath = nullptr;  ///< 目标路径(UTF-8)

    int64_t operator()() const
{

    std::wstring wpath = ZmString::UTF8_To_Unicode(*destPath);
    if (wpath.empty())
        return -1;
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    const char* p = f->data.data();
    size_t off = 0;
    while (off < f->data.size())
    {
        DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(f->data.size() - off, (1u << 26)));
        DWORD wrote = 0;
        if (!::WriteFile(h, p + off, chunk, &wrote, nullptr) || wrote == 0)
        {
            ::CloseHandle(h);
            return -1;
        }
        off += wrote;
    }
    ::CloseHandle(h);
    return static_cast<int64_t>(f->data.size());
}
};

drogon::Task<int64_t> ZmHttpServer::SaveMultipartFile(const ZmMultipartResult::File& f,
                                                      const string& destPath)
{
    // 离核工作池写盘(事件循环纪律:回调内禁止直接写文件);路径 UTF-8 → wide
    co_return co_await RunOnPool<int64_t>(ZmMultipartWriteTask{&f, &destPath});
}

// ── 流式接收(路径 B;与 Options.enableRequestStream 配套) ──
//   drogon stream 契约:registerHandler 底层 FunctionTraits 匹配三参形态
//   (req, RequestStreamPtr&&, callback) → isStreamHandler = true,框架自动经
//   internal::createRequestStream 注入流对象;数据经 RequestStreamReader 逐块交付。
//   RegisterStreamCoro:三参流式回调(事件循环) → async_run 桥接业务协程。
//   SaveStreamToFile:块到即写 + maxBytes 兜底,完成回调恢复协程。
namespace
{
/**
 * @brief 流式上传的落盘执行器(路径 B)
 *
 * 事件循环线程接收数据块(fifo 入队),专用写线程顺序落盘 —— 写盘绝不占用事件循环
 * (NFR)。统一处理停写(超限/用户取消/网络中断)、进度节流、结束与失败回执,
 * 并保证半成品一致性(失败/超限即删除半成品)。
 *
 * 生命周期:Create 的返回值会被 awaiter 丢弃,故 Attach() 内部自引用撑住自身,
 * 直到 Done() 的完成回调才释放。
 */
class ZmUploadSink : public std::enable_shared_from_this<ZmUploadSink>
{
public:
    /**
     * @brief 创建落盘执行器并立即挂上流读取回调
     *
     * @param stream    请求流对象(框架注入)
     * @param destPath  目标文件路径(UTF-8 编码)
     * @param opts      落盘参数(上限/进度/总量)
     * @param tooLarge  出参:是否因超限收场(其生命周期须覆盖整个上传过程)
     * @param done      完成回调(bool ok),在事件循环线程调用一次
     * @return 执行器实例(调用方通常直接丢弃,内部自引用保活)
     */
    static std::shared_ptr<ZmUploadSink>
    Create(drogon::RequestStreamPtr stream, std::string destPath,
           ZmHttpUploadFileOptions opts, bool* tooLarge,
           std::function<void(bool)> done)
    {
        auto p = std::shared_ptr<ZmUploadSink>(new ZmUploadSink(
            std::move(stream), std::move(destPath), std::move(opts),
            tooLarge, std::move(done)));
        p->Attach();   // 挂读取回调(内部自引用保活)
        return p;
    }

    /**
     * @brief 析构:停止写线程(安全 join),文件收尾由写线程自身负责
     *
     * 写线程退出前持有自身强引用,故半成品的清理总是先于本析构完成。
     */
    ~ZmUploadSink()
    {
        // 置停止标志后 Stop(join) —— 写线程自身持有强引用,半成品清理必达
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

    /**
     * @brief 构造执行器(只存参数,不启动写线程;请经 Create 走完整流程)
     *
     * @param stream    请求流对象
     * @param destPath  目标文件路径(UTF-8)
     * @param opts      落盘参数
     * @param tooLarge  超限标记出参
     * @param done      完成回调
     */
    ZmUploadSink(drogon::RequestStreamPtr stream, std::string destPath,
                 ZmHttpUploadFileOptions opts, bool* tooLarge,
                 std::function<void(bool)> done)
        : m_stream(std::move(stream)), m_dest(std::move(destPath)),
          m_opts(std::move(opts)), m_tooLarge(tooLarge), m_done(std::move(done))
    {
    }

    /**
     * @brief 启动写线程并挂上流读取回调(自引用保活)
     *
     * 写线程必须先于 setStreamReader 启动:挂读回调时 drogon 会同步灌入已缓冲的
     * 请求体,写线程先就位才能即时消费,否则初始突发会撞上积压上限。
     */
    void Attach()
    {
        // 回调与回执统一投本请求所属事件循环:进度/完成回调与数据块回调(OnData)同线程,
        // 业务侧无需加锁(与 RunOnPool 的"连接 loop 优先"取值一致)
        m_loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        if (!m_loop)
            m_loop = drogon::app().getLoop();
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
            Finish(false, true, 0);
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

    /**
     * @brief 打开目标文件(覆盖创建),仅写线程调用
     * @return true 句柄有效;false 路径转换失败或创建文件失败
     */
    bool OpenFile()
    {
        std::wstring wpath = ZmString::UTF8_To_Unicode(m_dest);
        if (wpath.empty())
            return false;
        m_handle = ::CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
    }

    /**
     * @brief 把一整块数据完整写入(内部按 64MB 分片循环 WriteFile),仅写线程调用
     *
     * @param buf  数据指针
     * @param len  数据长度(字节)
     * @return true 全部写入;false 写盘失败(调用方负责清理半成品)
     */
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

    /**
     * @brief 关闭文件句柄(幂等),仅写线程调用
     */
    void CloseFile()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    /**
     * @brief 清理收尾:关句柄,并按需删除半成品文件,仅写线程调用
     *
     * @param deleteFile  true = 失败/中止/超限,删除半成品;false = 保留已落盘内容
     */
    void Cleanup(bool deleteFile)   // 失败/中止/超限 → 删半成品
    {
        CloseFile();
        if (deleteFile)
        {
            std::wstring wpath = ZmString::UTF8_To_Unicode(m_dest);
            if (!wpath.empty())
                ::DeleteFileW(wpath.c_str());
        }
    }

    /**
     * @brief 触发完成回调(只调一次)并释放自引用,仅事件循环线程调用
     *
     * 自引用释放后,若写线程已退出,最后一个引用随之归零 → 对象在事件循环上析构。
     *
     * @param ok  上传是否成功
     */
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

    /**
     * @brief 写线程 → 事件循环的结果回执,仅写线程调用
     *
     * 以强引用入队,保证残局(半成品清理 + done 回调)必然执行,不依赖对象的存活判定。
     * 投到本请求所属事件循环:与 OnData 同线程,故 m_fini/流对象都只在单一线程上变更。
     *
     * @param self      自引用(保证入队与回执期间对象存活)
     * @param ok        上传是否成功
     * @param tooLarge  是否因超限失败
     * @param written   本次落盘总字节(成功路径的终态进度值)
     */
    void PostFinish(std::shared_ptr<ZmUploadSink> self, bool ok, bool tooLarge,
                    uint64_t written)
    {
        auto* loop = m_loop ? m_loop : drogon::app().getLoop();
        loop->queueInLoop(
            [self, ok, tooLarge, written]() { self->Finish(ok, tooLarge, written); });
    }

    /**
     * @brief 专用写线程主循环:开文件 → 顺序消费写队列 → 结果回执到事件循环
     *
     * 终局有三种:写盘失败、累计写入超限、队列排空(收到结束或停止信号)。
     * 前两者删除半成品;排空后仅在未收到停止信号时保留文件。
     *
     * @param keepAlive  自引用(写线程运行期间保证 sink 存活)
     */
    void WriterMain(std::shared_ptr<ZmUploadSink> keepAlive)
    {
        if (!OpenFile())
        {
            // 开文件失败:置 tooLarge 标记,由事件循环侧删半成品
            m_writerDead.store(true);
            PostFinish(std::move(keepAlive), false, true, 0);
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
                PostFinish(std::move(keepAlive), false, false, 0);
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
                PostFinish(std::move(keepAlive), false, true, 0);
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
        // written = 本次应落盘总量(队列已排空),成功即业务可见的 100%
        PostFinish(std::move(keepAlive), ok, false, written);
    }

    /**
     * @brief 收尾(幂等),仅事件循环线程调用
     *
     * 回填超限标记,失败/超限时把流换成 NullReader 丢弃剩余网络数据,最后触发 done。
     * 成功路径在 done 之前补发一次终态进度(全量已落盘),业务因此必然收到 100%。
     *
     * @param ok            上传是否成功
     * @param tooLarge      是否超限
     * @param finalWritten  本次落盘总字节(成功路径的终态进度值;失败路径忽略)
     */
    void Finish(bool ok, bool tooLarge, uint64_t finalWritten)
    {
        if (m_fini)
            return;
        m_fini = true;
        if (m_tooLarge)
            *m_tooLarge = tooLarge;
        if (!ok || tooLarge)
            m_stream->setStreamReader(drogon::RequestStreamReader::newNullReader());
        else
            EmitProgress(finalWritten);   // 终态进度:先报满量,再触发 done
        Done(ok);
    }

    /**
     * @brief 回调一次进度
     *
     * 分母优先取业务透传的 totalBytes(如 X-File-Size),未透传时退回 maxBytes
     * (maxBytes = 0 即不限量,此时百分比分母为 0,由业务自行处理)。
     *
     * @param written  本次已落盘字节
     */
    void EmitProgress(uint64_t written)
    {
        if (m_opts.onProgress)
            m_opts.onProgress(written, m_opts.totalBytes ? m_opts.totalBytes
                                                         : m_opts.maxBytes);
    }

    /**
     * @brief 数据块到达(事件循环线程):入队给写线程,并处理积压与进度
     *
     * 积压超过上限(kUploadQueueCap)即中止上传,保证缓冲内存有界;
     * 进度回调按 progressIntervalMs 节流(m_lastProgressMs 初值 0 使首块必过闸)。
     * 本函数看到的是"已落盘量",恒落后于当前块,故 100% 由 Finish() 在落盘完成后补发。
     *
     * @param buf  数据块指针
     * @param len  数据块长度(字节)
     */
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
            Finish(false, false, 0);
            return;
        }
        if (m_opts.onProgress)
        {
            int64_t now = NowMs();
            if (now - m_lastProgressMs >= (int64_t)m_opts.progressIntervalMs)
            {
                m_lastProgressMs = now;
                EmitProgress(m_written.load(std::memory_order_relaxed));
            }
        }
    }

    /**
     * @brief 请求体结束(事件循环线程):通知写线程排空,异常则作废半成品
     *
     * @param e  非空 = 网络/客户端异常,立即以失败收尾;空 = 正常结束,
     *           等写线程把队列落完后再回执成功
     */
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
        Finish(false, false, 0);
    }

    drogon::RequestStreamPtr m_stream;
    std::string m_dest;
    ZmHttpUploadFileOptions m_opts;
    bool* m_tooLarge = nullptr;
    std::function<void(bool)> m_done;
    std::shared_ptr<ZmUploadSink> m_self;   ///< 自引用:Create 返回值被丢弃,靠它撑生命周期

    /// 本请求所属事件循环(Attach 时定型):数据块回调、进度、完成回执与写线程回执都投这里
    trantor::EventLoop* m_loop = nullptr;

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

/**
 * @brief SaveStreamToFile 的恢复器:把落盘完成回调桥接为协程值
 */
struct ZmSinkAwaiter : drogon::CallbackAwaiter<bool>
{
    drogon::RequestStreamPtr stream;
    std::string dest;
    ZmHttpUploadFileOptions opts;
    bool* tooLarge = nullptr;

    /**
     * @brief 挂起业务协程,创建落盘执行器,在其完成回调里写值并恢复协程
     *
     * @param h  被挂起的业务协程句柄(局部变量位于协程帧内,回调期间仍有效)
     */
    void await_suspend(std::coroutine_handle<> h)
    {
        // 完成回调在事件循环线程触发:先写值,再恢复业务协程
        ZmUploadSink::Create(std::move(stream), std::move(dest), std::move(opts),
                             tooLarge, [this, h](bool ok) {
            this->setValue(ok);
            h.resume();
        });
    }
};

}  // namespace

/**
 * @brief 流式接收落盘助手:把 RequestStream 写入目标路径
 *
 * 块到达即回调,全程经 ZmUploadSink(事件循环入队 + 专用写线程,磁盘 I/O 不占事件循环)。
 * 失败(网络中断/写盘错误/超限)会清理半成品。
 *
 * @param stream    请求流对象(由 stream-handler 形参传入)
 * @param destPath  目标文件路径(UTF-8 编码)
 * @param opts      落盘参数(上限/进度/总量)
 * @param tooLarge  出参:true = 因超限失败(需与其它失败区分以回 413)
 * @return true 完整落盘成功;false 失败(经 tooLarge 区分超限与其他原因)
 *
 * @example
 *   bool tooLarge = false;
 *   if (!co_await ZmHttpServer::SaveStreamToFile(std::move(stream), path, opts, &tooLarge))
 *       co_return ZmHttpServer::ErrorResponse(tooLarge ? 413 : 500, "upload failed");
 */
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

/**
 * @brief 注册"流式接收"路由(路径 B)
 *
 * handler 形参带 RequestStreamPtr,drogon FunctionTraits 据此判定为 stream-handler,
 * 框架自动注入流对象并逐块回调;内部再桥接到业务协程(回调 → 协程形态统一)。
 *
 * @param path      路由路径(含 {N} 占位符时自动改走 regex 注册)
 * @param m         HTTP 方法
 * @param h         业务协程 handler(形参含 RequestStreamPtr)
 * @param filters   filter 名列表(须已经 AddFilter 注册;未注册仅记错误日志)
 * @param maxBytes  路由级上传上限(0 = 不额外限制,由全局配置兜底):X-File-Size
 *                  声明超限 → 丢剩余并 413(newNullReader),并把上限写 req
 *                  attributes("ZmStreamMaxBytes")供业务 SaveStreamToFile 兜底取用
 */
/**
 * @brief 把 drogon 流对象桥接到业务流式协程,并把返回值写回响应回调
 */
struct ZmStreamDispatch
{
    ZmHttpStreamHandler                         h;         ///< 业务流式协程
    HttpRequestPtr                              req;       ///< 请求
    drogon::RequestStreamPtr                    streamCtx; ///< 框架注入的读流
    std::function<void(const HttpResponsePtr&)> cb;        ///< drogon 响应回调

    drogon::Task<> operator()() const
{

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
}
};

void ZmHttpServer::RegisterStreamCoro(const string& path, drogon::HttpMethod m,
                                      ZmHttpStreamHandler h, const vector<string>& filters,
                                      uint64_t maxBytes)
{
    // 归属校验(同 RegisterCoro)
    if (!CheckRouteOwnership(path, "RegisterStreamCoro"))
        return;
    vector<internal::HttpConstraint> cons;
    cons.emplace_back(m);
    for (const auto& fn : filters)
    {
        if (!CheckFilterRegistered(fn))
            PUBLIC_LOG_ERROR("RegisterStreamCoro[{}]: filter 未注册: {}", path, fn);
        cons.emplace_back(fn);
    }

    // 三参流式回调(FunctionTraits 匹配 (req, stream, cb) → isStreamHandler):
    // 数据逐块交付;内部 bridge 到业务协程(异步回调 → 协程, 协程形态统一)
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

        drogon::async_run(ZmStreamDispatch{h, req, std::move(streamCtx),
                                          std::move(cb)});
    };

    if (path.find('{') == string::npos)
        app().registerHandler(path, std::move(fn), cons);
    else
        app().registerHandlerViaRegex(PathPatternToRegex(path), std::move(fn), cons);
}

// ── WebSocket ──
/**
 * @brief 注册 WebSocket 路由
 *
 * 每个 path 生成唯一注册名,经 DrClassMap 工厂实例化通用 ZmWsController;
 * 回调按注册名存入全局表,控制器构造时快照副本(消息路径零加锁)。
 *
 * @param path  路由路径(WS 升级端点)
 * @param cb    回调集合(onOpen/onMessage/onClose/onAuth)
 *
 * @example
 *   ZmHttpServer::WsCallbacks cb;
 *   cb.onAuth = [](const HttpRequestPtr& req) { return CheckToken(req); };
 *   cb.onMessage = [](const WebSocketConnectionPtr& c, std::string&& m,
 *                     const WebSocketMessageType&) { c->send(m); };
 *   ws.RegisterWebSocket("/ws/echo", cb);
 */
void ZmHttpServer::RegisterWebSocket(const string& path, const WsCallbacks& cb)
{
    // 归属校验(WS 路由同样受 per-port 门禁约束)
    if (!CheckRouteOwnership(path, "RegisterWebSocket"))
        return;
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

// ── Filter 与 advice ──

// ── 路由与 Filter ──
/**
 * @brief 查询 filter 是否已注册
 *
 * @param name  filter 名
 * @return true 已注册;false 未注册(注册路由时仅记错误日志,不拒绝该路由)
 */
bool ZmHttpServer::CheckFilterRegistered(const string& name)
{
    std::lock_guard lock(s_filterMtx);
    return s_filters.count(name) != 0;
}

/**
 * @brief 注册具名 filter
 *
 * filter 本体存入文件级注册表,同时以自定义名注册进 DrClassMap 共享工厂 ——
 * 路由经 HttpConstraint 按名引用,构造时按名取回函数并适配为 drogon HttpFilter。
 * 同一 filter 只由归属面注册一次(重复注册会覆盖同名实体)。须在 Open 前调用。
 *
 * @param name  filter 名(路由侧以此名引用)
 * @param f     过滤函数:返回 false 即拒绝请求(可经第二参回填响应;
 *              未回填时由适配层兜底 500)
 *
 * @example
 *   api.AddFilter("auth", [](const HttpRequestPtr& req, HttpResponsePtr& resp) {
 *       if (!CheckToken(req)) {
 *           resp = ZmHttpServer::ErrorResponse(401, "unauthorized");
 *           return false;
 *       }
 *       return true;
 *   });
 *   api.RegisterCoro("/api/me", Get, HandleMe, {"auth"});
 */
namespace
{
/**
 * @brief 按名构造 filter 实例的工厂(drogon 反射按约束名解析时调用)
 *
 * 未注册的名字返回空指针;命中则取一份 filter 函数构造实例。
 */
struct ZmFilterFactory
{
    std::string name;   ///< filter 注册名(路由约束里引用的那个名字)

    std::shared_ptr<DrObjectBase> operator()() const
    {
        std::function<bool(const HttpRequestPtr&, HttpResponsePtr&)> fn;
        {
            std::lock_guard lock(s_filterMtx);
            auto it = s_filters.find(name);
            if (it == s_filters.end())
                return std::shared_ptr<DrObjectBase>(nullptr);
            fn = it->second;
        }
        return std::make_shared<ZmFuncFilter>(fn);
    }
};
}  // namespace

void ZmHttpServer::AddFilter(const string& name,
                                 const std::function<bool(const HttpRequestPtr&,
                                                          HttpResponsePtr&)>& f)
{
    {
        std::lock_guard lock(s_filterMtx);
        s_filters[name] = f;
    }
    // 按名注册:自定义名 + 共享工厂(经注册表按名取回函数)
    DrClassMap::registerClass(name, nullptr, ZmFilterFactory{name});
    // 强制物化单例,保障框架按名解析(与 Api 契约一致)
    DrClassMap::getSingleInstance(name);
}

// ── advice 挂点 ──
//   透传注册。纪律:同一 advice 只由归属面注册一次(门禁/SPA 归前端面、
//   CORS 与 OPTIONS 预检归 RESTful 面);advice 组合(如复用 PreRouting 挂多个)
//   允许按序注册。基类内置项(/ping、AccessLogger)仍经 once_flag 去重。
/**
 * @brief 注册 PreRouting advice(路由前;可改写请求或提前拒绝)
 *
 * 透传注册。纪律:同一 advice 只由归属面注册一次(门禁/SPA 归前端面,
 * CORS 与 OPTIONS 预检归 RESTful 面);同一挂点允许按序注册多个 advice。
 *
 * @param a  advice 函数:调用 cc() 放行到下一环,调用 cb(resp) 直接回响应
 */
void ZmHttpServer::RegisterPreRouting(std::function<void(const HttpRequestPtr&,
                                                             AdviceCallback&&,
                                                             AdviceChainCallback&&)> a)
{
    app().registerPreRoutingAdvice(std::move(a));
}

/**
 * @brief 注册 PostRouting advice(路由已确定、handler 执行前)
 *
 * @param a  advice 函数:调用 cc() 放行到 handler,调用 cb(resp) 直接回响应
 */
void ZmHttpServer::RegisterPostRouting(std::function<void(const HttpRequestPtr&,
                                                              AdviceCallback&&,
                                                              AdviceChainCallback&&)> a)
{
    app().registerPostRoutingAdvice(std::move(a));
}

/**
 * @brief 注册 PostHandling advice(handler 已产出响应)
 *
 * ⚠ 覆盖面有限:在 drogon 1.9.13 中只覆盖 controller/binder 响应路径
 * (HttpServer.cc:658/692/764),静态目录(含 304)、Range 响应、重定向等经
 * sendResponses 直达发送链,不经本 advice —— 需要"覆盖一切响应"的结算请挂 PreSending。
 *
 * @param a  advice 函数(只读观察响应,不能再改状态码)
 */
void ZmHttpServer::RegisterPostHandling(std::function<void(const HttpRequestPtr&,
                                                               const HttpResponsePtr&)> a)
{
    app().registerPostHandlingAdvice(std::move(a));
}

/**
 * @brief 注册 PreSending advice(响应即将发送,覆盖一切响应)
 *
 * 统一出口:静态目录/304/Range/重定向/拦截响应都会经过这里,适合做响应头注入
 * 与访问日志结算。
 *
 * @param a  advice 函数(可经 resp 修改头/体;此时不可再改状态码)
 */
void ZmHttpServer::RegisterPreSending(std::function<void(const HttpRequestPtr&,
                                                             const HttpResponsePtr&)> a)
{
    app().registerPreSendingAdvice(std::move(a));
}

// ── 归属与共享 ──

// ── 路由归属:声明 / 查询 / 校验 ──
/**
 * @brief 生成面的可读描述(日志/归属诊断用)
 *
 * @return 形如 "/api@8443" 的描述;root 未设时显示"无root",监听未登记时端口记 0
 */
std::string ZmHttpServer::FaceDesc() const
{
    std::string root = m_rootPath.empty() ? std::string("无root") : m_rootPath;
    return root + "@" + std::to_string(m_listenerSet ? m_listener.port : 0);
}

/**
 * @brief 声明本面的业务根路径并登记进进程级归属表
 *
 * root 在进程内唯一:重复声明即记入归属冲突,Open() 会拒绝启动(保留先声明者)。
 * run 之后不可再改归属。
 *
 * @param path  根路径;空串 = 本面不拥有任何前缀(如只挂 advice 的重定向实例),
 *              "/" = 兜底归属(未被其他面认领的路径归本面)
 *
 * @example
 *   api.SetRootPath("/api");     // 拥有 /api 及其子路径
 *   front.SetRootPath("/");      // 兜底面
 */
void ZmHttpServer::SetRootPath(const std::string& path)
{
    if (s_state.load() >= ZmRuntimeState::Opened)
    {
        PUBLIC_LOG_ERROR("SetRootPath 已跳过(run 后不可改归属): {}", path);
        return;
    }
    std::lock_guard<std::mutex> lk(s_ownerMtx);
    // 一对象一条声明:换 root 即覆盖旧声明(空串 = 撤回),归属表随之整体重建
    ZmClaimSlot(s_rootClaims, this) = path;
    m_rootPath = path;
    ZmRebuildRootOwners();
    if (path.empty())
        return;   // 空 = 不拥有任何前缀(如前端重定向专用实例:只挂 advice)
    auto it = s_rootOwners.find(path);
    if (it != s_rootOwners.end() && it->second != this)
    {
        PUBLIC_LOG_ERROR("SetRootPath: root \"{}\" 被 {} 与 {} 同时声明"
                         " —— 归属冲突,Open() 将拒绝启动(保留先声明者)",
                         path, it->second->FaceDesc(), FaceDesc());
        return;
    }
    PUBLIC_LOG_INFO("ZmHttpServer::SetRootPath: {} 声明归属 \"{}\"{}", FaceDesc(), path,
                    path == "/" ? "(兜底:未被其他面认领的路径归本面)" : "");
}

/**
 * @brief 查询路径的归属面
 *
 * 最长段前缀;Open 之后走只读快照,零加锁。
 *
 * @param path  请求路径
 * @return 归属面指针;无归属时为 nullptr
 */
const ZmHttpServer* ZmHttpServer::LookupOwner(std::string_view path)
{
    if (auto snap = s_ownerSnap.load(std::memory_order_acquire))
        return ZmLookupInTable(snap->roots, path);
    std::lock_guard<std::mutex> lk(s_ownerMtx);
    return ZmLookupInTable(s_rootOwners, path);
}

/**
 * @brief 查询路径是否为平台共享路径
 *
 * 共享路径三面可达:各面门禁与运行期归属网均以此豁免。
 *
 * @param path  请求路径
 * @return true 已由 MarkShared 声明为共享;false 未声明
 */
bool ZmHttpServer::IsSharedPath(std::string_view path)
{
    if (auto snap = s_ownerSnap.load(std::memory_order_acquire))
        return snap->shared.find(path) != snap->shared.end();
    std::lock_guard<std::mutex> lk(s_ownerMtx);
    return s_sharedPaths.find(path) != s_sharedPaths.end();
}

/**
 * @brief 把路径声明为平台共享(三面可达)
 *
 * 须在 Open 前调用;声明后该路径不受 per-port 门禁与归属网限制。
 *
 * @param path  路径,如 "/ping";空串忽略
 */
void ZmHttpServer::MarkShared(const std::string& path)
{
    if (path.empty())
        return;
    std::lock_guard<std::mutex> lk(s_ownerMtx);
    s_sharedPaths.insert(path);
    PUBLIC_LOG_INFO("ZmHttpServer::MarkShared: \"{}\" 声明为平台共享路径(三面可达)", path);
}

/**
 * @brief 业务路由归属校验:归属必须是本面,否则拒绝注册
 *
 * 跨面/游离注册会绕过该面的 per-port 门禁,一律拒绝并打印归属提示;
 * 合法注册会记入 s_routeLog,供 Open 期复检"先注册路由、后声明 root"这类归属漂移。
 *
 * @param path  路由路径
 * @param what  调用方名称(错误日志前缀,如 "RegisterCoro")
 * @return true 归属本面,允许注册;false 拒绝注册(run 后、路径为空或无归属)
 */
bool ZmHttpServer::CheckRouteOwnership(const std::string& path, const char* what)
{
    if (s_state.load() >= ZmRuntimeState::Opened)
    {
        PUBLIC_LOG_ERROR("{}[{}]: run 后不可再注册路由(注册须在 Open 前 Phase1 完成)",
                         what, path);
        return false;
    }
    if (path.empty())
    {
        PUBLIC_LOG_ERROR("{}: 路径为空,拒绝注册", what);
        return false;
    }
    const ZmHttpServer* owner = LookupOwner(path);
    if (owner == this)
    {
        std::lock_guard<std::mutex> lk(s_ownerMtx);
        s_routeLog.emplace_back(this, path);
        return true;
    }
    if (!owner)
        PUBLIC_LOG_ERROR(
            "{}[{}]: 路径不属于任何服务器面(本面 root=\"{}\")——拒绝注册。"
            "请在该路径归属面的 root 下注册,或先为该面 SetRootPath(\"/\" 表示兜底归属)",
            what, path, m_rootPath);
    else
        PUBLIC_LOG_ERROR(
            "{}[{}]: 路径归属 {} 而非本面(本面 root=\"{}\")——拒绝注册:"
            "跨面注册会绕过该面的 per-port 门禁,请改用归属面实例注册",
            what, path, owner->FaceDesc(), m_rootPath);
    return false;
}

/**
 * @brief 归属一致性校验(Open 期一次):任一项失败即拒绝启动
 *
 * 校验三类问题:root 声明冲突(root 重复/多个兜底面)、端口登记冲突(两面同端口)、
 * 已登记路由的归属漂移(注册时归属与现在不一致 —— 说明 root 声明晚于路由注册)。
 * 前两类按**当前声明表**重算,不累积历史:声明改掉即自愈,故"修正配置后可重试 Open"成立。
 * 静默泄漏比启动失败危险得多,故一律 fail-fast。通过后固化只读快照,此后
 * LookupOwner/IsSharedPath/门禁/归属网零锁查表。
 * 注:平台侧不设路由闸门—— 新增平台路由须自行确保落在某个面的 root 下或
 *   `MarkShared`(否则会在所有端口可达);运行期归属网会对"实际被服务且归属不符"
 *   的请求打 `[ROUTE-LEAK]` 告警作为兜底。
 *
 * @return true 校验通过(快照已固化);false 存在冲突,拒绝启动
 */
bool ZmHttpServer::ValidateRouteOwnership()
{
    {
        std::lock_guard<std::mutex> lk(s_ownerMtx);
        size_t bad = 0;

        // ① root 声明冲突(重复 root / 多个兜底面):按当前声明表重算
        {
            std::map<std::string, std::vector<const ZmHttpServer*>, std::less<>> byRoot;
            for (const auto& [face, path] : s_rootClaims)
            {
                if (!path.empty())
                    byRoot[path].push_back(face);
            }
            for (const auto& [path, faces] : byRoot)
            {
                if (faces.size() < 2)
                    continue;
                string who;
                for (const auto* f : faces)
                    who += (who.empty() ? "" : " 与 ") + f->FaceDesc();
                PUBLIC_LOG_ERROR("归属校验失败(root 冲突): root \"{}\" 被 {} 同时声明",
                                 path, who);
                ++bad;
            }
        }

        // ② 端口登记冲突(两面同端口):按当前声明表重算
        {
            std::map<uint16_t, std::vector<const ZmHttpServer*>> byPort;
            for (const auto& [face, port] : s_portClaims)
                byPort[port].push_back(face);
            for (const auto& [port, faces] : byPort)
            {
                if (faces.size() < 2)
                    continue;
                string who;
                for (const auto* f : faces)
                    who += (who.empty() ? "" : " 与 ") + f->FaceDesc();
                PUBLIC_LOG_ERROR("归属校验失败(端口冲突): 端口 {} 被 {} 同时登记",
                                 port, who);
                ++bad;
            }
        }

        // ③ 已登记路由归属复检:防"先注册路由、后声明 root"改变归属
        for (const auto& r : s_routeLog)
        {
            const ZmHttpServer* now = ZmLookupInTable(s_rootOwners, r.second);
            if (now != r.first)
            {
                PUBLIC_LOG_ERROR("归属校验失败: 路由 \"{}\" 注册时归属 {}, 现归属 {}"
                                 "(root 须在路由注册前声明)",
                                 r.second, r.first ? r.first->FaceDesc() : string("无"),
                                 now ? now->FaceDesc() : string("无"));
                ++bad;
            }
        }

        if (bad > 0)
        {
            PUBLIC_LOG_ERROR("ZmHttpServer::Open: 路由归属校验未通过({} 项),拒绝启动", bad);
            return false;
        }

        // 通过:固化只读快照(此后 LookupOwner/IsSharedPath/门禁/归属网零锁)
        auto snap = std::make_shared<ZmOwnerSnapshot>();
        snap->roots = s_rootOwners;
        snap->shared = s_sharedPaths;
        snap->portFace = s_portFace;
        s_ownerSnap.store(std::move(snap), std::memory_order_release);
        PUBLIC_LOG_INFO("ZmHttpServer: 路由归属快照已固化(面 {} 个 / 共享路径 {} 个 / 端口 {} 个)",
                        s_rootOwners.size(), s_sharedPaths.size(), s_portFace.size());
    }

    // JSONP 授权快照同期固化(声明须在 Open 前完成,与归属表同批)
    ZmBuildJsonpSnapshot();
    return true;
}

// ── CORS 白名单 ──

/**
 * @brief 声明 CORS 白名单(须在 Open 前调用,运行期不可改)
 *
 * 与 SetJsonpDefaults/SetRootPath 同款守卫:run 之后调用会被拒绝并记错误日志。
 *
 * @param origins  允许的 Origin 值列表(精确匹配;空列表 = 不放开跨域)
 *
 * @example
 *   api.SetCorsAllowedOrigins({"https://app.example.com"});
 */
void ZmHttpServer::SetCorsAllowedOrigins(const std::vector<std::string>& origins)
{
    if (s_state.load() >= ZmRuntimeState::Opened)
    {
        PUBLIC_LOG_ERROR("SetCorsAllowedOrigins 已跳过(run 后不可改 CORS 白名单)");
        return;
    }
    s_corsOrigins = origins;
}

/**
 * @brief 查询某 Origin 是否在白名单内
 *
 * @param origin  请求的 Origin 头值
 * @return true 命中白名单;false 不在白名单(空串恒为 false)
 */
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

// ── 自动 JSONP ──

// ── 自动 JSONP:全局基线 + 逐路由授权/差量(见文件上方机制说明) ──
/**
 * @brief 设置自动 JSONP 的全局基线
 *
 * 未命中任何 SetJsonpEnabled 前缀的路径按本基线处理。须在 Open 前调用
 * (启动期固化只读快照,运行期不可改)。
 *
 * @param opts  基线选项(参数名候选、错误包装、体积上限、enabled 默认位)
 *
 * @example
 *   ZmHttpServer::ZmJsonpOptions o;
 *   o.enabled = false;            // 只包装显式声明路由(目标形态)
 *   ZmHttpServer::SetJsonpDefaults(o);
 */
void ZmHttpServer::SetJsonpDefaults(const ZmHttpServer::ZmJsonpOptions& opts)
{
    if (s_state.load() >= ZmRuntimeState::Opened)
    {
        PUBLIC_LOG_ERROR("SetJsonpDefaults 已跳过(run 后不可改 JSONP 基线)");
        return;
    }
    std::lock_guard<std::mutex> lk(s_jsonpMtx);
    s_jsonpDefaults = opts;
}

/**
 * @brief 声明某前缀的自动 JSONP 姿态:授权包装 / 列为例外(逐路由安全决策)
 *
 * JSONP 天然绕过 CORS,故"某接口是否允许被任意站点 <script> 读取"必须逐接口声明。
 * 用前缀而非精确路径:路由模式含 {N} 时实际请求路径与声明串不相等。
 * 声明默认启用;`over.enabled = false` 把该前缀记为例外(声明 ≠ 启用),用于全局
 * `enabled = true` 的观察期下单独收口,不必把全局基线翻成 false。
 * 未覆盖的字段继承全局基线;例外与授权同表竞争,最长前缀优先。须在 Open 前调用。
 *
 * @param pathPrefix  路径前缀(段感知,含子路径;空串忽略)
 * @param over        差量覆盖(只写要改的字段,其余继承基线;enabled=false = 排除)
 *
 * @example
 *   ZmHttpServer::ZmJsonpOverride o;
 *   o.maxBodyBytes = 64 * 1024;         // 只改体积上限
 *   ZmHttpServer::SetJsonpEnabled("/api/public", o);
 *   ZmHttpServer::ZmJsonpOverride ex;
 *   ex.enabled = false;                 // 例外:该前缀不包装
 *   ZmHttpServer::SetJsonpEnabled("/zimo/api/secret", ex);
 */
void ZmHttpServer::SetJsonpEnabled(const std::string& pathPrefix,
                                   const ZmHttpServer::ZmJsonpOverride& over)
{
    if (pathPrefix.empty())
    {
        PUBLIC_LOG_ERROR("SetJsonpEnabled: 前缀为空,忽略");
        return;
    }
    if (s_state.load() >= ZmRuntimeState::Opened)
    {
        PUBLIC_LOG_ERROR("SetJsonpEnabled 已跳过(run 后不可再声明): {}", pathPrefix);
        return;
    }
    std::lock_guard<std::mutex> lk(s_jsonpMtx);
    // 基线 + 差量(optional 未设置 = 继承全局基线)
    ZmHttpServer::ZmJsonpOptions o = s_jsonpDefaults;
    if (over.paramNames)
        o.paramNames = *over.paramNames;
    if (over.wrapErrors)
        o.wrapErrors = *over.wrapErrors;
    if (over.maxBodyBytes)
        o.maxBodyBytes = *over.maxBodyBytes;
    // 声明默认启用;enabled=false 显式排除该前缀(声明 ≠ 启用)
    o.enabled = over.enabled.value_or(true);
    for (auto& e : s_jsonpRoutes)
    {
        if (e.first == pathPrefix)
        {
            e.second = o;
            return;
        }
    }
    s_jsonpRoutes.emplace_back(pathPrefix, o);
}

/**
 * @brief 查询自动 JSONP 的全局 enabled 位(诊断/测试用)
 * @return true 表示"未声明路由"的 JSON 响应也会被包装(观察期兼容模式)
 */
bool ZmHttpServer::IsAutoJsonpEnabled()
{
    if (auto snap = s_jsonpSnap.load(std::memory_order_acquire))
        return snap->defaults.enabled;
    std::lock_guard<std::mutex> lk(s_jsonpMtx);
    return s_jsonpDefaults.enabled;
}

/**
 * @brief 校验 JSONP 回调名是否合法(防 XSS 反射)
 *
 * 只允许 [A-Za-z0-9_.] 且长度 ≤128 —— 回调名会被拼进 <script> 响应体,
 * 放宽字符集等于开放注入面。
 *
 * @param cb  回调名(来自 query 参数)
 * @return true 合法;false 不合法(调用方应回 400 而非包装)
 */
namespace
{
bool IsValidJsonpCallback(const std::string& cb)
{
    // 白名单 [A-Za-z0-9_.](防 XSS 反射):长度 ≤128 且全字符合法
    if (cb.empty() || cb.size() > 128)
        return false;
    return std::all_of(cb.begin(), cb.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
    });
}
}  // namespace


// ── 文件与流式传输 ──

// ── 文件传输:Range 解析 + 方案甲/乙 + Hybrid ──
// ----------------------------------------------------------------------------
/**
 * @brief 解析请求的 Range 头(RFC 7233;方案甲/乙共用)
 *
 * 结果三分:①合法单段 → partial(206);②合法但不可满足(起点越界/后缀 0/空文件)
 * → unsatisfiable(416 + Content-Range 通配,即星号斜杠后接总大小);
 * ③语法非法/多段/未知 unit → present=true 且两者皆 false → 忽略并回 200 全文件
 * ( MAY ignore or reject;取 ignore 对多线程下载器更友好, 亦允许 416,
 * 两者皆合规)。
 * 注:drogon 的 newFileResponse 不解析 Range 头,故所有范围语义在此统一实现。
 *
 * @param req       请求(读取 Range 头)
 * @param fileSize  文件总大小(字节)
 * @return 解析结果:present 表示客户端带了 Range 头,partial/unsatisfiable 二选一成立
 */
ZmHttpServer::RangeInfo ZmHttpServer::ParseRange(const HttpRequestPtr& req,
                                                         size_t fileSize)
{
    RangeInfo r;
    string range = req->getHeader("Range");
    if (range.empty())
        return r;                                    // 无 Range → 全文件(present=false)
    r.present = true;

    if (range.rfind("bytes=", 0) != 0)
        return r;                                    // 不认识的 unit → MUST ignore
    string body = range.substr(6);
    // 多段区间(multipart/byteranges)本实现不提供:按 RFC 7233 "MAY ignore" 整体忽略 → 200。
    // 不可只服务首段 —— 请求多段的客户端会按区间拼装,缺段会被当成"这一段没变"而静默出错。
    if (body.find(',') != string::npos)
        return r;                                    // present=true,partial/unsatisfiable=false → 忽略
    size_t dash = body.find('-');
    if (dash == string::npos)
        return r;                                    // 语法非法 → ignore
    string startS = body.substr(0, dash);
    string endS = body.substr(dash + 1);

    if (startS.empty() && endS.empty())
        return r;                                    // "bytes=-" 语法非法 → ignore
    if (!startS.empty() && !std::all_of(startS.begin(), startS.end(),
                                        [](char c) { return std::isdigit((unsigned char)c); }))
        return r;                                    // 语法非法 → ignore
    if (!endS.empty() && !std::all_of(endS.begin(), endS.end(),
                                      [](char c) { return std::isdigit((unsigned char)c); }))
        return r;                                    // 语法非法 → ignore

    try
    {
        if (startS.empty())
        {
            // 后缀范围:bytes=-N(最后 N 字节)
            size_t n = std::stoull(endS);
            if (n == 0 || fileSize == 0)
            {
                r.unsatisfiable = true;              // 零长度后缀/空文件 → 不可满足
                return r;
            }
            n = std::min(n, fileSize);
            r.offset = fileSize - n;
            r.length = n;
        }
        else
        {
            size_t start = std::stoull(startS);
            if (fileSize == 0 || start >= fileSize)
            {
                r.unsatisfiable = true;              // 起点越界 → 416
                return r;
            }
            size_t end = endS.empty() ? fileSize - 1 : std::stoull(endS);
            if (end < start)
                return r;                            // last < first 非法→ ignore
            end = std::min(end, fileSize - 1);
            r.offset = start;
            r.length = end - start + 1;
        }
        r.partial = true;
    }
    catch (...)
    {
        // 数值溢出等解析异常 → ignore(200 全文件)
    }
    return r;
}

/**
 * @brief 构造 416 Range Not Satisfiable 响应
 *
 * @param hasRange  是否在响应中附 Content-Range 头(请求确实带了 Range 头时才附;
 *                  语法非法而被忽略的请求不应回 416)
 * @param fileSize  文件总大小(用于 Content-Range 通配值)
 * @return 416 JSON 错误响应
 */
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

/**
 * @brief 按扩展名取 MIME 类型
 *
 * 表内为固定白名单(静态资源/音视频/文档/字体等);查表按路径最后一段扩展名,
 * 文本类均带 charset=utf-8。
 *
 * @param path  文件路径或文件名(大小写敏感:扩展名须小写)
 * @return MIME 串;未收录的扩展名返回 application/octet-stream
 */
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

// ── 静态资源条件请求(SendFile* 与前端面 SPA 回落共用,行为单源) ──
/**
 * @brief 把 epoch 秒格式化为 RFC 1123 HTTP 日期串
 *
 * @param t  epoch 秒(如文件的 mtime)
 * @return 形如 "Thu, 03 Sep 2026 10:00:00 GMT" 的串;格式化失败返回空串
 */
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

/**
 * @brief 构造 Content-Disposition 头的值(RFC 6266/5987/8187)
 *
 * ASCII 安全名 → filename="..."(剔除控制符/引号/反斜杠,防头结构破坏与注入);
 * 含非 ASCII → 追加 filename*=UTF-8''<percent-encoding>(RFC 5987,浏览器据此正确
 * 显示中文名)。不再交给框架裸拼,故转义规则单源。
 *
 * @param name  下载文件名(可含中文)
 * @return 形如 attachment; filename="a.txt"; filename*=UTF-8''%E4%B8%AD.txt 的头值
 */
static string MakeContentDisposition(const string& name)
{
    string fallback;
    bool ascii = true;
    for (char c : name)
    {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc > 0x7E || c == '"' || c == '\\')
        {
            ascii = false;
            continue;
        }
        fallback += c;
    }
    if (fallback.empty())
        fallback = "download";

    string out = "attachment; filename=\"" + fallback + "\"";
    if (ascii)
        return out;

    // filename*:UTF-8 百分号编码(unreserved = A-Za-z0-9-._~)
    static const char* hex = "0123456789ABCDEF";
    string enc;
    enc.reserve(name.size() * 3);
    for (char c : name)
    {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '.' || c == '_' || c == '~')
            enc += c;
        else
        {
            enc += '%';
            enc += hex[uc >> 4];
            enc += hex[uc & 0xF];
        }
    }
    out += "; filename*=UTF-8''" + enc;
    return out;
}

/**
 * @brief 取文件元信息(单次 stat;条件请求与 Range 共用)
 *
 * file_time(文件时钟) → system_clock(epoch 秒):以两个时钟各自的 now 为桥换算,
 * 避免直接假定两时钟同源。
 *
 * @param path  文件路径(UTF-8 编码)
 * @return 元信息:found/sizeFailed/size/mtimeSec(任一 stat 失败时对应标志位置位)
 */
ZmHttpServer::ZmFileMeta ZmHttpServer::FetchFileMeta(const string& path)
{
    ZmFileMeta m;
    // filesystem 的窄串按 ANSI 码页解码,UTF-8 路径必错——
    // 先 UTF-8 → wide 再进 filesystem(与上传侧 CP_UTF8 契约一致)
    const std::wstring wpath = ZmString::UTF8_To_Unicode(path);
    if (wpath.empty() && !path.empty())
        return m;
    // 单次 GetFileAttributesExW 取齐 存在性/大小/修改时间:静态资源热路径每请求
    // 省掉两次内核调用(旧实现 exists / file_size / last_write_time 各一次)
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad))
        return m;                                    // 不存在或无权访问 → found=false(调用方 404)
    m.found = true;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        m.sizeFailed = true;   // 目录不是可发送文件(保持旧语义:调用方回 500)
        return m;
    }
    ULARGE_INTEGER sz{};
    sz.LowPart = fad.nFileSizeLow;
    sz.HighPart = fad.nFileSizeHigh;
    m.size = static_cast<size_t>(sz.QuadPart);
    // FILETIME(1601-01-01 起算,单位 100ns)→ Unix 秒
    ULARGE_INTEGER ft{};
    ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;   // 1601→1970 的 100ns 数
    if (ft.QuadPart >= kEpochDiff100ns)
        m.mtimeSec = static_cast<int64_t>((ft.QuadPart - kEpochDiff100ns) / 10000000ULL);
    return m;
}

/**
 * @brief 生成缓存头对(200/206/304 共用)
 *
 * ETag 为强验证器:文件大小与 mtime 的组合,二者任一变化即视为新版本。
 *
 * @param m  文件元信息
 * @return first = Last-Modified(RFC 1123 日期串),second = 强 ETag(形如 "12345-1757654321")
 */
pair<string, string> ZmHttpServer::CacheHeaders(const ZmFileMeta& m)
{
    // ETag 前缀(如条目 id):区分 size/mtime 恰好相同的不同条目
    string etag = "\"";
    if (!m.key.empty())
        etag += m.key + "-";
    etag += std::to_string(m.size) + "-" + std::to_string(m.mtimeSec) + "\"";
    return {HttpDateStr(m.mtimeSec), std::move(etag)};
}

/**
 * @brief 条件请求判定:命中缓存即生成 304 响应
 *
 * If-None-Match 优先(命中 → 304;带了但未命中则不再看 If-Modified-Since);
 * If-Modified-Since 兜底(文件未修改 → 304);日期非法一律视为未提供。
 * If-None-Match 按逗号列表逐项匹配(去 W/ 前缀与首尾引号后精确相等),
 * 不做整串子串匹配,防 ETag 前缀误命中。
 *
 * @param req           请求(读取 If-None-Match / If-Modified-Since)
 * @param m             文件元信息(mtimeSec 用于 If-Modified-Since 比较)
 * @param cacheHeaders  CacheHeaders 生成的 (Last-Modified, ETag) 对
 * @return 非空 = 304 响应(调用方直接返回);nullptr = 继续正常 200/206 流程
 *
 * @example
 *   auto cacheHeaders = CacheHeaders(m);
 *   if (auto notMod = Maybe304(req, m, cacheHeaders))
 *       co_return notMod;          // 命中缓存,无需再读文件
 */
namespace
{
/**
 * @brief 归一化 ETag 片段:去掉 W/ 弱标记与首尾引号,便于按 RFC 7232 做弱比较
 *
 * 容错目的:curl 等客户端经 -H 传 If-None-Match 时会剥掉引号,
 * 若直接用带引号的 etag 匹配会漏命中(客户端拿到 200 而非 304)。
 *
 * @param tag 原始 ETag 片段(可能带 W/ 前缀或引号)
 * @return 归一化后的标签
 */
string StripEtagTag(string tag)
{
    if (tag.size() >= 2 && (tag[0] == 'W' || tag[0] == 'w') && tag[1] == '/')
        tag = tag.substr(2);
    if (tag.size() >= 2 && tag.front() == '"' && tag.back() == '"')
        tag = tag.substr(1, tag.size() - 2);
    return tag;
}
}  // namespace

drogon::HttpResponsePtr ZmHttpServer::Maybe304(const HttpRequestPtr& req,
                                               const ZmFileMeta& m,
                                               const pair<string, string>& cacheHeaders)
{
    const string& lastMod = cacheHeaders.first;
    const string& etag = cacheHeaders.second;
    string inm = req->getHeader("If-None-Match");
    if (!inm.empty())
    {
        // RFC 7232:If-None-Match 弱比较(逗号列表、W/ 前缀、entity-tag 本身带引号)。
        // 容错:比较前去 W/ 前缀与首尾引号——curl 等客户端经 -H 传值时会剥掉引号,
        // 若直接用带引号的 etag 做匹配会漏命中(客户端拿到 200 而非 304)。
        string etagN = StripEtagTag(etag);
        bool hit = (inm == "*");
        if (!hit && !etagN.empty())
        {
            size_t pos = 0;
            while (pos <= inm.size())
            {
                size_t comma = inm.find(',', pos);
                string tag =
                    inm.substr(pos, comma == string::npos ? string::npos : comma - pos);
                // 去项内空白后精确比较(弱比较:忽略 W/ 前缀)
                size_t b = tag.find_first_not_of(" \t");
                size_t e = tag.find_last_not_of(" \t");
                tag = (b == string::npos) ? string()
                                          : tag.substr(b, e - b + 1);
                if (!tag.empty() && StripEtagTag(tag) == etagN)
                {
                    hit = true;
                    break;
                }
                if (comma == string::npos)
                    break;
                pos = comma + 1;
            }
        }
        if (hit)
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

bool ZmHttpServer::IfRangeAllowsPartial(const HttpRequestPtr& req,
                                        const pair<string, string>& cacheHeaders)
{
    string v = req->getHeader("If-Range");
    if (v.empty())
        return true;   // 未带 If-Range:Range 照常处理
    // 强 ETag 形态(带引号)才按 ETag 比;弱 ETag(W/"…")按 RFC 不得用于 If-Range,
    // 落到日期分支必然不等 → 判为不匹配,符合规范意图
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        return v == cacheHeaders.second;
    return v == cacheHeaders.first;
}

// ── SendFileCoro —— 文件下载"方案甲": ──
//   直接使用 drogon::HttpResponse::newFileResponse(内部为 trantor sendFile,
//   零拷贝分段读盘入发送缓冲),Range/206/Content-Length/Accept-Ranges 语义由框架兜底。
//   适用:常规文件(< Hybrid 阈值);要求简洁、带宽内无额外延迟。
//   边界:无字节级水位(慢客户端背压依赖 trantor 输出缓冲 O1)。
//   条件请求:Last-Modified(mtime)+ 强 ETag(size-mtime) → If-None-Match 优先、
//   If-Modified-Since 兜底 → 304(缓存节流,配合前端静态资源显著省流量)。
/**
 * @brief 发送文件 —— 方案甲:零拷贝分段响应
 *
 * 先做一次 stat,把元信息转交 SendFileCoroImpl(与 Hybrid 入口共用,避免重复 stat)。
 *
 * @param req             请求(取条件请求头与 Range)
 * @param path            文件绝对路径(UTF-8 编码)
 * @param attachmentName  非空 = 以附件下载并设置该文件名;空 = 内联展示
 * @return 文件响应(200 全文件 / 206 部分 / 304 未修改 / 404 不存在 / 500 stat 失败 / 416)
 *
 * @example
 *   api.RegisterCoro("/dl/{id}", Get,
 *       [](HttpRequestPtr req) -> drogon::Task<HttpResponsePtr> {
 *           co_return co_await api.SendFileCoro(req, "D:/data/a.bin", "a.bin");
 *       });
 */
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

/**
 * @brief 方案甲内部实现:由已取得的文件元信息组装响应
 *
 * 供 SendFileCoro 与 SendFileHybridCoro 复用(避免二次 stat)。
 *
 * @param req             请求(取条件请求头与 Range)
 * @param path            文件绝对路径(UTF-8 编码)
 * @param attachmentName  非空 = 以附件下载
 * @param fileSize        文件总大小(字节,调用方已 stat)
 * @param mtimeSec        文件修改时间(epoch 秒,调用方已 stat)
 * @param etagKey         ETag 前缀(空 = 仅 size-mtime);须与方案乙同源
 * @return 文件响应(200/206/304/416)
 */
drogon::Task<HttpResponsePtr> ZmHttpServer::SendFileCoroImpl(const HttpRequestPtr& req,
                                                             const string& path,
                                                             const string& attachmentName,
                                                             size_t fileSize,
                                                             int64_t mtimeSec,
                                                             const string& etagKey)
{
    ZmFileMeta m;                       // 元信息由公开入口/Hybrid 已取,此处只组装
    m.size = fileSize;
    m.mtimeSec = mtimeSec;
    // ETag 前缀与方案乙同源:否则同一文件在 Hybrid 阈值两侧拿到不同 ETag,
    // 缓存验证器与 If-Range 判定会随路径不同而打架
    m.key = etagKey;

    // ① 条件请求(304 无 body);命中即返回,不再走 Range/200
    auto cacheHeaders = CacheHeaders(m);
    if (auto notMod = Maybe304(req, m, cacheHeaders))
        co_return notMod;

    // ② 解析 Range 头(共用解析器):合法区间 → partial(206);合法但越界
    //    → unsatisfiable(416);语法非法 → ignore(200 全文件,RFC 7233 )
    RangeInfo r = ParseRange(req, fileSize);
    // If-Range 不匹配 → 整个 Range 作废,按整份发(否则旧副本的区间会拼到新内容上)
    if (!IfRangeAllowsPartial(req, cacheHeaders))
        r = RangeInfo{};
    if (r.unsatisfiable)
    {
        co_return Range416Response(true, fileSize);
    }

    // ③ 构造文件响应:
    //    partial(有合法 Range)→ offset/length 只发区间,setContentRange 由下面手动加头;
    //    全文件 → offset=0,length=fileSize;
    //    attachmentName 非空 → 经 MakeContentDisposition 统一写头(转义 + RFC 5987,
    //    不再交给框架裸拼)。
    //    ⚠ setContentRange 传 false:统一由本函数显式写 Content-Range,避免框架/手动双写
    //    (drogon 的 newFileResponse 不解析 Range 头,区间语义全部由本函数承担)
    HttpResponsePtr resp = HttpResponse::newFileResponse(
        path, r.offset, r.partial ? r.length : fileSize,
        /*setContentRange=*/false, /*attachmentName=*/"", CT_NONE, "", req);

    // ④ 缓存头(与 304 一致:客户端代理/浏览器核对条件)
    resp->addHeader("Last-Modified", cacheHeaders.first);
    resp->addHeader("ETag", cacheHeaders.second);
    // ⑤ 告知客户端支持断点续传(Range 请求有效依据)
    resp->addHeader("Accept-Ranges", "bytes");
    // ⑥ 部分内容:206 + Content-Range: bytes {offset}-{offset+length-1}/{fileSize}
    if (r.partial)
    {
        resp->setStatusCode(k206PartialContent);
        resp->addHeader("Content-Range",
                        "bytes " + std::to_string(r.offset) + "-" +
                            std::to_string(r.offset + r.length - 1) + "/" +
                            std::to_string(fileSize));
    }
    // ⑦ 下载文件名(浏览器另存为;转义 + RFC 5987 编码)
    if (!attachmentName.empty())
        resp->addHeader("Content-Disposition", MakeContentDisposition(attachmentName));
    co_return resp;
}

// ── SendFileStreamCoro —— 文件下载"方案乙": ──
//   newAsyncStreamResponse 分块流式(Transfer-Encoding: chunked,不设 Content-Length),
//   块间定时器节流:预读窗口 = 1 块 → 内存有界(慢客户端缓冲不随时长线性涨)。
//   读盘走专用 I/O 线程池(HttpIoPool),事件循环线程绝不阻塞(NFR);
//   支持停滞放弃(stallAbortMs,客户端 Range 续传)与进度回调(onProgress)。
//   适用:≥Hybrid 阈值的大文件、需要进度回调/长连接稳定性控制的场景。
//   条件请求:同方案甲(304 为普通响应,不在流内)。
/**
 * @brief 发送文件 —— 方案乙:定时器链分块流式响应
 *
 * 先做一次 stat,把元信息转交 SendFileStreamCoroImpl。适用大文件与慢客户端:
 * 预读窗口为 1 块,内存有界;可经 opts 配置进度回调与停滞放弃。
 *
 * @param req             请求(取条件请求头与 Range)
 * @param path            文件绝对路径(UTF-8 编码)
 * @param attachmentName  非空 = 以附件下载并设置该文件名
 * @param opts            分块粒度/块间间隔/停滞放弃阈值/进度回调
 * @return 流式响应(200/206/304/404/500/416)
 *
 * @example
 *   ZmHttpSendFileOptions o;
 *   o.chunkSize = 4 * 1024 * 1024;      // 每块 4MB
 *   o.stallAbortMs = 60 * 1000;         // 对端停滞 60s 即放弃
 *   co_return co_await api.SendFileStreamCoro(req, path, "big.zip", o);
 */
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

/**
 * @brief 方案乙内部实现:由已取得的文件元信息组装流式响应
 *
 * 供 SendFileStreamCoro 与 SendFileHybridCoro 复用(避免二次 stat)。响应不带
 * Content-Length(chunked 编码),故下载进度只能由客户端按已收字节估算。
 *
 * @param req             请求(取条件请求头与 Range)
 * @param path            文件绝对路径(UTF-8 编码)
 * @param attachmentName  非空 = 以附件下载
 * @param opts            分块粒度/块间间隔/停滞放弃阈值/进度回调
 * @param fileSize        文件总大小(字节,调用方已 stat)
 * @param mtimeSec        文件修改时间(epoch 秒,调用方已 stat)
 * @return 流式响应(200/206/304/416)
 */
drogon::Task<HttpResponsePtr> ZmHttpServer::SendFileStreamCoroImpl(
    const HttpRequestPtr& req, const string& path, const string& attachmentName,
    const ZmHttpSendFileOptions& opts, size_t fileSize, int64_t mtimeSec)
{
    // ① 条件请求(304 无 body);命中即返回,不再走流式
    ZmFileMeta m;
    m.size = fileSize;
    m.mtimeSec = mtimeSec;
    m.key = opts.etagKey;   // 可选:让 ETag 带上条目身份,避免跨条目碰撞
    auto cacheHeaders = CacheHeaders(m);
    if (auto notMod = Maybe304(req, m, cacheHeaders))
        co_return notMod;

    // ② Range 解析(同方案甲共用):合法区间 → partial;合法但越界 → 416;
    //    语法非法 → ignore(200 全文件,RFC 7233 )
    RangeInfo r = ParseRange(req, fileSize);
    // If-Range 不匹配 → 整个 Range 作废,按整份发(同方案甲)
    if (!IfRangeAllowsPartial(req, cacheHeaders))
        r = RangeInfo{};
    if (r.unsatisfiable)
    {
        co_return Range416Response(true, fileSize);
    }

    // ③ 流式响应工厂:回调在发送启动时(事件循环线程)被框架调用,
    //    在此把 文件路径/区间/行为参数 注入发送状态机,并立即 Run() 驱动第一块;
    //    true = disableKickoffTimeout:禁用 trantor 默认启动超时(大文件/长流不被误杀)
    HttpResponsePtr resp = HttpResponse::newAsyncStreamResponse(
        [path, fileSize, r, opts, attachmentName, req](ResponseStreamPtr stream) mutable {
            auto st = std::make_shared<ZmStreamLoopState>();   // 状态机对象(持所有权)
            st->path = path;
            st->offset = r.offset;                              // Range 起点(续传定位)
            st->total = r.partial ? r.length : fileSize;       // 本次发送总量(区间或全文件)
            st->remaining = st->total;                          // 剩余待发字节
            st->abortMs = opts.stallAbortMs;                    // 停滞放弃阈值(默认 120s)
            st->opts = opts;
            st->stream = std::move(stream);                     // 排他持有流(close 由状态机负责)
            // 停滞判定信号源:连接弱引用 → bytesSent 差值反映对端真实消费
            st->connWk = req->getConnectionPtr();
            st->Run();                                          // 打开文件句柄并调度第一块
        },
        true);

    // ④ 响应头:断点续传声明 + MIME
    //    默认(分块)模式无 Content-Length,走 chunked 编码;
    //    raw 模式由本函数显式写 content-length 界定正文长度(drogon 见到该头就不再加 chunked)
    resp->addHeader("Accept-Ranges", "bytes");
    // 用 setContentTypeString 而非 addHeader:框架另有一个 contentType_ 成员也会输出
    // content-type(默认 text/plain),只往 headers_ 里追加会出现两个 Content-Type ——
    // 对 .zip 这类非文本文件会同时下发 text/plain 与 application/zip,客户端可能取错
    resp->setContentTypeString(MimeForExt(path));
    if (opts.raw)
        resp->addHeader("content-length",
                        std::to_string(r.partial ? r.length : fileSize));
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
    // ⑦ 下载文件名(浏览器另存为;转义 + RFC 5987 编码)
    if (!attachmentName.empty())
    {
        resp->addHeader("Content-Disposition", MakeContentDisposition(attachmentName));
    }
    co_return resp;
}

// ── SendFileHybridCoro —— 文件下载"便捷入口": ──
//   按文件大小自动路由:fileSize < threshold → 方案甲 SendFileCoro(常规,零拷贝)
//                        fileSize ≥ threshold → 方案乙 SendFileStreamCoro(流式,内存有界)
//   阈值默认 2GB(调用方可覆盖;0 = 恒乙);不关心细节的业务层直接用本入口。
/**
 * @brief 发送文件 —— 便捷入口:按文件大小自动选择方案甲/乙
 *
 * 不关心传输细节的业务层直接用本入口;stat 只做一次,再按阈值路由到对应内部实现。
 *
 * @param req             请求(取条件请求头与 Range)
 * @param path            文件绝对路径(UTF-8 编码)
 * @param attachmentName  非空 = 以附件下载
 * @param threshold       切换阈值(字节):小于走方案甲,达到及以上走方案乙;
 *                        传 0 = 恒走方案乙
 * @param streamOpts      方案乙的分块/节流参数(走方案甲时忽略)
 * @return 文件响应(200/206/304/404/500/416)
 *
 * @example
 *   // 默认 2GB 阈值:小文件零拷贝、大文件内存有界
 *   co_return co_await api.SendFileHybridCoro(req, path, "包.zip");
 */
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
        co_return co_await SendFileCoroImpl(req, path, attachmentName, m.size, m.mtimeSec,
                                            streamOpts.etagKey);
    co_return co_await SendFileStreamCoroImpl(req, path, attachmentName, streamOpts,
                                              m.size, m.mtimeSec);
}

// ── 流式工厂 ──
/**
 * @brief 构造流式响应
 *
 * 返回 newAsyncStreamResponse 响应,状态码与响应头由调用方自行设置。
 *
 * @param cb              流对象回调(在事件循环线程被调用,拿到流后即可驱动发送)
 * @param disableKickoff  true = 关闭 trantor 默认启动超时
 *                        (长流或"业务线程先启动"的场景必备,否则会被框架误杀)
 * @return 流式响应
 *
 * @example
 *   auto resp = ZmHttpServer::MakeStreamResponse(
 *       [](ResponseStreamPtr stream) { stream->send("hello"); stream->close(); });
 *   resp->addHeader("Content-Type", "text/plain");
 *   co_return resp;
 */
HttpResponsePtr ZmHttpServer::MakeStreamResponse(StreamCb cb, bool disableKickoff)
{
    return HttpResponse::newAsyncStreamResponse(std::move(cb), disableKickoff);
}

// ── 响应助手 ──

// ── 响应助手(业务层使用 ZMJSON,输出为构造序) ──


/**
 * @brief 构造统一 JSON 响应
 *
 * 裸 JSON 体(业务语义),状态码由参数指定;ZMJSON 直接序列化,输出保持构造序。
 *
 * @param status  HTTP 状态码
 * @param data    响应体(ZMJSON)
 * @return application/json 响应
 *
 * @example
 *   ZMJSON d;
 *   d["ok"] = true;                       // 输出键序 = 构造序
 *   co_return ZmHttpServer::JsonResponse(200, d);
 */
HttpResponsePtr ZmHttpServer::JsonResponse(int status, const ZMJSON& data)
{
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<HttpStatusCode>(status));
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(data.dump());
    return resp;
}

/**
 * @brief 构造统一错误响应
 *
 * 错误包形态 {error:{code,message}},与前端 auth.js 的解析约定一致。
 *
 * @param status  HTTP 状态码(同时写入 error.code)
 * @param msg     错误说明(写入 error.message)
 * @return JSON 错误响应
 *
 * @example
 *   co_return ZmHttpServer::ErrorResponse(401, "unauthorized");
 */
HttpResponsePtr ZmHttpServer::ErrorResponse(int status, const string& msg)
{
    ZMJSON error;
    error["error"]["code"] = status;
    error["error"]["message"] = msg;
    return JsonResponse(status, error);
}

/**
 * @brief 构造显式 JSONP 响应
 *
 * 有合法 callback → cb(json);(application/javascript);无 callback → 常规 JSON;
 * callback 名非法 → 400。与自动 JSONP(PreSending advice,授权/行为逐路由声明，
 * 的区别:本函数由业务显式调用,不依赖路由声明。
 *
 * @param req   请求(取 callback 参数)
 * @param data  响应体
 * @return application/javascript 或 application/json 响应;callback 非法时为 400
 */
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

/**
 * @brief drogon(Json::Value)→ 业务 ZMJSON(递归转换)
 *
 * ⚠ 对象键序按 Json::Value 内部的 map 字典序;需要构造序时请业务侧直接用 ZMJSON
 * 构造,不要经本转换再序列化。
 *
 * @param v  jsoncpp 值
 * @return 等价的 ZMJSON(未知类型按 null 处理)
 */
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

/**
 * @brief 业务 ZMJSON → drogon(Json::Value)(递归转换)
 *
 * 仅在 drogon API 明确要求 jsoncpp 值的场合使用(如等价 loadConfigJson 的场景);
 * 业务链路无需接触 jsoncpp。
 *
 * @param v  ZMJSON 值
 * @return 等价的 Json::Value(无法归类的类型按 null 处理)
 */
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

// ── 页面/文件/跳转响应助手(业务/平台边界) ──
//   动机:业务层若自行 newFileResponse 组装页面响应,会漏掉 Last-Modified/ETag/304
//   (宣告的"页面 304"随之在真实路径上失效);平台侧另有 SPA 回落实现
//   (AddSpaFallback,无调用者)形成双实现。本组助手收敛职责:业务给"发哪个文件/
//   跳哪去",平台负责条件请求与缓存语义 —— 与 SendFile*/SPA 回落共用同一份 helper
//   (行为单源)。
/**
 * @brief 页面/小文件响应(带条件请求语义)
 *
 * 单次 stat 后生成响应,并附 Last-Modified + 强 ETag;条件命中走 Maybe304。
 * Cache-Control 由调用方自行设置(平台不覆盖)。
 * ⚠ 不带 Range:大文件/下载/断点续传请走 SendFileCoro / SendFileHybridCoro。
 *
 * @param req       请求(取条件请求头)
 * @param filePath  文件路径(UTF-8 编码)
 * @return 200 文件响应 / 304 未修改 / 404 不存在或 stat 失败(记 WARN)
 *
 * @example
 *   // SPA 回落:找不到页面时回 index.html
 *   co_return ZmHttpServer::FileResponse(req, "D:/www/index.html");
 */
HttpResponsePtr ZmHttpServer::FileResponse(const HttpRequestPtr& req, const string& filePath)
{
    // 单次 stat:存在性/大小/mtime(失败区分 404/500 语义,见 ZmFileMeta)
    ZmFileMeta m = FetchFileMeta(filePath);
    if (!m.found || m.sizeFailed)
    {
        PUBLIC_LOG_WARN("FileResponse: 文件不可用(不存在/stat 失败): {}", filePath);
        return NotFoundResponse(req);
    }

    // 条件请求(增强):命中 → 304 无 body(与 SendFile* 同一判定)
    auto cacheHeaders = CacheHeaders(m);
    if (auto notMod = Maybe304(req, m, cacheHeaders))
        return notMod;

    auto resp = HttpResponse::newFileResponse(filePath);
    resp->addHeader("Last-Modified", cacheHeaders.first);
    resp->addHeader("ETag", cacheHeaders.second);
    return resp;
}

/**
 * @brief 跳转响应(默认 302 临时跳转)
 *
 * @param url     目标 URL(可相对可绝对,原样写入 Location 头)
 * @param status  状态码;需要永久跳转语义时显式传 301(303/307/308 同理)
 * @return 重定向响应
 *
 * @example
 *   co_return ZmHttpServer::RedirectResponse("https://example.com/", 301);
 */
HttpResponsePtr ZmHttpServer::RedirectResponse(const string& url, int status)
{
    return HttpResponse::newRedirectionResponse(url,
                                               static_cast<HttpStatusCode>(status));
}

/**
 * @brief 构造 404 响应
 *
 * @param req  请求;当前线程为服务器 loop 且已 SetNotFoundPage 时回自定义 404 页,
 *             否则回 drogon 内置 404 页(该参数可为空)
 * @return 404 响应
 */
HttpResponsePtr ZmHttpServer::NotFoundResponse(const HttpRequestPtr& req)
{
    return HttpResponse::newNotFoundResponse(req);
}

// ── 限流 ──

// ── 限流(drogon RateLimiter 底层;全部 SafeRateLimiter 线程安全包装) ──
//   overlay 规则:COW 快照(原子 shared_ptr 读零锁;写=copy+swap,低频)
//   per-IP 桶:全局容器锁保护 map(每次请求一次短临界查找;isAllowed 无锁)
//   专项额度桶:挂在规则上(随规则存亡;一 IP 一桶,跨面共享)
namespace
{
/**
 * @brief 构造线程安全的限流器(SafeRateLimiter 包装)
 *
 * 裸 RateLimiter 非线程安全,多事件循环线程共用一个桶时必须经此包装。
 *
 * @param type      限流算法(固定窗口/滑动窗口/令牌桶等)
 * @param capacity  时间单位内允许的次数
 * @param timeSec   时间单位(秒)
 * @return 包装后的限流器指针
 */
drogon::RateLimiterPtr MakeSafeRateLimiter(drogon::RateLimiterType type,
                                           size_t capacity, double timeSec)
{
    return std::make_shared<drogon::SafeRateLimiter>(
        drogon::RateLimiter::newRateLimiter(
            type, capacity, std::chrono::duration<double>(timeSec)));
}

/// 专项额度槽:一个 IP 一条 Quota 规则对应一个槽,规则删除即随之释放
class ZmQuotaSlot
{
public:
    /**
     * @brief 记录建桶参数(桶本身惰性建立)
     * @param cap      时间单位内允许的次数
     * @param timeSec  时间单位(秒)
     */
    ZmQuotaSlot(size_t cap, double timeSec) : m_cap(cap), m_timeSec(timeSec) {}

    /**
     * @brief 取本槽的桶(未建立则按 type 建立一次,此后复用同一桶)
     *
     * 算法 type 是逐实例配置而规则是进程级的,故由首个建桶者定型:同进程内给同一 IP
     * 的额度配两套算法自相矛盾,先到先得即可。
     *
     * @param type  限流算法(仅首次建立时使用)
     * @return 桶指针(线程安全包装,可跨事件循环线程使用)
     */
    drogon::RateLimiterPtr Get(drogon::RateLimiterType type)
    {
        if (auto lim = m_lim.load(std::memory_order_acquire))
            return lim;
        std::lock_guard lk(m_buildMtx);
        if (auto lim = m_lim.load(std::memory_order_acquire))
            return lim;   // 双检:并发首访只建一个桶
        auto lim = MakeSafeRateLimiter(type, m_cap, m_timeSec);
        m_lim.store(lim, std::memory_order_release);
        return lim;
    }

private:
    std::atomic<drogon::RateLimiterPtr> m_lim{nullptr};   ///< 已建立的桶(空 = 未建立)
    size_t     m_cap = 0;        ///< 时间单位内允许的次数
    double     m_timeSec = 0;    ///< 时间单位(秒)
    std::mutex m_buildMtx;       ///< 仅首次建立桶时进入
};

/// overlay 规则条目
struct ZmOverlayRule
{
    enum Kind { None, Blocked, Allowed, Quota } kind = None;
    size_t capacity = 0;
    double timeSec = 0;
    /// 专项额度桶槽:kind == Quota 时必非空(由 OverlayWrite 保证);随规则存亡
    std::shared_ptr<ZmQuotaSlot> quota;
};
using ZmOverlayMap = std::unordered_map<string, ZmOverlayRule>;

/// 进程级 overlay(COW):原子指针读、写时整体替换
std::atomic<std::shared_ptr<const ZmOverlayMap>> s_overlayPtr{
    std::make_shared<const ZmOverlayMap>()};
std::mutex s_overlayWriteMtx;

/**
 * @brief 写 overlay 规则(COW:复制当前表 → 修改 → 原子替换)
 *
 * 低频写路径,故直接整体复制;读侧因此可零锁取快照。
 * 专项额度槽的生死与规则一致:删除规则即释放槽(份额状态不残留),参数未变的
 * Quota 改写则沿用原槽(额度状态跨改写延续)。
 *
 * @param ip        对端 IP 字面量
 * @param kind      规则类型(None = 删除该 IP 的规则)
 * @param capacity  额度桶容量(kind = Quota 时有效)
 * @param timeSec   额度时间单位秒(kind = Quota 时有效)
 */
void OverlayWrite(const string& ip, ZmOverlayRule::Kind kind,
                  size_t capacity, double timeSec)
{
    std::lock_guard lk(s_overlayWriteMtx);
    auto cur = s_overlayPtr.load();
    auto nxt = std::make_shared<ZmOverlayMap>(*cur);
    if (kind == ZmOverlayRule::None)
    {
        nxt->erase(ip);   // 槽随条目一并释放(最后一份快照释放时桶析构)
    }
    else
    {
        ZmOverlayRule rule{kind, capacity, timeSec, nullptr};
        if (kind == ZmOverlayRule::Quota)
        {
            auto old = cur->find(ip);
            if (old != cur->end() && old->second.kind == ZmOverlayRule::Quota &&
                old->second.capacity == capacity && old->second.timeSec == timeSec)
                rule.quota = old->second.quota;   // 参数未变 → 沿用(状态延续)
            if (!rule.quota)
                rule.quota = std::make_shared<ZmQuotaSlot>(capacity, timeSec);
        }
        (*nxt)[ip] = std::move(rule);
    }
    s_overlayPtr.store(nxt);
}

}  // namespace

/**
 * @brief 创建线程安全的限流器(供业务自建 per-route 限流使用)
 *
 * @param type         限流算法
 * @param capacity     时间单位内允许的次数
 * @param timeUnitSec  时间单位(秒)
 * @return 限流器指针(SafeRateLimiter 包装,可跨事件循环线程使用)
 */
drogon::RateLimiterPtr ZmHttpServer::CreateRateLimiter(drogon::RateLimiterType type,
                                                       size_t capacity,
                                                       double timeUnitSec)
{
    return MakeSafeRateLimiter(type, capacity, timeUnitSec);
}

/**
 * @brief 封禁某 IP(命中即 429,优先于默认桶)
 * @param ip  对端 IP 字面量
 */
void ZmHttpServer::SetIpBlocked(const string& ip)
{
    OverlayWrite(ip, ZmOverlayRule::Blocked, 0, 0);
}
/**
 * @brief 解除某 IP 的封禁
 * @param ip  对端 IP 字面量
 */
void ZmHttpServer::UnblockIp(const string& ip)
{
    OverlayWrite(ip, ZmOverlayRule::None, 0, 0);
}
/**
 * @brief 为某 IP 设置专项额度桶(覆盖默认桶参数)
 * @param ip           对端 IP 字面量
 * @param capacity     时间单位内允许的次数
 * @param timeUnitSec  时间单位(秒)
 */
void ZmHttpServer::SetIpQuota(const string& ip, size_t capacity, double timeUnitSec)
{
    OverlayWrite(ip, ZmOverlayRule::Quota, capacity, timeUnitSec);
}
/**
 * @brief 把某 IP 加入白名单(跳过限流)
 * @param ip  对端 IP 字面量
 */
void ZmHttpServer::SetIpAllowed(const string& ip)
{
    OverlayWrite(ip, ZmOverlayRule::Allowed, 0, 0);
}
/**
 * @brief 移除某 IP 的 overlay 规则(回落到默认桶)
 * @param ip  对端 IP 字面量
 */
void ZmHttpServer::RemoveRateRule(const string& ip)
{
    OverlayWrite(ip, ZmOverlayRule::None, 0, 0);
}
/**
 * @brief 查询某 IP 是否已有 overlay 规则(诊断用)
 * @param ip  对端 IP 字面量
 * @return true 存在覆盖规则(封禁/白名单/专项额度);false 无
 */
bool ZmHttpServer::IsRateRuleHit(const string& ip)
{
    auto ov = s_overlayPtr.load();
    return ov->find(ip) != ov->end();
}

// ----------------------------------------------------------------------------
// ZmIpRateLimiter:逐 IP 桶协调器(每个 IP 独立桶;有界 + LRU 驱逐)
//   默认桶:key = 来访 IP(攻击者可控)→ 必须有界;驱逐须按"最久未使用"而非插入序,
//   否则攻击者轮换假 IP 就能把活跃用户的桶挤出,桶重建 = 计数清零 = 放行一波。
//   专项桶:key = 有 Quota 规则的 IP(管理动作可控)→ 挂在规则上,随规则存亡
// ----------------------------------------------------------------------------
struct ZmHttpServer::ZmIpRateLimiter::Impl
{
    drogon::RateLimiterType type = drogon::RateLimiterType::kFixedWindow;
    size_t cap = 0;
    double timeSec = 0;
    size_t maxEntries = 10000;

    std::mutex mtx;   // 短临界:桶查找/创建/驱逐(仅事件循环线程批;耗时微秒级)
    std::unordered_map<string, drogon::RateLimiterPtr> buckets;
    std::list<string> lru;   // 队首 = 最久未使用(list 迭代器稳定,才能 O(1) 提升)
    std::unordered_map<string, std::list<string>::iterator> lruPos;
    // 注:专项额度桶不在本容器 —— 它挂在 overlay 规则上(见 ZmQuotaSlot),
    //     条目数恒等于 Quota 规则数,不需要也不该有驱逐(驱逐 = 重置额度 = 放行一波)

    /**
     * @brief 取(或惰性创建)某 IP 的默认桶
     *
     * 命中即提升到 LRU 队尾;桶数达到 maxEntries 时驱逐最久未使用者(防 IP 无限
     * 增长导致内存泄漏)。提升与驱逐均为 O(1)(list::splice + 迭代器表)。
     * 桶创建为低频操作(每 IP 首个请求一次),故在锁内惰性创建。
     *
     * @param ip  对端 IP 字面量
     * @return 该 IP 的限流器(线程安全包装)
     */
    drogon::RateLimiterPtr BucketFor(const string& ip)
    {
        std::lock_guard lk(mtx);
        auto it = buckets.find(ip);
        if (it != buckets.end())
        {
            // 命中即提升:不提升就退化成 FIFO,轮换假 IP 可挤掉活跃桶
            auto pos = lruPos.find(ip);
            if (pos != lruPos.end())
                lru.splice(lru.end(), lru, pos->second);
            return it->second;
        }
        if (buckets.size() >= maxEntries)
        {
            // 驱逐最久未使用(队首);迭代器表与桶表必须同步摘除
            while (!lru.empty())
            {
                string old = std::move(lru.front());
                lru.pop_front();
                lruPos.erase(old);
                if (buckets.erase(old) > 0)
                    break;
            }
        }
        auto lim = MakeSafeRateLimiter(type, cap, timeSec);
        buckets.emplace(ip, lim);
        lru.emplace_back(ip);
        lruPos[ip] = std::prev(lru.end());
        return lim;
    }
};

/**
 * @brief 创建逐 IP 桶协调器(私有构造,只能经此获得实例)
 *
 * @param type        限流算法(所有默认桶共用)
 * @param capacity    每桶在时间单位内允许的次数
 * @param timeUnitSec 时间单位(秒)
 * @param maxEntries  IP 桶数量上限;传入 0 会被夹到 1(超出按插入序驱逐最旧)
 * @return 协调器实例
 */
std::shared_ptr<ZmHttpServer::ZmIpRateLimiter>
ZmHttpServer::ZmIpRateLimiter::Create(drogon::RateLimiterType type, size_t capacity,
                                      double timeUnitSec, size_t maxEntries)
{
    auto r = std::shared_ptr<ZmIpRateLimiter>(new ZmIpRateLimiter());
    r->m_impl = std::make_shared<Impl>();
    r->m_impl->type = type;
    r->m_impl->cap = capacity;
    r->m_impl->timeSec = timeUnitSec;
    r->m_impl->maxEntries = maxEntries > 0 ? maxEntries : 1;
    return r;
}

/**
 * @brief 取 X-Forwarded-For 首跳 IP(仅 Options.trustProxyHeaders 开启时使用)
 *
 * 语义:最左项 = 原始客户端(右侧各项由中间代理追加)。只接受"非空、长度 ≤64、
 * 仅含 [0-9A-Za-z.:]"的串;任何畸形一律返回空串,由调用方退回对端 IP。
 *
 * @param req 请求
 * @return 首跳 IP 字面量;无该头或格式非法返回空串
 */
static string FirstForwardedFor(const HttpRequestPtr& req)
{
    string xff = req->getHeader("X-Forwarded-For");
    if (xff.empty())
        return {};
    size_t comma = xff.find(',');
    if (comma != string::npos)
        xff.resize(comma);
    // 去首尾空白(代理常写成 "1.2.3.4, 5.6.7.8")
    size_t b = xff.find_first_not_of(" \t");
    if (b == string::npos)
        return {};
    size_t e = xff.find_last_not_of(" \t");
    string first = xff.substr(b, e - b + 1);
    if (first.size() > 64)
        return {};
    for (char c : first)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':'))
            return {};
    }
    return first;
}

/**
 * @brief 组合执行限流判定:overlay 规则优先,未命中走默认 per-IP 桶
 *
 * 判定顺序:封禁 → 429;白名单 → 放行;专项额度 → 独立桶(规则参数变更即重建);
 * 其余 IP 走默认桶。key 取值来源见 Options.trustProxyHeaders。
 *
 * @param req   请求(取对端 IP / X-Forwarded-For)
 * @param resp  出参:被限流时写入 429 响应
 * @return true 放行;false 已拒绝(此时 resp 必非空)
 *
 * @example
 *   // 作为 filter 使用:拒绝时把 resp 交给框架
 *   api.AddFilter("rate", [&lim](const HttpRequestPtr& req, HttpResponsePtr& resp) {
 *       return lim->Check(req, resp);
 *   });
 */
bool ZmHttpServer::ZmIpRateLimiter::Check(const drogon::HttpRequestPtr& req,
                                          drogon::HttpResponsePtr& resp)
{
    // 限流 key:默认只认对端 IP —— 反代/负载均衡后置时全体客户端会同桶,
    // 需经 Options.trustProxyHeaders 显式开启 X-Forwarded-For(该头可伪造,
    // 只在入口已被可信代理覆写时才可信任)。
    string ip;
    if (s_trustProxy.load(std::memory_order_relaxed))
        ip = FirstForwardedFor(req);
    if (ip.empty())
        ip = req->getPeerAddr().toIp();
    if (ip.empty())
        return true;   // 无对端信息(异常态)不拦

    auto ov = s_overlayPtr.load();
    auto it = ov->find(ip);
    if (it != ov->end())
    {
        switch (it->second.kind)
        {
        case ZmOverlayRule::Blocked:
            resp = ErrorResponse(429, "rate limited");   // 引用入参:无条件赋值(doFilter 判非空)
            return false;
        case ZmOverlayRule::Allowed:
            return true;
        case ZmOverlayRule::Quota:
        {
            // 专项额度:桶挂在规则上(进程内一 IP 一桶,稳态零锁)
            if (it->second.quota->Get(m_impl->type)->isAllowed())
                return true;
            resp = ErrorResponse(429, "rate limited");
            return false;
        }
        default:
            break;
        }
    }
    auto lim = m_impl->BucketFor(ip);
    if (lim->isAllowed())
        return true;
    resp = ErrorResponse(429, "rate limited");
    return false;
}

// ── 线程池 ──

/**
 * @brief 取三面共享的静态阻塞工作池
 *
 * RunOnPool 的唯一目的地。容量为进程级全局(经 SetWorkPoolSize 注入),
 * 首次调用即定型;同一进程内所有 HTTP 面共用,不与业务自建线程池混用。
 *
 * @return 进程级唯一实例的引用
 */
ZmThreadPool& ZmHttpServer::WorkPool()
{
    static ZmThreadPool pool(static_cast<uint16_t>(s_workPoolSize.load()),
                             "DrogonHttp-Worker");   // 首次调用即定型
    return pool;
}

/**
 * @brief 设置业务阻塞工作池的线程数
 *
 * DB/磁盘/CPU 型 handler 在该池排队执行。须在首次 RunOnPool 前调用
 * (静态池首次使用即定型),高并发可调大。
 * 切勿设 0:ZmThreadPool 按该数创建工作线程,0 线程 = 任务永不执行、业务卡死。
 *
 * @param n  工作线程数
 *
 * @example
 *   ZmHttpServer::SetWorkPoolSize(16);   // 在 Init 前调用
 */
void ZmHttpServer::SetWorkPoolSize(size_t n)
{
    s_workPoolSize.store(n);
}

/**
 * @brief 取业务阻塞工作池的线程数
 * @return 当前配置的线程数(未显式设置时为默认值)
 */
size_t ZmHttpServer::GetWorkPoolSize()
{
    return s_workPoolSize.load();
}
