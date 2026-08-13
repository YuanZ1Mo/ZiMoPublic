#ifndef ZM_NET_REQ_LOOP_H
#define ZM_NET_REQ_LOOP_H

#include "../util/zm_util_thread.h"

#include <../libevent/include/event2/event.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

// 前向声明(头文件中仅通过指针使用)
struct event_base;
class ZmReqLoopPool;
class ZmHttpdTask;

/** @brief JRPC 业务回调(loop 为本请求的 ZmReqLoop 实例)
 *  @param loop    本请求的 ZmReqLoop 实例(回复经 ZmReqLoopJrpc::ResponseJson(loop, rsp))
 *  @param reqData 请求 JSON 字符串(仅在回调期间有效) */
using ZmReqLoopJrpcRequestCB = std::function<void(class ZmReqLoop*, const char* reqData)>;

/** @brief RESTful 业务回调(loop 为本请求的 ZmReqLoop 实例)
 *  @param loop    本请求的 ZmReqLoop 实例(经 ZmReqLoopRest::Response* 回复)
 *  @param body    请求体字节指针(指向请求 evbuffer,回复前有效,勿拷贝后使用)
 *  @param body_len 请求体长度 */
using ZmReqLoopRestfulRequestCB = std::function<void(class ZmReqLoop*, const BYTE* body, size_t body_len)>;

/**
 * @brief per-request 事件循环线程(类似 ZmEvBaseRunLoop 的 event_base 驱动线程)
 *
 * 事件驱动:业务 = 入口回调 + 续体回调,全部在本线程(event_base)上执行;
 * close/超时/外部返回均以事件(PostToLoop)送达本线程。per-request 状态仅本线程触碰。
 *
 * 生命周期:池 Acquire → PostToLoop(START) → 本线程 ProcessStart(Bind + onStart)
 * → 业务分段推进 → 回复 helper(TryReply + task 直通 + Release) → 回池。
 * 回收纪律:Release() 后不得再碰 task;epoch++ 使队列中陈旧事件失效。
 */
class ZmReqLoop : public ZmThread
{
public:
    // 桥事件信号(PostToLoop 的 what 值,跨线程 event_active 使用)
    enum {
        REQ_LOOP_SIG_START    = 0x0001,   ///< 业务入口(ProcessStart 内完成 Bind + onStart)
        REQ_LOOP_SIG_CLOSE    = 0x0002,   ///< 客户端断开(close 通知器投递;投递时 ctx 必须为请求的 ZmHttpdTask*)
        REQ_LOOP_SIG_TIMEOUT  = 0x0004,   ///< deadline 到期(定时器投递,同线程)
        REQ_LOOP_SIG_RESPONSE = 0x0008,   ///< 外部请求响应到达(PostToLoop 投递,ctx=回传数据,onResponse 续体消费)
        REQ_LOOP_SIG_DONE     = 0x0010,   ///< 业务收尾(外部线程完成流式后投递,见音频模块;投递时 ctx 必须为请求的 ZmHttpdTask*)
        REQ_LOOP_SIG_EXEC     = 0x0020,   ///< 通用执行投递(PostToLoop(fn),fn 在 ZmReqLoop 线程执行;回复 helper 收敛用)
    };

    /** @brief per-request 业务回调 */
    struct Handlers
    {
        std::function<void(ZmReqLoop*)> onStart;    ///< 业务入口(Bind 后调用,必填)
        std::function<void(ZmReqLoop*)> onClose;    ///< 客户端断开清理(可选,调用后仍执行默认收尾)
        std::function<void(ZmReqLoop*)> onTimeout;  ///< 超时处理(可选,缺省 504 收尾)
        std::function<void(ZmReqLoop*)> onResponse; ///< 外部请求响应续体(可选;PostToLoop(REQ_LOOP_SIG_RESPONSE) 触发,ctx 经 GetResponseCtx() 取)
    };

    /** @brief START 投递包(堆分配,由本线程消费) */
    struct StartCtx
    {
        ZmHttpdTask* task;           ///< 请求上下文(回复直通路径)
        int64_t      deadlineMs;     ///< 绝对截止时间(毫秒,GetTickCount64,自请求到达起算)
        Handlers     handlers;       ///< 业务回调
    };

