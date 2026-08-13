/**
 * @file zm_net_websocket_server.cpp
 * @brief WebSocket 服务端组件实现
 *
 * 关键事实(源自 vendored libevent ws.c):
 *   - evws_connection_free 内部最先触发 closecb(ws.c:92),随后立即释放连接 —
 *     closecb 内不得触碰 evws_connection;
 *   - closecb 不提供对端关闭码,业务 onClose 无 reason 参数;
 *   - evws 无协议层 ping/pong;ws_evhttp_error_cb 仅处理 EOF,不处理 TIMEOUT —
 *     心跳必须应用层兜底(Task 3);
 *   - evws_send 无条件 bufferevent_lock(ws.c:477),启用 BEV_OPT_THREADSAFE 后写路径线程安全。
 */

#include "zm_net_websocket_server.h"

#include "zm_net_req_loop.h"          // ZmReqLoop(PostToLoop/Release)
#include "zm_net_req_loop_pool.h"     // ZmReqLoopPool(Acquire/BudgetMs)
#include "../util/zm_util_logger.h"   // DEFAULT_LOG_WARN

#include <../libevent/include/event2/ws.h>
#include <../libevent/include/event2/bufferevent.h>   // BEV_OPT_THREADSAFE

// ============================================================================
// ZmWebSocketSession
// ============================================================================

void ZmWebSocketSession::SendText(const std::string& text)
{
    if (m_closed.load() || !m_conn)
        return;
    evws_send_text(m_conn, text.c_str());
    m_lastActiveMs.store(::GetTickCount64());
}

void ZmWebSocketSession::SendBinary(const BYTE* data, size_t len)
{
    if (m_closed.load() || !m_conn || len == 0)
        return;
    evws_send_binary(m_conn, (const char*)data, len);
    m_lastActiveMs.store(::GetTickCount64());
}

void ZmWebSocketSession::PostSendText(const std::string& text)
{
    ZmWebSocketServer* server = m_server;
    if (!server || !server->m_httpServer || m_closed.load())
        return;
    std::string copy = text;
    server->m_httpServer->PostWsReply([session = this, copy = std::move(copy)]() {
        // 循环线程执行:会话对象由僵尸表保证存活(服务器析构前);m_closed 在此复查
        if (!session->m_closed.load() && session->m_conn)
            session->SendText(copy);
    });
}

void ZmWebSocketSession::PostSendBinary(const BYTE* data, size_t len)
{
    ZmWebSocketServer* server = m_server;
    if (!server || !server->m_httpServer || m_closed.load() || len == 0)
        return;   // 空帧无意义;string(nullptr, 0) 为 UB
    std::string copy((const char*)data, len);
    server->m_httpServer->PostWsReply([session = this, copy = std::move(copy)]() {
        if (!session->m_closed.load() && session->m_conn)
            session->SendBinary((const BYTE*)copy.data(), copy.size());
    });
}

void ZmWebSocketSession::Close(uint16_t reason)
{
    if (m_closed.load() || !m_conn)
        return;
    m_closed.store(true);
    evws_close(m_conn, reason);
}

// ============================================================================
// ZmWebSocketServer
// ============================================================================

ZmWebSocketServer::ZmWebSocketServer(ZmHttpServer* httpServer)
    : m_httpServer(httpServer)
{
}

ZmWebSocketServer::~ZmWebSocketServer()
{
    // 僵尸表统一释放:此时 evhttp 已释放(closecb 全部触发完毕)、回包闭包已排空,
    // 无任何回调/闭包再引用会话对象
    for (ZmWebSocketSession* s : m_zombies)
        delete s;
    m_zombies.clear();

    // 心跳定时器(循环线程事件,在此释放;m_wsClosing 已置位,残留触发仅空转返回)
    if (m_heartbeatTimer)
    {
        event_free(m_heartbeatTimer);
        m_heartbeatTimer = nullptr;
    }
}

