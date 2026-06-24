#ifndef ZM_BUFFEREVENT_PAIR_POOL_H
#define ZM_BUFFEREVENT_PAIR_POOL_H

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

struct bufferevent;
struct event_base;

/**
 * @brief bufferevent_pair 池句柄
 *
 * 每个句柄持有一对 bufferevent_pair（bev0 响应端 / bev1 TAP 端）。
 *
 * === 双事件互斥 ===
 *
 * pair1 的两种结局（Responded 或 EOF）最终都会在 bev0 侧汇合为两个事件：
 *   OnResponseRead  / OnResponseEvent
 *
 * === 回收条件 ===
 * 请自己按需回收
 */
struct BuffereventPairHandle
{
    struct bufferevent* bev0;  ///< 响应端（原 pair[0]）
    struct bufferevent* bev1;  ///< TAP 端（原 pair[1]）

    BuffereventPairHandle() = default;

    // std::deque 需要 MoveInsertable（atomic 不可拷贝/移动，需手动实现）
    BuffereventPairHandle(BuffereventPairHandle&& other) noexcept
        : bev0(other.bev0), bev1(other.bev1)
        , pair0_done(other.pair0_done.load(std::memory_order_relaxed))
        , pair1_done(other.pair1_done.load(std::memory_order_relaxed))
        , owner_(other.owner_)
    {
        other.bev0 = nullptr;
        other.bev1 = nullptr;
        other.owner_ = nullptr;
    }

    BuffereventPairHandle& operator=(BuffereventPairHandle&&) = delete;
    BuffereventPairHandle(const BuffereventPairHandle&) = delete;
    BuffereventPairHandle& operator=(const BuffereventPairHandle&) = delete;

    // ========================================================================
    // pair1 终端操作（由 FreeRequesterEnd 调用）
    // ========================================================================

    /**
     * @brief pair1 关闭——触发 BEV_EVENT_EOF 到 bev0
     *
     * WriteResponse flush 后 Drop 走到 FreeRequesterEnd 调用本方法。
     * 正常路径下 pair0 的数据可能已到、也可能未到——不影响，
     * 事件循环线程上 OnResponseRead 和 OnResponseEvent 排队处理。
     */
    void Pair0EOF();

    // ========================================================================
    // pair0 完成操作（由 OnResponseEvent 调用）
    // ========================================================================

    /**
     * @brief pair0 侧完成（OnResponseEvent 中调用）
     *
     * 标记 pair0 完成；若 pair1 也已 done，立即回收 pair 对。
     */
    void ReleasePair0()
    {
        pair0_done.store(true, std::memory_order_release);
        TryReturn();
    }

    void ReleasePair1()
    {
        pair1_done.store(true, std::memory_order_release);
        TryReturn();
    }

    /**
     * @brief 异常路径：强制回收 pair 对（两端均未完成时）
     *
     * 适用于：evbuffer_add 失败、OnPairAcceptBev 失败等
     * pair1 尚未注入 Hub 代理链的场景。
     */
    void ReleasePair()
    {
        pair0_done.store(true, std::memory_order_release);
        pair1_done.store(true, std::memory_order_release);
        TryReturn();
    }

private:
    friend class BuffereventPairPool;

    std::atomic<bool>    pair0_done = false;  ///< 响应端已释放
    std::atomic<bool>    pair1_done = false;  ///< TAP 端已释放
    BuffereventPairPool* owner_     = nullptr;

    /// 两端都完成后 Reset() → 归还池
    void TryReturn();

    /// 重置 bufferevent 状态：disable + drain + setcb(nullptr) + 重置 atomics
    void Reset();
};

/**
 * @brief bufferevent_pair 对象池
 *
 * 预创建固定数量的 bufferevent_pair，空闲时 O(1) 获取。
 * 池耗尽时自动扩容（单次创建一个，归还后进入空闲栈复用）。
 * 使用 std::deque 保证扩容时已外借的句柄指针不失效。
 * Acquire/Return 加锁保证跨线程原子操作。
 */
class BuffereventPairPool
{
public:
    BuffereventPairPool();
    ~BuffereventPairPool();

    /// 预创建池
    void Init(struct event_base* evbase, int capacity);

    /// 销毁池中所有 pair
    void Shutdown();

    /// 获取句柄（O(1)，池耗尽时自动扩容），仅 bufferevent_pair_new 失败时返回 nullptr
    BuffereventPairHandle* Acquire();

private:
    friend struct BuffereventPairHandle;

    /// 句柄两端都释放后由 TryReturn 调用
    void Return(BuffereventPairHandle* h);

    /// 池耗尽时创建新句柄
    bool Grow();

    struct event_base*                m_evbase;
    std::mutex                        m_mutex;         ///< 保护 m_free_stack 的跨线程访问
    std::deque<BuffereventPairHandle> m_slots;         ///< 槽位队列（push_back 不失效已有指针）
    std::vector<BuffereventPairHandle*> m_free_stack;  ///< 空闲栈
};

#endif // ZM_BUFFEREVENT_PAIR_POOL_H
