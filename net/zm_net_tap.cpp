#include "zm_net_tap.h"

#include "zm_net_dns.h"
#include "../util/zm_util_libevent.h"
#include "../spdlog/zm_logger.h"

#include <../libevent/include/event2/dns.h>


void ZM_TAP_CTX::Clear()
{
    tap_context = nullptr;
    ev_timeout = nullptr;
    requester_bev = nullptr;
    dns_request = nullptr;
    delegate = nullptr;
    request.Init();
    requester_data = nullptr;
    memset(onback_chains, 0, sizeof(onback_chains));
    onback_data = nullptr;
    onback_dlen = 0;
    state = ZM_TAP_STATE_NONE;
    _slot = nullptr;
    pair_handle = nullptr;
    drop_timeout_error_code = 0;
    requester_data_len = 0;
    requester_received_len = 0;
    requester_port = 0;
    memset(seq_num, 0, sizeof(seq_num));
    memset(requester_ip, 0, sizeof(requester_ip));
}

void ZM_TAP_CTX::SetTapContext(ZmTapContext* pTapContext)
{
    if (pTapContext && !tap_context)
    {
        tap_context = pTapContext;
    }
}

void ZM_TAP_CTX::Drop(const char* reason)
{
    if (tap_context)
    {
        tap_context->Drop(this, reason);
    }
}

ZmTapContext::ZmTapContext()
{
    m_capacity = 0x400;
    m_count = 0;
    m_slots = (ZM_TAP_SLOT*)malloc(TAP_ITEM_SIZE * m_capacity);
    memset(m_slots, 0, TAP_ITEM_SIZE * m_capacity);
    m_seq_counter.store(0, std::memory_order_relaxed);
}

ZmTapContext::~ZmTapContext()
{
    Clear();
}

void ZmTapContext::Clear()
{
    PUBLIC_LOG_INFO("TapContext Clear");

    for (size_t i = 0; i < m_count; i++)
    {
        ZM_TAP_CTX* tap = m_slots[i].tap;
        if (!tap) continue;

        if (tap->delegate)
            tap->delegate->OnTapDrop(tap);

        CancelResolve(tap);
        FreeRequesterEnd(tap);

        if (tap->ev_timeout)
        {
            event_del(tap->ev_timeout);
            event_free(tap->ev_timeout);
            tap->ev_timeout = nullptr;
        }

        if (tap->request.host)      { free(tap->request.host);      tap->request.host      = nullptr; }
        if (tap->request.userinfo)  { free(tap->request.userinfo);  tap->request.userinfo  = nullptr; }
        if (tap->request.path)      { free(tap->request.path);      tap->request.path      = nullptr; }
        if (tap->request.useragent) { free(tap->request.useragent); tap->request.useragent = nullptr; }
        if (tap->requester_data)    { free(tap->requester_data);    tap->requester_data    = nullptr; }
        if (tap->onback_data)       { free(tap->onback_data);       tap->onback_data       = nullptr; }

        ZM_TAP_SLOT* slot = tap->_slot;
        tap->Clear();
        if (slot) slot->tap = nullptr;

        FreeTap(tap);
    }
    memset(m_slots, 0, TAP_ITEM_SIZE * m_capacity);
    m_count = 0;
    m_free_stack.clear();
}

