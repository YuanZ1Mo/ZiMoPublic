/**
 * @file zm_websocket_server.cpp
 * @brief WebSocket 服务端实现
 *
 * 实现 ZmMessageWSServer（核心服务端）和 ZmMessageServer（同步包装层）。
 */

#include "zm_websocket_server.h"
#include "../net/zm_net_socket.h"

#include <algorithm>
#include <event2/event.h>

// ==================== ZmMessageWSServer ====================

ZmMessageWSServer::ZmMessageWSServer() :
    ZmMessageWSBase(),
    m_httpServer(nullptr),
    m_bindDoneCallback(nullptr),
    m_closeDoneCallback(nullptr),
    m_maxConnections(0),
    m_nextClientId(1),
    m_heartbeatEnabled(false),
    m_heartbeatIntervalMs(30000),
    m_heartbeatTimeoutMs(10000)
{
    m_clients.clear();
    m_pendingMessages.clear();
    memset(&m_statistics, 0, sizeof(m_statistics));
}

/**
 * @brief 析构函数
 *
 * 将状态设为 STOPPED，清理回调指针，释放控制事件、缓存消息、客户端列表和 HTTP 服务器。
 * evhttp_free 会释放关联的 event_base，因此 m_evbase 置空即可。
 */
ZmMessageWSServer::~ZmMessageWSServer()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    changeState(ZM_WS_STATE_STOPPED);
    m_closeDoneCallback = nullptr;
    m_bindDoneCallback = nullptr;

    if (m_ctrlEvent)
    {
        event_free(m_ctrlEvent);
        m_ctrlEvent = nullptr;
    }

    ClearAllCachedMessages();
    clearAllClients();

    if (m_httpServer)
    {
        evhttp_free(m_httpServer);
        m_httpServer = nullptr;
    }

    // m_evbase 由 evhttp_free 内部释放时已关联，需检查
    m_evbase = nullptr;
}

void ZmMessageWSServer::SetBindPort(uint16_t port)
{
    m_port = port;
}

/**
 * @brief 设置读写超时
 *
 * override 基类版本，增加了 mutex 保护。
 */
void ZmMessageWSServer::SetReadAndWriteTimeout(long read_msec, long write_msec)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    m_readTimeout.tv_sec = read_msec / 1000;
    m_readTimeout.tv_usec = (read_msec % 1000) * 1000;

    m_writeTimeout.tv_sec = write_msec / 1000;
    m_writeTimeout.tv_usec = (write_msec % 1000) * 1000;
}

void ZmMessageWSServer::SetCloseDoneCallback(ZmWSServerCloseDoneCallback cb)
{
    m_closeDoneCallback = std::move(cb);
}

void ZmMessageWSServer::SetBindDoneCallback(ZmWSServerBindDoneCallback cb)
{
    m_bindDoneCallback = std::move(cb);
}

void ZmMessageWSServer::SetBindErrorCallback(ZmWSServerBindErrorCallback cb)
{
    m_bindErrorCallback = std::move(cb);
}

bool ZmMessageWSServer::IsBound()
{
    return hasState(ZM_WS_STATE_BOUND);
}

uint16_t ZmMessageWSServer::GetBindPort()
{
    return m_port;
}

/**
 * @brief HTTP 请求回调 — 处理 WebSocket Upgrade
 *
 * 由 evhttp 通用回调触发（evhttp_set_gencb 注册）。
 *
 * 流程：
 * 1. 参数校验 + 连接数限制检查
 * 2. evws_new_session 自动完成 HTTP 101 Upgrade 握手并创建 evws_connection
 * 3. 创建 ZmMessageWsClient_t 记录客户端信息
 * 4. 加入 m_clients 列表
 * 5. 叠加 CONNECTED 状态
 * 6. 触发 ZM_WS_CONTROL_SEND 发送缓存消息
 */
