/**
 * @file zm_net_broadcast_server.cpp
 * @brief TCP 广播服务端实现
 */

#include "zm_net_broadcast_server.h"

#include "zm_net_runloop.h"
#include "../util/zm_util_sys.h"
#include "../json/zm_json.h"
#include "../spdlog/zm_logger.h"

#include <../libevent/include/event2/listener.h>
#include <../libevent/include/event2/bufferevent.h>
#include <../libevent/include/event2/buffer.h>
#include <../libevent/include/event2/event.h>
#include <../libevent/include/event2/util.h>

#include <algorithm>
#include <chrono>
#include <thread>

// ============================================================================
// 构造 / 析构
// ============================================================================

ZmBroadcastServer::ZmBroadcastServer(const BcServerConfig& config, const BcServerCallbacks& cbs)
    : m_config(config)
    , m_callbacks(cbs)
    , m_state(ZM_BC_STATE_IDLE)
    , m_listenPort(0)
    , m_listener(nullptr)
    , m_dispatchEvent(nullptr)
    , m_retryTimer(nullptr)
    , m_sentCount(0)
    , m_discardCount(0)
    , m_startTime(0)
    , m_retryCount(0)
{
}

ZmBroadcastServer::~ZmBroadcastServer()
{
    Stop();
}

// ============================================================================
// 状态查询（线程安全）
// ============================================================================

ZM_BROADCAST_STATE ZmBroadcastServer::GetState() const
{
    return m_state.load(std::memory_order_acquire);
}

uint16_t ZmBroadcastServer::GetPort() const
{
    return m_listenPort.load(std::memory_order_acquire);
}

int ZmBroadcastServer::GetConnectionCount() const
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return (int)m_clients.size();
}

int ZmBroadcastServer::GetMaxConnections() const
{
    return m_config.maxConnections;
}

size_t ZmBroadcastServer::GetGlobalQueueSize() const
{
    std::lock_guard<std::mutex> lock(m_taskMutex);
    return m_pendingTasks.size();
}

uint64_t ZmBroadcastServer::GetRunningTime() const
{
    if (m_startTime == 0)
        return 0;
    return (BcNowMillis() - m_startTime) / 1000;
}

uint64_t ZmBroadcastServer::GetSentCount() const
{
    return m_sentCount.load(std::memory_order_acquire);
}

uint64_t ZmBroadcastServer::GetDiscardCount() const
{
    return m_discardCount.load(std::memory_order_acquire);
}

BcClientInfo ZmBroadcastServer::GetClientInfo(const std::string& clientId) const
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(clientId);
    if (it != m_clients.end())
    {
        BcClientInfo info = it->second->info;
        info.queuePending = it->second->msgQueue.size();
        return info;
    }
    return BcClientInfo();
}

std::vector<BcClientInfo> ZmBroadcastServer::GetAllClients() const
{
    std::vector<BcClientInfo> result;
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    result.reserve(m_clients.size());
    for (const auto& pair : m_clients)
    {
        BcClientInfo info = pair.second->info;
        info.queuePending = pair.second->msgQueue.size();
        result.push_back(std::move(info));
    }
    return result;
}

// ============================================================================
// 运行时配置修改
// ============================================================================

void ZmBroadcastServer::SetMaxConnections(int max)
{
    m_config.maxConnections = max;
}

void ZmBroadcastServer::SetHeartbeatTime(int seconds)
{
    if (seconds > 0)
        m_config.heartbeatTime = seconds;
}

void ZmBroadcastServer::SetClientQueueMaxSize(size_t max)
{
    m_config.clientQueueMaxSize = max;
}

// ============================================================================
// 生命周期 — Start
// ============================================================================

