#include "zm_util_logger.h"

#include <../spdlog/pattern_formatter.h>
#include <../spdlog/sinks/base_sink.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
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

// ============================================================================
// 自研控制台 sink（Debug 模式）：UTF-8 → UTF-16 → WriteConsoleW
//   专治中文乱码：不依赖控制台代码页(GBK/65001)，也绕开 spdlog 内置 wincolor
//   sink 的 WriteConsoleA/WriteFile 裸字节输出；重定向到文件/管道时原样写 UTF-8
// ============================================================================

// 全局开关，EnableConsoleSink() 打开；CreateLogger 按此决定是否追加控制台 sink
static std::atomic<bool> g_console_sink_enabled{ false };

class ConsoleSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
    ConsoleSink()
        : out_(::GetStdHandle(STD_OUTPUT_HANDLE))
        , pid_(::GetCurrentProcessId())
    {
        DWORD mode = 0;
        is_console_ = (::GetConsoleMode(out_, &mode) != 0);
        CONSOLE_SCREEN_BUFFER_INFO info = {};
        if (is_console_ && ::GetConsoleScreenBufferInfo(out_, &info))
            orig_attrs_ = info.wAttributes;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        // 字段级手工渲染：文本内容与文件日志的 Config::pattern 保持一致（仅增减颜色控制字节）
        std::string out;
        out.reserve(192);

        att<ATTR_GRAY>(out);   out += '[';
        att<ATTR_WHITE>(out);  append_time(msg.time, out);
        att<ATTR_GRAY>(out);   out += "] [";
        att<ATTR_CYAN>(out);   append_sv(msg.logger_name, out);
        att<ATTR_GRAY>(out);   out += "] [";
        append_uint(pid_, out);
        att<ATTR_DIM>(out);    out += "] [";
        append_uint(msg.thread_id, out);
        att<ATTR_DIM>(out);    out += "] [";
        append_thread_name(msg.thread_id, out);
        att<ATTR_GRAY>(out);   out += "] [";
        att(out, att_for_level(msg.level));
        append_sv(spdlog::level::to_string_view(msg.level), out);
        att<ATTR_GRAY>(out);   out += "] [";
        att<ATTR_MAGENTA>(out); append_source_file(msg.source.filename, out);
        att<ATTR_GRAY>(out);   out += "] [";
        att<ATTR_CYAN>(out);   append_cstr(msg.source.funcname, out);
        att<ATTR_GRAY>(out);   out += "] [";
        append_uint(msg.source.line, out);
        att<ATTR_GRAY>(out);   out += "] ";
        att<ATTR_WHITE>(out);  append_sv(msg.payload, out);
        out += '\n';

        emit(out);
    }

    void flush_() override
    {
        ::FlushFileBuffers(out_);
    }

