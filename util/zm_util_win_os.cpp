#include "zm_util_win_os.h"
#include <winternl.h>

// ========== OS Version ==========

OS_VERSION OSVersion()
{
    OS_VERSION osv = { 0 };

    do
    {
        // 使用 ntdll.dll 的 RtlGetVersion 获取真实版本号
        // 不使用已废弃的 GetVersionEx，后者受应用程序清单影响会返回错误版本
        RTL_OSVERSIONINFOEXW osvi = { sizeof(RTL_OSVERSIONINFOEXW) };
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");

        if (!hNtdll)
        {
            break;
        }

        NTSTATUS(WINAPI * pRtlGetVersion)(LPOSVERSIONINFOEXW) = (NTSTATUS(WINAPI*)(LPOSVERSIONINFOEXW))GetProcAddress(hNtdll, "RtlGetVersion");

        if (!pRtlGetVersion || pRtlGetVersion((PRTL_OSVERSIONINFOEXW)&osvi) != 0)
        {
            break;
        }

        osv.dwMajorVersion = osvi.dwMajorVersion;
        osv.dwMinorVersion = osvi.dwMinorVersion;
        osv.dwBuildNumber = osvi.dwBuildNumber;

    } while (false);

    return osv;
}

std::string GetOSVersionDetailed()
{
    OS_VERSION osvi = OSVersion();
    std::string version;

    if (osvi.dwMajorVersion == 10 && osvi.dwMinorVersion == 0)
    {
        // Windows 10 和 11 共享主版本号 10.0，通过 build 号区分
        version = "Windows 10";
        if (osvi.dwBuildNumber >= 22000)
            version = "Windows 11";
    }
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 3)
        version = "Windows 8.1";
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 2)
        version = "Windows 8";
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1)
        version = "Windows 7";
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 0)
        version = "Windows Vista";
    else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 1)
        version = "Windows XP";
    else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 2)
    {
        // 5.2 对应 XP x64 和 Server 2003，通过 SM_SERVERR2 (值为89) 区分
        version = (GetSystemMetrics(89) == 0) ? "Windows XP Professional x64 Edition" : "Windows Server 2003";
    }
    else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 0)
        version = "Windows 2000";
    else
        version = "Unknown Windows Version";

    version += " (Build " + std::to_string(osvi.dwBuildNumber) + ")";

    return version;
}

// ========== OS Version Checks ==========

bool IsWin7()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion == 6 && osv.dwMinorVersion == 1);
}

bool IsWin7Higher()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion > 6) || (osv.dwMajorVersion == 6 && osv.dwMinorVersion > 1);
}

bool IsWin7Lower()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion < 6) || (osv.dwMajorVersion == 6 && osv.dwMinorVersion < 1);
}

bool IsWin8()
{
    OS_VERSION osv = OSVersion();
    // Windows 8 为 6.2，Windows 8.1 为 6.3，两者视为同一代
    return (osv.dwMajorVersion == 6 && osv.dwMinorVersion == 2) || (osv.dwMajorVersion == 6 && osv.dwMinorVersion == 3);
}

bool IsWin8Higher()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion > 6) || (osv.dwMajorVersion == 6 && osv.dwMinorVersion > 2);
}

bool IsWin8Lower()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion < 6) || (osv.dwMajorVersion == 6 && osv.dwMinorVersion < 2);
}

bool IsWin10()
{
    OS_VERSION osv = OSVersion();
    // Windows 10 和 11 共享 10.0，通过 build < 22000 限定为 Win10
    return (osv.dwMajorVersion == 10 && osv.dwMinorVersion == 0 && osv.dwBuildNumber < 22000);
}

bool IsWin10Higher()
{
    OS_VERSION osv = OSVersion();
    // 包含 Windows 11 (build ≥ 22000) 及未来版本
    return (osv.dwMajorVersion > 10) || (osv.dwMajorVersion == 10 && osv.dwBuildNumber >= 22000);
}

bool IsWin10Lower()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion < 10);
}

bool IsWin11()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion == 10 && osv.dwMinorVersion == 0 && osv.dwBuildNumber >= 22000);
}

bool IsWin11Higher()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion > 10);
}

bool IsWin11Lower()
{
    OS_VERSION osv = OSVersion();
    return (osv.dwMajorVersion < 10) || (osv.dwMajorVersion == 10 && osv.dwBuildNumber < 22000);
}

// ========== Session ==========

DWORD GetActiveSessionId()
{
    // 动态加载 kernel32.dll 中的 WTSGetActiveConsoleSessionId，
    // 避免静态链接 wtsapi32.lib 产生额外依赖
    HMODULE hKern = GetModuleHandleW(L"kernel32.dll");
    if (!hKern) return (DWORD)-1;
    auto fn = (DWORD(WINAPI*)())GetProcAddress(hKern, "WTSGetActiveConsoleSessionId");
    if (!fn) return (DWORD)-1;
    return fn();
}

bool IsServiceSession()
{
    DWORD sessionId = 0;
    // Session 0 隔离: Windows Vista+ 服务运行在 Session 0，用户会话从 Session 1 开始
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    return sessionId == 0;
}

// ========== Security ==========

bool IsElevated()
{
    BOOL elevated = FALSE;
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION te = {};
        DWORD size;
        if (GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &size))
            elevated = te.TokenIsElevated;
        CloseHandle(hToken);
    }
    return elevated == TRUE;
}

// ========== Idle ==========

DWORD GetSystemIdleTime()
{
    LASTINPUTINFO lii = {};
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return 0;
    // GetTickCount 返回 DWORD (32位)，系统运行超过 49.7 天后会回绕
    return GetTickCount() - lii.dwTime;
}

// ========== Power ==========

bool PowerOptions(POWER_OPTIONS option, DWORD time)
{
    // 提升关机权限
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (::OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        ::LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ::AdjustTokenPrivileges(hToken, false, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
        ::CloseHandle(hToken);
    }

    if (time > 0)
        Sleep(time * 1000);

    bool bRet = false;
    switch (option)
    {
    case PO_POWEROFF:
        bRet = InitiateSystemShutdownExW(NULL, NULL, 0, TRUE, FALSE,
            SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER) != FALSE;
        break;
    case PO_REBOOT:
        bRet = InitiateSystemShutdownExW(NULL, NULL, 0, TRUE, TRUE,
            SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER) != FALSE;
        break;
    case PO_LOGOFF:
        bRet = ExitWindowsEx(EWX_LOGOFF | EWX_FORCE, 0) != FALSE;
        break;
    default:
        break;
    }
    return bRet;
}

bool PowerOptionsEx(POWER_OPTIONS option)
{
    // 提升关机权限
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        TOKEN_PRIVILEGES tp = {};
        LUID luid;
        LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
        CloseHandle(hToken);
    }

    switch (option)
    {
    case PO_POWEROFF:
        // InitiateSystemShutdownExW 可从服务会话 (Session 0) 调用，ExitWindowsEx 则不能
        return InitiateSystemShutdownExW(NULL, NULL, 0, TRUE, FALSE,
            SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER) != FALSE;
    case PO_REBOOT:
        return InitiateSystemShutdownExW(NULL, NULL, 0, TRUE, TRUE,
            SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER) != FALSE;
    case PO_LOGOFF:
        // 注销操作仍使用 ExitWindowsEx + EWX_FORCE 强制结束用户进程
        return ExitWindowsEx(EWX_LOGOFF | EWX_FORCE, 0) != FALSE;
    default:
        return false;
    }
}
