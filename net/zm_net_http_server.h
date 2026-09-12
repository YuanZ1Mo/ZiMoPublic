#ifndef ZM_NET_HTTP_SERVER_H
#define ZM_NET_HTTP_SERVER_H

/**
 * @file zm_net_http_server.h
 * @brief Drogon 1.9.13 HTTP 服务器基类（三个服务器面公共底座）
 *
 * 说明：
 *  - 生命周期为**进程级静态**状态机（Uninit→Initialized→Opened→Closed）：
 *      ZmHttpServer::Init(opts)  一次性注入全局运行参数/证书/全局 advice（/ping、访问日志、JSONP）
 *      ZmHttpServer::Open()      后台线程跑 app().run()，绑定失败 fail-fast
 *      ZmHttpServer::Close()     全局唯一关闭：app().quit()+join；幂等；Closed 为终态
 *    drogon app() 为全局单例且 run() 只能跑一次，故"关闭后不能再打开"，
 *    不支持运行期单端口启停/热重启。
 *  - 派生面"结构上多实例、运行时单 app()"：每个派生对象只负责"端口 + 路由登记"
 *    （AddListener/Setup/RegisterRoutes），全部须在 Open() 前完成；
 *    路由表/运行参数全局共享，路径前缀 + per-port 门禁区分（D2）。
 *  - 通用响应助手/文件传输（Range）/阻塞离核 RunOnPool 均在此基类。
 *
 * 设计：ZiMoService docs/designs/2026-08-30-drogon-httpserver-base-design.md
 */

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/RequestStream.h>
#include <drogon/RateLimiter.h>
#include <drogon/utils/FunctionTraits.h>
#include <drogon/utils/coroutine.h>
#include <drogon/WebSocketConnection.h>

#include <trantor/net/EventLoop.h>

#include <zm_util_thread.h>
#include <zm_util_json.h>
#include <zm_util_logger.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace drogon
{
class HttpFilterBase;
class HttpResponse;
}  // namespace drogon

// ----------------------------------------------------------------------------
// 公共类型
// ----------------------------------------------------------------------------

/// 协程 handler：业务层注册的请求回调形态（D3）
/// 注：本捆绑 drogon 的 FunctionTraits 协程特化仅匹配"按值 HttpRequestPtr"形参，
/// 因此此处用按值传递（与 drogon 协程契约一致）。
using ZmHttpCoroHandler = std::function<
    drogon::Task<drogon::HttpResponsePtr>(drogon::HttpRequestPtr)>;

/**
 * @brief 流式分块发送（SendFileStreamCoro）的行为参数
 */
struct ZmHttpSendFileOptions
{
    size_t chunkSize      = 1 * 1024 * 1024;   ///< 分块粒度
    /// 块间定时器间隔（定时器链节流，内存有界）；0 → 发完即调度
    /// （自适应，吞吐 = 排水速率，快客户端不设限）
    size_t interBlockMs   = 50;
    size_t watermarkBytes = 8 * 1024 * 1024;   ///< 严格水位目标（可选增强，默认不启用）
    int64_t stallAbortMs  = 120 * 1000;        ///< 对端停滞放弃阈值（毫秒）

    /// 进度回调（可选）：（已发送，总大小）
    std::function<void(uint64_t sent, uint64_t total)> onProgress;
};

/// 流式接收协程 handler（路径 B）：形参带 RequestStreamPtr 时被 drogon
/// FunctionTraits 判定为 stream-handler，框架自动注入流对象（数据逐块交付）
using ZmHttpStreamHandler = std::function<drogon::Task<drogon::HttpResponsePtr>(
    drogon::HttpRequestPtr, drogon::RequestStreamPtr)>;

/**
 * @brief 流式落盘参数（SaveStreamToFile）
 */
struct ZmHttpUploadFileOptions
{
    uint64_t maxBytes = 0;              ///< 实时兜底上限（块累加，超限停写并清理）；0 = 不限制
    /// 进度总量（百分比分母；0 = 未知，进度回退用 maxBytes）。业务可传 X-File-Size。
    uint64_t totalBytes = 0;
    uint64_t progressIntervalMs = 100;  ///< 进度回调最小间隔（毫秒）
    std::function<void(uint64_t written, uint64_t total)> onProgress;  ///< （可选）进度回调
};

/**
 * @brief 取请求到达的本地端口（字节交换等价 ntohs）
 *
 * 手工换序而不调 ntohs：避免 winsock 头的包含顺序问题。
 *
 * @param req  请求
 * @return 本机字节序的本地端口号
 */
inline uint16_t ZmHttpLocalPort(const drogon::HttpRequestPtr& req)
{
    uint16_t p = req->getLocalAddr().portNetEndian();
    return static_cast<uint16_t>((p >> 8) | ((p & 0xFF) << 8));
}

/**
 * @brief 判断请求是否到达给定端口集合中的任一端口
 *
 * @param req    请求
 * @param ports  端口集合（本机字节序）
 * @return true 到达的端口在集合内；false 不在集合内
 *
 * @example
 *   if (ZmHttpLocalPortIn(req, {80, 443})) { ... }
 */
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

/**
 * @brief 三个服务器面（前端 / JSON-RPC / RESTful）的公共基类
 *
 * 提供：静态生命周期、监听登记、协程/流式/WebSocket 路由注册、路由归属与门禁机制、
 * 限流、文件传输与统一响应助手、阻塞离核工作池。
 */
class ZmHttpServer
{
public:

    /**
     * @brief 虚析构：允许经基类指针销毁派生面对象
     */
    virtual ~ZmHttpServer() = default;