    ZmReqLoop();
    virtual ~ZmReqLoop();

    /** @brief 启动事件循环线程并等待就绪(参考 ZmEvBaseRunLoop::Loop) */
    bool Loop();
    /** @brief 停止事件循环线程(OnStopping → event_active EXIT,内部 join) */
    void Stop();

    /** @brief 任意线程:投递信号到本 loop
     *  @param signal  REQ_LOOP_SIG_* 之一
     *  @param ctx     堆分配业务负载(START 用 StartCtx;RESPONSE 用回传数据),由 deleter 释放
     *  @param deleter ctx 的释放函数;本线程消费或 epoch 丢弃时都会调用 */
    void PostToLoop(int signal, void* ctx = nullptr, std::function<void(void*)> deleter = {});

    /** @brief 任意线程:投递执行函数到本 loop 线程(回复 helper 收敛用)
     *  @param fn 在 ZmReqLoop 线程执行;投递到执行之间请求已收尾(epoch 变化)则静默丢弃
     *  @note loop 已退出时静默丢弃(fn 及捕获数据随 Post 未创建而释放);
     *        fn 内可安全触碰 task/per-request 状态(关闭时 join 保证执行先于销毁) */
    void PostToLoop(std::function<void(ZmReqLoop*)> fn);

    // ── 仅本线程(业务回调内调用)──
    /** @brief 是否已取消(超时/断连置位;续体回调先查此标志,置位则立即退出) */
    bool          IsCancelled() const { return m_cancelled.load(); }
    /** @brief 获取请求上下文(回复直通;Release 前有效) */
    ZmHttpdTask*  Task() const { return m_task; }
    /** @brief 回复所有权原子门:true=本调用取得回复权;false=已被他人回复
     *  @note 原子跨线程可调用(音频/SSE 外部线程收尾先取门),但调用方须确保 loop 仍属本请求
     *        (断连后 loop 可能已被回收复用——微窗口内抢门后果为单请求静默无响应(客户端侧超时兜底),非损坏) */
    bool          TryReply();

    /** @brief 服务器关闭中标志:ZmReqLoopPool::Shutdown 置位后,回复 helper 先查此门
     *  @note 缩窄外部线程在关闭窗口内调 Response* 的 UAF 窗口(门只拦"关闭已开始"的调用;
     *        关闭完成后调用任何 helper 仍属契约外,须业务层 gone 标志兜底) */
    bool          IsClosing() const { return m_closing.load(); }
    /** @brief 置位关闭标志(仅 ZmReqLoopPool::Shutdown 调用,join 前置位) */
    void          MarkClosing() { m_closing.store(true); }
    /** @brief 取消 deadline 定时器(流式开始后自动调用,见 ZmReqLoopRest) */
    void          CancelDeadline();
    /** @brief 重设 deadline 定时器(约定:仅本线程,业务回调/续体/超时处理内调用):
     *  从当前时刻重新计时,并撤销已置位的超时取消标志
     *  @param timeoutMs 新超时毫秒(>0);<=0 忽略(原 deadline 保持不动)
     *  @note 在超时处理(SetTimeoutHandler 回调)内调用可"到期后延长继续":
     *        返回后请求不再走缺省 504 收尾,续体经 IsCancelled() 判假继续推进 */
    void          SetDeadline(int64_t timeoutMs);
    /** @brief 注册客户端断开清理回调(仅本线程,业务入口内调用)
     *  @param cb 连接关闭时于本线程同步调用的清理函数(如取消外部请求、关闭句柄);
     *            调用后仍执行默认收尾(回复门 + Release 驱动回收),回调内勿调 Release() */
    void SetCloseHandler(std::function<void(ZmReqLoop*)> cb) { m_handlers.onClose = std::move(cb); }
    /** @brief 注册超时处理(仿 SetCloseHandler;缺省 504 收尾,注册后由业务自行收尾) */
    void SetTimeoutHandler(std::function<void(ZmReqLoop*)> cb)
    {
        m_handlers.onTimeout = std::move(cb);
    }
    /** @brief 业务在 onResponse 续体内取回传数据(PostToLoop(RESPONSE, ctx) 的 ctx) */
    void* GetResponseCtx() const { return m_responseCtx; }

