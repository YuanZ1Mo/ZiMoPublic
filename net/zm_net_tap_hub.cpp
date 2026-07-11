#include "zm_net_tap_hub.h"

#include "zm_net_tap_jrpc.h"
#include "zm_net_tap_rest.h"
#include "zm_net_ip.h"
#include "../util/zm_util_libevent.h"
#include "../spdlog/zm_logger.h"
#include "../util/zm_util_sys.h"

#define ZM_PROXY_LISTEN_IP   "127.0.0.1"
#define ZM_DEFAULT_PROXY_LISTENER_NUM  8

// ============================================================================
// ZmTapHubBase — 构造与析构
// ============================================================================

ZmTapHubBase::ZmTapHubBase(struct event_base* evbase)
	: ZmTapDelegate(evbase)
{
}

ZmTapHubBase::~ZmTapHubBase()
{
}

// ============================================================================
// ZmTapHubBase — ZmTapDelegate 默认实现
// ============================================================================

bool ZmTapHubBase::OnTapRequesterAccept(ZM_TAP_CTX* tap)
{
	ZM_UNUSED(tap);
	return true;
}

void ZmTapHubBase::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
	ZM_UNUSED(tap);
	ZM_UNUSED(app_input);
	ZM_UNUSED(datalen);
}

void ZmTapHubBase::OnTapDelegateEvent(short what)
{
	ZM_UNUSED(what);
}

// ============================================================================
// ZmTapHubBase — 监听管理
// ============================================================================

/**
 * @brief 关闭监听器并释放 IPv4/IPv6 evconnlistener 资源
 *
 * 先 disable 停止接受新连接，再 free 释放资源。
 * 同时释放 v4 和 v6 两端。
 */
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

/**
 * @brief 创建单个协议族的 evconnlistener
 *
 * 根据地址族构造对应的 sockaddr，解析并绑定指定地址和端口。
 * 地址无效时：any (0.0.0.0) / :: 回退到 INADDR_ANY / in6addr_any，
 * 其他回退到 loopback。
 */
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

		if (evutil_inet_pton(AF_INET6, addr, &(sin6->sin6_addr)) != 1)
		{
			if (_stricmp("any", addr) == 0 || _stricmp("0.0.0.0", addr) == 0
				|| _stricmp("::", addr) == 0 || _stricmp("[::]", addr) == 0)
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

		sin4->sin_family = AF_INET;
		sin4->sin_port   = port;

		if (evutil_inet_pton(AF_INET, addr, &(sin4->sin_addr)) != 1)
		{
			if (_stricmp("any", addr) == 0 || _stricmp("0.0.0.0", addr) == 0)
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
		return evconnlistener_new_bind(evbase,
			(cb == nullptr) ? ZmTapContextEventHandler::OnRequesterAcceptConnCB : cb,
			ctx, ZM_EVENT_LISTEN_FLAGS, SOMAXCONN, sin, (int)socklen);
	}
	return nullptr;
}

/**
 * @brief 同时创建 IPv4 + IPv6 双栈监听
 *
 * Windows 需显式分别绑定 IPv4 和 IPv6，仅绑 v6 不会隐式绑 v4。
 * 127.0.0.1 → ::1（本地回环）；0.0.0.0 → ::（全接口）。
 * 跨平台最稳妥做法：显式同时绑定。
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

	if (listener->v4 != nullptr)
	{
		sockaddr_in     sa      = { 0 };
		socklen_t       socklen = sizeof(sa);
		evutil_socket_t fd4     = evconnlistener_get_fd(listener->v4);

		if (getsockname(fd4, (struct sockaddr*)&sa, &socklen) == 0)
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
					char portstr[8] = {0};
					const char* addrv6 = addr;
					if (ZmString::IsEmpty(addr) || strcmp("127.0.0.1", addr) == 0)
					{
						addrv6 = "::1";
					}
					else if (strcmp("0.0.0.0", addr) == 0)
					{
						addrv6 = "::";
					}
					listener->v6 = ListenEV(evbase, cb, ctx, addrv6, AF_INET6,
						ZmString::L_To_A(listener->port, portstr));
					if (listener->v6 != nullptr)
					{
						PUBLIC_LOG_INFO("Listening tcpv6 succeeded: {}:{}", addrv6, listener->port);
					}
					else
					{
						PUBLIC_LOG_ERROR("Listening tcpv6 on {}:{} failed, errMsg={}",
							addrv6, portstr, ZmSystem::ErrMsg(-1));
					}
				}
			}
		}
		else
		{
			listener->port = 0;
			PUBLIC_LOG_ERROR("getsockname failed for listener v4, fd={}, errMsg={}",
				fd4, ZmSystem::ErrMsg(-1));
		}
	}
	else
	{
		PUBLIC_LOG_ERROR("Listening tcpv4 on {}:{} failed, errMsg={}",
			addr, sock_name, ZmSystem::ErrMsg(-1));
	}
	return (listener->v4 != nullptr || listener->v6 != nullptr);
}

// ============================================================================
// ZmTapHubProxy — 构造与析构
// ============================================================================

ZmTapHubProxy::ZmTapHubProxy(struct event_base* evbase)
	: ZmTapHubBase(evbase)
	, m_proxy_listeners(ZM_DEFAULT_PROXY_LISTENER_NUM)
	, m_delegate_jrpc(nullptr)
	, m_delegate_restful(nullptr)
{
	TapDelegateName("ZmTapHubProxy");
}

ZmTapHubProxy::~ZmTapHubProxy()
{
}

// ============================================================================
// ZmTapHubProxy — 生命周期
// ============================================================================

void ZmTapHubProxy::StartTapDelegate(int mode)
{
	ZmTapDelegate::StartTapDelegate(mode);
}

// ============================================================================
// ZmTapHubProxy — 端口管理
// ============================================================================

/**
 * @brief 添加一个代理监听端口
 *
 * 若 port > 0 且已存在同 host:port 的监听器则直接返回已有端口号。
 * 否则创建新的 ZM_HUB_LISTENER，绑定双栈监听。
 *
 * @return 实际绑定的端口号，失败返回 0
 */
