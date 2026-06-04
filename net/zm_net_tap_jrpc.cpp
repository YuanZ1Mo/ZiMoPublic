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

void ZmTapDelegateJRPC::WriteResponse(ZM_TAP_CTX* tap, const char* jstr, size_t dlen)
{
    //SP_LOGT("%s[%p] jsonrpc %.1024s...", __SP_FUNC__, tap, jstr.c_str());


    uint32_t rsp_len = htonl((uint32_t)dlen);
    int ret = 0;
    ret = evbuffer_add(bufferevent_get_output(tap->requester_bev), &rsp_len, 4);
    if(ret != 0)
    {
        //SP_LOGT("%s[%p] evbuffer_add len failed %d", __SP_FUNC__, tap, ret);
    }
    ret = evbuffer_add(bufferevent_get_output(tap->requester_bev), jstr, dlen);
    if(ret != 0)
    {
        //SP_LOGT("%s[%p] evbuffer_add json failed %d", __SP_FUNC__, tap, ret);
    }
    ret = bufferevent_flush(tap->requester_bev, EV_WRITE, BEV_FLUSH);
    if(ret != 1)
    {
        //SP_LOGT("%s[%p] bufferevent_flush return %d", __SP_FUNC__, tap, ret);
    }
    ZmTapContext::SetDropTimer(tap, 30);
}

void ZmTapDelegateJRPC::OnTapDelegateBackEvent(ZM_TAP_CTX* tap)
{
    //SP_OPS_LOGT("%s[%p] errcode=%d, onback_eventid=%d, onback_data=%.1024s...", __SP_FUNC__,
    //    tap, errcode, tap->onback_eventid, jstr.c_str());

    tap->delegate = this;

    WriteResponse(tap, (const char*)tap->onback_data, tap->onback_dlen);
}

void ZmTapDelegateJRPC::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
    if (tap->delegate->TapDelegateMode() == _mode)
    {
        // SP_DEV_LOGT("%s[jrpc][%p] datalen=%ld", __SP_FUNC__, tap, (long)datalen);
        /** 当数据长度超过 LibEvnet Socket 缓冲区大小时，无法接收完整数据，需要自建数据缓冲区 */
        if (0 == tap->requester_data_len)
        {
            /** 当数据长度超过 LibEvnet Socket 缓冲区大小时，无法接收完整数据，需要自建数据缓冲区 */
            if (datalen > 4)
            {
                // [4]len [*]JSON-Data
                const uint32_t* plen = (const uint32_t*)evbuffer_pullup(app_input, 4);
                if (!plen)
                {
                    PUBLIC_LOG_ERROR("Tap: {}, evbuffer_pullup failed in JRPC header", (void*)tap);
                    tap->tap_context->Drop(tap, "JRPC header pullup failed");
                    return;
                }
                uint32_t mlen = ntohl(*plen);
                ZmTapContext::SetOptData(tap, mlen + 1);
                tap->requester_data_len = mlen; /** 需要的数据长度 */
                tap->requester_content_len = 0;    /** 已经读取到的数据长度 */
                evbuffer_drain(app_input, 4);
                datalen -= 4;
            }
            else
            {
                return;
            }
        }

        /** 只读取需要的长度，防止越界 */
        size_t rlen = ZM_MIN(datalen, tap->requester_data_len - tap->requester_content_len);
        const void* pdata = evbuffer_pullup(app_input, rlen);
        if (!pdata)
        {
            PUBLIC_LOG_ERROR("Tap: {}, evbuffer_pullup failed in JRPC body", (void*)tap);
            tap->tap_context->Drop(tap, "JRPC body pullup failed");
            return;
        }
        memcpy(tap->requester_data + tap->requester_content_len, pdata, rlen);
        tap->requester_content_len += (uint32_t)rlen;
        evbuffer_drain(app_input, rlen);
        if (tap->requester_content_len < tap->requester_data_len)
        {
            return;
        }

        if (m_tapDelegateJrpcRequestReadCB)
        {
            ZmTapContext::BackChainPush(tap, this);
            m_tapDelegateJrpcRequestReadCB(tap, (const char*)tap->requester_data);
        }
        else
        {
            ZMJSON jrsp;
            jrsp["error"] = ZMJSON{ {"code", 32000}, {"message", "Internal portal does not have JRPC processing channel set up"}};
            std::string  jstr = ZMJSON(jrsp).dump();
            WriteResponse(tap, jstr.data(), jstr.size());
        }

        //std::string jerr;
        //ZMJSON json = zm_json_parse((const char*)tap->requester_data, jerr);
        //if (jerr.empty())
        //{
        //    ZMJSON jrsp;
        //    jrsp["error"] = ZMJSON{ {"code", 0} };
        //    std::string  jstr = ZMJSON(jrsp).dump();
        //    WriteResponse(tap, jstr.data(), jstr.size());
        //}
        //else
        //{
        //    //SP_LOGT("[jrpc][%p] received error format json data: %s", tap, jerr.c_str());
        //    tap->tap_context->Drop(tap);
        //}
    }
}

void ZmTapDelegateJRPC::SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb)
{
    m_tapDelegateJrpcRequestReadCB = cb;
}