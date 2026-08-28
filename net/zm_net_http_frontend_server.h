#ifndef ZM_NET_HTTP_FRONTEND_SERVER
#define ZM_NET_HTTP_FRONTEND_SERVER

/**
 * @file zm_net_http_frontend_server.h
 * @brief 前端服务器面(80/443):静态 + SPA 回落 + 404 + 页面路由 + 80→443 重定向(HTTPS 模式)
 *
 * 设计:ZiMoService docs/designs/2026-08-30-drogon-httpserver-base-design.md
 */


// ============================================================================
// ZmHttpFrontendServer:前端服务器面(80/443)
//  静态文件 + SPA 回落 + 404 + 页面别名 handler + /share 302
//  结构 advice 链(注册顺序):80→443 重定向 → per-port 门禁 → SPA 回落 → /doc 封禁
//  (页面别名 handler 由业务层(ServicePortal)经 RegisterCoro 注册,本面只挂结构 advice)
//  设计:docs/designs/2026-08-30-drogon-httpserver-base-design.md §11.2
// ============================================================================

#include "zm_net_http_server.h"

#include <string>
#include <utility>
#include <vector>

class ZmHttpFrontendServer : public ZmHttpServer
{
public:
    ZmHttpFrontendServer() = default;

    /// 监听配置(前端端口固定由协议模式决定,故不需 port 参数;签名与 JRPC/RESTful 面形态一致)
    /// @param useSSL  HTTPS 模式 → 挂 443(HTTPS)+80(HTTP 重定向);false → 仅 80(HTTP)
    /// @param rootPath 业务根路径(前端为静态服务,传空 = 关闭门禁)
    void SetupListeners(const std::string& ip = "0.0.0.0",
                        bool useSSL = false,
                        const std::string& rootPath = "");

    // ── 前端面可配置结构(业务层在 Open 前调用;平台层只给机制,不含具体路径) ──
    /// 注册 SPA 回落:请求命中 prefix(含子路径)时返回 file(相对文档根的页面)。
    /// 平台层不硬编码任何页面路径,由业务层按自身页面结构配置。
    void AddSpaFallback(const std::string& prefix, const std::string& file);
    /// 注册路径封禁:请求命中 prefix(含子路径)一律 404(如物理存在的 /doc 目录)。
    void AddDeniedPath(const std::string& prefix);

    // ── 前端专属:静态文件 + 自定义 404(FR-20;仅前端有"文档根"概念) ──
    /// 设置静态文档根(前端静态服务;app().setDocumentRoot 全局,仅前端面使用)
    void SetDocumentRoot(const std::string& www);
    const std::string& GetDocumentRoot() const { return m_docRoot; }
    /// 自定义 404 页(仅前端;SPA 回落请用 AddSpaFallback,勿用 setImplicitPage)
    void SetNotFoundPage(const std::string& file);

    /// 其他面的业务根路径(前端门禁用于拒绝外来前缀;由宿主配置时调用)
    void AddOtherRootPath(const std::string& path)
    {
        if (!path.empty())
            m_otherRootPaths.push_back(path);
    }

protected:
    void RegisterRoutes() override;   // 结构 advice:重定向/门禁 + 遍历可配置的 SPA 回落与封禁
private:
    std::vector<std::pair<std::string, std::string>> m_spaFallbacks;  // {prefix, file}
    std::vector<std::string> m_deniedPaths;                           // 封禁前缀
    std::string m_docRoot;               // SetDocumentRoot 缓存(构造页面绝对路径用)
    std::vector<std::string> m_otherRootPaths;  // 其他面根路径(前端门禁拒绝外来前缀)
};


#endif /* ZM_NET_HTTP_FRONTEND_SERVER */
