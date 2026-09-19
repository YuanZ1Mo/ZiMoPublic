#include "zm_util_evbase_runloop.h"

#include "../util/zm_util_libevent.h"
#include "../util/zm_util_logger.h"
#include "../util/zm_util_str.h"

#include <../libevent/include/event2/dns.h>
#include <../libevent/include/event2/event.h>

enum { CONTROL_LOOP_SUCCESS = 0x0200, };

/**
 * @brief 武装/重臂一个定时器事件(改间隔后调用即按新间隔重新计时)
 *
 * 纯事件操作;调用方须持有 _mutex_ev,且不得同时持有 _mutex_loop
 * (event_del 在回调执行中会阻塞等待,持 _mutex_loop 会与回调收尾互锁)。
 *
 * @param ev         定时器事件
 * @param intervalMs 触发间隔毫秒(>0)
 * @return true 已武装;false 武装失败
 */
static bool ArmEvent(event* ev, int64_t intervalMs)
{
    timeval tv = {};
    tv.tv_sec  = static_cast<long>(intervalMs / 1000);
    tv.tv_usec = static_cast<long>((intervalMs % 1000) * 1000);
    event_del(ev);   // 幂等(未武装则空操作);已武装则改间隔重新计时
    return event_add(ev, &tv) == 0;
}

ZmEvBaseRunLoop::ZmEvBaseRunLoop(const std::string& name): ZmThread(name)
{
    _evbase     = nullptr;
    _eventCtrl  = nullptr;
    _b_looped = false;
    _b_run_finished = false;
}

ZmEvBaseRunLoop::~ZmEvBaseRunLoop()
{
}

void ZmEvBaseRunLoop::freeEventObjects()
{
    PUBLIC_LOG_INFO("Free the event objects");

    // 定时器事件挂在 base 上,先于 base 释放
    // (调用方保证此刻没有并发的定时器操作:循环启动前,或循环退出后的持锁点)
    for (auto& kv : _timers)
    {
        if (kv.second.ev)
            event_free(kv.second.ev);
    }
    _timers.clear();
    _retired.clear();
    _runningTimerId = 0;
    _nextTimerId    = 1;

    if (_eventCtrl)
    {
        event_free(_eventCtrl);
        _eventCtrl = nullptr;
    }

    if (_evbase)
    {
        event_base_free(_evbase);
        _evbase = nullptr;
    }
}

bool ZmEvBaseRunLoop::Loop()
{
    std::unique_lock<std::mutex> lock(_mutex_loop);
    if (!_b_looped)
    {
        Start();
        _cv_loop.wait(lock, [this] { return _b_looped || _b_run_finished; });
    }

    return _b_looped;
}

bool ZmEvBaseRunLoop::IsLooped()
{
    std::unique_lock<std::mutex> lock(_mutex_loop);
    return _b_looped;
}

void ZmEvBaseRunLoop::Control(short events)
{
    std::unique_lock<std::mutex> lock(_mutex_loop);

    PUBLIC_LOG_INFO("Received control event: events={}", events);

    if (IsRunning() || IsStopping())
    {
        if (_eventCtrl)
        {
            event_active(_eventCtrl, events, 0);
        }
    }
}

event_base* ZmEvBaseRunLoop::GetEventBase()
{
    std::unique_lock<std::mutex> lock(_mutex_loop);
    return _evbase;
}

// ============================================================================
// 定时器
// ============================================================================

