/**
 * @file zm_websocket_utils.h
 * @brief WebSocket 通信基类 — 状态管理 + 消息队列 + 控制事件抽象
 *
 * ZmMessageWSBase 是 ZmMessageWSServer 和 ZmMessageWSClient 的公共基类，
 * 提供位掩码状态管理、待发送消息队列、JSON 消息格式化等基础能力。
 * 派生类通过实现 onEventControl() 处理控制事件分发。
 *
 * 同时定义了整个 WebSocket 模块共用的数据结构和枚举。
 */
#ifndef ZM_WEBSOCKET_UTILS_H
#define ZM_WEBSOCKET_UTILS_H

#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

#include "zm_util_libevent.h"

/** 消息队列最大缓存数（默认值） */
#define ZM_MAX_MESSAGE_LIST_SIZE      1000

/**
 * @brief 消息载荷结构体
 *
 * 封装了一块堆分配的内存数据及其长度。
 * data 通过 malloc 分配，调用者负责 free。
 */
struct ZmMessageItem
{
    size_t len = 0;       // 消息数据长度
    void* data = nullptr; // 消息数据（malloc 分配）
};

typedef struct ZmMessageItem ZmMessageItem_t;

/**
 * @brief 控制事件类型（位掩码）
 *
 * 通过 event_active() 触发，不关联任何 fd。
 * 派生类在 onEventControl() 中按位检测并分发处理。
 */
enum ZM_WS_CONTROL
{
    ZM_WS_CONTROL_SEND      = 1 << 0,  // 触发消息发送
    ZM_WS_CONTROL_BOUND     = 1 << 1,  // 端口绑定完成
    ZM_WS_CONTROL_CONNECTED = 1 << 2,  // 连接建立完成（Client 专用）
    ZM_WS_CONTROL_CLOSE     = 1 << 3   // 触发关闭
};

/**
 * @brief 发送方标识（预留）
 */
enum ZM_WS_SENDER
{
    ZM_WS_SENDER_SERVER = 1 << 0,
    ZM_WS_SENDER_CLIENT = 1 << 1
};

/**
 * @brief 服务端运行统计信息
 */
struct ZmServerStatistics
{
    size_t total_connections = 0;      // 历史总连接数
    size_t current_connections = 0;    // 当前活跃连接数
    size_t messages_sent = 0;          // 已发送消息数
    size_t messages_received = 0;      // 已接收消息数
    size_t messages_dropped = 0;       // 因队列满而丢弃的消息数（字段已声明，代码中未递增）
    size_t ack_timeouts = 0;           // ACK 超时数（字段已声明，代码中未递增）
    size_t heartbeat_timeouts = 0;     // 心跳超时数（字段已声明，代码中未递增）
};
typedef struct ZmServerStatistics ZmServerStatistics_t;

/**
 * @brief 通信实体运行状态（位掩码）
 *
 * 状态可以叠加（如 RUNNING | BOUND | CONNECTED 表示正在运行且已绑定且有客户端连接）。
 * 使用 std::atomic 保证线程安全的读写。
 */
enum ZM_WS_STATE {
    ZM_WS_STATE_STOPPED   = 0 << 0,  // 未运行
    ZM_WS_STATE_RUNNING   = 1 << 0,  // 事件循环运行中
    ZM_WS_STATE_CONNECTED = 1 << 1,  // 有对端连接
    ZM_WS_STATE_BOUND     = 1 << 2   // 端口绑定成功
};

/* 使 ZM_WS_STATE 枚举支持位运算 */
inline ZM_WS_STATE operator&(ZM_WS_STATE lhs, ZM_WS_STATE rhs)
{
    using T = std::underlying_type_t<ZM_WS_STATE>;
    return static_cast<ZM_WS_STATE>(static_cast<T>(lhs) & static_cast<T>(rhs));
}