void ZmMessageWSServer::onHttpRequest(struct evhttp_request* req, void* arg)
{
    ZmMessageWSServer* self = static_cast<ZmMessageWSServer*>(arg);
    if (!self || !req)
    {
        if (req)
        {
            evhttp_send_error(req, HTTP_INTERNAL, "Internal Server Error");
        }
        return;
    }

    // 检查连接数限制
    if (self->m_maxConnections > 0 && self->m_clients.size() >= self->m_maxConnections)
    {
        evhttp_send_error(req, HTTP_SERVUNAVAIL, "Too many connections");
        return;
    }

    // 创建 WebSocket 会话，evws_new_session 自动处理 Upgrade 握手
    struct evws_connection* wsConn = evws_new_session(req,
        ZmMessageWSServer::onWsMessage, self, 0);

    if (!wsConn)
    {
        // evws_new_session 失败时会自动回复错误，不需要再调用 evhttp_send_error
        return;
    }

    // 设置关闭回调
    evws_connection_set_closecb(wsConn, ZmMessageWSServer::onWsClose, self);

    std::unique_lock<std::recursive_mutex> lock(self->m_mutex);

    // 创建客户端对象
    ZmMessageWsClient_t* client = (ZmMessageWsClient_t*)calloc(1, sizeof(*client));
    if (!client)
    {
        evws_close(wsConn, WS_CR_PROTO_ERR);
        return;
    }

    client->server = self;
    client->ws_conn = wsConn;
    client->client_id = self->m_nextClientId++;
    client->last_activity = time(nullptr);

    self->m_clients.push_back(client);
    self->m_statistics.total_connections++;

    self->addState(ZM_WS_STATE_CONNECTED);

    // 新客户端连接后触发一次缓存数据发送
    if (self->m_ctrlEvent)
    {
        event_active(self->m_ctrlEvent, ZM_WS_CONTROL_SEND, 0);
    }
}

/**
 * @brief WebSocket 消息回调
 *
 * 由 libevent 在收到客户端 WebSocket 帧时调用。
 * type 为 WS_TEXT_FRAME / WS_BINARY_FRAME 等常量。
 * Ping/Pong 由 evws 库自动处理，不会到达此回调。
 *
 * 当前仅更新客户端活动时间和统计信息，不解析业务内容。
 */
void ZmMessageWSServer::onWsMessage(struct evws_connection* wsConn, int type,
    const unsigned char* data, size_t len, void* arg)
{
    ZmMessageWSServer* self = static_cast<ZmMessageWSServer*>(arg);
    if (!self) return;

    std::unique_lock<std::recursive_mutex> lock(self->m_mutex);

    // 更新活动时间
    ZmMessageWsClient_t* client = self->findClientByConn(wsConn);
    if (client)
    {
        client->last_activity = time(nullptr);
    }

    if (type == WS_TEXT_FRAME || type == WS_BINARY_FRAME)
    {
        self->m_statistics.messages_received++;
    }
}

/**
 * @brief WebSocket 连接关闭回调
 *
 * 客户端断开时由 libevent 调用。从 m_clients 中移除对应客户端，
 * 释放 evws_connection 和 ZmMessageWsClient_t。
 * 当所有客户端断开后移除 CONNECTED 状态。
 */
void ZmMessageWSServer::onWsClose(struct evws_connection* wsConn, void* arg)
{
    ZmMessageWSServer* self = static_cast<ZmMessageWSServer*>(arg);
    if (!self) return;

    std::unique_lock<std::recursive_mutex> lock(self->m_mutex);

    auto findIt = std::find_if(self->m_clients.begin(), self->m_clients.end(),
        [wsConn](ZmMessageWsClient_t* c) { return c->ws_conn == wsConn; });

    if (findIt != self->m_clients.end())
    {
        ZmMessageWsClient_t* client = *findIt;
        self->m_clients.erase(findIt);

        evws_connection_free(client->ws_conn);
        free(client);

        if (self->m_clients.empty())
        {
            self->removeState(ZM_WS_STATE_CONNECTED);
        }
    }
}

/**
 * @brief 根据 evws_connection 指针查找客户端对象
 *
 * 线性遍历 m_clients，通过 ws_conn 指针匹配。
 * 在事件回调中使用，ws_conn 指针由 libevent 保证唯一。
 */
ZmMessageWsClient_t* ZmMessageWSServer::findClientByConn(struct evws_connection* wsConn)
{
    for (auto& client : m_clients)
    {
        if (client->ws_conn == wsConn)
        {
            return client;
        }
    }
    return nullptr;
}

