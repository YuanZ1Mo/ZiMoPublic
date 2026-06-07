#ifndef ZM_NET_TAP_JRPC_H
#define ZM_NET_TAP_JRPC_H

#include <vector>
#include <string>
#include <unordered_map>

#include "zm_net_tap.h"

/** @brief JRPC 请求读取回调函数类型：TAP 上下文 + 请求数据 */
using TapDelegateJrpcRequestReadCB = std::function<void(ZM_TAP_CTX* tap, const char* reqData)>;

/**
 * @brief JRPC 协议委托，处理经过 Hub 转发的 JSON-RPC 请求/响应
 *
 * 继承 ZmTapDelegate，实现基于长度前缀帧（4字节大端长度 + JSON 体）的 JRPC 协议解析。
 * 工作于 ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC 模式，由 Hub 代理将连接请求转发至此处理。
 */
class ZmTapDelegateJRPC : public ZmTapDelegate
{
public:
    ZmTapDelegateJRPC();
    virtual ~ZmTapDelegateJRPC();

    /** @brief 设置 JRPC 请求到达时的回调函数
     *  @param cb 回调函数，参数为 TAP 上下文和请求 JSON 字符串 */
    void SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb);

    // ZmTapDelegate 接口实现
    /** @brief 请求端接受连接（JRPC 模式不接受直连，始终返回 false） */
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd,
        struct sockaddr* address) override { return false; }
    /** @brief delegate 内部事件回调（JRPC 模式下无内部事件，空实现） */
    virtual void OnTapDelegateEvent(short what) override {}
    /** @brief 请求端数据到达回调，解析长度前缀帧格式的 JRPC 请求 */
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) override;
    /** @brief 回传数据到达回调，将外部 JRPC 响应写回客户端 */
    virtual void OnTapDelegateBackEvent(ZM_TAP_CTX* tap) override;

private:
    /** @brief 向客户端写入 JSON-RPC 响应（4字节大端长度前缀 + JSON 体）
     *  @param tap       目标 TAP 上下文
     *  @param json_str  响应 JSON 字符串指针
     *  @param data_len  JSON 字符串长度 */
    void WriteResponse(ZM_TAP_CTX* tap, const char* json_str, size_t data_len);

    /** @brief JRPC 请求到达时的外部回调 */
    TapDelegateJrpcRequestReadCB m_tapDelegateJrpcRequestReadCB;
};

#endif  // ZM_NET_TAP_JRPC_H
