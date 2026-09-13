#include "zm_net_http_frontend_server.h"

#include "../util/zm_util_logger.h"
#include "../util/zm_util_str.h"   // ZmString::UTF8_To_Unicode(路径 UTF-8 → wide 转换)

#include <../drogon/include/drogon/HttpResponse.h>

#include <cctype>
#include <filesystem>

using namespace drogon;
using std::string;

// ── 构造 ──
/**
 * @brief 构造前端面实例
 * @param redirectOnly  true = 仅 80→443 重定向专用实例；false = 完整前端实例
 */
ZmHttpFrontendServer::ZmHttpFrontendServer(bool redirectOnly)
    : m_redirectOnly(redirectOnly)
{
}

// ── 监听 ──
//   端口由协议模式决定，故本面 SetupListeners 不收 port 参数；
//   证书全局经 ZmHttpServer::Init 的 Options 注入，本面只登记监听。
/**
 * @brief 按协议模式登记本面监听（一对象一端口）
 * @param ip        绑定地址
 * @param useSSL    完整面：true → 443（HTTPS），false → 80（HTTP）
 * @param rootPath  非空则一并声明本面归属（"/" = 兜底归属）
 */
void ZmHttpFrontendServer::SetupListeners(const string& ip, bool useSSL,
                                          const string& rootPath)
{
    if (!rootPath.empty())
        SetRootPath(rootPath);           // 先声明归属：门禁判定以其为前提
    if (m_redirectOnly)
    {
        // 重定向专用实例：恒 80（HTTP），仅负责 301 → 443
        AddListener(80, false, ip);
    }
    else if (useSSL)
    {
        AddListener(443, true, ip);      // HTTPS 模式：完整前端挂 443
    }
    else
    {
        AddListener(80, false, ip);      // 无证书：完整前端挂 80（HTTP）
    }
}

// ── 前端面可配置结构 ──
/**
 * @brief 追加一条封禁前缀（命中即 404，遍历见 RegisterRoutes 的路径封禁段）
 * @param prefix  封禁前缀
 */
void ZmHttpFrontendServer::AddDeniedPath(const string& prefix)
{
    m_deniedPaths.push_back(prefix);
}

// ── 前端专属：静态文件 + 自定义 404 ──
/**
 * @brief 设置静态文档根，并同步给 drogon 的 StaticFileRouter
 *
 * 静态目录的条件请求说明：SetDocumentRoot 下的文件由 drogon StaticFileRouter 服务，
 * If-Modified-Since → 304 为内建且默认开启（StaticFileRouter.h:144），本面无需重复
 * 实现；缓存头策略见 SetStaticCachePolicy；业务页面响应（module_gate）经平台
 * FileResponse 助手，条件请求（304）同源。
 *
 * @param docRoot  文档根目录
 */
void ZmHttpFrontendServer::SetDocumentRoot(const string& docRoot)
{
    m_docRoot = docRoot;                     // 缓存一份：业务/本面据此拼页面绝对路径
    app().setDocumentRoot(docRoot);          // 交给框架的静态文件路由服务
}

/**
 * @brief 设置自定义 404 页（校验存在性后再交给框架）
 * @param file  404 页面文件路径
 */
void ZmHttpFrontendServer::SetNotFoundPage(const string& file)
{
    // filesystem 的窄串按 ANSI 码页解码,UTF-8 路径必错 → 先 UTF-8 → wide 再进 filesystem
    // (与基类 FetchFileMeta 同款;drogon 读文件自身经 toNativePath 转 wide,故此处校验通过即可)
    const std::wstring wfile = ZmString::UTF8_To_Unicode(file);
    if (wfile.empty() && !file.empty())
    {
        PUBLIC_LOG_ERROR("SetNotFoundPage: 路径转换失败: {}", file);
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(wfile), ec) || ec)
    {
        // 页面缺失时不接管：保持框架默认 404，避免把坏路径设成 404 页
        PUBLIC_LOG_ERROR("SetNotFoundPage: 页面不存在: {}", file);
        return;
    }
    app().setCustom404Page(HttpResponse::newFileResponse(file), true);
}

