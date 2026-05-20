#ifndef ZM_UTIL_WIN_OS_H
#define ZM_UTIL_WIN_OS_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// ========== Types ==========

/**
 * @brief 操作系统版本信息，由 OSVersion() 填充
 *
 * Windows 版本号对照表：
 *   Windows 11      → 10.0 (build ≥ 22000)
 *   Windows 10      → 10.0 (build < 22000)
 *   Windows 8.1     → 6.3
 *   Windows 8       → 6.2
 *   Windows 7       → 6.1
 *   Windows Vista   → 6.0
 *   Windows XP      → 5.1
 */
struct OS_VERSION
{
    DWORD dwMajorVersion = 0;   ///< 主版本号，如 Windows 10/11 为 10
    DWORD dwMinorVersion = 0;   ///< 次版本号，如 Windows 10 为 0
    DWORD dwBuildNumber = 0;    ///< 构建号，如 Windows 11 首版为 22000
};

/**
 * @brief 系统电源操作类型
 */
enum POWER_OPTIONS
{
    PO_POWEROFF = 0,    ///< 关机
    PO_REBOOT,          ///< 重启
    PO_LOGOFF           ///< 注销
};

// ========== OS Version ==========

/**
 * @brief 获取当前操作系统的版本号
 *
 * 通过 ntdll.dll 的 RtlGetVersion 获取真实版本号，不受应用程序清单兼容性设置影响
 * (GetVersionEx 在 Windows 8.1+ 会被清单欺骗返回错误版本)
 *
 * @return OS_VERSION 结构体，获取失败时所有字段为 0
 *
 * @example
 *   OS_VERSION osv = OSVersion();
 *   printf("Version: %d.%d Build %d\n", osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber);
 */
OS_VERSION OSVersion();

/**
 * @brief 获取当前操作系统的可读版本名称
 * @return 版本名称字符串，如 "Windows 11 (Build 22631)"，无法识别时返回 "Unknown Windows Version (Build x)"
 *
 * @example
 *   std::string ver = GetOSVersionDetailed();
 *   printf("OS: %s\n", ver.c_str());  // "Windows 11 (Build 22631)"
 */
std::string GetOSVersionDetailed();

// ========== OS Version Checks ==========

/**
 * @brief 当前系统是否为 Windows 7
 * @return true 表示当前系统为 Windows 7 (6.1)
 */
bool IsWin7();

/**
 * @brief 当前系统是否为 Windows 7 或更高版本
 * @return true 表示当前系统版本 ≥ Windows 7 (6.1)
 */
bool IsWin7Higher();

/**
 * @brief 当前系统是否低于 Windows 7
 * @return true 表示当前系统版本 < Windows 7 (6.1)
 */
bool IsWin7Lower();

/**
 * @brief 当前系统是否为 Windows 8 或 8.1
 * @return true 表示当前系统为 Windows 8 (6.2) 或 Windows 8.1 (6.3)
 */
bool IsWin8();

/**
 * @brief 当前系统是否为 Windows 8 或更高版本
 * @return true 表示当前系统版本 ≥ Windows 8 (6.2)
 */
bool IsWin8Higher();

/**
 * @brief 当前系统是否低于 Windows 8
 * @return true 表示当前系统版本 < Windows 8 (6.2)
 */
bool IsWin8Lower();

/**
 * @brief 当前系统是否为 Windows 10
 * @return true 表示当前系统为 Windows 10 (10.0, build < 22000)
 */
bool IsWin10();

/**
 * @brief 当前系统是否为 Windows 10 或更高版本 (含 Windows 11)
 * @return true 表示当前系统版本 ≥ Windows 10 (含 Windows 11)
 */
bool IsWin10Higher();

/**
 * @brief 当前系统是否低于 Windows 10
 * @return true 表示当前系统版本 < Windows 10 (不含 Windows 11)
 */
bool IsWin10Lower();

/**
 * @brief 当前系统是否为 Windows 11
 * @return true 表示当前系统为 Windows 11 (10.0, build ≥ 22000)
 */
bool IsWin11();

/**
 * @brief 当前系统是否高于 Windows 11
 * @return true 表示当前系统版本 > Windows 11 (未来版本)
 */
bool IsWin11Higher();

/**
 * @brief 当前系统是否低于 Windows 11
 * @return true 表示当前系统版本 < Windows 11 (build < 22000)
 */
bool IsWin11Lower();

// ========== Session ==========

/**
 * @brief 获取当前连接到物理控制台的活跃用户会话 ID
 *
 * 服务进程运行在 Session 0，此函数用于获取实际登录用户的会话 ID，
 * 以便服务与用户进程交互（如创建进程到用户桌面）
 *
 * @return 会话 ID，获取失败返回 (DWORD)-1
 *
 * @example
 *   DWORD sessionId = GetActiveSessionId();
 *   if (sessionId != (DWORD)-1)
 *       printf("Active session: %u\n", sessionId);
 */
DWORD GetActiveSessionId();

/**
 * @brief 当前进程是否运行在服务会话 (Session 0) 中
 * @return true 表示当前运行在 Session 0，即作为服务运行
 *
 * @example
 *   if (IsServiceSession())
 *       printf("Running as service in Session 0\n");
 */
bool IsServiceSession();

// ========== Security ==========

/**
 * @brief 检测当前进程是否以管理员权限运行 (UAC 提升)
 * @return true 表示当前进程已提升为管理员权限
 *
 * @example
 *   if (!IsElevated())
 *       printf("Not running as administrator\n");
 */
bool IsElevated();

// ========== Idle ==========

/**
 * @brief 获取用户自最后一次输入以来的空闲时间
 * @return 空闲时间（毫秒）。注意: 使用 GetTickCount (32位)，系统运行超过 49.7 天后可能溢出
 *
 * @example
 *   DWORD idle = GetSystemIdleTime();
 *   if (idle > 5 * 60 * 1000)
 *       printf("User idle for %u seconds\n", idle / 1000);
 */
DWORD GetSystemIdleTime();

// ========== Power ==========

/**
 * @brief 执行系统电源操作（带延时）
 *
 * 先尝试提升 SE_SHUTDOWN_NAME 权限，然后等待指定秒数后执行操作。
 * 注意: 此函数在服务进程中可能不可靠，服务场景建议使用 PowerOptionsEx()
 *
 * @param option 电源操作类型 (PO_POWEROFF / PO_REBOOT / PO_LOGOFF)
 * @param time   延时秒数，0 表示立即执行
 * @return true 表示操作发起成功（关机/重启时函数不一定能返回）
 *
 * @example
 *   PowerOptions(PO_REBOOT, 5);  // 5秒后重启
 */
bool PowerOptions(POWER_OPTIONS option, DWORD time);

/**
 * @brief 执行系统电源操作（服务进程增强版）
 *
 * 使用 InitiateSystemShutdownExW 替代 ExitWindowsEx，在服务会话 (Session 0) 中
 * 能可靠执行关机/重启操作。注销操作仍使用 ExitWindowsEx + EWX_FORCE
 *
 * @param option 电源操作类型 (PO_POWEROFF / PO_REBOOT / PO_LOGOFF)
 * @return true 表示操作发起成功
 *
 * @example
 *   if (IsServiceSession())
 *       PowerOptionsEx(PO_POWEROFF);  // 服务进程安全关机
 */
bool PowerOptionsEx(POWER_OPTIONS option);

#endif
