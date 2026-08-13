#ifndef DOCK_RUNLOOP_H
#define DOCK_RUNLOOP_H

#include "../util/zm_util_thread.h"

#include <../libevent/include/event2/util.h>

#include <functional>

// libevent 结构体前向声明（头文件中仅通过指针成员使用）
struct event_base;
struct evdns_base;
struct event;

/** ZmEvBaseRunLoop 心跳定时器间隔（秒） */
#define ZM_DOCK_HEARTBEAT_SEC   60

class ZmEvBaseRunLoop : public ZmThread
{
public:
    enum { CONTROL_LOOP_EXIT =0x0100,};

    ZmEvBaseRunLoop(const std::string& name);
    virtual ~ZmEvBaseRunLoop();

    bool Loop();

    bool IsLooped();

    void Control(short events);

    /**
     * @brief 手动启动周期定时器(不再随 Run 自动启动;任意线程可调)
     * @param intervalSec 触发间隔秒(>0);定时器已运行时调用 = 改间隔重臂
     * @note 依赖 base 线程锁(event_add/event_del 内部持锁;须先经 zm_util_eventbase_init 开启线程支持)
     * @return true 已武装;false 未启动/已退出/参数非法
     */
    bool StartTimer(int64_t intervalSec = ZM_DOCK_HEARTBEAT_SEC);

    /**
     * @brief 停止周期定时器(幂等;任意线程可调)
     * @note event_del(EVENT_DEL_AUTOBLOCK):到期回调正在执行时,调用方阻塞至其完成
     */
    void StopTimer();

    /**
     * @brief 设置定时器到期回调(任意线程可调;执行于 loop 线程,优先于虚函数 OnTimerCB)
     * @param cb 到期回调;传空 = 复位,回退到虚函数 OnTimerCB(缺省心跳)
     * @note 回调异常须自行捕获(异常击穿 event_base_loop 栈会终止循环线程)
     */
    void SetTimerCallback(std::function<void()> cb);

    event_base* GetEventBase();
    evdns_base* GetEventDnsBase();

protected:
    static  void    OnEventCtrlCB(evutil_socket_t fd, short what, void* arg);

    /**
     * @brief 定时器到期回调(虚函数,子类覆写自定义周期任务;loop 线程执行)
     * @note 缺省 = 原心跳行为:打印当前时间(诊断用);
     *       SetTimerCallback 设置的回调优先于本虚函数
     * @note 回调异常须自行捕获(异常击穿 event_base_loop 栈会终止循环线程)
     */
    virtual void    OnTimerCB();

    virtual void    Run();
    virtual void    OnStopping();

private:
    void            freeEventObjects();

    event_base*  _evbase;
    evdns_base* _evdnsbase;
    event*       _eventTimer;
    event*       _eventCtrl;
    std::function<void()> _timerCb;   ///< 定时器回调(SetTimerCallback 设置;空 = 走虚函数 OnTimerCB)

    std::mutex              _mutex_loop;
    std::condition_variable _cv_loop;
    bool _b_looped;
    bool _b_run_finished;
};

#endif /* DOCK_RUNLOOP_H */