    // ── 类型与全局参数 ──
    //   经 Init 一次性注入，运行期不可改；含义/特殊值见 cpp 对应实现
    struct Options
    {
        size_t threadNum = 0;              ///< 事件循环线程数（0 = 自动 = CPU 核数）
        size_t maxConnections = 8192;      ///< 最大连接数护栏（0 大概率不限，慎用）
        /// 框架级请求体上限（1.9.13 对【全体请求含流式】强制，HttpRequestParser.cc:266，
        /// 超限 413）。它是流式大上传（RegisterStreamCoro）唯一的框架兜底，
        /// 勿调小于业务最大上传；非流式路由的提前拒绝由 nonStreamBodyLimit 承担。
        size_t clientMaxBodySize = 10ULL * 1024 * 1024 * 1024;
        /// 非流式路由请求体上限（PreRouting 按 Content-Length 预检，超限 413）：
        /// 带 X-File-Size 声明的请求豁免（流式大上传路径，业务经 RegisterStreamCoro
        /// 的 maxBytes 兜底）；0 = 关闭预检。注：1.9.13 无 per-route 上限 API，
        /// 此为 header 阶段全局闸门（路由无关）。
        size_t nonStreamBodyLimit = 256ULL * 1024 * 1024;
        /// 请求体内存缓冲上限（超过部分落临时文件，drogon 默认 64KB）：
        /// "打满内存"的防护闸，内存增长有界（非流式大 body 的代价是磁盘/IO 而非内存）。
        size_t clientMaxMemoryBodySize = 64 * 1024;
        /// per-IP 连接数护栏（0 = 不限）
        /// ⚠ 单机压测时全部连接同源 IP，设值小于压测并发会拒连
        size_t maxConnectionsPerIP = 0;
        size_t idleTimeoutSec = 90;        ///< keep-alive 空闲回收秒（0 = 关闭空闲回收）
        size_t keepaliveRequests = 0;      ///< 单连接请求数上限（0 = 不限次数回收）
        bool enableRequestStream = true;   ///< 上传流式落盘开关（业务依赖，保持 true）
        size_t workPoolSize = 8;           ///< 业务阻塞工作池线程数（切勿设 0）
        /// 动态 gzip 压缩（>1024 非二进制 body，事件循环线程同步压）
        bool gzip = false;
        bool brotli = false;               ///< 动态 brotli 压缩（同上；CPU 更高，压缩率更优）
        /// 静态 gzip：客户端支持时优先发同路径 <file>.gz 孪生
        /// （非现场压缩，无孪生照发原文件）
        bool gzipStatic = false;
        /// 静态 brotli：客户端支持时优先发同路径 <file>.br 孪生
        /// （库已链接，无孪生照发原文件）
        bool brotliStatic = false;
        bool ticketDisabled = false;       ///< TLS SessionTicket 禁用（安全项）
        std::string certFile;              ///< 全局证书（空 = 纯 HTTP）
        std::string keyFile;               ///< 全局私钥（与 certFile 配套）
        // 注：CORS 白名单**不在本结构体** —— 它是业务策略而非传输参数，
        //     改经 `SetCorsAllowedOrigins`（Open 前）声明，与 SetJsonp*/SetRootPath 同款。
    };

    //    文件内容内存交付，受单请求 maxBytes 约束；大文件走 RegisterStreamCoro
    /**
     * @brief multipart 解析结果（表单字段 + 上传文件）
     */
    struct ZmMultipartResult
    {
        /**
         * @brief 单个上传文件（内容驻留内存）
         */
        struct File
        {
            std::string itemName;      ///< 表单 item 名（原始）
            std::string fileName;      ///< 已消毒文件名（仅文件名段；空 = 无合法名）
            std::string contentType;   ///< 客户端声明的 Content-Type
            uint64_t size = 0;         ///< 文件字节数
            std::string data;          ///< 内容（内存；有界 = 单请求 maxBytes）
        };
        std::vector<std::pair<std::string, std::string>> fields;  ///< 表单字段（key,value）
        std::vector<File> files;                                  ///< 上传文件列表
    };

    using ZmMultipartHandler = std::function<drogon::Task<drogon::HttpResponsePtr>(
        const drogon::HttpRequestPtr&, const ZmMultipartResult&)>;

    /**
     * @brief WebSocket 回调集（按注册名存入全局表）
     */
    struct WsCallbacks
    {
        std::function<void(const drogon::WebSocketConnectionPtr&,
                           const drogon::HttpRequestPtr&)> onOpen;   ///< 连接建立
        std::function<void(const drogon::WebSocketConnectionPtr&)> onClose;  ///< 连接关闭
        /// 握手鉴权：false → 升级回调内拒绝
        std::function<bool(const drogon::HttpRequestPtr& /* 握手请求 */)> onAuth;
        std::function<void(const drogon::WebSocketConnectionPtr&,
                           std::string&&, drogon::WebSocketMessageType)> onMessage;  ///< 收到消息
    };

    /**
     * @brief JSONP 全局基线（不设置则用下列内置默认）
     */
    struct ZmJsonpOptions
    {
        /// 候选参数名（按序取首个非空）
        std::vector<std::string> paramNames = {"callback"};
        /// 错误响应（4xx/5xx 的 JSON）是否也包装 —— JSONP 客户端经 <script>
        /// 加载，本就不看状态码，故默认 true（兼容现状）
        bool   wrapErrors   = true;
        /// 响应体超限则**不包装**（避免全量拼接多一份拷贝）
        size_t maxBodyBytes = 256 * 1024;
        /// **未声明路由**是否默认包装（观察期默认开；确认无调用方后应置 false =
        /// 仅包装显式声明路由）；同时是各声明前缀的初始值，可被
        /// `ZmJsonpOverride::enabled` 逐条覆盖
        bool   enabled      = true;
    };

    /**
     * @brief 逐路由**差量**覆盖：只写要改的字段，其余继承全局基线
     *
     * （用 optional 表达"未设置"——否则 `ZmJsonpOverride{}` 的默认值会把全局设置拍回去）
     */
    struct ZmJsonpOverride
    {
        /// 本前缀是否包装：false = **例外**（在全局 `enabled = true` 的观察期下
        /// 也能单独关掉它；未设置 = 继承基线，声明默认启用）
        std::optional<bool>   enabled;
        std::optional<std::vector<std::string>> paramNames;   ///< 候选参数名
        std::optional<bool>   wrapErrors;                     ///< 是否包装错误响应
        std::optional<size_t> maxBodyBytes;                   ///< 包装体量上限
    };

    // ── 静态生命周期 ──
    /**
     * @brief 一次性初始化：应用全局参数 + 证书 + 全局 advice（/ping、访问日志、JSONP）
     *
     * 只能调用一次；未 Init 就 Open 会报错。
     *
     * @param opts  全局运行参数（见 Options）
     * @return true 初始化成功；false 状态非法（已初始化）或参数/证书不可用
     */
    static bool Init(const Options& opts);

    /**
     * @brief 启动服务器：后台线程跑 app().run()
     *
     * 须已 Init 且已登记至少一个监听；绑定失败（端口占用等）在 300ms 内探测到。
     *
     * @return true 启动成功；false 未 Init / 无监听 / 绑定失败
     *
     * @example
     *   ZmHttpServer::Init(opts);
     *   srv.Setup();     // 登记路由与 advice
     *   if (!ZmHttpServer::Open())
     *       return -1;   // 端口占用等绑定失败
     */
    static bool Open();

    /**
     * @brief 全局唯一关闭：app().quit() + join
     *
     * 幂等；Closed 为终态，之后不能再 Open（drogon app() 的 run() 只能跑一次）。
     */
    static void Close();

    /**
     * @brief 是否已完成一次性初始化
     * @return true 状态 ≥ Initialized
     */
    static bool IsInitialized();

    /**
     * @brief 服务器是否处于运行中
     * @return true 已 Open 且未 Close
     */
    static bool IsOpened();

    /**
     * @brief 本面监听是否启用 TLS（一对象一端口）
     *
     * 证书为进程级全局，故各面判定结果一致。
     * @return true 本面走 HTTPS；false 走 HTTP（或监听未登记）
     */
    virtual bool IsHttps() const;

