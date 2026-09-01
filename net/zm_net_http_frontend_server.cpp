#include "zm_net_http_frontend_server.h"

#include <drogon/HttpResponse.h>

#include <cctype>
#include <filesystem>

#include <zm_util_logger.h>

using namespace drogon;
using std::string;

// ============================================================================
// 构造:redirectOnly 标记(一对象一端口,v2.5)
// ============================================================================
ZmHttpFrontendServer::ZmHttpFrontendServer(bool redirectOnly)
    : m_redirectOnly(redirectOnly)
{
}

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
    if (m_redirectOnly)
    {
        // 重定向专用实例:恒 80(HTTP),仅负责 301 → 443(FR-22)
        AddListener(80, false, ip);
    }
    else if (useSSL)
    {
        AddListener(443, true, ip);      // HTTPS 模式:完整前端挂 443
    }
    else
    {
        AddListener(80, false, ip);      // 无证书:完整前端挂 80(HTTP)
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
// 结构 advice 链(FR-20/22/24;设计 §11.2 v2.5)
//   重定向专用实例:仅 80→443 重定向(FR-22)
//   完整前端实例:①per-port 门禁 ②SPA 回落(遍历业务层配置)③路径封禁
// ============================================================================
void ZmHttpFrontendServer::RegisterRoutes()
{
    // ── 重定向专用实例:仅注册 80→443 重定向(FR-22),无门禁/SPA/封禁 ──
    if (m_redirectOnly)
    {
        RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                                  AdviceChainCallback&& cc) {
            if (!IsLocalPortIn(req))
            {
                cc();
                return;
            }
            // 以 Host 头重建 https 地址:去掉主机上的端口段,统一 :443
            // Host 兜底不用 127.0.0.1:通配绑定(0.0.0.0)时客户端经局域网 IP 访问会重定向错误,
            // 故优先用请求实际到达的本地地址(req->getLocalAddr),再回退本面绑定的 ip。
            string host = req->getHeader("Host");
            if (host.empty())
            {
                const string localIp = req->getLocalAddr().toIp();
                host = (localIp == "0.0.0.0" || localIp == "::")
                           ? (m_listener.ip.empty() ? string("127.0.0.1") : m_listener.ip)
                           : localIp;
            }
            size_t colon = host.rfind(':');
            if (colon != string::npos && host.find(']') == string::npos)
                host = host.substr(0, colon);
            string loc = "https://" + host + ":443" + string(req->path());
            string query = req->getQuery();
            if (!query.empty())
                loc += "?" + query;
            cb(HttpResponse::newRedirectionResponse(loc));
        });
        return;
    }

    // ── 完整前端实例:门禁 → SPA 回落 → 路径封禁 ──
    RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                              AdviceChainCallback&& cc) {
        const uint16_t port = ZmHttpLocalPort(req);
        const bool isSelf = IsLocalPortIn(req);
        const string path(req->path());

        // ① per-port 门禁(设计 §4.4):请求到本面端口,但路径属其他面 → 404
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

        // ② SPA 回落:遍历业务层配置(prefix → 页面文件);命中 → 返回文件
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

        // ③ 路径封禁:遍历业务层配置(如物理存在的 /doc 目录 → 不可达)
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