bool ZmWebSocketServer::Init()
{
    // 心跳定时器(应用层心跳:evws 无协议层 ping/pong,error_cb 不处理 TIMEOUT,须应用层兜底)
    if (m_heartbeatIntervalSec > 0 && m_httpServer->m_evbase)
    {
        m_heartbeatTimer = event_new(
            m_httpServer->m_evbase, -1, EV_PERSIST, OnHeartbeatCb, this);
        struct timeval tv;
        tv.tv_sec = m_heartbeatIntervalSec;
        tv.tv_usec = 0;
        event_add(m_heartbeatTimer, &tv);
    }
    return true;
}

void ZmWebSocketServer::Close()
{
    // ① 置 closing:此后 closecb(evhttp_free 阶段触发)仅置位 m_closed,不再碰表与业务回调
    m_wsClosing.store(true);

    // ② 断开全部活跃会话(发送关闭帧;evws 连接对象由 evhttp_free 兜底物理释放)
    // ★ m_closed 先行检查:与 closecb 竞态的会话(其 conn 已被 evws 释放)不得再触碰
    for (auto& kv : m_sessionsByPath)
    {
        for (ZmWebSocketSession* s : kv.second)
        {
            if (!s->m_closed.load() && s->m_conn)
            {
                s->m_closed.store(true);
                evws_close(s->m_conn, WS_CR_NORMAL);
            }
        }
    }

    // ③ 清活跃表:全部会话移入僵尸表,留待析构统一释放
    for (auto& kv : m_sessionsByPath)
    {
        for (ZmWebSocketSession* s : kv.second)
            m_zombies.push_back(s);
    }
    m_sessionsByPath.clear();
    m_sessionPath.clear();
    m_sessionCount = 0;
}

bool ZmWebSocketServer::TryUpgrade(struct evhttp_request* req)
{
    // ① 业务回调注册检查(未启用 → 走普通 HTTP 流程)
    if (!m_cbs.onMessage)
        return false;

    // ② Upgrade 头检查(无 Upgrade 头或非 websocket → 不是升级请求)
    struct evkeyvalq* in = evhttp_request_get_input_headers(req);
    const char* upgrade = evhttp_find_header(in, "Upgrade");
    if (!upgrade || evutil_ascii_strcasecmp(upgrade, "websocket") != 0)
        return false;

    // ③ 路径前缀检查(仅接受 /ws 与 /ws/*;不匹配 → 走普通流程)
    const char* uri = evhttp_request_get_uri(req);
    std::string fullUri = uri ? uri : "";
    std::string path = fullUri;
    size_t q = path.find('?');
    if (q != std::string::npos)
        path = path.substr(0, q);
    if (path != "/ws" && path.rfind("/ws/", 0) != 0)
        return false;

    // ④ 握手鉴权(可选钩子:false → 400 且不建会话;已处置,调用方不得再提交 doer)
    if (m_cbs.onAuth && !m_cbs.onAuth(fullUri))
    {
        evhttp_send_error(req, ZM_HTTP_STATUS_CODE_BAD_REQUEST, nullptr);
        return true;
    }

    // ⑤ 创建会话并接管(evws_new_session 内部校验 Connection/Sec-WebSocket-Key,
    //    并发送 101;失败时自行发 400 返回 NULL,请求已处置)
    ZmWebSocketSession* session = new ZmWebSocketSession();
    session->m_server = this;
    session->m_path = std::move(path);
    session->m_lastActiveMs.store(::GetTickCount64());

    struct evws_connection* conn =
        evws_new_session(req, OnMsgCb, session, BEV_OPT_THREADSAFE);
    if (!conn)
    {
        delete session;
        return true;   // 已处置(400 已发),调用方不得再提交 doer
    }
    session->m_conn = conn;
    evws_connection_set_closecb(conn, OnCloseCb, session);

    // ⑥ 入会话表
    m_sessionsByPath[session->m_path].insert(session);
    m_sessionPath[session] = session->m_path;
    ++m_sessionCount;

    // ⑦ 业务回调(循环线程,轻逻辑)
    if (m_cbs.onOpen)
        m_cbs.onOpen(session);
    return true;
}

