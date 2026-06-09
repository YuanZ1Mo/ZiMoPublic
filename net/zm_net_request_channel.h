#ifndef ZM_NET_REQUEST_CHANNEL_H
#define ZM_NET_REQUEST_CHANNEL_H

#include <string>
#include <future>
#include <memory>
#include <mutex>
#include <deque>
#include <atomic>
#include <functional>

#include <event2/event.h>

/**
 * @brief 单个异步请求项，包含请求数据、序列号和响应 promise
 *
 * promise 由调用线程通过 future 等待，由事件循环线程在处理完成后设置。
 * 使用 shared_ptr 管理生命周期，确保异步回调触发前对象存活，
 * 同时满足 std::function 对可拷贝捕获的要求。
 */
struct ZmNetRequestItem
{
    uint64_t                  seq_id;           ///< 请求序列号（递增，用于日志追踪）
    std::string               request_json;     ///< 请求数据字符串
    std::promise<std::string> response_promise; ///< 响应 promise（移动语义，不可拷贝）
};

/**
 * @brief 进程内异步请求通道，实现任意线程 → 事件循环线程的双向通信
 *
 * 通过 SPSC 队列 + std::promise/future + event_active 唤醒实现：
 *   - 调用线程：Submit(请求) → 获取 future → wait_for() 等待响应
 *   - 事件循环线程：event_active 唤醒入口 → Drain() 批量处理 → 回调 set_value(响应)
 *
 * 处理回调（RequestHandler）在事件循环线程中调用，负责将请求注入实际处理链路。
 * 回调立即返回（仅设置处理管线），响应在后续事件循环迭代中异步到达。
 *
 * 用途：替代 evutil_socketpair 做进程内跨线程通信。对比 socketpair 优势：
 *   - 零内核穿越（纯用户态 shared_ptr 队列节点交换）
 *   - 无 socket 生命周期问题（无需 shutdown 顺序、无 fd 泄漏风险）
 *   - 关闭时主动 reject 所有 pending promise 唤醒等待线程，根除关闭死锁
 *
 * 线程安全：Submit 可任意线程调用；Drain / Close 仅在事件循环线程调用。
 *
 * @example 基本用法
 * @code
 *   ZmNetRequestChannel channel;
 *   channel.Open(evbase, [](const std::string& req, auto callback) {
 *       // 异步处理：注入处理链路，完成后调用 callback
 *       ProcessAsync(req, std::move(callback));
 *   });
 *
 *   // 其他线程
 *   auto future = channel.Submit("request data");
 *   std::string response = future.get(); // 阻塞等待
 *
 *   // 关闭
 *   channel.Close(evbase);
 * @endcode
 */
class ZmNetRequestChannel
{
public:
    /**
     * @brief 异步请求处理回调
     * @param request_json 请求数据字符串
     * @param callback     响应回调，处理完成后调用此函数传递响应（空字符串表示错误）
     *
     * 回调在事件循环线程中调用，但处理可能异步进行。回调可能在同一次调用栈中触发，
     * 也可能在后续事件循环迭代中触发。实现者负责在适当的时机调用 callback。
     */
    using RequestHandler = std::function<void(const std::string& request_json,
                                               std::function<void(std::string)> callback)>;

    ZmNetRequestChannel();
    ~ZmNetRequestChannel();

    // --- 禁止拷贝 ---
    ZmNetRequestChannel(const ZmNetRequestChannel&) = delete;
    ZmNetRequestChannel& operator=(const ZmNetRequestChannel&) = delete;

    /**
     * @brief 打开通道，创建持久化唤醒事件
     * @param evbase  事件循环基
     * @param handler 异步请求处理回调
     * @return true 成功，false 失败（evbase 为空或 event_new 失败）
     */
    bool Open(struct event_base* evbase, RequestHandler handler);

    /**
     * @brief 关闭通道，拒绝所有待处理请求并阻止新提交
     *
     * 将 event_free 投递到事件循环线程安全释放，避免跨线程操作 event_base。
     * 所有 pending 的 promise 被设置为空字符串（表示错误），
     * 使阻塞在 future.wait 上的调用线程安全退出。
     *
     * @param evbase 事件循环基（用于投递清理事件）
     */
    void Close(struct event_base* evbase);

    /**
     * @brief 提交请求（任意线程调用）
     * @param request_json 请求数据字符串
     * @return 响应的 future，调用线程通过 wait_for / get 获取结果
     *
     * 若通道已关闭（m_closed == true），返回已就绪的 future（空字符串表示错误）。
     * 提交成功后通过 event_active 唤醒事件循环线程。
     */
    std::future<std::string> Submit(const std::string& request_json);

    /**
     * @brief 排空并异步处理所有待处理请求（仅事件循环线程调用）
     *
     * 原子地取出队列中所有请求，逐个调用 m_handler 处理。
     * 每个请求携带一个回调闭包，回调中 set_value 对应的 promise。
     * 处理过程中新提交的请求留待下次 Drain 处理，避免单次调用饥饿。
     */
    void Drain();

private:
    /** @brief libevent 唤醒回调，转发到 Drain() */
    static void OnNotifyEvent(evutil_socket_t fd, short events, void* ctx);

    /** @brief 清理事件回调（用于将 event_free 投递到事件循环线程安全释放） */
    static void OnCleanupEvent(evutil_socket_t fd, short events, void* ctx);

    std::mutex    m_mutex;        ///< 保护 m_queue 的互斥锁
    std::deque<std::shared_ptr<ZmNetRequestItem>> m_queue; ///< 待处理请求队列
    struct event* m_notifyEvent;  ///< 持久化唤醒事件（fd=-1，event_active 触发）
    RequestHandler m_handler;     ///< 异步请求处理回调
    std::atomic<uint64_t> m_seq{0};      ///< 序列号生成器
    std::atomic<bool>      m_closed{false}; ///< 通道是否已关闭，阻止新提交
};

#endif // ZM_NET_REQUEST_CHANNEL_H
