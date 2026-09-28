#include "service.h"
#include "startup.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

static SERVICE_STATUS         g_status;
static SERVICE_STATUS_HANDLE  g_statusHandle = NULL;
static volatile LONG          g_stopping = 0;

void service_set_stopping(void) { InterlockedExchange(&g_stopping, 1); }
bool service_is_stopping(void)  { return InterlockedCompareExchange(&g_stopping, 0, 0) != 0; }

static void report_status(DWORD state, DWORD win32_exit) {
    if (!g_statusHandle) return;
    g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState            = state;
    g_status.dwControlsAccepted        = 0;
    g_status.dwWin32ExitCode           = win32_exit;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint              = 0;
    g_status.dwWaitHint                = 0;
    if (state == SERVICE_START_PENDING)
        g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    if (state == SERVICE_RUNNING)
        g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_statusHandle, &g_status);
}

static DWORD WINAPI ctrl_handler(DWORD ctrl, DWORD evtType,
                                 void* evtData, void* ctx) {
    (void)evtType; (void)evtData; (void)ctx;
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report_status(SERVICE_STOP_PENDING, 0);
            service_set_stopping();
            /* final STOPPED is reported by svc_main after the worker exits */
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            report_status(SERVICE_RUNNING, 0);
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

/* filled in by main.c; the service run loop is driven externally */
void service_on_run(void);  /* defined in main.c */

static void WINAPI svc_main(DWORD argc, wchar_t** argv) {
    (void)argc; (void)argv;
    g_statusHandle = RegisterServiceCtrlHandlerExW(SVC_NAME_W, ctrl_handler, NULL);
    if (!g_statusHandle) return;
    report_status(SERVICE_START_PENDING, 0);
    report_status(SERVICE_RUNNING, 0);
    service_on_run();          /* blocks while worker runs */
    report_status(SERVICE_STOPPED, 0);
}

bool service_run(void) {
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)SVC_NAME_W, svc_main },
        { NULL, NULL }
    };
    return StartServiceCtrlDispatcherW(table) != 0;
}

/* ---------------- install / uninstall ---------------- */

static wchar_t* widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = (wchar_t*)calloc(n, sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

bool service_install(const char* exe_path, bool auto_start) {
    SC_HANDLE mgr = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!mgr) return false;

    char cmd[MAX_PATH * 2];
    _snprintf(cmd, sizeof(cmd), "\"%s\" --service", exe_path);

    SC_HANDLE svc = CreateServiceA(
        mgr, SVC_NAME, SVC_DISPLAY,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        auto_start ? SERVICE_AUTO_START : SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL, cmd, NULL, NULL, NULL, NULL, NULL);
    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS) {
            /* 已存在：重新打开并更新可执行路径 */
            svc = OpenServiceA(mgr, SVC_NAME, SERVICE_ALL_ACCESS);
            if (!svc) {
                /* 打不开（未提权 / SCM 拒绝 SERVICE_ALL_ACCESS）时必须报失败，
                   否则会掉到函数末尾 return true，让调用方显示"安装成功"，
                   而实际上服务配置一点没变。 */
                DWORD e2 = GetLastError();
                CloseServiceHandle(mgr);
                LOG_ERROR("OpenService(existing) failed err=%lu", e2);
                SetLastError(e2);
                return false;
            }
            wchar_t* wcmd = widen(cmd);
            BOOL ok = wcmd && ChangeServiceConfigW(svc, SERVICE_NO_CHANGE,
                             SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                             wcmd, NULL, NULL, NULL, NULL, NULL, NULL);
            if (!ok) {
                DWORD le = GetLastError();
                CloseServiceHandle(svc);
                CloseServiceHandle(mgr);
                free(wcmd);
                LOG_ERROR("ChangeServiceConfig failed err=%lu", le);
                SetLastError(le);
                return false;
            }
            free(wcmd);
            CloseServiceHandle(svc);
        } else {
            CloseServiceHandle(mgr);
            LOG_ERROR("CreateService failed err=%lu", e);
            SetLastError(e);
            return false;
        }
    } else {
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(mgr);
    // 服务跑在 Session 0，桌面看不到托盘；补一个 ONLOGON 托盘伴随任务进用户会话
    startup_tray_for_service(true);
    LOG_INFO("tray companion task ensured for service");
    return true;
}

bool service_uninstall(void) {
    SC_HANDLE mgr = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return false;
    SC_HANDLE svc = OpenServiceA(mgr, SVC_NAME, DELETE);
    if (!svc) {
        CloseServiceHandle(mgr);
        return false;
    }
    BOOL ok = DeleteService(svc);
    DWORD e = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(mgr);
    if (!ok) LOG_ERROR("DeleteService failed err=%lu", e);
    else startup_tray_for_service(false);
    return ok != 0;
}

svc_state_t service_query_state(void) {
    SC_HANDLE mgr = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return SVC_UNKNOWN;
    SC_HANDLE svc = OpenServiceA(mgr, SVC_NAME, SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(mgr);
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) return SVC_NOT_INSTALLED;
        return SVC_UNKNOWN;
    }
    SERVICE_STATUS st;
    svc_state_t out = SVC_UNKNOWN;
    if (QueryServiceStatus(svc, &st)) {
        switch (st.dwCurrentState) {
            case SERVICE_STOPPED:        out = SVC_STOPPED; break;
            case SERVICE_RUNNING:        out = SVC_RUNNING; break;
            case SERVICE_START_PENDING:  out = SVC_START_PENDING; break;
            case SERVICE_STOP_PENDING:   out = SVC_STOP_PENDING; break;
            default:                     out = SVC_UNKNOWN; break;
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(mgr);
    return out;
}

const char* service_state_name(svc_state_t s) {
    switch (s) {
        case SVC_NOT_INSTALLED: return "未安装";
        case SVC_STOPPED:       return "已停止";
        case SVC_RUNNING:       return "运行中";
        case SVC_START_PENDING: return "正在启动";
        case SVC_STOP_PENDING:  return "正在停止";
        default:                return "未知";
    }
}

bool service_start_svc(void) {
    SC_HANDLE mgr = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return false;
    SC_HANDLE svc = OpenServiceA(mgr, SVC_NAME, SERVICE_START);
    if (!svc) { CloseServiceHandle(mgr); return false; }
    BOOL ok = StartServiceA(svc, 0, NULL);
    DWORD e = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(mgr);
    if (!ok && e != ERROR_SERVICE_ALREADY_RUNNING) {
        LOG_ERROR("StartService failed err=%lu", e);
        return false;
    }
    return true;
}

bool service_stop_svc(void) {
    SC_HANDLE mgr = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return false;
    SC_HANDLE svc = OpenServiceA(mgr, SVC_NAME, SERVICE_STOP);
    if (!svc) { CloseServiceHandle(mgr); return false; }
    SERVICE_STATUS st;
    BOOL ok = ControlService(svc, SERVICE_CONTROL_STOP, &st);
    DWORD e = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(mgr);
    if (!ok) {
        LOG_ERROR("ControlService STOP failed err=%lu", e);
        return false;
    }
    return true;
}

bool service_is_elevated(void) {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el; DWORD ret = 0;
    BOOL elevated = FALSE;
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &ret))
        elevated = el.TokenIsElevated;
    CloseHandle(tok);
    return elevated != 0;
}
