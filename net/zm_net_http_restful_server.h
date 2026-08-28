#ifndef ZM_NET_HTTP_RESTFUL_SERVER
#define ZM_NET_HTTP_RESTFUL_SERVER

/**
 * @file zm_net_http_restful_server.h
 * @brief 业务 API 服务器面(39441 /zimo/api):业务路由/WS/CORS 由业务层经基类接口注册
 *
 * 设计:ZiMoService docs/designs/2026-08-30-drogon-httpserver-base-design.md
 */


// ============================================================================
// ZmHttpRestfulServer:业务 API 服务器面(39441 /zimo/api)
//  本面只挂结构 advice(per-port 门禁,设计 §4.4);业务路由/WS/CORS 由业务层
//  (ServicePortal)经基类接口注册。
//  设计:docs/designs/2026-08-30-drogon-httpserver-base-design.md §11.4
// ============================================================================

#include "zm_net_http_server.h"

class ZmHttpRestfulServer : public ZmHttpServer
{
public:
    ZmHttpRestfulServer() = default;

    /// 端口与协议(默认 39441;useSSL=true 时经全局证书启用 HTTPS,证书由 ZmHttpServer::Init 的 Options 注入)
    /// @param rootPath 业务根路径(空 = 关闭门禁;默认 URI 由 Manager 传入,平台层不依赖服务宏)
    void SetupListeners(uint16_t port = 39441, const std::string& ip = "0.0.0.0",
                        bool useSSL = false,
                        const std::string& rootPath = "");

protected:
    void RegisterRoutes() override;   // 仅 per-port 门禁
};


#endif /* ZM_NET_HTTP_RESTFUL_SERVER */
