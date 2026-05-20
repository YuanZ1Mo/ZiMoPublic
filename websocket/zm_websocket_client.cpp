#include "zm_websocket_client.h"
#include "../json11/json11.hpp"

// ==================== ZmMessageWSClient ====================

ZmMessageWSClient::ZmMessageWSClient() :
    ZmMessageWSBase(),
    m_connectBev(nullptr),
    m_wsState(WS_CLIENT_HANDSHAKE),
    m_clientReadCallback(nullptr),
    m_connectDoneCallback(nullptr),
    m_connectErrorCallback(nullptr),
    m_closeDoneCallback(nullptr),
    m_heartbeatEnabled(true),
    m_heartbeatIntervalMs(30000)
{

}

ZmMessageWSClient::~ZmMessageWSClient()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    changeState(ZM_WS_STATE_STOPPED);

    m_clientReadCallback = nullptr;
    m_closeDoneCallback = nullptr;
    m_connectErrorCallback = nullptr;
    m_connectDoneCallback = nullptr;

    clearAll();
}

void ZmMessageWSClient::Connect(const char* address, uint16_t port)
{
    if (hasState(ZM_WS_STATE_RUNNING))
    {
        return;
    }

    changeState(ZM_WS_STATE_RUNNING);
    bool init_ok = false;
    evutil_socket_t connect_fd = -1;

    clearEvent();

    m_handshakeBuffer.clear();
    m_frameBuffer.clear();
    m_wsState = WS_CLIENT_HANDSHAKE;

    zm_util_eventbase_init();

    do
    {
        m_evbase = event_base_new();
        if (!m_evbase)
        {
            break;
        }

        if ((connect_fd = zm_util_create_socket_nonblock(address, port, ZmSocketReuseType::ZM_SO_REUSEADDR)) == -1)
        {
            break;
        }

        m_connectBev = bufferevent_socket_new(m_evbase, connect_fd, BEV_OPT_CLOSE_ON_FREE);
        if (!m_connectBev)
        {
            break;
        }

        bufferevent_enable(m_connectBev, EV_READ | EV_WRITE);
        bufferevent_setwatermark(m_connectBev, EV_WRITE, 0, 0);
        bufferevent_setwatermark(m_connectBev, EV_READ, 0, 0);
        bufferevent_set_timeouts(m_connectBev, &m_readTimeout, &m_writeTimeout);

        // 设置 bufferevent 回调
        bufferevent_setcb(m_connectBev,
            [](struct bufferevent* bev, void* ctx) {
                ZmMessageWSClient* self = static_cast<ZmMessageWSClient*>(ctx);
                struct evbuffer* input = bufferevent_get_input(bev);
                if (input)
                {
                    size_t len = evbuffer_get_length(input);
                    if (len > 0)
                    {
                        std::vector<char> buf(len, 0);
                        bufferevent_read(bev, buf.data(), len);
                        evbuffer_drain(input, len);
                        self->onBufferedRead(buf.data(), len);
                    }
                }
            },
            [](struct bufferevent* bev, void* ctx) {
                (void)(bev);
                (void)(ctx);
            },
            [](struct bufferevent* bev, short what, void* ctx) {
                ZmMessageWSClient* self = static_cast<ZmMessageWSClient*>(ctx);
                self->onBufferedEvent(bev, what, ctx);
            },
            this);

        if (zm_util_be_socket_connect(m_connectBev, address, port) < 0)
        {
            break;
        }

        init_ok = true;

    } while (false);

    if (init_ok)
    {
        m_port = port;

        m_ctrlEvent = event_new(m_evbase, -1, EV_PERSIST | EV_READ,
            [](evutil_socket_t fd, short what, void* callback_arg) {
                ZmMessageWSClient* self = static_cast<ZmMessageWSClient*>(callback_arg);
                self->onEventControl(fd, what, callback_arg);
            }, this);

        if (m_ctrlEvent == nullptr)
        {
            init_ok = false;
            clearAll();
        }
        else
        {
            event_add(m_ctrlEvent, nullptr);

            // TCP 连接成功后，BEV_EVENT_CONNECTED 回调中发送握手请求
            event_base_dispatch(m_evbase);
        }
    }

    if (!init_ok)
    {
        clearAll();

        if (connect_fd >= 0)
        {
            evutil_closesocket(connect_fd);
            connect_fd = -1;
        }
    }

    m_port = 0;

    if (init_ok && m_closeDoneCallback)
    {
        m_closeDoneCallback();
    }

    if (!init_ok && m_connectErrorCallback)
    {
        m_connectErrorCallback(BEV_EVENT_ERROR);
    }

    changeState(ZM_WS_STATE_STOPPED);
}