ZmEvBaseRunLoop::TimerId ZmEvBaseRunLoop::AddTimer(int64_t intervalMs,
                                                   std::function<void()> cb, bool repeat)
{
    if (intervalMs <= 0 || !cb)
        return 0;   // 间隔非法或回调为空:没有意义,直接拒收

    TimerEntry*        e    = nullptr;
    TimerId            id   = 0;
    struct event_base* base = nullptr;
    {
        // 新事件此刻还没有别的线程能看到,无需 _mutex_ev
        std::lock_guard<std::mutex> lock(_mutex_loop);
        if (!_b_looped || _evbase == nullptr)
            return 0;   // 未启动/已退出

        id = _nextTimerId++;
        if (id == 0)
            id = _nextTimerId++;   // 0 保留为"无效句柄",回绕时跳过

        // 先登记、后建 event:回调参数取登记项地址。unordered_map 是节点式容器,
        // 后续插入引发的 rehash 不搬动已有元素,故该地址在登记项被移除前一直有效
        e             = &_timers[id];
        e->owner      = this;
        e->id         = id;
        e->intervalMs = intervalMs;
        e->repeat     = repeat;
        e->retired    = false;
        e->cb         = std::move(cb);
        base          = _evbase;
    }

    e->ev = event_new(base, -1, repeat ? (EV_TIMEOUT | EV_PERSIST) : EV_TIMEOUT,
                      &ZmEvBaseRunLoop::OnTimerDispatchCB, e);
    if (e->ev == nullptr || !ArmEvent(e->ev, intervalMs))
    {
        if (e->ev != nullptr)
            event_free(e->ev);
        std::lock_guard<std::mutex> lock(_mutex_loop);
        _timers.erase(id);
        return 0;
    }
    return id;
}

bool ZmEvBaseRunLoop::StopTimer(TimerId id)
{
    std::lock_guard<std::mutex> evLock(_mutex_ev);
    event*                      ev = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex_loop);
        TimerEntry*                 e = findTimerLocked(id);
        if (e == nullptr || e->ev == nullptr)
            return false;
        if (event_pending(e->ev, EV_TIMEOUT, nullptr) == 0)
            return false;   // 本就停着
        ev = e->ev;
    }
    // 锁外停表:非 loop 线程调用时 event_del 会等到该定时器的回调跑完,
    // 而回调收尾要拿 _mutex_loop —— 持着它调用必与回调互锁
    event_del(ev);
    return true;
}

bool ZmEvBaseRunLoop::ArmTimer(TimerId id)
{
    std::lock_guard<std::mutex> evLock(_mutex_ev);
    event*                      ev = nullptr;
    int64_t                     ms = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex_loop);
        TimerEntry*                 e = findTimerLocked(id);
        if (e == nullptr || e->ev == nullptr)
            return false;
        ev = e->ev;
        ms = e->intervalMs;
    }
    // 锁外武装(同上:event_del 可能阻塞)
    return ArmEvent(ev, ms);
}

bool ZmEvBaseRunLoop::SetTimerInterval(TimerId id, int64_t intervalMs)
{
    if (intervalMs <= 0)
        return false;

    std::lock_guard<std::mutex> evLock(_mutex_ev);
    event*                      ev    = nullptr;
    bool                        armed = false;
    {
        std::lock_guard<std::mutex> lock(_mutex_loop);
        TimerEntry*                 e = findTimerLocked(id);
        if (e == nullptr)
            return false;
        e->intervalMs = intervalMs;
        ev            = e->ev;
        armed         = (ev != nullptr) && event_pending(ev, EV_TIMEOUT, nullptr) != 0;
    }
    if (ev == nullptr || !armed)
        return true;   // 停着的只更新登记,等 ArmTimer 时按新间隔生效
    return ArmEvent(ev, intervalMs);
}

size_t ZmEvBaseRunLoop::StopAllTimers()
{
    std::vector<TimerId> ids;
    {
        std::lock_guard<std::mutex> lock(_mutex_loop);
        for (const auto& kv : _timers)
        {
            if (!kv.second.retired)
                ids.push_back(kv.first);
        }
    }
    size_t n = 0;
    for (TimerId id : ids)
    {
        if (StopTimer(id))
            ++n;   // 本就停着的、期间被别处移除的不计
    }
    return n;
}