bool ZmBroadcastServer::Start()
{
    ZM_BROADCAST_STATE expected = ZM_BC_STATE_IDLE;
    ZM_BROADCAST_STATE stopped = ZM_BC_STATE_STOPPED;
    if (!m_state.compare_exchange_strong(expected, ZM_BC_STATE_STARTING) &&
        !m_state.compare_exchange_strong(stopped, ZM_BC_STATE_STARTING))
    {
        return false;
    }

    if (!m_config.evbase)
    {
        m_state.store(ZM_BC_STATE_ERROR);
        if (m_callbacks.onListenFailed)
            m_callbacks.onListenFailed("evbase is null");
        return false;
    }

    // 使用 event_base_once 调度 DoStart() 到事件循环线程，
    // 避免 Start() 时 m_dispatchEvent 尚未创建，ScheduleTask 无法投递。
    event_base_once(m_config.evbase, -1, EV_TIMEOUT,
        [](evutil_socket_t, short, void* ctx) {
            ZmBroadcastServer* server = (ZmBroadcastServer*)ctx;
            server->DoStart();
        }, this, nullptr);

    return true;
}

void ZmBroadcastServer::DoStart()
{
    struct event_base* evbase = m_config.evbase;
    if (!evbase)
    {
        m_state.store(ZM_BC_STATE_ERROR);
        if (m_callbacks.onListenFailed)
            m_callbacks.onListenFailed("event_base is null");
        return;
    }

    // 记录事件循环线程 ID
    m_loopThreadId = std::this_thread::get_id();

    // 创建跨线程调度事件
    m_dispatchEvent = event_new(evbase, -1, EV_READ | EV_PERSIST,
                                ZmBroadcastServer::OnDispatchEventCB, this);
    if (!m_dispatchEvent)
    {
        m_state.store(ZM_BC_STATE_ERROR);
        if (m_callbacks.onListenFailed)
            m_callbacks.onListenFailed("Failed to create dispatch event");
        return;
    }
    event_add(m_dispatchEvent, nullptr);

    // 释放旧的失败重试定时器
    if (m_retryTimer)
    {
        event_free(m_retryTimer);
        m_retryTimer = nullptr;
    }

    // 尝试绑定监听
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(m_config.listenPort);

    if (m_config.listenIp == "0.0.0.0")
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        evutil_inet_pton(AF_INET, m_config.listenIp.c_str(), &sin.sin_addr);

    m_listener = evconnlistener_new_bind(evbase,
        ZmBroadcastServer::OnAcceptConnCB, this,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        (struct sockaddr*)&sin, sizeof(sin));

    if (m_listener)
    {
        // 获取实际端口
        struct sockaddr_storage ss;
        ev_socklen_t slen = sizeof(ss);
        evutil_socket_t fd = evconnlistener_get_fd(m_listener);
        if (getsockname(fd, (struct sockaddr*)&ss, &slen) == 0)
        {
            if (ss.ss_family == AF_INET)
            {
                uint16_t port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
                m_listenPort.store(port);
                if (m_config.listenPort == 0)
                    m_config.listenPort = port;
            }
        }

        m_startTime = BcNowMillis();
        m_retryCount = 0;
        m_state.store(ZM_BC_STATE_LISTENING);

        DEFAULT_LOG_INFO("[BcServer] Listening on {}:{}", m_config.listenIp, m_listenPort.load());

        if (m_callbacks.onListenSuccess)
            m_callbacks.onListenSuccess(m_listenPort.load());
    }
    else
    {
        // 绑定失败，1 秒后重试
        m_retryCount++;
        int err = EVUTIL_SOCKET_ERROR();
        DEFAULT_LOG_ERROR("[BcServer] Bind failed on {}:{} (err={}), retry #{}",
                          m_config.listenIp, m_config.listenPort, err, m_retryCount);

        timeval tv = {1, 0};
        m_retryTimer = evtimer_new(evbase,
            [](evutil_socket_t, short, void* ctx) {
                ZmBroadcastServer* server = (ZmBroadcastServer*)ctx;
                server->DoStart();
            }, this);
        evtimer_add(m_retryTimer, &tv);
    }
}