    /** @brief 注册外部响应续体(仿 SetCloseHandler,per-request 存储) */
    void SetResponseHandler(std::function<void(ZmReqLoop*)> cb)
    {
        m_handlers.onResponse = std::move(cb);
    }
    /** @brief 清 per-request 状态、epoch++、回池;调用后不得再触碰本对象业务数据 */
    void          Release();

    /** @brief 池归属(由 ZmReqLoopPool::Init 设置) */
    void SetPool(ZmReqLoopPool* pool) { m_pool = pool; }

protected:
    virtual void Run() override;
    virtual void OnStopping() override;

    /** @brief 请求状态清空钩子(ClearRequestState 末尾调用,仅本线程);
     *  子类覆写以清理 per-request 私有成员(如 ZmReqLoopRest 流式状态) */
    virtual void OnRequestReleased() {}

private:
    struct Post
    {
        int                       signal;
        uint64_t                  epoch;    ///< 投递时 ZmReqLoop 的代际
        void*                     ctx;
        std::function<void(void*)> deleter;
        std::function<void(ZmReqLoop*)> exec;   ///< REQ_LOOP_SIG_EXEC 执行函数(捕获回复参数数据)
        ~Post() { if (deleter) deleter(ctx); }   ///< 丢弃/消费统一释放
    };

    static void OnSignalCB(evutil_socket_t fd, short what, void* arg);
    static void OnDeadlineCB(evutil_socket_t fd, short what, void* arg);
    static void OnCtrlCB(evutil_socket_t fd, short what, void* arg);

    void DispatchSignals();          ///< 弹出全部投递包,验 epoch 后分发
    void ProcessStart(Post& p);      ///< Bind + deadline 定时器 + onStart
    void ProcessClose(Post& p);      ///< onClose(可选)→ 默认收尾(置 cancelled + 回复门 + 回收驱动)
    void ProcessResponse(Post& p);   ///< 外部响应续体(onResponse;ctx 经 GetResponseCtx 取)
    void ProcessDeadline();          ///< onTimeout(可选)→ 缺省 504
    void ProcessDone(Post& p);       ///< 外部线程流式收尾:TryReply + Release
    void ProcessExec(Post& p);       ///< 通用执行投递:执行 p.exec(this)
    void OnRequestAborted();         ///< 业务异常隔离兜底:当前请求 500 收尾 + Release(loop 继续服务)
    void ClearRequestState();        ///< 清 task/handlers、del deadline、epoch++

    struct event_base* m_evbase;      ///< 本线程的事件循环
    struct event*      m_sigEvent;    ///< 常驻桥事件(PostToLoop 入口)
    struct event*      m_ctrlEvent;   ///< 退出控制事件
    struct event*      m_deadlineEvent; ///< per-request 定时器(START 时 add,Release 时 del)
    std::mutex         m_mutexLoop;
    std::condition_variable m_cvLoop;
    bool               m_looped;
    bool               m_run_finished;   ///< Run() 已退出/失败(Run() 返回后永不再执行)

    std::mutex         m_mutexPost;
    std::deque<Post*>  m_postQueue;   ///< 投递包队列(任意线程入队,本线程弹出)

    // ── per-request(本线程独占;ProcessStart 写入,Release 清空)──
    ZmHttpdTask*       m_task;
    Handlers           m_handlers;
    void*              m_responseCtx = nullptr;  ///< onResponse 续体的 ctx(仅 ZmReqLoop 线程)
    std::atomic<uint64_t> m_epoch;    ///< 代际:Release 时 ++;投递包携带投递时快照
    std::atomic<bool>  m_replied;
    std::atomic<bool>  m_cancelled;
    std::atomic<bool>  m_closing{false};   ///< 服务器关闭标志(Shutdown 置位,回复门)

    ZmReqLoopPool*     m_pool;
};

// ZmReqLoop::Release 经自由函数 ZmReqLoopPoolReturn 回池;声明与 ZmReqLoopPool
// 同位于 zm_net_req_loop_pool.h(zm_net_req_loop.cpp include 之)。

#endif // ZM_NET_REQ_LOOP_H
