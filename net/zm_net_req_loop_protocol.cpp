#include "zm_net_req_loop_protocol.h"

#include "zm_net_http.h"
#include "zm_logger.h"

#include <io.h>   // _close(ResponseFile 失败分支依赖,显式包含避免隐性头链依赖)
#include <vector>  // ResponseStreamStart 的 headers 打包

// ============================================================================
// 回复 helper — 投递式收敛(ZmReqLoop::PostToLoop(fn))
// ============================================================================
// 外部线程只做 PostToLoop(线程安全,带 epoch 快照);组装/发送/门/收尾全部在
// ZmReqLoop 线程执行。收益:
//   1. 外部线程不再触碰 task/loop 内部状态(关闭窗口内安全面只剩 PostToLoop 一次调用)
//   2. 关闭时 Shutdown 的 join 等的是 ZmReqLoop 线程 → fn 内的 task 操作必然发生在
//      doer 池销毁之前(doer 池删除在 pool Shutdown 返回之后)
//   3. epoch 校验:投递到执行之间请求已收尾(Release)则 fn 自动作废(回复丢弃)
// 代价:指针参数(Raw/Chunk)必须拷进闭包(数据所有权随投递走,零拷贝契约失效);
//       ResponseFile 的 fd 按值捕获,所有权随投递转移,门失败时 helper 负责 _close。
// ============================================================================

void ZmReqLoopJrpc::ResponseJson(ZmReqLoop* loop, const ZMJSON& rsp)
{
    if (loop->IsClosing()) return;   // 关闭门:快速失败(省一次投递)
    loop->PostToLoop([rsp](ZmReqLoop* l) {
        if (l->IsClosing() || !l->TryReply()) return;   // 关闭门/回复门(loop 线程内,读安全)
        ZmHttpdTask* task = l->Task();
        if (!task) return;   // 已释放(理论不可达),收尾责任在他人
        auto* jrpc = static_cast<ZmReqLoopJrpc*>(l);
        // 组装标准响应(信封 id/jsonrpc/method + result/error 二选一,含 JSONP/自定义头)
        ZmJsonRpcServer::BuildJsonRpcResponse(task, jrpc->m_envelope, rsp);
        task->TriggerReply();   // 投递 REPLY 到 HTTP event loop 实际发送
        l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // 同线程投 DONE,ProcessDone 统一 Release
    });
}

// ============================================================================
// ZmReqLoopRest — 回复 helper(投递式收敛;流式 start 在 loop 线程 CancelDeadline)
// ============================================================================

void ZmReqLoopRest::ResponseJson(ZmReqLoop* loop, int code, const ZMJSON& data)
{
    if (loop->IsClosing()) return;   // 关闭门:快速失败(省一次投递)
    loop->PostToLoop([code, data](ZmReqLoop* l) {
        if (l->IsClosing() || !l->TryReply()) return;   // 已被他人收尾,对方负责 Release(收尾责任闭环)
        ZmHttpdTask* task = l->Task();
        if (!task) return;   // 已释放(理论不可达),收尾责任在他人
        std::string json = data.dump();
        task->PutReplyHeader("Content-Type", "application/json; charset=utf-8");
        task->SetReply(code);
        task->SetReplyData((const BYTE*)json.c_str(), json.size());
        task->TriggerReply();
        l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // ★ ZmReqLoop 线程 ProcessDone 统一 Release
    });
}

void ZmReqLoopRest::ResponseError(ZmReqLoop* loop, int code, std::string_view msg)
{
    ZMJSON err = {{"error", {{"code", code}, {"message", std::string(msg)}}}};
    ResponseJson(loop, code, err);
}

