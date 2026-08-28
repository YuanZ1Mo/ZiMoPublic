#include "zm_net_http_restful_server.h"

using namespace drogon;
using std::string;

// ============================================================================
// 监听:39441(HTTPS 模式与前端共享全局证书;无证书回落 HTTP)
// ============================================================================
void ZmHttpRestfulServer::SetupListeners(uint16_t port, const string& ip, bool useSSL,
                                         const string& rootPath)
{
    if (!rootPath.empty())
        SetRootPath(rootPath);
    AddListener(port, useSSL, ip);
}

// ============================================================================
// 结构 advice:per-port 门禁——本地端口在本面 && 路径非 /zimo/api*(且非 /ping)→ 404
// ============================================================================
void ZmHttpRestfulServer::RegisterRoutes()
{
    RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                              AdviceChainCallback&& cc) {
        if (!IsLocalPortIn(req))
        {
            cc();
            return;
        }
        string path(req->path());
        // 根路径可经 SetRootPath 自定义(默认 /zimo/api;空 = 关闭本面门禁)
        if (m_rootPath.empty() || path == "/ping" || path == m_rootPath ||
            path.rfind(m_rootPath + "/", 0) == 0)
        {
            cc();
            return;
        }
        cb(HttpResponse::newNotFoundResponse());
    });
}