void ZmMessageWSClient::Stop()
{
    if (hasState(ZM_WS_STATE_RUNNING | ZM_WS_STATE_CONNECTED))
    {
        if (m_ctrlEvent != nullptr)
        {
            event_active(m_ctrlEvent, ZM_WS_CONTROL_CLOSE, 0);
        }
    }
    else
    {
        clearAll();

        ZmWSClientCloseDoneCallback closeDoneCallback;
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

bool ZmMessageWSClient::IsConnected()
{
    return m_wsState == WS_CLIENT_CONNECTED && hasState(ZM_WS_STATE_CONNECTED);
}

void ZmMessageWSClient::SetReadAndWriteTimeout(long read_msec, long write_msec)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    m_readTimeout.tv_sec = read_msec / 1000;
    m_readTimeout.tv_usec = (read_msec % 1000) * 1000;

    m_writeTimeout.tv_sec = write_msec / 1000;
    m_writeTimeout.tv_usec = (write_msec % 1000) * 1000;

    if (m_connectBev)
    {
        bufferevent_set_timeouts(m_connectBev, &m_readTimeout, &m_writeTimeout);
    }
}

void ZmMessageWSClient::SetCloseDoneCallback(ZmWSClientCloseDoneCallback cb)
{
    m_closeDoneCallback = std::move(cb);
}

void ZmMessageWSClient::SetConnectErrorCallback(ZmWSClientConnectErrorCallback cb)
{
    m_connectErrorCallback = std::move(cb);
}

void ZmMessageWSClient::SetConnectDoneCallback(ZmWSClientConnectDoneCallback cb)
{
    m_connectDoneCallback = std::move(cb);
}

void ZmMessageWSClient::SetClientReadCallback(ZmWSClientReadCallback cb)
{
    m_clientReadCallback = std::move(cb);
}

void ZmMessageWSClient::SetHeartbeatEnabled(bool enabled)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_heartbeatEnabled = enabled;
}

void ZmMessageWSClient::SetHeartbeatInterval(uint32_t interval_ms)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_heartbeatIntervalMs = interval_ms;
}

// ==================== 数据接收处理 ====================

void ZmMessageWSClient::onBufferedRead(const char* data, size_t len)
{
    if (!data || len == 0) return;

    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (m_wsState == WS_CLIENT_HANDSHAKE)
    {
        m_handshakeBuffer.append(data, len);
        processHandshake(m_handshakeBuffer.c_str(), m_handshakeBuffer.length());
    }
    else if (m_wsState == WS_CLIENT_CONNECTED)
    {
        // 追加到帧缓冲区
        m_frameBuffer.insert(m_frameBuffer.end(), data, data + len);
        processFrameData(m_frameBuffer.data(), m_frameBuffer.size());
    }
}

bool ZmMessageWSClient::processHandshake(const char* data, size_t len)
{
    // 查找 HTTP 响应头结束标记
    std::string buf(data, len);
    size_t headerEnd = buf.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        return false; // 数据不完整，等待更多数据
    }

    // 验证握手响应
    if (!zm_ws_validate_handshake_response(data, headerEnd + 4, m_wsKey))
    {
        // 握手失败
        m_wsState = WS_CLIENT_HANDSHAKE;
        m_handshakeBuffer.clear();

        ZmWSClientConnectErrorCallback errorCallback;
        {
            std::unique_lock<std::recursive_mutex> lock2(m_mutex);
            errorCallback = m_connectErrorCallback;
        }

        if (errorCallback)
        {
            errorCallback(BEV_EVENT_ERROR);
        }
        return false;
    }

    // 握手成功
    m_wsState = WS_CLIENT_CONNECTED;
    size_t consumed = headerEnd + 4;

    // 处理可能的额外数据（HTTP 响应之后的帧数据）
    if (len > consumed)
    {
        m_frameBuffer.clear();
        m_frameBuffer.insert(m_frameBuffer.end(),
            m_handshakeBuffer.begin() + consumed,
            m_handshakeBuffer.end());
        processFrameData(m_frameBuffer.data(), m_frameBuffer.size());
    }

    m_handshakeBuffer.clear();

    // 通知连接成功
    addState(ZM_WS_STATE_CONNECTED);

    if (m_ctrlEvent)
    {
        event_active(m_ctrlEvent, ZM_WS_CONTROL_CONNECTED, 0);
    }

    return true;
}

