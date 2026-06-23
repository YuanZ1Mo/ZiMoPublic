#ifndef ZM_BUFFEREVENT_PAIR_POOL_H
#define ZM_BUFFEREVENT_PAIR_POOL_H

#include <deque>
#include <vector>

struct bufferevent;
struct event_base;

/**
 * @brief bufferevent_pair 池句柄
 *
 * 每个句柄持有一对 bufferevent_pair（bev0 响应端 / bev1 TAP 端）。
 * 两端各自独立释放，最后一端触发 Reset + 归还池。
 *
 * ReleasePair1() 内置双回调防护：若业务层已通过 MarkDataWritten()
 * 标记数据已写出，则不触发 EOF——pair0 读到数据后自行回收，无第二事件。
 */
struct BuffereventPairHandle
{
    struct bufferevent* bev0;  ///< 响应端（原 pair[0]）
    struct bufferevent* bev1;  ///< TAP 端（原 pair[1]）

    /// 释放响应端
    void ReleasePair0();

    /// 释放 TAP 端（data_written 为真时跳过 EOF，从根源消灭双回调）
    void ReleasePair1();

    /// TAP 端EOF
    void Pair1EOF();

    /// 标记数据已写入并刷新（WriteResponse flush 后调用）
    void MarkDataWritten();

    /// 异常路径：两端均未送出时直接中止并回收
    void Cancel();

private:
    friend class BuffereventPairPool;
    bool                 pair0_done   = false; ///< 响应端已归还
    bool                 pair1_done   = false; ///< TAP 端已归还
    bool                 data_written = false; ///< 数据已写出（ReleasePair1 跳过 EOF）
    BuffereventPairPool* owner_       = nullptr;

    /// 两端都 done → Reset() → 归还
    void TryReturn();

    /// 重置 bufferevent 状态：disable + drain + setcb(nullptr)
    void Reset();
};

/**
 * @brief bufferevent_pair 对象池
 *
 * 预创建固定数量的 bufferevent_pair，空闲时 O(1) 获取。
 * 池耗尽时自动扩容（单次创建一个，归还后进入空闲栈复用）。
 * 使用 std::deque 保证扩容时已外借的句柄指针不失效。
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

    struct event_base*               m_evbase;
    std::deque<BuffereventPairHandle> m_slots;       ///< 槽位队列（push_back 不失效已有指针）
    std::vector<BuffereventPairHandle*> m_free_stack; ///< 空闲栈
};

#endif // ZM_BUFFEREVENT_PAIR_POOL_H
