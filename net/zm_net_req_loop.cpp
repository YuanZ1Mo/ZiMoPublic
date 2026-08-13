#include "zm_net_req_loop.h"

#include "zm_net_req_loop_pool.h"   // ZmReqLoopPoolReturn(Release 回池桥声明)
#include "zm_net_http.h"
#include "../util/zm_util_logger.h"
#include "../util/zm_util_libevent.h"

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
    auto* p = new Post{ signal, m_epoch.load(), ctx, std::move(deleter), {} };
    {
        std::lock_guard<std::mutex> lockPost(m_mutexPost);
        m_postQueue.push_back(p);
    }
    event_active(m_sigEvent, signal, 0);   // 锁内:Run 不可能同时释放 m_sigEvent
}

void ZmReqLoop::PostToLoop(std::function<void(ZmReqLoop*)> fn)
{
    std::unique_lock<std::mutex> lock(m_mutexLoop);   // 与 Run 退出路径互斥
    if (!m_looped)
        return;   // loop 已退出:投递丢弃(fn 及其捕获数据随 Post 未创建而释放)
    auto* p = new Post{ REQ_LOOP_SIG_EXEC, m_epoch.load(), nullptr, {}, std::move(fn) };
    {
        std::lock_guard<std::mutex> lockPost(m_mutexPost);
        m_postQueue.push_back(p);
    }
    event_active(m_sigEvent, REQ_LOOP_SIG_EXEC, 0);   // 锁内:Run 不可能同时释放 m_sigEvent
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

void ZmReqLoop::SetDeadline(int64_t timeoutMs)
{
    // event_del/event_add 内部持 base 锁(evthread_use_windows_threads 开启后),堆层面跨线程安全;
    // 但 m_task/m_deadlineEvent 的生命周期仅本线程可保证(请求释放/loop 退出窗口),故仍约定
    // 仅本线程调用;外部线程请用 PostToLoop 桥接。请求已释放(m_task 空)或参数非法时保持原状。
    if (m_deadlineEvent == nullptr || m_task == nullptr || timeoutMs <= 0)
        return;
    m_cancelled.store(false);   // 重设即撤销本次超时取消(超时处理内延长后续体不再提前退出)
    timeval tv = { (long)(timeoutMs / 1000), (int)((timeoutMs % 1000) * 1000) };
    event_del(m_deadlineEvent);
    event_add(m_deadlineEvent, &tv);
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
        // ★ 业务异常隔离:业务回调(onStart/onResponse/onTimeout/onClose/exec)抛出的
        //   C++ 异常在此捕获(对齐 worker 线程池纪律),按 500 兜底收尾当前请求,
        //   loop 继续服务下一个请求——单个请求的业务 bug 不拖垮整个服务进程。
        //   (注意:SEH 级崩溃(访问违例)不转 C++ 异常,由进程级 SCM Recovery 兜底)
        try
        {
            switch (p->signal)
            {
            case REQ_LOOP_SIG_START:    ProcessStart(*p); break;
            case REQ_LOOP_SIG_CLOSE:    ProcessClose(*p); break;
            case REQ_LOOP_SIG_TIMEOUT:  ProcessDeadline(); break;
            case REQ_LOOP_SIG_RESPONSE: ProcessResponse(*p); break;
            case REQ_LOOP_SIG_DONE:     ProcessDone(*p);  break;
            case REQ_LOOP_SIG_EXEC:     ProcessExec(*p);  break;
            default: break;  // 未定义信号,忽略
            }
        }
        catch (const std::exception& e)
        {
            PUBLIC_LOG_ERROR("{}: 业务回调异常(已隔离): {}", GetName(), e.what());
            OnRequestAborted();
        }
        catch (...)
        {
            PUBLIC_LOG_ERROR("{}: 业务回调未知异常(已隔离)", GetName());
            OnRequestAborted();
        }
        delete p;
    }
}

void ZmReqLoop::ProcessStart(Post& p)
{
    auto* ctx = static_cast<StartCtx*>(p.ctx);

    // Bind(仅 ZmReqLoop 线程触碰 per-request 状态)
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
        h.onTimeout(this);   // 业务自定义超时:须自行收尾(TryReply + Release)或 SetDeadline 延长继续
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

void ZmReqLoop::OnRequestAborted()
{
    // 业务异常后的兜底收尾(对齐 ProcessClose 默认收尾):
    // 异常只跳出业务代码,ZmReqLoop 自身状态机(m_task/m_replied/epoch)未破坏,
    // 业务侧半成品资源由 RAII 清理——可按 500 收尾当前请求并回池,loop 继续服务。
    // 若门已被业务拿走(异常发生在组装中途),不回复,仅 Release(客户端超时兜底)。
    if (m_task && TryReply())
    {
        if (m_task->IsStreaming())
            m_task->EndStreamReply();   // 流式已开:结束流,驱动 doer 回收
        else
        {
            m_task->SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR, "Internal Error");
            m_task->TriggerReply();
        }
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

void ZmReqLoop::ProcessExec(Post& p)
{
    // epoch 校验已在 DispatchSignals 完成:能执行到这里说明请求未收尾(未 Release)
    if (p.exec)
        p.exec(this);   // 回复 helper 的组装/发送/收尾(见 zm_net_req_loop_protocol.cpp)
    // 注意:exec 内通常投 DONE 收尾;exec 返回后不得再触碰 per-request 状态
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
