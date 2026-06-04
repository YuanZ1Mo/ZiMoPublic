#include "zm_net_tap.h"

#include "../util/zm_util_libevent.h"
#include "zm_net_dns.h"
#include "../spdlog/zm_logger.h"
#include "zm_net_tap_hub.h"


#define ZM_EVENT_BEV_OPTIONS        BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS|BEV_OPT_THREADSAFE




ZmTapContext::ZmTapContext()
{
    _capacity = 0x400;
    _count = 0;
    _taps = (ZM_TAP_CTX**)malloc(TAP_ITEM_SIZE * _capacity);
    memset(_taps, 0, TAP_ITEM_SIZE * _capacity);
    _seq_counter.store(0, std::memory_order_relaxed);
}

ZmTapContext::~ZmTapContext()
{
    Clear();
}

void ZmTapContext::Clear()
{
    PUBLIC_LOG_INFO("TapContext Clear");

    for (size_t i = 0; i < _count; i++)
    {
        ZM_TAP_CTX* item = (ZM_TAP_CTX*)_taps[i];
        Drop(item, "By Clear");
        FreeTap(item);
    }
    memset(_taps, 0, TAP_ITEM_SIZE * _capacity);
    _count = 0;
    _free_stack.clear();
}

void ZmTapContext::Drop(ZM_TAP_CTX* tap, const char* reason)
{
    /** 通过判断 tap->mode 可以有效防止重复释放 */
    if (tap && ZM_TAP_STATE_DROPPING != tap->state)
    {
        tap->state = ZM_TAP_STATE_DROPPING;

        PUBLIC_LOG_INFO("Dropping Tap: {}, Reason:{}", (void*)tap, reason);

        if (tap->delegate)
        {
            tap->delegate->OnTapDrop(tap);
        }

        CancelResolve(tap);

        FreeRequesterEnd(tap);

        if (tap->ev_timeout)
        {
            event_free(tap->ev_timeout);
            tap->ev_timeout = nullptr;
        }

        ZmHttpUtil::FreeRequest(tap->request);

        if (tap->requester_data)
        {
            free(tap->requester_data);
            tap->requester_data = nullptr;
        }

        if (tap->onback_data)
        {
            free(tap->onback_data);
            tap->onback_data = nullptr;
        }

        tap->Clear();

        // 回收到空闲栈，供 Get() 快速复用（O(1)）
        _free_stack.push_back(tap);

        PUBLIC_LOG_INFO("Drop Tap: {} Over", (void*)tap);
    }
}

void ZmTapContext::FreeRequesterEnd(ZM_TAP_CTX* tap)
{
    zm_util_bufferevent_free(tap->requester_bev);
    tap->requester_bev = nullptr;
}

ZM_TAP_CTX* ZmTapContext::Get()
{
    std::lock_guard<std::mutex> lock(_mutex);
    ZM_TAP_CTX* tap = nullptr;

    // O(1) 从空闲栈取
    if (!_free_stack.empty())
    {
        tap = _free_stack.back();
        _free_stack.pop_back();
        tap->Clear();
    }

    if (!tap)
    {
        PUBLIC_LOG_INFO("There are no available tap containers, try creating a new tap container");

        /**
        * //TODO
         * 这里存在隐患，扩容时，如果外部还存在指针，则扩容后，这些指针会变成野指针
         * 目前的处理方式：禁止外部持久拥有 _taps 指针
         * TODO: 正确的处理方式为
         *          typedef struct { ZM_TAP_CTX* tap; } SP_TAP_CTX_ITEM;
         *       容器中存放 SP_TAP_CTX_ITEM
         *       如何从 ZM_TAP_CTX 推导 SP_TAP_CTX_ITEM ??
         *          在 ZM_TAP_CTX 中新增成员 up_ptr，每次扩容时需更新此 up_ptr
         */
         // Expand
        if (_count >= _capacity)
        {
            PUBLIC_LOG_INFO("Tap container pool size is insufficient, try expanding it, Current size: {}", _count);

            ZM_TAP_CTX** bak_taps = _taps;
            size_t       bak_capacity = _capacity;

            _capacity += _capacity / 2;
            _taps = (ZM_TAP_CTX**)malloc(TAP_ITEM_SIZE * _capacity);
            memset(_taps, 0, TAP_ITEM_SIZE * _capacity);
            memcpy(_taps, bak_taps, TAP_ITEM_SIZE * bak_capacity);
            free(bak_taps);

            PUBLIC_LOG_INFO("The total size of the tap pool after expansion: {}", _capacity);
        }

        tap = CreateTap();
        _taps[_count++] = (ZM_TAP_CTX*)tap;
    }

    tap->tap_context = this;

    // 原子自增生成唯一 seq_num，O(1) 且保证不重复
    uint64_t sn = _seq_counter.fetch_add(1, std::memory_order_relaxed);
    snprintf(tap->seq_num, sizeof(tap->seq_num), "%llu", (unsigned long long)sn);

    return tap;
}