bool ZmEvBaseRunLoop::RemoveTimer(TimerId id)
{
    std::lock_guard<std::mutex> evLock(_mutex_ev);

    event* ev      = nullptr;
    bool   onStack = false;
    {
        std::lock_guard<std::mutex> lock(_mutex_loop);
        auto                        it = _timers.find(id);
        if (it == _timers.end() || it->second.retired)
            return false;
        it->second.retired = true;   // 即刻失效:查找与统计不再看到它
        ev                 = it->second.ev;
        onStack            = (_runningTimerId == id);   // 回调正跑在本线程栈上
    }
    if (ev != nullptr)
    {
        event_del(ev);   // 锁外:非 loop 线程调用时会等到回调结束
        if (!onStack)
            event_free(ev);
    }
    {
        std::lock_guard<std::mutex> lock(_mutex_loop);
        if (onStack && ev != nullptr)
            _retired.push_back(id);   // 回调还在栈上:交给下一轮派发回收
        else
            _timers.erase(id);
    }
    return true;
}

size_t ZmEvBaseRunLoop::RemoveAllTimers()
{
    std::vector<TimerId> ids;
    {
        std::lock_guard<std::mutex> lock(_mutex_loop);
        for (const auto& kv : _timers)
        {
            if (!kv.second.retired)
                ids.push_back(kv.first);
        }
    }
    size_t n = 0;
    for (TimerId id : ids)
    {
        if (RemoveTimer(id))
            ++n;
    }
    return n;
}

size_t ZmEvBaseRunLoop::TimerCount()
{
    std::lock_guard<std::mutex> lock(_mutex_loop);
    size_t                      n = 0;
    for (const auto& kv : _timers)
    {
        if (!kv.second.retired)
            ++n;
    }
    return n;
}

// ============================================================================
// 循环线程
// ============================================================================

void ZmEvBaseRunLoop::Run()
{
    PUBLIC_LOG_INFO("The ZmEvBaseRunLoop is running");

    zm_util_eventbase_init();

    freeEventObjects();

    struct event_config* cfg = event_config_new();
    event_config_set_flag(cfg, EVENT_BASE_FLAG_STARTUP_IOCP);

    _evbase = event_base_new();
    if (nullptr != _evbase)
    {
        _eventCtrl = event_new(_evbase, -1, EV_PERSIST | EV_READ, ZmEvBaseRunLoop::OnEventCtrlCB, (void*)this);
        event_add(_eventCtrl, 0);
        event_active(_eventCtrl, CONTROL_LOOP_SUCCESS, 0);

        // 定时器不随 Run 自动启动:全部由 AddTimer() 手动登记
        // (EV_PERSIST 保证周期定时器触发后自动重臂,见 event_persist_closure)

        // #define EVLOOP_ONCE              0x01
        // #define EVLOOP_NONBLOCK          0x02
        // #define EVLOOP_NO_EXIT_ON_EMPTY  0x04
        int ret = event_base_loop(_evbase, EVLOOP_NO_EXIT_ON_EMPTY);
        PUBLIC_LOG_INFO("ZmEvBaseRunLoop is exited ret:{}, unexpected:{}", ret, (0 == event_base_got_exit(_evbase)) ? 0 : 1);

        // 先标记结束状态并通知等待者，消除 _evbase 释放与状态变更之间的不一致窗口
        {
            std::lock_guard<std::mutex> lock(_mutex_loop);
            _b_looped = false;
            _b_run_finished = true;
            _cv_loop.notify_one();
        }

        // 等待者已收到通知，安全释放资源
        // (持锁:与定时器接口/Control 的成员访问互斥,防退出窗口内 event 被并发触碰;
        //  _mutex_ev 在前,与各定时器接口的取锁顺序一致)
        {
            std::lock_guard<std::mutex> evLock(_mutex_ev);
            std::lock_guard<std::mutex> lock(_mutex_loop);
            freeEventObjects();
        }
    }
    else
    {
        PUBLIC_LOG_ERROR("Open event base failed");
        {
            std::lock_guard<std::mutex> lock(_mutex_loop);
            _b_run_finished = true;
        }
        _cv_loop.notify_one();
    }

    PUBLIC_LOG_INFO("The ZmEvBaseRunLoop is stoped");
}

void ZmEvBaseRunLoop::OnStopping()
{
    Control(CONTROL_LOOP_EXIT);
}

