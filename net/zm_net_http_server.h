#ifndef ZM_NET_HTTP_SERVER_H
#define ZM_NET_HTTP_SERVER_H

/**
 * @file zm_net_http_server.h
 * @brief Drogon 1.9.13 HTTP 服务器基类(三个服务器面公共底座)
 *
 * 说明:
 *  - 生命周期为**进程级静态**状态机(Uninit→Initialized→Opened→Closed):
 *      ZmHttpServer::Init(opts)  一次性注入全局运行参数/证书/全局 advice(/ping、访问日志、JSONP)
 *      ZmHttpServer::Open()      后台线程跑 app().run(),绑定失败 fail-fast
 *      ZmHttpServer::Close()     全局唯一关闭:app().quit()+join;幂等;Closed 为终态
 *    drogon app() 为全局单例且 run() 只能跑一次,故"关闭后不能再打开",
 *    不支持运行期单端口启停/热重启(见设计文档 §2.2)。
 *  - 派生面"结构上多实例、运行时单 app()":每个派生对象只负责"端口 + 路由登记"
 *    (AddListener/Setup/RegisterRoutes),全部须在 Open() 前完成;
 *    路由表/运行参数全局共享,路径前缀 + per-port 门禁区分(D2)。
 *  - 通用响应助手/文件传输(Range)/阻塞离核 RunOnPool 均在此基类。
 *
 * 设计:ZiMoService docs/designs/2026-08-30-drogon-httpserver-base-design.md
 */

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/RequestStream.h>
#include <drogon/utils/coroutine.h>
#include <drogon/WebSocketConnection.h>

#include <trantor/net/EventLoop.h>

#include <zm_util_thread.h>
#include <zm_util_json.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace drogon
{
class HttpFilterBase;
class HttpResponse;
}  // namespace drogon

// ----------------------------------------------------------------------------
// 公共类型
// ----------------------------------------------------------------------------

/// 协程 handler:业务层注册的请求回调形态(FR-05, D3)
/// 注:本捆绑 drogon 的 FunctionTraits 协程特化仅匹配"按值 HttpRequestPtr"形参,
/// 因此此处用按值传递(与 drogon 协程契约一致)。
using ZmHttpCoroHandler = std::function<
    drogon::Task<drogon::HttpResponsePtr>(drogon::HttpRequestPtr)>;

/// 方案乙(流式分块发送)行为参数(FR-12)
struct ZmHttpSendFileOptions
{
    size_t chunkSize      = 1 * 1024 * 1024;   ///< 分块粒度
    size_t interBlockMs   = 50;                ///< 块间定时器间隔(定时器链节流,内存有界);interBlockMs == 0 → 发完即调度(自适应,吞吐=排水速率,快客户端不设限)
    size_t watermarkBytes = 8 * 1024 * 1024;   ///< 严格水位目标(可选增强,默认不启用)
    int64_t stallAbortMs  = 120 * 1000;        ///< 对端停滞放弃阈值

    /// 进度回调(可选):(已发送, 总大小)
    std::function<void(uint64_t sent, uint64_t total)> onProgress;
};

/// 流式接收协程 handler(FR-15/路径 B):形参带 RequestStreamPtr 时被 drogon
/// FunctionTraits 判定为 stream-handler,框架自动注入流对象(数据逐块交付)
using ZmHttpStreamHandler = std::function<drogon::Task<drogon::HttpResponsePtr>(
    drogon::HttpRequestPtr, drogon::RequestStreamPtr)>;

/// 流式落盘参数(FR-15/SaveStreamToFile)
struct ZmHttpUploadFileOptions
{
    uint64_t maxBytes = 0;              ///< 实时兜底上限(块累加,超限停写并清理);0 = 不限制
    uint64_t progressIntervalMs = 100;  ///< 进度回调最小间隔(毫秒)
    std::function<void(uint64_t written, uint64_t total)> onProgress;  ///< (可选)进度回调
};

/// 请求到达的本地端口(字节交换等价 ntohs,避免 winsock 头顺序问题)
inline uint16_t ZmHttpLocalPort(const drogon::HttpRequestPtr& req)
{
    uint16_t p = req->getLocalAddr().portNetEndian();
    return static_cast<uint16_t>((p >> 8) | ((p & 0xFF) << 8));
}

