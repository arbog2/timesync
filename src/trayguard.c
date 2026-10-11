#define UNICODE
#define _UNICODE
#include "trayguard.h"
#include "util.h"
#include "startup.h"
#include <windows.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#define TG_CHECK_MS      15000    /* 检查间隔 */
#define TG_MISS_LIMIT    3        /* 连续 3 次看不到托盘才动手（防抖） */
#define TG_TASKCHECK_MS  600000   /* 托盘计划任务重建的最短间隔 */

/* 自愿退出标记：写在 ProgramData 下，托盘进程（普通用户）可写、
   服务（SYSTEM）可读。用文件而不是注册表，是因为服务读不到用户的 HKCU，
   而写 HKLM 托盘又没权限。内容是一个 unix 时间戳，日志里能看出用户何时退出。 */
static WCHAR s_quitMark[MAX_PATH] = L"";

static HANDLE s_thread = NULL;
static DWORD  s_threadId = 0;
static volatile LONG s_stopping = 0;

/* ------------------------------------------------------------------ */
/* 自愿退出标记                                                       */
/* ------------------------------------------------------------------ */

static void build_quit_mark_path(void) {
    char prog[MAX_PATH] = "";
    if (!GetEnvironmentVariableA("ProgramData", prog, sizeof(prog)))
        _snprintf(prog, sizeof(prog), "%s", "C:\\ProgramData");
    char p[MAX_PATH] = "";
    _snprintf(p, sizeof(p), "%s\\LANTimeSync\\tray.quit", prog);
    p[sizeof(p) - 1] = '\0';
    MultiByteToWideChar(CP_UTF8, 0, p, -1, s_quitMark, MAX_PATH);
    s_quitMark[MAX_PATH - 1] = L'\0';
}

static bool quit_mark_exists(void) {
    if (!s_quitMark[0]) build_quit_mark_path();
    if (!s_quitMark[0]) return false;
    return GetFileAttributesW(s_quitMark) != INVALID_FILE_ATTRIBUTES;
}

void trayguard_note_user_quit(void) {
    if (!s_quitMark[0]) build_quit_mark_path();
    if (!s_quitMark[0]) return;

    WCHAR tmp[MAX_PATH + 8] = L"";
    _snwprintf(tmp, MAX_PATH + 7, L"%s.tmp", s_quitMark);
    tmp[MAX_PATH + 7] = L'\0';

    FILE* f = NULL;
    if (_wfopen_s(&f, tmp, L"w") == 0 && f) {
        fwprintf(f, L"%lld\n", (long long)time(NULL));
        fclose(f);
        if (MoveFileExW(tmp, s_quitMark, MOVEFILE_REPLACE_EXISTING)) {
            LOG_INFO("tray quit mark written (watchdog will not relaunch tray)");
            return;
        }
    }
    /* 写不了也不能阻止退出：只是看门狗之后还可能会把它拉回来 */
    LOG_DEBUG("tray quit mark write failed: %ls", s_quitMark);
}

void trayguard_clear_quit_mark(void) {
    if (!s_quitMark[0]) build_quit_mark_path();
    if (s_quitMark[0]) DeleteFileW(s_quitMark);
}

/* ------------------------------------------------------------------ */
/* 托盘进程探测                                                       */
/* ------------------------------------------------------------------ */

/* 有没有"另一个" timesync.exe 在跑。
   服务自己也是这个可执行文件名，所以排除自身 PID。
   判据刻意从简：只看同名的其它进程在不在，不解析命令行。
   --once 这类短命提权进程也会被算成"在跑"，后果只是"多等几轮才拉起"，
   换来的是不需要跨进程读 PEB / 不起 WMI。 */
