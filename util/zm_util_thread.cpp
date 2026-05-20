#include "zm_util_thread.h"
#include "zm_util_sys.h"

#include <algorithm>


/// 全局线程池实例, 供 InvokeLater / InvokeCancel 使用
ZmThreadPool g_zm_thread_pool;


// ============================ ZmThread ============================

// --- 构造 / 析构 ---

ZmThread::ZmThread(const std::string& name, std::function<void(std::stop_token)> func)
    : m_name(name)
    , m_func(std::move(func))
{
}

ZmThread::ZmThread(const std::string& name, std::function<void()> func)
    : m_name(name)
    , m_func0(std::move(func))
{
}

ZmThread::ZmThread(const std::string& name)
    : m_name(name)
{
}

ZmThread::~ZmThread()
{
    if (m_autoDelete)
    {
        // 自动销毁模式下, 线程池会负责 delete
        // 这里 detach 防止 jthread 析构时 join 死锁
        if (m_thread.joinable())
        {
            m_thread.detach();
        }
    }
    else
    {
        Stop();
    }
}

// --- 线程控制 ---

bool ZmThread::Start()
{
    std::unique_lock lock(m_mutex);
    if (m_state != ZM_THD_STATE_STOPPED)
        return false;

    m_state = ZM_THD_STATE_STARTING;

    // promise/future 用于同步: 主线程阻塞在 fut.wait(),
    // 工作线程在 runInternal 中 set_value() 后主线程才返回
    std::promise<void> started;
    auto fut = started.get_future();

    try
    {
        // jthread 自动检测 lambda 参数是否接受 stop_token,
        // 如果接受则将内部的 stop_token 作为第一个参数传入
        m_thread = std::jthread([this, p = std::move(started)](std::stop_token st) mutable {
            runInternal(st, std::move(p));
        });
    }
    catch (...)
    {
        // 线程创建失败 (如系统资源不足), 恢复状态
        m_state = ZM_THD_STATE_STOPPED;
        return false;
    }

    lock.unlock();
    fut.wait();

    return m_state == ZM_THD_STATE_RUNNING;
}

void ZmThread::Stop()
{
    {
        std::unique_lock lock(m_mutex);
        if (m_state == ZM_THD_STATE_STOPPED)
            return;

        if (m_state == ZM_THD_STATE_RUNNING || m_state == ZM_THD_STATE_STARTING)
        {
            m_state = ZM_THD_STATE_STOPPING;
            // 先触发 OnStopping, 给子类机会打断阻塞调用 (如 event_base_loopexit)
            OnStopping();
        }
    }

    // 设置 stop_token, 工作函数中 stop_requested() 变为 true
    m_thread.request_stop();

    {
        std::unique_lock lock(m_mutex);
        // 等待 runInternal 将状态设为 STOPPED 并 notify_all
        m_cv.wait(lock, [this] { return m_state == ZM_THD_STATE_STOPPED; });
        // 自动销毁模式下不能 join (线程池负责 delete, jthread 析构时不能 join 自己)
        if (!m_autoDelete && m_thread.joinable())
        {
            m_thread.join();
        }
    }
}

// --- 状态查询 ---

ZM_THD_STATE ZmThread::GetState() const
{
    return m_state.load();
}

bool ZmThread::IsStopped() const
{
    return ZmThdHasState(m_state.load(), ZM_THD_STATE_STOPPED);
}

bool ZmThread::IsRunning() const
{
    return ZmThdHasState(m_state.load(), ZM_THD_STATE_RUNNING);
}

bool ZmThread::IsStopping() const
{
    return ZmThdHasState(m_state.load(), ZM_THD_STATE_STOPPING);
}

// --- 属性访问 ---

const std::string& ZmThread::GetName() const
{
    return m_name;
}

void ZmThread::SetName(const std::string& name)
{
    m_name = name;
}

std::thread::id ZmThread::ThreadID() const
{
    return m_threadId.load();
}

std::thread::id ZmThread::CurrentThreadID()
{
    return std::this_thread::get_id();
}

// --- 配置 ---

void ZmThread::SetAutoDelete(bool autoDel)
{
    m_autoDelete = autoDel;
}

bool ZmThread::IsAutoDelete() const
{
    return m_autoDelete.load();
}