/// 请求是否到达给定端口集合
inline bool ZmHttpLocalPortIn(const drogon::HttpRequestPtr& req,
                              const std::vector<uint16_t>& ports)
{
    uint16_t local = ZmHttpLocalPort(req);
    for (uint16_t p : ports)
    {
        if (local == p)
            return true;
    }
    return false;
}

class ZmHttpServer
{
public:
    virtual ~ZmHttpServer() = default;

    // ── 全局运行参数(FR-03;drogon app() 为全局单例,这些参数为进程级全局,
    //   经 Init 一次性注入,运行期不可改;含义/特殊值见 cpp 对应实现) ──
    struct Options
    {
        size_t threadNum = 0;              // 事件循环线程数(0 = 自动 = CPU 核数)
        size_t maxConnections = 8192;      // 最大连接数护栏(0 大概率不限,慎用)
        // 框架级请求体上限(1.9.13 对【全体请求含流式】强制,HttpRequestParser.cc:266,超限 413)。
        // 它是流式大上传(RegisterStreamCoro)唯一的框架兜底,勿调小于业务最大上传;
        // 非流式路由的提前拒绝由 nonStreamBodyLimit 承担。
        size_t clientMaxBodySize = 10ULL * 1024 * 1024 * 1024;
        /// 非流式路由请求体上限(PreRouting 按 Content-Length 预检,超限 413):
        /// 带 X-File-Size 声明的请求豁免(流式大上传路径,业务经 RegisterStreamCoro 的 maxBytes 兜底);
        /// 0 = 关闭预检。注:1.9.13 无 per-route 上限 API,此为 header 阶段全局闸门(路由无关)。
        size_t nonStreamBodyLimit = 256ULL * 1024 * 1024;
        /// 请求体内存缓冲上限(超过部分落临时文件,drogon 默认 64KB):
        /// "打满内存"的防护闸,内存增长有界(非流式大 body 的代价是磁盘/IO 而非内存)。
        size_t clientMaxMemoryBodySize = 64 * 1024;
        size_t maxConnectionsPerIP = 0;    // per-IP 连接数护栏(0 = 不限;⚠ 单机压测全部连接同源 IP,设值小于压测并发 → 拒连)
        size_t idleTimeoutSec = 90;        // keep-alive 空闲回收秒(0 = 关闭空闲回收)
        size_t keepaliveRequests = 0;      // 单连接请求数上限(0 = 不限次数回收)
        bool enableRequestStream = true;   // 上传流式落盘开关(业务依赖,保持 true)
        size_t workPoolSize = 8;           // 业务阻塞工作池线程数(切勿设 0)
        bool gzip = false;                 // 动态 gzip 压缩(>1024 非二进制 body,事件循环线程同步压)
        bool brotli = false;               // 动态 brotli 压缩(同上;CPU 更高,压缩率更优)
        bool gzipStatic = false;           // 静态 gzip:客户端支持时优先发同路径 <file>.gz 孪生(非现场压缩,无孪生照发原文件)
        bool brotliStatic = false;         // 静态 brotli:同上,孪生为 <file>.br(库已链接;无孪生照发原文件)
        bool ticketDisabled = false;       // TLS SessionTicket 禁用(安全项)
        std::string certFile;              // 全局证书(空 = 纯 HTTP)
        std::string keyFile;
        /// CORS 白名单(按 Origin 全串精确匹配,如 "https://www.example.com")。
        /// 空 = 不回显任何 CORS 头(跨域被浏览器拒绝,默认收敛);命中才回显 Origin +
        /// Allow-Credentials。跨域调用方在此登记;不含 "*"(与凭据头互斥)。
        std::vector<std::string> corsAllowedOrigins;
    };

    // ── 静态生命周期(进程级一次;状态机 Uninit→Initialized→Opened→Closed) ──
    /// 一次性初始化:应用全局参数 + 证书 + 全局 advice(/ping、访问日志、JSONP)。
    /// 只能调用一次;未 Init 就 Open 会报错。
    static bool Init(const Options& opts);
    /// 启动服务器:后台线程跑 app().run();须已 Init 且已登记至少一个监听。
    /// 绑定失败(端口占用等)在 300ms 内探测到并返回 false。
    static bool Open();
    /// 全局唯一关闭:app().quit() + join;幂等;Closed 为终态,之后不能再 Open。
    static void Close();
    static bool IsInitialized();
    static bool IsOpened();

