/**
 * @file zm_net_broadcast_base.h
 * @brief 广播模块公共定义
 *
 * 提供 TCP 广播服务端的公共类型定义：
 *   - 服务端状态枚举
 *   - 消息结构体
 *   - 客户端信息结构体
 *   - 帧协议编解码函数声明
 */

#ifndef ZM_NET_BROADCAST_BASE_H
#define ZM_NET_BROADCAST_BASE_H

#include <cstdint>
#include <string>
#include <vector>

struct bufferevent;
struct evbuffer;

// ============================================================================
// 状态枚举
// ============================================================================

/**
 * @brief 广播服务端状态枚举
 *
 * 状态流转: IDLE → STARTING → LISTENING → STOPPING → STOPPED
 * 任意阶段可进入 ERROR
 */
enum ZM_BROADCAST_STATE
{
    ZM_BC_STATE_IDLE      = 0,  ///< 未启动
    ZM_BC_STATE_STARTING  = 1,  ///< 启动中（正在绑定监听）
    ZM_BC_STATE_LISTENING = 2,  ///< 已监听，可接受连接
    ZM_BC_STATE_STOPPING  = 3,  ///< 停止中（正在关闭连接、清理 pending）
    ZM_BC_STATE_STOPPED   = 4,  ///< 已停止
    ZM_BC_STATE_ERROR     = 5,  ///< 错误
};

// ============================================================================
// 消息结构
// ============================================================================

/**
 * @brief 广播消息结构体
 *
 * tag 仅用于服务端过滤匹配，不进入线上 JSON。
 * 线上 JSON 格式: {"id":"...","timestamp":"...","topic":"...","content":...}
 */
struct BcMessage
{
    std::string id;         ///< 消息唯一ID（UUID v4）
    std::string topic;      ///< 主题
    std::string tag;        ///< 过滤标签（仅用于匹配，不序列化到线上）
    std::string content;    ///< 内容（JSON 任意类型字符串）
    std::string timestamp;  ///< 发送时 ISO-8601 时间戳
};

// ============================================================================
// 客户端信息
// ============================================================================

/**
 * @brief 客户端信息快照，用于查询和回调
 */
struct BcClientInfo
{
    std::string clientId;              ///< 服务端分配的唯一 ID
    std::string ip;                    ///< 对端 IP 地址
    uint16_t    port;                  ///< 对端端口号
    uint64_t    connectTime;           ///< 连接建立时间戳（毫秒）
    std::vector<std::string> tags;     ///< 当前订阅的 tag 列表
    size_t      queuePending;          ///< 队列中待发送消息数
    uint64_t    lastActiveTime;        ///< 最后活跃时间戳（毫秒）
    uint64_t    sentCount;             ///< 已发送消息数
};

// ============================================================================
// 帧协议
// ============================================================================

/**
 * @brief 从 evbuffer 中解码一帧数据
 *
 * 帧格式: 4 字节大端长度前缀 + JSON body。
 * 先 peek 4 字节解出长度，若 evbuffer 数据不足则返回空字符串等待更多数据。
 *
 * @param input  bufferevent 的输入 evbuffer
 * @return       解码后的 JSON 字符串，数据不足时返回空字符串
 */
std::string BcFrameDecode(struct evbuffer* input);

/**
 * @brief 将 JSON 字符串编码为帧并写入 bufferevent 输出缓冲区
 *
 * @param bev   目标 bufferevent
 * @param json  待发送的 JSON 字符串
 * @return      true 成功，false 失败
 */
bool BcFrameEncode(struct bufferevent* bev, const std::string& json);

// ============================================================================
// 工具函数
// ============================================================================

/**
 * @brief 生成 UUID v4 格式的唯一 ID 字符串
 * @return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" 格式字符串
 */
std::string BcGenerateUUID();

/**
 * @brief 获取当前 UTC 时间的 ISO-8601 格式字符串
 * @return "2026-06-15T12:00:00Z" 格式字符串
 */
std::string BcNowTimestamp();

/**
 * @brief 获取当前毫秒级 Unix 时间戳
 * @return 毫秒时间戳
 */
uint64_t BcNowMillis();

#endif // ZM_NET_BROADCAST_BASE_H
