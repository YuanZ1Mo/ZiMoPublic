#ifndef ZM_NET_TAP_H
#define ZM_NET_TAP_H

#include "zm_net_http.h"

#include <atomic>
#include <mutex>
#include <vector>

#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/dns.h>
#include <event2/listener.h>



#define ZM_TAP_DELEGATE_CHAIN_MAX   4
#define ZM_EVENT_LISTEN_FLAGS       LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC

#define ZM_BUF_SIZE_16K             0x3FF0              // 16k - 12 - 4
#define ZM_BUF_SIZE_32K             0x8000              // 32k
#define ZM_BUF_SIZE_64K             0x10000             // 64k
#define ZM_BUF_SIZE_512K            0x80000             // 512k
#define ZM_BUF_SIZE_1M              0x100000            // 1024*1024
#define ZM_BUF_SIZE_4M              0x400000            // 1024*1024*4

#define ZM_BUF_WATERMARK_HIGH       ZM_BUF_SIZE_64K
#define ZM_BUF_WATERMARK_LOW        ZM_BUF_SIZE_16K

/** TLV结构 */
typedef struct SP_PACKED
{
    char     tag[4];
    uint32_t len;
    char     value[0];
}ZM_EXT_TLV_HEAD;

typedef struct ZM_TAP_CTX ZM_TAP_CTX;  // 前置声明，供 ZM_TAP_SLOT 使用

/** Pool 中的槽位，扩容时地址不变（数组整体重分配后回指指针会被同步修正） */
typedef struct ZM_TAP_SLOT
{
    ZM_TAP_CTX* tap;
} ZM_TAP_SLOT;

class ZmTapDelegate;
class ZmTapContext;


typedef enum
{
    ZM_DELEGATE_MODE_NONE = 0,
    ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB = 1,  //内部代理
    ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC = 2,  //内部JRPC代理
    ZM_DELEGATE_MODE_DNR_ASYNC = 3,  //异步域名解析器
} ZM_DELEGATE_MODE;

typedef enum
{
    ZM_TAP_STATE_NONE = 0x00,      // Created
    ZM_TAP_STATE_INUSE = 0x01, 
    ZM_TAP_STATE_DROPPING = 0x7F,     // Dropping
}ZM_TAP_STATE;


typedef struct ZM_TAP_CTX
{
    //event_base* ev_base;  //必须设置


    ZmTapContext* tap_context;

    event* ev_timeout;       // [4/8] timeout event
    uint32_t drop_timeout_error_code = 0;

    bufferevent* requester_bev;        // [4/8] requester End  - bufferevent
    BYTE* requester_data;        // [4/8] option data
    uint32_t                requester_data_len;         // [4] option data len
    uint32_t            requester_content_len;
    uint16_t                requester_port;       // [2]
    char                    requester_ip[64];     


    uint8_t                 state;


    evdns_getaddrinfo_request* dns_request;    // [4/8]


    // the follow fields should not be free when free TAP
    ZmTapDelegate* delegate;       // [4/8] Delegate
    //uint8_t        delegate_mode;  //mode
    //ZmTapDelegate* delegate_jrpc;


    ZM_HTTP_REQ request;             // 内联存储，避免每次 malloc






    /** 链式回调代理 */
    ZmTapDelegate* onback_chains[ZM_TAP_DELEGATE_CHAIN_MAX];    // [4/8]*4
    void* onback_data;        // [4/8]
    uint32_t  onback_dlen;        // [4]


    char seq_num[16];       // 消息序号

    ZM_TAP_SLOT* _slot;       // 回指 pool 中的槽位（扩容时由 ZmTapContext 同步更新，外部可通过此指针稳定引用槽位）

    void Clear()
    {
        //ev_base = nullptr;
        tap_context = nullptr;
        ev_timeout = nullptr;
        requester_bev = nullptr;
        dns_request = nullptr;
        delegate = nullptr;
        //delegate_jrpc = nullptr;
        request.Init();
        requester_data = nullptr;
        memset(onback_chains, 0, sizeof(onback_chains));
        onback_data = nullptr;
        onback_dlen = 0;

        //delegate_mode = ZM_DELEGATE_MODE_NONE;
        state = ZM_TAP_STATE_NONE;
        _slot = nullptr;  // 由 ZmTapContext::Drop 后的回收逻辑统一处理
        drop_timeout_error_code = 0;
        requester_data_len = 0;
        requester_content_len = 0;
        requester_port = 0;

        memset(seq_num, 0, sizeof(seq_num));
        memset(requester_ip, 0, sizeof(requester_ip));
    }

};


/**
 * TODO:
 *  1. 每次释放后需要重新排序（将没有使用的放到后面），以加快搜索效率（避免遍历）
 *  2. 定时释放空间，以减少内存占用
 */
class ZmTapContext
{
public:
    ZmTapContext();
    ~ZmTapContext();

    void        Clear();

    /** 回收引用，但不释放内存，可循环使用 */
    void        Drop(ZM_TAP_CTX* tap, const char* reason = "");


    ZM_TAP_CTX* Get();

    inline void ForEach(std::function<void(ZM_TAP_CTX*)> fnaction,
        std::function<bool(const ZM_TAP_CTX*)> fnmatches)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        for (size_t i = 0; i < _count; i++)
        {
            ZM_TAP_CTX* item = _slots[i].tap;
            if (!fnmatches || fnmatches(item))
            {
                fnaction(item);
            }
        }
    }