// ============================================================================
// 生命周期 — Stop
// ============================================================================

void ZmBroadcastServer::Stop()
{
    AsyncStop();

    // 同步等待事件循环线程完成 DoStop(仅非 loop 线程需要;
    // loop 线程内 ScheduleTask 已同步执行,状态即刻为 STOPPED)
    if (std::this_thread::get_id() != m_loopThreadId)
    {
        for (int i = 0; i < 100 && m_state.load() != ZM_BC_STATE_STOPPED; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (m_state.load() != ZM_BC_STATE_STOPPED)
            DEFAULT_LOG_WARN("[BcServer] Stop sync wait timeout, state={}",
                (int)m_state.load());
    }
}

void ZmBroadcastServer::AsyncStop()
{
    ZM_BROADCAST_STATE expected = ZM_BC_STATE_LISTENING;
    if (!m_state.compare_exchange_strong(expected, ZM_BC_STATE_STOPPING))
    {
        expected = ZM_BC_STATE_STARTING;
        if (!m_state.compare_exchange_strong(expected, ZM_BC_STATE_STOPPING))
        {
            if (m_state.load() == ZM_BC_STATE_STOPPED ||
                m_state.load() == ZM_BC_STATE_IDLE ||
                m_state.load() == ZM_BC_STATE_STOPPING)
                return;
        }
    }

    BcScheduledTask task;
    task.type = BC_TASK_STOP;
    ScheduleTask(task);
}

void ZmBroadcastServer::DoStop()
{
    // 关闭监听器
    if (m_listener)
    {
        evconnlistener_free(m_listener);
        m_listener = nullptr;
    }

    // 断开所有客户端
    {
        std::vector<std::string> allIds;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            allIds.reserve(m_clients.size());
            for (const auto& pair : m_clients)
                allIds.push_back(pair.first);
        }
        for (const auto& id : allIds)
            RemoveClient(id);
    }

    // 释放重试定时器
    if (m_retryTimer)
    {
        event_free(m_retryTimer);
        m_retryTimer = nullptr;
    }

    // 释放 dispatch 事件
    if (m_dispatchEvent)
    {
        event_free(m_dispatchEvent);
        m_dispatchEvent = nullptr;
    }

    m_startTime = 0;
    m_state.store(ZM_BC_STATE_STOPPED);

    if (m_callbacks.onListenStopped)
        m_callbacks.onListenStopped();

    DEFAULT_LOG_INFO("[BcServer] Server stopped");
}

// ============================================================================
// 客户端连接 — Accept 回调
// ============================================================================