void ZmEvBaseRunLoop::OnEventCtrlCB(evutil_socket_t fd, short what, void* arg)
{
    PUBLIC_LOG_INFO("Received control event: fd={}, what={}, arg={}", (int)fd, what, arg);

    ZmEvBaseRunLoop* dockRunloop = (ZmEvBaseRunLoop*)arg;

    //剥离 libevent标准事件标志、保留自定义控制命令
    what = what & 0x7F00;
    if ((what & CONTROL_LOOP_EXIT) == CONTROL_LOOP_EXIT)
    {
        if (nullptr != dockRunloop->_evbase)
        {
            event_base_loopexit(dockRunloop->_evbase, NULL);
        }
    }
    if ((what & CONTROL_LOOP_SUCCESS) == CONTROL_LOOP_SUCCESS)
    {
        {
            /** event_base_loopbreak() 立即退出， event_base_loopexit() 完成未完成的任务后再退出 */
            std::lock_guard<std::mutex> lock(dockRunloop->_mutex_loop);
            dockRunloop->_b_looped = true;
        }
        dockRunloop->_cv_loop.notify_one();
    }
}

// ============================================================================
// 定时器内部实现
// ============================================================================

void ZmEvBaseRunLoop::OnTimerDispatchCB(evutil_socket_t fd, short what, void* arg)
{
    (void)fd;
    (void)what;

    TimerEntry* e = static_cast<TimerEntry*>(arg);
    if (e == nullptr || e->owner == nullptr)
        return;

    ZmEvBaseRunLoop*      self = e->owner;
    std::function<void()> cb;
    bool                  once = false;
    {
        // 锁内拷贝、锁外调用:回调会重入本类接口,持锁调用必死锁
        std::lock_guard<std::mutex> lock(self->_mutex_loop);
        self->drainRetiredLocked(e->id);
        cb                    = e->cb;
        once                  = !e->repeat;
        self->_runningTimerId = e->id;
    }

    try
    {
        cb();
    }
    // 就地拦下异常:一旦击穿 libevent 的派发栈,整个循环线程就没了
    catch (const std::exception& ex)
    {
        PUBLIC_LOG_ERROR("{}:timer callback threw, isolated: {}", self->GetName(), ex.what());
    }
    catch (...)
    {
        PUBLIC_LOG_ERROR("{}:timer callback threw unknown exception, isolated", self->GetName());
    }

    {
        std::lock_guard<std::mutex> lock(self->_mutex_loop);
        if (once)
        {
            auto it = self->_timers.find(e->id);
            if (it != self->_timers.end() && !it->second.retired)
            {
                it->second.retired = true;   // 一次性:响过即注销
                if (it->second.ev != nullptr)
                {
                    // 一次性事件响过后不再武装,event_del 不会阻塞;而回调还在这条线程
                    // 的栈上,event 只能交给下一轮派发回收
                    event_del(it->second.ev);
                    self->_retired.push_back(e->id);
                }
                else
                {
                    self->_timers.erase(it);
                }
            }
        }
        self->_runningTimerId = 0;
    }
}

ZmEvBaseRunLoop::TimerEntry* ZmEvBaseRunLoop::findTimerLocked(TimerId id)
{
    auto it = _timers.find(id);
    if (it == _timers.end() || it->second.retired)
        return nullptr;   // 已注销的视同不存在
    return &it->second;
}

void ZmEvBaseRunLoop::drainRetiredLocked(TimerId runningId)
{
    // 循环单线程逐个派发,此刻栈上最多只有 runningId 这一个定时器的回调;
    // 其余待回收的都不在执行中,可以直接释放(event_free 内部的 event_del 不会阻塞)
    std::vector<TimerId> keep;
    for (TimerId id : _retired)
    {
        if (id == runningId)
        {
            keep.push_back(id);   // 还在派发它,留到下一轮
            continue;
        }
        auto it = _timers.find(id);
        if (it == _timers.end())
            continue;
        if (it->second.ev != nullptr)
            event_free(it->second.ev);
        _timers.erase(it);
    }
    _retired.swap(keep);
}