    // ── 监听与证书 ──
    /**
     * @brief 本面监听端口
     * @return 端口号；未登记监听时返回 0
     */
    virtual uint16_t GetPort() const { return m_listenerSet ? m_listener.port : 0; }

    /**
     * @brief 本面绑定地址
     * @return 绑定 IP（0.0.0.0/:: = 通配监听）；未登记监听时返回空串
     */
    virtual std::string GetBindIp() const { return m_listenerSet ? m_listener.ip : ""; }

    /**
     * @brief 登记本面唯一监听（一对象一端口）
     *
     * 仅在已 Init 且 run 之前生效：port=0、重复登记、未 Init、run 后调用均记
     * ERROR 并忽略（重复登记保留首个）。certFile/keyFile 传空 → 使用全局
     * setSSLFiles 配置（保证 ReloadCertificates 的热加载语义）。
     *
     * @param port         监听端口（不得为 0）
     * @param useSSL       true = 启用 TLS
     * @param ip           绑定地址（默认 0.0.0.0 通配）
     * @param useOldTLS    是否允许旧版 TLS 协议
     * @param sslConfCmds  per-listener TLS 配置项（name/value 对）
     */
    virtual void AddListener(uint16_t port, bool useSSL = false,
                             const std::string& ip = "0.0.0.0",
                             bool useOldTLS = false,
                             const std::vector<std::pair<std::string, std::string>>& sslConfCmds = {});

    /**
     * @brief 证书热重载（运行期唯一可热更新能力）：reloadSSLFiles() 换内容不换路径
     *
     * 证书为进程级全局（Init 注入），故本方法为静态能力。
     * @return true 已触发重载；false 未配置证书
     */
    static bool ReloadCertificates();

    // ── 路由注册 ──
    /**
     * @brief 登记本面内置路由与 advice（幂等，派生面经 RegisterRoutes 实现）
     *
     * 须在 Open() 前调用；重复调用只生效一次。
     */
    virtual void Setup();

    /**
     * @brief 注册协程路由（路径参数经正则，不做形参绑定）
     *
     * 须在 Open 前调用，且路径归属必须是本面（跨面注册被拒）。
     * 路径不含 {N} 时走原生 registerHandler；含 {N} 时改走 registerHandlerViaRegex
     * —— 本捆绑 drogon 的原生转换路径在 Windows 上崩溃。因 handler 被擦成单参
     * `std::function`，路径参数不进形参（`getParameter("N")` 恒为空），需要按形参
     * 取路径参数请改用 RegisterCoroWithPathParams。
     *
     * @param path     路由路径（可含 {N} 占位符）
     * @param m        HTTP 方法
     * @param h        协程 handler（首参须按值 HttpRequestPtr）
     * @param filters  过滤器名列表（须已 AddFilter 注册，未注册记 ERROR 但仍登记）
     *
     * @example
     *   srv.RegisterCoro("/zimo/api/status", drogon::Get,
     *       [](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
     *           co_return ZmHttpServer::JsonResponse(200, ZMJSON{{"ok", true}});
     *       });
     */
    virtual void RegisterCoro(const std::string& path, drogon::HttpMethod m,
                              ZmHttpCoroHandler h,
                              const std::vector<std::string>& filters = {});

    /**
     * @brief 注册带**路径参数形参**的协程路由：路径参数由 drogon 原生绑定
     *
     * {1} → 第 1 个形参，依次类推，**不走正则**。与 RegisterCoro 的分工：
     *   · 有路径参数（{1}…）→ 用本接口（形参即参数，编译期可见，精确匹配）；
     *   · 无路径参数        → 用 RegisterCoro。
     * 为什么必须单独一个入口：RegisterCoro 的 handler 被擦成
     * `std::function<Task<HttpResponsePtr>(HttpRequestPtr)>`，路径参数不在形参里 →
     * `HttpBinder::paramCount()==0` → 含 {N} 的路径命中 `addHttpPath` 校验
     * （place > paramCount）会 **exit(1)**（HttpControllersRouter.cc:368）；故 RegisterCoro
     * 只能绕道 registerHandlerViaRegex，业务只能另行取参 —— 而 `getParameter("1")`
     * **恒为空**（路径参数不进 parameters_，drogon 原生亦然），于是被迫手工解析路径。
     * 守卫（两个方向都失败得早）：arity==0 → 编译期 static_assert；
     * 路径无 {N} → 运行期 ERROR + 拒绝注册（形参与路径不匹配 = 静默错绑）。
     * 注：路径参数形参建议用 `std::string` 自行校验（用 int 等类型时转换失败会抛 → 500）。
     *
     * @param path     路由路径（必须含 {N} 占位符）
     * @param m        HTTP 方法
     * @param h        协程 handler：`Task<HttpResponsePtr>(HttpRequestPtr, 路径参数...)`，
     *                 首参须按值，形参个数须与 {N} 个数一致
     * @param filters  过滤器名列表（须已 AddFilter 注册）
     *
     * @example
     *   srv.RegisterCoroWithPathParams("/zimo/api/admin/users/{1}", drogon::Get,
     *       [this](drogon::HttpRequestPtr req, std::string uidStr)
     *           -> drogon::Task<drogon::HttpResponsePtr> { ... });
     */
    template <typename F>
    void RegisterCoroWithPathParams(const std::string& path, drogon::HttpMethod m, F&& h,
                                    const std::vector<std::string>& filters = {});

    /**
     * @brief 注册带业务级 deadline 的协程路由
     *
     * 到期由事件循环定时器发 504，原子门（TryReply）保证只回一次；业务晚到结果经
     * "弱引用 + connected()"安全丢弃。流式/下载端点不要用本接口（会与流式响应的
     * 生命周期冲突）。deadlineMs=0 会让每个请求立即 504（静默失效），故被拒并记 ERROR。
     *
     * @param path        路由路径
     * @param m           HTTP 方法
     * @param h           协程 handler
     * @param deadlineMs  超时毫秒（必须 > 0）
     * @param filters     过滤器名列表（须已 AddFilter 注册）
     */
    virtual void RegisterCoroWithDeadline(const std::string& path, drogon::HttpMethod m,
                                          ZmHttpCoroHandler h, size_t deadlineMs,
                                          const std::vector<std::string>& filters = {});

    /**
     * @brief 注册 multipart 路由（预检 → 流式收集 → 交付业务协程）
     *
     * 流程：Content-Type/boundary 预检（缺 → 400）→ 声明超限 413 → 流式 reader
     * 收集（字段值 ≤1MB，超限 413）→ 交付业务协程。
     *
     * @param path      路由路径
     * @param m         HTTP 方法
     * @param h         业务协程 handler（收解析结果）
     * @param filters   过滤器名列表（须已 AddFilter 注册）
     * @param maxBytes  单请求总量上限（0 = 仅框架 clientMaxBodySize 兜底）
     */
    virtual void RegisterMultipartCoro(const std::string& path, drogon::HttpMethod m,
                                       ZmMultipartHandler h,
                                       const std::vector<std::string>& filters = {},
                                       uint64_t maxBytes = 256ULL * 1024 * 1024);

