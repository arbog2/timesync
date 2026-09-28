#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <shellapi.h>

#include "util.h"
#include "config.h"
#include "service.h"
#include "task.h"
#include "window.h"
#include "ntp.h"
#include "timeset.h"
#include "startup.h"
#include "ntpserver.h"

static config_t     g_cfg;
static char         g_cfgPath[MAX_PATH] = "";
static task_state_t g_state;

static void print_usage(void) {
    printf(
        "LAN Time Sync - local network clock sync client\n"
        "\n"
        "Usage:\n"
        "  timesync                    run (tray, or service per config)\n"
        "  timesync --install-service  install and enable as Windows service\n"
        "  timesync --uninstall-service remove the service\n"
        "  timesync --service          (internal) SCM starts us this way\n"
        "  timesync --once             one sync pass, print result, exit\n"
        "  timesync --config <file>    use an explicit config file\n"
        "  timesync --help             this help\n"
        "\n"
        "Config: <ProgramData>\\LANTimeSync\\config.ini or exe-dir config.ini\n"
    );
}

/* ---------------- service worker ---------------- */

static DWORD WINAPI service_worker(LPVOID p) {
    (void)p;
    task_run(&g_cfg, &g_state);
    return 0;
}

void service_on_run(void) {
    LOG_INFO("service mode running");
    HANDLE th = CreateThread(NULL, 0, service_worker, NULL, 0, NULL);
    if (!th) {
        LOG_ERROR("CreateThread failed err=%lu", GetLastError());
        service_set_stopping();
        return;
    }
    while (!service_is_stopping())
        Sleep(200);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    LOG_INFO("service mode exiting");
}

/* ---------------- tray mode ---------------- */

static DWORD WINAPI tray_worker(LPVOID p) {
    (void)p;
    task_run(&g_cfg, &g_state);
    return 0;
}

static int run_as_tray(bool start_hidden) {
    LOG_INFO("GUI mode running (cfg=%s, hidden=%d)", g_cfgPath, start_hidden);

    HANDLE th = CreateThread(NULL, 0, tray_worker, NULL, 0, NULL);
    if (!th) { LOG_ERROR("worker create failed"); return 1; }

    char exeDir[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exeDir, sizeof(exeDir));
    char* bs = strrchr(exeDir, '\\');
    if (bs) *bs = '\0';

    HINSTANCE hInst = GetModuleHandleW(NULL);
    HWND hwnd = window_create(hInst, &g_cfg, &g_state, g_cfgPath, exeDir);
    if (!hwnd) {
        LOG_ERROR("window_create failed err=%lu", GetLastError());
        service_set_stopping();
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
        return 1;
    }
    if (start_hidden) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    int rc = window_message_loop(hwnd);

    service_set_stopping();
    WaitForSingleObject(th, 4000);
    CloseHandle(th);
    return rc;
}