uint16_t ZmTapHubProxy::AddListenPort(uint16_t port, const char* host, ZM_HUB_PROXY_PORT_TYPE type)
{
	ZM_UNUSED(type);

	if (m_evbase == nullptr)
	{
		PUBLIC_LOG_ERROR("AddListenPort failed: event base not ready, Host: {}, Port: {}", host, port);
		return 0;
	}

	if (port > 0)
	{
		const char* cmp_host = ZmString::IsEmpty(host) ? ZM_PROXY_LISTEN_IP : host;
		for (size_t i = 0; i < m_proxy_listeners.Count(); i++)
		{
			if (m_proxy_listeners.At(i)->port == port
				&& strcmp(m_proxy_listeners.At(i)->host, cmp_host) == 0)
			{
				return port;
			}
		}
	}

	ZM_HUB_LISTENER* listener = m_proxy_listeners.Add();
	if (listener == nullptr)
	{
		PUBLIC_LOG_ERROR("AddListenPort failed: m_proxy_listeners.Add() returned nullptr, Host: {}, Port: {}",
			host, port);
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
	// 预留 SOCKS5 回调分类: if (type == PROXY_PORT_SOCKS5) cb = OnRequesterSOCKS5AcceptConnCB;

	char pstr[16] = { 0 };
	bool bListen = Listen(listener, m_evbase, cb, this, listener->host,
		false, ZmString::L_To_A(listener->port, pstr));

	if (bListen)
	{
		PUBLIC_LOG_INFO("AddListenPort, Host: {}, Port: {}, Success",
			listener->host, listener->port);
	}
	else
	{
		CloseListener(listener);
		m_proxy_listeners.Remove(m_proxy_listeners.OffsetOf(listener));
		PUBLIC_LOG_ERROR("AddListenPort, Host: {}, Port: {}, Failed",
			listener->host, listener->port);
	}

	return bListen ? listener->port : 0;
}

void ZmTapHubProxy::RemoveListenPort(uint16_t port, const char* host)
{
	const char* cmp_host = ZmString::IsEmpty(host) ? ZM_PROXY_LISTEN_IP : host;
	for (size_t i = 0; i < m_proxy_listeners.Count(); i++)
	{
		if (m_proxy_listeners.At(i)->port == port
			&& strcmp(m_proxy_listeners.At(i)->host, cmp_host) == 0)
		{
			CloseListener(m_proxy_listeners.At(i));
			m_proxy_listeners.Remove(i);
			return;
		}
	}
}

// ============================================================================
// ZmTapHubProxy — 配置
// ============================================================================

void ZmTapHubProxy::SetJrpcDelegate(ZmTapDelegateJRPC* DelegateJRPC)
{
	m_delegate_jrpc = DelegateJRPC;
}

void ZmTapHubProxy::SetRESTfulDelegate(ZmTapDelegateRESTful* DelegateRESTful)
{
	m_delegate_restful = DelegateRESTful;
}

// ============================================================================
// ZmTapHubProxy — ZmTapDelegate 接口实现
// ============================================================================

bool ZmTapHubProxy::OnTapRequesterAccept(ZM_TAP_CTX* tap)
{
	//PUBLIC_LOG_INFO("HubProxy setting up probe callbacks for Tap: {}", (void*)tap);

	/** 设置 4 字节读水位线，确保首包至少包含协议魔数 */
	bufferevent_setwatermark(tap->requester_bev, EV_READ, 4, 0);
	bufferevent_setcb(tap->requester_bev,
		ZmTapHubProxy::OnProtocolDetectReadCB,
		nullptr,
		ZmTapHubProxy::OnProtocolDetectEventCB,
		tap);
	bufferevent_enable(tap->requester_bev, EV_READ | EV_WRITE);

	return true;
}

void ZmTapHubProxy::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
	ZM_UNUSED(app_input);
	ZM_UNUSED(datalen);

	if (tap->delegate->TapDelegateMode() != m_mode)
	{
		/** delegate 已切换，正常路径不应走到这里 */
		PUBLIC_LOG_ERROR("HubProxy OnTapRequesterRead called after delegate switch, dropping Tap: {}",
			(void*)tap);
		tap->Drop("HubProxy unexpected read after probe");
		return;
	}

	/** probe 未触发或 IsCallbackSelfManaged 未生效 */
	PUBLIC_LOG_ERROR("HubProxy OnTapRequesterRead in HUB mode (probe should have handled), dropping Tap: {}",
		(void*)tap);
	tap->Drop("HubProxy unexpected read");
}

