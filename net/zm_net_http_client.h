#ifndef ZM_NET_HTTP_CLIENT_H
#define ZM_NET_HTTP_CLIENT_H

/**
 * @file zm_net_http_client.h
 * @brief Drogon 1.9.13 出站 HTTP/HTTPS 客户端门面(进程级静态;独立双 lane)
 *
 * 说明:
 *  - 生命周期为**进程级静态**状态机(Uninit→Initialized→Closed):
 *      ZmHttpClient::Init(opts)  一次性注入全局参数并创建客户端双 lane
 *      ZmHttpClient::Close()     全局唯一关闭(三步序:先下载通道后普通 lane);幂等;Closed 为终态
 *    与服务器 ZmHttpServer 完全解耦:服务器未 Open/已 Close 均不影响客户端可用性。
 *  - 普通请求(HTTP/HTTPS)经 drogon HttpClient + per-target 连接池(HttpClient-Loop,Lane A);
 *    大文件下载走 trantor TcpClient 自研直写盘通道(HttpClient-DL,Lane B,见设计 §10)。
 *  - 三种调用形态:协程(推荐)/异步回调/同步(仅业务线程);统一返回 ZmHttpResult,
 *    业务不接触 drogon 请求类型(仅结果泊接 HttpResponsePtr)。
 *  - TLS 参数为 client 级创建时固化,仅 Options 可配,**无逐请求覆盖**(设计 §8)。
 *
 * 设计:ZiMoService docs/designs/2026-09-01-drogon-httpclient-design.md
 */

#include <drogon/HttpTypes.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>

#include <trantor/net/EventLoop.h>

#include <zm_util_json.h>
#include <zm_util_thread.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>

// ----------------------------------------------------------------------------
// 公共类型(设计 §5)
// ----------------------------------------------------------------------------

/// 单次请求运行时选项(每请求覆盖全局默认)
struct ZmHttpRequestOptions
{
    double timeoutSec = 0;              ///< 0 = 全局默认; -1 = 不超时
    int    retryCount = -1;             ///< -1 = 全局默认; 0 = 不重试
    bool   idempotent = false;          ///< 非幂等方法显式声明后可重试(配合业务重放语义)
    bool   followRedirect = true;       ///< 跟随重定向(上限走全局 maxRedirects)
    trantor::EventLoop* resumeLoop = nullptr;  ///< 协程恢复目标 loop(空 = 客户端 lane loop;线程亲和业务用)
    std::map<std::string, std::string> headers;   ///< 追加头
    size_t maxBodyBytes = 0;            ///< 0 = 全局护栏
};

/// 统一结果(业务感知的唯一载体;错误分类对齐 drogon ReqResult)
struct ZmHttpResult
{
    drogon::ReqResult err = drogon::ReqResult::Ok;  ///< 网络层错误分类(Ok/BadResponse/NetworkFailure/
                                                    ///< BadServerAddress/Timeout/HandshakeError/
                                                    ///< InvalidCertificate/EncryptionFailure)
    int    status = 0;                  ///< HTTP 状态码(0 = 无响应)
    drogon::HttpResponsePtr resp;       ///< 泊接 drogon(头/体),业务按需读取
    double elapsedSec = 0;              ///< 本次请求总耗时(含重试)
    int    retries = 0;                 ///< 实际重试次数
    bool   followedRedirect = false;    ///< 是否发生过重定向跟随
    std::string finalUrl;               ///< 重定向终结 URL

    /// Content-Type 为 JSON 时解析为 ZMJSON(解析失败空对象 + 告警日志)
    ZMJSON Json() const;
    /// 响应体原样字节(解码候选:Content-Encoding 已由 drogon 处理)
    std::string Body() const;
};

// ----------------------------------------------------------------------------
// 门面(公共类型定义在类内;实现对应 zm_net_http_client.cpp)
// ----------------------------------------------------------------------------

class ZmHttpClient
{
public:
    // ── 全局运行参数(client 级创建时固化;TLS 唯一开关,无逐请求覆盖;设计 §5/§8) ──
    struct Options
    {
        size_t normalLoopThreads = 1;   ///< 普通请求 lane 事件循环线程数(1..8;>1 时按 target 哈希绑定 loop)
        size_t workPoolSize = 4;        ///< 客户端自持阻塞工作池(离核任务,切勿设 0)
        size_t maxConnPerHost = 4;      ///< 每目标持久连接数
        size_t maxTargets = 256;        ///< 池容量上限(超限逐出 idle 池,LRU 由创建序近似)
        double defaultTimeoutSec = 10;  ///< 总超时默认值(0 不准,-1 不超时)
        int    retryMax = 3;            ///< 幂等请求最大重试次数
        int    maxTotalAttempts = 8;    ///< 重试+重定向 总尝试预算(防互跳死循环)
        double retryBaseMs = 200;       ///< 指数退避基数
        double retryCapMs = 3000;       ///< 退避上限
        double retryJitter = 0.25;      ///< 抖动幅度(±)
        bool   autoRedirect = true;     ///< 跟随重定向
        int    maxRedirects = 5;
        bool   validateCert = true;     ///< 全局 TLS 校验(**唯一开关**,client 级固化无逐请求覆盖;应急关闭须日志告警)
        std::string trustCA;            ///< 追加信任根 PEM(内网自签);实现路径见设计 §8;空 = 系统信任
        std::string clientCert, clientKey;  ///< mTLS 客户端证书(可选;普通 lane 经 setCertPath,下载通道经 TLSPolicy)
        std::string userAgent = "ZiMoClient/1.0";
        std::map<std::string, std::string> commonHeaders;  ///< 全量附加头(日志脱敏字段豁免)
        size_t maxBodyBytes = 100ULL * 1024 * 1024;  ///< 普通请求整包缓冲护栏(100MB 默认;**业务护栏**非内存保护,设计 §12)
        bool   outboundAccessLog = true;   ///< 出站访问日志开关
        // —— 流式下载通道 ——
        bool   enableDownload = true;      ///< 是否创建下载通道
        size_t downloadChunkBytes = 1 * 1024 * 1024;  ///< 读回调单次写盘分块
        size_t downloadStallAbortMs = 120 * 1000;     ///< 对端停滞/无写进展放弃
        size_t downloadWriteMaxMs = 200;   ///< 单次写盘耗时超限 → abort(防慢盘卡死通道 loop)
    };

