#ifndef ZM_NET_TAP_HUB_H
#define ZM_NET_TAP_HUB_H

#include "zm_net_tap.h"

#include <../libevent/include/event2/listener.h>

// 前向声明（头文件中仅通过指针使用）
class ZmTapDelegateJRPC;

// ============================================================================
// ZmTapHubBase — 监听基础设施
// ============================================================================

/**
 * @brief TAP Hub 基类，提供 IPv4/IPv6 双栈监听基础设施
 *
 * 封装 evconnlistener 的创建、绑定和生命周期管理，
 * 供 ZmTapHubProxy 等子类复用监听逻辑。
 */
class ZmTapHubBase : public ZmTapDelegate
{
public:
	ZmTapHubBase(struct event_base* evbase);
	virtual ~ZmTapHubBase();

	// --- ZmTapDelegate 默认实现（子类按需重写）---

	/** @brief 接受新连接（默认接受）
	 *  @return 始终返回 true */
	bool OnTapRequesterAccept(ZM_TAP_CTX* tap) override;

	/** @brief 数据读取回调（Hub 模式下不直接读数据） */
	void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) override;

	/** @brief delegate 内部事件回调（默认无操作） */
	void OnTapDelegateEvent(short what) override;

protected:
	/** @brief 监听器描述符，封装 IPv4/IPv6 双栈 evconnlistener */
	struct ZM_HUB_LISTENER
	{
		char                    host[64];       ///< 监听地址（如 127.0.0.1 或 ::1）
		uint16_t                port;           ///< 监听端口号
		struct evconnlistener*  v4;             ///< IPv4 evconnlistener 句柄
		struct evconnlistener*  v6;             ///< IPv6 evconnlistener 句柄
	};

	/** @brief 关闭监听器并释放 IPv4/IPv6 evconnlistener 资源
	 *  @param listener 要关闭的监听器 */
	void            CloseListener(ZM_HUB_LISTENER* listener);

	/** @brief 创建单个协议族的 evconnlistener
	 *  @param evbase    libevent 事件循环基
	 *  @param cb        连接回调（nullptr 则使用默认 OnRequesterAcceptConnCB）
	 *  @param ctx       回调透传参数
	 *  @param addr      监听地址字符串
	 *  @param family    地址族（AF_INET / AF_INET6 / AF_UNIX）
	 *  @param sock_name 端口号字符串（非数字时按 AF_UNIX 路径处理）
	 *  @return 成功返回 evconnlistener 指针，失败返回 nullptr */
	evconnlistener* ListenEV(struct event_base* evbase, evconnlistener_cb cb, void* ctx,
	                         const char* addr, uint16_t family = AF_INET, const char* sock_name = nullptr);

	/** @brief 同时创建 IPv4 + IPv6 双栈监听
	 *  @param listener  监听器描述符
	 *  @param evbase    libevent 事件循环基
	 *  @param cb        连接回调
	 *  @param ctx       回调透传参数
	 *  @param addr      监听地址
	 *  @param v4only    仅 IPv4（Windows 双栈或 Unix 路径时自动忽略 v6）
	 *  @param sock_name 端口号或 Unix socket 路径
	 *  @return 任一协议族监听成功则返回 true */
	bool            Listen(ZM_HUB_LISTENER* listener, struct event_base* evbase, evconnlistener_cb cb,
	                       void* ctx, const char* addr, bool v4only = false, const char* sock_name = nullptr);
};

// ============================================================================
// ZmTapHubProxy — 协议探测 + 路由分发
// ============================================================================

/** @brief 代理端口类型枚举（预留 SOCKS5 等扩展） */
typedef enum {
	PROXY_PORT_NOTYPE = 0,  ///< 默认类型（无特殊协议）
} ZM_HUB_PROXY_PORT_TYPE;

/**
 * @brief TAP Hub 代理，负责连接监听、协议探测和 delegate 路由
 *
 * 每个新连接到达时通过魔数探测识别协议类型，然后切换到对应的协议 delegate
 * （如 JRPC、预留 SOCKS5）。内部持有 TAP 上下文池和 JRPC delegate 引用。
 */
