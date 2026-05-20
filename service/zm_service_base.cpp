#include "zm_service_base.h"
#include "../spdlog/zm_logger.h"

#include <cassert>
#include <algorithm>
#include <strsafe.h>

// ============================================================================
// ZmServiceBase
// ============================================================================

ZmServiceBase* ZmServiceBase::m_service = nullptr;

ZmServiceBase::ZmServiceBase(const String& name,
    const String& displayName,
    const String& description,
    DWORD dwStartType,
    DWORD dwErrCtrlType,
    DWORD dwAcceptedCmds,
    const String& depends,
    const String& account,
    const String& password)
    : m_name(name)
    , m_displayName(displayName)
    , m_description(description)
    , m_dwStartType(dwStartType)
    , m_dwErrorCtrlType(dwErrCtrlType)
    , m_depends(depends)
    , m_account(account)
    , m_password(password)
    , m_svcStatusHandle(nullptr)
{
    m_svcStatus.dwControlsAccepted = dwAcceptedCmds;
    m_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    m_svcStatus.dwCurrentState = SERVICE_START_PENDING;
    m_svcStatus.dwWin32ExitCode = NO_ERROR;
    m_svcStatus.dwServiceSpecificExitCode = 0;
    m_svcStatus.dwCheckPoint = 0;
    m_svcStatus.dwWaitHint = 0;
}

// --- 访问器 ---

const TCHAR* ZmServiceBase::GetName() const { return m_name.c_str(); }
const TCHAR* ZmServiceBase::GetDisplayName() const { return m_displayName.c_str(); }
const TCHAR* ZmServiceBase::GetDescription() const { return m_description.c_str(); }
DWORD ZmServiceBase::GetStartType() const { return m_dwStartType; }
DWORD ZmServiceBase::GetErrorControlType() const { return m_dwErrorCtrlType; }
const TCHAR* ZmServiceBase::GetDependencies() const { return m_depends.c_str(); }
const TCHAR* ZmServiceBase::GetAccount() const { return m_account.c_str(); }
const TCHAR* ZmServiceBase::GetPassword() const { return m_password.c_str(); }

// --- 运行 ---

bool ZmServiceBase::Run()
{
    m_service = this;

    SERVICE_TABLE_ENTRY tableEntry[] = {
        { const_cast<LPTSTR>(GetName()), SvcMain },
        { nullptr, nullptr }
    };

    // StartServiceCtrlDispatcher 会阻塞直到服务停止
    return ::StartServiceCtrlDispatcher(tableEntry) == TRUE;
}

void ZmServiceBase::RunDebugMode(DWORD argc, TCHAR* argv[])
{
    m_service = this;
    SvcMain(argc, argv);
}

// --- 状态管理 ---

void ZmServiceBase::SetStatus(DWORD dwState, DWORD dwErrCode, DWORD dwWait)
{
    SERVICE_STATUS status = m_svcStatus;
    status.dwCurrentState = dwState;
    status.dwWin32ExitCode = dwErrCode;
    status.dwWaitHint = dwWait;

    // 仅在 SCM 接受时才更新本地缓存
    if (::SetServiceStatus(m_svcStatusHandle, &status))
    {
        m_svcStatus = status;
    }
}

// --- 事件日志 ---

void ZmServiceBase::WriteToEventLog(const String& msg, WORD type)
{
    HANDLE hSource = ::RegisterEventSource(nullptr, m_name.c_str());
    if (hSource)
    {
        const TCHAR* msgData[2] = { m_name.c_str(), msg.c_str() };
        ::ReportEvent(hSource, type, 0, 0, nullptr, 2, 0, msgData, nullptr);
        ::DeregisterEventSource(hSource);
    }
}

// --- SCM 静态回调 ---