void ZmBroadcastServer::OnAcceptConnCB(struct evconnlistener* listener,
                                        evutil_socket_t fd, struct sockaddr* addr,
                                        int socklen, void* ctx)
{
    ZmBroadcastServer* server = (ZmBroadcastServer*)ctx;

    // 检查连接数限制
    if (server->m_config.maxConnections > 0)
    {
        std::lock_guard<std::mutex> lock(server->m_clientsMutex);
        if ((int)server->m_clients.size() >= server->m_config.maxConnections)
        {
            DEFAULT_LOG_WARN("[BcServer] Connection limit reached ({})", server->m_config.maxConnections);
            evutil_closesocket(fd);
            return;
        }
    }

    struct event_base* evbase = server->m_config.evbase;

    // 创建 bufferevent
    struct bufferevent* bev = bufferevent_socket_new(evbase, fd,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    if (!bev)
    {
        DEFAULT_LOG_ERROR("[BcServer] Failed to create bufferevent for new connection");
        evutil_closesocket(fd);
        return;
    }

    // 分配客户端状态
    BcClient* client = new BcClient();
    client->bev = bev;
    client->clientId = BcGenerateUUID();
    client->info.clientId = client->clientId;
    client->info.connectTime = BcNowMillis();
    client->lastActiveTime = BcNowMillis();
    client->info.lastActiveTime = client->lastActiveTime;
    client->handshakeDone = false;
    client->m_owner = server;

    // 解析对端地址信息
    if (addr->sa_family == AF_INET)
    {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        char ipBuf[64];
        evutil_inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
        client->info.ip = ipBuf;
        client->info.port = ntohs(sin->sin_port);
    }

    // 设置 bufferevent 回调
    bufferevent_setcb(bev,
        ZmBroadcastServer::OnClientReadCB,
        ZmBroadcastServer::OnClientWriteCB,
        ZmBroadcastServer::OnClientEventCB,
        client);

    // 设置读水位线（至少 6 字节确保帧可开始解析：4 字节头 + 最少 2 字节 body）
    bufferevent_setwatermark(bev, EV_READ, 6, 0);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    // 发送 settings 帧
    ZMJSON settings;
    settings["settings"]["heartbeat_time"] = server->m_config.heartbeatTime;
    std::string settingsJson = zm_json_dump(settings);
    BcFrameEncode(bev, settingsJson);
    client->lastDataSentTime = BcNowMillis();

    // 启动握手超时定时器
    timeval handshakeTv = {server->m_config.handshakeTimeout, 0};
    client->handshakeTimer = evtimer_new(evbase,
        ZmBroadcastServer::OnHandshakeTimeoutCB, client);
    evtimer_add(client->handshakeTimer, &handshakeTv);

    // 加入客户端列表
    {
        std::lock_guard<std::mutex> lock(server->m_clientsMutex);
        server->m_clients[client->clientId] = client;
    }

    DEFAULT_LOG_INFO("[BcServer] New connection: fd={}, client_id={}, ip={}:{}",
                     (int)fd, client->clientId, client->info.ip, client->info.port);
}

// ============================================================================
// 客户端数据读取
// ============================================================================

void ZmBroadcastServer::OnClientReadCB(struct bufferevent* bev, void* ctx)
{
    BcClient* client = (BcClient*)ctx;

    struct evbuffer* input = bufferevent_get_input(bev);

    // 循环解码所有完整帧
    while (true)
    {
        std::string json = BcFrameDecode(input);
        if (json.empty())
            break; // 数据不足，等待更多数据

        // 刷新活跃时间
        client->lastActiveTime = BcNowMillis();
        client->info.lastActiveTime = client->lastActiveTime;

        // 解析 JSON
        std::string error;
        ZMJSON msg = zm_json_parse(json, error);
        if (!error.empty() || !msg.is_object())
        {
            DEFAULT_LOG_WARN("[BcServer] Invalid JSON from client {}: {}", client->clientId, error);
            continue;
        }

        std::string action = zm_json_get_str(msg, "action", "");

        if (action == "confirm_settings")
        {
            // 握手完成
            if (client->handshakeDone)
                continue;

            client->handshakeDone = true;

            // 取消握手超时定时器
            if (client->handshakeTimer)
            {
                event_free(client->handshakeTimer);
                client->handshakeTimer = nullptr;
            }

            ZmBroadcastServer* server = client->m_owner;

            // 启动心跳检测定时器
            struct event_base* evbase = server->m_config.evbase;
            int pingInterval = server->m_config.heartbeatTime / 2;
            if (pingInterval < 1) pingInterval = 1;

            timeval heartbeatTv = {pingInterval, 0};
            client->heartbeatTimer = event_new(evbase, -1, EV_TIMEOUT | EV_PERSIST,
                                                ZmBroadcastServer::OnHeartbeatCheckCB, client);
            event_add(client->heartbeatTimer, &heartbeatTv);

            DEFAULT_LOG_INFO("[BcServer] Client {} handshake completed", client->clientId);

            if (server->m_callbacks.onClientOnline)
                server->m_callbacks.onClientOnline(client->info);
        }
        else if (action == "pong")
        {
            // pong 响应已通过 lastActiveTime 刷新体现
            DEFAULT_LOG_DEBUG("[BcServer] Client {} pong", client->clientId);
        }
        else if (action == "subscribe")
        {
            if (msg.contains("tags") && msg["tags"].is_array())
            {
                for (const auto& t : msg["tags"])
                {
                    if (t.is_string())
                    {
                        std::string tag = t.get<std::string>();
                        bool found = false;
                        for (const auto& existing : client->info.tags)
                        {
                            if (existing == tag) { found = true; break; }
                        }
                        if (!found)
                            client->info.tags.push_back(tag);
                    }
                }
                DEFAULT_LOG_INFO("[BcServer] Client {} subscribed tags: {} total",
                                 client->clientId, client->info.tags.size());
            }
        }
        else if (action == "unsubscribe")
        {
            if (msg.contains("tags") && msg["tags"].is_array())
            {
                for (const auto& t : msg["tags"])
                {
                    if (t.is_string())
                    {
                        std::string tag = t.get<std::string>();
                        auto& tags = client->info.tags;
                        auto it = std::find(tags.begin(), tags.end(), tag);
                        if (it != tags.end())
                            tags.erase(it);
                    }
                }
                DEFAULT_LOG_INFO("[BcServer] Client {} unsubscribed tags: {} remaining",
                                 client->clientId, client->info.tags.size());
            }
        }
        else
        {
            DEFAULT_LOG_DEBUG("[BcServer] Unknown action from client {}: {}", client->clientId, action);
        }
    }
}

// ============================================================================
// 客户端事件（断开 / 错误）
// ============================================================================

void ZmBroadcastServer::OnClientEventCB(struct bufferevent* bev, short events, void* ctx)
{
    BcClient* client = (BcClient*)ctx;

    if (events & BEV_EVENT_EOF)
        DEFAULT_LOG_INFO("[BcServer] Client {} disconnected (EOF)", client->clientId);
    else if (events & BEV_EVENT_ERROR)
    {
        int err = EVUTIL_SOCKET_ERROR();
        DEFAULT_LOG_ERROR("[BcServer] Client {} socket error: {}", client->clientId, err);
    }
    else if (events & BEV_EVENT_TIMEOUT)
        DEFAULT_LOG_WARN("[BcServer] Client {} timeout", client->clientId);

	else
		return; // 非断开事件（如 WRITE），不处理

	if (client->m_owner)
        client->m_owner->RemoveClient(client->clientId);
}

// ============================================================================
// 客户端写完成（队列消费）
// ============================================================================

void ZmBroadcastServer::OnClientWriteCB(struct bufferevent* bev, void* ctx)
{
    BcClient* client = (BcClient*)ctx;

    // 写完成后从队列取出已完成帧
    if (!client->msgQueue.empty())
    {
        client->msgQueue.pop_front();
    }

    // 发送下一帧
    if (!client->msgQueue.empty())
    {
        const std::string& nextFrame = client->msgQueue.front();
        bufferevent_write(bev, nextFrame.data(), nextFrame.size());
    }
}

// ============================================================================
// 握手超时
// ============================================================================

void ZmBroadcastServer::OnHandshakeTimeoutCB(evutil_socket_t fd, short what, void* ctx)
{
    BcClient* client = (BcClient*)ctx;
    DEFAULT_LOG_WARN("[BcServer] Client {} handshake timeout", client->clientId);

    if (client->m_owner)
        client->m_owner->RemoveClient(client->clientId);
}

// ============================================================================
// 心跳检测
// ============================================================================

void ZmBroadcastServer::OnHeartbeatCheckCB(evutil_socket_t fd, short what, void* ctx)
{
    BcClient* client = (BcClient*)ctx;
    if (!client || !client->m_owner)
        return;

    ZmBroadcastServer* server = client->m_owner;
    uint64_t now = BcNowMillis();
    uint64_t heartbeatTimeMs = (uint64_t)server->m_config.heartbeatTime * 1000;
    uint64_t pingIntervalMs = (uint64_t)(server->m_config.heartbeatTime / 2) * 1000;
    if (pingIntervalMs < 1000) pingIntervalMs = 1000;

    // 1. 检查超时
    uint64_t elapsed = now - client->lastActiveTime;
    if (elapsed >= heartbeatTimeMs)
    {
        DEFAULT_LOG_WARN("[BcServer] Client {} heartbeat timeout (elapsed={}ms, threshold={}ms)",
                         client->clientId, elapsed, heartbeatTimeMs);
        server->RemoveClient(client->clientId);
        return;
    }

    // 2. 若空闲超过 ping 间隔则发送 ping
    uint64_t sendElapsed = now - client->lastDataSentTime;
    if (sendElapsed >= pingIntervalMs)
    {
        ZMJSON ping;
        ping["action"] = "ping";
        std::string pingJson = zm_json_dump(ping);
        if (BcFrameEncode(client->bev, pingJson))
        {
            client->lastDataSentTime = now;
            DEFAULT_LOG_DEBUG("[BcServer] Ping sent to client {}", client->clientId);
        }
    }
}

// ============================================================================
// 客户端清理
// ============================================================================

void ZmBroadcastServer::RemoveClient(const std::string& clientId)
{
    BcClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(clientId);
        if (it == m_clients.end())
            return;
        client = it->second;
        m_clients.erase(it);
    }

    // 回调离线（保存快照）
    if (m_callbacks.onClientOffline)
    {
        BcClientInfo info = client->info;
        info.queuePending = client->msgQueue.size();
        m_callbacks.onClientOffline(info);
    }

    // 释放资源
    if (client->handshakeTimer)
    {
        event_free(client->handshakeTimer);
        client->handshakeTimer = nullptr;
    }
    if (client->heartbeatTimer)
    {
        event_free(client->heartbeatTimer);
        client->heartbeatTimer = nullptr;
    }
    if (client->bev)
    {
        bufferevent_free(client->bev);
        client->bev = nullptr;
    }

    delete client;
    DEFAULT_LOG_INFO("[BcServer] Client {} removed", clientId);
}

