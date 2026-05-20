/**
 * @file zm_websocket_server.h
 * @brief 基于 libevent evhttp + evws 的 WebSocket 服务端
 *
 * 提供两层封装：
 * - ZmMessageWSServer：核心服务端，运行在 libevent 事件循环线程中
 * - ZmMessageServer：线程安全的同步包装层，供业务层直接调用
 *
 * 服务端通过 evhttp 监听 HTTP 请求，由 evws_new_session 自动完成
 * WebSocket Upgrade 握手，之后通过 evws_connection 进行全双工帧通信。
 * 帧编解码由 libevent 内部处理，服务端无需手动处理 RFC 6455 帧格式。
 */
#ifndef ZM_WEBSOCKET_SERVER_H
#define ZM_WEBSOCKET_SERVER_H

#include "zm_websocket_utils.h"

#include <condition_variable>
#include <event2/http.h>
extern "C" {
#include <event2/ws.h>
}
#include <event2/buffer.h>
#include <event2/bufferevent.h>

/** 监听队列最大连接数 */
#ifndef ZM_LISTEN_BACKLOG
#define ZM_LISTEN_BACKLOG 128
#endif

/**
 * @brief WebSocket 客户端连接上下文
 *
 * 每个连接到服务端的客户端都会创建一个此结构体实例，
 * 保存在 ZmMessageWSServer::m_clients 列表中。
 */
struct ZmMessageWsClient
{
    void* server;                           // 关联的 ZmMessageWSServer 指针
    struct evws_connection* ws_conn;        // libevent WebSocket 连接句柄
    uint32_t client_id;                     // 客户端唯一标识符（递增分配）
    time_t last_activity;                   // 最后活动时间（用于心跳超时检测）
};

typedef struct ZmMessageWsClient ZmMessageWsClient_t;

/** 服务端关闭完成回调 */
typedef std::function<void()> ZmWSServerCloseDoneCallback;
/** 服务端绑定失败回调 */
typedef std::function<void()> ZmWSServerBindErrorCallback;
/** 服务端绑定成功回调，参数为实际绑定的端口号 */
typedef std::function<void(uint16_t)> ZmWSServerBindDoneCallback;

/**
 * @class ZmMessageWSServer
 * @brief WebSocket 核心服务端
 *
 * 基于 libevent evhttp + evws 实现，运行在独立的事件循环线程中。
 *
 * 生命周期：Start() → 事件循环 → Stop()
 * - Start() 创建 evhttp + event_base，绑定端口后进入 event_base_dispatch 阻塞
 * - Stop() 通过 event_active(ZM_WS_CONTROL_CLOSE) 通知事件循环退出
 *
 * 消息发送模型：
 * - PostNotificationWithTopic → 消息入队 → event_active(ZM_WS_CONTROL_SEND)
 * - 事件循环中 onControlSend() 取出消息 → evws_send_binary 广播给所有客户端
 *
 * 线程安全：
 * - 所有 evhttp/evws 回调在事件循环线程中串行执行
 * - m_mutex 保护 m_clients / m_pendingMessages / m_statistics 等共享状态
 * - 外部调用 PostNotificationWithTopic 通过 event_active 委托给事件循环线程
 */
class ZmMessageWSServer : public ZmMessageWSBase
{
public:
    ZmMessageWSServer();
    ~ZmMessageWSServer();

public:
    /**
     * @brief 启动服务端（阻塞，在事件循环线程中调用）
     *
     * 创建 evhttp → 绑定端口 → 设置回调 → 进入 event_base_dispatch 阻塞。
     * 绑定成功后通过 ZM_WS_CONTROL_BOUND 事件触发 onControlBound 回调。
     * 事件循环退出后清理资源并调用 closeDoneCallback。
     */
    void Start();

    /**
     * @brief 停止服务端（可从任意线程调用）
     *
     * 若正在运行则通过 event_active(ZM_WS_CONTROL_CLOSE) 通知事件循环退出。
     * 否则直接清理并回调。
     */
    void Stop();

public:
    /** 设置要监听的端口（Start 前调用），0 表示随机分配 */
    void SetBindPort(uint16_t port);

    /** 设置读写超时（毫秒），0 表示无限等待 */
    void SetReadAndWriteTimeout(long read_msec = 0, long write_msec = 0) override;