// ── 静态资源缓存策略 ──
//   只加头不改状态：StaticFileRouter 的响应（带 Last-Modified 特征）按扩展名
//   下发 Cache-Control，不覆盖已有的头。与 304配合：no-cache 态每次
//   IMS→304，长缓存态 max-age 命中本地零请求。
/**
 * @brief 保存静态缓存策略（Open 前调用；RegisterRoutes 据此决定是否注册 advice）
 * @param cfg  缓存策略（defaultPolicy 为空 = 不注册 PreSending advice）
 */
void ZmHttpFrontendServer::SetStaticCachePolicy(const ZmStaticCacheConfig& cfg)
{
    m_staticCache = cfg;
}

// ── 结构 advice 链 ──
//   重定向专用实例：仅 80→443 重定向
//   完整前端实例：①per-port 门禁（归属表推导）②路径封禁；页面/SPA 由业务层 advice 承载
/**
 * @brief 注册本面结构 advice（重定向 or 门禁 + 封禁 + 静态缓存头）
 */
void ZmHttpFrontendServer::RegisterRoutes()
{
    // ── 重定向专用实例：仅注册 80→443 重定向，无门禁/SPA/封禁 ──
    if (m_redirectOnly)
    {
        RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                                  AdviceChainCallback&& cc) {
            RedirectAdvice(req, std::move(cb), std::move(cc));
        });
        return;
    }

    // ── 完整前端实例：门禁 → 路径封禁（页面/SPA 由业务层 advice 承载） ──
    RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                              AdviceChainCallback&& cc) {
        GateAdvice(req, std::move(cb), std::move(cc));
    });

    // ── 静态缓存头：PreSending 纯加头（不改状态，无发送路径风险） ──
    if (!m_staticCache.defaultPolicy.empty())
    {
        RegisterPreSending([this](const HttpRequestPtr& req,
                                  const HttpResponsePtr& resp) {
            CacheHeaderAdvice(req, resp);
        });
    }
}

/**
 * @brief 80→443 重定向（仅重定向专用实例注册）
 *
 * 用 Host 头重建 https 地址并剥离端口；Host 缺失时回退请求实际到达的本地地址，
 * 再回退本面绑定地址（通配绑定时用请求地址才对，不能用固定 127.0.0.1）。
 *
 * @param req 请求
 * @param cb  短路回调（回 302）
 * @param cc  放行回调
 */
void ZmHttpFrontendServer::RedirectAdvice(const HttpRequestPtr& req,
                                         drogon::AdviceCallback&& cb,
                                         drogon::AdviceChainCallback&& cc)
{

    if (!IsLocalPortIn(req))
    {
        cc();                        // 非本面端口：不干预，交给后续链路
        return;
    }
    // 以 Host 头重建 https 地址：去掉主机上的端口段，统一 :443
    // Host 缺省兜底不用 127.0.0.1：通配绑定（0.0.0.0）时客户端经局域网 IP 访问会
    // 重定向错误，故优先用请求实际到达的本地地址（req->getLocalAddr），
    // 再回退本面绑定的 ip。
    string host = req->getHeader("Host");
    if (host.empty())
    {
        const string localIp = req->getLocalAddr().toIp();
        host = (localIp == "0.0.0.0" || localIp == "::")
                   ? (m_listener.ip.empty() ? string("127.0.0.1") : m_listener.ip)
                   : localIp;
    }
    // 端口剥离：IPv6 形如 "[::1]:80" —— 冒号须在 ']' 之后才是端口分隔符
    // （否则拼出 "https://[::1]:80:443" 这类非法地址）
    size_t colon = host.rfind(':');
    size_t bracket = host.find(']');
    if (colon != string::npos &&
        (bracket == string::npos || colon > bracket))
    {
        host = host.substr(0, colon);
    }
    string loc = "https://" + host + ":443" + string(req->path());
    string query = req->getQuery();   // 保留原查询串（重定向不丢参数）
    if (!query.empty())
        loc += "?" + query;
    cb(HttpResponse::newRedirectionResponse(loc));
}

