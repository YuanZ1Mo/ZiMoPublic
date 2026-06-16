/**
 * @file zm_net_broadcast_client.cpp
 * @brief TCP 广播客户端实现
 */

#include "zm_net_broadcast_client.h"

#include "../spdlog/zm_logger.h"
#include "../json/zm_json.h"
#include "../util/zm_util_thread.h"

#include <../libevent/include/event2/bufferevent.h>
#include <../libevent/include/event2/buffer.h>
#include <../libevent/include/event2/event.h>
#include <../libevent/include/event2/util.h>

#include <cstring>
#include <thread>
#include <future>

// ============================================================================
// 构造 / 析构
// ============================================================================

ZmBroadcastClient::ZmBroadcastClient(const BcClientConfig& config, const BcClientCallbacks& cbs)
	: m_config(config)
	, m_callbacks(cbs)
	, m_state(ZM_BC_STATE_IDLE)
	, m_threadPool(nullptr)
	, m_bev(nullptr)
	, m_retryTimer(nullptr)
	, m_handshakeTimer(nullptr)
	, m_dispatchEvent(nullptr)
	, m_handshakeDone(false)
	, m_startTime(0)
	, m_receivedCount(0)
	, m_currentTags(config.initialTags)
{
}

ZmBroadcastClient::~ZmBroadcastClient()
{
	Disconnect();
	// 兜底清理（Disconnect 正常路径已清理，此处防止异常路径泄漏）
	if (m_threadPool)
	{
		delete m_threadPool;
		m_threadPool = nullptr;
	}
}

// ============================================================================
// 状态查询（线程安全）
// ============================================================================

ZM_BROADCAST_STATE ZmBroadcastClient::GetState() const
{
	return m_state.load(std::memory_order_acquire);
}

std::string ZmBroadcastClient::GetServerIp() const
{
	return m_config.serverIp;
}

uint16_t ZmBroadcastClient::GetServerPort() const
{
	return m_config.serverPort;
}

uint64_t ZmBroadcastClient::GetRunningTime() const
{
	if (m_startTime.load(std::memory_order_acquire) == 0)
		return 0;
	return (BcNowMillis() - m_startTime.load(std::memory_order_acquire)) / 1000;
}

uint64_t ZmBroadcastClient::GetReceivedCount() const
{
	return m_receivedCount.load(std::memory_order_acquire);
}

// ============================================================================
// Tag 管理（线程安全）
// ============================================================================

bool ZmBroadcastClient::Subscribe(const std::vector<std::string>& tags)
{
	if (tags.empty())
		return false;

	{
		std::lock_guard<std::mutex> lock(m_tagsMutex);
		for (const auto& tag : tags)
		{
			bool found = false;
			for (const auto& existing : m_currentTags)
			{
				if (existing == tag) { found = true; break; }
			}
			if (!found)
				m_currentTags.push_back(tag);
		}
	}

	// 若已连接则立即发送标签订阅
	if (m_handshakeDone.load(std::memory_order_acquire))
	{
		ZMJSON msg;
		msg["action"] = "subscribe";
		msg["tags"] = tags;
		SendJson(zm_json_dump(msg));
	}

	return true;
}

bool ZmBroadcastClient::Unsubscribe(const std::vector<std::string>& tags)
{
	if (tags.empty())
		return false;

	{
		std::lock_guard<std::mutex> lock(m_tagsMutex);
		for (const auto& tag : tags)
		{
			auto it = std::find(m_currentTags.begin(), m_currentTags.end(), tag);
			if (it != m_currentTags.end())
				m_currentTags.erase(it);
		}
	}

	// 若已连接则立即发送取消订阅
	if (m_handshakeDone.load(std::memory_order_acquire))
	{
		ZMJSON msg;
		msg["action"] = "unsubscribe";
		msg["tags"] = tags;
		SendJson(zm_json_dump(msg));
	}

	return true;
}

std::vector<std::string> ZmBroadcastClient::GetTags() const
{
	std::lock_guard<std::mutex> lock(m_tagsMutex);
	return m_currentTags;
}

// ============================================================================
// 生命周期 — Connect
// ============================================================================