void ZmTapContext::Drop(ZM_TAP_CTX* tap, const char* reason)
{
    if (!tap) return;

    // ★ CAS 原子 Check-And-Set：多线程同时 Drop 时只有一个成功
    uint8_t expected = ZM_TAP_STATE_INUSE;
    if (!tap->state.compare_exchange_strong(expected, ZM_TAP_STATE_DROPPING))
        return;

    if (tap->delegate)
        tap->delegate->OnTapDrop(tap);

    CancelResolve(tap);
    FreeRequesterEnd(tap);

    if (tap->ev_timeout)
    {
        event_del(tap->ev_timeout);
        event_free(tap->ev_timeout);
        tap->ev_timeout = nullptr;
    }

    if (tap->request.host)      { free(tap->request.host);      tap->request.host      = nullptr; }
    if (tap->request.userinfo)  { free(tap->request.userinfo);  tap->request.userinfo  = nullptr; }
    if (tap->request.path)      { free(tap->request.path);      tap->request.path      = nullptr; }
    if (tap->request.useragent) { free(tap->request.useragent); tap->request.useragent = nullptr; }
    if (tap->requester_data)    { free(tap->requester_data);    tap->requester_data    = nullptr; }
    if (tap->onback_data)       { free(tap->onback_data);       tap->onback_data       = nullptr; }

    ZM_TAP_SLOT* slot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        slot = tap->_slot;               // ★ 锁内读，防扩容并发 free
        if (slot) slot->tap = nullptr;
        tap->Clear();
        m_free_stack.push_back(tap);
    }
}

void ZmTapContext::FreeRequesterEnd(ZM_TAP_CTX* tap)
{
    // 若来自 bufferevent_pair 池，触发 EOF 到 pair0
    // 双事件互斥不靠 CAS，靠事件循环线程上 OnResponseRead/OnResponseEvent 的
    // rctx->callback 指针判空（排队执行，谁先到谁消费）。
    if (tap->pair_handle)
    {
        tap->pair_handle->Pair0EOF();
        tap->pair_handle = nullptr;
    }
    else
    {
        zm_util_bufferevent_free(tap->requester_bev);
    }
    tap->requester_bev = nullptr;
}

ZM_TAP_CTX* ZmTapContext::Get()
{
    ZM_TAP_CTX* tap = nullptr;

    // 快速路径：从空闲栈 O(1) 取，锁仅保护栈操作
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_free_stack.empty())
        {
            tap = m_free_stack.back();
            m_free_stack.pop_back();
        }
    }

    if (tap)
    {
        // Clear / seq_num 无需锁：仅当前线程持有此 tap
        tap->Clear();
        tap->SetTapContext(this);
        tap->state = ZM_TAP_STATE_INUSE;
        uint64_t sn = m_seq_counter.fetch_add(1, std::memory_order_relaxed);
        snprintf(tap->seq_num, sizeof(tap->seq_num), "%llu", (unsigned long long)sn);
        return tap;
    }

    // 慢速路径：需要扩容或新建 — 将 malloc 和日志移出临界区
    PUBLIC_LOG_INFO("There are no available tap containers, try creating a new tap container");

    /**
     * 扩容安全保障：通过 ZM_TAP_SLOT + ZM_TAP_CTX::_slot 回指指针，
     * 扩容后同步修正所有 TAP 的 _slot，确保外部持有的槽位引用始终有效。
     */
    bool need_expand = false;
    size_t new_capacity = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_free_stack.empty())
        {
            // 二次检查：上面的锁间隙中可能有 tap 被归还
            tap = m_free_stack.back();
            m_free_stack.pop_back();
        }
        else if (m_count >= m_capacity)
        {
            need_expand = true;
            new_capacity = m_capacity + m_capacity / 2;
        }
    }

    if (tap)
    {
        tap->Clear();
        tap->SetTapContext(this);
        tap->state = ZM_TAP_STATE_INUSE;
        uint64_t sn = m_seq_counter.fetch_add(1, std::memory_order_relaxed);
        snprintf(tap->seq_num, sizeof(tap->seq_num), "%llu", (unsigned long long)sn);
        return tap;
    }

    // 扩容：malloc 在锁外执行
    ZM_TAP_SLOT* new_slots = nullptr;
    if (need_expand)
    {
        PUBLIC_LOG_INFO("Tap container pool size is insufficient, try expanding it, Current TAP size: {}, Current capacity size: {}, new capacity size: {}", m_count, m_capacity, new_capacity);
        new_slots = (ZM_TAP_SLOT*)malloc(TAP_ITEM_SIZE * new_capacity);
        if (new_slots)
        {
            memset(new_slots, 0, TAP_ITEM_SIZE * new_capacity);
        }
    }

    // 新建 tap + 更新数组（需锁）
    tap = CreateTap();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 二次确认：锁间隙中可能已有其他线程完成扩容
        if (new_slots && m_count < m_capacity)
        {
            free(new_slots);
            new_slots = nullptr;
        }
        if (new_slots)
        {
            // 拷贝旧槽位 → 新数组
            memcpy(new_slots, m_slots, TAP_ITEM_SIZE * m_count);
            // 修正所有 TAP 的 _slot 回指指针，指向新数组中的对应位置
            for (size_t i = 0; i < m_count; i++)
            {
                if (new_slots[i].tap)
                {
                    new_slots[i].tap->_slot = &new_slots[i];
                }
            }
            free(m_slots);
            m_slots = new_slots;
            m_capacity = new_capacity;
        }
        // 将新 tap 放入槽位，建立双向关联
        m_slots[m_count].tap = tap;
        tap->_slot = &m_slots[m_count];
        m_count++;
        PUBLIC_LOG_INFO("there are no idle TAP objects available. Create a new Tap. The current number of TAPs is: {}, and the total capacity is: {}", m_count, m_capacity);
    }

    tap->SetTapContext(this);
    tap->state = ZM_TAP_STATE_INUSE;
    uint64_t sn = m_seq_counter.fetch_add(1, std::memory_order_relaxed);
    snprintf(tap->seq_num, sizeof(tap->seq_num), "%llu", (unsigned long long)sn);
    return tap;
}