void ZmWebSocketServer::BroadcastText(const std::string& prefix, const std::string& text)
{
    if (m_sessionPath.empty())
        return;
    // 前缀匹配(整段路径边界):target == prefix,或 target 以 prefix + "/" 开头
    for (auto& kv : m_sessionPath)
    {
        const std::string& target = kv.second;
        bool hit = (target == prefix)
            || (target.size() > prefix.size()
                && target.compare(0, prefix.size(), prefix) == 0
                && target[prefix.size()] == '/');
        if (!hit)
            continue;
        ZmWebSocketSession* s = kv.first;
        if (!s->m_closed.load() && s->m_conn)
            s->SendText(text);
    }
}

// ============================================================================
// 回调胶水(C 函数指针 → 成员;全部在事件循环线程触发)
// ============================================================================

void ZmWebSocketServer::OnMsgCb(struct evws_connection* conn, int type,
                                const unsigned char* data, size_t len, void* arg)
{
    (void)conn;
    ZmWebSocketSession* session = static_cast<ZmWebSocketSession*>(arg);
    session->m_server->OnSessionMessage(session, type, data, len);
}

void ZmWebSocketServer::OnCloseCb(struct evws_connection* conn, void* arg)
{
    (void)conn;   // 回调内不得触碰 conn:返回后 evws_connection_free 立即释放(ws.c:88)
    ZmWebSocketSession* session = static_cast<ZmWebSocketSession*>(arg);
    session->m_server->OnSessionClosed(session);
}

void ZmWebSocketServer::OnSessionMessage(ZmWebSocketSession* s, int type,
                                         const BYTE* data, size_t len)
{
    s->m_lastActiveMs.store(::GetTickCount64());
    DispatchMessage(s, type, data, len);
}

// ============================================================================
// B 方案消息投递(设计文档 4.7):
//   evws 回调(循环线程) → m_threadPool 排队(非阻塞) → worker 线程 Acquire 业务 loop
//   (预算/断连放弃) → ZmReqLoop 线程业务回调 → 回包经 PostSendText 投递
// ============================================================================

void ZmWebSocketServer::DispatchMessage(ZmWebSocketSession* s, int type,
                                        const BYTE* data, size_t len)
{
    ZmHttpServer* http = m_httpServer;
    // 池未启用(裸 ZmHttpServer 未 EnableLoopPool):降级循环线程直调(测试/轻场景)
    if (!http || !http->m_threadPool || !http->m_reqLoopPool)
    {
        if (m_cbs.onMessage)
        {
            try
            {
                m_cbs.onMessage(s, type, data, len);
            }
            catch (...)
            {
                // 业务异常隔离:异常不得穿透事件循环(否则 loop 线程崩溃)
                DEFAULT_LOG_ERROR("[WsServer] 业务回调异常(直调路径) path={}", s->Path());
            }
        }
        return;
    }
    std::string payload((const char*)data, len);   // 文本帧不保证 \0 结尾,按 len 拷贝
    int64_t arriveMs = (int64_t)::GetTickCount64(); // deadline 起点(同 doer SetArriveTime)
    http->m_threadPool->Submit([this, s, type, payload = std::move(payload), arriveMs]() {
        WorkerMessage(s, type, payload, arriveMs);
    }, "WsMsg");
}