// --- 虚函数 (继承模式) ---

void ZmThread::Run(std::stop_token st)
{
    if (m_func)
    {
        // callable 带 stop_token 模式: 直接调用构造时传入的函数
        m_func(st);
    }
    else
    {
        // 回退到无参 Run(), 由继承模式的子类处理
        Run();
    }
}

void ZmThread::Run()
{
    if (m_func0)
    {
        // callable 无 stop_token 模式: 调用构造时传入的无参函数
        m_func0();
    }
    // 否则什么都不做 (继承模式下子类已 override Run(st))
}

void ZmThread::OnStopping()
{
}

// --- 内部实现 ---

void ZmThread::runInternal(std::stop_token st, std::promise<void> started)
{
    {
        std::unique_lock lock(m_mutex);
        m_state = ZM_THD_STATE_RUNNING;
        m_threadId = std::this_thread::get_id();
    }

#ifdef _WIN32
    // 设置 OS 级线程名, 在 Visual Studio 调试器和 Process Explorer 中可见
    // 动态加载 SetThreadDescription 以兼容 Windows 10 以下版本
    {
        typedef HRESULT(WINAPI* PFN_SetThreadDescription)(HANDLE, PCWSTR);
        auto pfn = (PFN_SetThreadDescription)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
        if (pfn)
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, m_name.c_str(), -1, NULL, 0);
            if (len > 0)
            {
                std::wstring wname(len - 1, 0);
                MultiByteToWideChar(CP_UTF8, 0, m_name.c_str(), -1, wname.data(), len);
                pfn(GetCurrentThread(), wname.c_str());
            }
        }
    }
#endif

    // 通知 Start() 线程已就绪, Start() 的 fut.wait() 由此返回
    started.set_value();

    try
    {
        Run(st);
    }
    catch (...)
    {
        // 吞掉异常, 防止未处理异常终止进程
    }

    {
        std::unique_lock lock(m_mutex);
        m_state = ZM_THD_STATE_STOPPED;
        m_threadId = std::thread::id();
    }
    // 通知 Stop() 线程已退出, Stop() 的 m_cv.wait() 由此返回
    m_cv.notify_all();

    // 自动销毁: 不能在线程内部直接 delete this (jthread 析构会 join 自己 → 死锁)
    // 通过线程池调度到另一个线程执行 delete
    if (m_autoDelete.exchange(false))
    {
        g_zm_thread_pool.Submit([this]() { delete this; });
    }
}


// ============================ ZmThreadPool ============================

// --- 构造 / 析构 ---

ZmThreadPool::ZmThreadPool(uint16_t threadCount)
    : m_initSize(threadCount)
{
    for (uint16_t i = 0; i < threadCount; i++)
    {
        m_workers.emplace_back([this](std::stop_token st) { workerProc(st); });
    }
    m_timerThread = std::jthread([this](std::stop_token st) { timerProc(st); });
}

ZmThreadPool::~ZmThreadPool()
{
    // 1. 请求所有线程停止
    m_timerThread.request_stop();
    for (auto& w : m_workers)
    {
        w.request_stop();
    }

    // 2. 唤醒正在 CV 上等待的线程, 让它们检查 stop_requested()
    m_delayCv.notify_all();
    m_taskCv.notify_all();

    // 3. 显式 join, 确保线程在成员析构前已完全退出
    m_timerThread.join();
    for (auto& w : m_workers)
    {
        if (w.joinable()) w.join();
    }
}

// --- 任务提交 ---

uint32_t ZmThreadPool::Submit(std::function<void()> task, uint32_t delayMs)
{
    if (delayMs == 0)
    {
        // 立即执行: 直接放入 worker 任务队列
        submitImmediate(std::move(task));
        return 0;
    }

    // 延迟执行: 放入延迟队列, 由 timer 线程到期后转入 worker 队列
    uint32_t id = m_nextId++;
    uint64_t deadline = ZmSystem::CurrentTimeMills() + delayMs;

    {
        std::unique_lock lock(m_delayMutex);
        m_delayed.push_back({ id, std::move(task), deadline });
    }
    // 唤醒 timer 线程重新评估最早截止时间
    m_delayCv.notify_all();

    return id;
}

