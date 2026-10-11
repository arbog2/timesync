#define UNICODE
#define _UNICODE
#include "startup.h"
#include "util.h"
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define STARTUP_REG_PATH   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_VALUE_NAME L"LANTimeSync"
#define TASK_NAME          L"LANTimeSync"
#define TASK_TRAY_NAME     L"LANTimeSyncTray"

// 旧版本用 HKCU\Run，自带 requireAdministrator 的程序会被 Windows 静默拦截不启动。
// 新版本改用计划任务（ONLOGON + /RL HIGHEST）实现带提权的静默自启。
// 自启任务必须带 --tray，启动时只进托盘，不弹主窗口。

// WOW64 探测与 %WINDIR%\System32(换 Sysnative) 路径拼接统一由 util 提供，
// 此前此处与 ntpserver.c 各存一份几乎相同的实现。

static void get_schtasks_path(WCHAR *out, size_t cap) { util_build_sys_bin_path(L"schtasks.exe", out, cap); }
static void get_task_file_path(WCHAR *out, size_t cap) { util_build_sys_bin_path(L"Tasks\\LANTimeSync", out, cap); }
static void get_task_tray_file_path(WCHAR *out, size_t cap) { util_build_sys_bin_path(L"Tasks\\LANTimeSyncTray", out, cap); }

static bool is_win7(void) {
    /* RtlGetVersion 返回真实版本号，不受 manifest 的 supportedOS 声明影响
       （GetVersionEx 在声明了 Win8/10 支持后会谎报 6.2+，导致 Win7 分支被跳过）。
       RtlGetVersion 自 XP 起由 ntdll 导出，Win7 上始终可用。 */
    typedef LONG (WINAPI *PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    if (h) {
        PFN_RtlGetVersion p = (PFN_RtlGetVersion)GetProcAddress(h, "RtlGetVersion");
        if (p) {
            RTL_OSVERSIONINFOW vi;
            memset(&vi, 0, sizeof(vi));
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (p(&vi) == 0)   /* STATUS_SUCCESS */
                return vi.dwMajorVersion == 6 && vi.dwMinorVersion == 1;
            return false;
        }
    }
    OSVERSIONINFOW vi; memset(&vi,0,sizeof(vi)); vi.dwOSVersionInfoSize=sizeof(vi);
#pragma warning(push)
#pragma warning(disable:4996)
    if (!GetVersionExW(&vi)) return false;
#pragma warning(pop)
    return vi.dwMajorVersion==6 && vi.dwMinorVersion==1;
}

static bool reg_is_enabled(void) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_PATH, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    WCHAR val[MAX_PATH * 2] = L"";
    DWORD cb = sizeof(val);
    DWORD type = 0;
    LONG r = RegQueryValueExW(hk, STARTUP_VALUE_NAME, NULL, &type, (BYTE*)val, &cb);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return false;
    if (type != REG_SZ) return false;
    return val[0] != L'\0';
}

static void reg_remove(void) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_PATH, 0, KEY_WRITE, &hk) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(hk, STARTUP_VALUE_NAME);
    RegCloseKey(hk);
}
static bool reg_create_fallback(void) {
    // 仅作为计划任务创建失败时的退化方案（Win7 受限环境）
    WCHAR exe[MAX_PATH]=L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!exe[0]) return false;
    WCHAR cmd[MAX_PATH*2+16]=L"";
    _snwprintf(cmd, MAX_PATH*2+15, L"\"%s\" --tray", exe);
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, STARTUP_REG_PATH, 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL)!=ERROR_SUCCESS) return false;
    LONG r=RegSetValueExW(hk, STARTUP_VALUE_NAME, 0, REG_SZ, (BYTE*)cmd, (DWORD)((wcslen(cmd)+1)*sizeof(WCHAR)));
    RegCloseKey(hk);
    return r==ERROR_SUCCESS;
}

static bool is_elevated(void) {
    return util_is_elevated();
}

/* schtasks 的一次性操作超时。开机路径必须短（否则拖住登录），
   而在安装/卸载这类"用户在等"的路径上可以放宽——一个 /Create
   在 Task Scheduler 忙时等十几秒是正常的。超时仍然会杀掉子进程
   （不能无限挂住），但至少留出余地，避免把一个半途执行完的 schtasks
   从中间砍断。 */