static bool other_timesync_running(void) {
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;   /* 枚举失败：保守当作还在 */

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) continue;
            if (lstrcmpiW(pe.szExeFile, L"timesync.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

/* ------------------------------------------------------------------ */
/* 拉起托盘                                                            */
/* ------------------------------------------------------------------ */

/* 在当前活动控制台会话里，以该会话登录用户的身份启动 timesync.exe --tray。
   返回 true 表示进程已创建。
   要点：
     - 用 WTSQueryUserToken 拿用户 Token，进程才会跑在用户会话而不是 Session 0
     - si.lpDesktop 必须是 winsta0\default，否则服务进程默认在 Service-0
       窗口站，起的进程直接失败
     - 环境块用 CreateEnvironmentBlock：服务自身的环境是 SYSTEM 的，
       PATH 之类给用户进程用会出错 */
static bool launch_tray_as_user(void) {
    DWORD sid_active = WTSGetActiveConsoleSessionId();
    if (sid_active == 0xFFFFFFFFu) {
        LOG_DEBUG("watchdog: no active console session");
        return false;
    }

    HANDLE hUser = NULL;
    if (!WTSQueryUserToken(sid_active, &hUser) || !hUser) {
        /* 没人登录（登录界面/锁屏）时这里会失败：不理它，
           ONLOGON 计划任务会在用户登录时把托盘拉起来。 */
        LOG_DEBUG("watchdog: no user token for session %lu (nobody logged in?)",
                  (unsigned long)sid_active);
        return false;
    }

    WCHAR exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR cmdline[MAX_PATH + 16] = L"";
    _snwprintf(cmdline, MAX_PATH + 15, L"\"%s\" --tray", exe);
    cmdline[MAX_PATH + 15] = L'\0';

    LPVOID env = NULL;
    CreateEnvironmentBlock(&env, hUser, FALSE);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"winsta0\\default";
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    BOOL ok = CreateProcessAsUserW(hUser, exe, cmdline, NULL, NULL, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP,
                                   env, NULL, &si, &pi);
    DWORD err = GetLastError();
    if (env) DestroyEnvironmentBlock(env);
    if (ok) {
        LOG_INFO("watchdog: tray relaunched as session %lu user (pid %lu)",
                 (unsigned long)sid_active, (unsigned long)pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LOG_WARN("watchdog: CreateProcessAsUser failed err=%lu", err);
    }
    CloseHandle(hUser);
    return ok != FALSE;
}

/* ------------------------------------------------------------------ */
/* 看门狗线程                                                         */
/* ------------------------------------------------------------------ */

static void watchdog_ensure_tray_task(DWORD* lastTaskFixTick) {
    /* 托盘计划任务被删：服务侧直接 CreateProcessAsUser 也能把托盘拉回来，
       但那样下次重启就没有 ONLOGON 任务了，登录后托盘不再自动出现。
       所以这里顺手把任务补回来。只有真缺了才动 schtasks（要等最多 8 秒），
       且至少 10 分钟才重试一次，避免反复失败时刷日志。 */
    if (startup_tray_task_exists()) return;
    DWORD now = GetTickCount();
    if (now - *lastTaskFixTick < TG_TASKCHECK_MS) return;
    *lastTaskFixTick = now;

    LOG_WARN("watchdog: tray startup task missing, recreating");
    if (startup_tray_for_service(true))
        LOG_INFO("watchdog: tray startup task recreated");
    else
        LOG_WARN("watchdog: tray startup task recreate failed err=%lu", GetLastError());
}

static DWORD WINAPI watchdog_thread(LPVOID param) {
    (void)param;
    int miss = 0;
    DWORD lastTaskFix = 0;

    while (!s_stopping) {
        Sleep(TG_CHECK_MS);
        if (s_stopping) break;

        watchdog_ensure_tray_task(&lastTaskFix);

        if (quit_mark_exists()) {
            /* 用户亲手退出的：尊重意愿，不拉 */
            miss = 0;
            continue;
        }

        if (other_timesync_running()) { miss = 0; continue; }

        if (++miss < TG_MISS_LIMIT) continue;
        miss = 0;   /* 无论成不成功都重新计数，失败时下一轮再来 */

        LOG_INFO("watchdog: tray process gone, relaunching");
        if (!launch_tray_as_user())
            LOG_DEBUG("watchdog: relaunch deferred this round");
    }
    return 0;
}

void trayguard_start(void) {
    if (s_thread) return;
    if (!s_quitMark[0]) build_quit_mark_path();
    InterlockedExchange(&s_stopping, 0);
    s_thread = CreateThread(NULL, 0, watchdog_thread, NULL, 0, &s_threadId);
    if (s_thread)
        LOG_INFO("tray watchdog started (interval %dms)", (int)TG_CHECK_MS);
    else
        LOG_WARN("tray watchdog failed to start err=%lu", GetLastError());
}

void trayguard_stop(void) {
    if (!s_thread) return;
    InterlockedExchange(&s_stopping, 1);
    /* 线程睡眠中不会及时醒来，最多等一个间隔 + 余量 */
    WaitForSingleObject(s_thread, TG_CHECK_MS + 3000);
    CloseHandle(s_thread);
    s_thread = NULL;
    LOG_INFO("tray watchdog stopped");
}