    /**
     * @brief 助手：多部件文件落盘（离核工作池）
     *
     * 业务回调内必须经此落盘，不得直接写文件 —— 事件循环纪律。
     * @param f         待落盘的文件（内容来自 ZmMultipartResult）
     * @param destPath  目标文件路径
     * @return 写入字节数；失败 -1
     */
    static drogon::Task<int64_t> SaveMultipartFile(const ZmMultipartResult::File& f,
                                                   const std::string& destPath);

    /**
     * @brief 注册流式接收路由（handler 形参带 RequestStreamPtr）
     *
     * handler 形参含 RequestStreamPtr 时被 drogon FunctionTraits 判定为
     * stream-handler，框架注入流对象并逐块回调。
     *
     * @param path      路由路径
     * @param m         HTTP 方法
     * @param h         流式协程 handler（数据逐块交付）
     * @param filters   过滤器名列表（须已 AddFilter 注册）
     * @param maxBytes  路由级上传上限（0 = 不额外限制，全局 10GB 兜底）；基类按
     *                  X-File-Size 声明自动早拒（超限 null reader + 413），并写入
     *                  req attributes（"ZmStreamMaxBytes"）供落盘兜底取用
     */
    virtual void RegisterStreamCoro(const std::string& path, drogon::HttpMethod m,
                                    ZmHttpStreamHandler h,
                                    const std::vector<std::string>& filters = {},
                                    uint64_t maxBytes = 0);

    /**
     * @brief 流式落盘（块到即写，写线程离核）
     *
     * @param stream    RequestStream（来自 RegisterStreamCoro 的 handler 形参）
     * @param destPath  目标文件路径
     * @param opts      上限/进度参数（见 ZmHttpUploadFileOptions）
     * @param tooLarge  输出：是否因超限而失败（可为 nullptr）
     * @return true 完整落盘成功；false 失败（超限经 tooLarge 区分），半成品已清理
     */
    static drogon::Task<bool> SaveStreamToFile(drogon::RequestStreamPtr stream,
                                               const std::string& destPath,
                                               const ZmHttpUploadFileOptions& opts = {},
                                               bool* tooLarge = nullptr);

    /**
     * @brief 注册 WebSocket 路由
     *
     * 每个 path 生成唯一注册名，经 DrClassMap 工厂实例化通用 ZmWsController，
     * 回调按注册名存入全局表；WS 路由同样受 per-port 门禁与归属校验约束。
     * @param path  路由路径
     * @param cb    回调集（见 WsCallbacks）
     */
    virtual void RegisterWebSocket(const std::string& path, const WsCallbacks& cb);

    // ── Filter 与 advice ──
    /**
     * @brief 按名注册过滤器（std::function → 框架 HttpFilter）
     *
     * 经 DrClassMap 注册工厂并按名物化单例，供路由以名字约束；同名重复注册覆盖。
     * @param name  过滤器名（路由以该名字引用）
     * @param f     判定函数：true = 放行；false = 拒绝（可写 resp 定制拒绝响应）
     */
    virtual void AddFilter(const std::string& name,
                           const std::function<bool(const drogon::HttpRequestPtr&,
                                                    drogon::HttpResponsePtr&)>& f);

    /**
     * @brief 注册 PreRouting advice（路由分发前）
     * @param a  advice：放行（cc()）、短路（cb(resp)）或异步续链
     */
    virtual void RegisterPreRouting(std::function<void(const drogon::HttpRequestPtr&,
                                                       drogon::AdviceCallback&&,
                                                       drogon::AdviceChainCallback&&)> a);

    /**
     * @brief 注册 PostRouting advice（路由后、handler 前）
     * @param a  advice：语义同 PreRouting
     */
    virtual void RegisterPostRouting(std::function<void(const drogon::HttpRequestPtr&,
                                                        drogon::AdviceCallback&&,
                                                        drogon::AdviceChainCallback&&)> a);

    /**
     * @brief 注册 PostHandling advice（handler 后、发送前）
     * @param a  advice：可读取/改写响应
     */
    virtual void RegisterPostHandling(std::function<void(const drogon::HttpRequestPtr&,
                                                         const drogon::HttpResponsePtr&)> a);

    /**
     * @brief 注册 PreSending advice（响应发送前最后一道）
     * @param a  advice：只加头不改状态（如静态缓存头、自动 JSONP）
     */
    virtual void RegisterPreSending(std::function<void(const drogon::HttpRequestPtr&,
                                                       const drogon::HttpResponsePtr&)> a);

    // ── 归属与共享 ──
    //   不变式：每条已注册路由都归属于某个声明了 root 的服务器面，或显式声明为平台共享。
    //     业务路由：归属必须是本面（跨面注册会绕过该面的 per-port 门禁）→ 拒绝注册；
    //        新增平台路由时须自行确保它落在某个面 root 下或已 MarkShared，
    //        否则会在所有端口可达（由运行期归属网兜底告警）。
    //     运行期：已服务响应的归属面 ≠ 端口所属面 → [ROUTE-LEAK] 告警
    //        （门禁漏网的唯一可见信号）。
    /**
     * @brief 声明本面业务根路径（副作用：声明即登记进进程级归属表）
     *
     * 供路由注册校验与门禁判定使用：root 唯一（重复声明记归属冲突，Open() 拒绝启动）；
     * 须在 run 之前声明（之后调用被拒）。
     *
     * @param path  业务根路径，如 "/zimo/jrpc"、"/zimo/api"；
     *              空串 = 本面不拥有任何前缀（不得注册业务路由，如前端重定向专用实例）；
     *              "/"  = 兜底归属（全局至多一个实例；未被其他面认领的路径归它，
     *                     如前端完整面）
     */
    virtual void SetRootPath(const std::string& path);

    /**
     * @brief 取本面业务根路径
     * @return 根路径（未声明时为空串）
     */
    virtual const std::string& GetRootPath() const { return m_rootPath; }

    /**
     * @brief 本面是否为兜底归属面
     * @return true root == "/"
     */
    bool IsCatchAllRoot() const { return m_rootPath == "/"; }

    /**
     * @brief 本面是否声明了任何归属
     * @return true root 非空（空 = 纯 advice 垫片，如前端重定向实例）
     */
    bool HasRoot() const { return !m_rootPath.empty(); }

    /**
     * @brief 面描述（日志/归属诊断用）
     * @return 形如 "/zimo/api@39441"、"无root@0"
     */
    std::string FaceDesc() const;

    /**
     * @brief 路径归属面查询（最长段前缀匹配；兜底 root 优先级最低）
     *
     * 启动后读只读快照，零锁。
     * @param path  请求路径
     * @return 归属的服务器面；nullptr = 不属于任何面（未声明 root 的面不认领）
     */
    static const ZmHttpServer* LookupOwner(std::string_view path);