// ============================================================================
// 立即发送（线程安全入口）
// ============================================================================

bool ZmBroadcastServer::Send(const std::string& clientId, const std::string& topic,
                              const std::string& content, const std::string& tag)
{
    if (clientId.empty() || topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.clientId = clientId;
    task.isBroadcast = false;
    task.delayMs = 0;
    task.timestampMs = 0;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

bool ZmBroadcastServer::Broadcast(const std::string& topic, const std::string& content,
                                   const std::string& tag)
{
    if (topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.isBroadcast = true;
    task.delayMs = 0;
    task.timestampMs = 0;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

// ============================================================================
// 延时发送（线程安全入口）
// ============================================================================

bool ZmBroadcastServer::SendDelayed(const std::string& clientId, const std::string& topic,
                                     const std::string& content, const std::string& tag,
                                     uint32_t delayMs)
{
    if (clientId.empty() || topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.clientId = clientId;
    task.isBroadcast = false;
    task.delayMs = delayMs;
    task.timestampMs = 0;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

bool ZmBroadcastServer::BroadcastDelayed(const std::string& topic, const std::string& content,
                                          const std::string& tag, uint32_t delayMs)
{
    if (topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.isBroadcast = true;
    task.delayMs = delayMs;
    task.timestampMs = 0;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

// ============================================================================
// 定时发送（线程安全入口）
// ============================================================================

bool ZmBroadcastServer::SendAt(const std::string& clientId, const std::string& topic,
                                const std::string& content, const std::string& tag,
                                uint64_t timestampMs)
{
    if (clientId.empty() || topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.clientId = clientId;
    task.isBroadcast = false;
    task.delayMs = 0;
    task.timestampMs = timestampMs;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

bool ZmBroadcastServer::BroadcastAt(const std::string& topic, const std::string& content,
                                     const std::string& tag, uint64_t timestampMs)
{
    if (topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.isBroadcast = true;
    task.delayMs = 0;
    task.timestampMs = timestampMs;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

// ============================================================================
// DoSend（事件循环线程中执行真正的发送）
// ============================================================================


void ZmBroadcastServer::DoSend(const BcMessage& msg, const std::string& clientId,
                                bool isBroadcast, uint32_t delayMs, uint64_t timestampMs)
{
    // 计算实际延迟
    uint32_t actualDelay = delayMs;
    if (timestampMs > 0)
    {
        uint64_t now = BcNowMillis();
        if (timestampMs > now)
            actualDelay = (uint32_t)(timestampMs - now);
        else
            actualDelay = 0;
    }

    // 如果需延迟，创建一次性定时器
    if (actualDelay > 0)
    {
        struct event_base* evbase = m_config.evbase;

        // 将任务数据拷贝到堆上
        BcScheduledTask* heapTask = new BcScheduledTask();
        heapTask->type = BC_TASK_SEND;
        heapTask->clientId = clientId;
        heapTask->isBroadcast = isBroadcast;
        heapTask->message = msg;
        heapTask->m_server = this;

        timeval tv;
        tv.tv_sec = actualDelay / 1000;
        tv.tv_usec = (actualDelay % 1000) * 1000;

        // 定时器存到 heapTask 中，回调里统一释放
        heapTask->timer = evtimer_new(evbase, OnDelayedSendCB, heapTask);
        evtimer_add(heapTask->timer, &tv);
        return;
    }

    // 构造线上 JSON
    ZMJSON jsonMsg;
    jsonMsg["id"] = msg.id;
    jsonMsg["timestamp"] = msg.timestamp;
    jsonMsg["topic"] = msg.topic;

    // content 可能是 JSON 字符串，尝试解析内嵌
    std::string contentError;
    ZMJSON contentJson = zm_json_parse(msg.content, contentError);
    if (contentError.empty())
        jsonMsg["content"] = contentJson;
    else
        jsonMsg["content"] = msg.content;

    std::string jsonStr = zm_json_dump(jsonMsg);

    if (isBroadcast)
    {
        // 遍历所有已握手客户端，匹配 tag
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clients)
        {
            BcClient* client = pair.second;
            if (!client->handshakeDone)
                continue;

            // tag 过滤：空 tag 表示全部推送，否则仅推送给订阅了此 tag 的客户端
            if (!msg.tag.empty())
            {
                bool subscribed = false;
                for (const auto& t : client->info.tags)
                {
                    if (t == msg.tag) { subscribed = true; break; }
                }
                if (!subscribed)
                    continue;
            }

            DeliverToClient(client, jsonStr);
        }
    }
    else
    {
        // 单播
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(clientId);
        if (it != m_clients.end() && it->second->handshakeDone)
        {
            BcClient* client = it->second;

            // tag 过滤
            if (!msg.tag.empty())
            {
                bool subscribed = false;
                for (const auto& t : client->info.tags)
                {
                    if (t == msg.tag) { subscribed = true; break; }
                }
                if (!subscribed)
                    return;
            }

            DeliverToClient(client, jsonStr);
        }
    }
}

// ============================================================================
// DeliverToClient（帧编码 + 入队列 + bufferevent_write）
// ============================================================================

void ZmBroadcastServer::DeliverToClient(BcClient* client, const std::string& jsonStr)
{
    // 帧编码（在 DeliverToClient 中复用以确保一致性）
    uint32_t bodyLen = (uint32_t)jsonStr.size();
    uint8_t lenBuf[4];
    lenBuf[0] = (bodyLen >> 24) & 0xFF;
    lenBuf[1] = (bodyLen >> 16) & 0xFF;
    lenBuf[2] = (bodyLen >> 8)  & 0xFF;
    lenBuf[3] =  bodyLen        & 0xFF;

    std::string frame;
    frame.reserve(4 + bodyLen);
    frame.append((const char*)lenBuf, 4);
    frame.append(jsonStr);

    // 检查队列上限
    if (client->msgQueue.size() >= m_config.clientQueueMaxSize)
    {
        // 丢弃最旧消息
        client->msgQueue.pop_front();
        m_discardCount.fetch_add(1, std::memory_order_relaxed);
    }

    // 入队列
    client->msgQueue.push_back(std::move(frame));

    // 如果队列之前为空，直接发送第一帧
    if (client->msgQueue.size() == 1)
    {
        const std::string& firstFrame = client->msgQueue.front();
        bufferevent_write(client->bev, firstFrame.data(), firstFrame.size());
    }
    // 否则等待 OnClientWriteCB 驱动后续发送

    client->info.sentCount++;
    client->info.queuePending = client->msgQueue.size();
    m_sentCount.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================================
// 延时发送回调
// ============================================================================

void ZmBroadcastServer::OnDelayedSendCB(evutil_socket_t fd, short what, void* ctx)
{
    BcScheduledTask* heapTask = (BcScheduledTask*)ctx;
    if (!heapTask)
        return;

    // 释放定时器（已在 heapTask->timer 中存储）
    if (heapTask->timer)
    {
        event_free(heapTask->timer);
        heapTask->timer = nullptr;
    }

    if (!heapTask->m_server)
    {
        delete heapTask;
        return;
    }

    ZmBroadcastServer* server = heapTask->m_server;

    // 重新入队为立即发送
    BcScheduledTask immediateTask = *heapTask;
    immediateTask.delayMs = 0;
    immediateTask.timestampMs = 0;

    // 释放堆上旧任务
    delete heapTask;

    // 投递执行
    server->ScheduleTask(immediateTask);
}

// ============================================================================
// KickClient
// ============================================================================

bool ZmBroadcastServer::KickClient(const std::string& clientId)
{
    if (clientId.empty())
        return false;

    // 检查客户端是否存在
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        if (m_clients.find(clientId) == m_clients.end())
            return false;
    }

    BcScheduledTask task;
    task.type = BC_TASK_KICK;
    task.clientId = clientId;
    ScheduleTask(task);
    return true;
}

// ============================================================================
// 跨线程任务调度
// ============================================================================

void ZmBroadcastServer::ScheduleTask(const BcScheduledTask& task)
{
    // 如果当前在事件循环线程，直接执行
    if (std::this_thread::get_id() == m_loopThreadId)
    {
        const_cast<ZmBroadcastServer*>(this)->ExecuteTask(task);
        return;
    }

    // 非事件循环线程，投递到 pending 队列然后激活 dispatch 事件
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        BcScheduledTask copy = task;
        copy.m_server = this;
        m_pendingTasks.push_back(std::move(copy));
    }

    if (m_dispatchEvent)
        event_active(m_dispatchEvent, EV_READ, 0);
}

void ZmBroadcastServer::OnDispatchEventCB(evutil_socket_t fd, short what, void* ctx)
{
    ZmBroadcastServer* server = (ZmBroadcastServer*)ctx;

    std::vector<BcScheduledTask> tasks;
    {
        std::lock_guard<std::mutex> lock(server->m_taskMutex);
        tasks.swap(server->m_pendingTasks);
    }

    for (auto& task : tasks)
    {
        server->ExecuteTask(task);
    }
}

void ZmBroadcastServer::ExecuteTask(const BcScheduledTask& task)
{
    switch (task.type)
    {
    case BC_TASK_START:
        DoStart();
        break;
    case BC_TASK_STOP:
        DoStop();
        break;
    case BC_TASK_SEND:
        DoSend(task.message, task.clientId, task.isBroadcast,
               task.delayMs, task.timestampMs);
        break;
    case BC_TASK_KICK:
        RemoveClient(task.clientId);
        break;
    }
}