void ZmReqLoopRest::ResponseEmpty(ZmReqLoop* loop, int code, const char* reason)
{
    if (loop->IsClosing()) return;   // 关闭门:快速失败
    std::string reasonStr = reason ? reason : "";   // 拷进闭包(nullptr 与 "" 在 SetReply 中等价)
    loop->PostToLoop([code, reasonStr = std::move(reasonStr)](ZmReqLoop* l) {
        if (l->IsClosing() || !l->TryReply()) return;   // 已被他人收尾,对方负责 Release
        ZmHttpdTask* task = l->Task();
        if (!task) return;
        task->SetReply(code, reasonStr.c_str());
        task->TriggerReply();
        l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);
    });
}

void ZmReqLoopRest::ResponseRaw(ZmReqLoop* loop, int code, const BYTE* data, size_t dlen,
                                const char* contentType)
{
    if (loop->IsClosing()) return;   // 关闭门:快速失败
    // ★ 零拷贝契约失效:data 指向外部缓冲区,投递是异步的 → 拷进闭包(数据所有权随投递走)
    std::string body(data ? (const char*)data : "", data ? dlen : 0);
    std::string ctype = contentType ? contentType : "text/plain; charset=utf-8";
    loop->PostToLoop([code, body = std::move(body), ctype = std::move(ctype)](ZmReqLoop* l) {
        if (l->IsClosing() || !l->TryReply()) return;   // 已被他人收尾,对方负责 Release
        ZmHttpdTask* task = l->Task();
        if (!task) return;
        task->PutReplyHeader("Content-Type", ctype.c_str());
        task->SetReply(code);
        if (!body.empty())
            task->SetReplyData((const BYTE*)body.data(), body.size());
        task->TriggerReply();
        l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);
    });
}

void ZmReqLoopRest::ResponseRedirect(ZmReqLoop* loop, const char* location, int code)
{
    if (loop->IsClosing()) return;   // 关闭门:快速失败
    std::string loc = location ? location : "";
    loop->PostToLoop([code, loc = std::move(loc)](ZmReqLoop* l) {
        if (l->IsClosing() || !l->TryReply()) return;   // 已被他人收尾,对方负责 Release
        ZmHttpdTask* task = l->Task();
        if (!task) return;
        task->PutReplyHeader("Location", loc.c_str());
        task->SetReply(code);
        task->TriggerReply();
        l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);
    });
}

void ZmReqLoopRest::ResponseFile(ZmReqLoop* loop, int fd, ev_off_t offset, ev_off_t length,
                                 const char* attachmentName, size_t downloadBps)
{
    if (loop->IsClosing())
    {
        if (fd >= 0) _close(fd);   // 关闭窗口:fd 无人接管,须关闭防泄漏
        return;
    }
    std::string disp = (attachmentName && attachmentName[0])
        ? std::string("attachment; filename=\"") + attachmentName + "\""
        : std::string();
    // fd 按值捕获:所有权随投递转移到 ZmReqLoop 线程(投递后外部线程不得再关 fd)
    loop->PostToLoop([fd, offset, length, disp = std::move(disp), downloadBps](ZmReqLoop* l) {
        if (l->IsClosing() || !l->TryReply())
        {
            if (fd >= 0) _close(fd);   // 门失败:fd 无人接管,须关闭防泄漏
            return;
        }
        ZmHttpdTask* task = l->Task();
        if (!task)
        {
            if (fd >= 0) _close(fd);
            return;
        }
        if (!disp.empty())
            task->PutReplyHeader("Content-Disposition", disp.c_str());
        if (downloadBps)
            task->SetRateLimit(downloadBps, 0);
        task->SetReply(200);   // ★ m_status_code 默认为 0,不设则发出非法状态行(旧实现同款)
        if (task->SetReplyFile(fd, offset, length) < 0)
        {
            if (fd >= 0) _close(fd);   // 失败不接管 fd
            task->SetReply(500);
        }
        task->TriggerReply();
        l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);
    });
}

