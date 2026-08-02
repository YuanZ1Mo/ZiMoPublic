#include "zm_net_req_loop.h"

#include "zm_net_http.h"
#include "zm_logger.h"
#include "zm_util_libevent.h"

#include <chrono>   // ZmReqLoopPool 排队轮询片(steady_clock)
#include <windows.h>

namespace {
enum { REQ_LOOP_CTRL_EXIT = 0x0100 };
int64_t NowMs() { return (int64_t)::GetTickCount64(); }
}

ZmReqLoop::ZmReqLoop()
    : ZmThread("ReqLoop")
    , m_evbase(nullptr), m_sigEvent(nullptr), m_ctrlEvent(nullptr), m_deadlineEvent(nullptr)
    , m_looped(false), m_run_finished(false)
    , m_task(nullptr)
    , m_epoch(0)
    , m_replied(false), m_cancelled(false)
    , m_pool(nullptr)
{
}

ZmReqLoop::~ZmReqLoop()
{
}

bool ZmReqLoop::Loop()
{
    std::unique_lock<std::mutex> lock(m_mutexLoop);
    if (!m_looped)
    {
        Start();
        // m_run_finished 粘滞:池 Shutdown 后本 loop 不再被重新 Loop();粘滞无害,仅文档化
        m_cvLoop.wait(lock, [this] { return m_looped || m_run_finished; });
    }
    return m_looped;
}

void ZmReqLoop::Stop()
{
    // 参考 ZmEvBaseRunLoop:OnStopping 由基类 Stop 调用,内部投递 EXIT
    ZmThread::Stop();
}

void ZmReqLoop::PostToLoop(int signal, void* ctx, std::function<void(void*)> deleter)
{
    std::unique_lock<std::mutex> lock(m_mutexLoop);   // 与 Run 退出路径互斥
    if (!m_looped)
    {
        // 约束:deleter 不得重入本 loop(PostToLoop/Release),否则持锁死锁;当前调用方均满足
        if (deleter) deleter(ctx);   // loop 已退出:同步释放,所有权契约不变
        return;
    }
    auto* p = new Post{ signal, m_epoch.load(), ctx, std::move(deleter) };
    {
        std::lock_guard<std::mutex> lockPost(m_mutexPost);
        m_postQueue.push_back(p);
    }
    event_active(m_sigEvent, signal, 0);   // 锁内:Run 不可能同时释放 m_sigEvent
}

bool ZmReqLoop::TryReply()
{
    return !m_replied.exchange(true);
}

void ZmReqLoop::CancelDeadline()
{
    if (m_deadlineEvent)
        event_del(m_deadlineEvent);
}

void ZmReqLoop::Release()
{
    if (m_task == nullptr)   // 本请求已释放或从未绑定:幂等返回
        return;
    ClearRequestState();
    if (m_pool)
        ZmReqLoopPoolReturn(m_pool, this);   // 经自由函数回池(ZmReqLoopPool 完整类型在 Task 4)
}

void ZmReqLoop::Run()
{
    zm_util_eventbase_init();

    m_evbase = event_base_new();
    if (!m_evbase)
    {
        PUBLIC_LOG_ERROR("{}: event_base_new failed", GetName());
        {
            std::lock_guard<std::mutex> lock(m_mutexLoop);
            m_run_finished = true;
        }
        m_cvLoop.notify_one();
        return;
    }

    m_sigEvent = event_new(m_evbase, -1, EV_PERSIST | EV_READ, ZmReqLoop::OnSignalCB, (void*)this);
    event_add(m_sigEvent, 0);

    m_ctrlEvent = event_new(m_evbase, -1, EV_PERSIST | EV_READ, ZmReqLoop::OnCtrlCB, (void*)this);
    event_add(m_ctrlEvent, 0);

    m_deadlineEvent = event_new(m_evbase, -1, EV_TIMEOUT, ZmReqLoop::OnDeadlineCB, (void*)this);

    {
        std::lock_guard<std::mutex> lock(m_mutexLoop);
        m_looped = true;
    }
    m_cvLoop.notify_one();

    PUBLIC_LOG_INFO("{}: event loop running", GetName());
    event_base_loop(m_evbase, EVLOOP_NO_EXIT_ON_EMPTY);
    PUBLIC_LOG_INFO("{}: event loop exited", GetName());

    {
        std::lock_guard<std::mutex> lock(m_mutexLoop);
        m_looped = false;
        m_run_finished = true;
    }
    m_cvLoop.notify_one();

    // 退出:先排空残留投递包(Post 析构执行 deleter,统一释放 ctx),再清理事件
    {
        std::lock_guard<std::mutex> lock(m_mutexPost);
        for (auto* p : m_postQueue) delete p;   // Post 析构执行 deleter,释放 ctx
        m_postQueue.clear();
    }
    if (m_deadlineEvent) { event_free(m_deadlineEvent); m_deadlineEvent = nullptr; }
    if (m_ctrlEvent)     { event_free(m_ctrlEvent);     m_ctrlEvent = nullptr; }
    if (m_sigEvent)      { event_free(m_sigEvent);      m_sigEvent = nullptr; }
    if (m_evbase)        { event_base_free(m_evbase);   m_evbase = nullptr; }
}