    /** 设置 WebSocket 关闭完成时的回调 */
    void SetCloseDoneCallback(ZmWSServerCloseDoneCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetCloseDoneCallback(void (T::* callback)(), T* obj)
    {
        SetCloseDoneCallback(std::bind(callback, obj));
    }

    /** 设置端口绑定成功时的回调，参数为实际绑定的端口号 */
    void SetBindDoneCallback(ZmWSServerBindDoneCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetBindDoneCallback(void (T::* callback)(uint16_t), T* obj)
    {
        SetBindDoneCallback(std::bind(callback, obj, std::placeholders::_1));
    }

    /** 设置端口绑定失败时的回调 */
    void SetBindErrorCallback(ZmWSServerBindErrorCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetBindErrorCallback(void (T::* callback)(), T* obj)
    {
        SetBindErrorCallback(std::bind(callback, obj));
    }

    /** 当前是否绑定端口成功 */
    bool IsBound();

    /** 获取当前绑定的端口号，未绑定或已停止时为 0 */
    uint16_t GetBindPort();

    // ---- 连接限制 ----

    /** 设置最大客户端连接数，0 表示不限制 */
    void SetMaxConnections(size_t max_conn);
    size_t GetMaxConnections() const;
    size_t GetCurrentConnectionCount() const;

    // ---- 心跳配置（已声明，定时器检测未实现） ----

    void SetHeartbeatEnabled(bool enabled);
    void SetHeartbeatInterval(uint32_t interval_ms);
    void SetHeartbeatTimeout(uint32_t timeout_ms);

    // ---- 统计信息 ----

    ZmServerStatistics_t GetStatistics() const;
    void ResetStatistics();

protected:
    /** 控制事件分发：根据 WS_CONTROL 位分发到 onControlSend / onControlBound / onControlClose */
    void onEventControl(evutil_socket_t fd, short what, void* ctx) override;

private:
    /**
     * @brief HTTP 请求回调（静态方法，由 evhttp 通用回调触发）
     *
     * 处理 WebSocket Upgrade 请求：
     * 1. 检查连接数限制
     * 2. evws_new_session 自动完成握手并创建 evws_connection
     * 3. 创建 ZmMessageWsClient_t 加入客户端列表
     * 4. 触发缓存消息发送
     */
    static void onHttpRequest(struct evhttp_request* req, void* arg);

    /**
     * @brief WebSocket 消息回调（静态方法）
     *
     * 由 libevent 在收到客户端 WebSocket 消息时调用。
     * 当前仅更新 last_activity 和统计信息，不解析业务内容。
     * Ping/Pong 由 evws 库自动处理，不会到达此回调。
     */
    static void onWsMessage(struct evws_connection* wsConn, int type,
        const unsigned char* data, size_t len, void* arg);

    /**
     * @brief WebSocket 连接关闭回调（静态方法）
     *
     * 客户端断开连接时由 libevent 调用。
     * 从 m_clients 中移除并释放客户端资源。
     * 当所有客户端断开后移除 CONNECTED 状态。
     */
    static void onWsClose(struct evws_connection* wsConn, void* arg);

    /** ZM_WS_CONTROL_BOUND 处理：标记绑定成功，回调通知 */
    void onControlBound();

    /** ZM_WS_CONTROL_CLOSE 处理：清理所有客户端和消息，退出事件循环 */
    void onControlClose();

    /** ZM_WS_CONTROL_SEND 处理：从队列取消息广播给所有客户端 */
    void onControlSend();

    /** 清理所有客户端连接并释放资源 */
    void clearAllClients();

    /** 清理所有资源（客户端 + 消息 + 事件 + HTTP 服务器） */
    void clearAll();

    /** 根据 evws_connection 指针查找对应的客户端对象 */
    ZmMessageWsClient_t* findClientByConn(struct evws_connection* wsConn);

private:
    struct evhttp* m_httpServer;                            // HTTP 服务器（处理 WebSocket Upgrade）
    std::vector<ZmMessageWsClient_t*> m_clients;           // 当前已连接的客户端列表

    ZmWSServerCloseDoneCallback m_closeDoneCallback;
    ZmWSServerBindErrorCallback m_bindErrorCallback;
    ZmWSServerBindDoneCallback m_bindDoneCallback;

    // 连接限制
    size_t m_maxConnections;                                // 最大连接数（0=不限制）
    std::atomic<uint32_t> m_nextClientId;                   // 客户端 ID 递增计数器

    // 心跳配置（定时器检测未实现）
    bool m_heartbeatEnabled;
    uint32_t m_heartbeatIntervalMs;
    uint32_t m_heartbeatTimeoutMs;

    // 统计信息
    mutable ZmServerStatistics_t m_statistics;
};


/**
 * @class ZmMessageServer
 * @brief 线程安全的同步包装层
 *
 * 将 ZmMessageWSServer 封装为可同步调用的接口。
 *
 * 核心设计：
 * - ZmMessageWSServer 运行在独立线程（std::thread + detach）
 * - Start() 通过 condition_variable 阻塞等待绑定完成
 * - Stop() 通过 condition_variable 阻塞等待关闭完成
 * - 所有代理方法加锁保护 m_wsServer 指针
 *
 * 回调链：
 * ZmMessageWSServer 回调 → ZmMessageServer::onXxxCallback()
 *   → 设置 m_wsServer = nullptr（错误/关闭时）
 *   → notifzm_all 唤醒等待的 Start/Stop
 *   → 释放 m_syncMutex
 *   → 拷贝用户回调到局部变量后调用
 *
 * 使用方式：
 * @code
 *   ZmMessageServer server;
 *   server.SetBindPort(8080);
 *   server.SetBindDoneCallback([](uint16_t port) { printf("bound on %d", port); });
 *   server.Start();  // 阻塞直到绑定完成
 *   server.PostNotificationWithTopic("event", "{\"key\":\"value\"}");
 *   server.Stop();   // 阻塞直到关闭完成
 * @endcode
 */
class ZmMessageServer
{
public:
    ZmMessageServer();
    ~ZmMessageServer();

    ZmMessageServer(const ZmMessageServer&) = delete;
    ZmMessageServer& operator=(const ZmMessageServer&) = delete;

public:
    /**
     * @brief 启动服务端（阻塞直到绑定完成或失败）
     *
     * 在独立线程中运行 ZmMessageWSServer::Start()，
     * 通过 condition_variable 等待绑定结果后返回。
     */
    void Start();

    /**
     * @brief 停止服务端（阻塞直到关闭完成）
     *
     * 调用 ZmMessageWSServer::Stop()，
     * 通过 condition_variable 等待关闭回调后返回。
     */
    void Stop();

public:
    /**
     * @brief 向所有已连接的客户端广播消息
     * @param topic 消息主题
     * @param jsonContent JSON 内容字符串
     */
    void PostNotificationWithTopic(std::string topic, std::string jsonContent);

    /** 设置监听端口，0 表示随机分配 */
    void SetBindPort(uint16_t port = 0);

    /** 设置消息队列最大缓存数 */
    void SetMaxMessagesCache(size_t num = ZM_MAX_MESSAGE_LIST_SIZE);

    /** 设置读写超时（毫秒） */
    void SetReadAndWriteTimeout(long read_msec = 0, long write_msec = 0);

    /** 设置关闭完成回调 */
    void SetCloseDoneCallback(ZmWSServerCloseDoneCallback cb = nullptr);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetCloseDoneCallback(void (T::* callback)(), T* obj)
    {
        SetCloseDoneCallback(std::bind(callback, obj));
    }

    /** 设置绑定失败回调 */
    void SetBindErrorCallback(ZmWSServerBindErrorCallback cb);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetBindErrorCallback(void (T::* callback)(), T* obj)
    {
        SetBindErrorCallback(std::bind(callback, obj));
    }

    /** 设置绑定成功回调 */
    void SetBindDoneCallback(ZmWSServerBindDoneCallback cb = nullptr);
    template <typename T, typename = std::enable_if_t<std::is_class_v<T>>>
    void SetBindDoneCallback(void (T::* callback)(uint16_t), T* obj)
    {
        SetBindDoneCallback(std::bind(callback, obj, std::placeholders::_1));
    }

    /** 清空缓存消息 */
    void ClearAllCachedMessages();

    /** 当前是否绑定成功 */
    bool IsBound();

    /** 获取实际绑定端口，未绑定时为 0 */
    uint16_t GetBindPort();

    // ---- 连接限制（代理到 ZmMessageWSServer） ----

    void SetMaxConnections(size_t max_conn);
    size_t GetMaxConnections() const;
    size_t GetCurrentConnectionCount() const;

    // ---- 心跳配置（代理到 ZmMessageWSServer） ----

    void SetHeartbeatEnabled(bool enabled);
    void SetHeartbeatInterval(uint32_t interval_ms);
    void SetHeartbeatTimeout(uint32_t timeout_ms);

    // ---- 统计信息（代理到 ZmMessageWSServer） ----

    ZmServerStatistics_t GetStatistics() const;
    void ResetStatistics();

private:
    /** 工作线程入口：运行 ZmMessageWSServer::Start() 后 delete server */
    void workThread(ZmMessageWSServer* server);

    // ---- 回调入口（由 ZmMessageWSServer 在事件循环线程中调用） ----

    /** 绑定成功：notifzm_all 唤醒 Start 的 wait，释放 m_syncMutex 后调用用户回调 */
    void onBindDoneCallback(uint16_t port);

    /** 绑定失败：置空 m_wsServer，notifzm_all 唤醒 Start 的 wait，释放 m_syncMutex 后调用用户回调 */
    void onBindErrorCallback();

    /** 关闭完成：置空 m_wsServer，notifzm_all 唤醒 Stop 的 wait，释放 m_syncMutex 后调用用户回调 */
    void onCloseDoneCallback();

private:
    ZmMessageWSServer* m_wsServer;               // 核心服务端实例（运行在工作线程中）
    std::condition_variable m_semaphore;         // 条件变量，同步 Start/Stop 的阻塞等待
    mutable std::mutex m_mutex;                  // 保护 m_wsServer 和所有回调指针
    std::mutex m_semaphoreMutex;                 // condition_variable 的配套 mutex
    std::mutex m_syncMutex;                      // 回调同步锁，防止 Start 返回前回调仍在执行
    uint16_t m_port;                             // 缓存的端口号
    ZmWSServerCloseDoneCallback m_closeDoneCallback;
    ZmWSServerBindErrorCallback m_bindErrorCallback;
    ZmWSServerBindDoneCallback m_bindDoneCallback;
    size_t m_maxPendingMessages;                 // 缓存的消息队列上限
    long m_readTimeout;                          // 缓存的读超时
    long m_writeTimeout;                         // 缓存的写超时
};

#endif /* ZM_WEBSOCKET_SERVER_H */
