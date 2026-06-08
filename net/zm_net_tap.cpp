#include "zm_net_tap.h"

#include "../util/zm_util_libevent.h"
#include "zm_net_dns.h"
#include "../spdlog/zm_logger.h"

#define ZM_EVENT_BEV_OPTIONS        BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS|BEV_OPT_THREADSAFE


void ZM_TAP_CTX::Clear()
{
    ev_base = nullptr;
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

const ZmTapContext* ZM_TAP_CTX::TapContext()
{
    return tap_context;
}

void ZM_TAP_CTX::SetEventBase(event_base* evbase)
{
    if (evbase && !ev_base)
    {
        ev_base = evbase;
    }
}

const event_base* ZM_TAP_CTX::EventBase()
{
    return ev_base;
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
        ZM_TAP_CTX* item = m_slots[i].tap;
        Drop(item, "By Clear");
        FreeTap(item);
    }
    memset(m_slots, 0, TAP_ITEM_SIZE * m_capacity);
    m_count = 0;
    m_free_stack.clear();
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

        // request 已改为内联存储，只需释放其内部的动态字符串
        if (tap->request.host) { free(tap->request.host); tap->request.host = nullptr; }
        if (tap->request.userinfo) { free(tap->request.userinfo); tap->request.userinfo = nullptr; }
        if (tap->request.path) { free(tap->request.path); tap->request.path = nullptr; }
        if (tap->request.useragent) { free(tap->request.useragent); tap->request.useragent = nullptr; }

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

        // 保存槽位引用后清除 TAP 状态
        ZM_TAP_SLOT* slot = tap->_slot;
        tap->Clear();

        // 回收到空闲栈，供 Get() 快速复用（O(1)）
        // 同时清除槽位回指，防止 ForEach 遍历到已回收的 TAP
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (slot)
            {
                slot->tap = nullptr;
            }
            m_free_stack.push_back(tap);
        }

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
        uint64_t sn = m_seq_counter.fetch_add(1, std::memory_order_relaxed);
        snprintf(tap->seq_num, sizeof(tap->seq_num), "%llu", (unsigned long long)sn);
        return tap;
    }

    // 扩容：malloc 在锁外执行
    ZM_TAP_SLOT* new_slots = nullptr;
    if (need_expand)
    {
        PUBLIC_LOG_INFO("Tap container pool size is insufficient, try expanding it, Current size: {}", m_count);
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
            PUBLIC_LOG_INFO("The total size of the tap pool after expansion: {}", m_capacity);
        }
        // 将新 tap 放入槽位，建立双向关联
        m_slots[m_count].tap = tap;
        tap->_slot = &m_slots[m_count];
        m_count++;
    }

    tap->SetTapContext(this);
    uint64_t sn = m_seq_counter.fetch_add(1, std::memory_order_relaxed);
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
            tap->ev_timeout = evtimer_new(const_cast<event_base*>(tap->EventBase()), ZmTapContextEventHandler::OnDropTimerCB, tap);
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

    if (tap->dns_request)
    {
        evdns_getaddrinfo_cancel(tap->dns_request);
        tap->dns_request = nullptr;
    }
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

ZM_TAP_CTX* ZmTapContext::CreateTap()
{
    ZM_TAP_CTX* tap = (ZM_TAP_CTX*)malloc(sizeof(ZM_TAP_CTX));

    if (tap)
    {
        tap->Clear();
    }

    tap->state = ZM_TAP_STATE_INUSE;

    return tap;
}

void ZmTapContext::ForEach(std::function<void(ZM_TAP_CTX*)> fnaction,
    std::function<bool(const ZM_TAP_CTX*)> fnmatches)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_count; i++)
    {
        ZM_TAP_CTX* item = m_slots[i].tap;
        if (!fnmatches || fnmatches(item))
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
            if (remove) { tap->onback_chains[i] = NULL; }
            return delegate;
        }
    }
    return NULL;
}

void ZmTapContext::BackChainPush(ZM_TAP_CTX* tap, ZmTapDelegate* delegate)
{
    if (delegate && delegate != BackChainPop(tap, false))
    {
        for (int i = 0; i < ZM_TAP_DELEGATE_CHAIN_MAX; i++)
        {
            if (NULL == tap->onback_chains[i])
            {
                tap->onback_chains[i] = delegate;
                break;
            }
        }
    }
}



//////////////////////////////////////////////////////////////////////////////////////////////////
// ZmTapDelegate
ZmTapDelegate::ZmTapDelegate()
    : m_evbase(nullptr), m_evdnsbase(nullptr), m_evdelegate(nullptr)
{
    TapDelegateName("ZmTapDelegate");
}

void ZmTapDelegate::StartTapDelegate(struct event_base* evbase, int mode)
{
    m_evbase = evbase;
    m_mode = mode;

    if (OnStartTap())
    {
        if (!m_evdelegate)
        {
            m_evdelegate = event_new(m_evbase, -1, EV_PERSIST | EV_READ, ZmTapContextEventHandler::OnTapDelegateEventCB, this);
        }
        event_add(m_evdelegate, NULL);
    }
}


