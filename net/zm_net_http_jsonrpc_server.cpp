#include "zm_net_http_jsonrpc_server.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>


#include "zm_util_json.h"   // ZMJSON = nlohmann::ordered_json

#include <zm_util_logger.h>

using namespace drogon;
using std::string;

// ── 构造 ──
ZmHttpJsonRpcServer::ZmHttpJsonRpcServer() = default;

// ── 监听 ──
/**
 * @brief 登记监听端口与协议（rootPath 非空时一并声明归属）
 * @param port      监听端口（默认 39440）
 * @param ip        绑定地址
 * @param useSSL    是否启用 TLS
 * @param rootPath  业务根路径（空 = 不声明归属）
 */
void ZmHttpJsonRpcServer::SetupListeners(uint16_t port, const string& ip, bool useSSL,
                                         const string& rootPath)
{
    if (!rootPath.empty())
        SetRootPath(rootPath);           // 先声明归属：门禁与路由注册都依赖它
    AddListener(port, useSSL, ip);
}

// ── method 注册 ──
/**
 * @brief 注册 method 处理器（同名覆盖并记 WARN）
 * @param name     方法名（对应请求的 method 字段）
 * @param handler  处理器：成功写 result 返回 true；失败写 error 返回 false
 */
void ZmHttpJsonRpcServer::RegisterMethod(const string& name, ZmJrpcMethodHandler handler)
{
    if (m_methods.count(name))
        PUBLIC_LOG_WARN("ZmHttpJsonRpcServer::RegisterMethod 重复覆盖: {}", name);
    m_methods[name] = std::move(handler);   // 同名覆盖：后注册者生效
}

// ── 结构路由（per-port 门禁 + 协议 handler，自动挂到 GetRootPath()） ──
/**
 * @brief 注册本面 per-port 门禁 advice 与唯一的 POST 协议 handler
 */
void ZmHttpJsonRpcServer::RegisterRoutes()
{
    // 根路径未设置 → 拒绝注册：空路径会把 handler 挂到全局（未知路由），
    // 且门禁失去判据；此处仅告警，门禁与 handler 均不生效
    if (m_rootPath.empty())
    {
        PUBLIC_LOG_ERROR("ZmHttpJsonRpcServer::RegisterRoutes: 未设置根路径(SetRootPath),JRPC 门禁与 handler 均未生效");
        return;
    }

    // 门禁：本地端口在本面 && 路径非本面根路由（且非共享路径）→ 404
    RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                              AdviceChainCallback&& cc) {
        GateAdvice(req, std::move(cb), std::move(cc));
    });

    // ── JRPC 协议 handler（平台内建校验 + 信封，对齐 JrpcRequestReadCB 骨架） ──
    // Dispatch 直接产出 ZMJSON 信封（id→jsonrpc→result|error 构造序），HTTP 恒 200
    RegisterCoro(m_rootPath, HttpMethod::Post,
        [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            ZMJSON rsp = Dispatch(ParseRequest(req));
            co_return ZmHttpServer::JsonResponse(200, rsp);
        });
}

// ── 请求解析（直接用 ZMJSON 解析 body，键序为报文序；不经 drogon/jsoncpp） ──
/**
 * @brief 解析请求体为 ZMJSON
 * @param req  请求
 * @return 解析结果；失败返回 null（由 Dispatch 转 -32700）
 */
ZMJSON ZmHttpJsonRpcServer::ParseRequest(const HttpRequestPtr& req)
{
    try
    {
        return ZMJSON::parse(string(req->getBody()));
    }
    catch (...)
    {
        return ZMJSON(nullptr);   // 解析失败(对外表现为 Parse error)
    }
}

// ── 协议校验与分发（JSON-RPC 2.0；返回完整信封） ──
//   校验顺序（兼容语义 + RFC 规范）：
//     -32700  Parse error：JSON 解析失败
//     -32600  Invalid Request：非对象、jsonrpc!="2.0"、缺 id、method 缺失/非字符串
//     -32602  Invalid params：params 存在但非 object/array
//     -32601  Method not found：未知 method
//     -32603  Internal error：handler 内部异常（或业务返回 false 未给 code）
namespace
{
/**
 * @brief 按 JSON-RPC 2.0 构造错误信封的 error 节点
 * @param code     错误码（-32700/-32600/-32601/-32602/-32603）
 * @param message  错误描述
 * @return error 对象（ZMJSON 保序，构造序 code→message）
 */
ZMJSON MakeJsonrpcError(int code, const string& message)
{
    return ZMJSON{{"code", code}, {"message", message}};
}

}  // namespace