void ZmMessageWSClient::processFrameData(const char* data, size_t len)
{
    size_t totalConsumed = 0;

    while (totalConsumed < len)
    {
        uint8_t opcode = 0;
        size_t payloadLen = 0;
        size_t consumedBytes = 0;

        char* payload = zm_ws_decode_frame(data + totalConsumed, len - totalConsumed,
            &opcode, &payloadLen, &consumedBytes);

        if (consumedBytes == 0)
        {
            // 数据不足以构成完整帧，等待更多数据
            break;
        }

        totalConsumed += consumedBytes;

        switch (opcode)
        {
        case ZM_WS_OPCODE_TEXT:
        case ZM_WS_OPCODE_BINARY:
            if (payload && payloadLen > 0)
            {
                // 解析 JSON payload
                std::string buffer(payload, payloadLen);
                std::string err;
                const auto json = json11::Json::parse(buffer, err);

                if (err.empty() && json.is_object())
                {
                    std::string topic;
                    std::string content;

                    if (json["TOPIC"].is_string())
                    {
                        topic = json["TOPIC"].string_value();
                    }
                    if (json["CONTENT"].is_object())
                    {
                        content = json11::Json(json["CONTENT"].object_items()).dump();
                    }
                    else if (json["CONTENT"].is_string())
                    {
                        content = json["CONTENT"].string_value();
                    }

                    ZmWSClientReadCallback readCallback;
                    {
                        std::unique_lock<std::recursive_mutex> lock(m_mutex);
                        readCallback = m_clientReadCallback;
                    }

                    if (readCallback)
                    {
                        readCallback(topic.c_str(), content.c_str());
                    }
                }
            }
            break;

        case ZM_WS_OPCODE_PING:
            // 收到 Ping，回复 Pong
            {
                size_t pongFrameLen = 0;
                char* pongFrame = zm_ws_encode_frame(payload, payloadLen,
                    ZM_WS_OPCODE_PONG, &pongFrameLen, true);
                if (pongFrame && m_connectBev)
                {
                    evbuffer_add(bufferevent_get_output(m_connectBev), pongFrame, pongFrameLen);
                    bufferevent_flush(m_connectBev, EV_WRITE, BEV_FLUSH);
                    free(pongFrame);
                }
            }
            break;

        case ZM_WS_OPCODE_PONG:
            // Pong 响应，更新心跳状态
            break;

        case ZM_WS_OPCODE_CLOSE:
            // 服务端发起关闭
            {
                if (m_ctrlEvent)
                {
                    event_active(m_ctrlEvent, ZM_WS_CONTROL_CLOSE, 0);
                }
            }
            break;

        default:
            break;
        }

        if (payload)
        {
            free(payload);
        }
    }

    // 移除已消费的数据
    if (totalConsumed > 0 && totalConsumed <= m_frameBuffer.size())
    {
        m_frameBuffer.erase(m_frameBuffer.begin(),
            m_frameBuffer.begin() + totalConsumed);
    }
}

// ==================== bufferevent 事件回调 ====================

void ZmMessageWSClient::onBufferedEvent(struct bufferevent* bev, short what, void* arg)
{
    (void)(bev);
    (void)(arg);

    if (what & BEV_EVENT_EOF)
    {
        removeState(ZM_WS_STATE_CONNECTED);
    }
    else if (what & BEV_EVENT_ERROR)
    {
        removeState(ZM_WS_STATE_CONNECTED);
    }
    else if (what & BEV_EVENT_TIMEOUT)
    {
        removeState(ZM_WS_STATE_CONNECTED);
    }
    else if (what & BEV_EVENT_CONNECTED)
    {
        // TCP 连接成功，发送 WebSocket 握手请求
        std::string host = "127.0.0.1";
        if (m_port != 80)
        {
            host += ":" + std::to_string(m_port);
        }

        // 生成握手 Key 并保存，用于后续验证
        m_wsKey = zm_ws_generate_key();

        // 构造握手请求
        char portStr[8] = { 0 };
        if (m_port != 80)
        {
            snprintf(portStr, sizeof(portStr), ":%u", m_port);
        }

        std::string request;
        request += "GET / HTTP/1.1\r\n";
        request += "Host: ";
        // 尝试从连接获取地址，使用默认值
        request += "127.0.0.1";
        request += portStr;
        request += "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: ";
        request += m_wsKey;
        request += "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";

        if (m_connectBev)
        {
            evbuffer_add(bufferevent_get_output(m_connectBev),
                request.c_str(), request.length());
            bufferevent_flush(m_connectBev, EV_WRITE, BEV_FLUSH);
        }

        return;
    }

    ZmWSClientConnectErrorCallback errorCallback;
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        errorCallback = m_connectErrorCallback;
    }

    if (errorCallback)
    {
        errorCallback(what);
    }
}