void ZmTapContext::SetDropTimer(ZM_TAP_CTX* tap, int seconds, int micros, uint32_t drop_timeout_error_code)
{
    PUBLIC_LOG_INFO("SetDropTimer, Tap: {}, time: {}s+{}ms, drop timeout errorCode: {}", (void*)tap, seconds, micros, drop_timeout_error_code);

    if (seconds >= 0 || micros >= 0)
    {
        tap->drop_timeout_error_code = drop_timeout_error_code;

        if (!tap->ev_timeout)
        {
            tap->ev_timeout = evtimer_new(tap->delegate->TapDelegateEventBase(), ZmTapContextEventHandler::OnDropTimerCB, tap);
        }

        timeval tv = { seconds, micros * 1000 };
        event_add(tap->ev_timeout, &tv);
    }
    else if (seconds < 0 || micros < 0)
    {
        if (tap->ev_timeout)
        {
            evtimer_del(tap->ev_timeout);
        }
    }
}

void ZmTapContext::SetOptData(ZM_TAP_CTX* tap, size_t optlen, const BYTE* optdata)
{
    if (tap->requester_data)
    {
        free(tap->requester_data);
    }
    tap->requester_data = nullptr;
    tap->requester_data_len = 0;
    if (optlen > 0)
    {
        tap->requester_data_len = (uint32_t)optlen;
        tap->requester_data = (BYTE*)malloc(optlen + 1);
        memset(tap->requester_data, 0, optlen + 1);
        if (optdata)
        {
            memcpy(tap->requester_data, optdata, optlen);
        }
    }
}

void ZmTapContext::SetOnBackData(ZM_TAP_CTX* tap, size_t dlen, const void* data)
{
    if (tap->onback_data)
    {
        free(tap->onback_data);
    }
    tap->onback_data = nullptr;
    tap->onback_dlen = 0;

    if (dlen > 0)
    {
        tap->onback_dlen = (uint32_t)dlen;
        tap->onback_data = (BYTE*)malloc(dlen + 1);
        memset(tap->onback_data, 0, dlen + 1);
        if (data)
        {
            memcpy(tap->onback_data, data, dlen);
        }
    }
}


void ZmTapContext::RequestCreate(ZM_TAP_CTX* tap)
{
    if (tap->request)
    {
        ZmHttpUtil::FreeRequest(tap->request);
    }
    tap->request = ZmHttpUtil::CreateRequest();
}

void ZmTapContext::RequestSetAddress(ZM_TAP_CTX* tap, const char* dst_host, uint16_t dst_port)
{
    if (nullptr == tap->request)
    {
        tap->request = ZmHttpUtil::CreateRequest();
    }
    else if (tap->request->host)
    {
        free(tap->request->host);
    }
    tap->request->host = _strdup(dst_host);
    tap->request->port = dst_port;
}

void ZmTapContext::EvDnsResolve(ZM_TAP_CTX* tap, const char* hostname, uint16_t port)
{
    PUBLIC_LOG_INFO("Tap: {}, EvDnsResolving HostName={}, port={}", (void*)tap, hostname, port);

    if (!tap->delegate->TapDelegateEvdnsBase())
    {
        PUBLIC_LOG_ERROR("EvDnsResolve failed: evdns_base is null");
        return;
    }

    CancelResolve(tap);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct evutil_addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = EVUTIL_AI_CANONNAME;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    tap->dns_request = evdns_getaddrinfo(tap->delegate->TapDelegateEvdnsBase(), hostname, port_str, &hints,
        ZmTapContextEventHandler::OnDnsResolvedCB, tap);

    if (!tap->dns_request)
    {
        // evdns_getaddrinfo 返回 NULL 表示立即完成了（命中缓存/hosts/或出错）
        // 此时回调已在 evdns_getaddrinfo 内部被同步调用
        PUBLIC_LOG_INFO("Tap: {}, EvDnsResolve completed immediately (cached or error)", (void*)tap);
    }
}

