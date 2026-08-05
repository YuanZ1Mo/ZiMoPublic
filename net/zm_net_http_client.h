#ifndef ZM_NET_HTTP_CLIENT_H
#define ZM_NET_HTTP_CLIENT_H

#include "../json/zm_json.h"
#include "../util/zm_util_str.h"   // BYTE

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class ZmHttpClientPrivate;

/** @brief 客户端错误码 */
enum ZmHttpClientError
{
    ZM_HTTPC_OK                  = 0,
    ZM_HTTPC_ERR_CONNECT,         ///< 连接失败
    ZM_HTTPC_ERR_CONNECT_TIMEOUT, ///< 连接超时
    ZM_HTTPC_ERR_TIMEOUT,         ///< 请求总超时
    ZM_HTTPC_ERR_CANCELLED,       ///< 主动取消
    ZM_HTTPC_ERR_PARSE,           ///< 响应解析失败
    ZM_HTTPC_ERR_FILE_IO,         ///< 本地文件读写失败(输出落盘/文件上传读取)
    ZM_HTTPC_ERR_STREAM_BROKEN,   ///< 流式中断(对端断开)
    ZM_HTTPC_ERR_SSL,             ///< TLS 错误
    ZM_HTTPC_ERR_PROXY,           ///< 代理错误
    ZM_HTTPC_ERR_REDIRECT,        ///< 重定向次数超限
    ZM_HTTPC_ERR_RESPONSE_TOO_LARGE, ///< 响应体超 SetMaxBodySize 上限
    ZM_HTTPC_ERR_UNSUPPORTED,     ///< 不支持的协议(如非 http/https)
};

/** @brief HTTP 请求(可链式设置) */
class ZmHttpClientRequest
{
public:
    ZmHttpClientRequest();

    // ── 基础 ──
    ZmHttpClientRequest& SetMethod(const char* method);       ///< GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH,默认 GET
    ZmHttpClientRequest& SetUrl(const char* url);             ///< http(s)://host[:port]/path
    ZmHttpClientRequest& SetHeader(const char* name, const char* value);
    void RemoveHeader(const char* name);                      ///< 移除指定请求头(重定向跨主机剥离 Authorization/Cookie 用)

    // ── 请求体(四选一,后设覆盖)──
    ZmHttpClientRequest& SetBody(const void* data, size_t len);
    ZmHttpClientRequest& SetBodyJson(const ZMJSON& json);     ///< Content-Type: application/json
    ZmHttpClientRequest& SetBodyForm(const std::map<std::string, std::string>& fields); ///< URL 编码表单;Content-Type: application/x-www-form-urlencoded
    ZmHttpClientRequest& SetBodyFile(const char* path);       ///< 文件上传(读取失败在请求错误中体现)
    ZmHttpClientRequest& SetUploadStream(std::function<std::string()> onUploadChunk); ///< chunked 流式上传;返回空串 = 结束

    // ── 超时(秒,0 = 不限制)──
    ZmHttpClientRequest& SetConnectTimeout(int seconds);      ///< 连接超时(秒,0 = 不限制,用 libevent 默认 45s),默认 10;读写超时由 SetReadWriteTimeout 独立控制,未设置时跟随本值
    ZmHttpClientRequest& SetReadWriteTimeout(int seconds);    ///< 读写超时(秒,0 = 跟随连接超时;连接超时为 0 时读写用 libevent 默认 50s);流式请求在响应头到达后由本超时接管空闲保护,SSE 心跳间隔需小于此值
    ZmHttpClientRequest& SetTotalTimeout(int seconds);        ///< 默认 30;流式开始后自动取消

    // ── 重定向 ──
    ZmHttpClientRequest& SetFollowRedirect(bool on, int max = 5);  ///< 跟随 301/302/303/307/308(默认开);max 为整条链请求次数上限(最多跟随 max-1 次跳转),超限 → ZM_HTTPC_ERR_REDIRECT;307/308 保留方法/体,301 POST/302/303(HEAD 除外)转 GET 清体

    // ── 代理 ──
    ZmHttpClientRequest& SetProxy(const char* host, uint16_t port);
    ZmHttpClientRequest& SetProxyAuth(const char* user, const char* pass); ///< 代理认证(user:pass 经 base64 → Proxy-Authorization: Basic ...;null 按空串);仅发代理(逐跳头),不带入目标请求

    // ── 认证 ──
    ZmHttpClientRequest& SetBasicAuth(const char* user, const char* pass);  ///< Basic 认证(user:pass 经 base64 → Authorization: Basic ...;null 按空串)
    ZmHttpClientRequest& SetBearerToken(const char* token);                 ///< Authorization: Bearer <token>

