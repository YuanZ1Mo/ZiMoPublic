#include "zm_net_tap_rest.h"

#include "../util/zm_util_thread.h"
#include "../spdlog/zm_logger.h"

#include <event2/buffer.h>

// ============================================================================
// ZmTapDelegateRESTful 构造 / 析构
// ============================================================================

ZmTapDelegateRESTful::ZmTapDelegateRESTful(struct event_base* evbase)
    : ZmTapDelegate(evbase)
    , m_threadPool(nullptr)
{
    TapDelegateName("ZmTapDelegateRESTful");
}

ZmTapDelegateRESTful::~ZmTapDelegateRESTful()
{
    StopThreadPool();
}

void ZmTapDelegateRESTful::SetRESTfulRequestCB(TapDelegateRESTfulRequestCB cb)
{
    m_requestCB = std::move(cb);
}

void ZmTapDelegateRESTful::StopThreadPool()
{
    if (m_threadPool)
    {
        delete m_threadPool;
        m_threadPool = nullptr;
    }
}

// ============================================================================
// ZmTapDelegate 接口
// ============================================================================

bool ZmTapDelegateRESTful::OnTapRequesterAccept(ZM_TAP_CTX* tap)
{
    return false;  // RESTful 不接受直连，只接受 Hub 注入
}

void ZmTapDelegateRESTful::OnTapDelegateEvent(short what)
{
    // RESTful 模式下无内部事件，空实现
}

void ZmTapDelegateRESTful::OnTapDrop(ZM_TAP_CTX* tap)
{
    // 清理未完成的帧解析状态（TAP 异常关闭时）
    if (tap->onback_data)
    {
        delete static_cast<FrameState*>(tap->onback_data);
        tap->onback_data = nullptr;
    }
}

void ZmTapDelegateRESTful::OnTapDelegateBackEvent(ZM_TAP_CTX* tap)
{
    // 响应不走 pair 回传，此回调不会被触发
    // 若被触发则丢弃 TAP
    PUBLIC_LOG_WARN("[RESTfulDelegate] 收到意外回传事件，丢弃 TAP");
    tap->Drop("unexpected back event");
}

// ============================================================================
// 帧解析状态机
// ============================================================================
//
// 帧格式（HttpRestfulManager 打包，Hub 协议探测后交给 delegate）:
//
//   ┌──────┬──────────┬──────────┐
//   │ REST │ body_len │ raw_body │
//   │ 4 B  │ 4 B BE   │  M bytes │
//   └──────┴──────────┴──────────┘
//     ↑                 ↑
//     Hub 消费并切换      delegate 从此处开始解析
//
// 状态机: READ_HEADER → (跳过) → READ_BODY_LEN → READ_BODY → DONE
//
bool ZmTapDelegateRESTful::TryParseFrame(ZM_TAP_CTX* tap, struct evbuffer* input, FrameState& state)
{
    while (state.stage != FrameState::DONE)
    {
        size_t avail = evbuffer_get_length(input);

        switch (state.stage)
        {
        case FrameState::READ_HEADER:
            state.stage = FrameState::READ_BODY_LEN;
            break;

        case FrameState::READ_BODY_LEN:
        {
            if (avail < 4) return false;
            uint32_t len;
            evbuffer_remove(input, &len, 4);
            state.body_len = ntohl(len);
            if (state.body_len > (512ULL * 1024 * 1024))
            {
                PUBLIC_LOG_ERROR("[RESTfulDelegate] body_len 异常: {}", state.body_len);
                tap->Drop("bad body length");
                return false;
            }
            state.stage = FrameState::READ_BODY;
        }
        break;

        case FrameState::READ_BODY:
        {
            if (avail < state.body_len) return false;
            if (state.body_len > 0)
            {
                state.body.resize(state.body_len);
                evbuffer_remove(input, &state.body[0], state.body_len);
            }
            state.stage = FrameState::DONE;
        }
        break;

        case FrameState::DONE:
            break;
        }
    }
    return true;
}

// ============================================================================
// 帧到达处理 — OnTapRequesterRead
// ============================================================================
//
// 数据到达后会多次调用本方法，直到一帧完整:
//   1. 从 tap->onback_data 取出本次请求的 FrameState（首次则为新建）
//   2. TryParseFrame 尝试从 input 读取数据直到一帧完整或数据不足
//   3. 帧完整后: 解析 meta_json → 连同 body 投递到工作线程 → 调业务回调
//   4. 业务回调中通过 tap->httpd_task 直接回写 HTTP 响应
//
// httpd_task 由 OnPairAcceptBev 在 TAP 创建时设置，无需 delegate 自行查找
//
void ZmTapDelegateRESTful::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
    // ① 从 tap 上取出或新建帧解析状态（复用 onback_data 字段存指针）
    FrameState* state = static_cast<FrameState*>(tap->onback_data);
    if (!state)
    {
        state = new FrameState();
        tap->onback_data = state;
    }

    // ② 尝试从输入缓冲区读取一帧
    if (TryParseFrame(tap, app_input, *state))
    {
        tap->onback_data = nullptr;
        auto* doneState = state;
        state = nullptr;

        // 把 body 打包投递到工作线程
        auto* bodyVec = new std::vector<char>(std::move(doneState->body));
        delete doneState;

        if (!m_threadPool) m_threadPool = new ZmThreadPool(4, "RESTfulDelegate");

        m_threadPool->Submit([this, tap, bodyVec]() {
            const BYTE* body = bodyVec->empty() ? nullptr
                : reinterpret_cast<const BYTE*>(bodyVec->data());
            size_t bodyLen = bodyVec->size();

            if (m_requestCB)
            {
                m_requestCB(tap, body, bodyLen);
            }
            else
            {
                PUBLIC_LOG_WARN("[RESTfulDelegate] 未设置业务回调");
                if (tap->httpd_task)
                {
                    tap->httpd_task->SetReply(501, "Not Implemented");
                    tap->httpd_task->TriggerReply();
                }
            }

            delete bodyVec;
            });
    }
}

// ============================================================================
// 调度（预留，当前在 OnTapRequesterRead 中直接完成）
// ============================================================================

void ZmTapDelegateRESTful::ParseAndDispatch(ZM_TAP_CTX* tap)
{
    // 由 OnTapRequesterRead 直接在工作线程中调用 m_requestCB
    // 此方法保留供将来扩展
}
