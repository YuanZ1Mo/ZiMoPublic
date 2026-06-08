#include "zm_util_sys.h"

#include <winsock2.h>
#include <cstdio>
#include <cstring>
#include <shlobj.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <vector>
#pragma comment(lib, "pdh.lib")

///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmSingleInstance

ZmSingleInstance::ZmSingleInstance(const char* name)
    : m_mutex(NULL), m_owned(false)
{
    if (!name) return;
    m_mutex = ::CreateMutexA(NULL, TRUE, name);
    if (m_mutex)
    {
        // CreateMutex 成功后 GetLastError 返回 ERROR_ALREADY_EXISTS 表示互斥体已存在，
        // 即另一个实例正在运行，此时当前进程不持有该互斥体
        m_owned = (::GetLastError() != ERROR_ALREADY_EXISTS);
    }
}

ZmSingleInstance::~ZmSingleInstance()
{
    Release();
}

bool ZmSingleInstance::IsAnotherRunning() const
{
    return !m_owned;
}

void ZmSingleInstance::Release()
{
    if (m_mutex)
    {
        // 仅在当前进程持有互斥体时才 ReleaseMutex，否则只关闭句柄
        if (m_owned) ::ReleaseMutex(m_mutex);
        ::CloseHandle(m_mutex);
        m_mutex = NULL;
        m_owned = false;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmSystem - Time

void ZmSystem::GetCurrentTimeVal(struct timeval& tv)
{
    // Windows FILETIME 从 1601-01-01 起算的 100ns 间隔数，
    // Unix Epoch 从 1970-01-01 起算，两者差值即此常量（11644473600秒 × 10000000）
    static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

    SYSTEMTIME  system_time;
    FILETIME    file_time;
    uint64_t    time;

    // TODO: 可优化为 GetSystemTimeAsFileTime，减少一次系统调用
    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);

    // 将 dwHighDateTime 和 dwLowDateTime 组合为 64 位 FILETIME 值
    time = ((uint64_t)file_time.dwLowDateTime);
    time += ((uint64_t)file_time.dwHighDateTime) << 32;

    // 100ns 间隔转为秒（整数除法截断毫秒部分），毫秒部分单独从 SYSTEMTIME 取出
    tv.tv_sec = (long)((time - EPOCH) / 10000000L);
    tv.tv_usec = (long)(system_time.wMilliseconds * 1000);
}

uint64_t ZmSystem::CurrentTimeMills()
{
    struct timeval tv;
    GetCurrentTimeVal(tv);
    return (uint64_t)(tv.tv_sec) * 1000 + (uint64_t)(tv.tv_usec) / 1000;
}

void ZmSystem::CurrentTimeEx(ZM_SYS_TIME* systm)
{
    if (!systm) return;
    memset(systm, 0, sizeof(ZM_SYS_TIME));
    SYSTEMTIME  occurs;
    // 使用本地时间，与用户时区一致
    GetLocalTime(&occurs);
    systm->year = occurs.wYear;
    systm->month = occurs.wMonth;
    systm->day = occurs.wDay;
    systm->hour = occurs.wHour;
    systm->minute = occurs.wMinute;
    systm->second = occurs.wSecond;
    systm->mills = occurs.wMilliseconds;
}

const char* ZmSystem::FormatTimeEx(const ZM_SYS_TIME* systm, char* buf, int buflen)
{
    // 最短输出 "0000-00-00 00:00:00.000" = 23 字符 + '\0' = 24
    if (!systm || !buf || buflen < 24)
        return "";
    snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        systm->year, systm->month, systm->day,
        systm->hour, systm->minute, systm->second, systm->mills);
    return buf;
}

