#include "zm_net_tap_hub.h"

#include "zm_net_ip.h"
#include "../util/zm_util_libevent.h"
#include "../spdlog/zm_logger.h"
#include "../util/zm_util_sys.h"

#define ZM_PROXY_LISTEN_IP   "127.0.0.1"
#define ZM_DEFAULT_PROXY_LISTENER_NUM  8

/**
 * IPv4 到 IPv6 双栈参考：
 * http://beej.us/guide/bgnet/output/html/multipage/ip4to6.html
 * http://osr600doc.sco.com/en/SDK_netapi/sockC.PortIPv4appIPv6.html
 */
////////////////////////////////////////////////////////////////////////////////////////////////
// ZmTapHubBase

ZmTapHubBase::ZmTapHubBase()
{
}

ZmTapHubBase::~ZmTapHubBase()
{
}

void ZmTapHubBase::CloseListener(ZM_HUB_LISTENER* listener)
{
    if (listener)
    {
        if (listener->v4)
        {
            evconnlistener_disable(listener->v4);
            evconnlistener_free(listener->v4);
        }
        if (listener->v6)
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
    uint16_t         port    = sock_name ? htons(atoi(sock_name) & 0x00FFFF) : 0;

    PUBLIC_LOG_INFO("ListenEV family={}, addr={}:{}", family, addr, sock_name);

    if (family == AF_INET6)
    {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)heap.Head();
        socklen = sizeof(struct sockaddr_in6);

        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = port;
        // in6addr_loopback / IN6ADDR_LOOPBACK_INIT / in6addr_any / IN6ADDR_ANY_INIT
        if (1 != evutil_inet_pton(AF_INET6, addr, &(sin6->sin6_addr)))
        {
            if (0 == _stricmp("any", addr) || 0 == _stricmp("0.0.0.0", addr)
                || 0 == _stricmp("::", addr) || 0 == _stricmp("[::]", addr))
            {
                memcpy(&sin6->sin6_addr, &in6addr_any, sizeof(struct in6_addr));
            }
            else
            {
                memcpy(&sin6->sin6_addr, &in6addr_loopback, sizeof(struct in6_addr));
            }
        }
    }
    else
    {
        struct sockaddr_in* sin4 = (struct sockaddr_in*)heap.Head();
        socklen = sizeof(struct sockaddr_in);

        // 使用前清零 sockaddr，防止平台特定字段干扰
        /** 设置为 INET 地址族 */
        sin4->sin_family = AF_INET;
        sin4->sin_port   = port;
        // INADDR_LOOPBACK / INADDR_ANY
        if (1 != evutil_inet_pton(AF_INET, addr, &(sin4->sin_addr)))
        {
            if (0 == _stricmp("any", addr) || 0 == _stricmp("0.0.0.0", addr))
            {
                sin4->sin_addr.s_addr = INADDR_ANY;
            }
            else
            {
                /** 未识别的地址回退到本地回环（INADDR_LOOPBACK = 127.0.0.1） */
                sin4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            }
        }
    }
    if (socklen > 0)
    {
        return evconnlistener_new_bind(evbase, (nullptr == cb) ? ZmTapContextEventHandler::OnRequesterAcceptConnCB : cb, ctx,
            ZM_EVENT_LISTEN_FLAGS, SOMAXCONN,
            sin, (int)socklen);
    }
    return nullptr;
}

/**
 * Winsock 双栈说明：
 *   - Windows 需要显式分别绑定 IPv4 和 IPv6，仅绑定 IPv6 不会隐式绑定 IPv4
 *   - Vista 以下自动双栈，Vista+ 需通过 setsockopt(IPV6_V6ONLY, 0) 启用
 *   - Linux 默认 net.ipv6.bindv6only=0 时绑定 IPv6 会隐式绑定 IPv4，部分发行版改为 1
 *   - 跨平台最稳妥做法：显式同时绑定 IPv4 + IPv6
 * 参考：
 *   https://msdn.microsoft.com/en-us/library/windows/desktop/bb513665(v=vs.85).aspx
 *   https://stackoverflow.com/a/20658285
 *   https://stackoverflow.com/questions/30184377/how-to-detect-if-dual-stack-socket-is-supported/30198991
 */
