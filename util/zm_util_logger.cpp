#include "zm_util_logger.h"

#include <../spdlog/pattern_formatter.h>

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// 自定义 spdlog pattern flag: %T → 当前线程名（Windows SetThreadDescription）
// ============================================================================

class ThreadNameFlag : public spdlog::custom_flag_formatter
{
public:
    void format(const spdlog::details::log_msg&, const std::tm&, spdlog::memory_buf_t& dest) override
    {
        std::string name;
#ifdef _WIN32
        PWSTR pName = nullptr;
        HRESULT hr = GetThreadDescription(GetCurrentThread(), &pName);
        if (SUCCEEDED(hr) && pName)
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, pName, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1)
            {
                name.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, pName, -1, name.data(), len, nullptr, nullptr);
            }
            LocalFree(pName);
        }
#endif
        if (name.empty())
        {
            // 兜底：输出线程 ID
            auto tid = std::this_thread::get_id();
            std::ostringstream ss;
            ss << tid;
            name = ss.str();
            // 截短显示
            if (name.size() > 8)
                name = name.substr(name.size() - 4);
        }
        dest.append(name.data(), name.data() + name.size());
    }

    std::unique_ptr<custom_flag_formatter> clone() const override
    {
        return spdlog::details::make_unique<ThreadNameFlag>();
    }
};

// 全局日志对象定义
std::shared_ptr<spdlog::logger> g_default_logger = nullptr;
std::shared_ptr<spdlog::logger> g_public_logger = nullptr;

// 共享 sink 管理：以日志文件路径为 key，同路径的 logger 共用同一个 rotating_file_sink_mt
// 避免多个 sink 写同一文件时大小追踪分裂和旋转冲突
struct SharedSinkEntry
{
    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> sink;
    int refcount = 0;
};
static std::mutex g_shared_sinks_mutex;
static std::unordered_map<std::string, SharedSinkEntry> g_shared_sinks;

// 获取或创建共享 sink（调用方需确保线程安全，内部加锁）
static std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> acquire_shared_sink(
    const std::string& path, std::size_t max_size, std::size_t max_files)
{
    std::lock_guard<std::mutex> lock(g_shared_sinks_mutex);
    auto it = g_shared_sinks.find(path);
    if (it != g_shared_sinks.end())
    {
        ++it->second.refcount;
        return it->second.sink;
    }
    // 新建 sink 并加入 map
    SharedSinkEntry entry;
    entry.sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, max_size, max_files);
    entry.refcount = 1;
    g_shared_sinks[path] = std::move(entry);
    return g_shared_sinks[path].sink;
}

// 释放共享 sink，引用计数归零时销毁
static void release_shared_sink(const std::string& path)
{
    std::lock_guard<std::mutex> lock(g_shared_sinks_mutex);
    auto it = g_shared_sinks.find(path);
    if (it != g_shared_sinks.end() && --it->second.refcount <= 0)
    {
        g_shared_sinks.erase(it);
    }
}

RotatingLoggerBase::RotatingLoggerBase(const Config& config)
    : config_(config)
{
}

RotatingLoggerBase::~RotatingLoggerBase()
{
    ReleaseLogger();
}

void RotatingLoggerBase::CreateLogger()
{
    // 先清理旧 logger（如果存在）
    spdlog::drop(config_.logger_name);

    // 获取或创建共享 sink（同路径复用，避免多 sink 写同一文件）
    std::string path = get_log_path();
    auto sink = acquire_shared_sink(path, config_.max_file_size, config_.max_files);

    // 创建 logger 并绑定共享 sink
    logger_ = std::make_shared<spdlog::logger>(config_.logger_name, std::move(sink));

    // 注入自定义 pattern flag: %T = 线程名
    spdlog::pattern_formatter::custom_flags customFlags;
    customFlags['T'] = std::make_unique<ThreadNameFlag>();
    auto formatter = std::make_unique<spdlog::pattern_formatter>(
        config_.pattern, spdlog::pattern_time_type::local, std::string("\n"), std::move(customFlags));
    logger_->set_formatter(std::move(formatter));

    logger_->flush_on(spdlog::level::trace);

    // 注册到 spdlog 全局 registry，使 spdlog::get() 和 spdlog::drop() 可用
    spdlog::register_logger(logger_);

    if (config_.is_default)
    {
        spdlog::set_default_logger(logger_);
    }
}

void RotatingLoggerBase::ReleaseLogger()
{
    if (logger_)
    {
        spdlog::drop(config_.logger_name);
        if (config_.is_default)
        {
            spdlog::set_default_logger(nullptr);
        }
        logger_.reset();

        // 释放共享 sink，引用计数归零时自动销毁 sink
        release_shared_sink(get_log_path());
    }
}

std::string RotatingLoggerBase::get_log_path() const
{
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exe_name = std::filesystem::path(buf).stem().string();

    char* program_data = nullptr;
    size_t required_size = 0;
    errno_t err = _dupenv_s(&program_data, &required_size, "ProgramData");

    std::string base_dir;
    if (err == 0 && program_data != nullptr) {
        base_dir = program_data;
        free(program_data);
    }
    else {
        base_dir = "C:\\ProgramData";
    }

    std::filesystem::path dir = std::filesystem::path(base_dir) / "ZiMo" / "logs";
    std::filesystem::create_directories(dir);

    return (dir / (exe_name + ".log")).string();
}

void DefaultLogger::Ensure()
{
    if (!g_default_logger)
    {
        static DefaultLogger dl;
    }
}

void PublicLogger::Ensure()
{
    if (!g_public_logger)
    {
        static PublicLogger pl;
    }
}