/**
 * @brief 处理消息发送（ZM_WS_CONTROL_SEND 事件处理）
 *
 * 从 m_pendingMessages 队列中逐一取出消息，遍历所有客户端通过 evws_send_binary 广播发送。
 * 遇到无效连接（ws_conn 为空）时清理并移除该客户端。
 * 发送完成后递增统计计数。
 *
 * @note 所有操作在事件循环线程中执行，通过 m_mutex 与 PostNotificationWithData 互斥。
 */
void ZmMessageWSServer::onControlSend()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (!hasState(ZM_WS_STATE_CONNECTED))
    {
        return;
    }

    ZmMessageItem_t* message_item = nullptr;

    while (!m_pendingMessages.empty())
    {
        auto msgIt = m_pendingMessages.begin();
        message_item = *msgIt;
        m_pendingMessages.erase(msgIt);

        if (message_item == nullptr)
        {
            continue;
        }

        if (message_item->len == 0 || message_item->data == nullptr)
        {
            if (message_item->data)
            {
                free(message_item->data);
                message_item->data = nullptr;
            }
            free(message_item);
            message_item = nullptr;
            continue;
        }

        // 遍历所有客户端，通过 evws_send_binary 广播发送
        for (auto it = m_clients.begin(); it != m_clients.end(); )
        {
            ZmMessageWsClient_t* client = *it;
            if (client && client->ws_conn)
            {
                evws_send_binary(client->ws_conn,
                    static_cast<const char*>(message_item->data),
                    message_item->len);
                ++it;
            }
            else
            {
                // 无效连接，清理并移除
                if (client)
                {
                    if (client->ws_conn)
                    {
                        evws_connection_free(client->ws_conn);
                    }
                    free(client);
                }
                it = m_clients.erase(it);
            }
        }

        // 释放消息内存
        if (message_item->data)
        {
            free(message_item->data);
            message_item->data = nullptr;
        }
        free(message_item);
        message_item = nullptr;

        m_statistics.messages_sent++;
    }

    // 队列还有消息则继续触发发送
    if (!m_pendingMessages.empty() && m_ctrlEvent)
    {
        event_active(m_ctrlEvent, ZM_WS_CONTROL_SEND, 0);
    }
}

/**
 * @brief 处理绑定完成（ZM_WS_CONTROL_BOUND 事件处理）
 *
 * 叠加 BOUND 状态，拷贝回调后释放锁再调用。
 * 回调在事件循环线程中执行，通知 ZmMessageServer 绑定成功。
 */
void ZmMessageWSServer::onControlBound()
{
    ZmWSServerBindDoneCallback bindDoneCallback;
    uint16_t port = 0;
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        addState(ZM_WS_STATE_BOUND);
        bindDoneCallback = m_bindDoneCallback;
        port = m_port;
    }

    if (bindDoneCallback)
    {
        bindDoneCallback(port);
    }
}

/**
 * @brief 处理关闭（ZM_WS_CONTROL_CLOSE 事件处理）
 *
 * 清理所有客户端连接和缓存消息，释放控制事件，退出事件循环。
 * 之后 Start() 的 event_base_dispatch 会返回。
 */
void ZmMessageWSServer::onControlClose()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    clearAllClients();
    ClearAllCachedMessages();

    if (m_ctrlEvent)
    {
        event_free(m_ctrlEvent);
        m_ctrlEvent = nullptr;
    }

    if (m_evbase)
    {
        event_base_loopbreak(m_evbase);
    }
}

/**
 * @brief 控制事件分发
 *
 * m_ctrlEvent 未关联 fd，通过 event_active() 手动触发。
 * 根据 what 参数中的 WS_CONTROL 位分发到对应处理函数。
 * 多个控制位可以同时触发（按位检测）。
 */
void ZmMessageWSServer::onEventControl(evutil_socket_t fd, short what, void* ctx)
{
    if (ctx)
    {
        ZmMessageWSServer* svr = static_cast<ZmMessageWSServer*>(ctx);

        if (ZM_WS_CONTROL_SEND & what)
        {
            svr->onControlSend();
        }

        if (ZM_WS_CONTROL_CLOSE & what)
        {
            svr->onControlClose();
        }

        if (ZM_WS_CONTROL_BOUND & what)
        {
            svr->onControlBound();
        }
    }
}

/**
 * @brief 清理所有客户端连接并释放资源
 *
 * 遍历 m_clients，逐一释放 evws_connection 和 ZmMessageWsClient_t。
 * 在 Stop / onControlClose / 析构时调用。
 */
