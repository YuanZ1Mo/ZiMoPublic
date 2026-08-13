#include "zm_net_runloop.h"

#include "zm_logger.h"
#include "zm_util_sys.h"
#include "zm_util_libevent.h"
#include "zm_net_dns.h"
#include "zm_util_str.h"

#include <../libevent/include/event2/dns.h>
#include <../libevent/include/event2/event.h>

enum { CONTROL_LOOP_SUCCESS = 0x0200, };

ZmEvBaseRunLoop::ZmEvBaseRunLoop(const std::string& name): ZmThread(name)
{
    _evbase     = nullptr;
    _evdnsbase = nullptr;
    _eventCtrl  = nullptr;
    _eventTimer = nullptr;
    _b_looped = false;
    _b_run_finished = false;
}

ZmEvBaseRunLoop::~ZmEvBaseRunLoop()
{
}

void ZmEvBaseRunLoop::freeEventObjects()
{
    PUBLIC_LOG_INFO("Free the event objects");

    if (_eventCtrl)
    {
        event_free(_eventCtrl);
        _eventCtrl = nullptr;
    }

    if (_evdnsbase)
    {
        evdns_base_free(_evdnsbase, 0);
        _evdnsbase = nullptr;
    }

    if (_eventTimer)
    {
        event_free(_eventTimer);
        _eventTimer = nullptr;
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

evdns_base* ZmEvBaseRunLoop::GetEventDnsBase()
{
    std::unique_lock<std::mutex> lock(_mutex_loop);
    return _evdnsbase;
}

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

        _evdnsbase = evdns_base_new(_evbase, 0);

        // 从系统获取 DNS 服务器并配置到 evdns_base，不然 evdns_getaddrinfo 无法正常工作
        {
            std::string dns_addrs = ZmNetDNS::GetDNSAddresses();
            if (!dns_addrs.empty())
            {
                char* addrs_buf = _strdup(dns_addrs.c_str());
                if (addrs_buf)
                {
                    char* cursor = addrs_buf;
                    char* token = zm_strsep(&cursor, ",");
                    while (token)
                    {
                        while (*token == ' ') token++;
                        if (*token)
                        {
                            evdns_base_nameserver_ip_add(_evdnsbase, token);
                            PUBLIC_LOG_INFO("Add DNS nameserver to evdns_base: {}", token);
                        }
                        token = zm_strsep(&cursor, ",");
                    }
                    free(addrs_buf);
                }
            }
        }

        // 周期定时器不再随 Run 自动启动:由 StartTimer() 手动触发
        // (EV_PERSIST 保证定时器循环触发,自动重臂,见 event_persist_closure)

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
        // (持锁:与 StartTimer/StopTimer/Control 的成员访问互斥,防退出窗口内 event 被并发触碰)
        {
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

bool ZmEvBaseRunLoop::StartTimer(int64_t intervalSec)
{
    std::lock_guard<std::mutex> lock(_mutex_loop);
    if (!_b_looped)
        return false;   // 未启动/已退出
    if (intervalSec <= 0)
        return false;

    if (_eventTimer == nullptr)
    {
        // EV_PERSIST 保证定时器循环触发,自动重臂(见 event_persist_closure)
        // 无捕获 lambda 可转 C 函数指针,arg=this 中转:
        // 分发顺序:SetTimerCallback 设置的回调优先,否则虚函数 OnTimerCB(缺省心跳)
        _eventTimer = event_new(_evbase, -1, EV_TIMEOUT | EV_PERSIST,
            [](evutil_socket_t, short, void* arg) {
                auto* self = static_cast<ZmEvBaseRunLoop*>(arg);
                // 锁内拷贝(与 SetTimerCallback 互斥),锁外调用
                // (用户回调可能重入 StartTimer/StopTimer,持锁调用会死锁)
                std::function<void()> cb;
                {
                    std::lock_guard<std::mutex> lock(self->_mutex_loop);
                    cb = self->_timerCb;
                }
                if (cb)
                    cb();                   // SetTimerCallback 优先
                else
                    self->OnTimerCB();      // 缺省:虚函数(心跳)
            }, this);
        if (_eventTimer == nullptr)
            return false;
    }
    timeval timer_second = { (long)intervalSec, 0 };
    event_del(_eventTimer);            // 幂等(未挂起则空操作);已运行时调用 = 改间隔重臂
    event_add(_eventTimer, &timer_second);
    return true;
}

void ZmEvBaseRunLoop::StopTimer()
{
    std::lock_guard<std::mutex> lock(_mutex_loop);
    if (_eventTimer)
        event_del(_eventTimer);
}

void ZmEvBaseRunLoop::SetTimerCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(_mutex_loop);
    _timerCb = std::move(cb);
}

void ZmEvBaseRunLoop::OnTimerCB()
{
    // 缺省实现:原心跳行为(仅诊断用;SetTimerCallback 未设置时生效)
    char buf[32];
    ZmSystem::CurrentTimeStr(buf, sizeof(buf));
    PUBLIC_LOG_INFO("{}:{} HeartbeatTime:{}", GetName(), (void*)this, buf);
}
