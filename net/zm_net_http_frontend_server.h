#ifndef ZM_NET_HTTP_FRONTEND_SERVER
#define ZM_NET_HTTP_FRONTEND_SERVER

/**
 * @file zm_net_http_frontend_server.h
 * @brief 前端服务器面（80/443）：静态 + 自定义 404 + 页面封禁 + 80→443 重定向（HTTPS 模式）
 *
 * 设计：ZiMoService docs/designs/2026-08-30-drogon-httpserver-base-design.md
 */


// ============================================================================
// ZmHttpFrontendServer：前端服务器面（80/443）
//  静态文件 + 404 + 页面封禁（docroot 策略）
//  结构 advice 链（注册顺序）：80→443 重定向（独立实例）→ per-port 门禁（归属表推导）
//  → 路径封禁；页面/SPA 路由与页面响应由业务层 advice（module_gate）承载，响应经平台
//  FileResponse 助手下发（条件请求 304 同源）。
// ============================================================================

#include "zm_net_http_server.h"

#include <string>
#include <utility>
#include <vector>

class ZmHttpFrontendServer : public ZmHttpServer
{
public:
    // ── 构造 ──
    /**
     * @brief 构造前端面实例
     *
     * @param redirectOnly  true = 仅 80→443 重定向专用实例（HTTPS 模式的 80 监听）；
     *                      false = 完整前端实例（443 HTTPS 或 80 HTTP）。
     *                      一对象一端口：前端面在 HTTPS 模式由两个实例组成
     */
    explicit ZmHttpFrontendServer(bool redirectOnly = false);

    // ── 监听 ──
    /**
     * @brief 登记本面监听（端口由协议模式 + redirectOnly 决定，故不收 port 参数）
     *
     * 签名与 JRPC/RESTful 面形态一致，便于统一编排。
     *
     * @param ip        绑定地址
     * @param useSSL    完整面：true → 443（HTTPS），false → 80（HTTP）；
     *                  重定向面：恒 80（HTTP）
     * @param rootPath  完整面传 "/" 声明**兜底归属**（未被其他面认领的路径归前端：
     *                  页面路由/SPA/静态文件；其他面 root 下的路径由门禁自动 404，
     *                  ）；传空 = 不声明归属（前端门禁退化为仅按
     *                  AddOtherRootPath 前缀拒绝）；重定向面忽略（须传空）
     */
    void SetupListeners(const std::string& ip = "0.0.0.0",
                        bool useSSL = false,
                        const std::string& rootPath = "");

    /**
     * @brief 是否为重定向专用实例
     * @return true 仅做 80→443 重定向，不承载页面路由
     */
    bool IsRedirectOnly() const { return m_redirectOnly; }

    // ── 前端面可配置结构（业务层在 Open 前调用；平台层只给机制，不含具体路径） ──
    /**
     * @brief 注册路径封禁：请求命中 prefix（含子路径）一律 404
     *
     * 平台层的 **docroot 访问策略**（物理存在的敏感目录不可达）。注：当前发布目录
     * （publish 产物 frontend/）无此类目录，故暂无调用者；页面/SPA 策略属业务层
     * （module_gate），不在此。
     *
     * @param prefix  封禁前缀
     */
    void AddDeniedPath(const std::string& prefix);

    /**
     * @brief 兜底口子：额外拒绝的外来前缀（归属表之外）
     *
     * 通常无需调用 —— 其他面 SetRootPath 声明后前门禁已自动拒绝其前缀。
     * @param path  额外拒绝的前缀（空串忽略）
     */
    void AddOtherRootPath(const std::string& path)
    {
        if (!path.empty())
            m_otherRootPaths.push_back(path);
    }

    // ── 前端专属：静态文件 + 自定义 404（仅前端有"文档根"概念） ──
    /**
     * @brief 设置静态文档根（前端静态服务）
     *
     * app().setDocumentRoot 为全局配置，仅前端面使用。
     * @param www  文档根目录（绝对路径）
     */
    void SetDocumentRoot(const std::string& www);
    /**
     * @brief 取静态文档根
     * @return 文档根目录（未设置时为空串）
     */
    const std::string& GetDocumentRoot() const { return m_docRoot; }
    /**
     * @brief 设置自定义 404 页（仅前端）
     *
     * 勿用 setImplicitPage —— 其语义为"目录解析"而非页面回落。
     * @param file  404 页面文件路径（UTF-8 编码；不存在则记 ERROR 并忽略）
     */
    void SetNotFoundPage(const std::string& file);

    // ── 静态资源缓存策略（Open 前调用） ──
    /**
     * @brief 静态资源缓存策略（发布层两态）
     */
    struct ZmStaticCacheConfig
    {
        /// 默认（再校验态），如 "public, max-age=0, must-revalidate"
        std::string defaultPolicy;
        /// 按扩展名的长缓存策略，如 {".js", "public, max-age=31536000, immutable"}
        std::vector<std::pair<std::string, std::string>> extPolicy;
    };
    /**
     * @brief 应用静态资源缓存策略
     *
     * 无指纹资源走再校验态（每次 IMS→304），指纹化资源长缓存态（命中本地零请求）；
     * 实现为"纯加头"的 PreSending advice（静态响应特征判定，不改响应状态，
     * 无发送路径风险）。
     *
     * @param cfg  缓存策略（defaultPolicy 为空 = 关闭该 advice）
     */
    void SetStaticCachePolicy(const ZmStaticCacheConfig& cfg);

protected:
    // ── 结构 advice 链 ──
    /**
     * @brief 结构 advice：重定向（仅重定向实例）/ per-port 门禁 + 路径封禁
     */
    void RegisterRoutes() override;
private:
    // ── 结构 advice 实现 ──
    /// @brief 80→443 重定向实现（仅重定向专用实例）
    void RedirectAdvice(const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
                        drogon::AdviceChainCallback&& cc);
    /// @brief per-port 门禁实现：归属其他面或封禁前缀的路径 → 404
    void GateAdvice(const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
                    drogon::AdviceChainCallback&& cc);
    /// @brief 静态资源 Cache-Control 实现（按扩展名，只加头）
    void CacheHeaderAdvice(const drogon::HttpRequestPtr& req,
                           const drogon::HttpResponsePtr& resp);
    bool m_redirectOnly = false;                 ///< 重定向专用实例标记
    std::vector<std::string> m_deniedPaths;      ///< 封禁前缀
    std::string m_docRoot;                       ///< SetDocumentRoot 缓存（构造页面绝对路径用）
    ZmStaticCacheConfig m_staticCache;           ///< 静态缓存头策略（defaultPolicy 空 = 关闭）
    std::vector<std::string> m_otherRootPaths;   ///< 兜底：归属表之外额外拒绝的前缀（空表亦正确）
};


#endif /* ZM_NET_HTTP_FRONTEND_SERVER */