void ZmTapDelegate::StopTapDelegate()
{
    OnStopTap();
    if (NULL != m_evdelegate)
    {
        event_free(m_evdelegate);
        m_evdelegate = NULL;
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



//////////////////////////////////////////////////////////////////////////////////////////////////
// ZmTapContextEventHandler

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
            tap->Drop("On Timer");;
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
    /*HUB有了新的连接 为它设置一个缓冲区*/
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

    ZmTapContext* context = delegate->TapContext();
    if (!context)
    {
        bufferevent_free(bev);
        return;
    }

    ZM_TAP_CTX* tap = context->Get();
    if (tap)
    {
        tap->delegate = delegate;
        tap->SetEventBase(delegate->TapDelegateEventBase());
        tap->requester_bev = bev;
        memcpy(tap->requester_ip, ipstr, sizeof(tap->requester_ip));
        tap->requester_port = app_port;
        tap->state = ZM_TAP_STATE_INUSE;

        PUBLIC_LOG_INFO("Accepted a incoming connection, Delegate: {}, mode: {}, Tap: {} from {}:{}; fd: {}, bev: {}", ctx, tap->delegate->TapDelegateMode(), (void*)tap, ipstr, app_port, fd, (void*)bev);

        if (delegate->OnTapRequesterAccept(tap, fd, address))
        {
            if (!delegate->IsCallbackSelfManaged())
            {
                /**分配读/写回调*/
                bufferevent_setcb(tap->requester_bev, ZmTapContextEventHandler::OnRequesterReadCB, NULL, ZmTapContextEventHandler::OnRequesterEventCB, tap);
                bufferevent_enable(tap->requester_bev, EV_READ | EV_WRITE);
                bufferevent_setwatermark(tap->requester_bev, EV_READ, 0, ZM_BUF_WATERMARK_HIGH);
            }
        }
        else
        {
            context->Drop(tap, "On Accept Fail");
        }
    }
    else
    {
        bufferevent_free(bev);
    }
}

/**
 * @brief 接受 socket pair 连接 — 与 OnRequesterAcceptConnCB 对应但无 evconnlistener 依赖
 *
 * 为 pair 的 fd 创建 bufferevent + TAP，注入 delegate 的协议探测链。
 * 用于进程内跨线程交付（如 Worker 线程 → 事件循环线程），替代 TCP 短连接。
 *
 * @note 调用后 fd 由 bufferevent 接管（BEV_OPT_CLOSE_ON_FREE），调用者不应再操作 fd
 */
bool ZmTapContextEventHandler::OnPairAcceptConn(void* ctx, evutil_socket_t fd)
{
    ZmTapDelegate* delegate = (ZmTapDelegate*)ctx;

    if (!delegate)
    {
        if (fd >= 0) evutil_closesocket(fd);
        return false;
    }

    // 1. 为 pair fd 创建 bufferevent
    struct bufferevent* bev = bufferevent_socket_new(delegate->TapDelegateEventBase(), fd, ZM_EVENT_BEV_OPTIONS);
    if (!bev)
    {
        evutil_closesocket(fd);
        return false;
    }

    ZmTapContext* context = delegate->TapContext();
    if (!context)
    {
        bufferevent_free(bev);
        evutil_closesocket(fd);
        return false;
    }

    // 2. 从池中获取 TAP
    ZM_TAP_CTX* tap = context->Get();
    if (!tap)
    {
        bufferevent_free(bev);
        evutil_closesocket(fd);
        return false;
    }

    // 3. 设置 TAP 字段（与 OnRequesterAcceptConnCB 一致，但 IP 固定为 127.0.0.1）
    tap->delegate = delegate;
    tap->SetEventBase(delegate->TapDelegateEventBase());
    tap->requester_bev = bev;
    strncpy_s(tap->requester_ip, "127.0.0.1", sizeof(tap->requester_ip));
    tap->requester_port = 0;
    tap->state = ZM_TAP_STATE_INUSE;

    // 4. 调用 delegate 的 Accept 设置协议探测回调
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (delegate->OnTapRequesterAccept(tap, fd, (struct sockaddr*)&addr))
    {
        if (!delegate->IsCallbackSelfManaged())
        {
            /**分配读/写回调*/
            bufferevent_setcb(tap->requester_bev, ZmTapContextEventHandler::OnRequesterReadCB, NULL, ZmTapContextEventHandler::OnRequesterEventCB, tap);
            bufferevent_enable(tap->requester_bev, EV_READ | EV_WRITE);
            bufferevent_setwatermark(tap->requester_bev, EV_READ, 0, ZM_BUF_WATERMARK_HIGH);
        }
    }
    else
    {
        context->Drop(tap, "OnPairAcceptConn: delegate accept failed");
        return false;
    }

    return true;
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
            tap->Drop("force close tunnel while EOF/ERROR occurs on app end");
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