void ZmReqLoop::OnStopping()
{
    if (m_ctrlEvent)
        event_active(m_ctrlEvent, REQ_LOOP_CTRL_EXIT, 0);
}

void ZmReqLoop::OnCtrlCB(evutil_socket_t /*fd*/, short what, void* arg)
{
    auto* self = static_cast<ZmReqLoop*>(arg);
    what &= 0x7F00;
    if ((what & REQ_LOOP_CTRL_EXIT) && self->m_evbase)
        event_base_loopexit(self->m_evbase, nullptr);
}

void ZmReqLoop::OnSignalCB(evutil_socket_t /*fd*/, short /*what*/, void* arg)
{
    static_cast<ZmReqLoop*>(arg)->DispatchSignals();
}

void ZmReqLoop::OnDeadlineCB(evutil_socket_t /*fd*/, short /*what*/, void* arg)
{
    static_cast<ZmReqLoop*>(arg)->ProcessDeadline();
}

void ZmReqLoop::DispatchSignals()
{
    while (true)
    {
        Post* p;
        {
            std::lock_guard<std::mutex> lock(m_mutexPost);
            if (m_postQueue.empty())
                return;
            p = m_postQueue.front();
            m_postQueue.pop_front();
        }
        if (p->epoch != m_epoch.load())   // 陈旧事件(本请求已释放)
        {
            delete p;
            continue;
        }
        switch (p->signal)
        {
        case REQ_LOOP_SIG_START:    ProcessStart(*p); break;
        case REQ_LOOP_SIG_CLOSE:    ProcessClose(*p); break;
        case REQ_LOOP_SIG_TIMEOUT:  ProcessDeadline(); break;
        case REQ_LOOP_SIG_RESPONSE: ProcessResponse(*p); break;
        case REQ_LOOP_SIG_DONE:     ProcessDone(*p);  break;
        default: break;  // 未定义信号,忽略
        }
        delete p;
    }
}

void ZmReqLoop::ProcessStart(Post& p)
{
    auto* ctx = static_cast<StartCtx*>(p.ctx);

    // Bind(仅 A 线程触碰 per-request 状态)
    m_task = ctx->task;
    m_handlers = std::move(ctx->handlers);
    m_replied.store(false);
    m_cancelled.store(false);

    int64_t remainMs = ctx->deadlineMs - NowMs();
    if (remainMs <= 0) remainMs = 1;
    timeval tv = { (long)(remainMs / 1000), (int)((remainMs % 1000) * 1000) };

    // 绑定前客户端已断(closecb 早于 START):直接走 close 默认收尾
    if (m_task->IsConnClosed())
    {
        // START 包的 ctx 是 StartCtx*,构造 ctx=本请求 task 的临时 Post 使 ProcessClose 身份校验通过
        Post close = { REQ_LOOP_SIG_CLOSE, m_epoch.load(), m_task, {} };
        ProcessClose(close);
        return;
    }
    event_add(m_deadlineEvent, &tv);

    // 拷贝而非 move:ProcessClose/ProcessDeadline 仍需读取 onClose/onTimeout
    Handlers h = m_handlers;
    if (h.onStart)
        h.onStart(this);
}

void ZmReqLoop::ProcessClose(Post& p)
{
    if (p.ctx != m_task)   // 陈旧投递:非当前请求的 CLOSE(跨代),丢弃
        return;
    m_cancelled.store(true);

    // 业务清理回调(可选)
    Handlers h = m_handlers;
    if (h.onClose)
        h.onClose(this);

    // 默认收尾:驱动 doer 回收(流式已开 → EndStreamReply;否则 TriggerReply 丢弃)
    if (m_task && TryReply())
    {
        if (m_task->IsStreaming())
            m_task->EndStreamReply();
        else
            m_task->TriggerReply();
    }
    Release();
}

void ZmReqLoop::ProcessResponse(Post& p)
{
    // ctx = 回传数据(堆分配,由 onResponse 续体消费;正常路径由 DispatchSignals 的 delete p(Post 析构 deleter)
    // 在续体返回后释放,续体不负责释放;epoch 丢弃路径同样由 Post 析构 deleter 释放)
    if (p.ctx != nullptr)
    {
        m_responseCtx = p.ctx;   // 供续体经 GetResponseCtx() 取用
        Handlers h = m_handlers;
        if (h.onResponse)
            h.onResponse(this);
        m_responseCtx = nullptr;
    }
    // 注意:续体通常在本回调内回复并 Release;回调返回后不得再触碰 per-request 状态
}

