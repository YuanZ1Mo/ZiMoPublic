#ifndef ZM_NET_TAP_DNR_H
#define ZM_NET_TAP_DNR_H


#include "../util/zm_util_thread.h"
#include "../util/zm_util_container.h"

#include <event2/event.h>

// Forward declaration — full definition in zm_net_tap.h
typedef struct ZM_TAP_CTX ZM_TAP_CTX;

typedef struct
{
} ZM_DNS_ASYNC_REQUEST;


class ZmTapDomainNameResolver : public ZmThread
{
public:
    enum
    {
        STATE_IDLE = 0,
        STATE_PENDING = 1,
        STATE_RESOLVING = 2,
        STATE_CANCELED = 3,
        STATE_RESOLVED = 4,
        STATE_ERROR = 5,
    };

    typedef void (*FN_OnNameResolved)(ZM_TAP_CTX* tap, uint32_t option, const char* hostname,
        int errcode, struct sockaddr_in6* sa6, socklen_t salen, const char* ipaddr);

    ZmTapDomainNameResolver();
    ~ZmTapDomainNameResolver();

    ZM_DNS_ASYNC_REQUEST* Resolve(struct event_base* evbase, const char* hostname, uint16_t port,
        FN_OnNameResolved cb, ZM_TAP_CTX* tap, uint32_t option = 0);
    void Cancel(ZM_DNS_ASYNC_REQUEST* req);

private:
    typedef struct _RESOLVE_TASK : public ZM_DNS_ASYNC_REQUEST
    {
        uint16_t            state;     // 0:idle, 1:pending, 2:resoving, 3:canceled, 4:resoved, 5:error

        uint16_t            port;
        char                hostname[128];
        ZM_TAP_CTX* tap;
        uint32_t            option;

        struct event* rsp_event;
        FN_OnNameResolved   pfn_onresolved;

        struct sockaddr_in6 sa;
        socklen_t           salen;

        char                ipaddr[64];

        void* delegate;
    };

protected:
    virtual void Run();

private:
    _RESOLVE_TASK* GetPendingTask();
    void ResolveRequest(_RESOLVE_TASK* task);
    void FireResponse(_RESOLVE_TASK* task);

    void Release(_RESOLVE_TASK* task, bool erasePending = true);

    static void OnEventResolved(evutil_socket_t fd, short what, void* ctx);

    std::recursive_mutex         _mutex;

    ZmObjectPool<_RESOLVE_TASK>  _task_pool;
    std::vector<_RESOLVE_TASK*>  _task_pends;
};


#endif /* ZM_NET_TAP_DNR_H */