void ZmWebSocketServer::WorkerMessage(ZmWebSocketSession* s, int type,
                                      const std::string& payload, int64_t arriveMs)
{
    ZmReqLoopPool* pool = m_httpServer->m_reqLoopPool;
    if (!pool)
        return;

    // 排队上限 = 剩余预算(公式同 AcquireLoop, zm_net_http.cpp:1420);abort = 会话关闭标志
    int64_t remainMs = arriveMs + (int64_t)pool->BudgetMs() - (int64_t)::GetTickCount64();
    if (remainMs <= 0)
        remainMs = 1;
    ZmReqLoop* loop = pool->Acquire((int)remainMs, &s->m_closed);
    if (!loop)
    {
        // 排队超时 / 会话已关闭:丢弃(设计决策:不回错误信封)
        DEFAULT_LOG_WARN("[WsServer] 消息丢弃:排队超时或会话已关闭 path={}", s->Path());
        return;
    }

    // 投递业务回调(loop 被本消息独占期间 epoch 不变,不会被误丢弃);业务完成后回池
    // ★ try/catch:业务异常不得穿透事件循环,且 Release 必须恒执行(否则池容量泄漏)
    loop->PostToLoop([this, s, type, payload](ZmReqLoop* loop) {
        if (!s->m_closed.load())
        {
            if (m_cbs.onMessage)
            {
                try
                {
                    m_cbs.onMessage(s, type, (const BYTE*)payload.data(), payload.size());
                }
                catch (...)
                {
                    DEFAULT_LOG_ERROR("[WsServer] 业务回调异常 path={}", s->Path());
                }
            }
        }
        loop->Release();
    });
}

// ============================================================================
// 广播
// ============================================================================

void ZmWebSocketServer::PostBroadcastText(const std::string& prefix, const std::string& text)
{
    ZmHttpServer* http = m_httpServer;
    if (!http)
        return;
    std::string p = prefix;
    std::string t = text;
    http->PostWsReply([this, p = std::move(p), t = std::move(t)]() {
        if (!m_wsClosing.load())
            BroadcastText(p, t);
    });
}

// ============================================================================
// 心跳(应用层)
// ============================================================================

void ZmWebSocketServer::OnHeartbeatCb(evutil_socket_t fd, short what, void* arg)
{
    (void)fd;
    (void)what;
    ZmWebSocketServer* server = static_cast<ZmWebSocketServer*>(arg);
    server->HeartbeatScan();
}

void ZmWebSocketServer::HeartbeatScan()
{
    if (m_wsClosing.load() || m_heartbeatTimeoutSec <= 0 || m_sessionsByPath.empty())
        return;
    int64_t now = (int64_t)::GetTickCount64();
    int64_t timeoutMs = (int64_t)m_heartbeatTimeoutSec * 1000;
    for (auto& kv : m_sessionsByPath)
    {
        for (ZmWebSocketSession* s : kv.second)
        {
            if (s->m_closed.load())
                continue;
            if (now - s->LastActiveMs() > timeoutMs)
            {
                DEFAULT_LOG_WARN("[WsServer] 心跳超时,关闭会话 path={}", s->Path());
                s->Close(WS_CR_NORMAL);   // 关闭帧发出;closecb 后续触发清理(同循环线程,无竞态)
            }
        }
    }
}

void ZmWebSocketServer::OnSessionClosed(ZmWebSocketSession* s)
{
    s->m_closed.store(true);
    if (m_wsClosing.load())
        return;   // 服务器关停中:会话已在 Close() 统一处理
    DetachSession(s);
    if (m_cbs.onClose)
        m_cbs.onClose(s);
}

void ZmWebSocketServer::DetachSession(ZmWebSocketSession* s)
{
    // ★ 关停中二次检查:OnSessionClosed 的 m_wsClosing 检查存在"检查后、摘除前"窗口,
    //   此时 Close()(主线程)正独占遍历会话表 —— 命中则跳过,由 Close() 统一收编僵尸表
    if (m_wsClosing.load())
        return;
    auto it = m_sessionPath.find(s);
    if (it == m_sessionPath.end())
        return;   // 已摘除(幂等)
    const std::string& path = it->second;
    auto pit = m_sessionsByPath.find(path);
    if (pit != m_sessionsByPath.end())
    {
        pit->second.erase(s);
        if (pit->second.empty())
            m_sessionsByPath.erase(pit);
    }
    m_sessionPath.erase(it);
    m_zombies.push_back(s);
    --m_sessionCount;
}
