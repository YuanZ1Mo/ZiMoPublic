#include "zm_net_tap_hub.h"


#include "zm_net_ip.h"
#include "../util/zm_util_libevent.h"
#include "../spdlog/zm_logger.h"
#include "../util/zm_util_sys.h"

#define ZM_PROXY_LISTEN_IP   "127.0.0.1"

//
// Jumping from IPv4 to IPv6
// http://beej.us/guide/bgnet/output/html/multipage/ip4to6.html
// http://osr600doc.sco.com/en/SDK_netapi/sockC.PortIPv4appIPv6.html
//
////////////////////////////////////////////////////////////////////////////////////////////////
// ZmTapHubBase
ZmTapHubBase::ZmTapHubBase()
{
    _listener.port = 0;
    _listener.v4 = nullptr;
    _listener.v6 = nullptr;
}

ZmTapHubBase::~ZmTapHubBase()
{}

void ZmTapHubBase::CloseListener(ZM_HUB_LISTENER* listener)
{
    if (listener )
    {
        if ( listener->v4 )
        {
            evconnlistener_disable(listener->v4);
            evconnlistener_free(listener->v4);
        }
        if ( listener->v6 )
        {
            evconnlistener_disable(listener->v6);
            evconnlistener_free(listener->v6);
        }
        listener->v4 = nullptr;
        listener->v6 = nullptr;
    }
}

struct evconnlistener* ZmTapHubBase::ListenEV(struct event_base* evbase, evconnlistener_cb cb, void* ctx,
                                              const char* addr, uint16_t family, const char* sock_name)
{
    ZmByteBuffer     heap(128);
    struct sockaddr* sin     = (struct sockaddr*)heap.Head();
    size_t           socklen = 0;
    uint16_t         port    = sock_name? htons(atoi(sock_name)&0x00FFFF) : 0;
    
    PUBLIC_LOG_INFO("ListenEV family={}, addr={}:{}", family, addr, sock_name);

    if ( family==AF_INET6 )
    {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)heap.Head();
        socklen = sizeof(struct sockaddr_in6);

        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = port;
        // in6addr_loopback / IN6ADDR_LOOPBACK_INIT / in6addr_any /IN6ADDR_ANY_INIT
        if ( 1!= evutil_inet_pton(AF_INET6, addr, &(sin6->sin6_addr)) )
        {
            if ( 0== _stricmp("any", addr) || 0== _stricmp("0.0.0.0", addr)
                || 0== _stricmp("::", addr) || 0== _stricmp("[::]", addr) )
            {
                // sp_inet_pton(AF_INET6, "::", &(sin6->sin6_addr));
                // sin6->sin6_addr = in6addr_any;
                memcpy(&sin6->sin6_addr, &in6addr_any, sizeof(struct in6_addr));
            }
            else
            {
                // sp_inet_pton(AF_INET6, "::1", &(sin6->sin6_addr));
                // sin6->sin6_addr = in6addr_loopback;
                memcpy(&sin6->sin6_addr, &in6addr_loopback, sizeof(struct in6_addr));
            }
        }
    }
    else
    {
        struct sockaddr_in* sin4 = (struct sockaddr_in*)heap.Head();
        socklen = sizeof(struct sockaddr_in);

        // Clear the sockaddr before using it, in case there are extra
        // platform-specific fields that can mess us up.
        // memset(&sin, 0, sizeof(struct sockaddr_in));
        /* This is an INET address */
        sin4->sin_family      = AF_INET;
        sin4->sin_port        = port;
        // INADDR_LOOPBACK / INADDR_ANY
        if ( 1!= evutil_inet_pton(AF_INET, addr, &(sin4->sin_addr)) )
        {
            if ( 0== _stricmp("any", addr) || 0== _stricmp("0.0.0.0", addr) )
            {
                // in4addr_any.s_addr
                // sin4->sin_addr.s_addr = htonl(0);
                sin4->sin_addr.s_addr = INADDR_ANY;
            }
            else
            {
                /* Listen on 0.0.0.0 or htonl(0x7F000001) or inet_addr("127.0.0.1") */
                // in4addr_loopback.s_addr
                // sin4->sin_addr.s_addr = htonl(0x7F000001);
                sin4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            }
        }
    }
    if ( socklen>0 )
    {
        return evconnlistener_new_bind(evbase, (nullptr == cb) ? ZmTapContextEventHandler::OnRequesterAcceptConnCB : cb, ctx,
            ZM_EVENT_LISTEN_FLAGS, SOMAXCONN,
            sin, (int)socklen);
    }
    return nullptr;
}