    /**
     * @brief 查询是否为平台共享路径（三面可达，如 /ping）
     * @param path  请求路径
     * @return true 该路径经 MarkShared 登记为共享
     */
    static bool IsSharedPath(std::string_view path);

    /**
     * @brief 声明平台共享路径（门禁与运行期归属网对其豁免）
     * @param path  路径（空串忽略）
     */
    static void MarkShared(const std::string& path);

    // ── CORS 白名单 ──
    /**
     * @brief 声明 CORS 白名单（**跨站**许可表；按 Origin 全串精确匹配）
     *
     * 语义：白名单是**平台提供的判据**，本身不产生行为 ——
     *   由其消费者（业务 CORS advice，当前仅 RESTful 面注册）决定预检放行与响应头回显。
     *   · 命中 → 回显 `Access-Control-Allow-Origin` + `Allow-Credentials: true`；
     *   · **同站跨端口**（Origin 与 Host 同 host，仅端口不同，如 443 → 39441）由业务侧
     *     硬逻辑放行，**不经过本名单**；
     *   · 空名单 = 只放行同站跨端口，任何**跨站** Origin 一律拒绝（预检 403、响应不回显头）。
     * 不含 "*"（与凭据头互斥）。须在 Open 前调用（运行期不可改）。
     *
     * @param origins  允许的 Origin 全串列表，如 "https://www.example.com"
     */
    static void SetCorsAllowedOrigins(const std::vector<std::string>& origins);

    /**
     * @brief CORS 白名单查询（判据；由业务 advice 调用）
     * @param origin  请求的 Origin 头
     * @return true 命中白名单（origin 为空恒 false）
     */
    static bool IsCorsOriginAllowed(const std::string& origin);

    // ── 自动 JSONP ──
    //   机制（转换规则）是通用的，但"某接口是否允许被任意站点 <script> 跨站读取"
    //   是逐接口的安全决策（JSONP 天然绕过 CORS），故授权收到逐路由。
    /**
     * @brief 设置 JSONP 全局基线（Init 前设置才生效）
     * @param opts  基线选项（见 ZmJsonpOptions）
     */
    static void SetJsonpDefaults(const ZmJsonpOptions& opts);

    /**
     * @brief 授权某前缀可自动 JSONP（**前缀匹配，含子路径；最长前缀优先**）
     *
     * 用前缀而非精确路径：路由模式含 `{N}` 时实际请求路径与声明串不相等。
     * 声明默认启用；`over.enabled = false` 把该前缀声明为**例外**，用于全局
     * `enabled = true` 的观察期下单独收口，不必把全局基线翻成 false。
     * 例外与授权同表竞争，仍按最长前缀优先。
     * 须在 Open 前调用（启动期固化只读快照，运行期零锁）。
     *
     * @param pathPrefix  可 JSONP 的路径前缀（空串忽略）
     * @param over        差量覆盖（未设置字段继承全局基线；`enabled = false` = 排除）
     *
     * @example
     *   ZmHttpServer::SetJsonpEnabled("/zimo/api/legacy/");          // 授权
     *   ZmHttpServer::ZmJsonpOverride ex;
     *   ex.enabled = false;
     *   ZmHttpServer::SetJsonpEnabled("/zimo/api/secret", ex);       // 例外(不包装)
     */
    static void SetJsonpEnabled(const std::string& pathPrefix,
                                const ZmJsonpOverride& over = {});

    /**
     * @brief 全局 enabled 位（诊断/测试用）
     * @return true = 未声明路由仍会被包装
     */
    static bool IsAutoJsonpEnabled();