void WINAPI ZmServiceBase::SvcMain(DWORD argc, TCHAR* argv[])
{
    assert(m_service);

    // 注册控制处理器，后续 SCM 命令通过 ServiceCtrlHandlerEx 回调
    m_service->m_svcStatusHandle = ::RegisterServiceCtrlHandlerEx(
        m_service->GetName(),
        ServiceCtrlHandlerEx,
        nullptr);

    if (!m_service->m_svcStatusHandle)
    {
        m_service->WriteToEventLog(_T("Can't set service control handler"), EVENTLOG_ERROR_TYPE);
        PUBLIC_LOG_ERROR("无法设置服务控制句柄");
        return;
    }

    m_service->Start(argc, argv);
}

DWORD WINAPI ZmServiceBase::ServiceCtrlHandlerEx(DWORD ctrlCode, DWORD evtType, void* evtData, void* /*context*/)
{
    switch (ctrlCode)
    {
    case SERVICE_CONTROL_STOP:
        m_service->Stop();
        break;

    case SERVICE_CONTROL_PAUSE:
        m_service->Pause();
        break;

    case SERVICE_CONTROL_CONTINUE:
        m_service->Continue();
        break;

    case SERVICE_CONTROL_SHUTDOWN:
        m_service->OnShutdown(evtType, reinterpret_cast<WTSSESSION_NOTIFICATION*>(evtData));
        m_service->Shutdown();
        break;

    case SERVICE_CONTROL_SESSIONCHANGE:
        m_service->OnSessionChange(evtType, reinterpret_cast<WTSSESSION_NOTIFICATION*>(evtData));
        break;

    case SERVICE_CONTROL_POWEREVENT:
        m_service->OnPowerChange(evtType, reinterpret_cast<POWERBROADCAST_SETTING*>(evtData));
        break;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    return NO_ERROR;
}

// --- 内部状态转换 ---

void ZmServiceBase::Start(DWORD argc, TCHAR* argv[])
{
    SetStatus(SERVICE_START_PENDING);
    OnStart(argc, argv);
    registerPowerNotifications();
    SetStatus(SERVICE_RUNNING);
}

void ZmServiceBase::Stop()
{
    SetStatus(SERVICE_STOP_PENDING);
    OnStop();
    unregisterPowerNotifications();
    SetStatus(SERVICE_STOPPED);
}

void ZmServiceBase::Pause()
{
    SetStatus(SERVICE_PAUSE_PENDING);
    OnPause();
    SetStatus(SERVICE_PAUSED);
}

void ZmServiceBase::Continue()
{
    SetStatus(SERVICE_CONTINUE_PENDING);
    OnContinue();
    SetStatus(SERVICE_RUNNING);
}

void ZmServiceBase::Shutdown()
{
    Stop();
}

// --- 虚函数默认实现（声明在头文件中，此处提供定义） ---

void ZmServiceBase::OnShutdown(DWORD /*evtType*/, WTSSESSION_NOTIFICATION* /*notification*/) {}
void ZmServiceBase::OnSessionChange(DWORD /*evtType*/, WTSSESSION_NOTIFICATION* /*notification*/) {}
void ZmServiceBase::OnPowerChange(DWORD /*evtType*/, POWERBROADCAST_SETTING* /*notification*/) {}

// --- 电源通知注册 ---

void ZmServiceBase::registerPowerNotifications()
{
    if (m_svcStatus.dwControlsAccepted & SERVICE_ACCEPT_POWEREVENT)
    {
        if (!m_svcStatusHandle)
        {
            PUBLIC_LOG_ERROR("注册电源通知失败: 服务状态句柄无效");
            return;
        }

        static const GUID s_powerGuids[] = {
            // 电池剩余百分比
            { 0xa7ad8041, 0xb45a, 0x4cae, { 0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1 } },
            // 显示器亮度
            { 0xaded5e82, 0xb909, 0x4619, { 0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb } },
            // 系统睡眠超时
            { 0x29f6c1db, 0x86da, 0x48c5, { 0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda } },
            // 显示器电源开关
            { 0x02731015, 0x4510, 0x4526, { 0xad, 0x22, 0xe8, 0x6d, 0x95, 0x1b, 0x08, 0x56 } },
            // 节能模式状态
            { 0xe00958c0, 0xc213, 0x4dfc, { 0xa2, 0x0e, 0x40, 0x13, 0x74, 0x50, 0x4b, 0x5c } },
        };

        m_powerNotifyHandles.clear();

        for (const GUID& guid : s_powerGuids)
        {
            HPOWERNOTIFY h = ::RegisterPowerSettingNotification(
                reinterpret_cast<HANDLE>(m_svcStatusHandle),
                &guid,
                DEVICE_NOTIFY_SERVICE_HANDLE);

            if (h)
                m_powerNotifyHandles.push_back(h);
            else
                PUBLIC_LOG_WARN("注册电源通知失败 ErrCode: {}", ::GetLastError());
        }

        PUBLIC_LOG_INFO("电源通知注册完成 已注册{}个", m_powerNotifyHandles.size());
    }
}

void ZmServiceBase::unregisterPowerNotifications()
{
    for (HPOWERNOTIFY h : m_powerNotifyHandles)
    {
        if (h)
            ::UnregisterPowerSettingNotification(h);
    }
    m_powerNotifyHandles.clear();
}

// ============================================================================
// ServiceHandle - SC_HANDLE RAII 包装
// ============================================================================

namespace
{
    class ServiceHandle
    {
    public:
        ServiceHandle(SC_HANDLE handle)
            : m_handle(handle) {}

        ~ServiceHandle()
        {
            if (m_handle)
            {
                ::CloseServiceHandle(m_handle);
            }
        }

        // 禁止拷贝和移动
        ServiceHandle(const ServiceHandle&) = delete;
        ServiceHandle& operator=(const ServiceHandle&) = delete;

        operator SC_HANDLE() const { return m_handle; }

    private:
        SC_HANDLE m_handle;
    };
}

// ============================================================================
// ZMServiceManager
// ============================================================================

bool ZMServiceManager::Install(const ZmServiceBase& service)
{
    // 获取当前 exe 路径并加引号，防止路径含空格
    TCHAR szFilePath[MAX_PATH] = {};
    ::GetModuleFileName(nullptr, szFilePath, MAX_PATH);

    TCHAR szServicePath[MAX_PATH] = {};
    ::StringCchPrintf(szServicePath, MAX_PATH, _T("\"%s\""), szFilePath);

    // 打开服务控制管理器
    ServiceHandle svcControlManager = ::OpenSCManager(
        nullptr, nullptr,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);

    if (!svcControlManager)
    {
        PUBLIC_LOG_ERROR("打开服务管理器失败 ErrCode: {}", ::GetLastError());
        return false;
    }

    // 读取可选参数
    const String depends = service.GetDependencies();
    const String acc = service.GetAccount();
    const String pass = service.GetPassword();

    // 创建服务
    ServiceHandle servHandle = ::CreateService(
        svcControlManager,
        service.GetName(),
        service.GetDisplayName(),
        SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG,
        SERVICE_WIN32_OWN_PROCESS,
        service.GetStartType(),
        service.GetErrorControlType(),
        szServicePath,
        nullptr,
        nullptr,
        (depends.empty() ? nullptr : depends.c_str()),
        (acc.empty() ? nullptr : acc.c_str()),
        (pass.empty() ? nullptr : pass.c_str()));

    if (!servHandle)
    {
        PUBLIC_LOG_ERROR("无法创建服务 ErrCode: {}", ::GetLastError());
        return false;
    }

    // 设置服务描述（services.msc 中的描述列）
    SERVICE_DESCRIPTION sd = {};
    sd.lpDescription = const_cast<LPTSTR>(service.GetDescription());
    if (!::ChangeServiceConfig2(servHandle, SERVICE_CONFIG_DESCRIPTION, &sd))
    {
        PUBLIC_LOG_WARN("设置服务描述失败 ErrCode: {}", ::GetLastError());
    }

    PUBLIC_LOG_INFO("创建服务成功");
    return true;
}

bool ZMServiceManager::Uninstall(const ZmServiceBase& service)
{
    // 打开服务控制管理器
    ServiceHandle svcControlManager = ::OpenSCManager(
        nullptr, nullptr, SC_MANAGER_CONNECT);

    if (!svcControlManager)
    {
        PUBLIC_LOG_ERROR("无法打开服务管理器 ErrCode: {}", ::GetLastError());
        return false;
    }

    // 打开目标服务
    ServiceHandle servHandle = ::OpenService(
        svcControlManager, service.GetName(),
        SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);

    if (!servHandle)
    {
        PUBLIC_LOG_ERROR("无法打开服务 ErrCode: {}", ::GetLastError());
        return false;
    }

    // 尝试停止正在运行的服务
    SERVICE_STATUS servStatus = {};
    if (::ControlService(servHandle, SERVICE_CONTROL_STOP, &servStatus))
    {
        PUBLIC_LOG_INFO("正在停止服务...");

        // 等待服务停止，每次轮询间隔 1 秒，最多等待 30 秒
        const int maxRetries = 30;
        for (int i = 0; i < maxRetries; ++i)
        {
            if (!::QueryServiceStatus(servHandle, &servStatus))
                break;
            if (servStatus.dwCurrentState != SERVICE_STOP_PENDING)
                break;
            ::Sleep(1000);
        }

        if (servStatus.dwCurrentState != SERVICE_STOPPED)
        {
            PUBLIC_LOG_ERROR("无法停止服务，当前状态: {}", servStatus.dwCurrentState);
        }
        else
        {
            PUBLIC_LOG_INFO("服务已停止");
        }
    }
    else
    {
        // 服务未在运行，这不是错误
        DWORD err = ::GetLastError();
        if (err != ERROR_SERVICE_NOT_ACTIVE)
        {
            PUBLIC_LOG_ERROR("无法控制服务 ErrCode: {}", err);
        }
        else
        {
            PUBLIC_LOG_INFO("服务未在运行，跳过停止步骤");
        }
    }

    // 删除服务
    if (!::DeleteService(servHandle))
    {
        PUBLIC_LOG_ERROR("删除服务失败 ErrCode: {}", ::GetLastError());
        return false;
    }

    PUBLIC_LOG_INFO("服务已卸载");
    return true;
}

bool ZMServiceManager::Start(const ZmServiceBase& service)
{
    ServiceHandle svcControlManager = ::OpenSCManager(
        nullptr, nullptr, SC_MANAGER_CONNECT);

    if (!svcControlManager)
    {
        PUBLIC_LOG_ERROR("无法打开服务管理器 ErrCode: {}", ::GetLastError());
        return false;
    }

    ServiceHandle servHandle = ::OpenService(
        svcControlManager, service.GetName(), SERVICE_START);

    if (!servHandle)
    {
        PUBLIC_LOG_ERROR("无法打开服务 ErrCode: {}", ::GetLastError());
        return false;
    }

    if (!::StartService(servHandle, 0, nullptr))
    {
        PUBLIC_LOG_ERROR("启动服务失败 ErrCode: {}", ::GetLastError());
        return false;
    }

    PUBLIC_LOG_INFO("服务已启动");
    return true;
}

bool ZMServiceManager::Stop(const ZmServiceBase& service)
{
    ServiceHandle svcControlManager = ::OpenSCManager(
        nullptr, nullptr, SC_MANAGER_CONNECT);

    if (!svcControlManager)
    {
        PUBLIC_LOG_ERROR("无法打开服务管理器 ErrCode: {}", ::GetLastError());
        return false;
    }

    ServiceHandle servHandle = ::OpenService(
        svcControlManager, service.GetName(),
        SERVICE_QUERY_STATUS | SERVICE_STOP);

    if (!servHandle)
    {
        PUBLIC_LOG_ERROR("无法打开服务 ErrCode: {}", ::GetLastError());
        return false;
    }

    SERVICE_STATUS servStatus = {};
    if (!::ControlService(servHandle, SERVICE_CONTROL_STOP, &servStatus))
    {
        PUBLIC_LOG_ERROR("停止服务失败 ErrCode: {}", ::GetLastError());
        return false;
    }

    PUBLIC_LOG_INFO("服务已停止");
    return true;
}
