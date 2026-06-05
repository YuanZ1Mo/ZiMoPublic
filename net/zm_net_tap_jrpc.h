#ifndef ZM_NET_TAP_JRPC_H
#define ZM_NET_TAP_JRPC_H

#include <vector>
#include <string>
#include <unordered_map>

#include "zm_net_tap.h"

typedef std::function<void(ZM_TAP_CTX* tap, const char* reqData)> TapDelegateJrpcRequestReadCB;

class ZmTapDelegateJRPC : public ZmTapDelegate
{
public:
    ZmTapDelegateJRPC();
    virtual ~ZmTapDelegateJRPC();

    /** @brief 设置 JRPC 请求到达时的回调函数 */
    void SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb);

    // ZmTapDelegate 接口实现
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) override { return false; }
    virtual void OnTapDelegateEvent(short what) override {}
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) override;
    virtual void OnTapDelegateBackEvent(ZM_TAP_CTX* tap) override;

private:
    /** @brief 向客户端写入 JSON-RPC 响应（长度前缀 + JSON） */
    void WriteResponse(ZM_TAP_CTX* tap, const char* json_str, size_t data_len);

    TapDelegateJrpcRequestReadCB m_tapDelegateJrpcRequestReadCB;
};

#endif  // ZM_NET_TAP_JRPC_H