private:
    // ------- 调色板  -------
    static constexpr char CTL_COLOR = '\x01';      // 控制字节：后随一个调色板索引（仅存在于内部缓冲，输出前剥离）
    enum {
        ATTR_GRAY = 0,   // 分隔符/括号、PID 等：灰
        ATTR_WHITE,      // 时间、消息正文：白
        ATTR_CYAN,       // logger 名、函数名：青
        ATTR_MAGENTA,    // 源文件：品红
        ATTR_DIM,        // TID/线程名/行号：暗蓝灰
        ATTR_GREEN,      // 级别-info：绿
        ATTR_YELLOW,     // 级别-warn：黄
        ATTR_RED,        // 级别-err：红
        ATTR_CRITICAL,   // 级别-critical：白字红底
        ATTR_COUNT
    };
    static const WORD s_palette[ATTR_COUNT];

    template <int Attr>
    static void att(std::string& buf)
    {
        buf.push_back(CTL_COLOR);
        buf.push_back(static_cast<char>(Attr));
    }

    static void att(std::string& buf, int idx)
    {
        buf.push_back(CTL_COLOR);
        buf.push_back(static_cast<char>(idx));
    }

    // 级别 → 调色板索引
    static int att_for_level(spdlog::level::level_enum lv)
    {
        switch (lv)
        {
        case spdlog::level::trace:    return ATTR_WHITE;
        case spdlog::level::debug:    return ATTR_CYAN;
        case spdlog::level::info:     return ATTR_GREEN;
        case spdlog::level::warn:     return ATTR_YELLOW;
        case spdlog::level::err:      return ATTR_RED;
        case spdlog::level::critical: return ATTR_CRITICAL;
        default:                      return ATTR_GRAY;
        }
    }

    static void append_sv(spdlog::string_view_t sv, std::string& out) { out.append(sv.data(), sv.size()); }
    static void append_cstr(const char* s, std::string& out) { if (s) out += s; }

    static void append_source_file(const char* path, std::string& out)
    {
        if (!path)
            return;
        std::string f(path);
        size_t pos = f.find_last_of("/\\");
        out += (pos == std::string::npos) ? f : f.substr(pos + 1);
    }

    static void append_uint(size_t v, std::string& out)
    {
        char buf[24];
        snprintf(buf, sizeof buf, "%zu", v);
        out += buf;
    }

    static void append_time(const spdlog::log_clock::time_point& tp, std::string& out)
    {
        auto secs = std::chrono::system_clock::to_time_t(tp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch())
                % std::chrono::milliseconds(1000);
        tm t{};
        localtime_s(&t, &secs);
        char buf[32];
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &t);
        out += buf;
        char m[8];
        snprintf(m, sizeof m, ".%03d", static_cast<int>(ms.count()));
        out += m;
    }

    static void append_thread_name(size_t tid, std::string& out)
    {
        std::string name;
        DWORD curTid = ::GetCurrentThreadId();
        if (tid != 0)
        {
            HANDLE h = (tid == curTid) ? ::GetCurrentThread()
                                       : ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(tid));
            if (h)
            {
                PWSTR p = nullptr;
                if (SUCCEEDED(::GetThreadDescription(h, &p)) && p)
                {
                    int len = ::WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 1)
                    {
                        name.resize(static_cast<size_t>(len - 1));
                        ::WideCharToMultiByte(CP_UTF8, 0, p, -1, name.data(), len, nullptr, nullptr);
                    }
                    ::LocalFree(p);
                }
                if (h != ::GetCurrentThread())
                    ::CloseHandle(h);
            }
        }
        if (name.empty())
        {
            char buf[24];
            snprintf(buf, sizeof buf, "%zu", tid);
            name = buf;
            if (name.size() > 8)
                name = name.substr(name.size() - 4);
        }
        out += name;
    }

    // ------- 输出  -------
    void emit(const std::string& line)
    {
        if (!is_console_)
        {
            // 重定向（管道/文件）：剥离控制字节后原样写 UTF-8
            std::string plain;
            plain.reserve(line.size());
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == CTL_COLOR) { ++i; continue; }
                plain.push_back(line[i]);
            }
            DWORD written = 0;
            ::WriteFile(out_, plain.data(), static_cast<DWORD>(plain.size()), &written, nullptr);
            return;
        }

        // 控制台：按控制字节分段着色，UTF-8 → UTF-16 → WriteConsoleW（与代码页无关）
        size_t i = 0;
        while (i < line.size())
        {
            if (line[i] == CTL_COLOR && i + 1 < line.size())
            {
                int idx = static_cast<unsigned char>(line[i + 1]);
                WORD attr = (idx >= 0 && idx < ATTR_COUNT) ? s_palette[idx] : orig_attrs_;
                ::SetConsoleTextAttribute(out_, attr);
                i += 2;
                continue;
            }
            size_t start = i;
            while (i < line.size() && line[i] != CTL_COLOR)
                ++i;
            write_wide(line.data() + start, i - start);
        }
        ::SetConsoleTextAttribute(out_, orig_attrs_);
    }

    void write_wide(const char* utf8, size_t n)
    {
        if (n == 0)
            return;
        int wn = ::MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(n), nullptr, 0);
        if (wn <= 0)
            return;
        std::vector<wchar_t> buf(static_cast<size_t>(wn));
        ::MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(n), buf.data(), wn);
        ::WriteConsoleW(out_, buf.data(), wn, nullptr, nullptr);
    }

    HANDLE out_;
    DWORD pid_;
    bool is_console_ = false;
    WORD orig_attrs_ = 0x07;
};

const WORD ConsoleSink::s_palette[ConsoleSink::ATTR_COUNT] = {
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,                              // GRAY     灰
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,       // WHITE    白
    FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,                        // CYAN     青
    FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,                          // MAGENTA  品红
    FOREGROUND_BLUE | FOREGROUND_INTENSITY,                                           // DIM      暗蓝灰
    FOREGROUND_GREEN | FOREGROUND_INTENSITY,                                          // GREEN    绿(info)
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,                         // YELLOW   黄(warn)
    FOREGROUND_RED | FOREGROUND_INTENSITY,                                            // RED      红(err)
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY | BACKGROUND_RED, // CRITICAL 白字红底
};

// 全局唯一控制台 sink，多个 logger 复用
static std::shared_ptr<ConsoleSink> get_console_sink()
{
    static std::once_flag flag;
    static std::shared_ptr<ConsoleSink> sink;
    std::call_once(flag, [] {
        sink = std::make_shared<ConsoleSink>();
    });
    return sink;
}

void EnableConsoleSink()
{
    g_console_sink_enabled = true;

    // 已存在的 logger 立即补上控制台输出
    auto console = get_console_sink();
    if (g_default_logger)
        g_default_logger->sinks().push_back(console);
    if (g_public_logger)
        g_public_logger->sinks().push_back(console);
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

    // Debug 模式：同时输出到控制台
    if (g_console_sink_enabled.load())
    {
        logger_->sinks().push_back(get_console_sink());
    }

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