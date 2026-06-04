#ifndef ZM_NET_TAP_HUB_H
#define ZM_NET_TAP_HUB_H

#include "zm_net_tap.h"
#include "zm_net_tap_jrpc.h"

#include <event2/listener.h>





class ZmTapHubBase : public ZmTapDelegate
{
public:
    ZmTapHubBase();
    virtual ~ZmTapHubBase();

    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) { return true; }

    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) {  }

    virtual void OnTapDelegateEvent(short what) {}

protected:
    struct ZM_HUB_LISTENER
    {
        char                    host[64];
        uint16_t                port;
        struct evconnlistener*  v4;
        struct evconnlistener*  v6;
    };

protected:
    evconnlistener* ListenEV(struct event_base* evbase, evconnlistener_cb cb, void* ctx,
                                           const char* addr, uint16_t family=AF_INET, const char* sock_name=NULL);
    bool                   Listen(ZM_HUB_LISTENER* listener, struct event_base* evbase,
                                         evconnlistener_cb cb, void* ctx,
                                         const char* addr, bool v4only=false, const char* sock_name=NULL);
    void                   CloseListener(ZM_HUB_LISTENER* listener);

protected:
    ZM_HUB_LISTENER    _listener;
};






typedef enum {
    PROXY_PORT_NOTYPE = 0,
} ZM_HUB_PROXY_PORT_TYPE;

typedef enum {
    PROXY_DROP_NOTYPE = 0,
} ZM_HUB_PROXY_DROP_TYPE;


class ZmTapHubProxy : public ZmTapHubBase
{
public:
    ZmTapHubProxy();
    virtual ~ZmTapHubProxy();

    void    StartTapDelegate(ZmTapContext* context, struct event_base* evbase, int mode = ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB)
    {
        _context = context;
        ZmTapDelegate::StartTapDelegate(evbase, mode);
    }

    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address);
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen);
    virtual void OnTapDelegateEvent(short what);

    /** Add multiple dummy listening port */
    uint16_t AddDummpy(uint16_t port, const char* host = nullptr, ZM_HUB_PROXY_PORT_TYPE type = PROXY_PORT_NOTYPE);
    void RemoveDummpy(uint16_t port, const char* host = nullptr);
    void SetJrpcDelegate(ZmTapDelegateJRPC* DelegateJRPC) { _delegate_jrpc = DelegateJRPC; }


    ZmTapContext* TapContext() { return _context; }
private:


protected:
    virtual bool OnStartTap();
    virtual void OnStopTap();

private:
    ZmArrayList<ZM_HUB_LISTENER>    _dummies;

    ZmTapDelegateJRPC* _delegate_jrpc;

    ZmTapContext* _context;
};

#endif  // ZM_NET_TAP_HUB_H
