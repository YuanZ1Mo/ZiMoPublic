/**
 * @file zm_net_broadcast_server.h
 * @brief TCP 广播服务端
 *
 * 基于 libevent + ZmEvBaseRunLoop 实现的一对多消息推送服务端。
 *
 * 核心特性:
 *   - TCP 监听（可配置地址/端口，端口绑定失败无限重试）
 *   - 客户端握手（settings → confirm_settings → 分配 client_id）
 *   - 双向活动检测心跳（服务端主导 ping/pong）
 *   - Tag 过滤订阅（subscribe / unsubscribe）
 *   - 立即/延时/定时消息发送（线程安全）
 *   - 每客户端独立消息队列（溢出丢弃最旧）
 *   - 连接数限制、按 client_id 踢出
 */

#ifndef ZM_NET_BROADCAST_SERVER_H
#define ZM_NET_BROADCAST_SERVER_H

#include "zm_net_broadcast_base.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <event2/util.h>

struct event_base;
struct evconnlistener;
struct bufferevent;
struct event;
class ZmEvBaseRunLoop;

// ============================================================================
// 服务端配置
// ============================================================================

/**
 * @brief 广播服务端配置
 */
struct BcServerConfig
{
    std::string listenIp;           ///< 监听地址，默认 "0.0.0.0"
    uint16_t    listenPort;         ///< 监听端口，0 = 随机分配
    int         maxConnections;     ///< 最大连接数，0 = 不限制
    int         heartbeatTime;      ///< 心跳超时秒数，默认 60
    int         handshakeTimeout;   ///< 握手超时秒数，默认 10
    size_t      clientQueueMaxSize; ///< 每客户端消息队列上限，默认 1024
    ZmEvBaseRunLoop* evLoop;        ///< 事件循环线程（必填）

    BcServerConfig()
        : listenIp("0.0.0.0")
        , listenPort(0)
        , maxConnections(0)
        , heartbeatTime(60)
        , handshakeTimeout(10)
        , clientQueueMaxSize(1024)
        , evLoop(nullptr)
    {}
};

// ============================================================================
// 服务端回调
// ============================================================================

/**
 * @brief 广播服务端事件回调集合
 */
struct BcServerCallbacks
{
    /// 监听成功回调，参数为实际监听端口
    std::function<void(uint16_t port)> onListenSuccess;

    /// 监听失败回调，参数为错误描述
    std::function<void(const std::string& error)> onListenFailed;

    /// 监听停止回调
    std::function<void()> onListenStopped;

    /// 错误回调
    std::function<void(const std::string& error)> onError;

    /// 客户端上线回调（握手完成 + 分配 client_id 后）
    std::function<void(const BcClientInfo&)> onClientOnline;

    /// 客户端离线回调（断开前最后一次快照）
    std::function<void(const BcClientInfo&)> onClientOffline;
};

// ============================================================================
// ZmBroadcastServer
// ============================================================================

/**
 * @brief TCP 广播服务端
 *
 * 使用方法:
 * @code
 *   BcServerConfig cfg;
 *   cfg.listenPort = 8080;
 *   cfg.evLoop = myRunLoop;
 *
 *   BcServerCallbacks cbs;
 *   cbs.onClientOnline = [](const BcClientInfo& info) { ... };
 *
 *   ZmBroadcastServer server(cfg, cbs);
 *   server.Start();
 *   // ...
 *   server.Broadcast("alert", "{\"msg\":\"hello\"}", "all");
 *   // ...
 *   server.Stop();
 * @endcode
 */
class ZmBroadcastServer
{
public:
    // --- 构造 / 析构 ---

    /**
     * @brief 构造广播服务端
     * @param config  服务端配置（evLoop 必填）
     * @param cbs     事件回调集合
     */
    ZmBroadcastServer(const BcServerConfig& config, const BcServerCallbacks& cbs);

    /** @brief 析构时自动调用 Stop() */
    ~ZmBroadcastServer();

    // 禁止拷贝 / 移动
    ZmBroadcastServer(const ZmBroadcastServer&) = delete;
    ZmBroadcastServer& operator=(const ZmBroadcastServer&) = delete;

    // --- 生命周期 ---

    /**
     * @brief 启动监听
     *
     * 异步操作: 内部向事件循环投递绑定任务，结果通过 onListenSuccess / onListenFailed 回调通知。
     *
     * @return true  任务已投递
     * @return false 状态不允许启动（非 IDLE/STOPPED）或 evLoop 未设置
     */
    bool Start();

    /** @brief 停止服务端，直接断开所有客户端连接并释放资源 */
    void Stop();

    // --- 状态查询（线程安全） ---

    /** @brief 获取当前服务端状态 */
    ZM_BROADCAST_STATE GetState() const;

    /** @brief 获取实际监听端口号（仅在 LISTENING 状态下有效） */
    uint16_t GetPort() const;

    /** @brief 获取当前已连接客户端数 */
    int GetConnectionCount() const;

    /** @brief 获取最大连接数限制 */
    int GetMaxConnections() const;

