#include "zm_net_http_frontend_server.h"

#include <drogon/HttpResponse.h>

#include <cctype>
#include <filesystem>

#include <zm_util_logger.h>

using namespace drogon;
using std::string;

// ============================================================================
// 前端专属:静态文件 + 自定义 404(FR-20;仅前端有"文档根"概念)
// ============================================================================
void ZmHttpFrontendServer::SetDocumentRoot(const string& www)
{
    m_docRoot = www;
    app().setDocumentRoot(www);
}

void ZmHttpFrontendServer::SetNotFoundPage(const string& file)
{
    if (!std::filesystem::exists(file))
    {
        PUBLIC_LOG_ERROR("SetNotFoundPage: 页面不存在: {}", file);
        return;
    }
    app().setCustom404Page(HttpResponse::newFileResponse(file), true);
}

// ============================================================================
// 监听:useSSL(HTTPS 模式)→ 443(HTTPS)+ 80(HTTP,重定向);否则仅 80(HTTP)
//   端口由协议模式决定,故本面 SetupListeners 不收 port 参数;
//   证书全局经 ZmHttpServer::Init 的 Options 注入,本面只登记监听。
// ============================================================================
void ZmHttpFrontendServer::SetupListeners(const string& ip, bool useSSL,
                                          const string& rootPath)
{
    if (!rootPath.empty())
        SetRootPath(rootPath);
    if (useSSL)
    {
        AddListener(443, true, ip);
        AddListener(80, false, ip);     // HTTP 监听,用于 80→443 重定向(FR-22)
    }
    else
    {
        AddListener(80, false, ip);
    }
}

// ============================================================================
// 前端面可配置结构(业务层在 Open 前调用;平台层只给机制,不含具体路径)
// ============================================================================
void ZmHttpFrontendServer::AddSpaFallback(const string& prefix, const string& file)
{
    m_spaFallbacks.emplace_back(prefix, file);
}

void ZmHttpFrontendServer::AddDeniedPath(const string& prefix)
{
    m_deniedPaths.push_back(prefix);
}

// ============================================================================
// 结构 advice 链(FR-20/22/24;设计 §11.2 v2.1)
//   顺序:①80→443 重定向(HTTPS 模式)②per-port 门禁(/zimo/* 在前端端口=404)
//        ③SPA 回落(遍历业务层配置)④路径封禁(遍历业务层配置)
// ============================================================================
void ZmHttpFrontendServer::RegisterRoutes()
{
    RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                              AdviceChainCallback&& cc) {
        const uint16_t port = ZmHttpLocalPort(req);
        const bool isSelf = IsLocalPortIn(req);
        const string path(req->path());

        // ① 80→443 重定向(FR-22):仅 HTTPS 模式下、本地端口 80
        if (IsHttps() && port == 80)
        {
            // 以 Host 头重建 https 地址:去掉主机上的端口段,统一 :443
            // Host 兜底不用 127.0.0.1:通配绑定(0.0.0.0)时客户端经局域网 IP 访问会重定向错误,
            // 故优先用请求实际到达的本地地址(req->getLocalAddr),再回退本面绑定的首个 ip。
            string host = req->getHeader("Host");
            if (host.empty())
            {
                const string localIp = req->getLocalAddr().toIp();
                host = (localIp == "0.0.0.0" || localIp == "::")
                           ? (m_bindIps.empty() ? string("127.0.0.1") : m_bindIps[0])
                           : localIp;
            }
            size_t colon = host.rfind(':');
            if (colon != string::npos && host.find(']') == string::npos)
                host = host.substr(0, colon);
            string loc = "https://" + host + ":443" + path;
            string query = req->getQuery();
            if (!query.empty())
                loc += "?" + query;
            cb(HttpResponse::newRedirectionResponse(loc));
            return;
        }

        // ② per-port 门禁(设计 §4.4):请求到本面端口,但路径属其他面 → 404
        //   其他面根路径由宿主经 AddOtherRootPath 配置(如 /zimo/jrpc、/zimo/api)
        if (isSelf)
        {
            for (const string& other : m_otherRootPaths)
            {
                if (path == other || path.rfind(other + "/", 0) == 0)
                {
                    cb(HttpResponse::newNotFoundResponse());
                    return;
                }
            }
        }

        // ③ SPA 回落:遍历业务层配置(prefix → 页面文件);命中 → 返回文件
        if (isSelf)
        {
            for (const auto& [prefix, file] : m_spaFallbacks)
            {
                if (path == prefix || path.rfind(prefix + "/", 0) == 0)
                {
                    string page = m_docRoot;
                    if (!page.empty() && page.back() != '\\' && page.back() != '/')
                        page += "\\";
                    page += file;
                    if (std::filesystem::exists(page))
                    {
                        cb(HttpResponse::newFileResponse(page));
                    }
                    else
                    {
                        PUBLIC_LOG_ERROR("SPA 回落: 页面不存在: {}", page);
                        cb(HttpResponse::newNotFoundResponse());
                    }
                    return;
                }
            }
        }

        // ④ 路径封禁:遍历业务层配置(如物理存在的 /doc 目录 → 不可达)
        if (isSelf)
        {
            for (const string& denied : m_deniedPaths)
            {
                if (path == denied || path.rfind(denied + "/", 0) == 0)
                {
                    cb(HttpResponse::newNotFoundResponse());
                    return;
                }
            }
        }

        cc();
    });
}