class ZmTapHubProxy : public ZmTapHubBase
{
public:
	ZmTapHubProxy(struct event_base* evbase);
	virtual ~ZmTapHubProxy();

	// --- 生命周期 ---

	/** @brief 启动 Hub 代理，关联 TAP 上下文池并绑定事件循环
	 *  @param context TAP 上下文池指针
	 *  @param evbase  libevent 事件循环基
	 *  @param mode    工作模式（默认 ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB） */
	void StartTapDelegate(int mode = ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB);

	// --- 端口管理 ---

	/** @brief 添加一个代理监听端口
	 *  @param port 期望端口号（0 表示系统自动分配）
	 *  @param host 监听地址（为空时默认 127.0.0.1）
	 *  @param type 端口类型（预留扩展，当前仅 PROXY_PORT_NOTYPE）
	 *  @return 实际绑定的端口号，失败返回 0 */
	uint16_t AddListenPort(uint16_t port, const char* host = nullptr,
	                       ZM_HUB_PROXY_PORT_TYPE type = PROXY_PORT_NOTYPE);

	/** @brief 移除指定的代理监听端口
	 *  @param port 要移除的端口号
	 *  @param host 监听地址（为空时默认 127.0.0.1） */
	void     RemoveListenPort(uint16_t port, const char* host = nullptr);

	// --- 配置 ---

	/** @brief 设置 JRPC 协议委托处理器
	 *  @param DelegateJRPC JRPC delegate 指针 */
	void SetJrpcDelegate(ZmTapDelegateJRPC* DelegateJRPC);

public:
	// --- ZmTapDelegate 接口实现 ---

	/** @brief 接受新连接并设置协议探测回调
	 *
	 * 设置 4 字节读水位线和 OnProtocolDetectReadCB 回调，
	 * 首包到达后自动识别协议类型并切换 delegate。
	 *
	 * @return 始终返回 true */
	bool OnTapRequesterAccept(ZM_TAP_CTX* tap) override;

	/** @brief 数据读取回调（正常情况下 probe 已完成 delegate 切换，不应走到这里） */
	void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) override;

	/** @brief delegate 内部事件回调（当前无操作） */
	void OnTapDelegateEvent(short what) override;

protected:
	// --- 生命周期回调 ---

	/** @brief delegate 启动回调
	 *  @return 始终返回 true */
	bool OnStartTap() override;

	/** @brief delegate 停止回调，关闭所有代理监听端口 */
	void OnStopTap() override;

	// --- 协议探测回调 ---

	/** @brief 协议探测读取回调 — 连接建立后仅触发一次，按魔数切换 delegate
	 *
	 * 从首包读取 4 字节魔数：
	 *   - "JRPC" → 切换到 ZmTapDelegateJRPC
	 *   - 其他   → Drop 连接
	 *
	 * 切换后替换 bufferevent 回调并将剩余数据交给新 delegate。
	 * @note 仅在 OnTapRequesterAccept 设置的回调中触发 */
	static void OnProtocolDetectReadCB(struct bufferevent* bev, void* ctx);

	/** @brief 协议探测事件回调 — 探测阶段连接异常时触发
	 *  @note 委托给 OnRequesterEventCB 处理，必要时 Drop TAP */
	static void OnProtocolDetectEventCB(struct bufferevent* bev, short events, void* ctx);

private:
	// --- 内部方法 ---

	/** @brief 切换 bufferevent 回调到新 delegate
	 *
	 * 将探测回调替换为标准 OnRequesterReadCB / OnRequesterEventCB，
	 * 后续数据直接路由到新 delegate。
	 *
	 * @param tap          目标 TAP 上下文
	 * @param new_delegate 要切换到的 delegate */
	void SwitchDelegate(ZM_TAP_CTX* tap, ZmTapDelegate* new_delegate);

	// --- 成员变量 ---

	ZmArrayList<ZM_HUB_LISTENER> m_proxy_listeners;  ///< 代理监听端口列表
	ZmTapDelegateJRPC*           m_delegate_jrpc;      ///< JRPC 协议委托处理器
};

#endif  // ZM_NET_TAP_HUB_H
