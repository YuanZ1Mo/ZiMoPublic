#include "zm_net_req_loop_pool.h"

#include "zm_net_req_loop.h"   // ZmReqLoop 完整定义(Loop/SetPool/Stop/MarkClosing 调用)
#include "../util/zm_util_logger.h"

#include <chrono>   // 排队轮询片(steady_clock)

namespace {
/// 排队轮询片:醒来复查 abort 标志与 deadline
constexpr auto kPollSlice = std::chrono::milliseconds(50);
}

ZmReqLoopPool::ZmReqLoopPool()
    : m_maxCount(0), m_budgetMs(5000), m_shutdown(false)
{
}

ZmReqLoopPool::~ZmReqLoopPool()
{
    Shutdown();
}

bool ZmReqLoopPool::Init(int preCreate, int maxCount, uint32_t businessBudgetMs)
{
    m_maxCount = maxCount;
    m_budgetMs = businessBudgetMs;

    for (int i = 0; i < preCreate; ++i)
    {
        auto* loop = m_factory ? m_factory() : new ZmReqLoop();
        loop->SetPool(this);
        if (!loop->Loop())
        {
            PUBLIC_LOG_ERROR("ZmReqLoopPool::Init failed: ZmReqLoop::Loop()");
            delete loop;   // 本次创建失败的 loop(未入容器)
            // 失败路径清理:停掉并释放已创建的 loop,复位容器,保证失败后池可重试 Init
            for (auto* created : m_all)
            {
                created->Stop();
                delete created;
            }
            m_all.clear();
            m_idle.clear();
            return false;
        }
        m_all.push_back(loop);
        m_idle.push_back(loop);
    }
    PUBLIC_LOG_INFO("ZmReqLoopPool::Init: preCreate={}, maxCount={}, budgetMs={}",
        preCreate, maxCount, businessBudgetMs);
    return true;
}

ZmReqLoop* ZmReqLoopPool::Acquire(int timeoutMs, const std::atomic<bool>* abort)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (!m_shutdown)
    {
        if (!m_idle.empty())
        {
            auto* loop = m_idle.back();
            m_idle.pop_back();
            return loop;
        }
        // deadline/abort 检查先于扩容:请求已过期或客户端已断,不再为其创建线程
        if (std::chrono::steady_clock::now() >= deadline || (abort && abort->load()))
            return nullptr;
        if ((int)m_all.size() < m_maxCount)   // 扩容
        {
            // 持锁安全的依据:新线程只触碰自己的 m_mutexLoop/m_cvLoop,不触碰池锁;
            // Release 在 Loop() 返回后才可能发生(此时本分支已 push 进 m_all/m_idle 完成)。
            auto* loop = m_factory ? m_factory() : new ZmReqLoop();
            loop->SetPool(this);
            if (!loop->Loop())
            {
                PUBLIC_LOG_ERROR("ZmReqLoopPool::Acquire: loop start failed");
                delete loop;
                return nullptr;
            }
            m_all.push_back(loop);
            return loop;
        }
        m_cv.wait_for(lock, kPollSlice);   // 50ms 片,醒来复查 abort
    }
    return nullptr;
}

void ZmReqLoopPool::Release(ZmReqLoop* loop)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_idle.push_back(loop);
    }
    m_cv.notify_one();
}

void ZmReqLoopPool::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown)
            return;
        m_shutdown = true;
    }
    m_cv.notify_all();

    // 置位关闭门:外部线程(音频/SSE 等)在关闭窗口内调 Response* 时检查后丢弃,
    // 防访问正在销毁的 loop/task(缩窄 UAF 窗口;须在 join/delete 之前完成)
    for (auto* loop : m_all)
        loop->MarkClosing();

    for (auto* loop : m_all)
        loop->Stop();          // join 等待 ZmReqLoop 线程退出
    for (auto* loop : m_all)
        delete loop;
    m_all.clear();
    m_idle.clear();
}

// ============================================================================
// ZmReqLoopPoolReturn — 桥接定义(ZmReqLoop::Release 经自由函数回池)
// zm_net_req_loop.h 仅前向声明 ZmReqLoopPool,故函数保持自由函数形态;
// 本文件已包含完整 ZmReqLoopPool 定义。
// ============================================================================

void ZmReqLoopPoolReturn(ZmReqLoopPool* pool, ZmReqLoop* loop)
{
    pool->Release(loop);
}
