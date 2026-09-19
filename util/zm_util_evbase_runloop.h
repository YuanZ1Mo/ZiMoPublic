#ifndef ZM_UTIL_EVBASE_RUNLOOP_H
#define ZM_UTIL_EVBASE_RUNLOOP_H

#include "../util/zm_util_thread.h"

#include <../libevent/include/event2/util.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// libevent 结构体前向声明（头文件中仅通过指针成员使用）
struct event_base;
struct evdns_base;
struct event;

/**
 * @brief 基于 libevent 的事件循环线程，内置多定时器管理
 *
 * 循环跑在自己的线程上；定时器回调一律在该线程执行，且同一循环上不会并发
 * （单线程逐个派发）。回调内可安全地增删本循环的定时器 —— 包括删掉自己。
 * 回调抛出的异常被就地拦下并记日志，不会击穿派发栈终止循环线程。
 */
class ZmEvBaseRunLoop : public ZmThread
{
public:
    /// 定时器句柄(0 = 无效;AddTimer 返回,其余定时器接口按它定位)
    using TimerId = uint64_t;

    enum { CONTROL_LOOP_EXIT =0x0100,};

    ZmEvBaseRunLoop(const std::string& name);
    virtual ~ZmEvBaseRunLoop();

    bool Loop();

    bool IsLooped();

    void Control(short events);

    // ── 定时器(全部接口任意线程可调;回调执行于 loop 线程) ──

    /**
     * @brief 添加定时器
     *
     * @param intervalMs 触发间隔毫秒(>0)
     * @param cb         到期回调(不可为空);执行于 loop 线程
     * @param repeat     true = 周期触发(自动重臂);false = 只触发一次,响过即自动移除
     * @return 定时器句柄;循环未启动/已退出、参数非法时返回 0
     *
     * @example
     *   auto t = loop.AddTimer(500, [] { Poll(); });        // 每 500ms 一次
     *   loop.AddTimer(3000, [] { Once(); }, false);         // 3 秒后一次
     *   loop.RemoveTimer(t);                                // 用完移除,释放句柄
     */
    TimerId AddTimer(int64_t intervalMs, std::function<void()> cb, bool repeat = true);

    /**
     * @brief 停止定时器(句柄保留,可再 ArmTimer 恢复)
     * @param id AddTimer 返回的句柄
     * @return true 已停止;false 句柄无效或本就处于停止态
     */
    bool StopTimer(TimerId id);

    /**
     * @brief 重新武装已停止的定时器(按原间隔重新计时)
     * @param id AddTimer 返回的句柄
     * @return true 已武装;false 句柄无效
     */
    bool ArmTimer(TimerId id);

    /**
     * @brief 改触发间隔;定时器在运行则按新间隔重新计时
     * @param id         AddTimer 返回的句柄
     * @param intervalMs 新间隔毫秒(>0)
     * @return true 已生效;false 句柄无效或参数非法
     */
    bool SetTimerInterval(TimerId id, int64_t intervalMs);

    /**
     * @brief 停止全部定时器(句柄全部保留,可逐个 ArmTimer 恢复)
     * @return 实际被停止的数量(本就停着的、已注销的不计)
     */
    size_t StopAllTimers();

    /**
     * @brief 停止并移除定时器(句柄作废,登记项一并释放)
     *
     * 与 StopTimer 的区别:StopTimer 只是停表、句柄还能复活;RemoveTimer 连句柄一起注销。
     * 增删频繁的场景用本接口,否则登记表只增不减。
     *
     * @param id AddTimer 返回的句柄
     * @return true 已移除;false 句柄无效
     */
    bool RemoveTimer(TimerId id);

    /**
     * @brief 停止并移除全部定时器
     * @return 实际被移除的数量
     */
    size_t RemoveAllTimers();

    /// @return 当前登记在册的定时器数量(含已停止、可用 ArmTimer 恢复的)
    size_t TimerCount();

    event_base* GetEventBase();
    evdns_base* GetEventDnsBase();

protected:
    static  void    OnEventCtrlCB(evutil_socket_t fd, short what, void* arg);

    virtual void    Run();
    virtual void    OnStopping();

private:
    /// 定时器登记项;存活于 _timers,移除前地址不变(派发时以它作回调参数)
    struct TimerEntry
    {
        ZmEvBaseRunLoop*      owner      = nullptr;  ///< 归属循环(派发时回指)
        TimerId               id         = 0;
        event*                ev         = nullptr;
        int64_t               intervalMs = 0;
        bool                  repeat     = true;     ///< false = 一次性
        bool                  retired    = false;    ///< 已注销、待释放(回调可能还在栈上)
        std::function<void()> cb;                    ///< 到期回调(AddTimer 保证非空)
    };

    static  void    OnTimerDispatchCB(evutil_socket_t fd, short what, void* arg);

    TimerEntry*     findTimerLocked(TimerId id);
    void            drainRetiredLocked(TimerId runningId);
    void            freeEventObjects();

    event_base*  _evbase;
    event*       _eventCtrl;

    std::unordered_map<TimerId, TimerEntry> _timers;      ///< 定时器登记表(按句柄)
    std::vector<TimerId>                    _retired;     ///< 待释放句柄:其回调可能仍在栈上
    TimerId                                 _nextTimerId{ 1 };
    TimerId                                 _runningTimerId{ 0 };  ///< 正在派发的定时器

    /// 串行化 event_new/add/del/free:这些调用在"回调正跑在 loop 线程"时会阻塞等待,
    /// 故**只能在未持有 _mutex_loop 时进行**(回调收尾要拿 _mutex_loop,持着它调用必互锁)。
    /// 取锁顺序固定为 _mutex_ev → _mutex_loop;派发路径只取 _mutex_loop。
    std::mutex              _mutex_ev;
    std::mutex              _mutex_loop;
    std::condition_variable _cv_loop;
    bool _b_looped;
    bool _b_run_finished;
};

#endif /* ZM_UTIL_EVBASE_RUNLOOP_H */
