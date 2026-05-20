#ifndef ZM_NET_TAP_JRPC_H
#define ZM_NET_TAP_JRPC_H

#include <vector>
#include <string>
#include <unordered_map>

#include "zm_net_tap.h"


class ZmTapDelegateJRPC : public ZmTapDelegate
{
public:
    ZmTapDelegateJRPC();
    virtual ~ZmTapDelegateJRPC();
    
public:
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) { return false; }
    virtual void OnTapDelegateEvent(short what) {}

    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap,  struct evbuffer* app_input,  size_t datalen);
    virtual void OnTapDelegateBackEvent(ZM_TAP_CTX* tap, int errcode=0);
        
private:
    void WriteResponse(ZM_TAP_CTX* tap, const char* jstr, size_t dlen);
};

#endif  // ZM_NET_TAP_JRPC_H
