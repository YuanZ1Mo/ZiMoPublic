#ifndef ZM_UTIL_SYS_H
#define ZM_UTIL_SYS_H

#include <stdint.h>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct timeval;

/**
 * @brief 加载动态链接库
 * @param filename DLL文件名，传入 NULL 时获取当前主程序模块句柄
 * @return 模块句柄(HMODULE)
 * @example
 *   HMODULE h = ZmLoadLib("mylib.dll");   // 加载 mylib.dll
 *   HMODULE h2 = ZmLoadLib(NULL);          // 获取当前主程序句柄
 */
#define  ZmLoadLib(filename)      ((NULL!=filename)? LoadLibraryA(filename) : GetModuleHandleA(NULL))

/**
 * @brief 释放动态链接库
 * @param lib 模块句柄，释放后自动置为 NULL；主程序句柄不会被释放
 * @note 使用 do-while(0) 包裹，避免在 if-else 语句中产生悬挂 else 问题
 * @example
 *   HMODULE h = ZmLoadLib("mylib.dll");
 *   ZmFreeLib(h);  // h == NULL
 */
#define  ZmFreeLib(lib)           do { if ( lib && lib!=GetModuleHandleA(NULL) ) { FreeLibrary(lib); lib=NULL; } } while(0)

/**
 * @brief 从模块中查找导出函数地址
 * @param h   模块句柄
 * @param name 函数名
 * @return 函数地址，失败返回 NULL
 * @example
 *   typedef int (*FuncType)(int);
 *   FuncType fn = (FuncType)ZmLookupSymbol(h, "MyFunction");
 */
#define  ZmLookupSymbol(h, name)  GetProcAddress(h, name)

/**
 * @brief 本地时间结构体，精确到毫秒
 */
typedef struct
{
    uint16_t    year;       ///< 年 (e.g. 2026)
    uint16_t    month;      ///< 月 (1-12)
    uint16_t    day;        ///< 日 (1-31)
    uint16_t    hour;       ///< 时 (0-23)
    uint16_t    minute;     ///< 分 (0-59)
    uint16_t    second;     ///< 秒 (0-59)
    uint16_t    mills;      ///< 毫秒 (0-999)
}ZM_SYS_TIME;

/**
 * @brief 系统信息结构体
 */
typedef struct
{
    uint32_t    cpu_cores;          ///< CPU 逻辑核心数
    uint64_t    total_memory_mb;    ///< 物理内存总量 (MB)
    uint64_t    available_memory_mb;///< 可用物理内存 (MB)
    char        hostname[256];      ///< 主机名
}ZM_SYS_INFO;

/**
 * @brief 单实例守卫，通过命名互斥体防止程序重复运行
 * @note 非拷贝、非赋值
 * @example
 *   int main() {
 *       ZmSingleInstance guard("MyService_Unique_Name");
 *       if (guard.IsAnotherRunning()) {
 *           printf("Already running!\n");
 *           return 1;
 *       }
 *       // 正常业务逻辑...
 *       return 0;
 *   }
 */
class ZmSingleInstance
{
public:
    /**
     * @brief 构造函数，创建或打开命名互斥体
     * @param name 互斥体名称，全局唯一标识符，不同程序应使用不同名称
     */
    ZmSingleInstance(const char* name);

    /**
     * @brief 析构函数，自动释放互斥体
     */
    ~ZmSingleInstance();

    /**
     * @brief 检查是否已有另一个实例在运行
     * @return true 表示已有实例运行，当前为重复启动
     */
    bool IsAnotherRunning() const;

    /**
     * @brief 手动释放互斥体，释放后 IsAnotherRunning() 将不再可靠
     */
    void Release();

private:
    HANDLE  m_mutex;    ///< 命名互斥体句柄
    bool    m_owned;    ///< 当前实例是否持有互斥体（false 表示已有其他实例持有）

    ZmSingleInstance(const ZmSingleInstance&);
    ZmSingleInstance& operator=(const ZmSingleInstance&);
};