    /** @brief 获取当前待处理任务数 */
    size_t GetGlobalQueueSize() const;

    /** @brief 获取服务已运行秒数（从 LISTENING 开始计时） */
    uint64_t GetRunningTime() const;

    /** @brief 获取累计成功发送消息数 */
    uint64_t GetSentCount() const;

    /** @brief 获取累计丢弃消息数（队列满时丢弃） */
    uint64_t GetDiscardCount() const;

    // --- 客户端查询（线程安全） ---

    /**
     * @brief 获取指定客户端的信息快照
     * @param clientId  客户端 ID
     * @return 客户端信息，若不存在则 clientId 为空
     */
    BcClientInfo GetClientInfo(const std::string& clientId) const;

    /** @brief 获取所有已连接客户端的信息快照 */
    std::vector<BcClientInfo> GetAllClients() const;

    // --- 立即发送（线程安全） ---

    /**
     * @brief 向指定客户端发送消息
     * @param clientId  目标客户端 ID
     * @param topic     消息主题
     * @param content   消息内容（JSON 字符串）
     * @param tag       过滤标签（仅当客户端订阅了此 tag 时送达，空字符串表示无条件发送）
     * @return true 任务已投递，false 参数无效
     */
    bool Send(const std::string& clientId, const std::string& topic,
              const std::string& content, const std::string& tag);

    /**
     * @brief 向所有匹配 tag 的客户端广播消息
     * @param topic     消息主题
     * @param content   消息内容（JSON 字符串）
     * @param tag       过滤标签（仅推送给订阅此 tag 的客户端，空字符串表示全部推送）
     * @return true 任务已投递，false 参数无效
     */
    bool Broadcast(const std::string& topic, const std::string& content,
                   const std::string& tag);

    // --- 延时发送（线程安全） ---

    /**
     * @brief 延时向指定客户端发送消息
     * @param clientId  目标客户端 ID
     * @param topic     消息主题
     * @param content   消息内容
     * @param tag       过滤标签
     * @param delayMs   延迟毫秒数
     * @return true 任务已投递
     */
    bool SendDelayed(const std::string& clientId, const std::string& topic,
                     const std::string& content, const std::string& tag, uint32_t delayMs);

    /**
     * @brief 延时广播
     * @param delayMs  延迟毫秒数
     */
    bool BroadcastDelayed(const std::string& topic, const std::string& content,
                          const std::string& tag, uint32_t delayMs);

    // --- 定时发送（线程安全） ---

    /**
     * @brief 在指定时间点向指定客户端发送消息
     * @param timestampMs  Unix 毫秒时间戳，小于等于当前时间则立即发送
     */
    bool SendAt(const std::string& clientId, const std::string& topic,
                const std::string& content, const std::string& tag, uint64_t timestampMs);

    /**
     * @brief 在指定时间点广播消息
     * @param timestampMs  Unix 毫秒时间戳
     */
    bool BroadcastAt(const std::string& topic, const std::string& content,
                     const std::string& tag, uint64_t timestampMs);

    // --- 客户端管理（线程安全） ---

    /**
     * @brief 踢出指定客户端
     * @param clientId  客户端 ID
     * @return true 客户端存在且已发起断开，false 客户端不存在
     */
    bool KickClient(const std::string& clientId);

    // --- 运行时配置修改 ---

    /** @brief 修改最大连接数限制 */
    void SetMaxConnections(int max);

    /** @brief 修改心跳超时秒数（对已有客户端下次检查生效） */
    void SetHeartbeatTime(int seconds);

    /** @brief 修改每客户端队列上限 */
    void SetClientQueueMaxSize(size_t max);

private:
    // ============================================================
    // 内部客户端状态
    // ============================================================

    /**
     * @brief 单个客户端的完整运行时状态
     */
    struct BcClient
    {
        std::string clientId;               ///< 唯一 ID
        struct bufferevent* bev;            ///< libevent bufferevent
        BcClientInfo info;                  ///< 对外快照信息
        std::deque<std::string> msgQueue;   ///< 已帧编码的待发送消息队列
        struct event* heartbeatTimer;       ///< 心跳检测定时器
        struct event* handshakeTimer;       ///< 握手超时定时器
        bool handshakeDone;                 ///< 握手是否已完成
        uint64_t lastActiveTime;            ///< 最后活跃时间戳（毫秒）
        uint64_t lastDataSentTime;          ///< 最后发送数据时间戳（毫秒）
        ZmBroadcastServer* m_owner;         ///< 所属服务端回指针

        BcClient()
            : bev(nullptr)
            , heartbeatTimer(nullptr)
            , handshakeTimer(nullptr)
            , handshakeDone(false)
            , lastActiveTime(0)
            , lastDataSentTime(0)
            , m_owner(nullptr)
        {
            info.port = 0;
            info.connectTime = 0;
            info.queuePending = 0;
            info.lastActiveTime = 0;
            info.sentCount = 0;
        }
    };