bool ZmTapHubBase::Listen(ZM_HUB_LISTENER* listener, struct event_base* evbase, evconnlistener_cb cb,
                          void* ctx, const char* addr, bool v4only, const char* sock_name)
{
    CloseListener(listener);

    if (ZmString::IsEmpty(sock_name) || ZmString::IsNumeric(sock_name))
    {
        listener->v4 = ListenEV(evbase, cb, ctx, addr, AF_INET, sock_name);
    }
    else
    {
        listener->v4 = ListenEV(evbase, cb, ctx, addr, AF_UNIX, sock_name);
    }

    if (listener->v4)
    {
        sockaddr_in     sa      = { 0 };
        socklen_t       socklen = sizeof(sa);
        evutil_socket_t fd4     = evconnlistener_get_fd(listener->v4);
        if (0 == getsockname(fd4, (struct sockaddr*)&sa, &socklen))
        {
            if (sa.sin_family == AF_UNIX)
            {
                PUBLIC_LOG_INFO("Listening on local: {}", sock_name);
            }
            else
            {
                listener->port = ntohs(sa.sin_port);
                PUBLIC_LOG_INFO("Listening tcpv4 succeeded: {}:{}", addr, listener->port);

                if (!v4only)
                {
                    // macOS 查看监听端口：lsof -n -iTCP:$PORT | grep LISTEN
                    /** 监听 0.0.0.0 时自动启用 IPv4+IPv6 双栈 */
                    char portstr[8] = {0};
                    const char* addrv6 = addr;
                    if (ZmString::IsEmpty(addr) || 0 == strcmp("127.0.0.1", addr))
                    {
                        addrv6 = "::1";
                    }
                    else if (0 == strcmp("0.0.0.0", addr))
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
                        PUBLIC_LOG_ERROR("Listening tcpv6 on {}:{} failed, errMsg={}", addrv6, portstr, ZmSystem::ErrMsg(-1));
                    }
                }
            }
        }
        else
        {
            listener->port = 0;
            PUBLIC_LOG_ERROR("getsockname failed for listener v4, fd={}, errMsg={}", fd4, ZmSystem::ErrMsg(-1));
        }
    }
    else
    {
        PUBLIC_LOG_ERROR("Listening tcpv4 on {}:{} failed, errMsg={}", addr, sock_name, ZmSystem::ErrMsg(-1));
    }
    return (nullptr != listener->v4 || nullptr != listener->v6);
}


////////////////////////////////////////////////////////////////////////////////////////////////
// ZmTapHubProxy —— 构造与析构

/**
 * @brief 构造 Hub 代理，初始化监听器列表和成员变量
 */
ZmTapHubProxy::ZmTapHubProxy()
    : m_proxy_listeners(ZM_DEFAULT_PROXY_LISTENER_NUM)
    , m_delegate_jrpc(nullptr)
    , m_context(nullptr)
{
    TapDelegateName("ZmTapHubProxy");
}

/**
 * @brief 析构（资源由 OnStopTap 和基类 StopTapDelegate 释放）
 */
ZmTapHubProxy::~ZmTapHubProxy()
{
}

// --- 生命周期管理 ---

/**
 * @brief 启动 Hub 代理，关联 TAP 上下文池并绑定到事件循环
 * @param context TAP 上下文池指针
 * @param evbase  libevent 事件循环基
 * @param mode    工作模式（默认内部 Hub 代理）
 */
void ZmTapHubProxy::StartTapDelegate(ZmTapContext* context, struct event_base* evbase, int mode)
{
    m_context = context;
    ZmTapDelegate::StartTapDelegate(evbase, mode);
}

// --- 端口管理 ---

/**
 * @brief 添加一个代理监听端口
 * @param port 期望端口号（0 表示由系统自动分配）
 * @param host 监听地址（为空时默认 127.0.0.1）
 * @param type 端口类型（预留扩展，当前仅 PROXY_PORT_NOTYPE）
 * @return 实际绑定的端口号，失败返回 0
 */