void ZmTapContext::SetDropTimer(ZM_TAP_CTX* tap, int seconds, int micros, uint32_t drop_timeout_error_code)
{
    if (!tap) return;

    if (seconds >= 0 || micros >= 0)
    {
        tap->drop_timeout_error_code = drop_timeout_error_code;

        if (tap->ev_timeout == nullptr)
        {
            tap->ev_timeout = evtimer_new(tap->delegate->TapDelegateEventBase(), ZmTapContextEventHandler::OnDropTimerCB, tap);
        }
        else
        {
            evtimer_del(tap->ev_timeout);
        }

        timeval tv = { seconds, micros * 1000 };
        event_add(tap->ev_timeout, &tv);
    }
    else
    {
        if (tap->ev_timeout)
            evtimer_del(tap->ev_timeout);
    }
}

void ZmTapContext::Response(ZM_TAP_CTX* tap, const ZMJSON& jsResponse)
{
    if (!tap) return;

    if (tap->state != ZM_TAP_STATE_INUSE)
    {
        //PUBLIC_LOG_WARN("TAP 已失效，丢弃响应，TAP:{}, state:{}", (void*)tap, tap->state);
        return;
    }

    ZmTapDelegate* back_delegate = BackChainPop(tap);
    if (back_delegate)
    {
        std::string jstr = jsResponse.dump();
        SetOnBackData(tap, jstr.size(), jstr.c_str());
        back_delegate->OnTapDelegateBackEvent(tap);
    }
    else
    {
        PUBLIC_LOG_WARN("TAP 回传链为空，无法写入响应，TAP:{}", (void*)tap);
        tap->Drop("back chain empty");
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
    // request 已内联，只需释放内部动态字符串后重新初始化
    if (tap->request.host) { free(tap->request.host); tap->request.host = nullptr; }
    if (tap->request.userinfo) { free(tap->request.userinfo); tap->request.userinfo = nullptr; }
    if (tap->request.path) { free(tap->request.path); tap->request.path = nullptr; }
    if (tap->request.useragent) { free(tap->request.useragent); tap->request.useragent = nullptr; }
    tap->request.Init();
    tap->request.major = 1;
    tap->request.minor = 1;
}

void ZmTapContext::RequestSetAddress(ZM_TAP_CTX* tap, const char* dst_host, uint16_t dst_port)
{
    if (tap->request.host)
    {
        free(tap->request.host);
    }
    tap->request.host = _strdup(dst_host);
    tap->request.port = dst_port;
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

    if (tap->dns_request == nullptr)
    {
        // evdns_getaddrinfo 返回 nullptr 表示立即完成了（命中缓存/hosts/或出错）
        // 此时回调已在 evdns_getaddrinfo 内部被同步调用
        PUBLIC_LOG_INFO("Tap: {}, EvDnsResolve completed immediately (cached or error)", (void*)tap);
    }
}

void ZmTapContext::CancelResolve(ZM_TAP_CTX* tap)
{
    //PUBLIC_LOG_INFO("Tap: {}, CancelResolve", (void*)tap);

    if (tap->dns_request)
    {
        evdns_getaddrinfo_cancel(tap->dns_request);
        tap->dns_request = nullptr;
    }
}

void ZmTapContext::FreeTap(ZM_TAP_CTX* tap)
{
    if (tap)
    {
        free(tap);
    }
}

ZM_TAP_CTX* ZmTapContext::CreateTap()
{
    ZM_TAP_CTX* tap = (ZM_TAP_CTX*)malloc(sizeof(ZM_TAP_CTX));

    if (tap)
    {
        tap->Clear();
    }

    return tap;
}

void ZmTapContext::ForEach(std::function<void(ZM_TAP_CTX*)> fnaction,
    std::function<bool(const ZM_TAP_CTX*)> fnmatches)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_count; i++)
    {
        ZM_TAP_CTX* item = m_slots[i].tap;
        if (fnmatches == nullptr || fnmatches(item))
        {
            fnaction(item);
        }
    }
}