// ==================== 控制事件 ====================

void ZmMessageWSClient::onEventControl(evutil_socket_t fd, short what, void* ctx)
{
    (void)(fd);

    if (ctx)
    {
        ZmMessageWSClient* client = static_cast<ZmMessageWSClient*>(ctx);

        if (ZM_WS_CONTROL_SEND & what)
        {
            client->doSendAction();
        }

        if (ZM_WS_CONTROL_CLOSE & what)
        {
            client->onControlClose();
        }

        if (ZM_WS_CONTROL_CONNECTED & what)
        {
            client->onControlConnected();
        }
    }
}

void ZmMessageWSClient::onControlConnected()
{
    ZmWSClientConnectDoneCallback connectDoneCallback;
    {
        std::unique_lock<std::recursive_mutex> lock(m_mutex);
        connectDoneCallback = m_connectDoneCallback;
    }

    if (connectDoneCallback)
    {
        connectDoneCallback();
    }
}

void ZmMessageWSClient::onControlClose()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    ClearAllCachedMessages();

    if (m_connectBev != nullptr)
    {
        bufferevent_free(m_connectBev);
        m_connectBev = nullptr;
    }

    if (m_ctrlEvent != nullptr)
    {
        event_free(m_ctrlEvent);
        m_ctrlEvent = nullptr;
    }

    if (m_evbase != nullptr)
    {
        event_base_loopbreak(m_evbase);
    }
}

// ==================== 发送消息 ====================

void ZmMessageWSClient::doSendAction()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (m_wsState != WS_CLIENT_CONNECTED || !m_connectBev)
    {
        return;
    }

    while (!m_pendingMessages.empty())
    {
        auto msgIt = m_pendingMessages.begin();
        ZmMessageItem_t* message_item = *msgIt;
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

        // 用 WebSocket 帧编码（客户端发送需要 mask）
        size_t frameLen = 0;
        char* frame = zm_ws_encode_frame(
            static_cast<const char*>(message_item->data),
            message_item->len,
            ZM_WS_OPCODE_TEXT, &frameLen, true);

        if (!frame)
        {
            // 内存分配失败，将消息放回队列头部并退出
            m_pendingMessages.insert(m_pendingMessages.begin(), message_item);
            break;
        }

        evbuffer_add(bufferevent_get_output(m_connectBev), frame, frameLen);
        bufferevent_flush(m_connectBev, EV_WRITE, BEV_FLUSH);

        free(frame);

        if (message_item->data)
        {
            free(message_item->data);
            message_item->data = nullptr;
        }
        free(message_item);
        message_item = nullptr;
    }

    if (!m_pendingMessages.empty() && m_ctrlEvent != nullptr)
    {
        event_active(m_ctrlEvent, ZM_WS_CONTROL_SEND, 0);
    }
}

// ==================== 清理 ====================

void ZmMessageWSClient::clearAll()
{
    ClearAllCachedMessages();
    clearEvent();

    m_handshakeBuffer.clear();
    m_frameBuffer.clear();
}

void ZmMessageWSClient::clearEvent()
{
    if (m_connectBev != nullptr)
    {
        bufferevent_free(m_connectBev);
        m_connectBev = nullptr;
    }

    if (m_ctrlEvent != nullptr)
    {
        event_free(m_ctrlEvent);
        m_ctrlEvent = nullptr;
    }

    if (m_evbase != nullptr)
    {
        event_base_free(m_evbase);
        m_evbase = nullptr;
    }
}


// ==================== ZmMessageClient ====================

ZmMessageClient::ZmMessageClient() :
    m_observerList(0),
    m_wsClient(nullptr),
    m_port(0),
    m_host(""),
    m_reconnectTimeout(0),
    m_closeDoneCallback(nullptr),
    m_connectErrorCallback(nullptr),
    m_connectDoneCallback(nullptr),
    m_maxPendingMessages(ZM_MAX_MESSAGE_LIST_SIZE),
    m_readTimeout(0),
    m_writeTimeout(0)
{

}