bool ZmThreadPool::Cancel(uint32_t taskId)
{
    std::unique_lock lock(m_delayMutex);
    auto it = std::find_if(m_delayed.begin(), m_delayed.end(),
        [taskId](const DelayedTask& t) { return t.id == taskId; });
    if (it != m_delayed.end())
    {
        m_delayed.erase(it);
        return true;
    }
    return false;
}

// --- 状态查询 ---

uint16_t ZmThreadPool::ThreadCount() const
{
    return static_cast<uint16_t>(m_workers.size());
}

uint16_t ZmThreadPool::IdleCount() const
{
    return m_idleCount.load();
}

// --- 全局线程池接口 ---

uint32_t ZmThreadPool::InvokeLater(std::function<void()> executor, uint32_t delayMs)
{
    return g_zm_thread_pool.Submit(std::move(executor), delayMs);
}

uint32_t ZmThreadPool::InvokeLater(std::function<void(void*)> executor, void* param, uint32_t delayMs)
{
    // 将 void* 参数通过 lambda 捕获包装成无参 function
    return g_zm_thread_pool.Submit([executor, param]() { executor(param); }, delayMs);
}

bool ZmThreadPool::InvokeCancel(uint32_t taskId)
{
    return g_zm_thread_pool.Cancel(taskId);
}

// --- 内部实现 ---

void ZmThreadPool::workerProc(std::stop_token st)
{
    while (!st.stop_requested())
    {
        std::function<void()> task;
        {
            std::unique_lock lock(m_taskMutex);
            m_idleCount++;
            // 阻塞等待: 直到有新任务或收到停止信号
            m_taskCv.wait(lock, [&] {
                return !m_tasks.empty() || st.stop_requested();
            });
            m_idleCount--;

            if (st.stop_requested()) return;
            if (m_tasks.empty()) continue;

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        // 任务异常被 catch 住, 不会杀死 worker, 继续处理下一个任务
        try { task(); }
        catch (...) {}
    }
}

void ZmThreadPool::timerProc(std::stop_token st)
{
    while (!st.stop_requested())
    {
        std::vector<std::function<void()>> expired;

        {
            std::unique_lock lock(m_delayMutex);

            // 阶段1: 等待延迟任务出现 (无任务时休眠, 不占 CPU)
            m_delayCv.wait(lock, [&] {
                return !m_delayed.empty() || st.stop_requested();
            });
            if (st.stop_requested()) return;

            // 阶段2: 精确等待最早任务到期
            //   - 使用 wait_for 等到截止时间, 而非轮询
            //   - Submit() 添加新任务时会 notify_all 打断等待, 重新评估最早时间
            while (!st.stop_requested() && !m_delayed.empty())
            {
                auto it = std::min_element(m_delayed.begin(), m_delayed.end(),
                    [](const DelayedTask& a, const DelayedTask& b) {
                        return a.deadline < b.deadline;
                    });

                uint64_t now = ZmSystem::CurrentTimeMills();
                if (it->deadline <= now) break;

                m_delayCv.wait_for(lock, std::chrono::milliseconds(it->deadline - now));
            }

            if (st.stop_requested()) return;
            if (m_delayed.empty()) continue;

            // 阶段3: 收集所有已到期任务, 从延迟队列移除
            uint64_t now = ZmSystem::CurrentTimeMills();
            for (auto it = m_delayed.begin(); it != m_delayed.end(); )
            {
                if (it->deadline <= now)
                {
                    expired.push_back(std::move(it->task));
                    it = m_delayed.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // 阶段4: 将到期任务提交到 worker 队列执行
        for (auto& task : expired)
        {
            submitImmediate(std::move(task));
        }
    }
}

void ZmThreadPool::submitImmediate(std::function<void()> task)
{
    {
        std::unique_lock lock(m_taskMutex);
        m_tasks.push(std::move(task));
        // 自动增长: 所有 worker 都忙且未达上限时, 新建一个 worker
        if (m_idleCount == 0 && m_workers.size() < ZM_POOL_MAX_THREADS)
        {
            m_workers.emplace_back([this](std::stop_token st) { workerProc(st); });
        }
    }
    m_taskCv.notify_one();
}
