#ifndef ZM_NET_TAP_RESTFUL_H
#define ZM_NET_TAP_RESTFUL_H

#include "zm_net_tap.h"

#include <string>
#include <vector>
#include <functional>
#include <memory>

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