ZmMessageClient::~ZmMessageClient()
{
    Stop();
    RemoveAllObserverList();
};

void ZmMessageClient::Connect(const char* address, uint16_t port)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsClient != nullptr)
    {
        return;
    }

    m_host = address ? address : "";
    m_port = port;

    try
    {
        m_wsClient = new ZmMessageWSClient();
    }
    catch (const std::bad_alloc&)
    {
        return;
    }

    m_wsClient->SetConnectDoneCallback(&ZmMessageClient::onConnectDoneCallback, this);
    m_wsClient->SetConnectErrorCallback(&ZmMessageClient::onConnectErrorCallback, this);
    m_wsClient->SetCloseDoneCallback(&ZmMessageClient::onCloseDoneCallback, this);
    m_wsClient->SetClientReadCallback(&ZmMessageClient::onReadCallback, this);
    m_wsClient->SetMaxMessagesCache(m_maxPendingMessages);
    m_wsClient->SetReadAndWriteTimeout(m_readTimeout, m_writeTimeout);

    std::unique_lock<std::mutex> lock_sync(m_syncMutex);
    lock.unlock();

    std::thread thread(std::bind(&ZmMessageClient::workThread, this, m_wsClient, m_host, m_port));
    thread.detach();

    std::unique_lock<std::mutex> semaphore_lock(m_semaphoreMutex);
    m_semaphore.wait_for(semaphore_lock, std::chrono::seconds(DEFAULT_CONNECT_TIMEOUT_SECONDS), [&]()
        {
            return m_wsClient ? m_wsClient->IsConnected() : true;
        });
}

void ZmMessageClient::Stop()
{
    ZmMessageWSClient* wsClient = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        wsClient = m_wsClient;
    }

    if (wsClient)
    {
        wsClient->Stop();

        std::unique_lock<std::mutex> lock(m_mutex);
        m_semaphore.wait(lock, [&]()
            {
                return m_wsClient == nullptr;
            });
    }
    else
    {
        ZmWSClientCloseDoneCallback closeDoneCallback;
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

bool ZmMessageClient::AddReceiverObserver(ReceiverMessageCallback callback, const void* observer)
{
    if (observer == nullptr || !callback)
    {
        return false;
    }

    std::unique_lock<std::mutex> lock(m_mutex);

    ZmMessageReceiverObserver* receiver_observer = nullptr;
    ZmMessageReceiverObserver lookup_receiver_observer;
    lookup_receiver_observer.observer = observer;
    lookup_receiver_observer.identifier = reinterpret_cast<long>(observer);

    auto it = m_observerList.find(&lookup_receiver_observer);
    receiver_observer = (it == m_observerList.end()) ? nullptr : *it;

    if (receiver_observer == nullptr)
    {
        try
        {
            receiver_observer = new ZmMessageReceiverObserver();
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        receiver_observer->observer = observer;
        receiver_observer->method = callback;
        receiver_observer->identifier = reinterpret_cast<long>(observer);

        m_observerList.insert(receiver_observer);
    }

    return true;
}

bool ZmMessageClient::RemoveReceiverObserver(ReceiverMessageCallback callback, const void* observer)
{
    if (observer == nullptr || !callback)
    {
        return false;
    }

    std::unique_lock<std::mutex> lock(m_mutex);

    ZmMessageReceiverObserver lookup_receiver_observer;
    lookup_receiver_observer.observer = observer;
    lookup_receiver_observer.identifier = reinterpret_cast<long>(observer);

    auto it = m_observerList.find(&lookup_receiver_observer);

    if (it != m_observerList.end())
    {
        ZmMessageReceiverObserver* found_observer = *it;
        m_observerList.erase(it);
        delete found_observer;

        return true;
    }

    return false;
}

bool ZmMessageClient::RemoveAllObserverList()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (!m_observerList.empty())
    {
        for (auto it = m_observerList.begin(); it != m_observerList.end(); )
        {
            ZmMessageReceiverObserver* observer = *it;
            it = m_observerList.erase(it);
            delete observer;
        }

        return true;
    }

    return false;
}

void ZmMessageClient::ClearAllCachedMessages()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsClient)
    {
        m_wsClient->ClearAllCachedMessages();
    }
}