/**
 * @brief 系统工具类，提供时间、错误信息、系统信息、路径、环境变量、进程等功能
 * @note 所有方法均为静态方法，无需实例化
 */
class ZmSystem
{
public:
    // ---- Time ----

    /**
     * @brief 获取当前 UTC 时间的 timeval 结构
     * @param tv[out] 输出参数，tv_sec 为 UTC 秒数，tv_usec 为微秒数
     * @note 内部基于 FILETIME 到 Unix Epoch 的偏移计算，EPOCH = 116444736000000000
     *       (1601-01-01 到 1970-01-01 的 100ns 间隔数)
     */
    static void        GetCurrentTimeVal(struct timeval& tv);

    /**
     * @brief 获取当前时间的 Unix 毫秒时间戳 (UTC)
     * @return 自 1970-01-01 00:00:00 UTC 以来的毫秒数
     * @example
     *   uint64_t ms = ZmSystem::CurrentTimeMills();
     *   printf("timestamp: %llu\n", ms);
     */
    static uint64_t    CurrentTimeMills();

    /**
     * @brief 获取当前本地时间，精确到毫秒
     * @param systm[out] 输出参数，传入 NULL 安全返回
     * @example
     *   ZM_SYS_TIME stm;
     *   ZmSystem::CurrentTimeEx(&stm);
     *   printf("%04d-%02d-%02d %02d:%02d:%02d.%03d\n",
     *       stm.year, stm.month, stm.day,
     *       stm.hour, stm.minute, stm.second, stm.mills);
     */
    static void        CurrentTimeEx(ZM_SYS_TIME* systm);

    /**
     * @brief 将 ZM_SYS_TIME 格式化为字符串
     * @param systm 时间结构体指针
     * @param buf   输出缓冲区
     * @param buflen 缓冲区大小，至少需要 24 字节
     * @return 格式化后的字符串指针（即 buf），参数无效时返回空字符串
     * @example
     *   ZM_SYS_TIME stm;
     *   ZmSystem::CurrentTimeEx(&stm);
     *   char buf[32];
     *   ZmSystem::FormatTimeEx(&stm, buf, sizeof(buf));
     *   // buf: "2026-05-23 10:30:45.123"
     */
    static const char* FormatTimeEx(const ZM_SYS_TIME* systm, char* buf, int buflen);

    /**
     * @brief 获取当前本地时间的格式化字符串 (便捷方法)
     * @param buf    输出缓冲区
     * @param buflen 缓冲区大小，至少需要 24 字节
     * @return 格式化后的字符串指针（即 buf），格式 "YYYY-MM-DD HH:MM:SS.mmm"
     * @example
     *   char buf[32];
     *   printf("now: %s\n", ZmSystem::CurrentTimeStr(buf, sizeof(buf)));
     */
    static const char* CurrentTimeStr(char* buf, int buflen);

    // ---- Error ----

    /**
     * @brief 获取 Windows 系统错误码对应的文本描述
     * @param errcode 错误码，传入 -1 时自动调用 GetLastError() 获取最近错误
     * @return 错误描述字符串，格式 " [错误码]描述内容"，线程安全
     * @note 返回的指针指向 thread_local 静态缓冲区，同一线程内下次调用会覆盖
     * @example
     *   if (!CreateFileA(...)) {
     *       printf("failed: %s\n", ZmSystem::ErrMsg(-1));
     *   }
     *   printf("error 5: %s\n", ZmSystem::ErrMsg(5));
     */
    static const char* ErrMsg(int errcode);

    // ---- System info ----

    /**
     * @brief 查询系统硬件信息（CPU 核数、内存、主机名）
     * @param info[out] 输出参数，传入 NULL 安全返回
     * @example
     *   ZM_SYS_INFO info;
     *   ZmSystem::QuerySysInfo(&info);
     *   printf("CPU: %u cores, RAM: %llu / %llu MB, Host: %s\n",
     *       info.cpu_cores, info.available_memory_mb, info.total_memory_mb, info.hostname);
     */
    static void        QuerySysInfo(ZM_SYS_INFO* info);

