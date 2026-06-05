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

    /** @brief ZmTapDelegate 接口默认实现（Hub 模式下通常不直接使用） */
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) { return true; }
    /** @brief ZmTapDelegate 数据读取默认实现（Hub 模式下无操作） */
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) {}
    /** @brief ZmTapDelegate 内部事件默认实现（Hub 模式下无操作） */
    virtual void OnTapDelegateEvent(short what) {}

protected:
    /** @brief 监听器描述符，封装单个监听地址的 IPv4/IPv6 双栈 evconnlistener */
    struct ZM_HUB_LISTENER
    {
        char                    host[64];       /** 监听地址（如 127.0.0.1 或 ::1） */
        uint16_t                port;           /** 监听端口号 */
        struct evconnlistener*  v4;             /** IPv4 evconnlistener 句柄 */
        struct evconnlistener*  v6;             /** IPv6 evconnlistener 句柄 */
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
};






/** @brief 代理端口类型枚举（预留扩展） */
typedef enum {
    PROXY_PORT_NOTYPE = 0,  /** 默认类型（无特殊协议） */
} ZM_HUB_PROXY_PORT_TYPE;

class ZmTapHubProxy : public ZmTapHubBase
{
public:
    ZmTapHubProxy();
    virtual ~ZmTapHubProxy();

    /** @brief 启动 Hub 代理 delegate，关联 TAP 上下文池并绑定事件循环 */
    void StartTapDelegate(ZmTapContext* context, struct event_base* evbase, int mode = ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB);

    /** @brief 添加一个代理监听端口，返回实际绑定的端口号 */
    uint16_t AddListenPort(uint16_t port, const char* host = nullptr, ZM_HUB_PROXY_PORT_TYPE type = PROXY_PORT_NOTYPE);
    /** @brief 移除指定的代理监听端口 */
    void     RemoveListenPort(uint16_t port, const char* host = nullptr);
    /** @brief 设置 JRPC 协议委托处理器 */
    void     SetJrpcDelegate(ZmTapDelegateJRPC* DelegateJRPC);

    // --- ZmTapDelegate 接口实现 ---
    /** @brief 接受新连接，设置协议探测回调并等待首包到达 */
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) override;
    /** @brief 数据读取回调（正常情况下不应被调用，探测完成后 delegate 已切换） */
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) override;
    /** @brief delegate 内部事件回调（当前无操作） */
    virtual void OnTapDelegateEvent(short what) override;
    /** @brief 标记为自行管理回调，禁止上层覆盖 */
    virtual bool IsCallbackSelfManaged() override;
    /** @brief 获取关联的 TAP 上下文池 */
    virtual ZmTapContext* TapContext() override;

protected:
    virtual bool OnStartTap() override;
    virtual void OnStopTap() override;

    /** @brief 协议探测读取回调 — 连接建立后首次触发，识别协议魔数 */
    static void OnProtocolDetectReadCB(struct bufferevent* bev, void* ctx);
    /** @brief 协议探测事件回调 — 处理探测阶段的连接异常 */
    static void OnProtocolDetectEventCB(struct bufferevent* bev, short events, void* ctx);

private:
    /** @brief 执行 delegate 切换并替换 bufferevent 回调 */
    void SwitchDelegate(ZM_TAP_CTX* tap, ZmTapDelegate* new_delegate);

    ZmArrayList<ZM_HUB_LISTENER> m_proxy_listeners;  /** 代理监听端口列表 */
    ZmTapDelegateJRPC*           m_delegate_jrpc;      /** JRPC 协议委托处理器 */
    ZmTapContext*                m_context;            /** 关联的 TAP 上下文池 */
};

#endif  // ZM_NET_TAP_HUB_H