uint16_t ZmTapHubProxy::AddListenPort(uint16_t port, const char* host, ZM_HUB_PROXY_PORT_TYPE type)
{
    if (!m_evbase)
    {
        PUBLIC_LOG_ERROR("AddListenPort failed: event base not ready, Host: {}, Port: {}", host, port);
        return 0;
    }

    if (port > 0)
    {
        for (size_t i = 0; i < m_proxy_listeners.Count(); i++)
        {
            if (m_proxy_listeners.At(i)->port == port
                && !strcmp(m_proxy_listeners.At(i)->host, ZmString::IsEmpty(host) ? ZM_PROXY_LISTEN_IP : host))
            {
                return port;
            }
        }
    }

    ZM_HUB_LISTENER* listener = m_proxy_listeners.Add();
    if (!listener)
    {
        PUBLIC_LOG_ERROR("AddListenPort failed: m_proxy_listeners.Add() returned nullptr, Host: {}, Port: {}", host, port);
        return 0;
    }

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

    // 回调分类（预留 SOCK5 等协议扩展）
    //if (type == PROXY_PORT_SOCK5) {
    //    cb = ZmTapContextEventHandler::OnRequesterSOCK5AcceptConnCB;
    //}

    char pstr[16] = { 0 };
    bool bListen = false;

    bListen = Listen(listener, m_evbase, cb, this, listener->host,
        false, ZmString::L_To_A(listener->port, pstr));

    if (bListen)
    {
        PUBLIC_LOG_INFO("AddListenPort, Host: {}, Port: {}, Success", host, listener->host, listener->port);
    }
    else
    {
        CloseListener(listener);
        m_proxy_listeners.Remove(m_proxy_listeners.OffsetOf(listener));
        PUBLIC_LOG_ERROR("AddListenPort, Host: {}, Port: {}, Failed", listener->host, listener->port);
    }

    return (bListen) ? listener->port : 0;
}

/**
 * @brief 移除指定的代理监听端口
 * @param port 要移除的端口号
 * @param host 要移除的监听地址（为空时默认 127.0.0.1，与 AddListenPort 保持一致）
 */
void ZmTapHubProxy::RemoveListenPort(uint16_t port, const char* host)
{
    const char* cmp_host = ZmString::IsEmpty(host) ? ZM_PROXY_LISTEN_IP : host;
    for (size_t i = 0; i < m_proxy_listeners.Count(); i++)
    {
        if (m_proxy_listeners.At(i)->port == port && 0 == strcmp(m_proxy_listeners.At(i)->host, cmp_host))
        {
            CloseListener(m_proxy_listeners.At(i));
            m_proxy_listeners.Remove(i);
            return;
        }
    }
}

// --- 配置 ---

/**
 * @brief 设置 JRPC 协议委托处理器，用于协议探测成功后的 delegate 切换
 * @param DelegateJRPC JRPC delegate 指针
 */
void ZmTapHubProxy::SetJrpcDelegate(ZmTapDelegateJRPC* DelegateJRPC)
{
    m_delegate_jrpc = DelegateJRPC;
}

// --- ZmTapDelegate 接口实现 ---

/**
 * @brief accept 阶段设置探测回调，等待首包到达后识别协议类型
 *
 * IsCallbackSelfManaged() 返回 true，上层 OnRequesterAcceptConnCB 不会覆盖
 * 此处设置的回调。首包到达时 OnProtocolDetectReadCB 执行协议探测，根据魔数切换 delegate。
 */
bool ZmTapHubProxy::OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address)
{
    PUBLIC_LOG_INFO("HubProxy setting up probe callbacks for Tap: {}", (void*)tap);

    // 设置 4 字节水位线，确保首包至少包含协议魔数
    bufferevent_setwatermark(tap->requester_bev, EV_READ, 4, 0);
    bufferevent_setcb(tap->requester_bev, ZmTapHubProxy::OnProtocolDetectReadCB,
        NULL, ZmTapHubProxy::OnProtocolDetectEventCB, tap);
    bufferevent_enable(tap->requester_bev, EV_READ | EV_WRITE);

    return true;
}

/**
 * @brief 正常数据读取 — HubProxy 不应该收到此回调
 *
 * 正常情况下协议探测在 OnProtocolDetectReadCB 中完成并切换 delegate 后，
 * 后续数据会直接路由到新 delegate。如果走到这里说明状态异常。
 */
void ZmTapHubProxy::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
    if (tap->delegate->TapDelegateMode() != m_mode)
    {
        // delegate 已切换，正常路径不应走到这里
        PUBLIC_LOG_ERROR("HubProxy OnTapRequesterRead called after delegate switch, dropping Tap: {}", (void*)tap);
        tap->Drop("HubProxy unexpected read after probe");
        return;
    }

    // 走到这里说明 IsCallbackSelfManaged 未生效或 probe 未触发
    PUBLIC_LOG_ERROR("HubProxy OnTapRequesterRead called in HUB mode (probe should have handled this), dropping Tap: {}", (void*)tap);
    tap->Drop("HubProxy unexpected read");
}

/**
 * @brief delegate 内部事件回调（当前无内部事件需要处理）
 * @param what 事件标志位
 */
void ZmTapHubProxy::OnTapDelegateEvent(short what)
{
}

/**
 * @brief 标记为自行管理 bufferevent 回调，禁止上层 OnRequesterAcceptConnCB 覆盖
 * @return 始终返回 true
 */
bool ZmTapHubProxy::IsCallbackSelfManaged()
{
    return true;
}

