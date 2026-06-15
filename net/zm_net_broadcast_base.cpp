/**
 * @file zm_net_broadcast_base.cpp
 * @brief 广播模块公共实现 — 帧协议编解码与工具函数
 */

#include "zm_net_broadcast_base.h"

#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include <random>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <ctime>
#include <cstdio>

// ============================================================================
// 帧协议实现
// ============================================================================

std::string BcFrameDecode(struct evbuffer* input)
{
    // 需要至少 4 字节长度头
    size_t avail = evbuffer_get_length(input);
    if (avail < 4)
        return std::string();

    // peek 4 字节大端长度，不消耗数据
    uint8_t lenBuf[4];
    evbuffer_copyout(input, lenBuf, 4);
    uint32_t bodyLen = ((uint32_t)lenBuf[0] << 24)
                     | ((uint32_t)lenBuf[1] << 16)
                     | ((uint32_t)lenBuf[2] << 8)
                     | ((uint32_t)lenBuf[3]);

    // body 数据不足则等待
    if (avail < 4 + (size_t)bodyLen)
        return std::string();

    // 消耗 4 字节长度头
    evbuffer_drain(input, 4);

    // 读取 body
    std::string body(bodyLen, '\0');
    evbuffer_remove(input, &body[0], bodyLen);

    return body;
}

bool BcFrameEncode(struct bufferevent* bev, const std::string& json)
{
    if (!bev || json.empty())
        return false;

    uint32_t bodyLen = (uint32_t)json.size();

    // 构造 4 字节大端长度前缀
    uint8_t lenBuf[4];
    lenBuf[0] = (bodyLen >> 24) & 0xFF;
    lenBuf[1] = (bodyLen >> 16) & 0xFF;
    lenBuf[2] = (bodyLen >> 8)  & 0xFF;
    lenBuf[3] =  bodyLen        & 0xFF;

    // 写入长度前缀
    if (bufferevent_write(bev, lenBuf, 4) != 0)
        return false;

    // 写入 body
    if (bufferevent_write(bev, json.data(), json.size()) != 0)
        return false;

    return true;
}

// ============================================================================
// 工具函数实现
// ============================================================================

std::string BcGenerateUUID()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);

    uint32_t a = dis(gen);
    uint32_t b = dis(gen);
    uint32_t c = dis(gen);
    uint32_t d = dis(gen);

    // UUID v4: 设置 variant 和 version 位
    // b 的第 19-16 位 = 0x4 (version 4)
    b = (b & 0xFFFF0FFF) | 0x00004000;
    // c 的第 31-30 位 = 0b10 (variant 1)
    c = (c & 0x3FFFFFFF) | 0x80000000;

    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%04x%08x",
        a, b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d);
    return std::string(buf);
}

std::string BcNowTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_s(&tm, &tt);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

uint64_t BcNowMillis()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch());
    return (uint64_t)ms.count();
}
