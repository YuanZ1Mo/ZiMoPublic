#include "zm_net_tap_jrpc.h"
#include "../spdlog/zm_logger.h"


///////////////////////////////////////////////////////////////////////////////////////////////
//
// SPTapJRPC
//
ZmTapDelegateJRPC::ZmTapDelegateJRPC() : ZmTapDelegate()
{}

ZmTapDelegateJRPC::~ZmTapDelegateJRPC()
{}

void ZmTapDelegateJRPC::WriteResponse(ZM_TAP_CTX* tap, const char* json_str, size_t data_len)
{
    PUBLIC_LOG_INFO("Received JRPC Response, TAP:{}, content: {}", (void*)tap, json_str);

    uint32_t rsp_len = htonl((uint32_t)data_len);
    int ret = 0;
    ret = evbuffer_add(bufferevent_get_output(tap->requester_bev), &rsp_len, 4);
    if(ret != 0)
    {
        PUBLIC_LOG_ERROR("evbuffer_add len failed, TAP:{}, ret:{}", (void*)tap, ret);
    }
    ret = evbuffer_add(bufferevent_get_output(tap->requester_bev), json_str, data_len);
    if(ret != 0)
    {
        PUBLIC_LOG_ERROR("evbuffer_add json failed, TAP:{}, ret:{}", (void*)tap, ret);
    }
    ret = bufferevent_flush(tap->requester_bev, EV_WRITE, BEV_FLUSH);
    if(ret != 1)
    {
        PUBLIC_LOG_ERROR("bufferevent_flush failed, TAP:{}, ret:{}", (void*)tap, ret);
    }
    ZmTapContext::SetDropTimer(tap, 30);
}

void ZmTapDelegateJRPC::OnTapDelegateBackEvent(ZM_TAP_CTX* tap)
{
    PUBLIC_LOG_INFO("Received reply message from JRPC external callback, TAP:{}, onback_data={}", (void*)tap, (const char*)tap->onback_data);

    tap->delegate = this;

    WriteResponse(tap, (const char*)tap->onback_data, tap->onback_dlen);
}

void ZmTapDelegateJRPC::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
    if (tap->delegate->TapDelegateMode() == m_mode)
    {
        PUBLIC_LOG_INFO("Received JRPC message forwarded by HUB, TAP:{}, datalen: {}", (void*)tap, datalen);
        /** 当数据长度超过 LibEvnet Socket 缓冲区大小时，无法接收完整数据，需要自建数据缓冲区 */
        if (0 == tap->requester_data_len)
        {
            /** 当数据长度超过 LibEvnet Socket 缓冲区大小时，无法接收完整数据，需要自建数据缓冲区 */
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
                    PUBLIC_LOG_ERROR("Tap: {}, JRPC request too large: {} bytes (max {})", (void*)tap, msg_len, (size_t)ZM_BUF_SIZE_4M);
                    tap->Drop("JRPC request too large");
                    return;
                }
                ZmTapContext::SetOptData(tap, msg_len + 1);
                tap->requester_data_len = msg_len; /** 需要的数据长度 */
                tap->requester_received_len = 0;    /** 已经读取到的数据长度 */
                evbuffer_drain(app_input, 4);
                datalen -= 4;
            }
            else
            {
                return;
            }
        }

        /** 只读取需要的长度，防止越界；使用 evbuffer_copyout 避免 pullup 的内部链表线性化开销 */
        size_t read_len = ZM_MIN(datalen, tap->requester_data_len - tap->requester_received_len);
        int copy_ret = evbuffer_copyout(app_input, tap->requester_data + tap->requester_received_len, read_len);
        if (copy_ret < 0 || (size_t)copy_ret != read_len)
        {
            PUBLIC_LOG_ERROR("Tap: {}, evbuffer_copyout failed in JRPC body, expected:{}, got:{}", (void*)tap, read_len, copy_ret);
            tap->Drop("JRPC body copyout failed");
            return;
        }
        tap->requester_received_len += (uint32_t)read_len;
        evbuffer_drain(app_input, read_len);
        if (tap->requester_received_len < tap->requester_data_len)
        {
            return;
        }

        PUBLIC_LOG_INFO("Received JRPC message forwarded by HUB, TAP:{}, content:{}", (void*)tap, (const char*)tap->requester_data);

        if (m_tapDelegateJrpcRequestReadCB)
        {
            ZmTapContext::BackChainPush(tap, this);
            m_tapDelegateJrpcRequestReadCB(tap, (const char*)tap->requester_data);
        }
        else
        {
            PUBLIC_LOG_INFO("Internal portal does not have JRPC processing channel set up, TAP:{}", (void*)tap);

            ZMJSON json_rsp;
            json_rsp["error"] = ZMJSON{ {"code", 32000}, {"message", "Internal portal does not have JRPC processing channel set up"}};
            std::string  json_str = ZMJSON(json_rsp).dump();
            WriteResponse(tap, json_str.data(), json_str.size());
        }
    }
}

void ZmTapDelegateJRPC::SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb)
{
    m_tapDelegateJrpcRequestReadCB = cb;
}