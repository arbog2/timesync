#include "service.h"
#include "startup.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

static SERVICE_STATUS         g_status;
static SERVICE_STATUS_HANDLE  g_statusHandle = NULL;
static volatile LONG          g_stopping = 0;

/* g_status 同时被 svc_main 线程（上报 START_PENDING/RUNNING/STOPPED）和
   SCM 自己的控制线程（ctrl_handler 回调）写。两个线程各写一遍同一个
   SERVICE_STATUS，SetServiceStatus 可能读到"半新半旧"的组合
   （例如 dwCurrentState 已是 STOPPED 而 dwCheckPoint 还留着旧值），
   services.msc 上表现为状态跳变、卡在"正在停止"。
   这里用一把临界区把"填结构 + SetServiceStatus"变成原子操作。
   临界区只在回调里短暂持有，不会拖住 SCM 的控制线程。 */
static CRITICAL_SECTION g_stLock;
static volatile LONG    g_stLockInited = 0;

static void st_lock_init(void) {
    if (InterlockedCompareExchange(&g_stLockInited, 1, 0) == 0)
        InitializeCriticalSectionAndSpinCount(&g_stLock, 0x400);
}

void service_set_stopping(void) { InterlockedExchange(&g_stopping, 1); }
bool service_is_stopping(void)  { return InterlockedCompareExchange(&g_stopping, 0, 0) != 0; }

/* 调用方必须已持有 g_stLock */
static void report_status_locked(DWORD state, DWORD win32_exit) {
    if (!g_statusHandle) return;
    g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState            = state;
    g_status.dwWin32ExitCode           = win32_exit;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint              = 0;
    g_status.dwWaitHint                = 0;
    if (state == SERVICE_START_PENDING)
        g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    else if (state == SERVICE_STOP_PENDING)
        g_status.dwControlsAccepted = 0;
    else if (state == SERVICE_RUNNING)
        g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    else
        g_status.dwControlsAccepted = 0;
    SetServiceStatus(g_statusHandle, &g_status);
}

