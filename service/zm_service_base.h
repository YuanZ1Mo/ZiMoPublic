#ifndef ZM_SERVICE_BASE_H_
#define ZM_SERVICE_BASE_H_

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../util/zm_util_str.h"
#include <tchar.h>

#include <vector>

// Windows 服务基类
// 子类继承并实现 OnStart 等虚函数，由 SCM 回调驱动生命周期
class ZmServiceBase
{
public:
    ZmServiceBase(const ZmServiceBase& other) = delete;
    ZmServiceBase& operator=(const ZmServiceBase& other) = delete;
    ZmServiceBase(ZmServiceBase&& other) = delete;
    ZmServiceBase& operator=(ZmServiceBase&& other) = delete;

    virtual ~ZmServiceBase() = default;

    // 连接 SCM 并启动服务调度器（阻塞直到服务停止）
    bool Run();

    // 调试模式：跳过 SCM，直接调用 SvcMain
    void RunDebugMode(DWORD argc, TCHAR* argv[]);

    // 服务注册信息访问器
    const TCHAR* GetName() const;
    const TCHAR* GetDisplayName() const;
    const TCHAR* GetDescription() const;
    DWORD GetStartType() const;
    DWORD GetErrorControlType() const;
    const TCHAR* GetDependencies() const;
    const TCHAR* GetAccount() const;
    const TCHAR* GetPassword() const;

protected:
    // name         - 服务内部名称（注册表键名）
    // displayName  - 服务显示名称（services.msc 中的名称列）
    // description  - 服务描述（services.msc 中的描述列）
    // dwStartType  - 启动类型（SERVICE_AUTO_START 等）
    // dwErrCtrlType - 错误控制类型，默认 SERVICE_ERROR_NORMAL
    // dwAcceptedCmds - 接受的控制码位掩码，默认 SERVICE_ACCEPT_STOP
    // depends      - 依赖的服务列表
    // account      - 运行账户
    // password     - 运行账户密码
    ZmServiceBase(const String& name,
        const String& displayName,
        const String& description,
        DWORD dwStartType,
        DWORD dwErrCtrlType = SERVICE_ERROR_NORMAL,
        DWORD dwAcceptedCmds = SERVICE_ACCEPT_STOP,
        const String& depends = _T(""),
        const String& account = _T(""),
        const String& password = _T(""));

    // 向 SCM 报告服务状态
    void SetStatus(DWORD dwState, DWORD dwErrCode = NO_ERROR, DWORD dwWait = 0);

    // 获取服务状态句柄（用于 RegisterPowerSettingNotification 等）
    SERVICE_STATUS_HANDLE GetStatusHandle() const { return m_svcStatusHandle; }

    // 写入 Windows 事件日志
    void WriteToEventLog(const String& msg, WORD type = EVENTLOG_INFORMATION_TYPE);

    // 生命周期回调 —— 子类按需重写
    virtual void OnStart(DWORD argc, TCHAR* argv[]) = 0;
    virtual void OnStop() {}
    virtual void OnPause() {}
    virtual void OnContinue() {}
    virtual void OnShutdown(DWORD evtType, WTSSESSION_NOTIFICATION* notification);
    virtual void OnSessionChange(DWORD evtType, WTSSESSION_NOTIFICATION* notification);
    virtual void OnPowerChange(DWORD evtType, POWERBROADCAST_SETTING* notification);

private:
    // SCM 入口点
    static void WINAPI SvcMain(DWORD argc, TCHAR* argv[]);

    // SCM 控制处理程序
    static DWORD WINAPI ServiceCtrlHandlerEx(DWORD ctrlCode, DWORD evtType, void* evtData, void* context);

    // 内部状态转换
    void Start(DWORD argc, TCHAR* argv[]);
    void Stop();
    void Pause();
    void Continue();
    void Shutdown();

    // 服务注册信息
    String m_name;
    String m_displayName;
    String m_description;
    DWORD m_dwStartType;
    DWORD m_dwErrorCtrlType;
    String m_depends;
    String m_account;
    String m_password;

    // 服务运行时状态
    SERVICE_STATUS m_svcStatus;
    SERVICE_STATUS_HANDLE m_svcStatusHandle;

    // 电源通知注册（当 SERVICE_ACCEPT_POWEREVENT 时自动管理）
    std::vector<HPOWERNOTIFY> m_powerNotifyHandles;
    void registerPowerNotifications();
    void unregisterPowerNotifications();

    // 单例指针，供静态回调转发到实例
    static ZmServiceBase* m_service;
};

// Windows 服务安装/卸载/启停工具类
class ZMServiceManager
{
public:
    static bool Install(const ZmServiceBase& service);
    static bool Uninstall(const ZmServiceBase& service);
    static bool Start(const ZmServiceBase& service);
    static bool Stop(const ZmServiceBase& service);

private:
    ZMServiceManager() = default;
};

#endif // ZM_SERVICE_BASE_H_
