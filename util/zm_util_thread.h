#ifndef ZM_UTIL_THREAD_H
#define ZM_UTIL_THREAD_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>


// ============================ ZmThread (C++20 jthread) ============================

/**
 * @brief 线程状态枚举, 使用位掩码设计, 支持组合查询
 *
 * 状态流转: STOPPED ->(Start) STARTING -> RUNNING ->(Stop) STOPPING -> STOPPED
 */
enum ZM_THD_STATE {
    ZM_THD_STATE_STOPPED  = 1 << 0,   ///< 线程已停止或尚未启动
    ZM_THD_STATE_STARTING = 1 << 1,   ///< Start() 已调用, 线程正在初始化
    ZM_THD_STATE_RUNNING  = 1 << 2,   ///< 线程正在执行 Run()
    ZM_THD_STATE_STOPPING = 1 << 3,   ///< Stop() 已调用, 等待线程退出
};

/// 位与运算, 用于状态判断: if (state & ZM_THD_STATE_RUNNING)
inline ZM_THD_STATE operator&(ZM_THD_STATE lhs, ZM_THD_STATE rhs)
{
    using T = std::underlying_type_t<ZM_THD_STATE>;
    return static_cast<ZM_THD_STATE>(static_cast<T>(lhs) & static_cast<T>(rhs));
}

