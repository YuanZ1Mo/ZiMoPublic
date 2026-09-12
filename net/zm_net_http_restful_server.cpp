#include "zm_net_http_restful_server.h"

using namespace drogon;
using std::string;

// ── 监听 ──
/**
 * @brief 登记监听端口与协议（rootPath 非空时一并声明归属）
 * @param port      监听端口（默认 39441）
 * @param ip        绑定地址
 * @param useSSL    是否启用 TLS
 * @param rootPath  业务根路径（空 = 不声明归属）
 */
void ZmHttpRestfulServer::SetupListeners(uint16_t port, const string& ip, bool useSSL,
                                         const string& rootPath)
{
    if (!rootPath.empty())
        SetRootPath(rootPath);           // 先声明归属：门禁判定以其为前提
    AddListener(port, useSSL, ip);
}

// ── 结构 advice（per-port 门禁） ──
/**
 * @brief 注册本面 per-port 门禁 advice（业务路由不在本面注册）
 */
void ZmHttpRestfulServer::RegisterRoutes()
{
    RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                              AdviceChainCallback&& cc) {
        GateAdvice(req, std::move(cb), std::move(cc));
    });
}

/**
 * @brief per-port 门禁：本面端口且路径不在本面 root 前缀下（且非共享路径）→ 404
 *
 * 本面是全部子路径的入口（业务路由多而散），故按前缀匹配放行；
 * JRPC 面是单一 POST 端点，那里只认精确路径。
 *
 * @param req 请求
 * @param cb  短路回调（非本面路径时回 404）
 * @param cc  放行回调
 */
void ZmHttpRestfulServer::GateAdvice(const HttpRequestPtr& req, AdviceCallback&& cb,
                                     AdviceChainCallback&& cc)
{
    if (!IsLocalPortIn(req))
    {
        cc();                            // 非本面端口：交给其他面的 advice
        return;
    }
    string path(req->path());
    // 根路径可经 SetRootPath 自定义（默认 /zimo/api；空 = 关闭本面门禁）
    // 共享路径（/ping 等）经归属表判定，不在各面硬编码
    if (m_rootPath.empty() || ZmHttpServer::IsSharedPath(path) || path == m_rootPath ||
        path.rfind(m_rootPath + "/", 0) == 0)
    {
        cc();                            // 命中本面 root 或共享路径：放行
        return;
    }
    cb(HttpResponse::newNotFoundResponse());
}