void ZmTapContext::CancelResolve(ZM_TAP_CTX* tap)
{
    PUBLIC_LOG_INFO("Tap: {}, CancelResolve", (void*)tap);

    evdns_getaddrinfo_cancel(tap->dns_request);
    tap->dns_request = nullptr;
}

size_t ZmTapContext::RequesterInputLen(ZM_TAP_CTX* tap)
{
    return zm_util_bufferevent_input_len(tap->requester_bev);
}

void ZmTapContext::FreeTap(ZM_TAP_CTX* tap)
{
    if (tap)
    {
        free(tap);
    }
}



//////////////////////////////////////////////////////////////////////////////////////////////////
// SPTapDelegate
void ZmTapDelegate::StartTapDelegate(struct event_base* evbase, int mode)
{
    _evbase = evbase;
    _mode = mode;

    if (OnStartTap())
    {
        if (!_evdelegate)
        {
            _evdelegate = event_new(_evbase, -1, EV_PERSIST | EV_READ, ZmTapContextEventHandler::OnTapDelegateEventCB, this);
        }
        event_add(_evdelegate, NULL);
    }
}


void ZmTapDelegate::StopTapDelegate()
{
    OnStopTap();
    if (NULL != _evdelegate)
    {
        event_free(_evdelegate);
        _evdelegate = NULL;
    }
}


char* ZmTapDelegate::TapDelegateName(const char* name)
{
    if (name)
    {
        snprintf(_name, sizeof(_name), "%s", name);
    }
    return _name;
}

void ZmTapDelegate::SetEvDns(evdns_base* evdnsbase)
{
    _evdnsbase = evdnsbase;
}

//void ZmTapDelegate::EvDnsResolve(ZM_TAP_CTX* tap, const char* hostname, uint16_t port)
//{
//
//}
//
//void ZmTapDelegate::CancelResolve(ZM_TAP_CTX* tap)
//{
//    evdns_getaddrinfo_cancel(tap->dns_request);
//    tap->dns_request = nullptr;
//}





//////////////////////////////////////////////////////////////////////////////////////////////////
// SPEventHandler

// Inner Events
void ZmTapContextEventHandler::OnTapDelegateEventCB(evutil_socket_t fd, short what, void* ctx)
{
    if (ctx)
    {
        ((ZmTapDelegate*)ctx)->OnTapDelegateEvent(what);
    }
}

void ZmTapContextEventHandler::OnDropTimerCB(evutil_socket_t fd, short what, void* ctx)
{
    if (ctx)
    {
        ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;

        if (tap->drop_timeout_error_code > 0
            && tap->delegate
            && tap->delegate->OnTapError(tap, tap->drop_timeout_error_code))
        {
            // 超时错误，什么也不做，OnTapError 里面处理
        }
        else
        {
            tap->tap_context->Drop(tap, "On Timer");;
        }
    }
}

// DNS resolved
void ZmTapContextEventHandler::OnDnsResolvedCB(int errcode, struct evutil_addrinfo* addr, void* ctx)
{
    ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
    tap->dns_request = nullptr;
    if (errcode != EVUTIL_EAI_CANCEL && addr)
    {
        struct sockaddr_in6 sa6 = { 0 };
        socklen_t           salen = 0;
        char                ipstr[64] = { 0 };
        if (0 == errcode)
        {
            salen = ZmNetDNS::ExtractEventAddrInfo(&sa6, addr, ipstr, sizeof(ipstr));
        }
        PUBLIC_LOG_INFO("Tap: {}, Received the DNS resolved response: errcode={}, hostname={}, ip={}", (void*)tap, errcode, addr->ai_canonname, ipstr);
        tap->delegate->OnTapDnsResolved(tap, &sa6, salen, ipstr, addr->ai_canonname);
    }
    if (addr)
    {
        evutil_freeaddrinfo(addr);
    }
}

