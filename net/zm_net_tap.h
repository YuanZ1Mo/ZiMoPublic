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

/** @brief 回传代理链最大长度 */
#define ZM_TAP_DELEGATE_CHAIN_MAX   4
/** @brief evconnlistener 监听标志：地址复用 + 释放时关闭 + exec 时关闭 */
#define ZM_EVENT_LISTEN_FLAGS       LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC

/** @brief 缓冲区大小预设 */
#define ZM_BUF_SIZE_16K             0x3FF0
#define ZM_BUF_SIZE_32K             0x8000
#define ZM_BUF_SIZE_64K             0x10000
#define ZM_BUF_SIZE_512K            0x80000
#define ZM_BUF_SIZE_1M              0x100000
#define ZM_BUF_SIZE_4M              0x400000

/** @brief bufferevent 水位线：高水位 64KB，低水位 16KB */
#define ZM_BUF_WATERMARK_HIGH       ZM_BUF_SIZE_64K
#define ZM_BUF_WATERMARK_LOW        ZM_BUF_SIZE_16K

/** @brief TLV 扩展头结构体（SP_PACKED 紧凑布局） */
typedef struct SP_PACKED
{
    char     tag[4];       /** 标签标识 */
    uint32_t len;          /** 数据长度 */
    char     value[0];     /** 柔性数组，实际数据紧跟其后 */
}ZM_EXT_TLV_HEAD;

/** @brief ZM_TAP_CTX 前置声明，供 ZM_TAP_SLOT 使用 */
typedef struct ZM_TAP_CTX ZM_TAP_CTX;

/** @brief Pool 中的槽位，通过回指指针在扩容时保持引用有效 */
typedef struct ZM_TAP_SLOT
{
    ZM_TAP_CTX* tap;       /** 指向对应的 TAP 上下文 */
} ZM_TAP_SLOT;

class ZmTapDelegate;
class ZmTapContext;

/** @brief delegate 工作模式枚举 */
typedef enum
{
    ZM_DELEGATE_MODE_NONE = 0,                 /** 未设置 */
    ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB = 1,   /** 内部 Hub 代理 */
    ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC = 2,  /** 内部 JRPC 代理 */
    ZM_DELEGATE_MODE_DNR_ASYNC = 3,            /** 异步域名解析器（已废弃） */
} ZM_DELEGATE_MODE;

/** @brief TAP 状态枚举 */
typedef enum
{
    ZM_TAP_STATE_NONE     = 0x00,  /** 已创建，空闲中 */
    ZM_TAP_STATE_INUSE    = 0x01,  /** 使用中 */
    ZM_TAP_STATE_DROPPING = 0x7F,  /** 正在释放 */
}ZM_TAP_STATE;

/**
 * @brief TAP 上下文结构体，代表一个网络连接会话的完整状态
 *
 * 生命周期由 ZmTapContext 管理：Get() 获取 → 使用 → Drop() 回收 → 空闲栈复用。
 * 扩容时 _slot 回指指针会被同步修正，保证外部持有的槽位引用不悬空。
 */
typedef struct ZM_TAP_CTX
{
private:
    ZmTapContext* tap_context;                /** 所属的 TAP 池，暂定每个ZM_TAP_CTX对象在未收回前有且只有一个唯一值  */

public:

    event*        ev_timeout;                 /** 超时定时器事件 */
    uint32_t      drop_timeout_error_code;    /** 超时错误码 */

    bufferevent*  requester_bev;              /** 请求端 bufferevent */
    BYTE*         requester_data;             /** 请求数据接收缓冲区（变长，堆分配） */
    uint32_t      requester_data_len;         /** 期望接收的请求数据总长度 */
    uint32_t      requester_received_len;     /** 已接收的请求数据长度 */
    uint16_t      requester_port;             /** 请求来源端口 */
    char          requester_ip[64];           /** 请求来源 IP 地址字符串 */

    uint8_t       state;                      /** 当前状态，见 ZM_TAP_STATE */

    evdns_getaddrinfo_request* dns_request;   /** libevent DNS 解析请求句柄 */

    // 以下字段在释放 TAP 时不 free（由 delegate 持有或内联存储）
    ZmTapDelegate* delegate;                  /** 当前关联的协议处理器 */
    ZM_HTTP_REQ    request;                   /** HTTP 请求参数（内联存储，避免额外 malloc） */

    /** 回传代理链：响应数据按 LIFO 顺序经过链上各 delegate 处理 */
    ZmTapDelegate* onback_chains[ZM_TAP_DELEGATE_CHAIN_MAX];
    void*          onback_data;               /** 回传数据缓冲区 */
    uint32_t       onback_dlen;               /** 回传数据长度 */

    char           seq_num[16];               /** 消息序号（原子自增生成，唯一标识） */
    ZM_TAP_SLOT*   _slot;                     /** 回指 pool 中的槽位，扩容时被 ZmTapContext 同步更新 */

public:
    /**
     * @brief 重置所有字段到初始状态
     * @note 不清除 _slot 指向的槽位关系，仅将 _slot 置空
     */
    void Clear();

    void SetTapContext(ZmTapContext* pTapContext);

    const ZmTapContext* TapContext();

    void Drop(const char* reason = "");
} ZM_TAP_CTX;

