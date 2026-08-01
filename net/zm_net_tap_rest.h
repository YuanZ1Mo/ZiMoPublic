#ifndef ZM_NET_TAP_RESTFUL_H
#define ZM_NET_TAP_RESTFUL_H

#include "zm_net_tap.h"

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>
#include <initializer_list>

class ZmThreadPool;

/**
 * @brief RESTful 请求到达时的业务回调
 * @param tap      TAP 上下文（tap->httpd_task 指向 HTTP 请求，业务层通过它回写响应）
 * @param meta     请求元信息 JSON: {method, path, headers}
 * @param body     请求体原始字节指针（仅在回调期间有效）
 * @param body_len 请求体长度
 */
using TapDelegateRESTfulRequestCB = std::function<void(
    ZM_TAP_CTX* tap, const BYTE* body, size_t body_len)>;

/**
 * @brief RESTful 协议委托，解析 Hub 转发来的 RESTful 请求帧并分发到业务回调
 *
 * 帧格式（由 HttpRestfulManager 打包）:
 *   [4 字节 "REST"][4 字节大端 body_len][raw_body 字节]
 *
 * method/path/headers 业务层直接从 tap->httpd_task 读取，不打包进帧。
 */
class ZmTapDelegateRESTful : public ZmTapDelegate
{
public:
    ZmTapDelegateRESTful(struct event_base* evbase);
    virtual ~ZmTapDelegateRESTful();

    /** @brief 设置 RESTful 请求回调（业务层注册） */
    void SetRESTfulRequestCB(TapDelegateRESTfulRequestCB cb);

    /** @brief 停止内部线程池 */
    void StopThreadPool();

    // ---- ZmTapDelegate 接口 ----
    bool OnTapRequesterAccept(ZM_TAP_CTX* tap) override;
    void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) override;
    void OnTapDelegateBackEvent(ZM_TAP_CTX* tap) override;
    void OnTapDelegateEvent(short what) override;
    void OnTapDrop(ZM_TAP_CTX* tap) override;

public:
    // --- 跨线程安全响应方法 ---
    // 所有方法均可在任意线程调用;内部检查 TAP 有效性(state == ZM_TAP_STATE_INUSE),
    // 已失效则静默丢弃,防止 UAF。完整返回类写完响应后自动 tap->Drop(),
    // 流式类由 ResponseStreamEnd / ResponseSSEEnd 统一收尾(Drop)。

    /** @brief 快捷返回 JSON 响应(Content-Type: application/json),写完自动 Drop */
    static void ResponseJson(ZM_TAP_CTX* tap, int code, const ZMJSON& data);

    /** @brief 快捷返回 JSON 错误响应 {"error":{"code":...,"message":...}},写完自动 Drop */
    static void ResponseError(ZM_TAP_CTX* tap, int code, std::string_view msg);

    /** @brief 快捷返回空响应体(204 No Content、304 Not Modified 等),写完自动 Drop */
    static void ResponseEmpty(ZM_TAP_CTX* tap, int code, const char* reason = nullptr);

    /** @brief 快捷返回原始文本/二进制完整响应(自定义 Content-Type),写完自动 Drop
     *  @param contentType nullptr 时默认 "text/plain; charset=utf-8" */
    static void ResponseRaw(ZM_TAP_CTX* tap, int code, const BYTE* data, size_t dlen,
                            const char* contentType = nullptr);

    /** @brief 快捷返回重定向(301/302/307,自动设置 Location 头),写完自动 Drop */
    static void ResponseRedirect(ZM_TAP_CTX* tap, const char* location, int code = 302);

    /** @brief 快捷零拷贝发送文件(SetReplyFile,mmap/sendfile),写完自动 Drop
     *  @param fd            文件句柄,函数接管所有权:成功由 libevent 传输后自动关闭,失败由本方法关闭
     *  @param offset/length 文件发送区间
     *  @param attachmentName 非空时设置 Content-Disposition: attachment; filename="..."
     *  @param downloadBps   单连接下载限速(0 = 不限速) */
    static void ResponseFile(ZM_TAP_CTX* tap, int fd, ev_off_t offset, ev_off_t length,
                             const char* attachmentName = nullptr, size_t downloadBps = 0);

    /** @brief 开始流式响应(仅发送响应头,Transfer-Encoding: chunked),不 Drop
     *  @param headers 响应头列表,如 {{"Content-Type","text/event-stream"},{"Cache-Control","no-cache"}} */
    static void ResponseStreamStart(ZM_TAP_CTX* tap, int code,
        std::initializer_list<std::pair<const char*, const char*>> headers = {});

    /** @brief 发送一个流式数据块,不 Drop */
    static void ResponseStreamChunk(ZM_TAP_CTX* tap, const BYTE* data, size_t dlen);

    /** @brief 结束流式响应(发送终止块),随后自动 Drop */
    static void ResponseStreamEnd(ZM_TAP_CTX* tap);

    /** @brief 开始 SSE 推送(Content-Type: text/event-stream + Cache-Control: no-cache),不 Drop */
    static void ResponseSSEStart(ZM_TAP_CTX* tap);

    /** @brief 发送一条 SSE 事件 "data: {json}\n\n",不 Drop */
    static void ResponseSSEEvent(ZM_TAP_CTX* tap, const ZMJSON& data);

    /** @brief 结束 SSE 推送,随后自动 Drop */
    static void ResponseSSEEnd(ZM_TAP_CTX* tap);

private:
    /** @brief 解析帧并分发到业务回调（在工作线程中执行） */
    void ParseAndDispatch(ZM_TAP_CTX* tap);

    /** @brief 按帧格式从 evbuffer 读取数据 */
    struct FrameState
    {
        enum Stage { READ_HEADER, READ_BODY_LEN, READ_BODY, DONE };
        Stage    stage = READ_HEADER;
        uint32_t body_len = 0;
        std::vector<char> body;
    };

    /** @brief 尝试解析一帧（可能多次调用才能读完一帧） */
    bool TryParseFrame(ZM_TAP_CTX* tap, struct evbuffer* input, FrameState& state);

    TapDelegateRESTfulRequestCB m_requestCB;    ///< 业务层注册的回调
    ZmThreadPool*               m_threadPool;   ///< 业务处理线程池
};

#endif  // ZM_NET_TAP_RESTFUL_H