void ZmTapContextEventHandler::OnRequesterAcceptConnCB(struct evconnlistener* listener,
    evutil_socket_t fd, struct sockaddr* address, int socklen, void* ctx)
{
    /* We got a new connection! Set up a bufferevent for it. */
    struct event_base* base = evconnlistener_get_base(listener);
    struct bufferevent* bev = bufferevent_socket_new(base, fd, ZM_EVENT_BEV_OPTIONS);
    if (!bev)
    {
        evutil_closesocket(fd);
        return;
    }
    else if (!ctx)
    {
        bufferevent_free(bev);
        return;
    }


    ZmTapDelegate* delegate = (ZmTapDelegate*)ctx;

    // address->sa_family==AF_INET address->sa_family==AF_INET6 address->sa_family==AF_UNIX
    struct sockaddr_in* sa4 = (struct sockaddr_in*)address;
    /** sockaddr_in 和 sockaddr_in6 中 port 的偏移量一致，因此可直接使用 sockaddr_in.sin_port 获取 */
    uint16_t       app_port = ntohs(sa4->sin_port);
    char ipstr[64] = { 0 };
    if (AF_INET6 == address->sa_family)
    {
        ZmNetIP::IPv6ToStr(&((struct sockaddr_in6*)address)->sin6_addr, ipstr, sizeof(ipstr));
    }
    else
    {
        ZmNetIP::IPv4ToStr(((struct sockaddr_in*)address)->sin_addr.s_addr, ipstr, true);
    }

    ZM_TAP_CTX* tap = ((ZmTapHubProxy*)delegate)->TapContext()->Get();
    if (tap)
    {
        tap->tap_context = ((ZmTapHubProxy*)delegate)->TapContext();
        tap->delegate = delegate;
        tap->requester_bev = bev;
        memcpy(tap->requester_ip, ipstr, sizeof(tap->requester_ip));
        tap->requester_port = app_port;

        PUBLIC_LOG_INFO("Accepted a incoming connection, Delegate: {}, mode: {}, Tap: {} from {}:{}; fd: {}, bev: {}", ctx, tap->delegate->TapDelegateMode(), (void*)tap, ipstr, app_port, fd, (void*)bev);

        if (delegate->OnTapRequesterAccept(tap, fd, address))
        {
            if (!delegate->IsCallbackSelfManaged())
            {
                /** assign the read/write callback  */
                bufferevent_setcb(tap->requester_bev, ZmTapContextEventHandler::OnRequesterReadCB, NULL, ZmTapContextEventHandler::OnRequesterEventCB, tap);
                bufferevent_enable(tap->requester_bev, EV_READ | EV_WRITE);
                bufferevent_setwatermark(tap->requester_bev, EV_READ, 0, ZM_BUF_WATERMARK_HIGH);
            }
        }
        else
        {
            ((ZmTapHubProxy*)delegate)->TapContext()->Drop(tap, "On Accept Fail");
        }
    }
    else
    {
        bufferevent_free(bev);
    }
}

void ZmTapContextEventHandler::OnRequesterEventCB(struct bufferevent* requester_bev, short events, void* ctx)
{
    if (ctx)
    {
        ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
        if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
        {
            // needs callback before release
            if (tap->delegate)
            {
                /** 尝试处理未读取数据 */
                size_t applen = ZmTapContext::RequesterInputLen(tap);
                if (applen > 0)
                {
                    tap->delegate->OnTapRequesterRead(tap, bufferevent_get_input(tap->requester_bev), applen);
                }

                tap->delegate->OnTapRequesterEvent(tap, requester_bev, events);
            }
            // force close tunnel while EOF/ERROR occurs on app end.
            tap->tap_context->Drop(tap);
        }
        else if (events & BEV_EVENT_CONNECTED)
        {
            bufferevent_setcb(requester_bev, ZmTapContextEventHandler::OnRequesterReadCB, NULL, ZmTapContextEventHandler::OnRequesterEventCB, tap);
            bufferevent_enable(requester_bev, EV_READ | EV_WRITE);
            bufferevent_setwatermark(requester_bev, EV_READ, 0, ZM_BUF_WATERMARK_HIGH);
            if (tap->delegate)
            {
                tap->delegate->OnTapRequesterEvent(tap, requester_bev, events);
            }
        }
    }
    else
    {
        bufferevent_free(requester_bev);
    }
}

void ZmTapContextEventHandler::OnRequesterReadCB(struct bufferevent* requester_bev, void* ctx)
{
    struct evbuffer* app_input = bufferevent_get_input(requester_bev);
    size_t           datalen = app_input ? evbuffer_get_length(app_input) : 0;
    if (ctx)
    {
        ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
        if (tap->delegate)
        {
            tap->delegate->OnTapRequesterRead(tap, app_input, datalen);
        }
    }
    else
    {
        evbuffer_drain(app_input, datalen);
    }
}

void ZmTapContextEventHandler::OnRequesterWriteCB(struct bufferevent* requester_bev, void* ctx)
{
    if (ctx)
    {
        ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
        if (tap->delegate)
        {
            tap->delegate->OnTapRequesterWrite(tap, requester_bev);
        }
    }
    else
    {
        bufferevent_free(requester_bev);
    }
}