#ifndef ZM_NET_HTTP_CLIENT_LOOP_H
#define ZM_NET_HTTP_CLIENT_LOOP_H

#include "../util/zm_util_thread.h"

#include <../libevent/include/event2/event.h>

#include <condition_variable>
#include <mutex>
#include <string>

struct event_base;
struct evdns_base;

/**
 * @brief ZmHttpClient 的自带循环线程(仿 ZmEvBaseRunLoop 结构,定制:无心跳定时器)
 *
 * 生命周期:Start() 启动线程并等待就绪;Stop() 退出循环(join)。
 * event_base / evdns_base 在线程内创建,线程退出时释放。
 */
class ZmHttpClientLoop : public ZmThread
{
public:
    explicit ZmHttpClientLoop(const std::string& name);
    virtual ~ZmHttpClientLoop();

    bool Start();                    ///< 启动线程,等待 event_base 就绪
    void Stop();                     ///< 退出循环(OnStopping → event_active EXIT,内部 join)

    event_base* EventBase() const;   ///< 循环线程的事件基(仅循环线程内使用)
    evdns_base* EventDnsBase() const;

private:
    static void OnCtrlCB(evutil_socket_t fd, short what, void* arg);

protected:
    virtual void Run() override;
    virtual void OnStopping() override;

    struct event_base* m_evbase;
    struct evdns_base* m_evdnsbase;
    struct event*      m_ctrlEvent;
    mutable std::mutex         m_mutex;
    std::condition_variable    m_cv;
    bool               m_looped;
    bool               m_runFinished;
};

#endif // ZM_NET_HTTP_CLIENT_LOOP_H