    // ── 文件与流式传输 ──
    /**
     * @brief 发送文件（整体读入响应；特大文件请用 Stream/Hybrid 变体）
     *
     * 支持条件请求：Last-Modified（mtime）+ 强 ETag（size-mtime），If-None-Match
     * 优先、If-Modified-Since 兜底 → 304（无 body）；Range/206 语义同前。
     *
     * @param req             请求（取条件请求头与 Range）
     * @param path            文件路径
     * @param attachmentName  非空 = 以附件下载（Content-Disposition）
     * @return 响应（200/206/304/404/416）
     */
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                 const std::string& attachmentName = "");

    /**
     * @brief 流式发送文件（定时器链分块，内存有界；大文件/慢客户端用）
     *
     * @param req             请求
     * @param path            文件路径
     * @param attachmentName  非空 = 以附件下载
     * @param opts            分块/节流/停滞放弃参数（见 ZmHttpSendFileOptions）
     * @return 响应（200/206/304/404/416）
     */
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileStreamCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                       const std::string& attachmentName,
                       const ZmHttpSendFileOptions& opts = {});

    /**
     * @brief 混合发送：小于阈值走整体路径，达到阈值走流式路径
     *
     * @param req             请求
     * @param path            文件路径
     * @param attachmentName  非空 = 以附件下载
     * @param threshold       切换阈值（字节）
     * @param streamOpts      流式路径的分块/节流参数
     * @return 响应（200/206/304/404/416）
     */
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileHybridCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                       const std::string& attachmentName,
                       size_t threshold = 2ULL * 1024 * 1024 * 1024,
                       const ZmHttpSendFileOptions& streamOpts = {});

    using StreamCb = std::function<void(drogon::ResponseStreamPtr)>;   ///< 流对象回调
    /**
     * @brief 构造流式响应（newAsyncStreamResponse）
     * @param cb              流对象回调（在事件循环线程被调用）
     * @param disableKickoff  true = 关闭 trantor 默认启动超时
     *                        （长流/业务线程先启动场景必备）
     * @return 流式响应（调用方自行设置状态码与响应头）
     */
    static drogon::HttpResponsePtr MakeStreamResponse(StreamCb cb, bool disableKickoff = true);

    // ── 响应助手 ──
    //   边界：业务可持有 drogon 的值/协程类型（HttpRequestPtr/Task），
    //   但**响应构造与缓存语义必须经平台助手** —— 否则会出现"业务自写页面响应，
    //   静默绕过条件请求/缓存策略"（module_gate.ServeIndex 曾如此， 的页面 304
    //   因此未生效）。可 grep 检查：业务层出现 drogon::HttpResponse::new* 即越界。
    /**
     * @brief 构造 JSON 响应（裸 JSON 体，键序 = ZMJSON 构造序）
     * @param status  HTTP 状态码
     * @param data    响应数据（业务语义）
     * @return 响应对象（Content-Type: application/json）
     */
    static drogon::HttpResponsePtr JsonResponse(int status, const ZMJSON& data);

    /**
     * @brief 构造统一错误响应：{error:{code,message}}（与前端 auth.js 约定一致）
     * @param status  HTTP 状态码（同时作为 error.code）
     * @param msg     错误描述
     * @return 响应对象
     *
     * @example
     *   co_return ZmHttpServer::ErrorResponse(403, "forbidden");
     */
    static drogon::HttpResponsePtr ErrorResponse(int status, const std::string& msg);

    /**
     * @brief 构造（显式）JSONP 响应
     *
     * 有合法 callback 参数 → `cb(json);`（application/javascript）；无 callback →
     * 退化为常规 JSON；非法 callback 名 → 400（防 XSS 反射）。自动 JSONP
     * （PreSending advice）见 SetJsonpDefaults/SetJsonpEnabled。
     *
     * @param req   请求（取 callback 参数）
     * @param data  响应数据
     * @return 响应对象（200/400）
     */
    static drogon::HttpResponsePtr JsonpResponse(const drogon::HttpRequestPtr& req,
                                                 const ZMJSON& data);

    /**
     * @brief 交叉转换：drogon（Json::Value）→ 业务 ZMJSON（值/数组/对象递归）
     *
     * 注意对象键序来自 Json::Value 的 map，如需要构造序请在业务侧用 ZMJSON 构造。
     * @param v  drogon/jsoncpp 值
     * @return 等价的 ZMJSON
     */
    static ZMJSON FromDrogonJson(const Json::Value& v);

    /**
     * @brief 交叉转换：业务 ZMJSON → drogon（Json::Value）
     *
     * 仅 drogon API 要求处使用（如 loadConfigJson）；业务链路无需接触 jsoncpp。
     * @param v  业务 ZMJSON
     * @return 等价的 Json::Value
     */
    static Json::Value ToDrogonJson(const ZMJSON& v);

    /**
     * @brief 页面/小文件响应（带条件请求）
     *
     * 单次 stat → Last-Modified + 强 ETag；If-None-Match 优先、If-Modified-Since
     * 兜底 → 304（无 body）；不存在/stat 失败 → 404（记 WARN）；Cache-Control 由
     * 调用方设置（平台不覆盖）。
     * ⚠ 不带 Range：大文件/下载/断点续传走 SendFileCoro / SendFileHybridCoro。
     *
     * @param req       请求（取条件请求头）
     * @param filePath  文件路径
     * @return 响应（200/304/404）
     */
    static drogon::HttpResponsePtr FileResponse(const drogon::HttpRequestPtr& req,
                                                const std::string& filePath);

    /**
     * @brief 跳转响应
     * @param url     目标地址
     * @param status  状态码（默认 302；需要 301 语义时显式传 301/303/307/308）
     * @return 响应对象
     */
    static drogon::HttpResponsePtr RedirectResponse(const std::string& url,
                                                    int status = 302);

    /**
     * @brief 404 响应（可回自定义 404 页）
     * @param req  请求（可空；当前线程为服务器 loop 且已 SetNotFoundPage 时回
     *             自定义 404 页）
     * @return 响应对象
     */
    static drogon::HttpResponsePtr
    NotFoundResponse(const drogon::HttpRequestPtr& req = nullptr);

    // ── 限流 ──
    using ZmRateLimiterPtr = drogon::RateLimiterPtr;   ///< 已包装 SafeRateLimiter
    /**
     * @brief 创建单桶限流器（全局配额；一次容量一个额度）
     * @param type        限流算法（固定窗口/滑动窗口/令牌桶）
     * @param capacity    时间单位内允许的次数
     * @param timeUnitSec 时间单位（秒）
     * @return 线程安全的限流器（如全站每小时 10 万次）
     */
    static ZmRateLimiterPtr CreateRateLimiter(drogon::RateLimiterType type,
                                              size_t capacity,
                                              double timeUnitSec);

    /**
     * @brief 逐 IP 桶协调器（每个 IP 独立桶；有界 maxEntries，溢出按插入序驱逐防泄漏）
     */
    class ZmIpRateLimiter
    {
    public:
        /**
         * @brief 创建协调器
         * @param type        限流算法
         * @param capacity    每桶在时间单位内允许的次数
         * @param timeUnitSec 时间单位（秒）
         * @param maxEntries  IP 桶上限（超出按插入序驱逐最旧桶）
         * @return 协调器实例（私有构造，只能经此获得）
         */
        static std::shared_ptr<ZmIpRateLimiter>
        Create(drogon::RateLimiterType type, size_t capacity, double timeUnitSec,
               size_t maxEntries = 10000);
        /**
         * @brief 组合执行：overlay（封禁→false/白→true/专项额度）未命中走本桶
         *
         * @param req   请求（取对端 IP）
         * @param resp  输出：被限流时写入 429 响应
         * @return true 放行；false 已拒绝（resp 已填）
         */
        bool Check(const drogon::HttpRequestPtr& req, drogon::HttpResponsePtr& resp);
    private:
        /// 私有构造：实例一律经 Create 获得
        ZmIpRateLimiter() = default;
        struct Impl;
        std::shared_ptr<Impl> m_impl;
    };

    /**
     * @brief 封禁某 IP（命中直接 429）
     * @param ip  对端 IP 字面量
     */
    static void SetIpBlocked(const std::string& ip);

    /**
     * @brief 解封某 IP（撤销封禁）
     * @param ip  对端 IP 字面量
     */
    static void UnblockIp(const std::string& ip);

    /**
     * @brief 设置某 IP 的专项额度桶（覆盖默认桶参数）
     * @param ip          对端 IP 字面量
     * @param capacity    时间单位内允许的次数
     * @param timeUnitSec 时间单位（秒）
     */
    static void SetIpQuota(const std::string& ip, size_t capacity,
                           double timeUnitSec);

    /**
     * @brief 白名单放行某 IP（跳过限流）
     * @param ip  对端 IP 字面量
     */
    static void SetIpAllowed(const std::string& ip);

    /**
     * @brief 删除某 IP 的 overlay 规则（恢复正常限流）
     * @param ip  对端 IP 字面量
     */
    static void RemoveRateRule(const std::string& ip);

    /**
     * @brief 审计标注：该 IP 是否命中 overlay 规则
     * @param ip  对端 IP 字面量
     * @return true 命中封禁/白名单/专项额度中的任一条
     */
    static bool IsRateRuleHit(const std::string& ip);

    // ── 线程池 ──
    /**
     * @brief 设置共享工作池线程数
     *
     * 切勿设 0：自研 ZmThreadPool 按该数创建工作线程，0 线程 = 任务永不执行、
     * 业务卡死。首次 RunOnPool 前调用才生效。
     * @param n  工作线程数（≥1）
     */
    static void SetWorkPoolSize(size_t n);

    /**
     * @brief 查询共享工作池线程数
     * @return 当前线程数（默认 8）
     */
    static size_t GetWorkPoolSize();

    /**
     * @brief 在共享工作池执行阻塞任务，完成后回事件循环恢复协程
     *
     * @tparam T  任务返回类型
     * @param fn  阻塞任务（在工作线程执行）
     * @return 任务结果（异常原样传播到调用协程）
     *
     * @example
     *   auto rows = co_await ZmHttpServer::RunOnPool<std::string>([&] { return QueryDb(); });
     */
    template <typename T>
    static drogon::Task<T> RunOnPool(std::function<T()> fn);

    /**
     * @brief 取共享工作池实例
     * @return 进程级单例工作池（线程数见 SetWorkPoolSize）
     */
    static ZmThreadPool& WorkPool();

