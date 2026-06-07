#include "zm_net_tap_jrpc.h"
#include "../spdlog/zm_logger.h"

///////////////////////////////////////////////////////////////////////////////////////////////
// ZmTapDelegateJRPC
//
ZmTapDelegateJRPC::ZmTapDelegateJRPC()
    : ZmTapDelegate()
{
}

ZmTapDelegateJRPC::~ZmTapDelegateJRPC()
{
}

void ZmTapDelegateJRPC::SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb)
{
    m_tapDelegateJrpcRequestReadCB = cb;
}

/**
 * @brief 请求端数据到达回调，解析长度前缀帧格式的 JRPC 请求
 *
 * 协议格式：[4字节大端长度] [JSON 数据体]
 * 处理流程：
 *   1. 首次到达时从头部读取4字节获取消息总长度
 *   2. 分多次读取完整消息体（使用 evbuffer_copyout 避免线性化开销）
 *   3. 收齐完整消息后通过回调链传递给外部处理器
 *
 * @param tap       目标 TAP 上下文
 * @param app_input libevent 输入缓冲区
 * @param datalen   本次到达的数据长度
 */
void ZmTapDelegateJRPC::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
    if (tap->delegate->TapDelegateMode() == m_mode)
    {
        PUBLIC_LOG_INFO("Received JRPC message forwarded by HUB, TAP:{}, datalen: {}", (void*)tap, datalen);

        /** 当数据长度超过 libevent Socket 缓冲区大小时需自建数据缓冲区分次读取 */
        if (0 == tap->requester_data_len)
        {
            if (datalen >= 4)
            {
                // [4]len [*]JSON-Data
                const uint32_t* p_msg_len = (const uint32_t*)evbuffer_pullup(app_input, 4);
                if (!p_msg_len)
                {
                    PUBLIC_LOG_ERROR("Tap: {}, evbuffer_pullup failed in JRPC header", (void*)tap);
                    tap->Drop("JRPC header pullup failed");
                    return;
                }
                uint32_t msg_len = ntohl(*p_msg_len);
                if (msg_len > ZM_BUF_SIZE_4M)
                {
                    PUBLIC_LOG_ERROR("Tap: {}, JRPC request too large: {} bytes (max {})",
                        (void*)tap, msg_len, (size_t)ZM_BUF_SIZE_4M);
                    tap->Drop("JRPC request too large");
                    return;
                }
                ZmTapContext::SetOptData(tap, msg_len + 1);
                tap->requester_data_len = msg_len;       /** 期望接收的数据总长度 */
                tap->requester_received_len = 0;          /** 已接收的数据长度 */
                evbuffer_drain(app_input, 4);
                datalen -= 4;
            }
            else
            {
                return;
            }
        }

        /** 使用 evbuffer_copyout 避免 pullup 的内部链表线性化开销，只读取尚缺的长度 */
        size_t read_len = ZM_MIN(datalen, tap->requester_data_len - tap->requester_received_len);
        int copy_ret = evbuffer_copyout(app_input, tap->requester_data + tap->requester_received_len, read_len);
        if (copy_ret < 0 || (size_t)copy_ret != read_len)
        {
            PUBLIC_LOG_ERROR("Tap: {}, evbuffer_copyout failed in JRPC body, expected:{}, got:{}",
                (void*)tap, read_len, copy_ret);
            tap->Drop("JRPC body copyout failed");
            return;
        }
        tap->requester_received_len += (uint32_t)read_len;
        evbuffer_drain(app_input, read_len);
        /** 未收齐完整消息则等待后续数据 */
        if (tap->requester_received_len < tap->requester_data_len)
        {
            return;
        }

        PUBLIC_LOG_INFO("Received JRPC message forwarded by HUB, TAP:{}, content:{}",
            (void*)tap, (const char*)tap->requester_data);

        if (m_tapDelegateJrpcRequestReadCB)
        {
            ZmTapContext::BackChainPush(tap, this);
            m_tapDelegateJrpcRequestReadCB(tap, (const char*)tap->requester_data);
        }
        else
        {
            PUBLIC_LOG_INFO("Internal portal does not have JRPC processing channel set up, TAP:{}", (void*)tap);

            ZMJSON json_rsp;
            json_rsp["error"] = ZMJSON{
                {"code", 32000},
                {"message", "Internal portal does not have JRPC processing channel set up"}
            };
            std::string json_str = ZMJSON(json_rsp).dump();
            WriteResponse(tap, json_str.data(), json_str.size());
        }
    }
}

/**
 * @brief 回传数据到达回调，将外部 JRPC 响应写回客户端
 *
 * 外部处理器完成 JRPC 处理后，通过 BackChainPop 弹出此 delegate 并设置 onback_data，
 * 触发本回调将响应数据通过长度前缀帧格式写回客户端连接。
 *
 * @param tap 目标 TAP 上下文
 */
void ZmTapDelegateJRPC::OnTapDelegateBackEvent(ZM_TAP_CTX* tap)
{
    PUBLIC_LOG_INFO("Received reply message from JRPC external callback, TAP:{}, onback_data={}",
        (void*)tap, (const char*)tap->onback_data);

    tap->delegate = this;

    WriteResponse(tap, (const char*)tap->onback_data, tap->onback_dlen);
}

/**
 * @brief 向客户端写入 JSON-RPC 响应
 *
 * 使用 iovec 将4字节大端长度头与 JSON 体一次性提交到 bufferevent 输出缓冲区，
 * 减少锁获取和内部链表操作次数。写入后设置30秒超时定时器。
 *
 * @param tap       目标 TAP 上下文
 * @param json_str  响应 JSON 字符串
 * @param data_len  JSON 字符串长度
 */
void ZmTapDelegateJRPC::WriteResponse(ZM_TAP_CTX* tap, const char* json_str, size_t data_len)
{
    PUBLIC_LOG_INFO("Received JRPC Response, TAP:{}, content: {}", (void*)tap, json_str);

    uint32_t rsp_len = htonl((uint32_t)data_len);

    /** 使用 iovec 将长度头与 JSON 体单次提交，减少锁获取和内部链表操作次数 */
    struct evbuffer_iovec iov[2];
    iov[0].iov_base = &rsp_len;
    iov[0].iov_len = 4;
    iov[1].iov_base = (void*)json_str;
    iov[1].iov_len = data_len;

    int ret = evbuffer_add_iovec(bufferevent_get_output(tap->requester_bev), iov, 2);
    if (ret != 0)
    {
        PUBLIC_LOG_ERROR("evbuffer_add_iovec failed, TAP:{}, ret:{}", (void*)tap, ret);
    }
    ret = bufferevent_flush(tap->requester_bev, EV_WRITE, BEV_FLUSH);
    if (ret != 1)
    {
        PUBLIC_LOG_ERROR("bufferevent_flush success, TAP:{}, ret:{}", (void*)tap, ret);
    }
    ZmTapContext::SetDropTimer(tap, 30);
}