/* ---------------- main ---------------- */

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 2;

    config_t cfg;
    config_init_defaults(&cfg);

    int nArgs = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    const char* mode = NULL;
    char cfgOverride[MAX_PATH] = "";
    bool delete_cfg = false; /* --temp-config: remove the --config file after load */
    bool start_hidden = false; // --tray / --startup：自启时只进托盘，不弹主窗口
    for (int i = 1; i < nArgs; i++) {
        char b[128] = "";
        /* 参数超过缓冲区时 WideCharToMultiByte 返回 0 且不写入任何内容；
           若忽略返回值，b 保持未初始化状态，后续 strcmp 会读栈上的垃圾数据。
           显式判返回值，转换失败即视作无法识别的参数。 */
        if (WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, b, sizeof(b), NULL, NULL) == 0)
            continue;
        if (strcmp(b, "--install-service") == 0) mode = "install";
        else if (strcmp(b, "--uninstall-service") == 0) mode = "uninstall";
        else if (strcmp(b, "--service") == 0)  mode = "service";
        else if (strcmp(b, "--once") == 0)     mode = "once";
        else if (strcmp(b, "--service-start") == 0) mode = "service-start";
        else if (strcmp(b, "--service-stop") == 0)  mode = "service-stop";
        else if (strcmp(b, "--tray") == 0 || strcmp(b, "--startup") == 0 || strcmp(b, "--hidden") == 0) start_hidden = true;
        else if (strcmp(b, "--ntp-enable") == 0) mode = "ntp-enable";
        else if (strcmp(b, "--ntp-disable") == 0) mode = "ntp-disable";
        else if (strcmp(b, "--config") == 0 && i + 1 < nArgs) {
            WideCharToMultiByte(CP_UTF8, 0, argvW[++i], -1,
                                cfgOverride, sizeof(cfgOverride), NULL, NULL);
        }
        else if (strcmp(b, "--temp-config") == 0) delete_cfg = true;
        else if (strcmp(b, "--help") == 0 || strcmp(b, "-h") == 0) {
            print_usage();
            LocalFree(argvW); WSACleanup(); return 0;
        }
    }
    LocalFree(argvW);

    /* 配置必须在任何模式分发之前加载：
       - 服务分支（--service）过去在 config_load 之前就 return，导致 g_cfg 全零
         （server="" port=0 timeout=0 interval=0），服务模式下永远无法同步、也不写日志；
       - --install-service / --uninstall-service 里的 LOG_* 过去同样没有日志句柄。
       这里只做一次小文件读取，耗时可忽略，不会影响 SCM 的启动超时。 */
    char cfgPath[MAX_PATH] = "";
    config_default_path(cfgPath, sizeof(cfgPath));
    if (cfgOverride[0])
        strncpy(cfgPath, cfgOverride, sizeof(cfgPath) - 1);

    bool loaded = config_load(&cfg, cfgPath);
    if (!loaded) {
        char exe[MAX_PATH] = "";
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        char* slash = strrchr(exe, '\\');
        if (slash) {
            *slash = '\0';
            char alt[MAX_PATH] = "";
            _snprintf(alt, sizeof(alt), "%s\\config.ini", exe);
            if (config_load(&cfg, alt))
                strncpy(cfgPath, alt, sizeof(cfgPath) - 1);
        }
    }
    /* temp config from the GUI runas helper: remove once loaded */
    if (delete_cfg && cfgOverride[0]) {
        WCHAR wdel[MAX_PATH] = L"";
        MultiByteToWideChar(CP_UTF8, 0, cfgOverride, -1, wdel, MAX_PATH);
        if (wdel[0]) DeleteFileW(wdel);
    }
    strncpy(g_cfgPath, cfgPath, sizeof(g_cfgPath) - 1);
    g_cfg = cfg;

    util_log_init(cfg.log_enabled ? cfg.log_file : NULL, cfg.log_level);

    /* SCM 启动的服务进程：必须在任何互斥体之前进入 dispatcher，
       否则会与 GUI 互斥导致启动失败（配置加载不涉及互斥体，放在此处是安全的） */
    if (mode && strcmp(mode, "service") == 0) {
        LOG_INFO("LAN Time Sync starting as service (server=%s:%d, cfg=%s)",
                 cfg.server, cfg.port, g_cfgPath);
        bool isService = service_run();
        WSACleanup();
        return isService ? 0 : 1;
    }

    if (mode && strcmp(mode, "install") == 0) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        if (!service_install(exe, true)) {
            fprintf(stderr, "install failed err=%lu\n", GetLastError());
            WSACleanup(); return 1;
        }
        // 安装后尝试启动，若失败仅提示（常见于未提权或杀软拦截）
        if (!service_start_svc()) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_ALREADY_RUNNING)
                fprintf(stderr, "installed but start failed err=%lu (try manually in services.msc)\n", e);
        }
        printf("LANTimeSync service installed.\n");
        WSACleanup(); return 0;
    }
    if (mode && strcmp(mode, "uninstall") == 0) {
        // 先尝试停止，若服务正在运行 DeleteService 会标记删除但不立即移除
        svc_state_t st = service_query_state();
        if (st == SVC_RUNNING || st == SVC_START_PENDING) {
            service_stop_svc();
            for (int i = 0; i < 30; i++) { Sleep(200); if (service_query_state() == SVC_STOPPED) break; }
        }
        if (!service_uninstall()) {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_MARKED_FOR_DELETE) {
                printf("service marked for delete (close services.msc and retry).\n");
                WSACleanup(); return 0;
            }
            fprintf(stderr, "uninstall failed err=%lu\n", e);
            WSACleanup(); return 1;
        }
        printf("LANTimeSync service removed.\n");
        WSACleanup(); return 0;
    }
    if (mode && strcmp(mode, "service-start") == 0) {
        bool ok = service_start_svc();
        printf("%s\n", ok ? "service started" : "start failed");
        WSACleanup(); return ok ? 0 : 1;
    }
    if (mode && strcmp(mode, "service-stop") == 0) {
        bool ok = service_stop_svc();
        printf("%s\n", ok ? "service stopped" : "stop failed");
        WSACleanup(); return ok ? 0 : 1;
    }
    if (mode && strcmp(mode, "ntp-enable") == 0) {
        bool ok = ntp_enable();
        if (!ok) { DWORD e=GetLastError(); printf("ntp enable failed err=%lu\n", e); }
        else printf("ntp enabled\n");
        WSACleanup(); return ok ? 0 : 1;
    }
    if (mode && strcmp(mode, "ntp-disable") == 0) {
        bool ok = ntp_disable();
        if (!ok) { DWORD e=GetLastError(); printf("ntp disable failed err=%lu\n", e); }
        else printf("ntp disabled\n");
        WSACleanup(); return ok ? 0 : 1;
    }
    // 提权子进程路径：ntp_enable/ntp_disable 内部会 runas 自身 --ntp-enable，
    // 实际的提权执行分支已经在上面处理；为了避免与托盘互斥，提权后直接执行 do_enable_steps
    // 的情形已在 ntpserver.c 内处理（is_elevated 分支），此处仅作为兜底


    LOG_INFO("LAN Time Sync starting (server=%s:%d, service=%s, cfg=%s)",
             cfg.server, cfg.port, cfg.service_enabled ? "yes" : "no", g_cfgPath);

    /* --once: short-lived sync helper (tray runas elevation). It must run */
    /* before the single-instance lock: the GUI holds the lock while it runs. */
    if (mode && strcmp(mode, "once") == 0) {
        bool ok = task_sync_once(&cfg, &g_state);
        printf("sync: %s  offset=%lld ms  rtt=%lld ms  error=%s\n",
               ok ? "OK" : "FAILED",
               (long long)g_state.last_offset_ms, (long long)g_state.last_rtt_ms,
               g_state.last_error[0] ? g_state.last_error : "-");
        WSACleanup();
        return ok ? 0 : 1;
    }

    HANDLE mutex = util_single_instance("Local\\LANTimeSync");
    if (!mutex) {
        fprintf(stderr, "another instance is already running.\n");
        WSACleanup(); return 1;
    }

    // 存量自启任务若为旧版（无 --tray），静默升级为托盘启动，避免下次开机弹主窗口。
    // 必须在互斥体之后：任务文件不可读时这个调用会 shell out 到 schtasks
    // （耗时数秒），放在锁前会让同时冷启动的每个实例都 fork 一遍 schtasks，
    // 而只有活下来的那一个才有资格去改自启任务。
    if (!start_hidden) startup_ensure_tray_args();

    int rc = run_as_tray(start_hidden);

    CloseHandle(mutex);
    WSACleanup();
    return rc;
}