ZmTapDelegate* ZmTapContext::BackChainPop(ZM_TAP_CTX* tap, bool remove)
{
    /** 从链尾向链首遍历，LIFO 语义：最后压入的最先弹出 */
    for (int i = (ZM_TAP_DELEGATE_CHAIN_MAX - 1); i >= 0; i--)
    {
        ZmTapDelegate* delegate = tap->onback_chains[i];
        if (delegate)
        {
            if (remove) { tap->onback_chains[i] = nullptr; }
            return delegate;
        }
    }
    return nullptr;
}

void ZmTapContext::BackChainPush(ZM_TAP_CTX* tap, ZmTapDelegate* delegate)
{
    if (delegate && delegate != BackChainPop(tap, false))
    {
        for (int i = 0; i < ZM_TAP_DELEGATE_CHAIN_MAX; i++)
        {
            if (tap->onback_chains[i] == nullptr)
            {
                tap->onback_chains[i] = delegate;
                break;
            }
        }
    }
}

bool ZmTapContext::IsBackChainEmpty(ZM_TAP_CTX* tap)
{
    for (int i = (ZM_TAP_DELEGATE_CHAIN_MAX - 1); i >= 0; i--)
    {
        ZmTapDelegate* delegate = tap->onback_chains[i];
        if (delegate)
        {
            return false;
        }
    }
    return true;
}


// ============================================================================
// BuffereventPairPool
// ============================================================================
// ZmTapDelegate
ZmTapDelegate::ZmTapDelegate(struct event_base* evbase)
    : m_evbase(evbase), m_evdnsbase(nullptr), m_evdelegate(nullptr), m_mode(0)
{
    TapDelegateName("ZmTapDelegate");
}

void ZmTapDelegate::StartTapDelegate(int mode)
{
    m_mode = mode;

    if (OnStartTap())
    {
        if (m_evdelegate == nullptr)
        {
            m_evdelegate = event_new(m_evbase, -1, EV_PERSIST | EV_READ, ZmTapContextEventHandler::OnTapDelegateEventCB, this);
        }
        event_add(m_evdelegate, nullptr);
    }
}

void ZmTapDelegate::StopTapDelegate()
{
    OnStopTap();

    if (m_evdelegate != nullptr)
    {
        event_free(m_evdelegate);
        m_evdelegate = nullptr;
    }
}