void ZmMessageClient::PostNotificationWithTopic(std::string topic, std::string jsonContent)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsClient)
    {
        m_wsClient->PostNotificationWithTopic(topic, jsonContent);
    }
}

void ZmMessageClient::SetReconnectTimeout(int msec)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_reconnectTimeout = msec;
}

void ZmMessageClient::SetMaxMessagesCache(size_t num)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    m_maxPendingMessages = num;

    if (m_wsClient)
    {
        m_wsClient->SetMaxMessagesCache(num);
    }
}

void ZmMessageClient::SetReadAndWriteTimeout(long read_msec, long write_msec)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    m_readTimeout = read_msec;
    m_writeTimeout = write_msec;

    if (m_wsClient)
    {
        m_wsClient->SetReadAndWriteTimeout(read_msec, write_msec);
    }
}

void ZmMessageClient::SetCloseDoneCallback(ZmWSClientCloseDoneCallback cb)
{
    m_closeDoneCallback = std::move(cb);
}

void ZmMessageClient::SetConnectErrorCallback(ZmWSClientConnectErrorCallback cb)
{
    m_connectErrorCallback = std::move(cb);
}

void ZmMessageClient::SetConnectDoneCallback(ZmWSClientConnectDoneCallback cb)
{
    m_connectDoneCallback = std::move(cb);
}

void ZmMessageClient::SetHeartbeatEnabled(bool enabled)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsClient)
    {
        m_wsClient->SetHeartbeatEnabled(enabled);
    }
}

void ZmMessageClient::SetHeartbeatInterval(uint32_t interval_ms)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_wsClient)
    {
        m_wsClient->SetHeartbeatInterval(interval_ms);
    }
}

void ZmMessageClient::workThread(ZmMessageWSClient* client, std::string host, uint16_t port)
{
    client->Connect(host.c_str(), port);
    delete client;
}

void ZmMessageClient::reconnect()
{
    int reconnectTimeout = 0;
    std::string host;
    uint16_t port = 0;
    bool shouldReconnect = false;

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        reconnectTimeout = m_reconnectTimeout;
        host = m_host;
        port = m_port;

        if (reconnectTimeout >= 0 && m_wsClient == nullptr)
        {
            shouldReconnect = true;
        }
    }

    if (shouldReconnect && !host.empty())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(reconnectTimeout));
        Connect(host.c_str(), port);
    }
}

void ZmMessageClient::onConnectDoneCallback()
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_semaphore.notify_all();
    }

    std::unique_lock<std::mutex> sync_lock(m_syncMutex);
    sync_lock.unlock();

    ZmWSClientConnectDoneCallback connectDoneCallback;
    {
        std::unique_lock<std::mutex> innerLock(m_mutex);
        connectDoneCallback = m_connectDoneCallback;
    }

    if (connectDoneCallback)
    {
        connectDoneCallback();
    }
}

void ZmMessageClient::onConnectErrorCallback(short what)
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wsClient = nullptr;
        m_semaphore.notify_all();
    }

    std::unique_lock<std::mutex> sync_lock(m_syncMutex);
    sync_lock.unlock();

    ZmWSClientConnectErrorCallback connectErrorCallback;
    {
        std::unique_lock<std::mutex> innerLock(m_mutex);
        connectErrorCallback = m_connectErrorCallback;
    }

    if (connectErrorCallback)
    {
        connectErrorCallback(what);
    }

    reconnect();
}

void ZmMessageClient::onCloseDoneCallback()
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wsClient = nullptr;
        m_semaphore.notify_all();
    }

    std::unique_lock<std::mutex> sync_lock(m_syncMutex);
    sync_lock.unlock();

    ZmWSClientCloseDoneCallback closeDoneCallback;
    {
        std::unique_lock<std::mutex> innerLock(m_mutex);
        closeDoneCallback = m_closeDoneCallback;
    }

    if (closeDoneCallback)
    {
        closeDoneCallback();
    }
}

void ZmMessageClient::onReadCallback(const char* topic, const char* content)
{
    std::vector<std::pair<const void*, std::function<void(const char*, const char*, void*)>>> observersCopy;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (const auto& observer : m_observerList)
        {
            if (observer != nullptr && observer->method)
            {
                observersCopy.emplace_back(observer->observer, observer->method);
            }
        }
    }

    for (const auto& obs : observersCopy)
    {
        if (obs.second)
        {
            obs.second(topic, content, const_cast<void*>(obs.first));
        }
    }
}