/**
 * @brief 获取关联的 TAP 上下文池
 * @return ZmTapContext 指针
 */
ZmTapContext* ZmTapHubProxy::TapContext()
{
    return m_context;
}

// --- 生命周期回调 ---

/**
 * @brief delegate 启动回调（当前无需额外初始化）
 * @return 始终返回 true
 */
bool ZmTapHubProxy::OnStartTap()
{
    return true;
}

/**
 * @brief delegate 停止回调，关闭所有代理监听端口
 */
void ZmTapHubProxy::OnStopTap()
{
    for (size_t i = 0; i < m_proxy_listeners.Count(); i++)
    {
        CloseListener(m_proxy_listeners.At(i));
    }
}

// --- 协议探测回调 ---

/**
 * @brief 协议探测读取回调 — 仅在连接建立后首次触发
 *
 * 读取首包前 4 字节魔数，识别协议类型后：
 *   1. 消耗魔数字节
 *   2. 切换到对应的协议 delegate
 *   3. 替换 bufferevent 回调为标准读写回调
 *   4. 将剩余数据交给新 delegate 处理
 */
void ZmTapHubProxy::OnProtocolDetectReadCB(struct bufferevent* bev, void* ctx)
{
    ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
    if (!tap || !tap->delegate)
    {
        if (bev) bufferevent_free(bev);
        return;
    }

    ZmTapHubProxy* self = static_cast<ZmTapHubProxy*>(tap->delegate);
    struct evbuffer* input = bufferevent_get_input(bev);
    size_t datalen = evbuffer_get_length(input);

    if (datalen < 4)
    {
        // 数据不足，继续等待（水位线已保证不应走到这里）
        return;
    }

    unsigned char* head = evbuffer_pullup(input, 4);
    if (!head)
    {
        self->TapContext()->Drop(tap, "Probe pullup failed");
        return;
    }

    // 识别协议魔数
    if (head[0] == 'J' && head[1] == 'R' && head[2] == 'P' && head[3] == 'C')
    {
        if (!self->m_delegate_jrpc)
        {
            PUBLIC_LOG_ERROR("JRPC protocol detected but delegate not set, dropping Tap: {}", (void*)tap);
            self->TapContext()->Drop(tap, "JRPC delegation not set");
            return;
        }

        PUBLIC_LOG_INFO("HubProxy probe detected JRPC protocol, switching delegate for Tap: {}", (void*)tap);
        evbuffer_drain(input, 4);
        // 切换到正式 delegate
        self->SwitchDelegate(tap, self->m_delegate_jrpc);
    }
    else
    {
        PUBLIC_LOG_INFO("Unrecognized protocol magic in probe, dropping Tap: {}", (void*)tap);
        self->TapContext()->Drop(tap, "Unrecognized message type");
        return;
    }

    ZmTapContext::SetDropTimer(tap, 30);
    // 将魔数之后的数据交给新 delegate
    tap->delegate->OnTapRequesterRead(tap, input, datalen - 4);
}

/**
 * @brief 协议探测事件回调 — 处理探测阶段的连接异常
 */
void ZmTapHubProxy::OnProtocolDetectEventCB(struct bufferevent* bev, short events, void* ctx)
{
    ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
    if (!tap || !tap->delegate) return;

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    {
        PUBLIC_LOG_INFO("Probe connection closed/error, dropping Tap: {}", (void*)tap);
        tap->delegate->OnTapRequesterEvent(tap, bev, events);
        tap->Drop("Probe connection closed");
    }
}

// --- 内部方法 ---

/**
 * @brief 执行 delegate 切换后的回调替换
 *
 * 将 bufferevent 回调从探测回调切换为标准读写回调，
 * 后续数据直接路由到新 delegate。
 */
void ZmTapHubProxy::SwitchDelegate(ZM_TAP_CTX* tap, ZmTapDelegate* new_delegate)
{
    ZmTapDelegate* old_delegate = tap->delegate;

    tap->delegate = new_delegate;

    // 替换回调为标准读写回调
    bufferevent_setcb(tap->requester_bev,
        ZmTapContextEventHandler::OnRequesterReadCB,
        NULL,
        ZmTapContextEventHandler::OnRequesterEventCB,
        tap);
    bufferevent_setwatermark(tap->requester_bev, EV_READ, 0, ZM_BUF_WATERMARK_HIGH);

    PUBLIC_LOG_INFO("SwitchDelegate: {} -> {}, Tap: {}",
        old_delegate->TapDelegateName(), new_delegate->TapDelegateName(), (void*)tap);
}
