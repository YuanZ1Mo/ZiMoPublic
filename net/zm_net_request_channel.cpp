#include "zm_net_request_channel.h"
#include "../spdlog/zm_logger.h"

ZmNetRequestChannel::ZmNetRequestChannel()
    : m_notifyEvent(nullptr)
{
}

ZmNetRequestChannel::~ZmNetRequestChannel()
{
    if (m_notifyEvent)
    {
        PUBLIC_LOG_WARN("ZmNetRequestChannel 析构时仍有未释放的 notify event，可能泄漏");
    }
}

bool ZmNetRequestChannel::Open(struct event_base* evbase, RequestHandler handler)
{
    if (!evbase)
    {
        PUBLIC_LOG_ERROR("ZmNetRequestChannel::Open 失败：evbase 为空");
        return false;
    }

    m_handler = std::move(handler);
    m_closed = false;

    m_notifyEvent = event_new(evbase, -1, EV_PERSIST, OnNotifyEvent, this);
    if (!m_notifyEvent)
    {
        PUBLIC_LOG_ERROR("ZmNetRequestChannel::Open 失败：event_new 返回 NULL");
        return false;
    }
    event_add(m_notifyEvent, NULL);

    PUBLIC_LOG_INFO("ZmNetRequestChannel 已打开");
    return true;
}

void ZmNetRequestChannel::Close(struct event_base* evbase)
{
    if (m_closed)
        return;

    // 1. 阻止新提交并拒绝所有待处理请求
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;

        for (auto& req : m_queue)
        {
            try
            {
                req->response_promise.set_value(std::string());
            }
            catch (const std::future_error&)
            {
                // promise 已被设置（不应发生，兜底）
            }
        }
        m_queue.clear();
    }

    // 2. 将 event_free 投递到事件循环线程安全释放
    if (m_notifyEvent && evbase)
    {
        struct event* cleanupEvent = event_new(evbase, -1, 0, OnCleanupEvent, m_notifyEvent);
        if (cleanupEvent)
        {
            event_active(cleanupEvent, 0, 0);
        }
        else
        {
            PUBLIC_LOG_WARN("ZmNetRequestChannel::Close 无法创建清理事件，直接释放 notify event");
            event_free(m_notifyEvent);
        }
        m_notifyEvent = nullptr;
    }

    PUBLIC_LOG_INFO("ZmNetRequestChannel 已关闭");
}

std::future<std::string> ZmNetRequestChannel::Submit(const std::string& request_json)
{
    auto req = std::make_shared<ZmNetRequestItem>();
    req->seq_id = m_seq.fetch_add(1, std::memory_order_relaxed);
    req->request_json = request_json;
    auto future = req->response_promise.get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
        {
            try
            {
                req->response_promise.set_value(std::string());
            }
            catch (const std::future_error&) {}
            return future;
        }
        m_queue.push_back(std::move(req));
    }

    // 唤醒事件循环线程（event_active 线程安全）
    if (m_notifyEvent)
        event_active(m_notifyEvent, 0, 0);

    return future;
}

void ZmNetRequestChannel::Drain()
{
    // 原子地取出所有待处理请求（交换 deque 避免长期持锁）
    std::deque<std::shared_ptr<ZmNetRequestItem>> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending.swap(m_queue);
    }

    if (pending.empty())
        return;

    for (auto& req : pending)
    {
        // 按值捕获 shared_ptr 确保异步回调触发前 ZmNetRequestItem 存活
        // shared_ptr 可拷贝，满足 std::function 的 CopyConstructible 要求
        auto requestJson = req->request_json;

        m_handler(requestJson,
            [req](std::string response) {
                try
                {
                    req->response_promise.set_value(std::move(response));
                }
                catch (const std::future_error&)
                {
                    // promise 已被设置（如通道关闭），忽略
                }
            });
    }
}

// ============================================================================
// 静态 libevent 回调
// ============================================================================

void ZmNetRequestChannel::OnNotifyEvent(evutil_socket_t /*fd*/, short /*events*/, void* ctx)
{
    auto* channel = static_cast<ZmNetRequestChannel*>(ctx);
    channel->Drain();
}

void ZmNetRequestChannel::OnCleanupEvent(evutil_socket_t /*fd*/, short /*events*/, void* ctx)
{
    struct event* notifyEvent = static_cast<struct event*>(ctx);
    event_free(notifyEvent);
}