    // ============================================================
    // 线程安全调度
    // ============================================================

    /** @brief 调度任务到事件循环线程执行的类型 */
    enum BcTaskType
    {
        BC_TASK_START       = 1,  ///< 执行 Start 绑定监听
        BC_TASK_STOP        = 2,  ///< 执行 Stop 清理
        BC_TASK_SEND        = 3,  ///< 执行消息发送
        BC_TASK_KICK        = 4,  ///< 执行踢出客户端
    };

    /** @brief 跨线程调度的任务数据 */
    struct BcScheduledTask
    {
        BcTaskType type;
        BcMessage  message;
        std::string clientId;       ///< Send 目标 / Kick 目标
        uint32_t   delayMs;         ///< 延迟毫秒（0 = 立即）
        uint64_t   timestampMs;     ///< 定时时间戳（0 = 不使用）
        bool       isBroadcast;     ///< true = Broadcast, false = Send
        ZmBroadcastServer* m_server; ///< 所属服务端回指针

        BcScheduledTask()
            : type(BC_TASK_SEND)
            , delayMs(0)
            , timestampMs(0)
            , isBroadcast(false)
            , m_server(nullptr)
        {}
    };

    /**
     * @brief 将任务投递到事件循环线程执行
     * @param task  要执行的任务
     */
    void ScheduleTask(const BcScheduledTask& task);

    /** @brief 事件循环线程中实际执行调度的任务 */
    void ExecuteTask(const BcScheduledTask& task);

    // ============================================================
    // 内部方法（仅在事件循环线程中调用）
    // ============================================================

    /** @brief 在事件循环线程中执行绑定监听 */
    void DoStart();

    /** @brief 在事件循环线程中执行停止清理 */
    void DoStop();

    /** @brief 在事件循环线程中执行消息发送 */
    void DoSend(const BcMessage& msg, const std::string& clientId, bool isBroadcast,
                uint32_t delayMs, uint64_t timestampMs);

    /** @brief 向单个客户端投递消息帧 */
    void DeliverToClient(BcClient* client, const std::string& frameJson);

    /** @brief 清理客户端所有资源并通知离线 */
    void RemoveClient(const std::string& clientId);

    // ============================================================
    // libevent 静态回调
    // ============================================================

    /** @brief evconnlistener 新连接回调 */
    static void OnAcceptConnCB(struct evconnlistener* listener,
                               evutil_socket_t fd, struct sockaddr* addr,
                               int socklen, void* ctx);

    /** @brief 客户端 bufferevent 可读回调 */
    static void OnClientReadCB(struct bufferevent* bev, void* ctx);

    /** @brief 客户端 bufferevent 事件回调（EOF/ERROR） */
    static void OnClientEventCB(struct bufferevent* bev, short events, void* ctx);

    /** @brief 客户端写完成回调（从队列取下一帧发送） */
    static void OnClientWriteCB(struct bufferevent* bev, void* ctx);

    /** @brief 握手超时定时器回调 */
    static void OnHandshakeTimeoutCB(evutil_socket_t fd, short what, void* ctx);

    /** @brief 心跳检测定时器回调 */
    static void OnHeartbeatCheckCB(evutil_socket_t fd, short what, void* ctx);

    /** @brief 跨线程调度控制事件回调 */
    static void OnDispatchEventCB(evutil_socket_t fd, short what, void* ctx);

    /** @brief 延时/定时发送到期回调 */
    static void OnDelayedSendCB(evutil_socket_t fd, short what, void* ctx);

    // ============================================================
    // 成员变量
    // ============================================================

    BcServerConfig m_config;                            ///< 配置副本
    BcServerCallbacks m_callbacks;                      ///< 回调副本
    std::atomic<ZM_BROADCAST_STATE> m_state;            ///< 当前状态
    std::atomic<uint16_t> m_listenPort;                 ///< 实际监听端口

    struct evconnlistener* m_listener;                  ///< libevent 监听器
    std::unordered_map<std::string, BcClient*> m_clients;  ///< clientId → 客户端状态

    mutable std::mutex m_clientsMutex;                  ///< 保护 m_clients 的互斥锁
    mutable std::mutex m_taskMutex;                     ///< 保护 m_pendingTasks 的互斥锁
    std::vector<BcScheduledTask> m_pendingTasks;        ///< 待投递的任务队列

    struct event* m_dispatchEvent;                      ///< 跨线程调度事件
    struct event* m_retryTimer;                         ///< 绑定重试定时器

    std::atomic<uint64_t> m_sentCount;                  ///< 累计发送成功数
    std::atomic<uint64_t> m_discardCount;               ///< 累计丢弃数
    uint64_t m_startTime;                               ///< 启动成功时间戳（毫秒），非原子（仅事件循环线程读写）

    int m_retryCount;                                   ///< 绑定重试计数（仅事件循环线程）
    std::thread::id m_loopThreadId;                     ///< 事件循环线程 ID（用于线程检测）
};

#endif // ZM_NET_BROADCAST_SERVER_H