/**
 * @brief TAP 对象池，管理 ZM_TAP_CTX 的创建、回收和复用
 *
 * 内部维护空闲栈（m_free_stack）实现 O(1) 获取和归还。
 * 扩容时通过 ZM_TAP_SLOT + _slot 回指指针保证槽位引用稳定。
 */
class ZmTapContext
{
public:
    ZmTapContext();
    ~ZmTapContext();

    /** @brief 清空池中所有 TAP 并释放内存 */
    void        Clear();
    /** @brief 回收 TAP 引用（不释放内存，置为 NONE 状态后推入空闲栈） */
    void        Drop(ZM_TAP_CTX* tap, const char* reason = "");
    /** @brief 从池中获取一个可用 TAP（优先从空闲栈 O(1) 获取） */
    ZM_TAP_CTX* Get();
    /** @brief 遍历所有 TAP，对匹配项执行回调 */
    void        ForEach(std::function<void(ZM_TAP_CTX*)> fnaction,
                       std::function<bool(const ZM_TAP_CTX*)> fnmatches);

public:
    // --- 静态工具方法 ---
    /** @brief 为 TAP 设置超时定时器，到期未取消则触发 Drop */
    static void SetDropTimer(ZM_TAP_CTX* tap, int seconds = 0, int micros = 0, uint32_t drop_timeout_error_code = 0);
    /** @brief 释放请求端 bufferevent */
    static void FreeRequesterEnd(ZM_TAP_CTX* tap);
    /** @brief 设置请求可选数据（会释放旧数据） */
    static void SetOptData(ZM_TAP_CTX* tap, size_t optlen = 0, const BYTE* optdata = nullptr);
    /** @brief 设置回传数据（会释放旧数据） */
    static void SetOnBackData(ZM_TAP_CTX* tap, size_t dlen = 0, const void* data = nullptr);
    /** @brief 创建/重置 HTTP 请求对象（释放旧动态字符串后 Init） */
    static void RequestCreate(ZM_TAP_CTX* tap);
    /** @brief 设置 HTTP 请求的目标地址 */
    static void RequestSetAddress(ZM_TAP_CTX* tap, const char* dst_host, uint16_t dst_port);
    /** @brief 通过 libevent 的 evdns_getaddrinfo 发起异步 DNS 解析 */
    static void EvDnsResolve(ZM_TAP_CTX* tap, const char* hostname, uint16_t port);
    /** @brief 取消正在进行的 DNS 解析 */
    static void CancelResolve(ZM_TAP_CTX* tap);
    /** @brief 获取请求端 bufferevent 输入缓冲区中未读取数据长度 */
    static size_t RequesterInputLen(ZM_TAP_CTX* tap);
    /** @brief 从回传链末尾弹出一个 delegate（LIFO）
     *  @param remove true 同时从链上移除 */
    static ZmTapDelegate* BackChainPop(ZM_TAP_CTX* tap, bool remove = true);
    /** @brief 向回传链压入一个 delegate（去重） */
    static void BackChainPush(ZM_TAP_CTX* tap, ZmTapDelegate* delegate);

private:
    /** @brief 分配并初始化一个新的 ZM_TAP_CTX */
    ZM_TAP_CTX* CreateTap();
    /** @brief 释放 ZM_TAP_CTX 内存 */
    void        FreeTap(ZM_TAP_CTX* tap);

private:
    enum { TAP_ITEM_SIZE = sizeof(ZM_TAP_SLOT) };
    ZM_TAP_SLOT*             m_slots;         /** 槽位数组，扩容时整体重分配 */
    size_t                   m_count;         /** 当前已用槽位数 */
    size_t                   m_capacity;      /** 槽位数组总容量 */
    std::mutex               m_mutex;         /** 保护 slots/count/capacity/free_stack */
    std::atomic<uint64_t>    m_seq_counter;   /** 原子自增序号，用于生成唯一 seq_num */
    std::vector<ZM_TAP_CTX*> m_free_stack;    /** 空闲 TAP 栈，Get/Drop 均 O(1) */
};