protected:

    // ── 派生面契约 ──
    /**
     * @brief 派生面实现：注册自己路径前缀的路由（须在 Open 前完成）
     */
    virtual void RegisterRoutes() = 0;

    // ── 归属校验 ──
    /**
     * @brief 业务路由归属校验：path 的归属必须是本面，否则 ERROR + 拒绝注册
     *
     * 合法注册同时登记进 Open() 期复检日志；path 为空或 run 之后调用同样被拒。
     * @param path  路由路径
     * @param what  注册接口名（日志用）
     * @return true 归属本面，可继续注册；false 拒绝注册
     */
    bool CheckRouteOwnership(const std::string& path, const char* what);

    /**
     * @brief 归属一致性校验（Open() 期调用一次）
     *
     * ① root 声明冲突（重复 root / 多个兜底面）② 已登记路由归属复检
     * （防"先注册后声明 root"）。任一失败 → 拒绝启动；通过 → 固化只读快照
     * （此后查询零锁），并同期固化 JSONP 授权快照。
     * @return true 校验通过；false 拒绝启动
     */
    static bool ValidateRouteOwnership();

    // ── 监听登记 ──
    /**
     * @brief 本面监听配置（AddListener 登记后有效）
     */
    struct ZmHttpListener
    {
        uint16_t port = 0;          ///< 监听端口
        bool useSSL = false;        ///< 是否启用 TLS
        std::string ip;             ///< 绑定地址
        bool useOldTLS = false;     ///< 是否允许旧版 TLS 协议
        std::vector<std::pair<std::string, std::string>> sslConfCmds;  ///< per-listener TLS 配置
    };

    ZmHttpListener m_listener;              ///< 本面监听（一对象仅一个）
    bool m_listenerSet = false;             ///< 监听是否已登记
    bool m_setupDone = false;               ///< Setup 幂等标记
    std::string m_rootPath;                 ///< 本面业务根路径（自定义；空 = 门禁失能）

    // ── per-port 辅助（共享路由表下恢复"端口隔离"） ──
    /**
     * @brief 请求是否到达本面监听的端口
     * @param req  请求
     * @return true 到达本面端口；false 否则（未登记监听时恒 false）
     */
    bool IsLocalPortIn(const drogon::HttpRequestPtr& req) const
    {
        return m_listenerSet && ZmHttpLocalPort(req) == m_listener.port;
    }

    // ── 静态资源条件请求 ──
    /**
     * @brief 文件元信息（单次 stat：存在性/大小/最后写入秒）
     */
    struct ZmFileMeta
    {
        bool found = false;       ///< 文件存在（否则 → 404）
        bool sizeFailed = false;  ///< 存在但取大小失败（否则 → 500）
        size_t size = 0;          ///< 文件字节数
        int64_t mtimeSec = 0;     ///< last_write_time → epoch 秒
    };

    /**
     * @brief 单次 stat 取文件元信息
     * @param path  文件路径
     * @return 元信息（found=false 表示不存在）
     */
    static ZmFileMeta FetchFileMeta(const std::string& path);

    /**
     * @brief 生成缓存头对（Last-Modified, ETag；强 ETag = "size-mtime"）
     * @param m  文件元信息
     * @return {Last-Modified, ETag} 字符串对
     */
    static std::pair<std::string, std::string>
    CacheHeaders(const ZmFileMeta& m);

    /**
     * @brief 条件请求判定：If-None-Match 优先（弱比较/列表），If-Modified-Since 兜底
     * @param req           请求
     * @param m             文件元信息
     * @param cacheHeaders  CacheHeaders 产出的头对
     * @return 非空 = 304 响应；nullptr = 继续 200/206 流程
     */
    static drogon::HttpResponsePtr Maybe304(const drogon::HttpRequestPtr& req,
                                            const ZmFileMeta& m,
                                            const std::pair<std::string, std::string>& cacheHeaders);

private:

    // ── filter 按名注册 ──
                                                        ///< 名字 → 判定函数
    /**
     * @brief 查询 filter 是否已按名注册
     * @param name  过滤器名
     * @return true 已注册
     */
    static bool CheckFilterRegistered(const std::string& name);

    /**
     * @brief JSONP 回调名白名单校验（防 XSS 反射）
     * @param cb  客户端传入的 callback 名
     * @return true 合法（[A-Za-z0-9_.] 且长度 ≤128）
     */
    static std::vector<std::string> s_corsOrigins;      ///< CORS 白名单（Init 注入；启动后只读）
    /**
     * @brief {N} 占位符 → 正则（手动转换；设计，绕开本捆绑 drogon 的崩溃点）
     *
     * 转义正则元字符后把 {N} 换成 `([^/]+)`；供 registerHandlerViaRegex 使用。
     * @param path  路由模式（可含 {N}）
     * @return 等价正则串
     */
    static std::string PathPatternToRegex(const std::string& path);

    // ── Range 解析 ──
    //    语法非法/多段/未知 unit → present=true、partial/unsatisfiable=false → 忽略
    //    （200 全文件，对多线程下载器友好； MAY ignore or reject）；合法但不可满足
    //    （起点越界/后缀 0/空文件）→ unsatisfiable → 416；合法单段 → partial → 206
    /**
     * @brief Range 头解析结果
     */
    struct RangeInfo
    {
        bool present = false;       ///< 请求带 Range 头
        bool unsatisfiable = false; ///< 合法但不可满足（416 + Content-Range: bytes */size）
        bool partial = false;       ///< 合法单段区间（206）
        size_t offset = 0;          ///< 起始偏移
        size_t length = 0;          ///< 区间长度（0 = 到文件尾）
    };

    /**
     * @brief 解析 Range 头（RFC 7233，单段）
     * @param req       请求
     * @param fileSize  文件总大小
     * @return 解析结果（语义见 RangeInfo）
     */
    static RangeInfo ParseRange(const drogon::HttpRequestPtr& req, size_t fileSize);

    /**
     * @brief 构造 416 响应（Content-Range 指示文件总长）
     *
     * 响应体为 JSON 错误包；hasRange 为真时附加 `Content-Range` 头，值为通配形式
     * （星号加斜杠加文件总长），供客户端修正续传区间。
     * @param hasRange  请求是否带 Range 头（决定是否附加 Content-Range）
     * @param fileSize  文件总大小
     * @return 416 响应
     */
    static drogon::HttpResponsePtr Range416Response(bool hasRange, size_t fileSize);

    /**
     * @brief 按扩展名查 Content-Type
     * @param path  文件路径（取其扩展名）
     * @return MIME 串（未知扩展名回落通用类型）
     */
    static const std::string& MimeForExt(const std::string& path);

    // ── 文件传输实现 ──
    /**
     * @brief 已知元信息的发送内部实现（公开入口与 Hybrid 共用，避免重复 stat）
     *
     * 接收已取的 fileSize/mtimeSec，内部完成 条件请求（304）→ Range → 响应构造。
     * @param req             请求
     * @param path            文件路径
     * @param attachmentName  非空 = 以附件下载
     * @param fileSize        已取的文件大小
     * @param mtimeSec        已取的最后写入秒
     * @return 响应（200/206/304/416）
     */
    static drogon::Task<drogon::HttpResponsePtr>
    SendFileCoroImpl(const drogon::HttpRequestPtr& req, const std::string& path,
                     const std::string& attachmentName, size_t fileSize,
                     int64_t mtimeSec);

    /**
     * @brief 已知元信息的流式发送内部实现（分工同 SendFileCoroImpl）
     * @param req             请求
     * @param path            文件路径
     * @param attachmentName  非空 = 以附件下载
     * @param opts            流式分块/节流参数
     * @param fileSize        已取的文件大小
     * @param mtimeSec        已取的最后写入秒
     * @return 响应（200/206/304/416）
     */
    static drogon::Task<drogon::HttpResponsePtr>
    SendFileStreamCoroImpl(const drogon::HttpRequestPtr& req,
                           const std::string& path,
                           const std::string& attachmentName,
                           const ZmHttpSendFileOptions& opts, size_t fileSize,
                           int64_t mtimeSec);

};

