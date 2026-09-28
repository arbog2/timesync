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
    DWORD attr = GetFileAttributesW(taskFile);
    if (attr != INVALID_FILE_ATTRIBUTES) return true;
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        // 再查一次 Sysnative/Tasks 目录下是否有旧任务，兼容 32/64 交叉
        // 若探针路径不同，再试一次硬编码 System32，避免 Sysnative 探针误判
        WCHAR alt[MAX_PATH]=L"";
        WCHAR win[MAX_PATH]=L""; util_get_windows_dir(win, MAX_PATH);
        _snwprintf(alt, MAX_PATH, L"%s\\System32\\Tasks\\LANTimeSync", win);
        alt[MAX_PATH-1]=L'\0';
        if (wcscmp(alt, taskFile)!=0) {
            if (GetFileAttributesW(alt)!=INVALID_FILE_ATTRIBUTES) return true;
        }
        // 仍未找到：若注册表回退存在，也视为已启用，便于迁移
        // 由外层统一处理
    } else {
        // 无法判断（如权限不足）：回退到 schtasks 查询
        WCHAR sch[MAX_PATH]=L""; get_schtasks_path(sch, MAX_PATH);
        WCHAR cmd[1024];
        _snwprintf(cmd, 1024, L"\"%s\" /Query /TN \"%s\"", sch, TASK_NAME);
        cmd[1023]=L'\0';
        WCHAR mutableCmd[1024]; wcscpy(mutableCmd, cmd);
        DWORD code = run_hidden_and_wait(mutableCmd, 4000);
        return code == 0;
    }
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return false;
    // 其他错误回退到 schtasks 查询
    WCHAR sch[MAX_PATH]=L""; get_schtasks_path(sch, MAX_PATH);
    WCHAR cmd2[1024];
    _snwprintf(cmd2, 1024, L"\"%s\" /Query /TN \"%s\"", sch, TASK_NAME);
    cmd2[1023]=L'\0';
    WCHAR mutableCmd2[1024]; wcscpy(mutableCmd2, cmd2);
    DWORD code = run_hidden_and_wait(mutableCmd2, 4000);
    return code == 0;
}

static bool run_schtasks_elevated_if_needed(const WCHAR *params) {
    if (is_elevated()) {
        WCHAR sch[MAX_PATH]=L""; get_schtasks_path(sch, MAX_PATH);
        WCHAR cmd[2048];
        _snwprintf(cmd, 2048, L"\"%s\" %s", sch, params);
        cmd[2047]=L'\0';
        WCHAR mutableCmd[2048]; wcscpy(mutableCmd, cmd);
        DWORD code = run_hidden_and_wait(mutableCmd, 8000);
        if (code==0) return true;
        // Win7 上 schtasks 可能需要 /IT 或用户名，失败时记录到日志便于现场排查
        // LOG 将在调用方记录
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
    DWORD wait = WaitForSingleObject(sei.hProcess, 8000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &code);
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
        _snwprintf(params, 2048, L"/Create /TN \"%s\" /TR \"\\\"%s\\\" --tray\" /SC ONLOGON /F", tname, exe);
    }
    params[2047]=L'\0';
    return run_schtasks_elevated_if_needed(params);
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
    bool ok = run_schtasks_elevated_if_needed(params);
    if (ok) return true;
    // schtasks 返回 1 通常为不存在，视为已清理
    WCHAR tf[MAX_PATH]=L"";
    if (wcscmp(tname, TASK_NAME)==0) get_task_file_path(tf, MAX_PATH);
    else get_task_tray_file_path(tf, MAX_PATH);
    if (GetFileAttributesW(tf)==INVALID_FILE_ATTRIBUTES && (GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)) return true;
    return false;
}
static bool task_delete(void) {
    bool a = task_delete_one(TASK_NAME);
    bool b = task_delete_one(TASK_TRAY_NAME);
    // 兼容旧版：主任务删不掉也尝试删托盘任务，至少保证 check 能回落
    bool ok = a && b;
    // 无论如何都清理注册表回退
    reg_remove();
    if (ok) return true;
    if (!task_exists_fast()) {
        // 主任务已不存在，视为成功
        WCHAR trayFile[MAX_PATH]=L""; get_task_tray_file_path(trayFile, MAX_PATH);
        if (GetFileAttributesW(trayFile)==INVALID_FILE_ATTRIBUTES) return true;
        // 托盘任务单独存在也算成功清理了一半，回退查询主任务
        return true;
    }
    return false;
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
    if (tray_task_exists_fast()) return true;
    WCHAR exe[MAX_PATH]=L""; GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!exe[0]) return false;
    // 托盘任务不带 HIGHEST，避免 Win7 上重复提权，且与服务校时不冲突
    return task_create_internal(TASK_TRAY_NAME, exe, false);
}
static void tray_task_remove(void) { task_delete_one(TASK_TRAY_NAME); }

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

void startup_ensure_tray_args(void) {
    WCHAR tf[MAX_PATH]=L""; get_task_file_path(tf, MAX_PATH);
    if (GetFileAttributesW(tf)==INVALID_FILE_ATTRIBUTES) {
        // 主任务不存在时不折腾，但若存在托盘任务也补一下
        if (tray_task_exists_fast()) {
            WCHAR trayFile[MAX_PATH]=L""; get_task_tray_file_path(trayFile, MAX_PATH);
            if (!task_has_tray_flag_file(trayFile)) { WCHAR exe[MAX_PATH]=L""; GetModuleFileNameW(NULL, exe, MAX_PATH); if(exe[0]) task_create_internal(TASK_TRAY_NAME, exe, false); }
        }
        return;
    }
    if (!task_has_tray_flag_file(tf)) task_create();
    // 托盘任务也补齐
    WCHAR trayFile[MAX_PATH]=L""; get_task_tray_file_path(trayFile, MAX_PATH);
    if (GetFileAttributesW(trayFile)!=INVALID_FILE_ATTRIBUTES) {
        if (!task_has_tray_flag_file(trayFile)) { WCHAR exe[MAX_PATH]=L""; GetModuleFileNameW(NULL, exe, MAX_PATH); if(exe[0]) task_create_internal(TASK_TRAY_NAME, exe, false); }
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
        if (task_create()) {
            return true;
        }
        return false;
    } else {
        bool ok = task_delete();
        // 同时清理服务托盘任务
        tray_task_remove();
        reg_remove();
        if (ok) return true;
        if (!task_exists_fast() && !reg_is_enabled()) {
            WCHAR trayFile[MAX_PATH]=L""; get_task_tray_file_path(trayFile, MAX_PATH);
            if (GetFileAttributesW(trayFile)==INVALID_FILE_ATTRIBUTES) return true;
        }
        return false;
    }
}

// 供 service.c 在安装/卸载时调用，保持服务与托盘任务的生命周期一致
bool startup_tray_for_service(bool enable) {
    if (enable) return tray_task_ensure();
    tray_task_remove();
    return true;
}