#define SCHTASKS_TIMEOUT_BOOT  8000
#define SCHTASKS_TIMEOUT_USER  30000

static DWORD run_hidden_and_wait(WCHAR *cmdline, DWORD timeoutMs) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0xFFFFFFFFu;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 0xFFFFFFFFu;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}
static bool task_exists_fast(void) {
    // 使用 Sysnative 感知的路径，避免 32 位在 64 位 Win7 上被重定向到 SysWOW64
    WCHAR taskFile[MAX_PATH]=L"";
    get_task_file_path(taskFile, MAX_PATH);
    if (GetFileAttributesW(taskFile)!=INVALID_FILE_ATTRIBUTES) return true;

    DWORD err = GetLastError();
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
        /* 不是"不存在"而是读不到（权限不足等）：只能老老实实问 schtasks。
           这一步会拉起一次进程，最多等 4 秒，所以调用方（UI 定时器）
           必须带缓存，不能每秒调。 */
        WCHAR sch[MAX_PATH]=L""; get_schtasks_path(sch, MAX_PATH);
        WCHAR cmd[1024];
        _snwprintf(cmd, 1024, L"\"%s\" /Query /TN \"%s\"", sch, TASK_NAME);
        cmd[1023]=L'\0';
        WCHAR mutableCmd[1024]; wcscpy(mutableCmd, cmd);
        return run_hidden_and_wait(mutableCmd, 4000)==0;
    }

    // 任务文件确实不存在：再查一次硬编码 System32 下是否有旧任务，
    // 兼容 32/64 交叉（Sysnative 探针在纯 32 位系统上会偏）
    WCHAR alt[MAX_PATH]=L"";
    WCHAR win[MAX_PATH]=L""; util_get_windows_dir(win, MAX_PATH);
    _snwprintf(alt, MAX_PATH, L"%s\\System32\\Tasks\\LANTimeSync", win);
    alt[MAX_PATH-1]=L'\0';
    if (wcscmp(alt, taskFile)!=0 && GetFileAttributesW(alt)!=INVALID_FILE_ATTRIBUTES)
        return true;
    /* 注：这里不查注册表回退。startup_is_enabled() 已经单独查了
       reg_is_enabled()，而本函数只回答"计划任务是否存在"，
       把两件事混在一起只会让调用方的判断变得不可预测。 */
    return false;
}

static bool run_schtasks_elevated_if_needed(const WCHAR *params, DWORD timeoutMs) {
    if (is_elevated()) {
        WCHAR sch[MAX_PATH]=L""; get_schtasks_path(sch, MAX_PATH);
        WCHAR cmd[2048];
        _snwprintf(cmd, 2048, L"\"%s\" %s", sch, params);
        cmd[2047]=L'\0';
        WCHAR mutableCmd[2048]; wcscpy(mutableCmd, cmd);
        DWORD code = run_hidden_and_wait(mutableCmd, timeoutMs);
        if (code==0) return true;
        /* schtasks 退出码 1 通常是"任务不存在"（删除场景），
           由调用方按需判定；这里把超时/被杀区分出来，便于现场排查 */
        if (code == 0xFFFFFFFFu)
            LOG_WARN("schtasks timed out or failed to start (params=%ls)", params);
        return false;
    }
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    WCHAR sch[MAX_PATH]=L""; get_schtasks_path(sch, MAX_PATH);
    sei.lpFile = sch;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (!sei.hProcess) return true;
    DWORD wait = WaitForSingleObject(sei.hProcess, timeoutMs);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &code);
    else LOG_WARN("schtasks (runas) timed out; killed");
    CloseHandle(sei.hProcess);
    return code == 0;
}