// ----------------------------------------------------------------------------
// RegisterCoroWithPathParams：路径参数形参 → drogon 原生绑定
//  - 与 RegisterCoro 共用归属校验与 filter 装配，唯一区别是注册方式：
//    本接口走 app().registerHandler（原生，paramCount = 路径参数个数 ≥ 1）；
//    RegisterCoro 走类型擦除 + PathPatternToRegex（paramCount 恒 0）。
//  - 守卫刻意做成"用错就失败得早"：arity==0 编译期报错；路径无 {N} 运行期拒绝。
// ----------------------------------------------------------------------------
/**
 * @brief 注册带路径参数形参的协程路由（声明见类内，此处为模板定义）
 *
 * 编译期守卫：handler 形态非法（首参非按值 HttpRequestPtr）或 arity==0 直接
 * static_assert 报错；运行期守卫：路径不含 {N} → ERROR + 拒绝注册。
 *
 * @tparam F  协程 handler 类型：`Task<HttpResponsePtr>(HttpRequestPtr, 路径参数...)`
 * @param path     路由路径（须含 {N} 占位符）
 * @param m        HTTP 方法
 * @param h        handler（转发给 app().registerHandler，由 drogon 绑定路径参数）
 * @param filters  过滤器名列表（须已 AddFilter 注册）
 */
template <typename F>
void ZmHttpServer::RegisterCoroWithPathParams(const std::string& path, drogon::HttpMethod m,
                                             F&& h,
                                             const std::vector<std::string>& filters)
{
    using Traits = drogon::internal::FunctionTraits<std::decay_t<F>>;
    static_assert(Traits::isHTTPFunction,
                  "handler 形态非法:需 Task<HttpResponsePtr>(HttpRequestPtr, 路径参数...) —— "
                  "首参须为**按值** HttpRequestPtr(本捆绑 FunctionTraits 协程特化的约束)");
    static_assert(Traits::arity > 0,
                  "RegisterCoroWithPathParams 需要声明路径参数形参(如 std::string uidStr);"
                  "无路径参数的路由请用 RegisterCoro");
    if (!CheckRouteOwnership(path, "RegisterCoroWithPathParams"))
        return;
    if (path.find('{') == std::string::npos)
    {
        PUBLIC_LOG_ERROR("RegisterCoroWithPathParams[{}]: 路径无 {{N}} 占位符,"
                         "形参与路径不匹配 → 拒绝注册(无路径参数请用 RegisterCoro)",
                         path);
        return;
    }
    std::vector<drogon::internal::HttpConstraint> cons;
    cons.emplace_back(m);
    for (const auto& fn : filters)
    {
        if (!CheckFilterRegistered(fn))
            PUBLIC_LOG_ERROR("RegisterCoroWithPathParams[{}]: filter 未注册: {}", path, fn);
        cons.emplace_back(fn);
    }
    // 原生注册：不经 PathPatternToRegex（{N} 由 drogon 自行转换为捕获组并绑定到形参）
    drogon::app().registerHandler(path, std::forward<F>(h), cons);
}

// ----------------------------------------------------------------------------
// RunOnPool：提交阻塞任务到共享工作池，CallbackAwaiter 桥回事件循环
//  - 工作池为单一静态共享池（线程数可配，默认 8，首次调用前生效）
//  - 协程恢复经事件循环投递，保证协程始终在事件循环线程执行
//  - Awaiter 以非局部模板类实现（避免函数模板内局部类的 MSVC 解析问题）
// ----------------------------------------------------------------------------
/**
 * @brief RunOnPool 的 awaiter：工作线程执行任务，结果/异常回投事件循环
 *
 * @tparam T  任务返回类型
 */
template <typename T>
struct ZmRunOnPoolAwaiter : drogon::CallbackAwaiter<T>
{
    std::function<T()> fn_;   ///< 待执行的阻塞任务

    /**
     * @brief 挂起协程：把任务投到工作池，完成后经事件循环恢复
     * @param h  当前协程句柄
     */
    void await_suspend(std::coroutine_handle<> h)
    {
        // 恢复投递：优先协程当前线程所属事件循环（连接 loop，免跨线程唤醒）；
        // 无所属 loop → 退回 app().getLoop();仍无(未 Open 等异常态)→ 告警后于工作线程恢复。
        trantor::EventLoop* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        if (!loop)
            loop = drogon::app().getLoop();
        ZmHttpServer::WorkPool().Submit([this, h, loop] {
            try
            {
                this->setValue(fn_());          // 正常结果：就地存值
            }
            catch (...)
            {
                this->setException(std::current_exception());   // 异常捕获后原样回传
            }
            if (loop)
                loop->queueInLoop([h] { h.resume(); });   // 回事件循环线程恢复协程
            else
            {
                PUBLIC_LOG_WARN("RunOnPool: 无可用事件循环,协程于工作线程恢复");
                h.resume();
            }
        });
    }
};

/**
 * @brief 在共享工作池执行阻塞任务（声明见类内，此处为模板定义）
 *
 * @tparam T  任务返回类型
 * @param fn  阻塞任务（在工作线程执行）
 * @return 任务结果（异常原样传播到调用协程）
 */
template <typename T>
drogon::Task<T> ZmHttpServer::RunOnPool(std::function<T()> fn)
{
    ZmRunOnPoolAwaiter<T> a;
    a.fn_ = std::move(fn);
    co_return co_await a;
}

#endif /* ZM_NET_HTTP_SERVER_H */