void ZmReqLoopRest::ResponseStreamStart(ZmReqLoop* loop, int code,
    std::initializer_list<std::pair<const char*, const char*>> headers)
{
    if (loop->IsClosing()) return;   // 关闭门:丢弃(未取消 deadline,504 兜底收尾)
    std::vector<std::pair<std::string, std::string>> hs;
    for (const auto& h : headers)
        hs.emplace_back(h.first, h.second);
    loop->PostToLoop([code, hs = std::move(hs)](ZmReqLoop* l) {
        if (l->IsClosing()) return;   // 投递后进入关闭窗口:丢弃
        ZmHttpdTask* task = l->Task();
        if (!task) return;
        l->CancelDeadline();   // ★ 流式长任务:取消超时,避免 504 误杀(loop 线程内,event_del 安全)
        for (const auto& h : hs)
            task->PutReplyHeader(h.first.c_str(), h.second.c_str());
        task->StartStreamReply(code);
    });
}

void ZmReqLoopRest::ResponseStreamChunk(ZmReqLoop* loop, const BYTE* data, size_t dlen)
{
    if (loop->IsClosing()) return;   // 关闭门:丢弃
    // ★ 零拷贝契约失效:data 指向外部缓冲区 → 拷进闭包(音频帧级,成本可接受)
    std::string chunk(data ? (const char*)data : "", data ? dlen : 0);
    loop->PostToLoop([chunk = std::move(chunk)](ZmReqLoop* l) {
        if (l->IsClosing()) return;   // 投递后进入关闭窗口:丢弃
        ZmHttpdTask* task = l->Task();
        if (!task) return;
        task->SendReplyChunk((const BYTE*)chunk.data(), chunk.size());
    });
}

void ZmReqLoopRest::ResponseStreamEnd(ZmReqLoop* loop)
{
    if (loop->IsClosing())
    {
        // 关闭窗口:不碰 task(doer 池可能已销毁),仍投 DONE 收尾——
        // Start 已取消 deadline,不投 DONE 则 loop 无人收尾,Shutdown join 会挂死
        loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, loop->Task());
        return;
    }
    loop->PostToLoop([](ZmReqLoop* l) {
        if (l->IsClosing())
        {
            // 投递后进入关闭窗口:同上,仍须投 DONE 收尾
            l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, l->Task());
            return;
        }
        ZmHttpdTask* task = l->Task();
        if (!task) return;   // 已释放(理论不可达),收尾责任在他人
        task->EndStreamReply();    // 驱动 HTTP 循环 STREAM_END → 回收 doer
        l->TryReply();
        l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);   // ★ ZmReqLoop 线程 ProcessDone 统一 Release
    });
}

void ZmReqLoopRest::ResponseSSEStart(ZmReqLoop* loop)
{
    if (loop->IsClosing()) return;   // 关闭门:丢弃
    loop->PostToLoop([](ZmReqLoop* l) {
        if (l->IsClosing()) return;   // 投递后进入关闭窗口:丢弃
        ZmHttpdTask* task = l->Task();
        if (!task) return;
        l->CancelDeadline();   // ★ 同上(loop 线程内,event_del 安全)
        task->PutReplyHeader("Content-Type", "text/event-stream");
        task->PutReplyHeader("Cache-Control", "no-cache");
        task->StartStreamReply(200);
    });
}

void ZmReqLoopRest::ResponseSSEEvent(ZmReqLoop* loop, const ZMJSON& data)
{
    if (loop->IsClosing()) return;   // 关闭门:丢弃
    std::string line = "data: " + data.dump() + "\n\n";
    loop->PostToLoop([line = std::move(line)](ZmReqLoop* l) {
        if (l->IsClosing()) return;   // 投递后进入关闭窗口:丢弃
        ZmHttpdTask* task = l->Task();
        if (!task) return;
        task->SendReplyChunk((const BYTE*)line.c_str(), line.size());
    });
}

void ZmReqLoopRest::ResponseSSEEnd(ZmReqLoop* loop)
{
    ResponseStreamEnd(loop);
}