    // ── cookie / gzip / 重试 ──
    ZmHttpClientRequest& SetUseCookieJar(bool on);            ///< 使用客户端 cookie jar(默认开)
    ZmHttpClientRequest& SetGzip(bool on);                    ///< Accept-Encoding: gzip,deflate + 自动解压(默认开)
    ZmHttpClientRequest& SetRetry(int count, int baseDelayMs = 500); ///< 幂等方法重试次数 + 基础退避

    // ── 回调(流式/SSE/进度)──
    ZmHttpClientRequest& SetOnDataChunk(std::function<void(const BYTE* data, size_t len)> cb); ///< 流式下载
    ZmHttpClientRequest& SetOnSseEvent(std::function<void(const std::string& data)> cb);       ///< SSE 逐事件
    ZmHttpClientRequest& SetProgressCallback(std::function<void(int64_t sent, int64_t total)> cb);

    // ── 响应体上限 ──
    ZmHttpClientRequest& SetMaxBodySize(uint64_t bytes);      ///< 响应体大小上限(解压后字节口径;0 = 不限制,默认);超限 → ZM_HTTPC_ERR_RESPONSE_TOO_LARGE(流式请求在增量分发时中止,全量在收口时报错)

    // ── 消费方式 ──
    // SetOutputFile:路径为调用方责任(建议绝对路径;进程 ACP 编码,中文路径需按系统代码页;
    // 文件以覆盖模式打开,失败/取消会截断已有文件)
    ZmHttpClientRequest& SetOutputFile(const char* path);     ///< 响应体边收边写落盘
    ZmHttpClientRequest& SetRange(int64_t offset);            ///< Range: bytes=offset-

    // ── 请求级 TLS 覆盖(可选,覆盖客户端级)──
    ZmHttpClientRequest& SetClientCert(const char* certFile, const char* keyFile);

    // ── 访问器(客户端内部使用)──
    const std::string& Method() const { return m_method; }
    const std::string& Url() const { return m_url; }
    const std::map<std::string, std::string>& Headers() const { return m_headers; }
    const std::vector<BYTE>& Body() const { return m_body; }
    bool HasBody() const { return !m_body.empty() || !m_bodyFile.empty() || m_uploadChunk; }
    const std::string& BodyFile() const { return m_bodyFile; }
    std::function<std::string()> UploadChunk() const { return m_uploadChunk; }
    int ConnectTimeout() const { return m_connectTimeout; }
    int ReadWriteTimeout() const { return m_readWriteTimeout; }
    int TotalTimeout() const { return m_totalTimeout; }
    bool FollowRedirect() const { return m_followRedirect; }
    int RedirectMax() const { return m_redirectMax; }
    bool HasProxy() const { return !m_proxyHost.empty(); }
    const std::string& ProxyHost() const { return m_proxyHost; }
    uint16_t ProxyPort() const { return m_proxyPort; }
    bool HasProxyAuth() const { return !m_proxyAuthHeader.empty(); }
    const std::string& ProxyAuthHeader() const { return m_proxyAuthHeader; }
    bool UseCookieJar() const { return m_useCookieJar; }
    bool Gzip() const { return m_gzip; }
    int RetryCount() const { return m_retryCount; }
    int RetryBaseDelayMs() const { return m_retryBaseDelayMs; }
    const std::function<void(const BYTE*, size_t)>& OnDataChunk() const { return m_onDataChunk; }
    const std::function<void(const std::string&)>& OnSseEvent() const { return m_onSseEvent; }
    const std::function<void(int64_t, int64_t)>& Progress() const { return m_progress; }
    const std::string& OutputFile() const { return m_outputFile; }
    uint64_t MaxBodySize() const { return m_maxBodySize; }
    int64_t Range() const { return m_range; }
    const std::string& ClientCertFile() const { return m_clientCertFile; }
    const std::string& ClientKeyFile() const { return m_clientKeyFile; }

private:
    std::string m_method = "GET";
    std::string m_url;
    std::map<std::string, std::string> m_headers;
    std::vector<BYTE> m_body;
    std::string m_bodyFile;
    std::function<std::string()> m_uploadChunk;
    int  m_connectTimeout = 10;
    int  m_totalTimeout = 30;
    bool m_followRedirect = true;
    int  m_redirectMax = 5;
    std::string m_proxyHost;
    uint16_t m_proxyPort = 0;
    std::string m_proxyAuthHeader;   ///< Proxy-Authorization 头值(SetProxyAuth 生成;空 = 无认证)
    bool m_useCookieJar = true;
    bool m_gzip = true;
    int  m_readWriteTimeout = 0;   ///< 读写超时(0 = 跟随连接超时)
    int  m_retryCount = 0;         ///< 0 = 不重试
    int  m_retryBaseDelayMs = 500;
    std::function<void(const BYTE*, size_t)> m_onDataChunk;
    std::function<void(const std::string&)>  m_onSseEvent;
    std::function<void(int64_t, int64_t)>  m_progress;
    std::string m_outputFile;
    uint64_t m_maxBodySize = 0;   ///< 响应体大小上限(解压后字节;0 = 不限制)
    int64_t m_range = -1;
    std::string m_clientCertFile;
    std::string m_clientKeyFile;
};

