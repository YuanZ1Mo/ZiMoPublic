/**
 * @file zm_websocket_utils.cpp
 * @brief WebSocket 通信基类实现
 *
 * 实现状态管理、消息格式化、消息队列管理等基础功能。
 */

#include "zm_websocket_utils.h"
#include <event2/event.h>

ZmMessageWSBase::ZmMessageWSBase():
    m_evbase(nullptr),
    m_ctrlEvent(nullptr),
    m_status(ZM_WS_STATE_STOPPED),
    m_port(0),
    m_writeTimeout({ 0 }),
    m_readTimeout({ 0 }),
    m_maxPendingMessages(ZM_MAX_MESSAGE_LIST_SIZE)
{
    m_pendingMessages.clear();
}

/**
 * @brief 设置消息队列最大缓存数
 * @note 基类版本无锁，派生类 ZmMessageWSServer 已 override 并加锁
 */
void ZmMessageWSBase::SetMaxMessagesCache(size_t num)
{
    m_maxPendingMessages = num;
}

/**
 * @brief 设置读写超时（毫秒转 timeval）
 *
 * 将毫秒值转换为 struct timeval（秒 + 微秒），
 * 供后续设置到 bufferevent / evhttp 的超时参数。
 */
void ZmMessageWSBase::SetReadAndWriteTimeout(long read_msec, long write_msec)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    m_readTimeout.tv_sec = read_msec / 1000;
    m_readTimeout.tv_usec = (read_msec % 1000) * 1000;

    m_writeTimeout.tv_sec = write_msec / 1000;
    m_writeTimeout.tv_usec = (write_msec % 1000) * 1000;
}

/**
 * @brief 叠加状态位
 *
 * 若当前状态不包含目标状态位，则通过 | 运算叠加。
 * 使用 recursive_mutex 保护，与 changeState / removeState 互斥。
 */
void ZmMessageWSBase::addState(ZM_WS_STATE state)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    if (!hasState(state))
    {
        m_status = m_status | state;
    }
}

/**
 * @brief 移除状态位
 *
 * 通过 & ~ 运算清除指定状态位。
 */
void ZmMessageWSBase::removeState(ZM_WS_STATE state)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_status = m_status & ~state;
}

/**
 * @brief 检查 lhs 是否包含 rhs 状态位
 * @return (lhs & rhs) == rhs
 */
bool ZmMessageWSBase::hasState(ZM_WS_STATE lhs, ZM_WS_STATE rsh) const
{
    return (lhs & rsh) == rsh;
}

/**
 * @brief 检查当前状态是否包含指定状态位
 */
bool ZmMessageWSBase::hasState(ZM_WS_STATE rsh) const
{
    return (m_status & rsh) == rsh;
}

/**
 * @brief 直接设置为目标状态（覆盖所有位）
 */
void ZmMessageWSBase::changeState(ZM_WS_STATE state)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (m_status != state)
    {
        m_status = state;
    }
}

ZM_WS_STATE ZmMessageWSBase::GetCurrentState() const
{
    return m_status;
}

/**
 * @brief JSON 字符串转义
 *
 * 按照 JSON 规范（RFC 8259 Section 7）对字符串中的特殊字符进行转义：
 * - " → \"    \ → \\    \b → \\b    \f → \\f
 * - \n → \\n  \r → \\r  \t → \\t
 * - 其他 < 0x20 的控制字符 → \\uXXXX
 * - 其余字符原样保留
 *
 * @param str 原始字符串
 * @param len 字符串长度
 * @return 转义后的字符串
 */
std::string ZmMessageWSBase::jsonEscapeString(const char* str, size_t len)
{
    std::string escaped;
    escaped.reserve(len);
    for (size_t i = 0; i < len; ++i)
    {
        char c = str[i];
        switch (c)
        {
        case '"':  escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b";  break;
        case '\f': escaped += "\\f";  break;
        case '\n': escaped += "\\n";  break;
        case '\r': escaped += "\\r";  break;
        case '\t': escaped += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                escaped += buf;
            }
            else
            {
                escaped += c;
            }
            break;
        }
    }
    return escaped;
}

