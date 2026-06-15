/**
 * @file zm_net_broadcast_client.h
 * @brief TCP 广播客户端
 *
 * 基于 libevent + ZmEvBaseRunLoop 实现的 TCP 广播客户端，
 * 与服务端配套使用，自动处理握手、心跳、标签订阅和消息回调。
 */

#ifndef ZM_NET_BROADCAST_CLIENT_H
#define ZM_NET_BROADCAST_CLIENT_H

#include "zm_net_broadcast_base.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <event2/util.h>

struct bufferevent;
struct event;
class ZmEvBaseRunLoop;
class ZmThreadPool;

// ============================================================================
// 客户端配置
// ============================================================================

/**
 * @brief 广播客户端配置
 */
struct BcClientConfig
{
	std::string serverIp;               ///< 服务端 IP 地址
	uint16_t    serverPort;             ///< 服务端端口号
	int         handshakeTimeout;       ///< 握手超时秒数，默认 10
	std::vector<std::string> initialTags; ///< 初始订阅的 tag 列表
	ZmThreadPool* threadPool;           ///< 业务层消息回调线程池（必填）

	BcClientConfig()
		: serverPort(0)
		, handshakeTimeout(10)
		, threadPool(nullptr)
	{}
};

// ============================================================================
// 客户端回调
// ============================================================================

/**
 * @brief 广播客户端事件回调集合
 */
struct BcClientCallbacks
{
	/// 连接成功 + 握手完成回调，参数为服务端 IP 和端口
	std::function<void(const std::string& serverIp, uint16_t port)> onConnected;

	/// 连接失败回调，参数为错误描述
	std::function<void(const std::string& error)> onConnectFailed;

	/// 断开回调（主动断开 / 服务端断开 / 错误）
	std::function<void()> onDisconnected;

	/// 错误回调
	std::function<void(const std::string& error)> onError;

	/// 业务消息回调（线程池线程中执行），参数为 topic 和 content
	std::function<void(const std::string& topic, const std::string& content)> onMessage;
};

// ============================================================================
// ZmBroadcastClient
// ============================================================================

/**
 * @brief TCP 广播客户端
 *
 * 使用方法:
 * @code
 *   BcClientConfig cfg;
 *   cfg.serverIp = "127.0.0.1";
 *   cfg.serverPort = 39640;
 *   cfg.threadPool = &myPool;
 *   cfg.initialTags = {"tag1", "tag2"};
 *
 *   BcClientCallbacks cbs;
 *   cbs.onMessage = [](const std::string& topic, const std::string& content) {
 *       printf("收到: %s\n", content.c_str());
 *   };
 *
 *   ZmBroadcastClient client(cfg, cbs);
 *   client.Connect();
 *   // ...
 *   client.Disconnect();
 * @endcode
 */
class ZmBroadcastClient
{
public:
	// --- 构造 / 析构 ---

	/**
	 * @brief 构造广播客户端
	 * @param config  客户端配置（threadPool 必填）
	 * @param cbs     事件回调集合
	 */
	ZmBroadcastClient(const BcClientConfig& config, const BcClientCallbacks& cbs);

	/** @brief 析构时自动断开连接 */
	~ZmBroadcastClient();

	// 禁止拷贝 / 移动
	ZmBroadcastClient(const ZmBroadcastClient&) = delete;
	ZmBroadcastClient& operator=(const ZmBroadcastClient&) = delete;

	// --- 生命周期 ---

	/**
	 * @brief 开始连接（异步，结果通过回调通知）
	 *
	 * 连接失败会自动重试（1 秒间隔，无限次）。
	 *
	 * @return true 任务已投递，false 状态不允许或配置无效
	 */
	bool Connect();

	/** @brief 断开连接并停止重试 */
	void Disconnect();

	// --- 状态查询（线程安全） ---

	/** @brief 获取当前状态 */
	ZM_BROADCAST_STATE GetState() const;

	/** @brief 获取服务端 IP */
	std::string GetServerIp() const;

	/** @brief 获取服务端端口 */
	uint16_t GetServerPort() const;

