#ifndef ZM_NET_HTTP_CLIENT_DOWNLOAD_H
#define ZM_NET_HTTP_CLIENT_DOWNLOAD_H

/**
 * @file zm_net_http_client_download.h
 * @brief 流式下载通道 ZmHttpDownloadChannel(设计二期 §15;写线程 + 指令队列直写盘)
 *
 * 设计要点(docs/designs/2026-09-01-drogon-httpclient-design.md §15):
 *  - 载体:自建 HttpClient-DL(单 trantor::EventLoopThread,Init 时启动、Close 时退出);
 *    **与 drogon app() / 服务器三面完全隔离**。dlLoop 零磁盘操作(磁盘只在写线程与工作池)。
 *  - 续传协议(§15.1):**.part 现有大小即续传起点 N**(唯一事实源,无 offset 持久化);
 *    变更检测用 **If-Range**(强 ETag 优先,退 Last-Modified;两者皆无则只按 Content-Range
 *    校验并告警);侧车 .part.meta 仅 {etag,lastModified},**每响应头后一次写**(经写线程);
 *    分支表:206+Content-Range 首字节==N → 续写;200 → **原连接续读 + 就地截断**(不重连);
 *    3xx 带 Location → 换目标跟随(跨目标剥敏感头,上限 maxRedirects);
 *    206 首字节≠N / 416 / 412 → 断开截断重连一次(epoch 守卫),仍异常 → BadResponse。
 *  - 写盘模型(§15.2):**每会话一个写线程**,dlLoop 经有界指令队列(WRITE/TRUNCATE/
 *    WRITE_META/FINISH/ABORT)投递;队列字节上限 downloadQueueMaxBytes(默认 64MB),
 *    触顶 ABORT(.part/.meta 保留可续传);写阻塞由 dlLoop CancelIoEx 解除;
 *    FINISH 由写线程确认"队列排空+句柄关闭+改名成功"后回调——残余未落盘类缺陷结构性不存在;
 *    被取消的写按真实原因回执(不报"写盘失败")。
 *  - 解析器(§15.3):fail-closed——chunk size 行必须全 hex、数据块后必须 CRLF;
 *    206 必须有可验证 Content-Range;响应 Content-Encoding 非 identity 当场终结;
 *    Content-Length 与 Transfer-Encoding 冲突、响应体超 CL、畸形状态行均 BadResponse 终结。
 *  - 生命周期(§15.4):连接代数(epoch)守卫,重连窗口内旧连接回调被忽略;
 *    文件打开/侧车读取由 StartDownload 提交客户端工作池执行(不占调用方线程);
 *    Done 回调线程 = 会话写线程(会话创建失败时 = 工作池线程),恰好一次。
 *
 * 设计:ZiMoService docs/designs/2026-09-01-drogon-httpclient-design.md §15
 */

#include "zm_net_http_client.h"

/// 单次下载的完成回调(线程 = 会话写线程,或会话创建失败时 = 客户端工作池线程;恰好一次)
class ZmHttpDownloadChannel
{
  public:
    using DoneFn = std::function<void(ZmHttpClient::ZmDownloadResult&&)>;

    // —— 生命周期(Init/Close 三步序 ①;幂等) ——
    static bool Start();                 ///< 启动通道(创建 dl 事件循环线程并等待就绪)
    static void Shutdown();              ///< 停收新任务 → 在飞会话中止(写线程收尾)→ quit + join
    static bool IsRunning();

    /// 提交一次下载(立即返回;受理后结果一律经 done 回调)
    /// @return true 已受理(URL 非法、文件打开失败等会话级失败也走 done 回执);
    ///         false 通道未运行或工作池已停(此时 done 不会被调用)
    /// @param err false 时回填原因(可空)
    static bool StartDownload(const std::string& url, const std::string& destPath,
                              ZmHttpClient::ZmHttpRequestOptionsPtr opts, DoneFn done,
                              std::string* err = nullptr);
};

#endif  // ZM_NET_HTTP_CLIENT_DOWNLOAD_H