/**
 * @brief 发送 topic + json_content（std::string 版本）
 *
 * 委托给 C 风格参数版本。
 */
void ZmMessageWSBase::PostNotificationWithTopic(std::string topic, std::string json_content)
{
    PostNotificationWithTopic(topic.c_str(), topic.length(), json_content.c_str(), json_content.length());
}

/**
 * @brief 发送 topic + json_content（C 风格参数版本）
 *
 * 将 topic 经过 JSON 转义后，拼接为标准 JSON 格式：
 *   { "TOPIC" : "<escaped_topic>", "CONTENT" : <json_content> }
 *
 * topic 作为 JSON 字符串（被引号包裹）需要转义；
 * json_content 作为 JSON value（对象/数组/字符串/数字等）原样拼入，
 * 调用方负责提供合法的 JSON 片段。
 */
void ZmMessageWSBase::PostNotificationWithTopic(const char* topic, size_t topic_len,
    const char* json_content, size_t content_len)
{
    if (topic == nullptr || json_content == nullptr)
    {
        return;
    }

    std::string escaped_topic = jsonEscapeString(topic, topic_len);
    std::string data = "{ \"TOPIC\" : \"" + escaped_topic +
                       "\", \"CONTENT\" : " +
                       std::string(json_content, content_len) + " }";

    PostNotificationWithData(data.data(), data.size());
}

/**
 * @brief 将原始数据加入待发送消息队列
 *
 * 流程：
 * 1. 分配 ZmMessageItem_t 并拷贝数据到堆
 * 2. 如果队列超过 m_maxPendingMessages，从队头开始丢弃最旧的消息
 * 3. 新消息入队
 * 4. 如果处于 RUNNING | CONNECTED 状态，触发 ZM_WS_CONTROL_SEND 事件
 *    通知事件循环中的 onControlSend 处理发送
 *
 * @note 消息的发送不在本线程完成，而是通过 event_active 委托给事件循环线程。
 *       这保证了所有对 m_pendingMessages 的操作都在事件循环线程中串行执行。
 */
void ZmMessageWSBase::PostNotificationWithData(const void* bytes, size_t blen)
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (bytes == nullptr || blen == 0)
    {
        return;
    }

    ZmMessageItem_t* message = (ZmMessageItem_t*)calloc(1, sizeof(*message));
    if (message == nullptr)
    {
        return;
    }
    message->data = malloc(blen);
    if (message->data == nullptr)
    {
        free(message);
        return;
    }
    memcpy(message->data, bytes, blen);
    message->len = blen;

    /* 队列超限时，从队头丢弃最旧的消息 */
    if (m_pendingMessages.size() > m_maxPendingMessages)
    {
        size_t beyondNum = m_pendingMessages.size() - m_maxPendingMessages;

        for (size_t i = 0; i < beyondNum; ++i)
        {
            auto it = m_pendingMessages.begin();
            if (it == m_pendingMessages.end())
            {
                break;
            }

            if ((*it)->data)
            {
                free((*it)->data);
            }

            free((*it));

            m_pendingMessages.erase(it);
        }
    }

    m_pendingMessages.push_back(message);

    /* 通知事件循环线程处理发送 */
    if (hasState(ZM_WS_STATE_RUNNING | ZM_WS_STATE_CONNECTED))
    {
        event_active(m_ctrlEvent, ZM_WS_CONTROL_SEND, 0);
    }
}

/**
 * @brief 清空所有缓存的消息并释放内存
 *
 * 遍历 m_pendingMessages，逐一释放每条消息的 data 和自身。
 * 在 Stop / Close / 析构时调用。
 */
void ZmMessageWSBase::ClearAllCachedMessages()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    if (!m_pendingMessages.empty())
    {
        for (auto it = m_pendingMessages.begin(); it != m_pendingMessages.end(); )
        {
            ZmMessageItem_t* message = *it;
            if (message)
            {
                if (message->data)
                {
                    free(message->data);
                    message->data = nullptr;
                }
                free(message);
            }
            it = m_pendingMessages.erase(it);
        }
    }
}