    // ── 静态生命周期(进程级一次;状态机 Uninit→Initialized→Closed,独立于 ZmHttpServer) ──
    /// 一次性初始化:注入全局参数(校验 1..8 lane / 工作池非 0);只能调用一次。
    /// Init 即创建客户端双 lane(普通请求池 loop;下载通道,见设计 §4.1)。
    static bool Init(const Options& opts);
    /// 已初始化且未 Close
    static bool IsReady();
    /// 全局唯一关闭(三步序:①下载通道 ②普通 lane ③置 Closed);幂等;终态。
    /// 严禁在任一已登记 loop 线程内调用(自锁)。
    static void Close();

    // ── 协程(推荐;resume 线程默认 = 客户端 lane loop,可经 opts.resumeLoop 回环) ──
    static drogon::Task<ZmHttpResult> SendCoro(drogon::HttpMethod m, const std::string& url,
        const ZMJSON& body = {}, const ZmHttpRequestOptions& opts = {});
    static drogon::Task<ZmHttpResult> GetCoro(const std::string& url,
        const ZmHttpRequestOptions& opts = {});
    static drogon::Task<ZmHttpResult> PostJsonCoro(const std::string& url, const ZMJSON& body,
        const ZmHttpRequestOptions& opts = {});
    static drogon::Task<ZmHttpResult> PostFormCoro(const std::string& url,
        const std::map<std::string, std::string>& fields, const ZmHttpRequestOptions& opts = {});
    static drogon::Task<ZmHttpResult> UploadCoro(const std::string& url,
        const std::string& filePath, const std::string& field, const ZmHttpRequestOptions& opts = {});  // multipart 手拼(设计 §9)

    // ── 回调(异步兼容) ──
    static void SendAsync(drogon::HttpMethod m, const std::string& url, const ZMJSON& body,
        std::function<void(ZmHttpResult)> cb, const ZmHttpRequestOptions& opts = {});

    // ── 同步(仅限业务线程/客户端自持工作池;所有已登记 loop 线程一律拒绝,设计 §12) ──
    static ZmHttpResult SendSync(drogon::HttpMethod m, const std::string& url,
        const ZMJSON& body = {}, const ZmHttpRequestOptions& opts = {});

    // ── 流式下载(本期;大文件边收边落盘,Range 续传走 .part/.meta,设计 §10) ──
    struct ZmDownloadResult
    {
        bool ok = false;          ///< 是否整体成功
        int  status = 0;          ///< HTTP 状态码(0 = 未获得响应)
        uint64_t written = 0;     ///< 实际落盘字节数
        std::string error;        ///< 失败原因(ok=false 时)
    };
    static drogon::Task<ZmDownloadResult> DownloadCoro(const std::string& url,
        const std::string& destPath, const ZmHttpRequestOptions& opts = {});  // 断点续传:自动取 .part/.meta 判定 Range 起点,服务端校验不符随时回退 0(设计 §10.3)

    // ── 内部:编排实现(唯一实现;公共形态一律薄壳转发,避免 Task 层数膨胀) ──
    // 设计约束:编排不在协程帧内——重试/重定向/多尝试循环 = 堆上 ZmSendMachine 回调状态机
    // (见 .cpp 匿名字空间);协程侧仅 ZmMachineAwaiter 薄桥,帧内成员仅指针对齐
    // (shared_ptr<ZmMachineCtx>/EventLoop*/coroutine_handle),值对象全部堆化;
    // opts 一律以 shared_ptr 飞行。详见设计文档 §4.3 帧隔离纪律。
    using ZmHttpRequestOptionsPtr = std::shared_ptr<const ZmHttpRequestOptions>;
    static drogon::Task<ZmHttpResult> SendPayload(drogon::HttpMethod m,
                                                  const std::string& url, ZMJSON jsonBody,
                                                  const std::string* rawBody,
                                                  const std::string& rawContentType,
                                                  ZmHttpRequestOptionsPtr opts);

    // ── 诊断/统计 ──
    static std::string DumpStats();   ///< per-target: 请求数/错误分类/字节/重试率
    static void ResetStats();

    // ── loop 登记表(设计 §4.3/§12;SendSync 拒绝与 Close 自锁保护的地基) ──
    /// 登记需要在 SendSync 被调用前完成(幂等);服务器各 loop 与客户端自身 lane loop 都应登记。
    static void RegisterLoop(trantor::EventLoop* loop);
    static void UnregisterLoop(trantor::EventLoop* loop);
    /// 当前线程是否任一已登记 loop 线程
    static bool IsLoopThread();

    /// 全局运行参数(须已 Init;供内部组件与测试读取)
    static const Options& GetOptions();
};

#endif  // ZM_NET_HTTP_CLIENT_H
