#ifndef ZM_NET_HTTP_JSONRPC_SERVER
#define ZM_NET_HTTP_JSONRPC_SERVER

/**
 * @file zm_net_http_jsonrpc_server.h
 * @brief JSON-RPC 2.0 服务器面（39440）
 *
 * 平台内建 JSON-RPC 2.0 协议校验与信封（语义沿用 ZmJsonRpcServer /
 * JrpcRequestReadCB）：
 *   - 单 handler 内部完成协议解析与校验（解析失败/格式错误/未知 method/非法 params）
 *   - 业务层经 RegisterMethod 注册 method 处理器；本面默认不注册任何 method
 *   - 信封单列（不复用 REST 错误格式）；HTTP 恒 200，错误见信封 error.code
 *
 * 设计：ZiMoService docs/designs/2026-08-30-drogon-httpserver-base-design.md
 */

#include "zm_net_http_server.h"

/**
 * @brief JSON-RPC 2.0 服务器面（默认 39440）
 */
class ZmHttpJsonRpcServer : public ZmHttpServer
{
public:
    /// JRPC method 处理器（业务层，项目标准类型 ZMJSON）：入参 params（object/array），
    /// 成功写 result 返回 true；失败写 error{code,message} 返回 false（默认 code=-32603）
    using ZmJrpcMethodHandler = std::function<bool(const ZMJSON& params,
                                                   ZMJSON& result,
                                                   ZMJSON& error)>;

    // ── 构造 ──
    /// @brief 构造实例（默认不注册任何 method;业务经 RegisterMethod 注册）
    ZmHttpJsonRpcServer();

    // ── 监听 ──
    /**
     * @brief 登记监听端口与协议（一对象一端口）
     *
     * useSSL=true 时经全局证书启用 HTTPS，证书由 ZmHttpServer::Init 的 Options
     * 注入（空 = 回落 HTTP）。
     *
     * @param port      监听端口（默认 39440）
     * @param ip        绑定地址
     * @param useSSL    是否启用 TLS
     * @param rootPath  业务根路径（空 = 关闭门禁；默认 URI 由 Manager 传入，
     *                  平台层不依赖服务宏）
     */
    void SetupListeners(uint16_t port = 39440, const std::string& ip = "0.0.0.0",
                        bool useSSL = false,
                        const std::string& rootPath = "");

    // ── method 注册 ──
    /**
     * @brief 注册 method 处理器（启动期，首个 Open 前调用）
     *
     * 重复注册覆盖旧处理器并记 WARN。处理器在工作线程之外、事件循环上被调用。
     * 首个 Open 之后调用会被拒绝并记 ERROR（运行期 method 表只读，读取侧无锁）。
     *
     * @param name     方法名（对应请求的 method 字段）
     * @param handler  处理器：成功写 result 并返回 true；失败写 error 并返回 false
     *
     * @example
     *   srv.RegisterMethod("user.get", [](const ZMJSON& params, ZMJSON& result,
     *                                     ZMJSON& error) {
     *       result["id"] = 42;
     *       return true;
     *   });
     */
    void RegisterMethod(const std::string& name, ZmJrpcMethodHandler handler);

protected:
    // ── 结构路由（per-port 门禁 + 协议 handler，自动挂到 GetRootPath()） ──
    /**
     * @brief per-port 门禁 + JRPC 协议 handler（自动注册于 GetRootPath()）
     */
    void RegisterRoutes() override;

private:
    // ── 请求解析 ──
    /**
     * @brief 请求体解析（ZMJSON 直接从 body 解析，nlohmann 保序）
     * @param req  请求
     * @return 解析出的 JSON；解析失败返回 null（对外表现为 Parse error）
     */
    static ZMJSON ParseRequest(const drogon::HttpRequestPtr& req);
    // ── 协议校验与分发 ──
    /**
     * @brief 协议校验与分发核心（返回完整 ZMJSON 信封）
     *
     * 校验顺序与错误码见 JSON-RPC 2.0（-32700/-32600/-32601/-32602/-32603）。
     * @param req  解析后的请求 JSON
     * @return 响应信封（id → jsonrpc → result 或 error，恒 HTTP 200）
     */
    ZMJSON Dispatch(const ZMJSON& req);

    // ── 门禁实现 ──
    /// @brief per-port 门禁实现：本面端口且路径非本面 root（且非共享路径）→ 404
    void GateAdvice(const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
                    drogon::AdviceChainCallback&& cc);

    /// 方法名 → 处理器（Phase1 只写、运行期只读；RegisterMethod 有 run 后拒绝守卫
    /// 保证写入只发生在启动期，故无需加锁）
    std::map<std::string, ZmJrpcMethodHandler> m_methods;
};

#endif /* ZM_NET_HTTP_JSONRPC_SERVER */