inline ZM_WS_STATE operator|(ZM_WS_STATE lhs, ZM_WS_STATE rhs)
{
    using T = std::underlying_type_t<ZM_WS_STATE>;
    return static_cast<ZM_WS_STATE>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

inline ZM_WS_STATE operator~(ZM_WS_STATE state)
{
    using T = std::underlying_type_t<ZM_WS_STATE>;
    return static_cast<ZM_WS_STATE>(~static_cast<T>(state));
}

/**
 * @class ZmMessageWSBase
 * @brief WebSocket 通信实体的抽象基类
 *
 * 提供以下核心能力：
 * 1. 位掩码状态管理（addState / removeState / hasState / changeState），线程安全
 * 2. 待发送消息队列（PostNotificationWithData 入队，onControlSend 出队发送）
 * 3. JSON 消息格式化（PostNotificationWithTopic）
 * 4. 控制事件抽象（onEventControl 纯虚函数，由派生类实现事件分发）
 *
 * 派生类：
 * - ZmMessageWSServer：基于 evhttp + evws 的服务端
 * - ZmMessageWSClient：基于 bufferevent 的客户端
 */
class ZmMessageWSBase
{
protected:
    ZmMessageWSBase();
    ~ZmMessageWSBase() = default;

public:
    /**
     * @brief 发送 topic + json_content 给对端
     * @param topic 消息主题（会经过 JSON 转义）
     * @param json_content JSON 内容（作为 JSON value 原样拼入，调用方负责提供合法 JSON）
     *
     * 内部将 topic 和 content 格式化为 { "TOPIC": "...", "CONTENT": ... } 后调用 PostNotificationWithData。
     */
    virtual void PostNotificationWithTopic(std::string topic, std::string json_content);

    /**
     * @brief 发送 topic + json_content 给对端（C 风格参数版本）
     */
    virtual void PostNotificationWithTopic(const char* topic, size_t topic_len, const char* json_content, size_t content_len);

    /**
     * @brief 发送原始字节流给对端
     * @param bytes 数据指针
     * @param blen 数据长度
     *
     * 将数据拷贝到堆上，封装为 ZmMessageItem_t 加入待发送队列。
     * 当处于 RUNNING | CONNECTED 状态时，自动触发 ZM_WS_CONTROL_SEND 事件。
     * 队列满时丢弃最旧的消息。
     */
    virtual void PostNotificationWithData(const void* bytes, size_t blen);

    /**
     * @brief 设置消息队列最大缓存数
     * @param num 最大缓存数，超出时丢弃队头最旧消息
     */
    virtual void SetMaxMessagesCache(size_t num);

    /**
     * @brief 设置读写超时（毫秒）
     */
    virtual void SetReadAndWriteTimeout(long read_msec = 0, long write_msec = 0);

    /** 获取当前运行状态 */
    virtual ZM_WS_STATE GetCurrentState() const;

    /** 清空所有缓存的消息并释放内存 */
    virtual void ClearAllCachedMessages();

protected:
    /** 在当前状态上叠加一个状态位（线程安全） */
    virtual void addState(ZM_WS_STATE state);
    /** 移除一个状态位（线程安全） */
    virtual void removeState(ZM_WS_STATE state);
    /** 直接设置为目标状态（线程安全） */
    virtual void changeState(ZM_WS_STATE state);
    /** 检查 lhs 是否包含 rhs 状态位 */
    virtual bool hasState(ZM_WS_STATE lhs, ZM_WS_STATE rsh) const;
    /** 检查当前状态是否包含指定状态位 */
    virtual bool hasState(ZM_WS_STATE rsh) const;

    /**
     * @brief 控制事件分发回调（纯虚函数）
     *
     * m_ctrlEvent 未关联任何 fd，通过 event_active() 手动触发。
     * 派生类根据 what 参数中的 WS_CONTROL 位分发到对应处理函数。
     */
    virtual void onEventControl(evutil_socket_t fd, short what, void* ctx) = 0;

private:
    /**
     * @brief JSON 字符串转义
     * @param str 原始字符串
     * @param len 字符串长度
     * @return 转义后的字符串（", \, 控制字符等被转义）
     */
    std::string jsonEscapeString(const char* str, size_t len);

protected:
    struct event_base* m_evbase;                        // libevent 事件基，所有子事件在其上派生和调度
    struct event* m_ctrlEvent;                          // 控制事件，用于线程间通信（无 fd，手动触发）
    mutable std::atomic<ZM_WS_STATE> m_status;          // 当前运行状态（原子变量，支持并发读取）
    uint16_t m_port;                                    // 当前关联的端口号
    mutable std::recursive_mutex m_mutex;               // 递归互斥锁，保护状态和消息队列
    struct timeval m_readTimeout;                       // 对端读超时
    struct timeval m_writeTimeout;                      // 本端写超时
    std::vector<ZmMessageItem_t*> m_pendingMessages;    // 待发送消息队列（先进先出）
    size_t m_maxPendingMessages;                        // 消息队列最大容量（超出时丢弃最旧消息）
};
#endif