void ZmMessageWSServer::clearAllClients()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    for (auto it = m_clients.begin(); it != m_clients.end(); )
    {
        ZmMessageWsClient_t* client = *it;
        it = m_clients.erase(it);

        if (client->ws_conn)
        {
            evws_connection_free(client->ws_conn);
            client->ws_conn = nullptr;
        }
        free(client);
    }
    m_clients.clear();
}

/**
 * @brief 清理所有资源
 *
 * 依次清理：缓存消息 → 客户端列表 → 控制事件 → HTTP 服务器。
 * evhttp_free 会释放关联的 event_base，因此 m_evbase 置空即可。
 */
void ZmMessageWSServer::clearAll()
{
    ClearAllCachedMessages();
    clearAllClients();

    if (m_ctrlEvent)
    {
        event_free(m_ctrlEvent);
        m_ctrlEvent = nullptr;
    }

    if (m_httpServer)
    {
        evhttp_free(m_httpServer);
        m_httpServer = nullptr;
    }

    // evhttp_free 会释放关联的 event_base
    m_evbase = nullptr;
}

/**
 * @brief 启动服务端（阻塞，在事件循环线程中调用）
 *
 * 完整启动流程：
 * 1. 检查是否已在运行（防止重复启动）
 * 2. 清理旧状态 → 初始化 libevent 线程支持
 * 3. 创建 event_base + evhttp → 绑定端口 → 注册回调 → 创建控制事件
 * 4. 成功：触发 BOUND 事件 → event_base_dispatch 进入事件循环（阻塞）
 *    失败：清理资源 → 调用 bindErrorCallback
 * 5. 事件循环退出后：释放 HTTP 服务器 → 调用 closeDoneCallback → 状态改为 STOPPED
 *
 * @note event_base_dispatch 会阻塞直到 event_base_loopbreak 被调用。
 *       Stop() 通过 event_active(ZM_WS_CONTROL_CLOSE) 触发 loopbreak。
 */
void ZmMessageWSServer::Start()
{
    if (hasState(ZM_WS_STATE_RUNNING))
    {
        return;
    }

    changeState(ZM_WS_STATE_RUNNING);

    bool init_ok = false;

    clearAll();
    zm_util_eventbase_init();

    do
    {
        m_evbase = event_base_new();
        if (!m_evbase)
        {
            break;
        }

        m_httpServer = evhttp_new(m_evbase);
        if (!m_httpServer)
        {
            break;
        }

        // 绑定端口（0.0.0.0:port，port=0 时由系统随机分配）
        struct evhttp_bound_socket* boundSocket = evhttp_bind_socket_with_handle(m_httpServer, "0.0.0.0", m_port);
        if (!boundSocket)
        {
            break;
        }

        // 获取实际绑定的端口（当 m_port=0 时随机分配）
        if (m_port == 0)
        {
            evutil_socket_t fd = evhttp_bound_socket_get_fd(boundSocket);
            if (fd >= 0)
            {
                m_port = ZmNetSocketBase::LocalPort(fd);
            }
        }

        // 注册 HTTP 请求回调（所有 HTTP 请求都进 onHttpRequest 处理 Upgrade）
        evhttp_set_gencb(m_httpServer, ZmMessageWSServer::onHttpRequest, this);

        // 创建控制事件（fd=-1 表示手动触发，EV_PERSIST 允许重复使用）
        m_ctrlEvent = event_new(m_evbase, -1, EV_PERSIST | EV_READ,
            [](evutil_socket_t fd, short what, void* callback_arg) {
                ZmMessageWSServer* self = static_cast<ZmMessageWSServer*>(callback_arg);
                self->onEventControl(fd, what, callback_arg);
            }, this);

        if (!m_ctrlEvent)
        {
            break;
        }
        event_add(m_ctrlEvent, nullptr);

        init_ok = true;

    } while (false);

    ZmWSServerBindErrorCallback bindErrorCallback;
    ZmWSServerCloseDoneCallback closeDoneCallback;
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        bindErrorCallback = m_bindErrorCallback;
        closeDoneCallback = m_closeDoneCallback;
    }

    if (init_ok)
    {
        // 通知绑定完成，然后进入事件循环
        event_active(m_ctrlEvent, ZM_WS_CONTROL_BOUND, 0);
        event_base_dispatch(m_evbase);
    }
    else
    {
        clearAll();

        if (bindErrorCallback)
        {
            bindErrorCallback();
        }
    }

    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    // 事件循环退出后释放资源
    if (m_httpServer)
    {
        evhttp_free(m_httpServer);
        m_httpServer = nullptr;
        m_evbase = nullptr; // evhttp_free 释放了关联的 event_base
    }
    else if (m_evbase)
    {
        event_base_free(m_evbase);
        m_evbase = nullptr;
    }

    m_port = 0;

    if (init_ok && closeDoneCallback)
    {
        closeDoneCallback();
    }

    changeState(ZM_WS_STATE_STOPPED);
}

