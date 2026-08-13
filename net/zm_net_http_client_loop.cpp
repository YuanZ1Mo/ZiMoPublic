#include "zm_net_http_client_loop.h"

#include "zm_net_dns.h"
#include "../util/zm_util_libevent.h"
#include "../util/zm_util_logger.h"
#include "../util/zm_util_str.h"

#include <../libevent/include/event2/dns.h>

enum { CLIENT_LOOP_CTRL_EXIT = 0x0100 };

ZmHttpClientLoop::ZmHttpClientLoop(const std::string& name)
    : ZmThread(name)
    , m_evbase(nullptr), m_evdnsbase(nullptr), m_ctrlEvent(nullptr)
    , m_looped(false), m_runFinished(false)
{
}

ZmHttpClientLoop::~ZmHttpClientLoop()
{
    Stop();
}

bool ZmHttpClientLoop::Start()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_looped && !m_runFinished)
    {
        if (!ZmThread::Start())   // 线程创建失败:直接返回 false,不等待
            return false;
        m_cv.wait(lock, [this] { return m_looped || m_runFinished; });
    }
    return m_looped;
}

void ZmHttpClientLoop::Stop()
{
    // 基类 Stop 调用 OnStopping → event_active EXIT → event_base_loopexit
    ZmThread::Stop();
}

event_base* ZmHttpClientLoop::EventBase() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_evbase;
}

evdns_base* ZmHttpClientLoop::EventDnsBase() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_evdnsbase;
}

void ZmHttpClientLoop::Run()
{
    zm_util_eventbase_init();

    m_evbase = event_base_new();
    if (!m_evbase)
    {
        PUBLIC_LOG_ERROR("{}: event_base_new failed", GetName());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_runFinished = true;
        }
        m_cv.notify_all();
        return;
    }

    m_ctrlEvent = event_new(m_evbase, -1, EV_PERSIST | EV_READ, ZmHttpClientLoop::OnCtrlCB, (void*)this);
    event_add(m_ctrlEvent, 0);

    m_evdnsbase = evdns_base_new(m_evbase, 0);
    if (!m_evdnsbase)
        PUBLIC_LOG_ERROR("{}: evdns_base_new failed", GetName());
    if (m_evdnsbase)
    {
        // 从系统获取 DNS 服务器并配置(与 ZmEvBaseRunLoop::Run 相同逻辑)
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
                        evdns_base_nameserver_ip_add(m_evdnsbase, token);
                    token = zm_strsep(&cursor, ",");
                }
                free(addrs_buf);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_looped = true;
    }
    m_cv.notify_all();

    PUBLIC_LOG_INFO("{}: loop running", GetName());
    event_base_loop(m_evbase, EVLOOP_NO_EXIT_ON_EMPTY);
    PUBLIC_LOG_INFO("{}: loop exited", GetName());

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_looped = false;
        m_runFinished = true;
    }

    if (m_ctrlEvent) { event_free(m_ctrlEvent); m_ctrlEvent = nullptr; }
    if (m_evdnsbase) { evdns_base_free(m_evdnsbase, 0); m_evdnsbase = nullptr; }
    if (m_evbase)    { event_base_free(m_evbase);    m_evbase = nullptr; }
}

void ZmHttpClientLoop::OnStopping()
{
    if (m_ctrlEvent)
        event_active(m_ctrlEvent, CLIENT_LOOP_CTRL_EXIT, 0);
}

void ZmHttpClientLoop::OnCtrlCB(evutil_socket_t /*fd*/, short what, void* arg)
{
    auto* self = static_cast<ZmHttpClientLoop*>(arg);
    what &= 0x7F00;
    if ((what & CLIENT_LOOP_CTRL_EXIT) && self->m_evbase)
        event_base_loopexit(self->m_evbase, nullptr);
}