/** @brief HTTP 响应(客户端内部构造,回调内只读) */
class ZmHttpClientResponse
{
public:
    int         Status() const { return m_status; }
    const std::string& Header(const char* name) const;
    const std::vector<std::pair<std::string, std::string>>& Headers() const { return m_headers; }
    const std::vector<BYTE>& Body() const { return m_body; }      ///< 全量模式
    uint64_t    ContentLength() const { return m_contentLength; }

private:
    friend class ZmHttpClient;
    friend class ZmHttpClientPrivate;   // 实现类(引擎收口/组装,见 cpp)
    int  m_status = 0;
    std::vector<std::pair<std::string, std::string>> m_headers;
    std::vector<BYTE> m_body;
    uint64_t m_contentLength = 0;
};

/** @brief 请求结果(回调参数,调用方在回调返回后不得再使用;同步模式由调用方 delete) */
class ZmHttpClientResult
{
public:
    int         Error() const { return m_error; }                 ///< ZmHttpClientError
    const char* ErrorText() const { return m_errorText.c_str(); }
    const ZmHttpClientResponse& Response() const { return m_response; }
    bool        Ok() const { return m_error == ZM_HTTPC_OK; }

private:
    friend class ZmHttpClient;
    friend class ZmHttpClientPrivate;   // 实现类(引擎收口/组装,见 cpp)
    int    m_error = ZM_HTTPC_OK;
    std::string m_errorText;
    ZmHttpClientResponse m_response;
};

/** @brief 异步完成回调
 *  @param result 结果(回调返回后失效)
 *  @param id     请求标识(请求时透传)
 *  @param params 请求参数(请求时透传) */
using ZmHttpClientCallback = std::function<void(ZmHttpClientResult* result, uint64_t id, void* params)>;

// ============================================================================
// ZmHttpClient — HTTP/S 客户端(自带独立事件循环)
// ============================================================================

class ZmHttpClient
{
public:
    explicit ZmHttpClient(const std::string& name);
    virtual ~ZmHttpClient();

    bool Start();                                    ///< 启动循环线程
    bool SetDnsServers(const char* servers);         ///< 手动 DNS(逗号分隔,如 "8.8.8.8,114.114.114.114";空/未设置 → 系统默认);循环运行中调用即时生效(循环线程应用)

    // TLS 证书配置(客户端级,内部经 ZmSSLContext 构建 SSL_CTX)
    // 契约:启动后、发请求前调用;变更将在下次 HTTPS 请求生效(懒重建);
    //      不得与在飞 HTTPS 请求并发变更
    bool SetClientCert(const char* certFile, const char* keyFile);   ///< mTLS 客户端证书
    void SetVerifyMode(bool verifyPeer, const char* caFile = nullptr); ///< CA 校验;false = 跳过(测试用)

    // 同步:阻塞直到完成/超时;返回结果对象(所有权归调用方,std::unique_ptr 自动释放;
    // 未启动/失败/超时亦返回错误对象,不返回 nullptr)
    std::unique_ptr<ZmHttpClientResult> Send(const ZmHttpClientRequest& req);
    // 异步:立即返回;回调在客户端循环线程执行,保证恰好触发一次;
    // id 由调用方自选(请用低 63 位,建议单调;高位区间 0x8000... 保留给同步请求
    // 内部使用 —— Cancel 按 id 匹配,两区间隔离防误杀)
    void SendAsync(const ZmHttpClientRequest& req, uint64_t id, void* params, ZmHttpClientCallback cb);

    void Cancel(uint64_t id);                        ///< 取消指定 id 的请求(异步 id 用低 63 位;同步请求 id 内部占用高位区间)
    void CancelAll();
    void CloseAll();                                 ///< 销毁全部连接(回池前置)

    bool IsLooped() const;
    const std::string& Name() const { return m_name; }

private:
    friend class ZmHttpClientPrivate;    // 实现类(见 cpp)

    ZmHttpClientPrivate* m_priv;         ///< 实现(循环/在飞请求/证书等),Start() 时创建
    std::string          m_name;
};

#endif // ZM_NET_HTTP_CLIENT_H
