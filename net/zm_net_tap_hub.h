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

    // ZmTapDelegate 接口默认实现（Hub 模式下通常不直接使用）
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) { return true; }
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) {}
    virtual void OnTapDelegateEvent(short what) {}

protected:
    /** @brief 监听器描述符 */
    struct ZM_HUB_LISTENER
    {
        char                    host[64];
        uint16_t                port;
        struct evconnlistener*  v4;
        struct evconnlistener*  v6;
    };

    /** @brief 创建单个协议的 evconnlistener */
    evconnlistener* ListenEV(struct event_base* evbase, evconnlistener_cb cb, void* ctx,
                             const char* addr, uint16_t family = AF_INET, const char* sock_name = NULL);
    /** @brief 同时创建 IPv4 + IPv6 双栈监听 */
    bool            Listen(ZM_HUB_LISTENER* listener, struct event_base* evbase,
                           evconnlistener_cb cb, void* ctx,
                           const char* addr, bool v4only = false, const char* sock_name = NULL);
    /** @brief 关闭监听器 */
    void            CloseListener(ZM_HUB_LISTENER* listener);

    ZM_HUB_LISTENER m_listener;
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

    void StartTapDelegate(ZmTapContext* context, struct event_base* evbase, int mode = ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB);
    ZmTapContext* TapContext();

    /** @brief 添加一个 dummy 监听端口，返回实际绑定的端口号 */
    uint16_t AddDummy(uint16_t port, const char* host = nullptr, ZM_HUB_PROXY_PORT_TYPE type = PROXY_PORT_NOTYPE);
    /** @brief 移除指定的 dummy 监听端口 */
    void     RemoveDummy(uint16_t port, const char* host = nullptr);
    /** @brief 设置 JRPC 协议委托处理器 */
    void     SetJrpcDelegate(ZmTapDelegateJRPC* DelegateJRPC);

    // ZmTapDelegate 接口
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) override;
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) override;
    virtual void OnTapDelegateEvent(short what) override;
    virtual bool IsCallbackSelfManaged() override;

protected:
    virtual bool OnStartTap() override;
    virtual void OnStopTap() override;

    /** @brief 协议探测读取回调 — 连接建立后首次触发，识别协议魔数 */
    static void OnProbeReadCB(struct bufferevent* bev, void* ctx);
    /** @brief 协议探测事件回调 — 处理探测阶段的连接异常 */
    static void OnProbeEventCB(struct bufferevent* bev, short events, void* ctx);

private:
    /** @brief 执行 delegate 切换并替换 bufferevent 回调 */
    void SwitchDelegate(ZM_TAP_CTX* tap, ZmTapDelegate* new_delegate);

    ZmArrayList<ZM_HUB_LISTENER> m_dummies;
    ZmTapDelegateJRPC*           m_delegate_jrpc;
    ZmTapContext*                m_context;
};

#endif  // ZM_NET_TAP_HUB_H