/**
 * Winsock 双栈 https://msdn.microsoft.com/en-us/library/windows/desktop/bb513665(v=vs.85).aspx
 *  1. 只侦听 IPv6
 *  2. 如果是低于 Vista 版本则自动双栈，否则 需通过 setsockopt 设置 IPV6_V6ONLY 为 0 启用自动双栈
 * 
 * https://stackoverflow.com/a/20658285
 *      Windows by default requires that you explicitly bind on IPv4 and IPv6. 
 *   Binding only to IPv6 will not implicitly bind to IPv4 as well.
 *      Linux by default will implicitly bind to IPv4 as well when you bind on IPv6, 
 *   only if the net.ipv6.bindv6only sysctl is set to 0. 
 *   Distributions such as Debian change this default to 1, breaking your assumption.
 *      I can't remember what Mac OS X does here (someone chirp in the comments please?),
 *   but the point is that explicitly binding to both protocols leaves no surprises.
 *
 *  https://stackoverflow.com/questions/30184377/how-to-detect-if-dual-stack-socket-is-supported/30198991
 */
bool ZmTapHubBase::Listen(ZM_HUB_LISTENER* listener, struct event_base* evbase, evconnlistener_cb cb,
                          void* ctx, const char* addr, bool v4only, const char* sock_name)
{
    CloseListener(listener);

    if ( ZmString::IsEmpty(sock_name) || ZmString::IsNumeric(sock_name) )
    {
        listener->v4 = ListenEV(evbase, cb, ctx, addr, AF_INET, sock_name);
    }
    else
    {
        listener->v4 = ListenEV(evbase, cb, ctx, addr, AF_UNIX, sock_name);
    }

    if ( listener->v4 )
    {
        sockaddr_in     sa      = { 0 };
        socklen_t       socklen = sizeof(sa);
        evutil_socket_t fd4     = evconnlistener_get_fd(listener->v4);
        if ( 0==getsockname(fd4, (struct sockaddr*)&sa, &socklen) )
        {
            if ( sa.sin_family==AF_UNIX )
            {
                PUBLIC_LOG_INFO("Listening on local: {}", sock_name);
            }
            else
            {
                listener->port = ntohs(sa.sin_port);
                PUBLIC_LOG_INFO("Listening tcpv4 succeeded: {}:{}", addr, listener->port);
                
                if ( !v4only )
                {
                    // macOS: lsof -n -iTCP:$PORT | grep LISTEN
                    /** auto IPv4+IPv6 while listen at 0.0.0.0 ? */
                    // listen the same port on ipv6 [::1]
                    char portstr[8] = {0};
                    memset(portstr, 0, sizeof(portstr));
                    const char* addrv6 = addr;
                    if ( ZmString::IsEmpty(addr) || 0==strcmp("127.0.0.1", addr) )
                    {
                        addrv6 = "::1";
                    }
                    else if ( 0==strcmp("0.0.0.0", addr) )
                    {
                        addrv6 = "::";
                    }
                    listener->v6 = ListenEV(evbase, cb, ctx, addrv6, AF_INET6, ZmString::L_To_A(listener->port, portstr));
                    if (nullptr != listener->v6)
                    {
                        PUBLIC_LOG_INFO("Listening tcpv6 succeeded: {}:{}", addrv6, listener->port);

                    }
                    else
                    {
                        PUBLIC_LOG_ERROR("Listening tcpv6 on '{}:{} failed, errMsg={}", addrv6, portstr, ZmSystem::ErrMsg(-1));
                    }
                }
            }
        }
    }
    else
    {
        PUBLIC_LOG_ERROR("Listening tcpv4 on {}:{} failed, errMsg={}", addr, sock_name, ZmSystem::ErrMsg(-1));
    }
    return (nullptr != listener->v4 || nullptr != listener->v6);
}