bool ZmBroadcastClient::Connect()
{
	// 状态检查：仅 IDLE 或 STOPPED 允许启动连接
	ZM_BROADCAST_STATE expected = ZM_BC_STATE_IDLE;
	ZM_BROADCAST_STATE stopped = ZM_BC_STATE_STOPPED;
	if (!m_state.compare_exchange_strong(expected, ZM_BC_STATE_STARTING) &&
		!m_state.compare_exchange_strong(stopped, ZM_BC_STATE_STARTING))
	{
		return false;
	}

	// 参数检查：外部事件循环必填
	if (m_config.serverIp.empty() || m_config.serverPort == 0 || !m_config.evbase)
	{
		m_state.store(ZM_BC_STATE_ERROR, std::memory_order_release);
		if (m_callbacks.onError)
			m_callbacks.onError("Invalid config: serverIp/serverPort/evbase required");
		return false;
	}

	// 创建内部线程池（用于业务消息回调）
	if (!m_threadPool)
	{
		m_threadPool = new ZmThreadPool(2, "BroadcastClient");
	}

	// 使用 event_base_once 在事件循环线程中执行 DoConnect
	// 此时 dispatchEvent 尚未创建，不能走 ScheduleTask 路径
	event_base_once(m_config.evbase, -1, EV_TIMEOUT,
		[](evutil_socket_t, short, void* ctx) {
			ZmBroadcastClient* client = (ZmBroadcastClient*)ctx;
			client->DoConnect();
		}, this, nullptr);

	return true;
}

