#include "zm_logger.h"

#include <cstdlib>
#include <filesystem>

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
    logger_->set_pattern(config_.pattern);
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