static bool task_create_internal(const WCHAR *tname, const WCHAR *exe, bool with_highest) {
    WCHAR params[2048];
    // Win7 对 /TR 的引号更挑剔：保持 "\"exe\" --tray" 形式，Win10+ 同样兼容
    // 托盘任务无需 /RL HIGHEST，可减少 Win7 上 UAC 触发；主任务仍需 HIGHEST 以便 SetSystemTime
    if (with_highest) {
        // Win7 上 schtasks /RL HIGHEST 需配合 /SC ONLOGON，/IT 在 Win10+ 上与 HIGHEST 互斥，故仅 Win7 加 /IT
        if (is_win7()) {
            _snwprintf(params, 2048, L"/Create /TN \"%s\" /TR \"\\\"%s\\\" --tray\" /SC ONLOGON /IT /RL HIGHEST /F", tname, exe);
        } else {
            _snwprintf(params, 2048, L"/Create /TN \"%s\" /TR \"\\\"%s\\\" --tray\" /SC ONLOGON /RL HIGHEST /F", tname, exe);
        }
    } else {
        /* 托盘伴随任务不带 HIGHEST，因此 /IT 在所有系统上都合法（/IT 只与
           HIGHEST 在 Win10+ 上互斥）。显式 /IT 保证任务以交互式进桌面，
           避免在 Win7 上被配成非交互而跑进看不见的会话（有进程、无托盘、
           无法唤起）。与主任务的 Win7 分支保持一致。 */
        _snwprintf(params, 2048, L"/Create /TN \"%s\" /TR \"\\\"%s\\\" --tray\" /SC ONLOGON /IT /F", tname, exe);
    }
    params[2047]=L'\0';
    /* 创建/删除任务都放 30 秒：这是用户点了按钮在等的一次性操作，
       开机路径不走这里（见 startup_ensure_tray_args，用 8 秒档）。 */
    return run_schtasks_elevated_if_needed(params, SCHTASKS_TIMEOUT_USER);
}

static bool task_create(void) {
    WCHAR exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!exe[0]) return false;
    if (task_create_internal(TASK_NAME, exe, true)) {
        reg_remove();
        return true;
    }
    // 计划任务创建失败（常见于 Win7 受限或 schtasks 版本差异），退化到注册表自启
    // 虽会被 UAC 拦截，但总比完全无法自启好，且不会报假成功
    if (reg_create_fallback()) return true;
    return false;
}

static bool task_delete_one(const WCHAR *tname) {
    WCHAR params[512];
    _snwprintf(params, 512, L"/Delete /TN \"%s\" /F", tname);
    params[511]=L'\0';
    bool ok = run_schtasks_elevated_if_needed(params, SCHTASKS_TIMEOUT_USER);
    if (ok) return true;
    // schtasks 返回 1 通常为不存在，视为已清理
    WCHAR tf[MAX_PATH]=L"";
    if (wcscmp(tname, TASK_NAME)==0) get_task_file_path(tf, MAX_PATH);
    else get_task_tray_file_path(tf, MAX_PATH);
    if (GetFileAttributesW(tf)==INVALID_FILE_ATTRIBUTES && (GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)) return true;
    return false;
}
static bool task_delete(void) {
    task_delete_one(TASK_NAME);
    // 兼容旧版：主任务删不掉也照样删托盘任务，至少保证 check 能回落
    task_delete_one(TASK_TRAY_NAME);
    // 无论如何都清理注册表回退
    reg_remove();
    // 判据是"自启是否真的没了"，而不是"schtasks 是否逐个返回 0"：
    // schtasks 删一个不存在的任务会返回 1，按删除返回值判断会把
    // 已经清干净的状态误报成失败。托盘任务残留也不影响自启主流程。
    return !startup_is_enabled();
}

// 为服务模式补一个常驻托盘任务：服务本身跑在 Session 0，桌面永远看不到图标，
// 另起一个不带 HIGHEST 的登录触发托盘任务，仅做 UI 展示。检测与切换都在 GUI 内。
static bool tray_task_exists_fast(void) {
    WCHAR tf[MAX_PATH]=L""; get_task_tray_file_path(tf, MAX_PATH);
    if (GetFileAttributesW(tf)!=INVALID_FILE_ATTRIBUTES) return true;
    if (GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND) return false;
    WCHAR sch[MAX_PATH]=L""; get_schtasks_path(sch, MAX_PATH);
    WCHAR cmd[1024]; _snwprintf(cmd,1024,L"\"%s\" /Query /TN \"%s\"", sch, TASK_TRAY_NAME);
    cmd[1023]=L'\0';
    WCHAR m[1024]; wcscpy(m,cmd);
    return run_hidden_and_wait(m, 4000)==0;
}
static bool tray_task_ensure(void) {
    /* 不做 exists-fast 短路：每次安装/重装服务都用 /F 覆写一次，
       顺带把旧版建出的无 /IT 托盘任务迁移为交互式。service_install
       本身已在提权上下文中，覆写不会引入额外的 UAC。 */
    WCHAR exe[MAX_PATH]=L""; GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!exe[0]) return false;
    // 托盘任务不带 HIGHEST，避免 Win7 上重复提权，且与服务校时不冲突
    return task_create_internal(TASK_TRAY_NAME, exe, false);
}
static void tray_task_remove(void) { task_delete_one(TASK_TRAY_NAME); }