void ZmTapHubProxy::OnTapDelegateEvent(short what)
{
	ZM_UNUSED(what);
}

// ============================================================================
// ZmTapHubProxy — 生命周期回调
// ============================================================================

bool ZmTapHubProxy::OnStartTap()
{
	return true;
}

void ZmTapHubProxy::OnStopTap()
{
	for (size_t i = 0; i < m_proxy_listeners.Count(); i++)
	{
		CloseListener(m_proxy_listeners.At(i));
	}
}

// ============================================================================
// ZmTapHubProxy — 协议探测
// ============================================================================

/**
 * @brief 协议探测读取回调 — 连接建立后仅触发一次
 *
 * 读取首包前 4 字节魔数识别协议类型：
 *   - "JRPC" → evbuffer_drain(4) → SwitchDelegate → 剩余数据交新 delegate
 *   - 其他   → Drop
 */
void ZmTapHubProxy::OnProtocolDetectReadCB(struct bufferevent* bev, void* ctx)
{
	ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
	if (tap == nullptr || tap->delegate == nullptr)
	{
		if (bev != nullptr)
			bufferevent_free(bev);
		return;
	}

	ZmTapHubProxy* self = static_cast<ZmTapHubProxy*>(tap->delegate);
	struct evbuffer* input = bufferevent_get_input(bev);
	size_t datalen = evbuffer_get_length(input);

	/** 数据不足时继续等待（水位线应保证不走到这里） */
	if (datalen < 4)
		return;

	unsigned char* head = evbuffer_pullup(input, 4);
	if (head == nullptr)
	{
		tap->Drop("Probe pullup failed");
		return;
	}

	if (head[0] == 'J' && head[1] == 'R' && head[2] == 'P' && head[3] == 'C')
	{
		if (self->m_delegate_jrpc == nullptr)
		{
			PUBLIC_LOG_ERROR("JRPC protocol detected but delegate not set, dropping Tap: {}", (void*)tap);
			tap->Drop("JRPC delegation not set");
			return;
		}

		//PUBLIC_LOG_INFO("HubProxy probe detected JRPC protocol, switching delegate for Tap: {}", (void*)tap);
		evbuffer_drain(input, 4);
		self->SwitchDelegate(tap, self->m_delegate_jrpc);
	}
	else if (head[0] == 'R' && head[1] == 'E' && head[2] == 'S' && head[3] == 'T')
	{
		if (self->m_delegate_restful == nullptr)
		{
			PUBLIC_LOG_ERROR("RESTful protocol detected but delegate not set, dropping Tap: {}", (void*)tap);
			tap->Drop("RESTful delegation not set");
			return;
		}

		evbuffer_drain(input, 4);
		self->SwitchDelegate(tap, self->m_delegate_restful);
	}
	else
	{
		PUBLIC_LOG_INFO("Unrecognized protocol magic in probe, dropping Tap: {}", (void*)tap);
		tap->Drop("Unrecognized message type");
		return;
	}

	/** 将魔数之后的数据交给新 delegate 处理 */
	tap->delegate->OnTapRequesterRead(tap, input, datalen - 4);
}

void ZmTapHubProxy::OnProtocolDetectEventCB(struct bufferevent* bev, short events, void* ctx)
{
	ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
	if (tap == nullptr || tap->delegate == nullptr)
		return;

	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
	{
		//PUBLIC_LOG_INFO("Probe connection closed/error, dropping Tap: {}", (void*)tap);
		tap->delegate->OnTapRequesterEvent(tap, bev, events);
		tap->Drop("Probe connection closed");
	}
}

// ============================================================================
// ZmTapHubProxy — 内部方法
// ============================================================================

/**
 * @brief 切换 bufferevent 回调到新 delegate
 *
 * 将 bufferevent 回调从探测回调替换为标准 OnRequesterReadCB / OnRequesterEventCB，
 * 并重置读水位线，后续数据直接路由到新 delegate。
 */
void ZmTapHubProxy::SwitchDelegate(ZM_TAP_CTX* tap, ZmTapDelegate* new_delegate)
{
	ZmTapDelegate* old_delegate = tap->delegate;

	tap->delegate = new_delegate;

	bufferevent_setcb(tap->requester_bev,
		ZmTapContextEventHandler::OnRequesterReadCB,
		nullptr,
		ZmTapContextEventHandler::OnRequesterEventCB,
		tap);
	bufferevent_setwatermark(tap->requester_bev, EV_READ, 0, ZM_BUF_WATERMARK_HIGH);

	//PUBLIC_LOG_INFO("SwitchDelegate: {} -> {}, Tap: {}",
	//	old_delegate->TapDelegateName(), new_delegate->TapDelegateName(), (void*)tap);
}