char* ZmTapDelegate::TapDelegateName(const char* name)
{
    if (name)
    {
        snprintf(m_name, sizeof(m_name), "%s", name);
    }
    return m_name;
}

void ZmTapDelegate::SetEvDns(evdns_base* evdnsbase)
{
    m_evdnsbase = evdnsbase;
}

int ZmTapDelegate::TapDelegateMode()
{
    return m_mode;
}

event_base* ZmTapDelegate::TapDelegateEventBase()
{
    return m_evbase;
}

evdns_base* ZmTapDelegate::TapDelegateEvdnsBase()
{
    return m_evdnsbase;
}

// ============================================================================
// ZmTapContextEventHandler

std::map < std::string, ContextEventHandlerParams>
ZmTapContextEventHandler::m_registry;

void ZmTapContextEventHandler::RegistryContextEventHandler(
    const char* name, ZmTapContext* ctx, void* delegate)
{
    if (name && ctx)
        m_registry[name] = { ctx, delegate };
}

void ZmTapContextEventHandler::UnregistryContextEventHandler(const char* name)
{
    if (name)
        m_registry.erase(name);
}

ContextEventHandlerParams* ZmTapContextEventHandler::FindContextEventHandler(const char* name)
{
    if (!name)
        return nullptr;
    auto it = m_registry.find(name);
    return (it != m_registry.end()) ? &it->second : nullptr;
}

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
            && tap->delegate->OnTapTimeOut(tap, tap->drop_timeout_error_code))
        {
            // 超时错误，什么也不做，OnTapError 里面处理
        }
        else
        {
            tap->Drop("On Timer");
        }
    }
}

