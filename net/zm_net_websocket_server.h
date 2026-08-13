/**
 * @file zm_net_websocket_server.h
 * @brief WebSocket 服务端组件(ZmHttpServer 内部成员)
 *
 * 基于 vendored libevent 的 evws 能力(ws.c 编入 event_extra),提供:
 *   - 升级分流:OnHttpRequestCB 一行 TryUpgrade(事件循环线程,接管后 request 已释放)
 *   - 会话管理:会话表(路径前缀分组)+ 僵尸表(会话指针永不悬垂)
 *   - B 方案业务投递:消息 → 服务器 m_threadPool → m_reqLoopPool(预算/断连放弃)
 *     → ZmReqLoop 业务回调 → 控制事件回包到循环线程 evws_send_text
 *   - 心跳(应用层):evws 无协议层 ping/pong,error_cb 不处理 TIMEOUT,须应用层兜底
 *   - 广播/统计
 *
 * 线程模型:evws 全部操作严格在事件循环线程(启用 BEV_OPT_THREADSAFE 作写路径双保险,
 * 但发送仍唯一经服务器 PostWsReply 投递到循环线程执行——锁解决不了连接生命周期竞态)。
 * 详见 docs/designs/2026-08-13-websocket-push-design.md
 */

#ifndef ZM_NET_WEBSOCKET_SERVER_H_
#define ZM_NET_WEBSOCKET_SERVER_H_

#include "zm_net_http.h"   // ZmHttpServer(友元访问内部)、BYTE

#include <event2/ws.h>

#include <stdint.h>
#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct evws_connection;

class ZmWebSocketServer;

/** @brief 一个 WebSocket 会话(封装 evws_connection;循环线程读写,m_closed 原子标志跨线程) */
class ZmWebSocketSession
{
public:
    /** @brief 升级时请求路径(用于按路径前缀广播) */
    const std::string& Path() const { return m_path; }

    /** @brief 最后活跃时间(心跳扫描用,跨线程可读) */
    int64_t LastActiveMs() const { return m_lastActiveMs.load(); }

    /** @brief 会话是否已关闭(断连放弃检查用,线程安全) */
    bool IsClosed() const { return m_closed.load(); }

    /** @brief 发送文本(仅事件循环线程内直接调用,零投递开销) */
    void SendText(const std::string& text);
    /** @brief 发送二进制(仅事件循环线程) */
    void SendBinary(const BYTE* data, size_t len);

    /** @brief 发送文本(任意线程可调;投递到循环线程,检查存活后发送) */
    void PostSendText(const std::string& text);
    /** @brief 发送二进制(任意线程可调,同 PostSendText 语义) */
    void PostSendBinary(const BYTE* data, size_t len);

    /** @brief 关闭会话(发送关闭帧;事件循环线程内调用,线程安全语义由 evws 锁保证) */
    void Close(uint16_t reason);

private:
    friend class ZmWebSocketServer;

    ZmWebSocketServer*      m_server = nullptr;    ///< 所属服务器(PostSend* 投递用)
    struct evws_connection* m_conn = nullptr;      ///< evws 连接(closecb 后即失效,勿再触碰)
    std::string             m_path;
    std::atomic<bool>       m_closed{false};       ///< closecb 置位 → Acquire 中止标志
    std::atomic<int64_t>    m_lastActiveMs{0};
};

/**
 * @brief WebSocket 业务回调(经 ZmHttpServer::SetWebSocketCallbacks 或
 *        ZmWebSocketServer::SetCallbacks 注册;服务器 Init() 之前设置)
 */
struct ZmWebSocketCallbacks
{
    /** @brief 连接建立(事件循环线程,轻逻辑,不得阻塞) */
    std::function<void(ZmWebSocketSession*)> onOpen;
    /** @brief 连接关闭(事件循环线程;libevent closecb 不提供关闭码) */
    std::function<void(ZmWebSocketSession*)> onClose;
    /**
     * @brief 握手鉴权(事件循环线程;返回 false → 400 且不建会话;缺省 = 放行)
     * @param uri 完整请求 URI(含 query,可携带 ?token=)
     */
    std::function<bool(const std::string& uri)> onAuth;
    /**
     * @brief 业务消息(ZmReqLoop 线程,与 RestfulRequestCB 同线程语义,可做重逻辑;
     *        服务器未启用业务 loop 池时降级为事件循环线程直调)
     * @param type WS_TEXT_FRAME(0x1) / WS_BINARY_FRAME(0x2)
     * @note 文本帧 data 不保证 \0 结尾,按 len 拷贝
     * @note 回包用 session->PostSendText(线程安全,投递到循环线程发送)
     */
    std::function<void(ZmWebSocketSession*, int type,
                       const BYTE* data, size_t len)> onMessage;
};

/** @brief WebSocket 会话管理器(ZmHttpServer 内部成员,生命周期由服务器 Init/Close 托管) */
class ZmWebSocketServer
{
public:

    explicit ZmWebSocketServer(ZmHttpServer* httpServer);
    ~ZmWebSocketServer();

    /** @brief 由 ZmHttpServer::Init 内部调用(成员生命周期托管) */
    bool Init();

    /** @brief 由 ZmHttpServer::Close 内部调用:置 closing 标志、断开全部会话、清活跃表
     *  @note 会话包装对象留待析构统一释放(在飞回包闭包/closecb 仍可安全引用) */
    void Close();

    /** @brief 配置业务回调(服务器 Init() 之前调用;onMessage 为空 = 不接受升级;
     *         通常经 ZmHttpServer::SetWebSocketCallbacks 直连设置) */
    void SetCallbacks(const ZmWebSocketCallbacks& cbs) { m_cbs = cbs; }

    /**
     * @brief 升级分流入口(事件循环线程,OnHttpRequestCB 调用)
     * @param req 升级请求(evhttp_request,仅回调期内有效)
     * @return true 已接管连接(request 已释放,调用方不得再提交 doer);
     *         false 视为普通请求走现有流程
     */
    bool TryUpgrade(struct evhttp_request* req);

    /** @brief 当前活跃会话数(循环线程读) */
    size_t SessionCount() const { return m_sessionCount; }

    /** @brief 按路径前缀广播文本(事件循环线程;prefix 匹配整段路径,如 "/ws/status" 覆盖其下子路径) */
    void BroadcastText(const std::string& prefix, const std::string& text);

    /** @brief 按路径前缀广播文本(任意线程可调;投递到循环线程执行) */
    void PostBroadcastText(const std::string& prefix, const std::string& text);

    /** @brief 心跳扫描周期(秒,0=禁用;默认 60;须在服务器 Init() 前设置) */
    void SetHeartbeatInterval(int seconds) { m_heartbeatIntervalSec = seconds; }
    /** @brief 无活跃超时(秒,0=禁用;默认 180;可随时设置,下次扫描生效) */
    void SetHeartbeatTimeout(int seconds) { m_heartbeatTimeoutSec = seconds; }

private:
    friend class ZmWebSocketSession;

    /** @brief evws 消息回调胶水(arg = ZmWebSocketSession*,C 函数指针 → 成员) */
    static void OnMsgCb(struct evws_connection* conn, int type,
                        const unsigned char* data, size_t len, void* arg);
    /** @brief evws 关闭回调胶水(在 evws_connection_free 内最先触发,回调内不得触碰 conn) */
    static void OnCloseCb(struct evws_connection* conn, void* arg);

    void OnSessionMessage(ZmWebSocketSession* s, int type, const BYTE* data, size_t len);
    void OnSessionClosed(ZmWebSocketSession* s);
    /** @brief 从活跃表摘除并入僵尸表(仅一次,closecb 触发) */
    void DetachSession(ZmWebSocketSession* s);

    // ── B 方案消息投递(详见设计文档 4.7)──
    /** @brief 循环线程:拷贝载荷 + 入队服务器 worker 池(非阻塞) */
    void DispatchMessage(ZmWebSocketSession* s, int type, const BYTE* data, size_t len);
    /** @brief worker 线程:Acquire 业务 loop(预算/断连放弃)→ 投递业务回调 → 回池 */
    void WorkerMessage(ZmWebSocketSession* s, int type,
                       const std::string& payload, int64_t arriveMs);

    // ── 心跳(应用层;evws 无 ping/pong,error_cb 不处理 TIMEOUT)──
    static void OnHeartbeatCb(evutil_socket_t fd, short what, void* arg);
    /** @brief 循环线程:扫描活跃会话,超时无活跃 → 主动关闭 */
    void HeartbeatScan();

    ZmHttpServer*         m_httpServer = nullptr;
    ZmWebSocketCallbacks  m_cbs;

    /** @brief 活跃会话:路径 → 集合(事件循环线程独享,无锁) */
    std::unordered_map<std::string, std::unordered_set<ZmWebSocketSession*>> m_sessionsByPath;
    /** @brief 会话 → 路径 反查(事件循环线程独享) */
    std::unordered_map<ZmWebSocketSession*, std::string> m_sessionPath;
    /** @brief 僵尸表:已关闭会话留待析构统一释放(保证业务持有的会话指针永不悬垂) */
    std::vector<ZmWebSocketSession*> m_zombies;
    size_t                    m_sessionCount = 0;
    std::atomic<bool>         m_wsClosing{false};   ///< 服务器关停中:closecb 只置位 m_closed,不碰表与回调

    struct event*             m_heartbeatTimer = nullptr;  ///< 心跳定时器(循环线程,析构释放)
    int                       m_heartbeatIntervalSec = 60;
    int                       m_heartbeatTimeoutSec = 180;
};

#endif // ZM_NET_WEBSOCKET_SERVER_H_