public:
    static void SetDropTimer(ZM_TAP_CTX* tap, int seconds = 0, int micros = 0, uint32_t drop_timeout_error_code = 0);
    static void FreeRequesterEnd(ZM_TAP_CTX* tap);
    static void SetOptData(ZM_TAP_CTX* tap, size_t optlen = 0, const BYTE* optdata = nullptr);
    static void SetOnBackData(ZM_TAP_CTX* tap, size_t dlen = 0, const void* data = nullptr);
    static void RequestCreate(ZM_TAP_CTX* tap);
    static void RequestSetAddress(ZM_TAP_CTX* tap, const char* dst_host, uint16_t dst_port);

    static void EvDnsResolve(ZM_TAP_CTX* tap, const char* hostname, uint16_t port);
    static void CancelResolve(ZM_TAP_CTX* tap);

    // Back-Delegate chain
    static inline ZmTapDelegate* BackChainPop(ZM_TAP_CTX* tap, bool remove = true)
    {
        for (int i = (ZM_TAP_DELEGATE_CHAIN_MAX - 1); i >= 0; i--)
        {
            ZmTapDelegate* delegate = tap->onback_chains[i];
            if (remove) { tap->onback_chains[i] = NULL; }
            if (delegate) { return delegate; }
        }
        return NULL;
    }

    static inline void BackChainPush(ZM_TAP_CTX* tap, ZmTapDelegate* delegate)
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

    static size_t RequesterInputLen(ZM_TAP_CTX* tap);

private:
    inline ZM_TAP_CTX* CreateTap()
    {
        ZM_TAP_CTX* tap = (ZM_TAP_CTX*)malloc(sizeof(ZM_TAP_CTX));

        if (tap)
        {
            tap->Clear();
        }

        tap->state = ZM_TAP_STATE_INUSE;

        return tap;
    }

    void        FreeTap(ZM_TAP_CTX* tap);

private:
    enum { TAP_ITEM_SIZE = sizeof(ZM_TAP_SLOT) };
    ZM_TAP_SLOT*    _slots;
    size_t          _count;
    size_t          _capacity;
    std::mutex      _mutex;

    std::atomic<uint64_t> _seq_counter;       // 原子自增，用于生成唯一 seq_num
    std::vector<ZM_TAP_CTX*> _free_stack;      // 空闲 TAP 栈，Get/Drop O(1)
};



class ZmTapDelegate
{
public:
    ZmTapDelegate() : _evbase(nullptr), _evdnsbase(nullptr), _evdelegate(nullptr) { TapDelegateName("ZmTapDelegate"); }
    virtual ~ZmTapDelegate() {}

    void    StartTapDelegate(struct event_base* evbase, int mode = ZM_DELEGATE_MODE_NONE);
    void    StopTapDelegate();
    char* TapDelegateName(const char* name = nullptr);
    void SetEvDns(evdns_base* evdnsbase);

    int    TapDelegateMode() { return _mode; }
    event_base* TapDelegateEventBase() { return  _evbase; }
    evdns_base* TapDelegateEvdnsBase() { return  _evdnsbase; }

    //void EvDnsResolve(ZM_TAP_CTX* tap, const char* hostname, uint16_t port);
    //void CancelResolve(ZM_TAP_CTX* tap);

public:
    virtual void OnTapRequesterEvent(ZM_TAP_CTX* tap, struct bufferevent* requester_bev, short events) {}
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) = 0;
    virtual void OnTapRequesterWrite(ZM_TAP_CTX* tap, struct bufferevent* requester_bev) {}
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) = 0;
    virtual void OnTapDnsResolved(ZM_TAP_CTX* tap, struct sockaddr_in6* sa6, socklen_t salen,
        const char* ipaddr, const char* hostname) {
    }

    //返回true表示OnTapError自己处理了错误,返回false表示自己不处理错误,由ZmTapContextEventHandler::OnDropTimerCB处理
    virtual bool OnTapError(ZM_TAP_CTX* tap, uint32_t error) { return false; }
    virtual void OnTapDrop(ZM_TAP_CTX* tap) {}
    virtual void OnTapDelegateEvent(short what) = 0;

    virtual void OnTapDelegateBackEvent(ZM_TAP_CTX* tap) {}

    /**
     * @brief delegate 是否自行管理 bufferevent 回调
     * @return true  表示 OnTapRequesterAccept 中已设置回调，上层不再覆盖
     *         false 使用默认的 OnRequesterReadCB / OnRequesterEventCB
     */
    virtual bool IsCallbackSelfManaged() { return false; }

protected:
    inline  void ActiveTapDelegateEvent(short what) { if (_evdelegate) { event_active(_evdelegate, what, 0); } }



    /**
     * @return bool 是否需要创建 _evdelegate
     */
    virtual bool OnStartTap() { return false; }
    virtual void OnStopTap() {}

protected:
    struct event_base* _evbase;
    struct evdns_base* _evdnsbase;
    struct event* _evdelegate;
    char          _name[32];
    int           _mode;
};






class ZmTapContextEventHandler
{
public:
    static void OnTapDelegateEventCB(evutil_socket_t fd, short what, void* ctx);
    static void OnDnsResolvedCB(int errcode, struct evutil_addrinfo* addr, void* ctx);
    static void OnDropTimerCB(evutil_socket_t fd, short what, void* ctx);


    static void OnRequesterAcceptConnCB(struct evconnlistener* listener,
        evutil_socket_t fd, struct sockaddr* address, int socklen, void* ctx);

    static void OnRequesterEventCB(struct bufferevent* requester_bev, short events, void* ctx);
    static void OnRequesterReadCB(struct bufferevent* requester_bev, void* ctx);
    static void OnRequesterWriteCB(struct bufferevent* requester_bev, void* ctx);
};


#endif /* ZM_NET_TAP_H */
