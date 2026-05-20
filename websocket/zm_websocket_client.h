#ifndef ZM_WEBSOCKET_CLIENT
#define ZM_WEBSOCKET_CLIENT

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <functional>
#include <unordered_set>
#include <vector>
#include <string>

#include "zm_socket_utils.h"
#include "zm_websocket_utils.h"
#include "zm_websocket_protocol.h"

// 默认连接超时时间(秒)
#ifndef ZM_DEFAULT_CONNECT_TIMEOUT_SECONDS
#define ZM_DEFAULT_CONNECT_TIMEOUT_SECONDS 30
#endif

//ws消息的最终回调
typedef std::function<void(const char*, const char*, void*)> ZmReceiverMessageCallback;
//ws客户端关闭时的回调
typedef std::function<void()> ZmWSClientCloseDoneCallback;
//ws客户端成功连接ws服务端时的回调
typedef std::function<void()> ZmWSClientConnectDoneCallback;
//ws客户端连接ws服务端失败时的回调
typedef std::function<void(short)> ZmWSClientConnectErrorCallback;
//ws消息回调
typedef std::function<void(const char* topic, const char* content)> ZmWSClientReadCallback;

/* WebSocket 客户端：先完成 HTTP Upgrade 握手，再切换到帧通信模式 */
class ZmMessageWSClient : public ZmMessageWSBase
{
public:
    ZmMessageWSClient();
    ~ZmMessageWSClient();

