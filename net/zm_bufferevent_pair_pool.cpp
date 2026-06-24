#include "zm_bufferevent_pair_pool.h"

#include "../spdlog/zm_logger.h"

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

// ============================================================================
// BuffereventPairHandle
// ============================================================================

void BuffereventPairHandle::Pair0EOF()
{
    if (bev0)
        bufferevent_trigger_event(bev0, BEV_EVENT_EOF, BEV_OPT_DEFER_CALLBACKS);
}

void BuffereventPairHandle::TryReturn()
{
    if (pair0_done.load(std::memory_order_acquire) &&
        pair1_done.load(std::memory_order_acquire))
    {
        Reset();
        if (owner_)
            owner_->Return(this);
    }
}

void BuffereventPairHandle::Reset()
{
    if (bev0)
    {
        bufferevent_disable(bev0, EV_READ | EV_WRITE);
        struct evbuffer* input  = bufferevent_get_input(bev0);
        struct evbuffer* output = bufferevent_get_output(bev0);
        if (input)  evbuffer_drain(input,  evbuffer_get_length(input));
        if (output) evbuffer_drain(output, evbuffer_get_length(output));
        bufferevent_setcb(bev0, nullptr, nullptr, nullptr, nullptr);
    }
    if (bev1)
    {
        bufferevent_disable(bev1, EV_READ | EV_WRITE);
        struct evbuffer* input  = bufferevent_get_input(bev1);
        struct evbuffer* output = bufferevent_get_output(bev1);
        if (input)  evbuffer_drain(input,  evbuffer_get_length(input));
        if (output) evbuffer_drain(output, evbuffer_get_length(output));
        bufferevent_setcb(bev1, nullptr, nullptr, nullptr, nullptr);
    }
    pair0_done.store(false, std::memory_order_release);
    pair1_done.store(false, std::memory_order_release);
}

// ============================================================================
// BuffereventPairPool
// ============================================================================

BuffereventPairPool::BuffereventPairPool()
    : m_evbase(nullptr)
{
}

BuffereventPairPool::~BuffereventPairPool()
{
    Shutdown();
}

void BuffereventPairPool::Init(struct event_base* evbase, int capacity)
{
    m_evbase = evbase;
    for (int i = 0; i < capacity; i++)
    {
        m_slots.emplace_back();
        auto& h = m_slots.back();
        h.owner_ = this;

        struct bufferevent* p[2] = { nullptr, nullptr };
        if (bufferevent_pair_new(m_evbase,
                BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE, p) == 0)
        {
            h.bev0 = p[0];
            h.bev1 = p[1];
            m_free_stack.push_back(&h);
        }
    }
    PUBLIC_LOG_INFO("BuffereventPairPool initialized: capacity={}, created={}",
        capacity, (int)m_free_stack.size());
}

void BuffereventPairPool::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& h : m_slots)
    {
        if (h.bev0) { bufferevent_free(h.bev0); h.bev0 = nullptr; }
        if (h.bev1) { bufferevent_free(h.bev1); h.bev1 = nullptr; }
    }
    m_free_stack.clear();
    m_slots.clear();
    m_evbase = nullptr;
}

BuffereventPairHandle* BuffereventPairPool::Acquire()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_free_stack.empty())
    {
        if (!Grow())
            return nullptr;
    }

    auto* h = m_free_stack.back();
    m_free_stack.pop_back();
    return h;
}

void BuffereventPairPool::Return(BuffereventPairHandle* h)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_free_stack.push_back(h);
}

bool BuffereventPairPool::Grow()
{
    // 调用者已持有 m_mutex
    m_slots.emplace_back();
    auto& h = m_slots.back();
    h.owner_ = this;

    struct bufferevent* p[2] = { nullptr, nullptr };
    if (bufferevent_pair_new(m_evbase,
            BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE, p) != 0)
    {
        m_slots.pop_back();
        return false;
    }
    h.bev0 = p[0];
    h.bev1 = p[1];
    m_free_stack.push_back(&h);

    PUBLIC_LOG_INFO("BuffereventPairPool grow: total slots={}", (int)m_slots.size());
    return true;
}