/**
 * @brief 协议校验并分发到注册的 method 处理器
 * @param req  解析后的请求 JSON
 * @return 响应信封（id → jsonrpc → result 或 error）
 */
ZMJSON ZmHttpJsonRpcServer::Dispatch(const ZMJSON& req)
{
    // 信封 ZMJSON 直构：构造序 id → jsonrpc → (result|error)
    // id 先占位 null，后续校验分支按需改值（改值不调整键序）；jsonrpc 恒 "2.0"
    ZMJSON rsp;
    rsp["id"] = nullptr;
    rsp["jsonrpc"] = "2.0";

    // -32700 Parse error（ParseRequest 的 null 标记）
    if (req.is_null())
    {
        rsp["error"] = MakeJsonrpcError(-32700, "Parse error");
        return rsp;
    }

    // 必须是对象（批量数组、标量一律视为无效请求）
    if (!req.is_object())
    {
        rsp["error"] = MakeJsonrpcError(-32600, "Invalid Request");
        return rsp;
    }

    // id 必须存在（兼容语义：客户端恒带 id；缺 id 视为无效请求，不按通知处理）
    if (!req.contains("id") ||
        !(req["id"].is_number_integer() || req["id"].is_string() || req["id"].is_null()))
    {
        rsp["error"] = MakeJsonrpcError(-32600, "Invalid Request, Missing id Parameter");
        return rsp;
    }
    rsp["id"] = req["id"];               // 校验通过后才回填 id（保证键序不变）

    // -32600 jsonrpc 版本
    if (!req.contains("jsonrpc") || !req["jsonrpc"].is_string() ||
        req["jsonrpc"].get<string>() != "2.0")
    {
        rsp["error"] = MakeJsonrpcError(-32600, "Invalid Request, Missing jrpc Parameter");
        return rsp;
    }

    // -32600 method
    if (!req.contains("method") || !req["method"].is_string())
    {
        rsp["error"] = MakeJsonrpcError(-32600, "Invalid Request, Missing method Parameter");
        return rsp;
    }
    string method = req["method"].get<string>();

    // -32602 params
    if (req.contains("params") && !(req["params"].is_object() || req["params"].is_array()))
    {
        rsp["error"] = MakeJsonrpcError(-32602, "Invalid params, Missing params Parameter");
        return rsp;
    }
    ZMJSON params = req.contains("params") ? req["params"] : ZMJSON::object();

    // -32601 method 分发
    auto it = m_methods.find(method);
    if (it == m_methods.end())
    {
        rsp["error"] = MakeJsonrpcError(-32601, "Method not found: " + method);
        return rsp;
    }

    // 业务处理（异常兜底 -32603）
    ZMJSON result, error;
    try
    {
        if (!(it->second)(params, result, error))
        {
            // 业务自报失败：给了 error 对象就用它，否则回通用 -32603
            rsp["error"] = error.is_object()
                               ? error
                               : MakeJsonrpcError(-32603, "Internal error");
            return rsp;
        }
    }
    catch (const std::exception& e)
    {
        // 业务异常不上抛：统一转 -32603，避免协程栈上抛出导致连接被断
        PUBLIC_LOG_ERROR("JRPC method '{}' 异常: {}", method, e.what());
        rsp["error"] = MakeJsonrpcError(-32603, "Internal error");
        return rsp;
    }
    rsp["result"] = result;
    return rsp;
}

// ── 门禁实现 ──
/**
 * @brief per-port 门禁：本面端口且路径非本面 root（且非共享路径）→ 404
 *
 * JRPC 面只有一个入口端点，故这里**只认精确路径**（对照 RESTful 面的前缀放行）。
 *
 * @param req 请求
 * @param cb  短路回调（非本面路径时回 404）
 * @param cc  放行回调
 */
void ZmHttpJsonRpcServer::GateAdvice(const HttpRequestPtr& req, AdviceCallback&& cb,
                                     AdviceChainCallback&& cc)
{
    if (!IsLocalPortIn(req))
    {
        cc();                            // 非本面端口：交给其他面的 advice
        return;
    }
    string path(req->path());
    // 根路径可经 SetRootPath 自定义（默认 /zimo/jrpc；空 = 关闭本面门禁）
    // 共享路径（/ping 等）经归属表判定，不在各面硬编码
    if (m_rootPath.empty() || ZmHttpServer::IsSharedPath(path) || path == m_rootPath)
    {
        cc();                            // 命中本面入口或共享路径：放行
        return;
    }
    cb(HttpResponse::newNotFoundResponse());
}