void ZmReqLoop::ProcessDeadline()
{
    m_cancelled.store(true);

    Handlers h = m_handlers;
    if (h.onTimeout)
    {
        h.onTimeout(this);   // 业务自定义超时(须自行 TryReply + Release)
        return;
    }

    // 缺省:504 收尾
    if (m_task && TryReply())
    {
        m_task->SetReply(504, "Request Timeout");
        m_task->TriggerReply();
    }
    Release();
}

void ZmReqLoop::ProcessDone(Post& p)
{
    if (p.ctx != m_task)   // 陈旧投递:非当前请求的 DONE,丢弃
        return;
    // 外部线程已结束流式(EndStreamReply 已驱动 doer 回收),此处只收回复门
    TryReply();
    Release();
}

void ZmReqLoop::ClearRequestState()
{
    m_task = nullptr;
    m_handlers = {};
    m_responseCtx = nullptr;
    m_replied.store(false);
    m_cancelled.store(false);
    if (m_deadlineEvent)
        event_del(m_deadlineEvent);
    m_epoch.fetch_add(1);
    OnRequestReleased();   // 子类钩子:清理 per-request 私有成员(如 m_reply)
}

// ============================================================================
// ZmReqLoopPool
// ============================================================================

namespace {
/// 排队轮询片:醒来复查 abort 标志与 deadline
constexpr auto kPollSlice = std::chrono::milliseconds(50);
}

ZmReqLoopPool::ZmReqLoopPool()
    : m_maxCount(0), m_budgetMs(5000), m_shutdown(false)
{
}

ZmReqLoopPool::~ZmReqLoopPool()
{
    Shutdown();
}

bool ZmReqLoopPool::Init(int preCreate, int maxCount, uint32_t businessBudgetMs)
{
    m_maxCount = maxCount;
    m_budgetMs = businessBudgetMs;

    for (int i = 0; i < preCreate; ++i)
    {
        auto* loop = m_factory ? m_factory() : new ZmReqLoop();
        loop->SetPool(this);
        if (!loop->Loop())
        {
            PUBLIC_LOG_ERROR("ZmReqLoopPool::Init failed: ZmReqLoop::Loop()");
            delete loop;   // 本次创建失败的 loop(未入容器)
            // 失败路径清理:停掉并释放已创建的 loop,复位容器,保证失败后池可重试 Init
            for (auto* created : m_all)
            {
                created->Stop();
                delete created;
            }
            m_all.clear();
            m_idle.clear();
            return false;
        }
        m_all.push_back(loop);
        m_idle.push_back(loop);
    }
    PUBLIC_LOG_INFO("ZmReqLoopPool::Init: preCreate={}, maxCount={}, budgetMs={}",
        preCreate, maxCount, businessBudgetMs);
    return true;
}

ZmReqLoop* ZmReqLoopPool::Acquire(int timeoutMs, const std::atomic<bool>* abort)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (!m_shutdown)
    {
        if (!m_idle.empty())
        {
            auto* loop = m_idle.back();
            m_idle.pop_back();
            return loop;
        }
        // deadline/abort 检查先于扩容:请求已过期或客户端已断,不再为其创建线程
        if (std::chrono::steady_clock::now() >= deadline || (abort && abort->load()))
            return nullptr;
        if ((int)m_all.size() < m_maxCount)   // 扩容
        {
            // 持锁安全的依据:新线程只触碰自己的 m_mutexLoop/m_cvLoop,不触碰池锁;
            // Release 在 Loop() 返回后才可能发生(此时本分支已 push 进 m_all/m_idle 完成)。
            auto* loop = m_factory ? m_factory() : new ZmReqLoop();
            loop->SetPool(this);
            if (!loop->Loop())
            {
                PUBLIC_LOG_ERROR("ZmReqLoopPool::Acquire: loop start failed");
                delete loop;
                return nullptr;
            }
            m_all.push_back(loop);
            return loop;
        }
        m_cv.wait_for(lock, kPollSlice);   // 50ms 片,醒来复查 abort
    }
    return nullptr;
}

void ZmReqLoopPool::Release(ZmReqLoop* loop)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_idle.push_back(loop);
    }
    m_cv.notify_one();
}

void ZmReqLoopPool::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown)
            return;
        m_shutdown = true;
    }
    m_cv.notify_all();

    for (auto* loop : m_all)
        loop->Stop();          // join 等待 ZmReqLoop 线程退出
    for (auto* loop : m_all)
        delete loop;
    m_all.clear();
    m_idle.clear();
}

// ============================================================================
// ZmReqLoopPoolReturn — 桥接定义(ZmReqLoop::Release 经自由函数回池)
// 本文件已包含完整 ZmReqLoopPool 定义,函数保持自由函数形态以便头文件仅前向声明。
// ============================================================================

void ZmReqLoopPoolReturn(ZmReqLoopPool* pool, ZmReqLoop* loop)
{
    pool->Release(loop);
}