/**
 * @brief 停止服务端（可从任意线程调用）
 *
 * 若处于 RUNNING | BOUND 状态，通过 event_active(ZM_WS_CONTROL_CLOSE)
 * 通知事件循环中的 onControlClose 执行清理并退出。
 * 否则直接清理并回调。
 *
 * @note event_active 是线程安全的，可以从非事件循环线程调用。
 */
void ZmMessageWSServer::Stop()
{
    if (hasState(ZM_WS_STATE_RUNNING | ZM_WS_STATE_BOUND))
    {
        if (m_ctrlEvent)
        {
            event_active(m_ctrlEvent, ZM_WS_CONTROL_CLOSE, 0);
        }
    }
    else
    {
        clearAll();

        ZmWSServerCloseDoneCallback closeDoneCallback;
        {
            std::unique_lock<std::recursive_mutex> lock(m_mutex);
            closeDoneCallback = m_closeDoneCallback;
        }

        if (closeDoneCallback)
        {
            closeDoneCallback();
        }
    }
}

// ==================== ZmMessageWSServer 配置接口 ====================

void ZmMessageWSServer::SetMaxConnections(size_t max_conn)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_maxConnections = max_conn;
}

size_t ZmMessageWSServer::GetMaxConnections() const
{
    return m_maxConnections;
}

size_t ZmMessageWSServer::GetCurrentConnectionCount() const
{
    return m_clients.size();
}

void ZmMessageWSServer::SetHeartbeatEnabled(bool enabled)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_heartbeatEnabled = enabled;
}

void ZmMessageWSServer::SetHeartbeatInterval(uint32_t interval_ms)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_heartbeatIntervalMs = interval_ms;
}

void ZmMessageWSServer::SetHeartbeatTimeout(uint32_t timeout_ms)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_heartbeatTimeoutMs = timeout_ms;
}

/**
 * @brief 获取统计数据快照
 *
 * 加锁拷贝统计信息到局部变量后返回，避免外部直接访问内部状态。
 * current_connections 取自 m_clients.size() 而非累计值。
 */
ZmServerStatistics_t ZmMessageWSServer::GetStatistics() const
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    ZmServerStatistics_t stats;
    stats.total_connections = m_statistics.total_connections;
    stats.current_connections = m_clients.size();
    stats.messages_sent = m_statistics.messages_sent;
    stats.messages_received = m_statistics.messages_received;
    stats.messages_dropped = m_statistics.messages_dropped;
    stats.ack_timeouts = m_statistics.ack_timeouts;
    stats.heartbeat_timeouts = m_statistics.heartbeat_timeouts;
    return stats;
}

void ZmMessageWSServer::ResetStatistics()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    memset(&m_statistics, 0, sizeof(m_statistics));
}


// ==================== ZmMessageServer ====================

ZmMessageServer::ZmMessageServer() :
    m_wsServer(nullptr),
    m_bindDoneCallback(nullptr),
    m_closeDoneCallback(nullptr),
    m_maxPendingMessages(ZM_MAX_MESSAGE_LIST_SIZE),
    m_port(0),
    m_readTimeout(0),
    m_writeTimeout(0)
{

}

/**
 * @brief 析构时同步等待服务端关闭完成
 */
ZmMessageServer::~ZmMessageServer()
{
    Stop();
}

/**
 * @brief 工作线程入口
 *
 * 运行 ZmMessageWSServer::Start()（阻塞直到 Stop），
 * 退出后 delete server 释放资源。
 */
void ZmMessageServer::workThread(ZmMessageWSServer* server)
{
    server->Start();
    delete server;
}

