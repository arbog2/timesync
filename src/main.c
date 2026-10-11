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
#include "watermark.h"
#include "cguard.h"
#include "trayguard.h"

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
        "  timesync --result <file>    (with --once) write the result to <file>\n"
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
    /* 服务同样要防配置被篡改：Session 0 里的服务才是真正持续校时的那一个 */
    cguard_init(g_cfgPath, &g_cfg);
    /* 反向看门狗：托盘进程（用户会话里那个）被杀时把它拉回来。
       服务自己的失败恢复动作只保服务自己，这一层补上另一半。 */
    trayguard_start();
    HANDLE th = CreateThread(NULL, 0, service_worker, NULL, 0, NULL);
    if (!th) {
        LOG_ERROR("CreateThread failed err=%lu", GetLastError());
        service_set_stopping();
        return;
    }
    while (!service_is_stopping())
        Sleep(200);
    /* 一次同步最坏阻塞 3 × 10000ms = 30s，给足 90s 上限（含系统繁忙抖动）。
     等待期间 service_join_worker 会持续上报 STOP_PENDING 的 CheckPoint，
     让 SCM 知道我们仍在收尾而不是卡死。 */
    bool exited = service_join_worker(th, 90000);
    if (!exited)
        LOG_ERROR("worker thread still running at stop deadline; exiting anyway");
    CloseHandle(th);
    /* 服务要停了：看门狗一起收掉，否则它会在服务停止后继续重启托盘 */
    trayguard_stop();
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

    /* 配置守卫要先于 worker 起来：worker 的 cguard_tick 依赖这里的基线，
       而"同步成功后固化 lastgood"要求此刻已有 lastgood 目录信息。 */
    cguard_init(g_cfgPath, &g_cfg);

    HANDLE th = CreateThread(NULL, 0, tray_worker, NULL, 0, NULL);
    if (!th) { LOG_ERROR("worker create failed"); return 1; }

    /* 服务已在运行时，GUI 的周期校时全部交给服务，避免两个进程同时
       SetSystemTime（非管理员身份的那个还会每次失败并在日志里刷 err=5）。
       手动"立即同步"仍然可用——worker 线程照常起，只关掉定时那部分。 */
    if (service_query_state() == SVC_RUNNING) {
        task_set_periodic(false);
        LOG_INFO("service is running; GUI periodic sync disabled, on-demand sync stays");
    }
    /* 托盘正常起来了：清掉上次"自愿退出"的标记，让看门狗恢复看护。
       顺序很重要——必须在任何可能被杀之前清，否则一次自愿退出会让
       看门狗从此再也不管。 */
    trayguard_clear_quit_mark();

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

    /* 桌面水印：独立于主窗口的 topmost 点击穿透窗口，所以主窗口隐藏到
       托盘后它依然在。放在主窗口之后创建——窗口都没有的情况下，先摆一个
       水印没有意义。
       服务模式（Session 0）不走到这里，水印由登录时的托盘伴随进程负责。 */
    watermark_apply(hInst, &g_cfg);

    int rc = window_message_loop(hwnd);

    watermark_destroy();
    service_set_stopping();
    /* 窗口已销毁，这里是进程退出前的收尾。worker 通常会在 1 秒内退出
       （循环里每秒检查一次停止标志）；只有恰好卡在一轮同步里才会更久，
       那种情况最多等 15 秒——再久就只能放弃，不能把退出无限期拖住。 */
    DWORD w = WaitForSingleObject(th, 15000);
    if (w != WAIT_OBJECT_0)
        LOG_WARN("worker still running at tray exit deadline; exiting anyway");
    CloseHandle(th);
    return rc;
}

/* ---------------- main ---------------- */

/* ---------------- --once result hand-off ----------------
   --once 以 /SUBSYSTEM:WINDOWS 链接，没有控制台，printf 的结果在 GUI
   拉起的提权子进程里没有任何去处。改由子进程把结果落到 --result 指定
   的文件，父进程（window.c）轮询取回并显示。

   先写 <path>.tmp 再 MoveFileEx 原子改名，与 config_extra.c 的写配置
   同一套做法：父进程只可能看到完整文件，不会在子进程 fopen 已建、
   内容未落完的窗口期读到半截数据。 */