    virtual bool IsHttps() const;            // 本面监听是否启用 TLS(一对象一端口,v2.5)
    /// 本面监听端口(未设置 = 0;一对象仅管理一个端口)
    virtual uint16_t GetPort() const { return m_listenerSet ? m_listener.port : 0; }
    /// 本面绑定地址(0.0.0.0/:: = 通配监听;未设置 = 空串)
    virtual std::string GetBindIp() const { return m_listenerSet ? m_listener.ip : ""; }

    // ── 结构路由注册(幂等;派生面注册内置路由/advice;须在 Open 前调用) ──
    virtual void Setup();

    // ── 监听配置(FR-02;证书全局经 Init 的 Options,此处不传 per-listener cert) ──
    virtual void AddListener(uint16_t port, bool useSSL = false,
                             const std::string& ip = "0.0.0.0",
                             bool useOldTLS = false,
                             const std::vector<std::pair<std::string, std::string>>& sslConfCmds = {});

    // ── TLS(FR-10/11;证书统一全局,保证热加载) ──
    /// 证书热重载(运行期唯一可热更新能力):reloadSSLFiles() 换内容不换路径。
    /// 证书为进程级全局(Init 注入),故本方法为静态能力。
    static bool ReloadCertificates();

    // ── 阻塞工作池(FR-19;单一静态共享池,默认 8,首次 RunOnPool 前生效) ──
    /// 切勿设 0:自研 ZmThreadPool 按该数创建工作线程,0 线程 = 任务永不执行、业务卡死。
    static void SetWorkPoolSize(size_t n);
    static size_t GetWorkPoolSize();

    // ── 协程路由(FR-05,带可选 filter 约束) ──
    virtual void RegisterCoro(const std::string& path, drogon::HttpMethod m,
                              ZmHttpCoroHandler h,
                              const std::vector<std::string>& filters = {});
    virtual void RegisterCoroWithDeadline(const std::string& path, drogon::HttpMethod m,
                                          ZmHttpCoroHandler h, size_t deadlineMs,
                                          const std::vector<std::string>& filters = {});

    // ── Filter(FR-06;std::function 适配为按名注册的 HttpFilter) ──
    virtual void AddFilter(const std::string& name,
                           const std::function<bool(const drogon::HttpRequestPtr&,
                                                    drogon::HttpResponsePtr&)>& f);

    // ── 全局横切 advice(FR-07;同名重复注册报 ERROR) ──
    virtual void RegisterPreRouting(std::function<void(const drogon::HttpRequestPtr&,
                                                       drogon::AdviceCallback&&,
                                                       drogon::AdviceChainCallback&&)> a);
    virtual void RegisterPostRouting(std::function<void(const drogon::HttpRequestPtr&,
                                                        drogon::AdviceCallback&&,
                                                        drogon::AdviceChainCallback&&)> a);
    virtual void RegisterPostHandling(std::function<void(const drogon::HttpRequestPtr&,
                                                         const drogon::HttpResponsePtr&)> a);
    virtual void RegisterPreSending(std::function<void(const drogon::HttpRequestPtr&,
                                                       const drogon::HttpResponsePtr&)> a);

    // ── 本面业务根路径(可自定义;门禁与派生面内置判断均以其为准) ──
    /// @param path 业务根路径,如 "/zimo/jrpc"、"/zimo/api";空串 = 关闭本面门禁
    virtual void SetRootPath(const std::string& path) { m_rootPath = path; }
    virtual const std::string& GetRootPath() const { return m_rootPath; }

    // ── 条件请求(FR-12 增强):Last-Modified/ETag → 304,white经 If-None-Match 优先 ──
    /// CORS 白名单查询(Init 注入 Options.corsAllowedOrigins,启动后只读):
    /// origin 在名单内 → 回显 CORS 头;空名单/不在 → 不发(浏览器拒绝跨域)。
    static bool IsCorsOriginAllowed(const std::string& origin);