/// 位或运算, 用于状态组合
inline ZM_THD_STATE operator|(ZM_THD_STATE lhs, ZM_THD_STATE rhs)
{
    using T = std::underlying_type_t<ZM_THD_STATE>;
    return static_cast<ZM_THD_STATE>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

/// 位取反运算
inline ZM_THD_STATE operator~(ZM_THD_STATE state)
{
    using T = std::underlying_type_t<ZM_THD_STATE>;
    return static_cast<ZM_THD_STATE>(~static_cast<T>(state));
}

/**
 * @brief 检查 state 中是否包含指定的 flag
 * @param state  当前状态 (支持位掩码组合)
 * @param flag   目标标志
 * @return true 表示 state 包含 flag
 */
inline bool ZmThdHasState(ZM_THD_STATE state, ZM_THD_STATE flag)
{
    return (state & flag) == flag;
}

/**
 * @brief 基于 std::jthread (C++20) 的线程封装, 支持同步启停和状态管理
 *
 * 提供两种使用模式:
 *   1. callable 模式 - 构造时传入函数对象, 适合简单场景
 *   2. 继承模式     - 子类 override Run(), 适合需要复杂逻辑的场景
 *
 * @example callable + stop_token (循环任务, 可协作停止)
 * @code
 *   ZmThread t("worker", [](std::stop_token st) {
 *       while (!st.stop_requested()) {
 *           DoWork();
 *       }
 *   });
 *   t.Start();
 *   t.Stop();
 * @endcode
 *
 * @example callable 无 stop_token (一次性任务, 自然结束)
 * @code
 *   ZmThread t("once", []() { DoSomething(); });
 *   t.Start();  // 执行完后自动进入 STOPPED
 * @endcode
 *
 * @example 继承模式
 * @code
 *   class MyWorker : public ZmThread {
 *   protected:
 *       void Run(std::stop_token st) override {
 *           while (!st.stop_requested()) { DoWork(); }
 *       }
 *   public:
 *       MyWorker() : ZmThread("my_worker") {}
 *   };
 * @endcode
 */
class ZmThread
{
public:
    // --- 构造 / 析构 ---

    /**
     * @brief callable 模式构造, 传入带 stop_token 的工作函数
     * @param name  线程名称, 在调试器中可见 (Windows 下通过 SetThreadDescription)
     * @param func  工作函数, 应在循环中检查 st.stop_requested() 以支持协作停止
     */
    ZmThread(const std::string& name, std::function<void(std::stop_token)> func);

    /**
     * @brief callable 模式构造, 传入无 stop_token 的工作函数
     * @param name  线程名称
     * @param func  工作函数, 函数执行完毕后线程自然进入 STOPPED
     *
     * @note 适用于一次性任务, Stop() 只能等函数自然跑完
     */
    ZmThread(const std::string& name, std::function<void()> func);

    /**
     * @brief 继承模式构造, 子类应 override Run() 或 Run(st)
     * @param name  线程名称
     */
    explicit ZmThread(const std::string& name = "zm_thread");

    /**
     * @brief 析构时自动调用 Stop(), 确保线程不会泄漏
     */
    virtual ~ZmThread();

    // --- 禁止拷贝 / 移动 ---

    ZmThread(const ZmThread&) = delete;
    ZmThread& operator=(const ZmThread&) = delete;
    ZmThread(ZmThread&&) = delete;
    ZmThread& operator=(ZmThread&&) = delete;

    // --- 线程控制 ---

    /**
     * @brief 同步启动线程, 阻塞直到线程进入 RUNNING 状态
     * @return true  启动成功
     * @return false 启动失败 (线程已在运行, 或创建线程失败)
     *
     * 内部通过 std::promise 实现同步等待:
     *   主线程创建 jthread 后阻塞在 future.wait(),
     *   工作线程设置好状态后 set_value() 唤醒主线程
     *
     * @example
     * @code
     *   ZmThread t("w", [](std::stop_token st) { while (!st.stop_requested()) {} });
     *   if (t.Start()) {
     *       printf("线程已启动, id=%d\n", t.ThreadID());
     *   }
     * @endcode
     */
    bool Start();

    /**
     * @brief 同步停止线程, 阻塞直到线程进入 STOPPED 状态
     *
     * 执行流程:
     *   1. 将状态设为 STOPPING
     *   2. 调用 OnStopping()  (子类可重写, 用于打断阻塞调用)
     *   3. 调用 request_stop() (设置 stop_token, 通知工作函数应退出)
     *   4. 等待状态变为 STOPPED (通过 condition_variable)
     *   5. join 线程
     *
     * @note Stop() 是线程安全的, 多线程并发调用同一 ZmThread 对象是安全的
     */
    void Stop();

    // --- 状态查询 ---

    /**
     * @brief 获取当前线程状态
     * @return 原子读取的 ZM_THD_STATE 值
     */
    ZM_THD_STATE GetState() const;

    /// @return 线程是否处于 STOPPED 状态
    bool IsStopped() const;
    /// @return 线程是否处于 RUNNING 状态
    bool IsRunning() const;
    /// @return 线程是否处于 STOPPING 状态 (已请求停止但尚未退出)
    bool IsStopping() const;

    // --- 属性访问 ---

    /**
     * @brief 获取线程名称
     * @return 线程名称的 const 引用
     */
    const std::string& GetName() const;

    /**
     * @brief 设置线程名称
     * @param name  新名称
     * @note 仅修改内部存储, 不会更新 OS 级线程名 (OS 级名称在 Start 时设置)
     */
    void SetName(const std::string& name);

    /**
     * @brief 获取工作线程的系统线程 ID
     * @return 线程未运行时返回默认的 std::thread::id()
     */
    std::thread::id ThreadID() const;

    /**
     * @brief 获取当前线程的系统线程 ID
     * @return 当前线程的 std::thread::id()
     */
    static std::thread::id CurrentThreadID();

    // --- 配置 ---

    /**
     * @brief 设置线程结束后是否自动销毁对象
     * @param autoDel  true 表示线程结束后自动 delete this
     *
     * @note 前提条件:
     *   - 对象必须通过 new 创建 (堆分配)
     *   - 设置后不得再手动 delete 或通过智能指针释放
     *   - 设置后不得再访问该指针 (线程结束后指针悬空)
     *   - 设置后不应再调用 Stop(), 析构函数等 (由线程自行处理退出)
     *
     * @example
     * @code
     *   auto* t = new ZmThread("bg", [](std::stop_token st) {
     *       while (!st.stop_requested()) { DoWork(); }
     *   });
     *   t->SetAutoDelete(true);
     *   t->Start();
     *   // t 指针之后不可再使用
     * @endcode
     */
    void SetAutoDelete(bool autoDel);

    /// @return 是否启用了自动销毁
    bool IsAutoDelete() const;

protected:
    // --- 虚函数 (继承模式) ---

    /**
     * @brief 带 stop_token 的工作函数, 继承模式下子类可重写
     * @param st  jthread 内部的停止令牌, 循环中应检查 st.stop_requested()
     *
     * 默认实现: 如果构造时传入了 m_func 则调用 m_func(st), 否则回退调用 Run()
     *
     * @example
     * @code
     *   class MyThread : public ZmThread {
     *   protected:
     *       void Run(std::stop_token st) override {
     *           while (!st.stop_requested()) { DoWork(); }
     *       }
     *   };
     * @endcode
     */
    virtual void Run(std::stop_token st);

    /**
     * @brief 无 stop_token 的简单工作函数, 继承模式下子类可重写
     *
     * 默认实现: 如果构造时传入了 m_func0 则调用 m_func0()
     * 当 Run(st) 未被覆盖且 m_func 未设置时, 会回退调用此方法
     *
     * @example
     * @code
     *   class OnceTask : public ZmThread {
     *   protected:
     *       void Run() override {
     *           DoSomething();  // 执行完线程自动停止
     *       }
     *   };
     * @endcode
     */
    virtual void Run();

    /**
     * @brief Stop() 中 request_stop() 之前触发, 用于打断 Run() 中的阻塞调用
     *
     * 默认空实现. 当工作线程内部运行的是无法响应 stop_token 的阻塞 API 时
     * (如 libevent 的 event_base_dispatch), 需要在此方法中调用该 API 的打断函数
     *
     * @example libevent 场景
     * @code
     *   class EvThread : public ZmThread {
     *       event_base* m_base;
     *   protected:
     *       void Run(std::stop_token st) override {
     *           event_base_dispatch(m_base);  // 阻塞, 不会检查 stop_token
     *       }
     *       void OnStopping() override {
     *           event_base_loopexit(m_base, nullptr);  // 打断 dispatch
     *       }
     *   };
     * @endcode
     */
    virtual void OnStopping();

private:
    /**
     * @brief jthread 的实际入口函数
     * @param st       jthread 传递的 stop_token
     * @param started  用于同步 Start() 的 promise, set_value() 后 Start() 返回
     *
     * 执行流程:
     *   1. 设置状态 RUNNING, 记录线程 ID
     *   2. (Windows) 调用 SetThreadDescription 设置 OS 级线程名
     *   3. set_value() 唤醒 Start() 的等待
     *   4. 执行 Run(st)
     *   5. 设置状态 STOPPED, 清除线程 ID, 通知 CV
     */
    void runInternal(std::stop_token st, std::promise<void> started);

    std::string m_name;
    std::atomic<ZM_THD_STATE> m_state{ ZM_THD_STATE_STOPPED };
    std::jthread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::function<void(std::stop_token)> m_func;    ///< callable 带 stop_token 模式的工作函数
    std::function<void()> m_func0;                   ///< callable 无 stop_token 模式的工作函数
    std::atomic<std::thread::id> m_threadId;
    std::atomic<bool> m_autoDelete{ false };          ///< 线程结束后是否自动 delete this
};


// ============================ ZmThreadPool ============================

/// 线程池最大线程数上限, submitImmediate 自动增长时不会超过此值
#define ZM_POOL_MAX_THREADS 128

/**
 * @brief 线程池, 支持立即执行、延迟执行和取消任务
 *
 * 内部结构:
 *   - N 个 worker 线程: 从任务队列取任务执行, 无任务时阻塞等待
 *   - 1 个 timer 线程:  管理延迟任务, 到期后转入 worker 的任务队列
 *
 * @example 基本使用
 * @code
 *   ZmThreadPool pool(4);
 *   pool.Submit([]() { printf("hello\n"); });           // 立即执行
 *   uint32_t id = pool.Submit([](){}, 5000);             // 5秒后执行
 *   pool.Cancel(id);                                     // 取消延迟任务
 * @endcode
 */
class ZmThreadPool
{
public:
    // --- 构造 / 析构 ---

    /**
     * @brief 构造线程池并启动 worker 线程和 timer 线程
     * @param threadCount  初始 worker 线程数量, 默认 4
     */
    explicit ZmThreadPool(uint16_t threadCount = 4);

    /**
     * @brief 析构时停止所有线程 (request_stop + notify + join)
     */
    ~ZmThreadPool();

    // --- 禁止拷贝 ---

    ZmThreadPool(const ZmThreadPool&) = delete;
    ZmThreadPool& operator=(const ZmThreadPool&) = delete;

    // --- 任务提交 ---

    /**
     * @brief 提交任务到线程池
     * @param task     任务函数
     * @param delayMs  延迟毫秒数, 0 表示立即执行
     * @return delayMs > 0 时返回任务 ID (用于 Cancel), delayMs = 0 时返回 0
     *
     * @example
     * @code
     *   pool.Submit([]() { DoWork(); });              // 立即执行, 返回 0
     *   uint32_t id = pool.Submit([](){}, 3000);      // 3秒后执行, 返回任务ID
     *   pool.Cancel(id);                               // 取消 (仅限未执行的)
     * @endcode
     */
    uint32_t Submit(std::function<void()> task, uint32_t delayMs = 0);

    /**
     * @brief 提交带返回值的任务, 立即执行
     * @tparam F      可调用对象类型
     * @tparam Args   参数类型
     * @param f       可调用对象
     * @param args    传给 f 的参数
     * @return std::future<ReturnType> 用于异步获取任务返回值
     *
     * @example
     * @code
     *   auto fut = pool.Async([]() -> int {
     *       return DoHeavyWork();
     *   });
     *   int result = fut.get();  // 阻塞等待结果
     * @endcode
     */
    template<class F, class... Args>
    auto Async(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
    {
        using RetType = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<RetType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        auto fut = task->get_future();
        Submit([task]() { (*task)(); });
        return fut;
    }

    /**
     * @brief 取消延迟任务
     * @param taskId  Submit 返回的任务 ID
     * @return true 取消成功, false 任务已执行或 ID 无效
     * @note 仅能取消 delayMs > 0 且尚未到期执行的任务
     */
    bool Cancel(uint32_t taskId);

    // --- 状态查询 ---

    /// @return worker 线程总数 (含自动增长的)
    uint16_t ThreadCount() const;
    /// @return 当前空闲的 worker 线程数
    uint16_t IdleCount() const;

    // --- 全局线程池接口 ---

    /**
     * @brief 通过全局线程池提交延迟任务 (无参)
     * @param executor  任务函数
     * @param delayMs   延迟毫秒数
     * @return 任务 ID
     */
    static uint32_t InvokeLater(std::function<void()> executor, uint32_t delayMs = 0);

    /**
     * @brief 通过全局线程池提交延迟任务 (带 void* 参数)
     * @param executor  任务函数
     * @param param     传给 executor 的参数
     * @param delayMs   延迟毫秒数
     * @return 任务 ID
     */
    static uint32_t InvokeLater(std::function<void(void*)> executor, void* param, uint32_t delayMs = 0);

    /**
     * @brief 通过全局线程池取消延迟任务
     * @param taskId  任务 ID
     * @return true 取消成功
     */
    static bool InvokeCancel(uint32_t taskId);

private:
    /** @brief worker 线程主循环: 从 m_tasks 取任务执行, 无任务时阻塞等待 */
    void workerProc(std::stop_token st);

    /**
     * @brief timer 线程主循环: 管理延迟任务, 到期后转入 worker 队列
     *
     * 工作流程:
     *   1. 等待 m_delayed 非空 (无延迟任务时休眠, 不占 CPU)
     *   2. 找到最早截止时间的任务, wait_for 精确等待到期
     *      - 中途有新任务插入 (Submit 调用 notify_all) 会打断等待, 重新评估最早截止时间
     *   3. 收集所有已到期任务, 移入 worker 的任务队列
     */
    void timerProc(std::stop_token st);

    /**
     * @brief 将任务放入 worker 任务队列, 并唤醒一个 worker
     *
     * 自动增长: 如果没有空闲 worker 且总线程数未达 ZM_POOL_MAX_THREADS, 则新建一个 worker
     */
    void submitImmediate(std::function<void()> task);

    struct DelayedTask {
        uint32_t id;                       ///< 任务 ID, 由 Submit 分配
        std::function<void()> task;        ///< 任务函数
        uint64_t deadline;                 ///< 截止时间 (毫秒时间戳)
    };

    // --- 成员变量 ---
    std::vector<std::jthread> m_workers;           ///< worker 线程集合
    uint16_t m_initSize;                            ///< 初始 worker 线程数
    std::jthread m_timerThread;                     ///< 延迟任务调度线程

    mutable std::mutex m_taskMutex;                 ///< 保护 m_tasks 和 m_idleCount
    std::condition_variable m_taskCv;               ///< worker 等待此 CV 获取新任务
    std::queue<std::function<void()>> m_tasks;
    std::atomic<uint16_t> m_idleCount{ 0 };         ///< 当前空闲 worker 数

    mutable std::mutex m_delayMutex;                ///< 保护 m_delayed
    std::condition_variable m_delayCv;              ///< timer 等待此 CV 获取新延迟任务
    std::vector<DelayedTask> m_delayed;

    std::atomic<uint32_t> m_nextId{ 1 };            ///< 下一个任务 ID (递增)
};

#endif /* ZM_UTIL_THREAD_H */
