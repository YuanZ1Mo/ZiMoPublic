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
/// 静态目录条件请求说明(2026-09-03 源码核验):SetDocumentRoot 下的文件由
/// drogon StaticFileRouter 服务,If-Modified-Since → 304 为内建且默认开启
/// (StaticFileRouter.h:144),本面无需重复实现;缓存头策略见下游 SetStaticCachePolicy;
/// SPA 回落路径(AddSpaFallback)不经 StaticFileRouter,其条件请求由
/// RegisterRoutes 分支补齐(§16.1.1)。
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
// 静态资源缓存头策略(第二期 §16.1.2,A 档)
//   只加头不改状态:StaticFileRouter 的响应(无 body + Last-Modified 特征)
//   按扩展名下发 Cache-Control;不覆盖已有的头。浏览器与 304(§16.1.0)配合:
//   no-cache 态每次 IMS→304,长缓存态 max-age 命中本地零请求。
// ============================================================================
void ZmHttpFrontendServer::SetStaticCachePolicy(const ZmStaticCacheConfig& cfg)
{
    m_staticCache = cfg;
}

namespace
{
/// 取扩展名(含点),如 ".js";无扩展名返回空串
string ExtOfPath(const string& path)
{
    size_t dot = path.rfind('.');
    if (dot == string::npos || dot == path.size() - 1)
        return "";
    // 目录名中的点不算(取最后一段最后一个点前要有非斜杠)
    if (path.find('/', dot) != string::npos)
        return "";
    return path.substr(dot);
}
}  // namespace

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
                    // 条件请求(§16.1.1):单次 stat → 304 命中直接回;否则 200 带缓存头
                    auto meta = ZmHttpServer::FetchFileMeta(page);
                    if (!meta.found || meta.sizeFailed)
                    {
                        PUBLIC_LOG_ERROR("SPA 回落: 页面不可用(不存在/stat 失败): {}", page);
                        cb(HttpResponse::newNotFoundResponse());
                    }
                    else
                    {
                        auto ch = ZmHttpServer::CacheHeaders(meta);
                        if (auto notMod = ZmHttpServer::Maybe304(req, meta, ch))
                        {
                            cb(notMod);
                        }
                        else
                        {
                            auto resp = HttpResponse::newFileResponse(page);
                            resp->addHeader("Last-Modified", ch.first);
                            resp->addHeader("ETag", ch.second);
                            cb(resp);
                        }
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

    // ── 静态缓存头(§16.1.2,A 档):PreSending 纯加头(不改状态,无发送路径风险) ──
    if (!m_staticCache.defaultPolicy.empty())
    {
        RegisterPreSending([this](const HttpRequestPtr& req,
                                  const HttpResponsePtr& resp) {
            if (!IsLocalPortIn(req))
                return;
            // 静态响应特征:文件通道(带 Last-Modified;注:文件型 getBody() 亦非空,
            // 不能以 body 为空判定);已有 Cache-Control 不覆盖
            if (!resp->getHeader("Last-Modified").empty() &&
                resp->getHeader("Cache-Control").empty())
            {
                string policy = m_staticCache.defaultPolicy;
                string ext = ExtOfPath(req->path());
                if (!ext.empty())
                {
                    for (const auto& [e, v] : m_staticCache.extPolicy)
                    {
                        if (e == ext)
                        {
                            policy = v;
                            break;
                        }
                    }
                }
                if (!policy.empty())
                    resp->addHeader("Cache-Control", policy);
            }
        });
    }
}