    void Connect(const char* address, uint16_t port);
    void Stop();

public:
    //是否已连接
    bool IsConnected();
    //设置控制socket的套接字的收发超时
    void SetReadAndWriteTimeout(long read_msec = 0, long write_msec = 0) override;
    //设置websocket关闭时的回调
    void SetCloseDoneCallback(ZmWSClientCloseDoneCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetCloseDoneCallback(void (T::* callback)(), T* obj)
    {
        SetCloseDoneCallback(std::bind(callback, obj));
    }
    //设置websocket连接失败时的回调
    void SetConnectErrorCallback(ZmWSClientConnectErrorCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetConnectErrorCallback(void (T::* callback)(short), T* obj)
    {
        SetConnectErrorCallback(std::bind(callback, obj, std::placeholders::_1));
    }
    //设置websocket连接成功时的回调
    void SetConnectDoneCallback(ZmWSClientConnectDoneCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetConnectDoneCallback(void (T::* callback)(), T* obj)
    {
        SetConnectDoneCallback(std::bind(callback, obj));
    }
    //设置websocket收到消息时的回调
    void SetClientReadCallback(ZmWSClientReadCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetClientReadCallback(void (T::* callback)(const char*, const char*), T* obj)
    {
        SetClientReadCallback(std::bind(callback, obj, std::placeholders::_1, std::placeholders::_2));
    }
    //心跳响应配置
    void SetHeartbeatEnabled(bool enabled);
    void SetHeartbeatInterval(uint32_t interval_ms);

private:
    // WebSocket 客户端内部状态
    enum WS_CLIENT_STATE {
        WS_CLIENT_HANDSHAKE,    // TCP 已连接，等待握手完成
        WS_CLIENT_CONNECTED     // WebSocket 握手完成，帧通信模式
    };

    void onBufferedEvent(struct bufferevent* bev, short what, void* arg);
    void onBufferedRead(const char* data, size_t len);
    void onEventControl(evutil_socket_t fd, short what, void* ctx) override;

    // 握手处理
    bool processHandshake(const char* data, size_t len);
    // 帧数据处理
    void processFrameData(const char* data, size_t len);

    void onControlConnected();
    void onControlClose();
    void doSendAction();
    void clearAll();
    void clearEvent();

private:
    struct bufferevent* m_connectBev;
    WS_CLIENT_STATE m_wsState;                  // 当前 WebSocket 状态
    std::string m_handshakeBuffer;              // 累积握手响应
    std::string m_wsKey;                        // 客户端发送的 Sec-WebSocket-Key
    std::vector<char> m_frameBuffer;            // 累积帧数据（可能一次 recv 不完整）

    ZmWSClientReadCallback m_clientReadCallback;
    ZmWSClientConnectDoneCallback m_connectDoneCallback;
    ZmWSClientConnectErrorCallback m_connectErrorCallback;
    ZmWSClientCloseDoneCallback m_closeDoneCallback;

    //心跳配置
    bool m_heartbeatEnabled;
    uint32_t m_heartbeatIntervalMs;
};


struct ZmMessageReceiverObserver {
    const void* observer;
    ZmReceiverMessageCallback method;
    long identifier;
};

struct ZmMessageClientObserverHash {
    size_t operator()(ZmMessageReceiverObserver* a) const {
        if (a == nullptr)
        {
            return 0;
        }
        return std::hash<long>()(a->identifier);
    }
};

struct ZmMessageClientObserverEqual {
    inline bool operator()(ZmMessageReceiverObserver* a, ZmMessageReceiverObserver* b) const {
        if (a == nullptr || b == nullptr)
        {
            return a == b;
        }
        return a->observer == b->observer;
    }
};

using ZmMessageClientObserverSet = std::unordered_set<ZmMessageReceiverObserver*,
    ZmMessageClientObserverHash,
    ZmMessageClientObserverEqual>;

class ZmMessageClient
{
public:
    ZmMessageClient();
    ~ZmMessageClient();

public:
    //连接到服务器,该方法同步
    void Connect(const char* address, uint16_t port);
    //停止连接,该方法同步
    void Stop();

public:
    //监听者模式,添加监听者
    bool AddReceiverObserver(ZmReceiverMessageCallback callback, const void* observer);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    bool AddReceiverObserver(void (T::* callback)(const char*, const char*, void*), T* obj)
    {
        if (obj == nullptr)
        {
            return false;
        }
        return AddReceiverObserver(std::bind(callback, obj, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3), obj);
    }
    //移除监听者
    bool RemoveReceiverObserver(ZmReceiverMessageCallback callback, const void* observer);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    bool RemoveReceiverObserver(void (T::* callback)(const char*, const char*, void*), T* obj)
    {
        if (obj == nullptr)
        {
            return false;
        }
        return RemoveReceiverObserver(std::bind(callback, obj, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3), obj);
    }
    //清空监听者
    bool RemoveAllObserverList();
    //清除消息队列中缓存的消息
    void ClearAllCachedMessages();
    //向ws服务端发送消息
    void PostNotificationWithTopic(std::string topic, std::string jsonContent);
    //设置控制socket的套接字的收发超时
    void SetReconnectTimeout(int msec);
    //设置消息队列中缓存的最大值
    void SetMaxMessagesCache(size_t num = ZM_MAX_MESSAGE_LIST_SIZE);
    //设置控制socket的套接字的收发超时
    void SetReadAndWriteTimeout(long read_msec = 0, long write_msec = 0);
    //设置websocket关闭时的回调
    void SetCloseDoneCallback(ZmWSClientCloseDoneCallback cb = nullptr);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetCloseDoneCallback(void (T::* callback)(), T* obj)
    {
        SetCloseDoneCallback(std::bind(callback, obj));
    }
    //设置websocket连接失败时的回调
    void SetConnectErrorCallback(ZmWSClientConnectErrorCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetConnectErrorCallback(void (T::* callback)(short), T* obj)
    {
        SetConnectErrorCallback(std::bind(callback, obj, std::placeholders::_1));
    }
    //设置websocket连接成功时的回调
    void SetConnectDoneCallback(ZmWSClientConnectDoneCallback cb = nullptr);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetConnectDoneCallback(void (T::* callback)(), T* obj)
    {
        SetConnectDoneCallback(std::bind(callback, obj));
    }
    //心跳配置
    void SetHeartbeatEnabled(bool enabled);
    void SetHeartbeatInterval(uint32_t interval_ms);

private:
    void workThread(ZmMessageWSClient* client, std::string host, uint16_t port);
    void reconnect();
    void onConnectDoneCallback();
    void onConnectErrorCallback(short what);
    void onCloseDoneCallback();
    void onReadCallback(const char* topic, const char* content);

private:
    uint16_t m_port;
    std::string m_host;
    int m_reconnectTimeout;
    size_t m_maxPendingMessages;
    long m_readTimeout;
    long m_writeTimeout;
    std::condition_variable m_semaphore;
    std::mutex m_mutex;
    std::mutex m_semaphoreMutex;
    std::mutex m_syncMutex;
    ZmMessageClientObserverSet m_observerList;
    ZmMessageWSClient* m_wsClient;
    ZmWSClientCloseDoneCallback m_closeDoneCallback;
    ZmWSClientConnectErrorCallback m_connectErrorCallback;
    ZmWSClientConnectDoneCallback m_connectDoneCallback;
};

#endif /* ZM_MESSAGECLIENT_H */