const char* ZmSystem::CurrentTimeStr(char* buf, int buflen)
{
    ZM_SYS_TIME stm;
    CurrentTimeEx(&stm);
    return FormatTimeEx(&stm, buf, buflen);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmSystem - Error

const char* ZmSystem::ErrMsg(int errcode)
{
    // thread_local 保证多线程并发调用时各自拥有独立缓冲区
    static thread_local char msg[1024];

    memset(msg, 0, sizeof(msg));

    // errcode == -1 时自动获取线程最近的错误码
    if (-1 == errcode)
    {
        errcode = GetLastError();
    }
    char* buf = NULL;
    if (::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errcode, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        (char*)&buf, 0, NULL))
    {
        int len = snprintf(msg, sizeof(msg), " [%d]%s", errcode, buf);
        // snprintf 返回值可能超过缓冲区大小，需检查 len < sizeof(msg) 防止越界访问
        // Windows FormatMessage 返回的字符串末尾通常带有 \r\n，在此去除
        if (len > 2 && len < (int)sizeof(msg) && msg[len - 1] == '\n' && msg[len - 2] == '\r')
        {
            msg[len - 1] = '\0';
            msg[len - 2] = '\0';
        }
        ::LocalFree(buf);
    }
    else
    {
        snprintf(msg, sizeof(msg), " [%d]Unknown error %d", errcode, errcode);
    }

    return (msg);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmSystem - System info

void ZmSystem::QuerySysInfo(ZM_SYS_INFO* info)
{
    if (!info) return;
    memset(info, 0, sizeof(ZM_SYS_INFO));

    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    info->cpu_cores = si.dwNumberOfProcessors;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms))
    {
        info->total_memory_mb = ms.ullTotalPhys / (1024 * 1024);
        info->available_memory_mb = ms.ullAvailPhys / (1024 * 1024);
    }

    DWORD size = sizeof(info->hostname);
    ::GetComputerNameA(info->hostname, &size);
}

std::string ZmSystem::GetSystemArchitecture()
{
    SYSTEM_INFO si = {};
    // 使用 GetNativeSystemInfo 而非 GetSystemInfo，确保在 WoW64 下获取真实硬件架构
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
    case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
    case PROCESSOR_ARCHITECTURE_ARM:   return "ARM";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    default: return "Unknown";
    }
}

ULONGLONG ZmSystem::GetSystemUptime()
{
    return GetTickCount64();
}

std::string ZmSystem::GetSystemBootTime()
{
    // 启动时间 = 当前时间 - 运行时长
    ULONGLONG tickMs = GetTickCount64();

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    // FILETIME 以 100 纳秒为单位，需要将毫秒转换为 100ns: × 10000
    ULONGLONG current100ns = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ULONGLONG boot100ns = current100ns - tickMs * 10000;
    ft.dwHighDateTime = (DWORD)(boot100ns >> 32);
    ft.dwLowDateTime = (DWORD)(boot100ns & 0xFFFFFFFF);

    FILETIME localFt;
    FileTimeToLocalFileTime(&ft, &localFt);
    SYSTEMTIME st;
    FileTimeToSystemTime(&localFt, &st);

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmSystem - Path

const char* ZmSystem::GetModuleDir(char* buf, int buflen)
{
    if (!buf || buflen <= 0) return "";
    GetModuleFileNameA(NULL, buf, buflen);
    // 截断最后一个反斜杠，仅保留目录部分
    char* p = strrchr(buf, '\\');
    if (p) *p = '\0';
    return buf;
}

const char* ZmSystem::GetTempDir(char* buf, int buflen)
{
    if (!buf || buflen <= 0) return "";
    GetTempPathA(buflen, buf);
    return buf;
}

const char* ZmSystem::GetAppDataDir(char* buf, int buflen)
{
    if (!buf || buflen <= 0) return "";
    // 通过读取 APPDATA 环境变量获取漫游应用数据目录
    if (!GetEnvironmentVariableA("APPDATA", buf, buflen))
        buf[0] = '\0';
    return buf;
}

std::wstring ZmSystem::GetSpecialFolder(int csidl)
{
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, buf)))
        return buf;
    return L"";
}

std::wstring ZmSystem::GetWindowsDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetWindowsDirectoryW(buf, MAX_PATH);
    return buf;
}