    // ── 文件传输(FR-12,双路径;Range 由基类内部解析) ──
    /// 支持条件请求:Last-Modified(mtime)+ 强 ETag(size-mtime),If-None-Match
    /// 优先、If-Modified-Since 兜底 → 304(无 body);Range/206 语义同前。
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                 const std::string& attachmentName = "");
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileStreamCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                       const std::string& attachmentName,
                       const ZmHttpSendFileOptions& opts = {});
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileHybridCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                       const std::string& attachmentName,
                       size_t threshold = 2ULL * 1024 * 1024 * 1024,
                       const ZmHttpSendFileOptions& streamOpts = {});

    // ── 流式(FR-13;静态工厂:返回流式响应,调用方自行设头) ──
    using StreamCb = std::function<void(drogon::ResponseStreamPtr)>;
    static drogon::HttpResponsePtr MakeStreamResponse(StreamCb cb, bool disableKickoff = true);

    // ── 流式接收(FR-15,路径 B;与 Options.enableRequestStream 配套) ──
    /// @param maxBytes 路由级上传上限(0 = 不额外限制,全局 10GB 兜底);基类按
    ///        X-File-Size 声明自动早拒(超限 null reader + 413),并写入 req
    ///        attributes("ZmStreamMaxBytes")供落盘兜底取用
    virtual void RegisterStreamCoro(const std::string& path, drogon::HttpMethod m,
                                    ZmHttpStreamHandler h,
                                    const std::vector<std::string>& filters = {},
                                    uint64_t maxBytes = 0);

    /// 流式落盘(块到即写):成功返回 true;超限/失败置 *tooLarge 并清理半成品
    static drogon::Task<bool> SaveStreamToFile(drogon::RequestStreamPtr stream,
                                               const std::string& destPath,
                                               const ZmHttpUploadFileOptions& opts = {},
                                               bool* tooLarge = nullptr);

    // ── WebSocket(FR-16;onAuth 收完整握手请求) ──
    struct WsCallbacks
    {
        std::function<void(const drogon::WebSocketConnectionPtr&,
                           const drogon::HttpRequestPtr&)> onOpen;
        std::function<void(const drogon::WebSocketConnectionPtr&)> onClose;
        std::function<bool(const drogon::HttpRequestPtr& /* 握手请求 */)> onAuth;  // false → 升级回调内拒绝
        std::function<void(const drogon::WebSocketConnectionPtr&,
                           std::string&&, drogon::WebSocketMessageType)> onMessage;
    };
    virtual void RegisterWebSocket(const std::string& path, const WsCallbacks& cb);

    // ── 自动 JSONP(FR-24,主流中间件语义):GET + 合法 callback + JSON 响应 → 自动包装 ──
    /// 全局开关(三面共享,默认开;Phase1 设置)
    void SetAutoJsonp(bool enable) { s_autoJsonp.store(enable, std::memory_order_relaxed); }
    static bool IsAutoJsonpEnabled() { return s_autoJsonp.load(std::memory_order_relaxed); }

    // ── 统一响应助手(FR-08/09/24,静态) ──
    static drogon::HttpResponsePtr JsonResponse(int status, const ZMJSON& data);
    static drogon::HttpResponsePtr ErrorResponse(int status, const std::string& msg);
    static drogon::HttpResponsePtr JsonpResponse(const drogon::HttpRequestPtr& req,
                                                 const ZMJSON& data);

    /// 交叉转换:drogon(Json::Value)→ 业务 ZMJSON(值/数组/对象递归;
    /// 注意对象键序来自 Json::Value 的 map,如需要构造序请在业务侧用 ZMJSON 构造)
    static ZMJSON FromDrogonJson(const Json::Value& v);

    /// 交叉转换:业务 ZMJSON → drogon(Json::Value)(仅 drogon API 要求处使用,
    /// 如 loadConfigJson;业务链路无需接触 jsoncpp)
    static Json::Value ToDrogonJson(const ZMJSON& v);

    // ── 阻塞离核(FR-19,静态模板;任务投递到单一静态共享工作池,完成后回事件循环) ──
    template <typename T>
    static drogon::Task<T> RunOnPool(std::function<T()> fn);

    // ── 内部:共享工作池(三面共用,首次 RunOnPool 前经 SetWorkPoolSize 调大) ──
    static ZmThreadPool& WorkPool();

