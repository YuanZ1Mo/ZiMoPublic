#ifndef ZM_LOGGER_H
#define ZM_LOGGER_H

#include "spdlog.h"
#include "sinks/rotating_file_sink.h"
#include <filesystem>
#include <string>

//支持宽字符串转 UTF-8
#ifndef SPDLOG_WCHAR_TO_UTF8_SUPPORT
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#endif

// 全局日志对象，通过宏使用
extern std::shared_ptr<spdlog::logger> g_default_logger;
extern std::shared_ptr<spdlog::logger> g_public_logger;

// Rotating file logger 基类，提供可配置的日志初始化与清理
// 继承此类只需在构造函数体中调用 CreateLogger() 即可，析构时自动清理
class RotatingLoggerBase
{
public:
    struct Config
    {
        std::string logger_name = "DEFAULT";
        bool is_default = false;
        size_t max_file_size = 1024 * 1024 * 10; // 10MB
        int max_files = 10;
        // 日志格式：时间 | logger名 | 进程ID | 线程ID | 级别 | 源文件 | 函数名 | 行号 | 内容
        std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%P] [%t] [%l] [%s] [%!] [%#] %v";
    };

    RotatingLoggerBase(RotatingLoggerBase&&) = delete;
    RotatingLoggerBase(const RotatingLoggerBase&) = delete;
    RotatingLoggerBase& operator=(const RotatingLoggerBase&) = delete;
    virtual ~RotatingLoggerBase();

protected:
    explicit RotatingLoggerBase(const Config& config = Config{});

    // 在派生类构造函数体中调用，此时虚表已就绪，get_log_path() 可正确分派
    void CreateLogger();
    void ReleaseLogger();

    // 返回日志文件路径，默认 %ProgramData%\ZiMo\logs\<exe_name>.log，子类可重写
    virtual std::string get_log_path() const;

    Config config_;
    std::shared_ptr<spdlog::logger> logger_;
};

// 默认日志管理器，logger_name 为 DEFAULT，设为默认 logger
class DefaultLogger : public RotatingLoggerBase
{
public:
    DefaultLogger() : RotatingLoggerBase({"DEFAULT", true}) { CreateLogger(); g_default_logger = logger_; }
    DefaultLogger(DefaultLogger&&) = delete;
    DefaultLogger(const DefaultLogger&) = delete;
    DefaultLogger& operator=(const DefaultLogger&) = delete;
    ~DefaultLogger() { g_default_logger.reset(); }

    static void Ensure();
};

// 公共库日志管理器，logger_name 为 PUBLIC，不设为默认 logger
class PublicLogger : public RotatingLoggerBase
{
public:
    PublicLogger() : RotatingLoggerBase({"PUBLIC", false}) { CreateLogger(); g_public_logger = logger_; }
    PublicLogger(PublicLogger&&) = delete;
    PublicLogger(const PublicLogger&) = delete;
    PublicLogger& operator=(const PublicLogger&) = delete;
    ~PublicLogger() { g_public_logger.reset(); }

    static void Ensure();
};

// 日志宏 —— 调用前自动检查 g_default_logger 是否有效，无效则自动初始化
#define DEFAULT_LOG_TRACE(...)    do { if (!g_default_logger) DefaultLogger::Ensure(); SPDLOG_LOGGER_TRACE(g_default_logger, __VA_ARGS__); } while(0)
#define DEFAULT_LOG_DEBUG(...)    do { if (!g_default_logger) DefaultLogger::Ensure(); SPDLOG_LOGGER_DEBUG(g_default_logger, __VA_ARGS__); } while(0)
#define DEFAULT_LOG_INFO(...)     do { if (!g_default_logger) DefaultLogger::Ensure(); SPDLOG_LOGGER_INFO(g_default_logger, __VA_ARGS__); } while(0)
#define DEFAULT_LOG_WARN(...)     do { if (!g_default_logger) DefaultLogger::Ensure(); SPDLOG_LOGGER_WARN(g_default_logger, __VA_ARGS__); } while(0)
#define DEFAULT_LOG_ERROR(...)    do { if (!g_default_logger) DefaultLogger::Ensure(); SPDLOG_LOGGER_ERROR(g_default_logger, __VA_ARGS__); } while(0)
#define DEFAULT_LOG_CRITICAL(...) do { if (!g_default_logger) DefaultLogger::Ensure(); SPDLOG_LOGGER_CRITICAL(g_default_logger, __VA_ARGS__); } while(0)

// 日志宏 —— 调用前自动检查 g_public_logger 是否有效，无效则自动初始化
#define PUBLIC_LOG_TRACE(...)    do { if (!g_public_logger) PublicLogger::Ensure(); SPDLOG_LOGGER_TRACE(g_public_logger, __VA_ARGS__); } while(0)
#define PUBLIC_LOG_DEBUG(...)    do { if (!g_public_logger) PublicLogger::Ensure(); SPDLOG_LOGGER_DEBUG(g_public_logger, __VA_ARGS__); } while(0)
#define PUBLIC_LOG_INFO(...)     do { if (!g_public_logger) PublicLogger::Ensure(); SPDLOG_LOGGER_INFO(g_public_logger, __VA_ARGS__); } while(0)
#define PUBLIC_LOG_WARN(...)     do { if (!g_public_logger) PublicLogger::Ensure(); SPDLOG_LOGGER_WARN(g_public_logger, __VA_ARGS__); } while(0)
#define PUBLIC_LOG_ERROR(...)    do { if (!g_public_logger) PublicLogger::Ensure(); SPDLOG_LOGGER_ERROR(g_public_logger, __VA_ARGS__); } while(0)
#define PUBLIC_LOG_CRITICAL(...) do { if (!g_public_logger) PublicLogger::Ensure(); SPDLOG_LOGGER_CRITICAL(g_public_logger, __VA_ARGS__); } while(0)

#endif // ZM_LOGGER_H