/**
 * @brief TAP 协议委托基类，子类实现具体的协议处理逻辑
 *
 * 每个 delegate 可接管 TAP 连接上的读写事件、DNS 解析、错误处理等。
 * 生命周期：构造 → StartTapDelegate() → 事件循环 → StopTapDelegate() → 析构。
 */
class ZmTapDelegate
{
public:
    ZmTapDelegate();
    virtual ~ZmTapDelegate() {}

    // --- 生命周期 ---
    /** @brief 启动 delegate（由外部在 event_base 就绪后调用）
     *  @param evbase libevent 事件循环基
     *  @param mode   工作模式 */
    void StartTapDelegate(struct event_base* evbase, int mode = ZM_DELEGATE_MODE_NONE);
    /** @brief 停止 delegate，释放内部事件资源 */
    void StopTapDelegate();
    /** @brief 读取/设置 delegate 名称（调试用）
     *  @param name 传入非空时设置名称，始终返回当前名称 */
    char* TapDelegateName(const char* name = nullptr);
    /** @brief 设置 evdns_base，供 EvDnsResolve 使用 */
    void SetEvDns(evdns_base* evdnsbase);

    // --- 属性访问 ---
    int          TapDelegateMode()      { return m_mode; }
    event_base*  TapDelegateEventBase() { return m_evbase; }
    evdns_base*  TapDelegateEvdnsBase() { return m_evdnsbase; }

    // --- 网络事件回调（子类按需重写） ---
    /** @brief bufferevent 事件回调（EOF/ERROR/CONNECTED） */
    virtual void OnTapRequesterEvent(ZM_TAP_CTX* tap, struct bufferevent* requester_bev, short events) {}
    /** @brief 数据到达回调（纯虚，子类必须实现） */
    virtual void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) = 0;
    /** @brief 写就绪回调 */
    virtual void OnTapRequesterWrite(ZM_TAP_CTX* tap, struct bufferevent* requester_bev) {}
    /** @brief 新连接到达回调（纯虚，子类必须实现）
     *  @return true 接受连接，false 拒绝 */
    virtual bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd, struct sockaddr* address) = 0;
    /** @brief DNS 解析完成回调 */
    virtual void OnTapDnsResolved(ZM_TAP_CTX* tap, struct sockaddr_in6* sa6, socklen_t salen,
        const char* ipaddr, const char* hostname) {}
    /** @brief 错误处理回调
     *  @return true 已自行处理，false 交由 OnDropTimerCB 统一处理 */
    virtual bool OnTapError(ZM_TAP_CTX* tap, uint32_t error) { return false; }
    /** @brief TAP 即将被 Drop 时的通知 */
    virtual void OnTapDrop(ZM_TAP_CTX* tap) {}
    /** @brief delegate 内部事件回调（纯虚） */
    virtual void OnTapDelegateEvent(short what) = 0;
    /** @brief 回传数据到达回调 */
    virtual void OnTapDelegateBackEvent(ZM_TAP_CTX* tap) {}
    /** @brief 是否自行管理 bufferevent 回调
     *  @return true  OnTapRequesterAccept 中已设置回调，上层不再覆盖
     *          false 使用默认的 OnRequesterReadCB / OnRequesterEventCB */
    virtual bool IsCallbackSelfManaged() { return false; }
    /** @brief 获取关联的 TAP 上下文池
     *  @return ZmTapContext 指针，默认返回 nullptr（非 Hub 模式） */
    virtual ZmTapContext* TapContext() { return nullptr; }

protected:
    /** @brief 触发 delegate 内部事件 */
    void ActiveTapDelegateEvent(short what) { if (m_evdelegate) { event_active(m_evdelegate, what, 0); } }
    /** @brief 启动时回调，返回 true 则创建 m_evdelegate */
    virtual bool OnStartTap() { return false; }
    /** @brief 停止时回调 */
    virtual void OnStopTap() {}

protected:
    struct event_base* m_evbase;       /** libevent 事件循环基 */
    struct evdns_base* m_evdnsbase;    /** libevent DNS 解析基 */
    struct event*      m_evdelegate;   /** delegate 内部事件 */
    char               m_name[32];     /** delegate 名称（调试用） */
    int                m_mode;         /** 工作模式 */
};

/**
 * @brief 静态事件回调分发器，将 libevent C 回调桥接到 ZmTapDelegate 的 C++ 虚函数
 *
 * 所有方法均为 static，ctx 参数携带 ZM_TAP_CTX* 或 ZmTapDelegate*。
 */
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