static void report_status(DWORD state, DWORD win32_exit) {
    st_lock_init();
    EnterCriticalSection(&g_stLock);
    report_status_locked(state, win32_exit);
    LeaveCriticalSection(&g_stLock);
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
            /* 这里过去无条件上报 RUNNING。SERVICE_CONTROL_INTERROGATE 是
               "SCM 想知道你现在到底什么状态"，services.msc 定时会发；
               若服务正处于 STOP_PENDING 就被这句改回 RUNNING，界面显示
               与真实流程矛盾。正确做法是把当前状态原样回给 SCM。 */
            st_lock_init();
            EnterCriticalSection(&g_stLock);
            {
                DWORD cur = g_status.dwCurrentState;
                DWORD ec  = g_status.dwWin32ExitCode;
                report_status_locked(cur, ec);
            }
            LeaveCriticalSection(&g_stLock);
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

/* 等 worker 线程收尾，期间持续向 SCM 上报 STOP_PENDING 的进度点。
   返回 true = worker 已在期限内退出；false = 等到上限仍未退出。

   为什么需要这个：worker 的一轮同步最坏是 ntp_sync_best(3 次采样) ×
   timeout_ms，而 config.c 允许 timeout_ms 最大到 10000 → 单轮可阻塞
   30 秒。SCM 给 SERVICE_CONTROL_SHUTDOWN 的时限大致也是 30 秒，
   过去这里只 WaitForSingleObject(th, 5000) 就放弃，svc_main 随即上报
   STOPPED 并返回，进程 ExitProcess 把还在阻塞的 worker 硬杀——
   那一次同步的半途状态（offset 已写、时钟没改）就丢了。

   上报方式：每 1 秒把 dwCheckPoint 递增一次。SCM 见到 CheckPoint 在动
   就认为服务仍在正常收尾，不会判它无响应；这不是"额外等待许可"，
   而是让等待过程可见。 */
bool service_join_worker(HANDLE th, DWORD waitMaxMs) {
    if (!th) return true;
    DWORD elapsed = 0;
    DWORD check   = 1;
    while (elapsed < waitMaxMs) {
        DWORD w = WaitForSingleObject(th, 1000);
        if (w == WAIT_OBJECT_0) return true;
        elapsed += 1000;
        /* 只在真的还在跑时才刷新 CheckPoint，别在已退出后还画蛇添足 */
        st_lock_init();
        EnterCriticalSection(&g_stLock);
        if (g_statusHandle && g_status.dwCurrentState == SERVICE_STOP_PENDING) {
            g_status.dwCheckPoint = check++;
            g_status.dwWaitHint   = waitMaxMs - elapsed;   /* 估计剩余 */
            SetServiceStatus(g_statusHandle, &g_status);
        }
        LeaveCriticalSection(&g_stLock);
    }
    LOG_WARN("worker did not exit within %lu ms", (unsigned long)waitMaxMs);
    return false;
}

/* ---------------- install / uninstall ---------------- */

static wchar_t* widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = (wchar_t*)calloc(n, sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* 失败恢复动作：服务进程异常退出（被任务管理器结束、崩溃）后 SCM 自动重启。
   这是"学生是本地管理员"场景下最便宜的自愈手段——杀完几秒又回来。
   注意它只覆盖"异常退出"：sc stop 这类干净停止不会触发，
   那部分由 GUI 里的看门狗补上（见 window.c watchdog_maybe_start_service）。
   ChangeServiceConfig2 自 Win2000 起可用，FAILURE_ACTIONS_FLAG 自 Vista 起，
   Win7 两种都支持。
   非致命：配不上（权限不足/旧系统）只记 WARN，不影响安装本身。 */
static void service_apply_failure_actions(SC_HANDLE svc) {
    SC_ACTION acts[3];
    acts[0].Type = SC_ACTION_RESTART; acts[0].Delay = 5000;    /* 首次：5 秒 */
    acts[1].Type = SC_ACTION_RESTART; acts[1].Delay = 15000;   /* 再次：15 秒 */
    acts[2].Type = SC_ACTION_RESTART; acts[2].Delay = 30000;   /* 之后：30 秒 */
    SERVICE_FAILURE_ACTIONS sfa;
    memset(&sfa, 0, sizeof(sfa));
    sfa.dwResetPeriod = 86400;   /* 24 小时无失败则清零失败计数 */
    sfa.lpRebootMsg = NULL;
    sfa.lpCommand   = NULL;
    sfa.cActions    = 3;
    sfa.lpsaActions = acts;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa)) {
        LOG_WARN("failure actions not applied err=%lu (killed service will not auto-restart)",
                 GetLastError());
        return;
    }
    /* 非崩溃的失败（返回非零退出码）也触发恢复；不加这条只有"崩溃"才算，
       而 TerminateProcess 杀掉的进程退出码是 1，可能被漏过 */
    SERVICE_FAILURE_ACTIONS_FLAG flg;
    flg.fFailureActionsOnNonCrashFailures = TRUE;
    if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flg))
        LOG_DEBUG("failure-actions flag not applied err=%lu", GetLastError());
    else
        LOG_INFO("service failure actions configured (restart on unexpected exit)");
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
            /* 重装/升级旧服务也要补上恢复动作：老版本装的服务没有这一项 */
            service_apply_failure_actions(svc);
            CloseServiceHandle(svc);
        } else {
            CloseServiceHandle(mgr);
            LOG_ERROR("CreateService failed err=%lu", e);
            SetLastError(e);
            return false;
        }
    } else {
        service_apply_failure_actions(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(mgr);
    // 服务跑在 Session 0，桌面看不到托盘；补一个 ONLOGON 托盘伴随任务进用户会话
    bool trayOk = startup_tray_for_service(true);
    if (trayOk) LOG_INFO("tray companion task ensured for service");
    else         LOG_ERROR("tray companion task creation failed (tray icon may be missing at next logon)");
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
    return util_is_elevated();
}