	/** @brief 获取已运行秒数（从握手完成开始计时） */
	uint64_t GetRunningTime() const;

	/** @brief 获取累计接收消息数 */
	uint64_t GetReceivedCount() const;

	// --- Tag 管理（线程安全） ---

	/**
	 * @brief 订阅标签（已在连接中则立即发送）
	 * @param tags  要新增订阅的标签列表
	 * @return true 成功
	 */
	bool Subscribe(const std::vector<std::string>& tags);

	/**
	 * @brief 取消订阅标签
	 * @param tags  要取消的标签列表
	 * @return true 成功
	 */
	bool Unsubscribe(const std::vector<std::string>& tags);

	/** @brief 获取当前订阅的 tag 列表 */
	std::vector<std::string> GetTags() const;

private:
	// ============================================================
	// 内部方法（仅在事件循环线程中调用）
	// ============================================================

	/** @brief 在事件循环线程中执行连接 */
	void DoConnect();

	/** @brief 在事件循环线程中执行断开 */
	void DoDisconnect();

	/**
	 * @brief 线程安全发送 JSON 帧
	 * @param json  完整的 JSON 字符串（不含帧头）
	 */
	void SendJson(const std::string& json);

	/** @brief 发送当前 tag 订阅列表 */
	void SendCurrentTags();

	/** @brief 调度重试连接（1 秒后） */
	void ScheduleRetry();

	/** @brief 消息路由：根据 JSON 分发到内部处理或业务回调 */
	void HandleMessage(const std::string& json);

	// ============================================================
	// 跨线程调度
	// ============================================================

	/** @brief 调度任务到事件循环线程执行 */
	enum BcTaskType
	{
		BC_CLIENT_TASK_CONNECT    = 1,
		BC_CLIENT_TASK_DISCONNECT = 2,
		BC_CLIENT_TASK_SEND       = 3,
	};

	struct BcClientTask
	{
		BcTaskType type;
		std::string json;       ///< 发送内容（仅 SEND）
	};

	void ScheduleTask(const BcClientTask& task);
	void ExecuteTask(const BcClientTask& task);

	// ============================================================
	// libevent 静态回调
	// ============================================================

	static void OnConnectCB(struct bufferevent* bev, short events, void* ctx);
	static void OnReadCB(struct bufferevent* bev, void* ctx);
	static void OnEventCB(struct bufferevent* bev, short events, void* ctx);
	static void OnRetryTimerCB(evutil_socket_t fd, short what, void* ctx);
	static void OnHandshakeTimeoutCB(evutil_socket_t fd, short what, void* ctx);
	static void OnDispatchEventCB(evutil_socket_t fd, short what, void* ctx);

	// ============================================================
	// 成员变量
	// ============================================================

	BcClientConfig    m_config;                         ///< 配置副本
	BcClientCallbacks m_callbacks;                      ///< 回调副本
	std::atomic<ZM_BROADCAST_STATE> m_state;            ///< 当前状态

	ZmEvBaseRunLoop*   m_evLoop;                        ///< 内部事件循环线程
	struct bufferevent* m_bev;                          ///< libevent bufferevent
	struct event* m_retryTimer;                         ///< 重试定时器
	struct event* m_handshakeTimer;                     ///< 握手超时定时器
	struct event* m_dispatchEvent;                      ///< 跨线程调度事件

	std::atomic<bool> m_handshakeDone;                  ///< 握手是否完成
    std::atomic<uint64_t> m_startTime;                  ///< 握手完成时间戳

	std::atomic<uint64_t> m_receivedCount;              ///< 累计接收消息数
	std::vector<std::string> m_currentTags;             ///< 当前订阅标签列表
	mutable std::mutex m_tagsMutex;                     ///< 保护 m_currentTags
	std::mutex m_sendMutex;                             ///< 保护发送队列
	std::vector<BcClientTask> m_pendingTasks;           ///< 待投递的任务队列

	std::thread::id m_loopThreadId;                     ///< 事件循环线程 ID
};

#endif // ZM_NET_BROADCAST_CLIENT_H
