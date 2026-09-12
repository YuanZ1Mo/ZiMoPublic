#ifndef ZM_NET_HTTP_RESTFUL_SERVER
#define ZM_NET_HTTP_RESTFUL_SERVER

/**
 * @file zm_net_http_restful_server.h
 * @brief 业务 API 服务器面（39441 /zimo/api）：业务路由/WS/CORS 由业务层经基类接口注册
 *
 * 设计：ZiMoService docs/designs/2026-08-30-drogon-httpserver-base-design.md
 */


// ============================================================================
// ZmHttpRestfulServer：业务 API 服务器面（39441 /zimo/api）
//  本面只挂结构 advice（per-port 门禁）；业务路由/WS/CORS 由业务层
//  （ServicePortal）经基类接口注册。
// ============================================================================

#include "zm_net_http_server.h"

/**
 * @brief 业务 API 服务器面（默认 39441 /zimo/api）
 */
class ZmHttpRestfulServer : public ZmHttpServer
{
public:
    /**
     * @brief 默认构造（无内建路由；路由与 advice 全由业务层注册）
     */
    ZmHttpRestfulServer() = default;

    // ── 监听 ──
    /**
     * @brief 登记监听端口与协议（一对象一端口）
     *
     * useSSL=true 时经全局证书启用 HTTPS，证书由 ZmHttpServer::Init 的 Options
     * 注入（空 = 回落 HTTP）。
     *
     * @param port      监听端口（默认 39441）
     * @param ip        绑定地址
     * @param useSSL    是否启用 TLS
     * @param rootPath  业务根路径（空 = 关闭门禁；默认 URI 由 Manager 传入，
     *                  平台层不依赖服务宏）
     */
    void SetupListeners(uint16_t port = 39441, const std::string& ip = "0.0.0.0",
                        bool useSSL = false,
                        const std::string& rootPath = "");

protected:
    // ── 结构 advice（per-port 门禁） ──
    /**
     * @brief 仅注册 per-port 门禁（业务路由由业务层经基类接口注册）
     */
    void RegisterRoutes() override;

private:
    /// @brief per-port 门禁实现：本面端口且路径不在本面 root 前缀下（且非共享路径）→ 404
    void GateAdvice(const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
                    drogon::AdviceChainCallback&& cc);
};


#endif /* ZM_NET_HTTP_RESTFUL_SERVER */