////////////////////////////////////////////////////////////////////////////////////////////////
// ZmTapHubProxy
ZmTapHubProxy::ZmTapHubProxy() : _dummies(8)
{
    TapDelegateName("ZmTapHubProxy");
}

ZmTapHubProxy::~ZmTapHubProxy()
{

}


bool ZmTapHubProxy::OnStartTap()
{
    return true;
}

void ZmTapHubProxy::OnStopTap()
{
    for (size_t i = 0; i < _dummies.Count(); i++)
    {
        CloseListener(_dummies.At(i));
    }
    CloseListener(&_listener);
}

uint16_t ZmTapHubProxy::AddDummpy(uint16_t port, const char* host, ZM_HUB_PROXY_PORT_TYPE type)
{
    if (port >= 0)
    {
        if (port > 0)
        {
            for (size_t i = 0; i < _dummies.Count(); i++)
            {
                if (_dummies.At(i)->port == port && strcmp(_dummies.At(i)->host, ZmString::IsEmpty(host) ? ZM_PROXY_LISTEN_IP : host))
                {
                    return port;
                }
            }
        }

        ZM_HUB_LISTENER* listener = _dummies.Add();

        if (_evbase)
        {
            if (ZmString::IsEmpty(host))
            {
                strncpy_s(listener->host, ZM_PROXY_LISTEN_IP, sizeof(listener->host));
                listener->host[sizeof(listener->host) - 1] = '\0';
            }
            else
            {
                strncpy_s(listener->host, host, sizeof(listener->host));
                listener->host[sizeof(listener->host) - 1] = '\0';
            }

            evconnlistener_cb cb = nullptr;

            //回调分类
            //if (type == PROXY_PORT_SOCK5) {
            //    cb = ZmTapContextEventHandler::OnRequesterSOCK5AcceptConnCB;
            //}

            int failed_count = 0;
            char pstr[16] = { 0 };
            bool bListen = false;

            bListen = Listen(listener, _evbase, cb, this, listener->host,
                false, ZmString::L_To_A(listener->port, pstr));

            if (bListen)
            {
                PUBLIC_LOG_INFO("AddDummpy,Host: {}, Port: {}, Success", host, listener->host, listener->port);
            }
            else
            {
                CloseListener(listener);
                _dummies.Remove(_dummies.OffsetOf(listener));
                PUBLIC_LOG_ERROR("AddDummpy,Host: {}, Port: {}, Failed", listener->host, listener->port);
            }

            return (bListen) ? listener->port : 0;
        }
    }
    //start listenning port failed
    return 0;
}

void ZmTapHubProxy::RemoveDummpy(uint16_t port, const char* host)
{
    for (size_t i = 0; i < _dummies.Count(); i++)
    {
        if (_dummies.At(i)->port == port && strcmp(_dummies.At(i)->host, host))
        {
            CloseListener(_dummies.At(i));
            _dummies.Remove(i);
            return;
        }
    }
}


void ZmTapHubProxy::OnTapDelegateEvent(short what)
{

}

bool ZmTapHubProxy::OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address)
{
    return true;
}


void ZmTapHubProxy::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
    if (tap->delegate_mode == _mode)
    {
        /** Probe the tunnel type */
        unsigned char* head = evbuffer_pullup(app_input, ZM_MIN(20, datalen));
        if (datalen > 4 && head[0] == 'J' && head[1] == 'R' && head[2] == 'P' && head[3] == 'C')
        {
            if (_delegate_jrpc)
            {
                ZmTapContext::SetDropTimer(tap, 30);
                evbuffer_drain(app_input, 4);
                tap->delegate_mode = _delegate_jrpc->TapDelegateMode();
                tap->delegate = _delegate_jrpc;
                return tap->delegate->OnTapRequesterRead(tap, app_input, datalen - 4);
            }
            else
            {
                tap->tap_context->Drop(tap, "JRPC delegation not set");
            }
        }
        else
        {
            PUBLIC_LOG_INFO("Detected the incoming message type failed, dropping Tap: {}", (void*)tap);
            tap->tap_context->Drop(tap, "Unrecognized message type");
        }
    }
    else
    {
        tap->tap_context->Drop(tap, "Error mode on Requester-Reading");
    }
}