#ifndef ZM_NET_REQ_LOOP_PROTOCOL_H
#define ZM_NET_REQ_LOOP_PROTOCOL_H

#include "zm_net_req_loop.h"
#include "../util/zm_util_json.h"   // ZMJSON(若实际路径不同,参照 zm_net_http.h 的 include 方式)

#include <functional>
#include <string_view>

/**
 * @brief JRPC 回复子类:承载 per-request 响应信封(id/jsonrpc/method),
 * 业务经静态 Response() 回复,与 ZmReqLoopRest 同构
 * (TryReply 门 → 服务器组装信封 → task 直通 → 投 DONE)。
 * 信封由服务器分发时经 SetEnvelope 写入,Response 时取出交给服务器组装,
 * 池复用前经 OnRequestReleased 清理(防旧信封污染下一请求)。
 */
class ZmReqLoopJrpc : public ZmReqLoop
{
public:
    /** @brief 保存本请求的响应信封(服务器分发时调用,仅本线程写入) */
    void SetEnvelope(ZMJSON envelope) { m_envelope = std::move(envelope); }

    /** @brief JRPC 回复(任意线程可调):TryReply 门 → 服务器组装信封并发送 → 投 DONE
     *  @param rsp 业务构造的响应 JSON(含 result 或 error,可选 headers);
     *             id/jsonrpc/method 信封按请求原样回传 */
    static void ResponseJson(ZmReqLoop* loop, const ZMJSON& rsp);

    /** @brief 覆写基类钩子:清理信封(防池复用后旧信封污染下一请求) */
    void OnRequestReleased() override { m_envelope = ZMJSON(); }

private:
    ZMJSON m_envelope;   ///< 本请求的响应信封(id/jsonrpc/method,仅本线程读写)
};

/**
 * @brief RESTful 回复子类:回复 helper 全部 task 直通;最终回复类 helper 内部 TryReply 门,
 * 回复后投 REQ_LOOP_SIG_DONE,收尾(Release 回池)统一由 ZmReqLoop 线程 ProcessDone 执行。
 * ResponseStreamStart/ResponseSSEStart 自动取消 deadline(流式长任务不被 504 误杀)。
 * @note 所有 helper **任意线程可调**(门保证单写者,收尾收敛到 ZmReqLoop 线程,无跨线程 Release);
 *       流式场景外部线程亦可沿用 task->EndStreamReply() + PostToLoop(REQ_LOOP_SIG_DONE, task) 模式。
 */
class ZmReqLoopRest : public ZmReqLoop
{
public:
    static void ResponseJson(ZmReqLoop* loop, int code, const ZMJSON& data);
    static void ResponseError(ZmReqLoop* loop, int code, std::string_view msg);
    static void ResponseEmpty(ZmReqLoop* loop, int code, const char* reason = nullptr);
    static void ResponseRaw(ZmReqLoop* loop, int code, const BYTE* data, size_t dlen,
                            const char* contentType = nullptr);
    static void ResponseRedirect(ZmReqLoop* loop, const char* location, int code = 302);
    static void ResponseFile(ZmReqLoop* loop, int fd, ev_off_t offset, ev_off_t length,
                             const char* attachmentName = nullptr, size_t downloadBps = 0);
    static void ResponseStreamStart(ZmReqLoop* loop, int code,
        std::initializer_list<std::pair<const char*, const char*>> headers = {});
    static void ResponseStreamChunk(ZmReqLoop* loop, const BYTE* data, size_t dlen);
    static void ResponseStreamEnd(ZmReqLoop* loop);
    static void ResponseSSEStart(ZmReqLoop* loop);
    static void ResponseSSEEvent(ZmReqLoop* loop, const ZMJSON& data);
    static void ResponseSSEEnd(ZmReqLoop* loop);
};

#endif // ZM_NET_REQ_LOOP_PROTOCOL_H