/* 托盘伴随任务是否已存在。只读任务文件属性、不打 schtasks，调用成本接近
   一次系统调用，所以能放在看门狗每轮的循环里。 */
bool startup_tray_task_exists(void) {
    return tray_task_exists_fast();
}

// 任务文件为 UTF-16 XML，直接在字节流里搜索 "--tray/--startup/--hidden" 的
// ASCII 与 UTF-16LE 两种编码，避免引入 XML 解析。
static bool task_has_tray_flag_file(const WCHAR *taskFile) {
    HANDLE h = CreateFileW(taskFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return true; // 读不到就不折腾，避免误覆写
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 256 * 1024) { CloseHandle(h); return true; }
    BYTE *buf = (BYTE*)malloc(sz);
    if (!buf) { CloseHandle(h); return true; }
    DWORD rd = 0;
    if (!ReadFile(h, buf, sz, &rd, NULL) || rd != sz) { free(buf); CloseHandle(h); return true; }
    CloseHandle(h);
    const char *needlesA[] = { "--tray", "--startup", "--hidden" };
    for (int k = 0; k < 3; k++) {
        const char *nd = needlesA[k];
        size_t nl = strlen(nd);
        for (DWORD i = 0; i + nl <= rd; i++) {
            if (_strnicmp((char*)buf + i, nd, nl) == 0) { free(buf); return true; }
        }
    }
    const WCHAR *needlesW[] = { L"--tray", L"--startup", L"--hidden" };
    for (int k = 0; k < 3; k++) {
        const WCHAR *nw = needlesW[k];
        size_t wl = wcslen(nw);
        size_t bl = wl * 2;
        for (DWORD i = 0; i + bl <= rd; i++) {
            bool match = true;
            for (size_t j = 0; j < wl; j++) {
                if (buf[i + j*2] != (BYTE)nw[j] || buf[i + j*2 + 1] != 0) { match = false; break; }
            }
            if (match) { free(buf); return true; }
            bool matchLow = true;
            for (size_t j = 0; j < wl; j++) {
                WCHAR wc = nw[j];
                if (wc >= L'A' && wc <= L'Z') wc = wc - L'A' + L'a';
                BYTE lo = (BYTE)wc, hi = 0;
                BYTE b0 = buf[i + j*2], b1 = buf[i + j*2+1];
                if (b1 != 0) { matchLow = false; break; }
                if (b0 >= 'A' && b0 <= 'Z') b0 = b0 - 'A' + 'a';
                if (b0 != lo || b1 != hi) { matchLow = false; break; }
            }
            if (matchLow) { free(buf); return true; }
        }
    }
    free(buf);
    return false;
}

/* 一次性迁移标记：旧任务补上 --tray 后就不再试第二次。
   标准用户的机器上 task_create() 最终只能落到 HKCU\Run 回退，任务文件
   依然没有 --tray；没有这个标记的话，每次登录都会走到 schtasks 的
   runas 分支弹一次 UAC——开机弹窗没修掉，反而多弹一个授权框。
   写在 HKCU：自启任务本身就是按用户建的，跟随用户身份正合适。 */
#define MIGRATE_KEY  L"Software\\LANTimeSync"
#define MIGRATE_VAL  L"TaskAutoStartMigrated"

static bool startup_migration_done(void) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, MIGRATE_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    DWORD v = 0, cb = sizeof(v), type = 0;
    LONG r = RegQueryValueExW(hk, MIGRATE_VAL, NULL, &type, (BYTE*)&v, &cb);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS && type == REG_DWORD && v == 1;
}