/**
 * @brief 启动服务端（阻塞直到绑定完成或失败）
 *
 * 流程：
 * 1. 加锁检查 m_wsServer 是否已存在（防止重复启动）
 * 2. 创建 ZmMessageWSServer 实例并配置
 * 3. 启动独立线程运行事件循环
 * 4. 通过 condition_variable 阻塞等待绑定完成
 *
 * 同步机制：
 * - m_mutex 保护 m_wsServer 指针和配置参数
 * - m_syncMutex 防止 Start 返回后回调仍在执行（回调中先 notify 再 lock m_syncMutex）
 * - m_semaphore + m_semaphoreMutex 实现 Start/Stop 的阻塞等待
 */
void ZmMessageServer::Start()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    std::unique_lock<std::mutex> sync_lock(m_syncMutex);

    if (!m_wsServer)
    {
        m_wsServer = new ZmMessageWSServer();
        m_wsServer->SetBindPort(m_port);
        m_wsServer->SetBindDoneCallback(&ZmMessageServer::onBindDoneCallback, this);
        m_wsServer->SetCloseDoneCallback(&ZmMessageServer::onCloseDoneCallback, this);
        m_wsServer->SetBindErrorCallback(&ZmMessageServer::onBindErrorCallback, this);
        m_wsServer->SetMaxMessagesCache(m_maxPendingMessages);
        m_wsServer->SetReadAndWriteTimeout(m_readTimeout, m_writeTimeout);

        std::thread thread(std::bind(&ZmMessageServer::workThread, this, m_wsServer));
        thread.detach();

        std::unique_lock<std::mutex> semaphore_lock(m_semaphoreMutex);
        m_semaphore.wait(semaphore_lock, [&]()
            {
                return m_wsServer ? m_wsServer->IsBound() : true;
            });
    }
}

/**
 * @brief 停止服务端（阻塞直到关闭完成）
 *
 * 先拷贝 m_wsServer 指针到局部变量（避免持锁调用 Stop），
 * 调用 Stop 后通过 condition_variable 等待 onCloseDoneCallback 将 m_wsServer 置空。
 * 若 m_wsServer 已为空则直接调用关闭回调。
 */
void ZmMessageServer::Stop()
{
    ZmMessageWSServer* wsServer = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        wsServer = m_wsServer;
    }

    if (wsServer)
    {
        wsServer->Stop();

        std::unique_lock<std::mutex> lock(m_mutex);
        m_semaphore.wait(lock, [&]()
            {
                return m_wsServer == nullptr;
            });
    }
    else
    {
        ZmWSServerCloseDoneCallback closeDoneCallback;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            closeDoneCallback = m_closeDoneCallback;
        }

        if (closeDoneCallback)
        {
            closeDoneCallback();
        }
    }
}

void ZmMessageServer::PostNotificationWithTopic(std::string topic, std::string jsonContent)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsServer)
    {
        m_wsServer->PostNotificationWithTopic(topic, jsonContent);
    }
}

void ZmMessageServer::ClearAllCachedMessages()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsServer)
    {
        m_wsServer->ClearAllCachedMessages();
    }
}

bool ZmMessageServer::IsBound()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsServer)
    {
        return m_wsServer->IsBound();
    }

    return false;
}

uint16_t ZmMessageServer::GetBindPort()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsServer)
    {
        return m_wsServer->GetBindPort();
    }

    return m_port;
}

/**
 * @brief 绑定成功回调
 *
 * 由 ZmMessageWSServer 在事件循环线程中通过 onControlBound 触发。
 *
 * 流程：
 * 1. notify_all 唤醒 Start() 中等待绑定的 condition_variable
 * 2. 获取 m_syncMutex（Start 持有此锁，等待 Start 函数退出后释放）
 * 3. 拷贝用户回调到局部变量后释放 m_mutex 再调用
 *
 * 时序保证：
 * Start() 持有 m_syncMutex → wait 在 m_semaphore → 回调 notify_all
 * → Start() 的 wait 返回 → Start() 函数退出释放 m_syncMutex
 * → 回调获得 m_syncMutex → 立即 unlock → 调用用户回调
 */
void ZmMessageServer::onBindDoneCallback(uint16_t port)
{
    {
        m_semaphore.notify_all();
    }

    std::unique_lock<std::mutex> sync_lock(m_syncMutex);
    sync_lock.unlock();

    ZmWSServerBindDoneCallback bindDoneCallback;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        bindDoneCallback = m_bindDoneCallback;
    }

    if (bindDoneCallback)
    {
        bindDoneCallback(port);
    }
}