void ZmBroadcastClient::DoConnect()
{
	// 释放旧的 retry 定时器（如果是重连场景）
	if (m_retryTimer)
	{
		event_free(m_retryTimer);
		m_retryTimer = nullptr;
	}

	// 记录事件循环线程 ID
	m_loopThreadId = std::this_thread::get_id();

	struct event_base* evbase = m_config.evbase;
	if (!evbase)
	{
		m_state.store(ZM_BC_STATE_ERROR, std::memory_order_release);
		if (m_callbacks.onError)
			m_callbacks.onError("event_base is null");
		return;
	}

	// 创建跨线程调度事件（每次 DoConnect 时重新创建，防止被 DoDisconnect 释放后缺失）
	if (!m_dispatchEvent)
	{
		m_dispatchEvent = event_new(evbase, -1, EV_READ | EV_PERSIST,
			ZmBroadcastClient::OnDispatchEventCB, this);
		if (m_dispatchEvent)
			event_add(m_dispatchEvent, nullptr);
	}

	// 创建 bufferevent
	m_bev = bufferevent_socket_new(evbase, -1,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
	if (!m_bev)
	{
		PUBLIC_LOG_ERROR("[BcClient] Failed to create bufferevent");
		auto cbf = m_callbacks.onConnectFailed;
		if (cbf)
			cbf("Failed to create socket");
		if (m_state.load() != ZM_BC_STATE_STOPPED)
			ScheduleRetry();
		return;
	}

	// 设置回调：OnConnectCB 作为连接阶段的事件回调
	bufferevent_setcb(m_bev, OnReadCB, nullptr, OnConnectCB, this);
	bufferevent_setwatermark(m_bev, EV_READ, 6, 0);

	// 发起连接
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(m_config.serverPort);
	if (evutil_inet_pton(AF_INET, m_config.serverIp.c_str(), &sin.sin_addr) != 1)
	{
		PUBLIC_LOG_ERROR("[BcClient] Invalid server IP: {}", m_config.serverIp);
		bufferevent_free(m_bev);
		m_bev = nullptr;
		auto cbf = m_callbacks.onConnectFailed;
		if (cbf)
			cbf("Invalid server IP: " + m_config.serverIp);
		if (m_state.load() != ZM_BC_STATE_STOPPED)
			ScheduleRetry();
		return;
	}

	if (bufferevent_socket_connect(m_bev, (struct sockaddr*)&sin, sizeof(sin)) != 0)
	{
		int err = EVUTIL_SOCKET_ERROR();
		PUBLIC_LOG_ERROR("[BcClient] Connect failed immediately (err={})", err);
		bufferevent_free(m_bev);
		m_bev = nullptr;
		auto cbf = m_callbacks.onConnectFailed;
		if (cbf)
			cbf("Connect failed immediately");
		if (m_state.load() != ZM_BC_STATE_STOPPED)
			ScheduleRetry();
		return;
	}

	PUBLIC_LOG_INFO("[BcClient] Connecting to {}:{}", m_config.serverIp, m_config.serverPort);
}

// ============================================================================
// 生命周期 — Disconnect
// ============================================================================

// ============================================================================
// Disconnect 同步辅助结构
// ============================================================================

/** @brief 用于同步 Disconnect 操作的辅助结构 */
struct BcDisconnectSync {
	ZmBroadcastClient* client;
	std::promise<void> promise;
};

void ZmBroadcastClient::Disconnect()
{
	// 多种状态下均可触发断开
	ZM_BROADCAST_STATE expected = ZM_BC_STATE_STARTING;
	if (m_state.compare_exchange_strong(expected, ZM_BC_STATE_STOPPING))
		goto schedule;
	expected = ZM_BC_STATE_LISTENING;
	if (m_state.compare_exchange_strong(expected, ZM_BC_STATE_STOPPING))
		goto schedule;
	expected = ZM_BC_STATE_ERROR;
	if (m_state.compare_exchange_strong(expected, ZM_BC_STATE_STOPPING))
		goto schedule;

	// 已在停止中或已停止
	{
		ZM_BROADCAST_STATE cur = m_state.load(std::memory_order_acquire);
		if (cur == ZM_BC_STATE_STOPPED || cur == ZM_BC_STATE_IDLE)
			return;
	}
	return;

schedule:
	if (std::this_thread::get_id() == m_loopThreadId)
	{
		// 已在事件循环线程，直接断开
		DoDisconnect();
	}
	else if (m_config.evbase)
	{
		// 使用 event_base_once + promise/future 确保 DoDisconnect 在事件循环线程执行
		auto task = new BcDisconnectSync{this, {}};
		auto future = task->promise.get_future();
		event_base_once(m_config.evbase, -1, EV_TIMEOUT,
			[](evutil_socket_t, short, void* arg) {
				auto* t = static_cast<BcDisconnectSync*>(arg);
				t->client->DoDisconnect();
				t->promise.set_value();
				delete t;
			}, task, nullptr);
		future.wait();
	}

	// 手动断开时回调通知
	if (m_callbacks.onDisconnected)
		m_callbacks.onDisconnected();

	// 释放内部线程池
	if (m_threadPool)
	{
		delete m_threadPool;
		m_threadPool = nullptr;
	}
}

void ZmBroadcastClient::DoDisconnect()
{
	// 取消握手超时定时器
	if (m_handshakeTimer)
	{
		event_free(m_handshakeTimer);
		m_handshakeTimer = nullptr;
	}

	// 取消重试定时器
	if (m_retryTimer)
	{
		event_free(m_retryTimer);
		m_retryTimer = nullptr;
	}

	// 释放调度事件
	if (m_dispatchEvent)
	{
		event_free(m_dispatchEvent);
		m_dispatchEvent = nullptr;
	}

	// 释放 bufferevent
	if (m_bev)
	{
		bufferevent_free(m_bev);
		m_bev = nullptr;
	}

	m_handshakeDone.store(false);
	m_startTime.store(0, std::memory_order_release);

	// 事件循环由外部管理，不在此停止

	m_state.store(ZM_BC_STATE_STOPPED, std::memory_order_release);


	PUBLIC_LOG_INFO("[BcClient] Disconnected");
}

// ============================================================================
// 重试机制
// ============================================================================

void ZmBroadcastClient::ScheduleRetry()
{
	struct event_base* evbase = m_config.evbase;
	if (!evbase)
		return;

	if (m_retryTimer)
	{
		event_free(m_retryTimer);
		m_retryTimer = nullptr;
	}

	struct timeval tv = {1, 0};
	m_retryTimer = evtimer_new(evbase, OnRetryTimerCB, this);
	if (m_retryTimer)
		evtimer_add(m_retryTimer, &tv);

	PUBLIC_LOG_DEBUG("[BcClient] Retry scheduled in 1s");
}

void ZmBroadcastClient::OnRetryTimerCB(evutil_socket_t fd, short what, void* ctx)
{
	ZmBroadcastClient* client = (ZmBroadcastClient*)ctx;
	client->DoConnect();
}

// ============================================================================
// libevent 回调 — 连接阶段
// ============================================================================

void ZmBroadcastClient::OnConnectCB(struct bufferevent* bev, short events, void* ctx)
{
	ZmBroadcastClient* client = (ZmBroadcastClient*)ctx;

	if (events & BEV_EVENT_CONNECTED)
	{
		PUBLIC_LOG_INFO("[BcClient] Connected to {}:{}",
		                 client->m_config.serverIp, client->m_config.serverPort);

		// 启用读写
		bufferevent_enable(bev, EV_READ | EV_WRITE);

		// 切换事件回调到 OnEventCB，后续事件（EOF/ERROR/TIMEOUT）由 OnEventCB 处理
		bufferevent_setcb(bev, OnReadCB, nullptr, OnEventCB, ctx);

		// 取消可能存在的重试定时器
		if (client->m_retryTimer)
		{
			event_free(client->m_retryTimer);
			client->m_retryTimer = nullptr;
		}

		// 启动握手超时定时器
		struct event_base* evbase = client->m_config.evbase;
		struct timeval tv = {client->m_config.handshakeTimeout, 0};
		client->m_handshakeTimer = evtimer_new(evbase, OnHandshakeTimeoutCB, client);
		evtimer_add(client->m_handshakeTimer, &tv);

		PUBLIC_LOG_DEBUG("[BcClient] Handshake timer started ({}s)", client->m_config.handshakeTimeout);
	}
	else
	{
		// 连接失败（包含 BEV_EVENT_EOF、BEV_EVENT_ERROR）
		int err = EVUTIL_SOCKET_ERROR();
		PUBLIC_LOG_ERROR("[BcClient] Connection failed to {}:{} (err={})",
		                  client->m_config.serverIp, client->m_config.serverPort, err);

		// 清理 bufferevent
		if (client->m_bev)
		{
			bufferevent_free(client->m_bev);
			client->m_bev = nullptr;
		}

		auto cbf = client->m_callbacks.onConnectFailed;
		if (cbf)
			cbf("Connection failed");

		if (client->m_state.load() != ZM_BC_STATE_STOPPED)
			client->ScheduleRetry();
	}
}

// ============================================================================
// libevent 回调 — 数据读取
// ============================================================================

void ZmBroadcastClient::OnReadCB(struct bufferevent* bev, void* ctx)
{
	ZmBroadcastClient* client = (ZmBroadcastClient*)ctx;
	struct evbuffer* input = bufferevent_get_input(bev);

	// 循环解码所有完整帧
	while (true)
	{
		std::string json = BcFrameDecode(input);
		if (json.empty())
			break; // 数据不足，等待更多

		client->HandleMessage(json);
	}
}

// ============================================================================
// libevent 回调 — 事件（EOF/ERROR/TIMEOUT）
// ============================================================================

void ZmBroadcastClient::OnEventCB(struct bufferevent* bev, short events, void* ctx)
{
	ZmBroadcastClient* client = (ZmBroadcastClient*)ctx;

	if (events & BEV_EVENT_EOF)
	{
		PUBLIC_LOG_INFO("[BcClient] Connection closed by server");
	}
	else if (events & BEV_EVENT_ERROR)
	{
		int err = EVUTIL_SOCKET_ERROR();
		PUBLIC_LOG_ERROR("[BcClient] Connection error: {}", err);
	}
	else if (events & BEV_EVENT_TIMEOUT)
	{
		PUBLIC_LOG_WARN("[BcClient] Connection timeout");
	}

	// 断开后的清理与重连
	if ((events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) != 0)
	{
		// 取消握手超时定时器
		if (client->m_handshakeTimer)
		{
			event_free(client->m_handshakeTimer);
			client->m_handshakeTimer = nullptr;
		}

		// 释放 bufferevent
		if (client->m_bev)
		{
			bufferevent_free(client->m_bev);
			client->m_bev = nullptr;
		}

		client->m_handshakeDone.store(false);

		auto cb = client->m_callbacks.onDisconnected;
		if (cb)
			cb();

		if (client->m_state.load() != ZM_BC_STATE_STOPPED)
			client->ScheduleRetry();
	}
}

// ============================================================================
// libevent 回调 — 握手超时
// ============================================================================

void ZmBroadcastClient::OnHandshakeTimeoutCB(evutil_socket_t fd, short what, void* ctx)
{
	ZmBroadcastClient* client = (ZmBroadcastClient*)ctx;
	PUBLIC_LOG_WARN("[BcClient] Handshake timeout for {}:{}",
	                 client->m_config.serverIp, client->m_config.serverPort);

	// 清理连接
	if (client->m_handshakeTimer)
	{
		event_free(client->m_handshakeTimer);
		client->m_handshakeTimer = nullptr;
	}

	if (client->m_bev)
	{
		bufferevent_free(client->m_bev);
		client->m_bev = nullptr;
	}

	client->m_handshakeDone.store(false);

	// 自动重连
	client->ScheduleRetry();
}

// ============================================================================
// 消息分发
// ============================================================================

void ZmBroadcastClient::HandleMessage(const std::string& json)
{
	// 解析 JSON
	std::string error;
	ZMJSON msg = zm_json_parse(json, error);
	if (!error.empty() || !msg.is_object())
	{
		PUBLIC_LOG_WARN("[BcClient] Invalid JSON: {}", error);
		return;
	}

	// 1. 检查是否为 settings（服务端握手帧）
	//    settings JSON 结构: {"settings":{"heartbeat_time":60}}
	if (msg.contains("settings") && msg["settings"].is_object())
	{
		// 回复 confirm_settings
		ZMJSON confirm;
		confirm["action"] = "confirm_settings";
		SendJson(zm_json_dump(confirm));

		// 取消握手超时定时器
		if (m_handshakeTimer)
		{
			event_free(m_handshakeTimer);
			m_handshakeTimer = nullptr;
		}

		// 握手完成
		m_handshakeDone.store(true);
		m_startTime.store(BcNowMillis(), std::memory_order_release);
		m_state.store(ZM_BC_STATE_LISTENING, std::memory_order_release);

		PUBLIC_LOG_INFO("[BcClient] Handshake complete with {}:{}",
		                 m_config.serverIp, m_config.serverPort);

		// 发送当前订阅的 tag 列表
		SendCurrentTags();

		// 回调通知连接成功
		if (m_callbacks.onConnected)
			m_callbacks.onConnected(m_config.serverIp, m_config.serverPort);
		return;
	}

	// 2. 检查是否为 ping 心跳
	std::string action = zm_json_get_str(msg, "action", "");
	if (action == "ping")
	{
		ZMJSON pong;
		pong["action"] = "pong";
		SendJson(zm_json_dump(pong));

		PUBLIC_LOG_DEBUG("[BcClient] Pong sent");
		return;
	}

	// 3. 检查是否为业务消息（同时包含 id 和 topic 字段）
	if (msg.contains("id") && msg.contains("topic"))
	{
		std::string topic = zm_json_get_str(msg, "topic", "");

		// content 可能为字符串或嵌套 JSON
		std::string content;
		if (msg.contains("content"))
		{
			if (msg["content"].is_string())
				content = msg["content"].get<std::string>();
			else
				content = zm_json_dump(msg["content"]);
		}

		m_receivedCount.fetch_add(1, std::memory_order_relaxed);

		// 通过线程池回调业务层
		if (m_callbacks.onMessage && m_threadPool)
		{
			m_threadPool->Submit([cb = m_callbacks.onMessage, topic, content]() {
				cb(topic, content);
				}, "Submit");
		}
		return;
	}

	PUBLIC_LOG_WARN("[BcClient] Unknown message: {}", json);
}

// ============================================================================
// 发送 / 当前标签同步
// ============================================================================

void ZmBroadcastClient::SendJson(const std::string& json)
{
	if (json.empty())
		return;

	BcClientTask task;
	task.type = BC_CLIENT_TASK_SEND;
	task.json = json;
	ScheduleTask(task);
}

void ZmBroadcastClient::SendCurrentTags()
{
	std::lock_guard<std::mutex> lock(m_tagsMutex);
	if (m_currentTags.empty())
		return;

	ZMJSON msg;
	msg["action"] = "subscribe";
	msg["tags"] = m_currentTags;
	SendJson(zm_json_dump(msg));
}

// ============================================================================
// 跨线程任务调度
// ============================================================================

void ZmBroadcastClient::ScheduleTask(const BcClientTask& task)
{
	// 若当前已在事件循环线程，直接执行
	if (std::this_thread::get_id() == m_loopThreadId)
	{
		const_cast<ZmBroadcastClient*>(this)->ExecuteTask(task);
		return;
	}

	// 非事件循环线程：入队后激活 dispatch 事件
	{
		std::lock_guard<std::mutex> lock(m_sendMutex);
		m_pendingTasks.push_back(task);
	}

	if (m_dispatchEvent)
		event_active(m_dispatchEvent, EV_READ, 0);
}

void ZmBroadcastClient::ExecuteTask(const BcClientTask& task)
{
	switch (task.type)
	{
	case BC_CLIENT_TASK_CONNECT:
		DoConnect();
		break;

	case BC_CLIENT_TASK_DISCONNECT:
		DoDisconnect();
		break;

	case BC_CLIENT_TASK_SEND:
		if (m_bev)
			BcFrameEncode(m_bev, task.json);
		break;
	}
}

void ZmBroadcastClient::OnDispatchEventCB(evutil_socket_t fd, short what, void* ctx)
{
	ZmBroadcastClient* client = (ZmBroadcastClient*)ctx;

	std::vector<BcClientTask> tasks;
	{
		std::lock_guard<std::mutex> lock(client->m_sendMutex);
		tasks.swap(client->m_pendingTasks);
	}

	for (auto& task : tasks)
		client->ExecuteTask(task);
}
