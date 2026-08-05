#ifndef ZM_NET_REQ_LOOP_POOL_H
#define ZM_NET_REQ_LOOP_POOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

// 前向声明(本头仅通过指针使用 ZmReqLoop)
class ZmReqLoop;

/** @brief 池回收桥:ZmReqLoop::Release 经此回池。
 *  zm_net_req_loop.h 仅前向声明 ZmReqLoopPool(成员调用需完整类型),故经自由函数间接调用;
 *  定义位于 zm_net_req_loop_pool.cpp(内部 pool->Release(loop)) */
void ZmReqLoopPoolReturn(class ZmReqLoopPool* pool, class ZmReqLoop* loop);

/**
 * @brief ZmReqLoop 池:预创建 + 扩容(上限) + 排队(doer 线程等待,不阻塞 HTTP 事件循环)
 *
 * 每台 HTTP 服务器各自持有一个实例。排队等待以 50ms 片轮询,谓词 =
 * (有空闲 ZmReqLoop || abort 标志置位(客户端已断));带超时,超时返回 nullptr(调用方回 503)。
 */
class ZmReqLoopPool
{
public:
    ZmReqLoopPool();
    ~ZmReqLoopPool();

    /** @brief 初始化:预创建 + 扩容上限 + 业务预算
     *  @param preCreate     预创建数(建议 hardware_concurrency,低并发可调小)
     *  @param maxCount      扩容上限(= 并发业务上限)
     *  @param businessBudgetMs 业务预算毫秒(deadline = 请求到达 + 预算) */
    bool Init(int preCreate, int maxCount, uint32_t businessBudgetMs);

    /** @brief 获取空闲 ZmReqLoop;无空闲且未达上限则扩容;达上限排队
     *  @param timeoutMs  排队等待上限(剩余预算)
     *  @param abort      可空;非空时该原子标志置位则提前放弃(客户端已断)
     *  @return 空闲 ZmReqLoop,失败(超时/中止/关闭)返回 nullptr */
    ZmReqLoop* Acquire(int timeoutMs, const std::atomic<bool>* abort);

    /** @brief 归还空闲 ZmReqLoop(ZmReqLoop::Release 内部调用) */
    void Release(ZmReqLoop* loop);

    /** @brief 关闭:停止全部 ZmReqLoop 线程(join)并释放(在飞业务受 deadline 约束,可 join 完成)
     *  @warning 调用方须保证无在飞 Acquire 结果仍被使用(先停请求入口,再 Shutdown;
     *           在飞业务受 deadline 约束可 join 完成) */
    void Shutdown();

    /** @brief 业务预算毫秒 */
    uint32_t BudgetMs() const { return m_budgetMs; }

    /** @brief 设置 loop 工厂(默认 new ZmReqLoop();回复全部 task 直通,基类实例即可)
     *  @note 必须在 Init() 之前调用,否则预创建出的仍是基类实例
     *  @note 工厂必须返回非空实例(返回 nullptr 将导致 SetPool 空解引用) */
    void SetLoopFactory(std::function<ZmReqLoop*()> factory) { m_factory = std::move(factory); }

private:
    std::vector<ZmReqLoop*> m_all;    ///< 全部实例(含 busy)
    std::vector<ZmReqLoop*> m_idle;   ///< 空闲栈
    std::function<ZmReqLoop*()> m_factory;   ///< loop 工厂(未设置时创建基类 ZmReqLoop)
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    int                     m_maxCount;
    uint32_t                m_budgetMs;
    bool                    m_shutdown;
};

#endif // ZM_NET_REQ_LOOP_POOL_H
