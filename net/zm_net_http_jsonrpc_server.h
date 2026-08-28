#ifndef ZM_NET_HTTP_JSONRPC_SERVER
#define ZM_NET_HTTP_JSONRPC_SERVER

/**
 * @file zm_net_http_jsonrpc_server.h
 * @brief JSON-RPC 2.0 服务器面(39440)
 *
 * 平台内建 JSON-RPC 2.0 协议校验与信封(参考旧版 ZmJsonRpcServer / JrpcRequestReadCB):
 *   - 单 handler 内部完成协议解析与校验(解析失败/格式错误/未知 method/非法 params)
 *   - 业务层仅注册 method 处理器(RegisterMethod);ping 内建
 *   - 信封单列(不复用 REST 错误格式);HTTP 恒 200,错误见信封 error.code
 *
 * 设计:ZiMoService docs/designs/2026-08-30-drogon-httpserver-base-design.md §11.3
 */

#include "zm_net_http_server.h"

class ZmHttpJsonRpcServer : public ZmHttpServer
{
public:
    /// JRPC method 处理器(业务层,项目标准类型 ZMJSON):入参 params(object/array),
    /// 成功写 result 返回 true;失败写 error{code,message} 返回 false(默认 code=-32603)
    using ZmJrpcMethodHandler = std::function<bool(const ZMJSON& params,
                                                   ZMJSON& result,
                                                   ZMJSON& error)>;

    ZmHttpJsonRpcServer();

    /// 端口与协议(默认 39440;useSSL=true 时经全局证书启用 HTTPS,证书由 ZmHttpServer::Init 的 Options 注入)
    /// @param rootPath 业务根路径(空 = 关闭门禁;默认 URI 由 Manager 传入,平台层不依赖服务宏)
    void SetupListeners(uint16_t port = 39440, const std::string& ip = "0.0.0.0",
                        bool useSSL = false,
                        const std::string& rootPath = "");

    /// 注册 method(Phase1,首个 Open 前);重复注册覆盖并打 ERROR
    void RegisterMethod(const std::string& name, ZmJrpcMethodHandler handler);

protected:
    void RegisterRoutes() override;   // per-port 门禁 + JRPC 协议 handler(自动注册于 GetRootPath())

private:
    /// 请求体解析(ZMJSON 直接从 body 解析,nlohmann 保序;null=解析失败)
    static ZMJSON ParseRequest(const drogon::HttpRequestPtr& req);
    /// 协议校验与分发核心(返回完整 ZMJSON 信封;错误码见 JSON-RPC 2.0)
    ZMJSON Dispatch(const ZMJSON& req);

    std::map<std::string, ZmJrpcMethodHandler> m_methods;   // Phase1 只写运行只读,无需加锁
};

#endif /* ZM_NET_HTTP_JSONRPC_SERVER */