    /**
     * @brief 获取系统处理器架构
     * @return 架构名称字符串: "x64", "ARM64", "ARM", "x86"，无法识别时返回 "Unknown"
     *
     * @example
     *   printf("Arch: %s\n", ZmSystem::GetSystemArchitecture().c_str());  // "x64"
     */
    static std::string GetSystemArchitecture();

    /**
     * @brief 获取系统自上次启动以来的运行时长
     * @return 运行时长（毫秒），使用 ULONGLONG 不会在 49.7 天后溢出
     *
     * @example
     *   ULONGLONG ms = ZmSystem::GetSystemUptime();
     *   printf("Uptime: %llu seconds\n", ms / 1000);
     */
    static ULONGLONG   GetSystemUptime();

    /**
     * @brief 获取系统最后一次启动的时间
     * @return 启动时间字符串，格式 "YYYY-MM-DD HH:MM:SS" (本地时间)
     *
     * @example
     *   printf("Boot at: %s\n", ZmSystem::GetSystemBootTime().c_str());  // "2026-05-23 08:30:15"
     */
    static std::string GetSystemBootTime();

    // ---- Path ----

    /**
     * @brief 获取当前可执行文件所在目录（不含末尾反斜杠）
     * @param buf    输出缓冲区
     * @param buflen 缓冲区大小，建议至少 MAX_PATH (260)
     * @return 目录路径字符串指针（即 buf），参数无效时返回空字符串
     * @example
     *   char buf[MAX_PATH];
     *   ZmSystem::GetModuleDir(buf, MAX_PATH);
     *   // buf: "C:\Program Files\MyApp"
     */
    static const char* GetModuleDir(char* buf, int buflen);

    /**
     * @brief 获取系统临时文件目录（含末尾反斜杠）
     * @param buf    输出缓冲区
     * @param buflen 缓冲区大小，建议至少 MAX_PATH (260)
     * @return 临时目录路径字符串指针（即 buf），参数无效时返回空字符串
     * @example
     *   char buf[MAX_PATH];
     *   ZmSystem::GetTempDir(buf, MAX_PATH);
     *   // buf: "C:\Users\Username\AppData\Local\Temp\"
     */
    static const char* GetTempDir(char* buf, int buflen);

    /**
     * @brief 获取当前用户的漫游应用数据目录（Roaming AppData）
     * @param buf    输出缓冲区
     * @param buflen 缓冲区大小，建议至少 MAX_PATH (260)
     * @return 目录路径字符串指针（即 buf），获取失败或参数无效时返回空字符串
     * @example
     *   char buf[MAX_PATH];
     *   ZmSystem::GetAppDataDir(buf, MAX_PATH);
     *   // buf: "C:\Users\Username\AppData\Roaming"
     */
    static const char* GetAppDataDir(char* buf, int buflen);

    /**
     * @brief 获取 Windows 特殊文件夹路径
     * @param csidl 特殊文件夹标识符，常用值:
     *   CSIDL_DESKTOP         - 桌面
     *   CSIDL_PROGRAMS         - 开始菜单程序组
     *   CSIDL_PERSONAL         - 我的文档
     *   CSIDL_FAVORITES        - 收藏夹
     *   CSIDL_STARTUP          - 启动文件夹
     *   CSIDL_RECENT           - 最近文档
     *   CSIDL_APPDATA          - Roaming AppData
     *   CSIDL_LOCAL_APPDATA    - Local AppData
     *   CSIDL_PROGRAM_FILES    - Program Files
     *   CSIDL_PROGRAM_FILESX86 - Program Files (x86)
     *   CSIDL_COMMON_APPDATA   - ProgramData
     * @return 文件夹路径，获取失败返回空字符串
     *
     * @example
     *   std::wstring appData = ZmSystem::GetSpecialFolder(CSIDL_APPDATA);
     *   // appData: "C:\\Users\\Username\\AppData\\Roaming"
     */
    static std::wstring GetSpecialFolder(int csidl);

