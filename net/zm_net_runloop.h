#ifndef DOCK_RUNLOOP_H
#define DOCK_RUNLOOP_H

#include "../util/zm_util_thread.h"
#include "../libevent/include/event2/dns.h"
#include "../libevent/include/event2/event.h"

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

    event_base* GetEventBase();
    evdns_base* GetEventDnsBase();

protected:
    static  void    OnEventCtrlCB(evutil_socket_t fd, short what, void* arg);
    static  void    OnTimerCB(evutil_socket_t fd, short what, void* ctx);

    virtual void    Run();
    virtual void    OnStopping();

private:
    void            freeEventObjects();

    event_base*  _evbase;
    evdns_base* _evdnsbase;
    event*       _eventTimer;
    event*       _eventCtrl;

    std::mutex              _mutex_loop;
    std::condition_variable _cv_loop;
    bool _b_looped;
    bool _b_run_finished;
};

#endif /* DOCK_RUNLOOP_H */
