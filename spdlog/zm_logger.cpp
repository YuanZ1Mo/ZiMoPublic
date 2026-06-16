#include "zm_logger.h"
#include "pattern_formatter.h"

#include <cstdlib>
#include <filesystem>

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
    spdlog::drop(config_.logger_name);
    logger_ = spdlog::rotating_logger_mt(config_.logger_name, get_log_path(), config_.max_file_size, config_.max_files);

    // 注入自定义 pattern flag: %T = 线程名
    spdlog::pattern_formatter::custom_flags customFlags;
    customFlags['T'] = std::make_unique<ThreadNameFlag>();
    auto formatter = std::make_unique<spdlog::pattern_formatter>(
        config_.pattern, spdlog::pattern_time_type::local, std::string("\n"), std::move(customFlags));
    logger_->set_formatter(std::move(formatter));

    logger_->flush_on(spdlog::level::trace);

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