/**
 * @brief per-port 门禁：归属其他面的路径 → 404；封禁前缀 → 404
 *
 * 归属由进程级归属表推导（其他面 SetRootPath 声明即自动生效），
 * 共享路径归属本兜底面故正常放行；页面路由/SPA 由业务层 advice 承载。
 *
 * @param req 请求
 * @param cb  短路回调（外来路径或封禁路径回 404）
 * @param cc  放行回调
 */
void ZmHttpFrontendServer::GateAdvice(const HttpRequestPtr& req,
                                     drogon::AdviceCallback&& cb,
                                     drogon::AdviceChainCallback&& cc)
{

    const bool isSelf = IsLocalPortIn(req);
    const string path(req->path());

    // ① per-port 门禁：请求到本面端口，但路径归属其他面 → 404
    //   归属由进程级归属表推导：其他面 SetRootPath 声明即自动生效，
    //   宿主无需再手工维护前缀名单（AddOtherRootPath 保留为兜底口子）。
    //   共享路径（/ping）归属本兜底面 → 正常放行；本面未声明 root 时不做归属拒绝。
    if (isSelf)
    {
        const ZmHttpServer* owner = ZmHttpServer::LookupOwner(path);
        if (owner && owner != this)
        {
            cb(HttpResponse::newNotFoundResponse());
            return;
        }
        for (const string& other : m_otherRootPaths)
        {
            // 前缀匹配须带 "/" 边界，避免 "/app2" 被 "/app" 误伤
            if (path == other || path.rfind(other + "/", 0) == 0)
            {
                cb(HttpResponse::newNotFoundResponse());
                return;
            }
        }
    }

    // ② 路径封禁：遍历业务层配置（如物理存在的敏感目录 → 不可达）
    //   注：当前发布目录（frontend/）无此类目录，故无调用者；保留为平台层
    //   docroot 访问策略口子（平台拥有 docroot，业务拥有页面策略）
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

    cc();                                // 未被本面拒绝：交给后续 advice/静态路由
}

namespace
{
/**
 * @brief 取路径扩展名（含点），如 ".js"
 * @param path  请求路径
 * @return 扩展名；无扩展名（无点或以点结尾）返回空串
 */
string ExtOfPath(const string& path)
{
    size_t dot = path.rfind('.');
    if (dot == string::npos || dot == path.size() - 1)
        return "";                       // 无点 / 以点结尾：视为无扩展名
    // 目录名中的点不算（最后一个点之后若还有 '/'，说明点落在目录段内）
    if (path.find('/', dot) != string::npos)
        return "";
    return path.substr(dot);
}
}  // namespace

/**
 * @brief 静态资源缓存头：按扩展名下发 Cache-Control（只加头不改状态）
 *
 * 静态响应特征 = 带 Last-Modified（文件型响应的 body 亦非空，不能用 body 判空）；
 * 已有 Cache-Control 时不覆盖。
 *
 * @param req  请求（取路径扩展名）
 * @param resp 即将发送的响应（就地追加 Cache-Control）
 */
void ZmHttpFrontendServer::CacheHeaderAdvice(const HttpRequestPtr& req,
                                            const drogon::HttpResponsePtr& resp)
{

    if (!IsLocalPortIn(req))
        return;                      // 只管本面端口产生的响应
    // 静态响应特征：文件通道（带 Last-Modified；注：文件型 getBody() 亦非空，
    // 不能以 body 为空判定）；已有 Cache-Control 不覆盖
    if (!resp->getHeader("Last-Modified").empty() &&
        resp->getHeader("Cache-Control").empty())
    {
        string policy = m_staticCache.defaultPolicy;   // 默认走再校验态
        string ext = ExtOfPath(req->path());
        if (!ext.empty())
        {
            for (const auto& [e, v] : m_staticCache.extPolicy)
            {
                if (e == ext)        // 命中扩展名：改走长缓存态
                {
                    policy = v;
                    break;
                }
            }
        }
        if (!policy.empty())
            resp->addHeader("Cache-Control", policy);
    }
}