// DNS resolved
void ZmTapContextEventHandler::OnDnsResolvedCB(int errcode, struct evutil_addrinfo* addr, void* ctx)
{
    ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;
    tap->dns_request = nullptr;
    if (errcode != EVUTIL_EAI_CANCEL && addr != nullptr)
    {
        struct sockaddr_in6 sa6 = { 0 };
        socklen_t           salen = 0;
        char                ipstr[64] = { 0 };
        if (errcode == 0)
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
    evutil_socket_t fd, struct sockaddr* address, int socklen, void* params)
{
    ZmTapDelegate* delegate = (ZmTapDelegate*)((ContextEventHandlerParams*)params)->delegate;
    ZmTapContext* context = ((ContextEventHandlerParams*)params)->ctx;
    struct event_base* base = evconnlistener_get_base(listener);

    if (delegate == nullptr || context == nullptr || context == nullptr)
    {
        evutil_closesocket(fd);
        return;
    }

    struct bufferevent* bev = bufferevent_socket_new(base, fd, ZM_EVENT_BEV_OPTIONS);
    if (bev == nullptr)
    {
        bufferevent_free(bev);
        evutil_closesocket(fd);
        return;
    }

    ZM_TAP_CTX* tap = context->Get();
    if (tap == nullptr)
    {
        bufferevent_free(bev);
        evutil_closesocket(fd);
        return;
    }

    tap->delegate = delegate;
    tap->requester_bev = bev;

    if (address)
    {
        if (address->sa_family == AF_INET6)
        {
            struct sockaddr_in6* addr6 = (struct sockaddr_in6*)address;
            ZmNetIP::IPv6ToStr(&addr6->sin6_addr, tap->requester_ip, sizeof(tap->requester_ip));
            tap->requester_port = ntohs(addr6->sin6_port);
        }
        else
        {
            struct sockaddr_in* addr4 = (struct sockaddr_in*)address;
            ZmNetIP::IPv4ToStr(&addr4->sin_addr, tap->requester_ip, sizeof(tap->requester_ip));
            tap->requester_port = ntohs(addr4->sin_port);
        }
    }
    else
    {
        strncpy_s(tap->requester_ip, "127.0.0.1", sizeof(tap->requester_ip));
        tap->requester_port = 0;
    }

    //PUBLIC_LOG_INFO("Accepted a incoming connection, Delegate: {}, mode: {}, Tap: {} from {}:{}; fd: {}, bev: {}", ctx, tap->delegate->TapDelegateMode(), (void*)tap, ipstr, app_port, fd, (void*)bev);

    if (!delegate->OnTapRequesterAccept(tap))
        tap->Drop("OnPairAcceptBev: delegate accept failed");
}

/**
 * @brief 接受 bufferevent 注入
 * 直接使用传入的 bufferevent（如 bufferevent_pair 的一端）作为 TAP 的 requester_bev。
 * 用于进程内零拷贝通信，bev 无需关联 socket fd。
 *
 * @note 调用后 bev 由 TAP 接管生命周期（BEV_OPT_CLOSE_ON_FREE），调用者不应再操作 bev
 */
bool ZmTapContextEventHandler::OnPairAcceptBev(ContextEventHandlerParams* params, struct bufferevent* bev,
                                                struct sockaddr* address, BuffereventPairHandle* handle)
{
    ZmTapDelegate* delegate = (ZmTapDelegate*)params->delegate;
    ZmTapContext* context = params->ctx;

    if (delegate == nullptr || bev == nullptr || context == nullptr)
    {
        if (bev && !handle)
            bufferevent_free(bev);
        return false;
    }

    ZM_TAP_CTX* tap = context->Get();
    if (tap == nullptr)
    {
        if (!handle)
            bufferevent_free(bev);
        return false;
    }

    tap->delegate = delegate;
    tap->requester_bev = bev;
    tap->pair_handle = handle;

    if (address)
    {
        if (address->sa_family == AF_INET6)
        {
            struct sockaddr_in6* addr6 = (struct sockaddr_in6*)address;
            ZmNetIP::IPv6ToStr(&addr6->sin6_addr, tap->requester_ip, sizeof(tap->requester_ip));
            tap->requester_port = ntohs(addr6->sin6_port);
        }
        else
        {
            struct sockaddr_in* addr4 = (struct sockaddr_in*)address;
            ZmNetIP::IPv4ToStr(&addr4->sin_addr, tap->requester_ip, sizeof(tap->requester_ip));
            tap->requester_port = ntohs(addr4->sin_port);
        }
    }
    else
    {
        strncpy_s(tap->requester_ip, "127.0.0.1", sizeof(tap->requester_ip));
        tap->requester_port = 0;
    }

    if (!delegate->OnTapRequesterAccept(tap))
        tap->Drop("OnPairAcceptBev: delegate accept failed");

    return true;
}

bool ZmTapContextEventHandler::OnPairAcceptBev(const char* name, struct bufferevent* bev,
                                                struct sockaddr* address, BuffereventPairHandle* handle)
{
    auto it = m_registry.find(name);
    if (it != m_registry.end())
        return OnPairAcceptBev(&it->second, bev, address, handle);

    return false;
}

void ZmTapContextEventHandler::OnRequesterEventCB(struct bufferevent* requester_bev, short events, void* ctx)
{
    ZM_TAP_CTX* tap = (ZM_TAP_CTX*)ctx;

    if (tap)
    {
        if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
        {
            if (tap->delegate)
            {
                tap->delegate->OnTapRequesterEvent(tap, requester_bev, events);
            }
        }
        else if (events & BEV_EVENT_CONNECTED)
        {
            bufferevent_setcb(requester_bev, ZmTapContextEventHandler::OnRequesterReadCB, nullptr, ZmTapContextEventHandler::OnRequesterEventCB, tap);
            bufferevent_enable(requester_bev, EV_READ | EV_WRITE);
            bufferevent_setwatermark(requester_bev, EV_READ, 0, ZM_BUF_WATERMARK_HIGH);
            if (tap->delegate)
            {
                tap->delegate->OnTapRequesterEvent(tap, requester_bev, events);
            }
        }
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