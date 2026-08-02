#include "zm_net_req_loop_protocol.h"

#include "zm_net_http.h"
#include "zm_logger.h"

#include <io.h>   // _close(ResponseFile 失败分支依赖,显式包含避免隐性头链依赖)

// ============================================================================
// ZmReqLoopJrpc
// ============================================================================

void ZmReqLoopJrpc::Response(ZmReqLoop* loop, const ZMJSON& rsp)
{
    auto* jrpc = static_cast<ZmReqLoopJrpc*>(loop);
    if (!jrpc->TryReply())     // 已回复(超时/断连兜底先到):静默丢弃,收尾责任在门获得者
    {
        return;
    }
    if (jrpc->m_reply)
        jrpc->m_reply(rsp);    // 服务器侧 replyCB:BuildJsonRpcResponse + task->TriggerReply
    else if (jrpc->Task())
        jrpc->Task()->TriggerReply();   // 兜底:无回复函数时直接触发发送
    jrpc->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, jrpc->Task());   // ★ A 线程 ProcessDone 统一 Release(任意线程可调)
}

// ============================================================================
// ZmReqLoopRest — 回复 helper(实现仅替换旧式 task 直通为 loop->Task(),
// 并在最终回复 helper 内加入 TryReply 门 + Release;流式 start 自动 CancelDeadline)
// ============================================================================

void ZmReqLoopRest::ResponseJson(ZmReqLoop* loop, int code, const ZMJSON& data)
{
    if (!loop->TryReply()) return;   // 已被他人收尾,对方负责 Release(收尾责任闭环)
    ZmHttpdTask* task = loop->Task();
    if (!task) return;   // 已释放(理论不可达),收尾责任在他人
    std::string json = data.dump();
    task->PutReplyHeader("Content-Type", "application/json; charset=utf-8");
    task->SetReply(code);
    task->SetReplyData((const BYTE*)json.c_str(), json.size());
    task->TriggerReply();
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // ★ A 线程 ProcessDone 统一 Release(任意线程可调)
}

void ZmReqLoopRest::ResponseError(ZmReqLoop* loop, int code, std::string_view msg)
{
    ZMJSON err = {{"error", {{"code", code}, {"message", std::string(msg)}}}};
    ResponseJson(loop, code, err);
}

void ZmReqLoopRest::ResponseEmpty(ZmReqLoop* loop, int code, const char* reason)
{
    if (!loop->TryReply()) return;   // 已被他人收尾,对方负责 Release(收尾责任闭环)
    ZmHttpdTask* task = loop->Task();
    if (!task) return;   // 已释放(理论不可达),收尾责任在他人
    task->SetReply(code, reason);
    task->TriggerReply();
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // ★ A 线程 ProcessDone 统一 Release(任意线程可调)
}

void ZmReqLoopRest::ResponseRaw(ZmReqLoop* loop, int code, const BYTE* data, size_t dlen,
                                const char* contentType)
{
    if (!loop->TryReply()) return;   // 已被他人收尾,对方负责 Release(收尾责任闭环)
    ZmHttpdTask* task = loop->Task();
    if (!task) return;   // 已释放(理论不可达),收尾责任在他人
    task->PutReplyHeader("Content-Type", contentType ? contentType : "text/plain; charset=utf-8");
    task->SetReply(code);
    if (data && dlen)
        task->SetReplyData(data, dlen);
    task->TriggerReply();
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // ★ A 线程 ProcessDone 统一 Release(任意线程可调)
}

void ZmReqLoopRest::ResponseRedirect(ZmReqLoop* loop, const char* location, int code)
{
    if (!loop->TryReply()) return;   // 已被他人收尾,对方负责 Release(收尾责任闭环)
    ZmHttpdTask* task = loop->Task();
    if (!task) return;   // 已释放(理论不可达),收尾责任在他人
    task->PutReplyHeader("Location", location);
    task->SetReply(code);
    task->TriggerReply();
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // ★ A 线程 ProcessDone 统一 Release(任意线程可调)
}

void ZmReqLoopRest::ResponseFile(ZmReqLoop* loop, int fd, ev_off_t offset, ev_off_t length,
                                 const char* attachmentName, size_t downloadBps)
{
    if (!loop->TryReply()) return;   // 已被他人收尾,对方负责 Release(收尾责任闭环)
    ZmHttpdTask* task = loop->Task();
    if (!task) return;   // 已释放(理论不可达),收尾责任在他人
    if (attachmentName && attachmentName[0])
    {
        task->PutReplyHeader("Content-Disposition",
            std::string("attachment; filename=\"") + attachmentName + "\"");
    }
    if (downloadBps)
        task->SetRateLimit(downloadBps, 0);
    task->SetReply(200);   // ★ m_status_code 默认为 0,不设则发出非法状态行(旧实现同款)
    if (task->SetReplyFile(fd, offset, length) < 0)
    {
        if (fd >= 0) _close(fd);   // 失败不接管 fd:调用方须自行关闭
        task->SetReply(500);
    }
    task->TriggerReply();
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // ★ A 线程 ProcessDone 统一 Release(任意线程可调)
}

void ZmReqLoopRest::ResponseStreamStart(ZmReqLoop* loop, int code,
    std::initializer_list<std::pair<const char*, const char*>> headers)
{
    ZmHttpdTask* task = loop->Task();
    if (!task) return;
    loop->CancelDeadline();   // ★ 流式长任务:取消超时,避免 504 误杀
    for (const auto& h : headers)
        task->PutReplyHeader(h.first, h.second);
    task->StartStreamReply(code);
}

void ZmReqLoopRest::ResponseStreamChunk(ZmReqLoop* loop, const BYTE* data, size_t dlen)
{
    ZmHttpdTask* task = loop->Task();
    if (!task) return;
    task->SendReplyChunk(data, dlen);
}

void ZmReqLoopRest::ResponseStreamEnd(ZmReqLoop* loop)
{
    ZmHttpdTask* task = loop->Task();
    if (!task) return;   // 已释放(理论不可达),收尾责任在他人
    task->EndStreamReply();    // 驱动 HTTP 循环 STREAM_END → 回收 doer
    loop->TryReply();
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // ★ A 线程 ProcessDone 统一 Release
}

void ZmReqLoopRest::ResponseSSEStart(ZmReqLoop* loop)
{
    ZmHttpdTask* task = loop->Task();
    if (!task) return;
    loop->CancelDeadline();   // ★ 同上
    task->PutReplyHeader("Content-Type", "text/event-stream");
    task->PutReplyHeader("Cache-Control", "no-cache");
    task->StartStreamReply(200);
}

void ZmReqLoopRest::ResponseSSEEvent(ZmReqLoop* loop, const ZMJSON& data)
{
    ZmHttpdTask* task = loop->Task();
    if (!task) return;
    std::string line = "data: " + data.dump() + "\n\n";
    task->SendReplyChunk((const BYTE*)line.c_str(), line.size());
}

void ZmReqLoopRest::ResponseSSEEnd(ZmReqLoop* loop)
{
    ResponseStreamEnd(loop);
}