std::vector<std::wstring> ZmSystem::GetUserProfilePaths()
{
    std::vector<std::wstring> paths;
    HKEY hProfileList;

    // 枚举 HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList 下的所有子键
    // 每个子键对应一个用户配置，子键名即用户 SID
    LONG result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
        0,
        KEY_READ,
        &hProfileList
    );

    if (result != ERROR_SUCCESS) {
        return paths;
    }

    DWORD index = 0;
    WCHAR subkeyName[256];
    DWORD subkeyNameSize = sizeof(subkeyName) / sizeof(WCHAR);

    while (true) {
        result = RegEnumKeyExW(
            hProfileList,
            index,
            subkeyName,
            &subkeyNameSize,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        );

        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) {
            break;
        }

        std::wstring subkeyPath = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\";
        subkeyPath += subkeyName;

        HKEY hUserKey;
        result = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            subkeyPath.c_str(),
            0,
            KEY_READ,
            &hUserKey
        );

        if (result != ERROR_SUCCESS) {
            index++;
            subkeyNameSize = sizeof(subkeyName) / sizeof(WCHAR);
            continue;
        }

        // 读取 ProfileImagePath 值，可能包含环境变量如 %SystemDrive%\Users\Username
        DWORD type, dataSize = 0;
        result = RegQueryValueExW(
            hUserKey,
            L"ProfileImagePath",
            nullptr,
            &type,
            nullptr,
            &dataSize
        );

        if (result == ERROR_SUCCESS && (type == REG_EXPAND_SZ || type == REG_SZ)) {
            // 额外分配一个 WCHAR 确保以 null 结尾
            std::vector<BYTE> buffer(dataSize + sizeof(WCHAR));
            result = RegQueryValueExW(
                hUserKey,
                L"ProfileImagePath",
                nullptr,
                &type,
                buffer.data(),
                &dataSize
            );

            if (result == ERROR_SUCCESS) {
                const wchar_t* profilePath = reinterpret_cast<const wchar_t*>(buffer.data());
                std::wstring expandedPath;

                if (type == REG_EXPAND_SZ) {
                    // 展开环境变量，如 %SystemDrive% → C:
                    DWORD bufSize = ExpandEnvironmentStringsW(profilePath, nullptr, 0);
                    if (bufSize > 0) {
                        std::vector<wchar_t> expanded(bufSize);
                        ExpandEnvironmentStringsW(profilePath, expanded.data(), bufSize);
                        expandedPath = expanded.data();
                    }
                }
                else {
                    expandedPath = profilePath;
                }

                // 过滤系统服务账户路径，如 C:\Windows\ServiceProfiles\LocalService
                if (!expandedPath.empty() &&
                    expandedPath.find(L"ServiceProfiles") == std::wstring::npos) {
                    paths.push_back(expandedPath);
                }
            }
        }

        RegCloseKey(hUserKey);
        index++;
        // 每次枚举前必须重置 size，RegEnumKeyExW 要求传入缓冲区大小
        subkeyNameSize = sizeof(subkeyName) / sizeof(WCHAR);
    }

    RegCloseKey(hProfileList);
    return paths;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmSystem - Environment

bool ZmSystem::GetEnv(const char* name, char* buf, int buflen)
{
    if (!name || !buf || buflen <= 0) return false;
    return ::GetEnvironmentVariableA(name, buf, buflen) > 0;
}