    /**
     * @brief 获取 Windows 系统目录路径
     * @return 系统目录路径，如 "C:\\Windows"
     *
     * @example
     *   printf("Windows: %ls\n", ZmSystem::GetWindowsDir().c_str());  // "C:\\Windows"
     */
    static std::wstring GetWindowsDir();

    /**
     * @brief 枚举系统中所有用户配置文件的目录路径
     *
     * 从注册表 ProfileList 枚举所有用户配置，自动展开环境变量，
     * 过滤系统服务账户路径 (ServiceProfiles)
     *
     * @return 用户目录路径列表，如 { "C:\\Users\\Alice", "C:\\Users\\Bob" }
     *
     * @example
     *   auto paths = ZmSystem::GetUserProfilePaths();
     *   for (auto& p : paths)
     *       printf("User: %ls\n", p.c_str());
     */
    static std::vector<std::wstring> GetUserProfilePaths();

    // ---- Environment ----

    /**
     * @brief 读取环境变量
     * @param name   环境变量名
     * @param buf    输出缓冲区
     * @param buflen 缓冲区大小
     * @return true 读取成功，false 变量不存在或参数无效
     * @example
     *   char buf[256];
     *   if (ZmSystem::GetEnv("PATH", buf, sizeof(buf))) {
     *       printf("PATH=%s\n", buf);
     *   }
     */
    static bool        GetEnv(const char* name, char* buf, int buflen);

    /**
     * @brief 设置环境变量
     * @param name  环境变量名
     * @param value 环境变量值，传入 NULL 时删除该环境变量
     * @return true 设置成功，false 失败
     * @example
     *   ZmSystem::SetEnv("MY_VAR", "hello");
     *   ZmSystem::SetEnv("MY_VAR", NULL);  // 删除
     */
    static bool        SetEnv(const char* name, const char* value);

    // ---- Process ----

    /**
     * @brief 获取当前进程 ID
     * @return 进程 ID
     */
    static uint32_t    GetPid();

    /**
     * @brief 获取当前线程 ID
     * @return 线程 ID
     */
    static uint32_t    GetTid();

    /**
     * @brief 设置当前进程的优先级
     * @param priority 优先级：-2=Idle, -1=BelowNormal, 0=Normal, 1=AboveNormal, 2=High, 3=Realtime
     * @return true 设置成功，false 优先级值无效或系统调用失败
     * @note Realtime 优先级需要管理员权限，滥用可能导致系统无响应
     * @example
     *   ZmSystem::SetProcessPriority(-1);  // 设为低于正常优先级
     */
    static bool        SetProcessPriority(int priority);

    /**
     * @brief 检测指定名称的进程是否正在运行
     * @param processName 进程可执行文件名（大小写不敏感），如 L"explorer.exe"
     * @return true 表示存在同名进程
     *
     * @example
     *   if (ZmSystem::IsProcessRunning(L"notepad.exe"))
     *       printf("Notepad is running\n");
     */
    static bool IsProcessRunning(const std::wstring& processName);

    /**
     * @brief 枚举系统中所有正在运行的进程
     * @return 进程列表，每个元素为 { PID, 进程名 } 的 pair
     *
     * @example
     *   auto list = ZmSystem::GetProcessList();
     *   for (auto& [pid, name] : list)
     *       printf("[%u] %ls\n", pid, name.c_str());
     */
    static std::vector<std::pair<DWORD, std::wstring>> GetProcessList();

    /**
     * @brief 获取当前进程的可执行文件完整路径
     * @return exe 文件完整路径，如 "C:\\Program Files\\MyApp\\MyApp.exe"
     *
     * @example
     *   std::wstring path = ZmSystem::GetCurrentProcessPath();
     *   printf("Path: %ls\n", path.c_str());
     */
    static std::wstring GetCurrentProcessPath();
};

#endif /* ZM_UTIL_SYS_H */