/**
 * @brief 绑定失败回调
 *
 * 将 m_wsServer 置空（workThread 会在 Start 返回后 delete 它），
 * notify_all 唤醒 Start() 的 wait（谓词 m_wsServer ? IsBound() : true 会返回 true）。
 */
void ZmMessageServer::onBindErrorCallback()
{
    ZmWSServerBindErrorCallback bindErrorCallback;
    {
        m_port = m_wsServer ? m_wsServer->GetBindPort() : 0;
        m_wsServer = nullptr;
        m_semaphore.notify_all();
        bindErrorCallback = m_bindErrorCallback;
    }

    std::unique_lock<std::mutex> sync_lock(m_syncMutex);
    sync_lock.unlock();

    if (bindErrorCallback)
    {
        bindErrorCallback();
    }
}

/**
 * @brief 关闭完成回调
 *
 * 将 m_wsServer 置空，notify_all 唤醒 Stop() 的 wait。
 * Stop() 检测到 m_wsServer == nullptr 后返回。
 */
void ZmMessageServer::onCloseDoneCallback()
{
    ZmWSServerCloseDoneCallback closeDoneCallback;
    {
        m_port = m_wsServer ? m_wsServer->GetBindPort() : 0;
        m_wsServer = nullptr;
        m_semaphore.notify_all();
        closeDoneCallback = m_closeDoneCallback;
    }

    std::unique_lock<std::mutex> sync_lock(m_syncMutex);
    sync_lock.unlock();

    if (closeDoneCallback)
    {
        closeDoneCallback();
    }
}

void ZmMessageServer::SetBindPort(uint16_t port)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    m_port = port;

    if (m_wsServer)
    {
        m_wsServer->SetBindPort(port);
    }
}

void ZmMessageServer::SetMaxMessagesCache(size_t num)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    m_maxPendingMessages = num;

    if (m_wsServer)
    {
        m_wsServer->SetMaxMessagesCache(num);
    }
}

void ZmMessageServer::SetReadAndWriteTimeout(long read_msec, long write_msec)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    m_readTimeout = read_msec;
    m_writeTimeout = write_msec;

    if (m_wsServer)
    {
        m_wsServer->SetReadAndWriteTimeout(read_msec, write_msec);
    }
}

void ZmMessageServer::SetCloseDoneCallback(ZmWSServerCloseDoneCallback cb)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_closeDoneCallback = std::move(cb);
}

void ZmMessageServer::SetBindErrorCallback(ZmWSServerBindErrorCallback cb)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_bindErrorCallback = std::move(cb);
}

void ZmMessageServer::SetBindDoneCallback(ZmWSServerBindDoneCallback cb)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_bindDoneCallback = std::move(cb);
}

// ==================== ZmMessageServer 代理方法 ====================

void ZmMessageServer::SetMaxConnections(size_t max_conn)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_wsServer)
    {
        m_wsServer->SetMaxConnections(max_conn);
    }
}

size_t ZmMessageServer::GetMaxConnections() const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_wsServer)
    {
        return m_wsServer->GetMaxConnections();
    }
    return 0;
}

size_t ZmMessageServer::GetCurrentConnectionCount() const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_wsServer)
    {
        return m_wsServer->GetCurrentConnectionCount();
    }
    return 0;
}

void ZmMessageServer::SetHeartbeatEnabled(bool enabled)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_wsServer)
    {
        m_wsServer->SetHeartbeatEnabled(enabled);
    }
}

void ZmMessageServer::SetHeartbeatInterval(uint32_t interval_ms)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_wsServer)
    {
        m_wsServer->SetHeartbeatInterval(interval_ms);
    }
}

void ZmMessageServer::SetHeartbeatTimeout(uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_wsServer)
    {
        m_wsServer->SetHeartbeatTimeout(timeout_ms);
    }
}

ZmServerStatistics_t ZmMessageServer::GetStatistics() const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_wsServer)
    {
        return m_wsServer->GetStatistics();
    }
    return ZmServerStatistics_t();
}

void ZmMessageServer::ResetStatistics()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_wsServer)
    {
        m_wsServer->ResetStatistics();
    }
}
