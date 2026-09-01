#include "zm_net_http_jsonrpc_server.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>


#include "zm_util_json.h"   // ZMJSON = nlohmann::ordered_json

#include <zm_util_logger.h>

using namespace drogon;
using std::string;

namespace
{
/// 按 JSON-RPC 2.0 构造错误信封节点(error 字段;ZMJSON,构造序 code→message)
ZMJSON MakeJsonrpcError(int code, const string& message)
{
    return ZMJSON{{"code", code}, {"message", message}};
}

}  // namespace

// ============================================================================
// 构造:内建 ping(旧版语义:result.pong=true)
// ============================================================================
ZmHttpJsonRpcServer::ZmHttpJsonRpcServer()
{
    RegisterMethod("ping", [](const ZMJSON& params, ZMJSON& result,
                              ZMJSON& error) {
        (void)params;
        (void)error;
        result["pong"] = true;
        return true;
    });
}

// ============================================================================
// 监听:39440(HTTPS 模式与前端共享全局证书;无证书回落 HTTP)
// ============================================================================
void ZmHttpJsonRpcServer::SetupListeners(uint16_t port, const string& ip, bool useSSL,
                                         const string& rootPath)
{
    if (!rootPath.empty())
        SetRootPath(rootPath);
    AddListener(port, useSSL, ip);
}

// ============================================================================
// method 注册(业务层,Phase1)
// ============================================================================
void ZmHttpJsonRpcServer::RegisterMethod(const string& name, ZmJrpcMethodHandler handler)
{
    if (m_methods.count(name))
        PUBLIC_LOG_WARN("ZmHttpJsonRpcServer::RegisterMethod 重复覆盖: {}", name);
    m_methods[name] = std::move(handler);
}

// ============================================================================
// 结构路由:per-port 门禁 + JRPC 协议 handler(自动挂到 GetRootPath())
// ============================================================================
void ZmHttpJsonRpcServer::RegisterRoutes()
{
    // 门禁(设计 §4.4):本地端口在本面 && 路径非本面根路由(且非 /ping)→ 404
    RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                              AdviceChainCallback&& cc) {
        if (!IsLocalPortIn(req))
        {
            cc();
            return;
        }
        string path(req->path());
        // 根路径可经 SetRootPath 自定义(默认 /zimo/jrpc;空 = 关闭本面门禁)
        if (m_rootPath.empty() || path == m_rootPath || path == "/ping")
        {
            cc();
            return;
        }
        cb(HttpResponse::newNotFoundResponse());
    });

    // ── JRPC 协议 handler(平台内建校验 + 信封,参考旧版 JrpcRequestReadCB) ──
    // Dispatch 直接产出 ZMJSON 信封(id→jsonrpc→result|error 构造序),HTTP 恒 200
    RegisterCoro(m_rootPath, HttpMethod::Post,
        [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            ZMJSON rsp = Dispatch(ParseRequest(req));
            co_return ZmHttpServer::JsonResponse(200, rsp);
        });

    // 结构依赖:RegisterCoro 将于 rootPath 空时注册失败,防御提示
    if (m_rootPath.empty())
        PUBLIC_LOG_ERROR("ZmHttpJsonRpcServer::RegisterRoutes: 未设置根路径(SetRootPath),JRPC handler 未生效");
}

// ============================================================================
// 请求解析(直接从 body 用 ZMJSON 解析:JSON 全链路统一 ZMJSON,键序为报文序;
// 已不再依赖 drogon/jsoncpp 的 getJsonObject)
// ============================================================================
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

// ============================================================================
// 协议校验与分发(JSON-RPC 2.0;返回完整信封)
//   校验顺序(旧版语义 + RFC 规范):
//     -32700  Parse error:JSON 解析失败
//     -32600  Invalid Request:非对象、jsonrpc!="2.0"、缺 id、method 缺失/非字符串
//     -32602  Invalid params:params 存在但非 object/array
//     -32601  Method not found:未知 method
//     -32603  Internal error:handler 内部异常(或业务返回 false 未给 code)
// ============================================================================
ZMJSON ZmHttpJsonRpcServer::Dispatch(const ZMJSON& req)
{
    // 信封 ZMJSON 直构:构造序 id → jsonrpc → (result|error)
    // id 先占位 null,后续校验分支按需改值(改值不调整键序);jsonrpc 恒 "2.0"
    ZMJSON rsp;
    rsp["id"] = nullptr;
    rsp["jsonrpc"] = "2.0";

    // -32700 Parse error
    if (req.is_null())
    {
        rsp["error"] = MakeJsonrpcError(-32700, "Parse error");
        return rsp;
    }

    // 必须是对象
    if (!req.is_object())
    {
        rsp["error"] = MakeJsonrpcError(-32600, "Invalid Request");
        return rsp;
    }

    // id 必须存在(旧版语义:客户端恒带 id;缺 id 视为无效请求,不按通知处理)
    if (!req.contains("id") ||
        !(req["id"].is_number_integer() || req["id"].is_string() || req["id"].is_null()))
    {
        rsp["error"] = MakeJsonrpcError(-32600, "Invalid Request, Missing id Parameter");
        return rsp;
    }
    rsp["id"] = req["id"];

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

    // 业务处理(异常兜底 -32603)
    ZMJSON result, error;
    try
    {
        if (!(it->second)(params, result, error))
        {
            rsp["error"] = error.is_object()
                               ? error
                               : MakeJsonrpcError(-32603, "Internal error");
            return rsp;
        }
    }
    catch (const std::exception& e)
    {
        PUBLIC_LOG_ERROR("JRPC method '{}' 异常: {}", method, e.what());
        rsp["error"] = MakeJsonrpcError(-32603, "Internal error");
        return rsp;
    }
    rsp["result"] = result;
    return rsp;
}