bool ZmSystem::SetEnv(const char* name, const char* value)
{
    if (!name) return false;
    return ::SetEnvironmentVariableA(name, value) != 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmSystem - Process

uint32_t ZmSystem::GetPid()
{
    return (uint32_t)::GetCurrentProcessId();
}

uint32_t ZmSystem::GetTid()
{
    return (uint32_t)::GetCurrentThreadId();
}

bool ZmSystem::SetProcessPriority(int priority)
{
    // priority 到 Windows 优先级类的映射：
    // -2:Idle  -1:BelowNormal  0:Normal  1:AboveNormal  2:High  3:Realtime
    DWORD cls;
    switch (priority)
    {
    case -2: cls = IDLE_PRIORITY_CLASS;         break;
    case -1: cls = BELOW_NORMAL_PRIORITY_CLASS;  break;
    case  0: cls = NORMAL_PRIORITY_CLASS;        break;
    case  1: cls = ABOVE_NORMAL_PRIORITY_CLASS;  break;
    case  2: cls = HIGH_PRIORITY_CLASS;          break;
    case  3: cls = REALTIME_PRIORITY_CLASS;      break;
    default: return false;
    }
    return ::SetPriorityClass(::GetCurrentProcess(), cls) != 0;
}

bool ZmSystem::IsProcessRunning(const std::wstring& processName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(hSnap, &pe))
    {
        do
        {
            // 大小写不敏感比较，如 "Notepad.EXE" 和 "notepad.exe" 视为相同
            if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return found;
}

std::vector<std::pair<DWORD, std::wstring>> ZmSystem::GetProcessList()
{
    std::vector<std::pair<DWORD, std::wstring>> list;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return list;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe))
    {
        do
        {
            list.push_back({ pe.th32ProcessID, pe.szExeFile });
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return list;
}

std::wstring ZmSystem::GetCurrentProcessPath()
{
    wchar_t buf[MAX_PATH] = {};
    // 传入 NULL 表示获取当前进程的主模块路径（即 exe 自身）
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return buf;
}

// ============================================================================
// 系统负载
// ============================================================================

#include <thread>
#include <chrono>

ZmSystemLoad ZmSystem::GetSystemLoad()
{
    ZmSystemLoad load = {};

    // --- CPU ---
    // 两次采样间隔 100ms 计算差值
    static thread_local FILETIME s_lastIdle = {}, s_lastKernel = {}, s_lastUser = {};
    static thread_local bool s_firstCall = true;

    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user))
    {
        if (!s_firstCall)
        {
            auto toU64 = [](const FILETIME& ft) -> uint64_t {
                return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            };
            uint64_t dIdle   = toU64(idle)   - toU64(s_lastIdle);
            uint64_t dKernel = toU64(kernel) - toU64(s_lastKernel);
            uint64_t dUser   = toU64(user)   - toU64(s_lastUser);
            uint64_t dTotal  = dKernel + dUser;
            if (dTotal > 0)
                load.cpu_percent = (double)(dTotal - dIdle) * 100.0 / (double)dTotal;
        }
        s_lastIdle   = idle;
        s_lastKernel = kernel;
        s_lastUser   = user;
        s_firstCall  = false;
    }

    // --- 内存 ---
    MEMORYSTATUSEX mem = { sizeof(mem) };
    if (GlobalMemoryStatusEx(&mem))
    {
        load.total_memory_mb = mem.ullTotalPhys / (1024 * 1024);
        uint64_t availMB     = mem.ullAvailPhys / (1024 * 1024);
        load.used_memory_mb  = load.total_memory_mb - availMB;
        load.memory_percent  = (double)load.used_memory_mb * 100.0 / (double)load.total_memory_mb;
    }

    // --- GPU（PDH 性能计数器，不可用时静默回退）---
    load.has_gpu     = false;
    load.gpu_percent = -1.0;

    // 枚举 \GPU Engine(*)\Utilization Percentage 计数器，取最大值
    HQUERY hQuery = nullptr;
    HCOUNTER hCounter = nullptr;
    PDH_STATUS pdhStatus = PdhOpenQueryW(nullptr, 0, &hQuery);
    if (pdhStatus == ERROR_SUCCESS)
    {
        // 获取 GPU 引擎计数器列表（通配符路径，让 PDH 展开所有实例）
        pdhStatus = PdhAddCounterW(hQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &hCounter);
        if (pdhStatus == ERROR_SUCCESS)
        {
            PdhCollectQueryData(hQuery);
            // 需要两次采集才能拿到有效值
            Sleep(100);
            PdhCollectQueryData(hQuery);

            DWORD bufSize = 0, itemCount = 0;
            PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
            if (bufSize > 0)
            {
                std::vector<BYTE> buf(bufSize);
                auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                if (PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, items) == ERROR_SUCCESS)
                {
                    // 遍历所有 GPU 引擎实例，取最大利用率
                    double maxUtil = 0.0;
                    for (DWORD i = 0; i < itemCount; i++)
                    {
                        if (items[i].FmtValue.doubleValue > maxUtil)
                            maxUtil = items[i].FmtValue.doubleValue;
                    }
                    load.has_gpu     = true;
                    load.gpu_percent = maxUtil;
                }
            }
        }
        PdhCloseQuery(hQuery);
    }

    return load;
}