static void write_result_file(const char* path, const task_state_t* st) {
    if (!path || !path[0]) return;
    WCHAR wpath[MAX_PATH] = L"", wtmp[MAX_PATH + 8] = L"";
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH) == 0) return;
    if (_snwprintf(wtmp, MAX_PATH + 7, L"%s.tmp", wpath) < 0) return;
    wtmp[MAX_PATH + 7] = L'\0';

    FILE* f = NULL;
    _wfopen_s(&f, wtmp, L"w");   /* 字节模式：UTF-8 原样写出，不加 BOM */
    if (!f) return;
    fprintf(f, "ok=%d\n",   st->last_success ? 1 : 0);
    fprintf(f, "offset=%lld\n", (long long)st->last_offset_ms);
    fprintf(f, "rtt=%lld\n",    (long long)st->last_rtt_ms);
    fprintf(f, "error=%s\n",    st->last_error[0] ? st->last_error : "-");
    fclose(f);
    if (!MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(wtmp);
}

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
    char resultOverride[MAX_PATH] = "";
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
            /* WideCharToMultiByte 空间不足时返回 0 且一个字节都不写。
               忽略返回值会让超过 MAX_PATH 的 --config 路径静默变成空串，
               子进程随后回落去读默认配置——用户在文本框里改的服务器地址
               就这么丢了，而且没有任何报错。显式判失败并留空。 */
            if (WideCharToMultiByte(CP_UTF8, 0, argvW[++i], -1,
                                    cfgOverride, sizeof(cfgOverride) - 1, NULL, NULL) == 0)
                cfgOverride[0] = '\0';
        }
        else if (strcmp(b, "--result") == 0 && i + 1 < nArgs) {
            if (WideCharToMultiByte(CP_UTF8, 0, argvW[++i], -1,
                                    resultOverride, sizeof(resultOverride) - 1, NULL, NULL) == 0)
                resultOverride[0] = '\0';
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

    /* 在任何写操作之前把 %ProgramData%\LANTimeSync 建好并放开 ACL。
       日志默认就走这个目录，而开机时服务（SYSTEM）或 /RL HIGHEST 任务
       可能已经先把日志建好了——那时属主是 SYSTEM，之后登录的标准用户
       既写不了日志也存不了配置，且失败完全静默（不弹窗、日志里也看不出来）。
       必须在 util_log_init 之前，否则日志第一次打开就已经失败。
       不特判 --config：便携安装（配置在 exe 目录）的日志同样默认在 ProgramData，
       而这个调用是幂等的，代价只是一次 SetNamedSecurityInfo。 */
    util_ensure_program_dir();

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
        if (MultiByteToWideChar(CP_UTF8, 0, cfgOverride, -1, wdel, MAX_PATH - 1) > 0)
            DeleteFileW(wdel);
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
        /* 错误码要在任何日志之前取：LOG_ERROR 内部会调 GetLocalTime /
           EnterCriticalSection 等，可能把 last-error 覆盖掉。 */
        DWORD err = ok ? 0 : GetLastError();
        if (!ok) LOG_ERROR("ntp enable failed err=%lu", err);
        else LOG_INFO("ntp enabled");
        /* 错误码必须原样带出去：父进程是靠退出码还原失败原因的。
           这里过去一律 return 1，于是 window.c 里为 1058(服务被禁用)/
           1053(启动超时)/5(拒绝访问)写的那几条针对性提示永远命中不了，
           用户只看到"开启失败，错误码 1"。 */
        WSACleanup(); return ok ? 0 : (int)(err & 0x7FFFFFFFu);
    }
    if (mode && strcmp(mode, "ntp-disable") == 0) {
        bool ok = ntp_disable();
        DWORD err = ok ? 0 : GetLastError();
        if (!ok) LOG_ERROR("ntp disable failed err=%lu", err);
        else LOG_INFO("ntp disabled");
        WSACleanup(); return ok ? 0 : (int)(err & 0x7FFFFFFFu);
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
        write_result_file(resultOverride, &g_state);
        WSACleanup();
        return ok ? 0 : 1;
    }

    HANDLE mutex = util_single_instance("Local\\LANTimeSync");
    if (!mutex) {
        /* 自启拉起的第二实例（--tray）只静默退出：开机时主任务与托盘伴随
           任务可能同时触发，后到的那个若去唤起窗口，会把本该隐藏的主窗口
           在登录时弹出来。只有用户手工双击（非隐藏启动）才走下面的唤起。 */
        if (start_hidden) { WSACleanup(); return 0; }
        /* 单实例已在运行：把它的主窗口带到前台，而不是静默退出。
           程序以 /SUBSYSTEM:WINDOWS 链接，没有控制台，过去这里的
           fprintf 用户根本看不到——双击毫无反应，还以为程序坏了。
           跨完整性（中 -> 高）PostMessage 会被 UIPI 拦截，好在
           window.c 里已对该消息 ChangeWindowMessageFilter 放行。 */
        HWND hExist = FindWindowW(L"LANTimeSyncMainWnd", NULL);
        if (hExist) {
            UINT msgShow = RegisterWindowMessageW(L"LANTimeSync_ShowWindow");
            if (msgShow) PostMessageW(hExist, msgShow, 0, 0);
            /* 已投递唤起消息即算成功：返回 0 */
            WSACleanup(); return 0;
        }
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