protected:
    virtual void RegisterRoutes() = 0;   // 派生面实现:注册自己路径前缀的路由

    // ── 一对象一端口(v2.5):本面唯一监听(未设置时 m_listenerSet=false) ──
    struct ZmHttpListener
    {
        uint16_t port = 0;
        bool useSSL = false;
        std::string ip;
        bool useOldTLS = false;
        std::vector<std::pair<std::string, std::string>> sslConfCmds;
    };
    ZmHttpListener m_listener;
    bool m_listenerSet = false;
    bool m_setupDone = false;
    std::string m_rootPath;              // 本面业务根路径(自定义;空 = 按"面自身"规则失能门禁)

    // ── per-port 辅助(共享路由表下恢复"端口隔离",设计 §4.4) ──
    bool IsLocalPortIn(const drogon::HttpRequestPtr& req) const
    {
        return m_listenerSet && ZmHttpLocalPort(req) == m_listener.port;
    }

private:
    // ── filter 按名注册 ──
    static std::mutex s_filterMtx;
    static std::map<std::string, std::function<bool(const drogon::HttpRequestPtr&,
                                                    drogon::HttpResponsePtr&)>> s_filters;
    static bool CheckFilterRegistered(const std::string& name);
    /// JSONP 回调名白名单校验(FR-24,防 XSS):[A-Za-z0-9_.] 且长度 ≤128
    static bool IsValidJsonpCallback(const std::string& cb);
    static std::atomic<bool> s_autoJsonp;   // 全局自动 JSONP 开关(默认开,对齐主流)
    static std::atomic<size_t> s_nonStreamBodyLimit;          // 非流式 body 上限(Init 注入;0 = 关)
    static std::vector<std::string> s_corsOrigins;            // CORS 白名单(Init 注入;启动后只读)
    /// {N} 占位符 → 正则(手动转换;设计 FR-05,绕开本捆绑 drogon 的崩溃点)
    static std::string PathPatternToRegex(const std::string& path);

    // ── Range 解析与文件传输实现 ──
    struct RangeInfo
    {
        bool present = false;      ///< 请求带 Range 头
        bool valid = false;        ///< Range 头合法(单段)
        bool partial = false;      ///< 请求了部分内容(非全文件)
        size_t offset = 0;
        size_t length = 0;         ///< 0 = 到文件尾
    };
    static RangeInfo ParseRange(const drogon::HttpRequestPtr& req, size_t fileSize);
    static drogon::HttpResponsePtr Range416Response(bool hasRange, size_t fileSize);
    static const std::string& MimeForExt(const std::string& path);

    /// 已知元信息的发送内部实现(公开入口与 Hybrid 共用,避免重复 stat):
    /// 接收已取的 fileSize/mtimeSec,内部完成 条件请求(304)→ Range → 响应构造。
    static drogon::Task<drogon::HttpResponsePtr>
    SendFileCoroImpl(const drogon::HttpRequestPtr& req, const std::string& path,
                     const std::string& attachmentName, size_t fileSize,
                     int64_t mtimeSec);
    static drogon::Task<drogon::HttpResponsePtr>
    SendFileStreamCoroImpl(const drogon::HttpRequestPtr& req,
                           const std::string& path,
                           const std::string& attachmentName,
                           const ZmHttpSendFileOptions& opts, size_t fileSize,
                           int64_t mtimeSec);
};

// ----------------------------------------------------------------------------
// RunOnPool:提交阻塞任务到共享工作池,CallbackAwaiter 桥回事件循环(FR-19)
//  - 工作池为单一静态共享池(s_workPoolSize 可配,默认 8,首次调用前生效)
//  - 协程恢复经 app().getLoop() 投递,保证协程始终在事件循环线程执行
//  - Awaiter 以非局部模板类实现(避免函数模板内局部类的 MSVC 解析问题)
// ----------------------------------------------------------------------------
template <typename T>
struct ZmRunOnPoolAwaiter : drogon::CallbackAwaiter<T>
{
    std::function<T()> fn_;

    void await_suspend(std::coroutine_handle<> h)
    {
        trantor::EventLoop* loop = drogon::app().getLoop();
        ZmHttpServer::WorkPool().Submit([this, h, loop] {
            try
            {
                this->setValue(fn_());
            }
            catch (...)
            {
                this->setException(std::current_exception());
            }
            if (loop)
                loop->queueInLoop([h] { h.resume(); });
            else
                h.resume();
        });
    }
};

template <typename T>
drogon::Task<T> ZmHttpServer::RunOnPool(std::function<T()> fn)
{
    ZmRunOnPoolAwaiter<T> a;
    a.fn_ = std::move(fn);
    co_return co_await a;
}

#endif /* ZM_NET_HTTP_SERVER_H */