static void startup_mark_migrated(void) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, MIGRATE_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS)
        return;
    DWORD v = 1;
    RegSetValueExW(hk, MIGRATE_VAL, 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    RegCloseKey(hk);
}

void startup_ensure_tray_args(void) {
    WCHAR tf[MAX_PATH]=L""; get_task_file_path(tf, MAX_PATH);
    if (GetFileAttributesW(tf)==INVALID_FILE_ATTRIBUTES) {
        // 主任务读不到：不猜。旧实现把"读不到"当成"已带 --tray"，
        // 于是从读不到任务的旧版升级上来的机器，开机弹主窗口的问题
        // 在新版上依然存在，而且没有任何日志能说明原因。
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            LOG_DEBUG("startup task file unreadable; skip migration probe");
        return;
    }
    if (task_has_tray_flag_file(tf)) {
        // 已经是新格式：顺手给托盘伴随任务也补一次（它可能还是旧的无 --tray 版本）
        WCHAR trayFile[MAX_PATH]=L""; get_task_tray_file_path(trayFile, MAX_PATH);
        if (GetFileAttributesW(trayFile)!=INVALID_FILE_ATTRIBUTES
            && !task_has_tray_flag_file(trayFile) && !startup_migration_done()) {
            WCHAR exe[MAX_PATH]=L""; GetModuleFileNameW(NULL, exe, MAX_PATH);
            if (exe[0]) task_create_internal(TASK_TRAY_NAME, exe, false);
        }
        return;
    }
    if (startup_migration_done()) return;   // 试过了，别再弹 UAC
    LOG_INFO("legacy startup task lacks --tray; migrating");
    if (task_create()) {
        startup_mark_migrated();
        LOG_INFO("startup task migrated to --tray");
    } else {
        /* 迁移失败（最常见：标准用户无法提权创建 ONLOGON/HIGHEST 任务）。
           仍然打标，避免每次登录重复尝试；task_create 内部会退化写
           HKCU\Run，自启本身还是能生效，只是没有提权。 */
        startup_mark_migrated();
        LOG_WARN("startup task migration failed; registry fallback used, will not retry this logon");
    }
}

bool startup_is_enabled(void) {
    if (task_exists_fast()) return true;
    WCHAR trayFile[MAX_PATH]=L""; get_task_tray_file_path(trayFile, MAX_PATH);
    if (GetFileAttributesW(trayFile)!=INVALID_FILE_ATTRIBUTES) return true;
    // 兼容 Win7 上被重定向的 System32 路径残留
    {
        WCHAR win[MAX_PATH]=L""; util_get_windows_dir(win, MAX_PATH);
        WCHAR alt[MAX_PATH]=L""; _snwprintf(alt, MAX_PATH, L"%s\\System32\\Tasks\\LANTimeSync", win);
        alt[MAX_PATH-1]=L'\0';
        WCHAR cur[MAX_PATH]=L""; get_task_file_path(cur, MAX_PATH);
        if (wcscmp(alt,cur)!=0 && GetFileAttributesW(alt)!=INVALID_FILE_ATTRIBUTES) return true;
        WCHAR alt2[MAX_PATH]=L""; _snwprintf(alt2, MAX_PATH, L"%s\\System32\\Tasks\\LANTimeSyncTray", win);
        alt2[MAX_PATH-1]=L'\0';
        WCHAR cur2[MAX_PATH]=L""; get_task_tray_file_path(cur2, MAX_PATH);
        if (wcscmp(alt2,cur2)!=0 && GetFileAttributesW(alt2)!=INVALID_FILE_ATTRIBUTES) return true;
    }
    return reg_is_enabled();
}

bool startup_set(bool enable) {
    if (enable) {
        return task_create();
    }
    task_delete();
    // 同时清理服务托盘任务与注册表回退
    tray_task_remove();
    reg_remove();
    // task_delete 内部已按"自启是否真的没了"判定过一次，这里以复查结果为准，
    // 避免托盘任务/注册表残留被报成删除失败。
    return !startup_is_enabled();
}

// 供 service.c 在安装/卸载时调用，保持服务与托盘任务的生命周期一致
bool startup_tray_for_service(bool enable) {
    if (enable) return tray_task_ensure();
    tray_task_remove();
    return true;
}
