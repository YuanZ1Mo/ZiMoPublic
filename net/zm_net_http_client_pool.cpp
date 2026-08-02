#include "zm_net_http_client_pool.h"

#include "zm_net_http_client.h"
#include "zm_logger.h"

#include <chrono>

ZmHttpClientPool::ZmHttpClientPool()
    : m_maxCount(0), m_shutdown(false)
{
}

ZmHttpClientPool::~ZmHttpClientPool()
{
    Shutdown();
}

bool ZmHttpClientPool::Init(int preCreate, int maxCount)
{
    m_maxCount = maxCount;
    for (int i = 0; i < preCreate; ++i)
    {
        auto* c = new ZmHttpClient("HttpClient");
        if (!c->Start())
        {
            PUBLIC_LOG_ERROR("ZmHttpClientPool::Init: client start failed");
            delete c;
            for (auto* created : m_all)
                delete created;
            m_all.clear();
            m_idle.clear();
            return false;   // 已清理,失败后池可重试 Init
        }
        m_all.push_back(c);
        m_idle.push_back(c);
    }
    return true;
}

ZmHttpClient* ZmHttpClientPool::Acquire(int timeoutMs, const std::atomic<bool>* abort)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!m_shutdown)
    {
        if (!m_idle.empty())
        {
            auto* c = m_idle.back();
            m_idle.pop_back();
            return c;
        }
        // deadline/abort 检查先于扩容(镜像 ZmReqLoopPool):已过期或已中止,不再创建实例
        if (std::chrono::steady_clock::now() >= deadline || (abort && abort->load()))
            return nullptr;
        if ((int)m_all.size() < m_maxCount)
        {
            auto* c = new ZmHttpClient("HttpClient");
            if (!c->Start())
            {
                PUBLIC_LOG_ERROR("ZmHttpClientPool::Acquire: client start failed");
                delete c;
                return nullptr;
            }
            m_all.push_back(c);
            return c;
        }
        m_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return nullptr;
}

bool ZmHttpClientPool::Release(ZmHttpClient* client)
{
    if (!client)
        return false;
    // 纯净契约:回池前必须无残留(在飞请求已收口、连接已销毁由调用方负责;
    // 池侧做幂等保护:同一实例重复 Release 返回 false)
    bool pushed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown)
            return false;   // 池已终止,拒绝回池(防悬垂指针)
        for (auto* c : m_idle)
            if (c == client)
                return false;   // 已在池中
        m_idle.push_back(client);
        pushed = true;
    }
    if (pushed)
        m_cv.notify_one();   // 锁外 notify
    return true;
}

void ZmHttpClientPool::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown)
            return;
        m_shutdown = true;
    }
    m_cv.notify_all();
    for (auto* c : m_all)
        delete c;
    m_all.clear();
    m_idle.clear();
}
