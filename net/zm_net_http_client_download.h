#ifndef ZM_NET_HTTP_CLIENT_DOWNLOAD_H
#define ZM_NET_HTTP_CLIENT_DOWNLOAD_H

/**
 * @file zm_net_http_client_download.h
 * @brief 流式下载通道 ZmHttpDownloadChannel(设计 §10;trantor 直写盘)
 *
 * 设计要点(实现约束,均经 spike/源码核实):
 *  - 载体:自建 HttpClient-DL(单 trantor::EventLoopThread,Init 时启动、Close 时退出);
 *    **与 drogon app() / 服务器三面完全隔离**。
 *  - DNS:**TcpClient 构造只收 InetAddress**,故 connect 前必须经
 *    trantor::Resolver::newResolver(dlLoop) 异步解析;禁止在通道 loop 内同步 getaddrinfo。
 *    **注意:resolve 回调的 InetAddress 端口恒为 0**——须回填
 *    setPortNetEndian(htons(url端口)) 后再 connect。
 *  - TLS:enableSSL(TLSPolicyPtr) **必须在 connect() 之前**;客户端策略 =
 *    TLSPolicy::defaultClientPolicy(hostname) + setCaPath(trustCA) + 保持系统证书库
 *    (setUseSystemCertStore(false)+仅 caPath = 完全不校验,禁止)。mTLS 经 setCertPath/setKeyPath。
 *  - 请求:手写 HTTP/1.1 GET(行 = GET path?query HTTP/1.1;头 = Host/User-Agent/
 *    Range(续传)/If-Match(.part.meta.etag)/Connection: close/Accept-Encoding: identity
 *    + 全局与请求级头注入)。
 *  - 响应:极简自研解析(状态行/状态码/Content-Length/Content-Range/Transfer-Encoding/
 *    Location;100 Continue 跳过);非 2xx/非 206 → 错误终结不落盘。
 *  - **直写盘**:读回调缓冲至 downloadChunkBytes 后一次顺序 WriteFile(方案 A;本通道是
 *    "事件循环绝不磁盘读写"铁律的定向豁免——专属 loop,单次写超阈值即 abort 止损);
 *    单次写 > downloadWriteMaxMs 或停滞 > downloadStallAbortMs → abort(保留 .part/.meta)。
 *  - 续传:先写 destPath + ".part",完成后 MoveFileExW(..., MOVEFILE_REPLACE_EXISTING) 覆盖;
 *    侧写 .part.meta(ZMJSON:url/etag/lastModified/offset);分支表见设计 §10.3
 *    (206 校验首字节;200 → 截断从头;Content-Range 不符/412 → 截断从头)。
 *
 * 设计:ZiMoService docs/designs/2026-09-01-drogon-httpclient-design.md §10
 */

#include "zm_net_http_client.h"

/// 单次下载的完成回调(线程 = HttpClient-DL 事件循环线程或发起线程)
class ZmHttpDownloadChannel
{
  public:
    using DoneFn = std::function<void(ZmHttpClient::ZmDownloadResult&&)>;

    // —— 生命周期(Init/Close 三步序 ①;幂等) ——
    static bool Start();                 ///< 启动通道(创建 dl 事件循环线程并等待就绪)
    static void Shutdown();              ///< 停收新任务 → 全部 disconnect → 关文件 → quit + join
    static bool IsRunning();

    /// 提交一次下载(立即返回;结果经 done 回调,线程 = dl loop)
    /// @return true 已受理;false 通道未运行/参数非法(此时 done 不会被调用)
    static bool StartDownload(const std::string& url, const std::string& destPath,
                              ZmHttpClient::ZmHttpRequestOptionsPtr opts, DoneFn done);
};

#endif  // ZM_NET_HTTP_CLIENT_DOWNLOAD_H
