#define UNICODE
#define _UNICODE
#include "ntpserver.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <shellapi.h>

#define W32TIME_SVC L"W32Time"

static bool is_wow64_local(void) {
    BOOL wow = FALSE;
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (h) {
        typedef BOOL (WINAPI *PFN_IsWow64)(HANDLE, PBOOL);
        PFN_IsWow64 p = (PFN_IsWow64)GetProcAddress(h, "IsWow64Process");
        if (p) p(GetCurrentProcess(), &wow);
    }
    return wow != FALSE;
}
static void get_windows_dir_local(WCHAR *out, size_t cap) {
    UINT n = GetWindowsDirectoryW(out, (DWORD)cap);
    if (n==0 || n>=cap) wcscpy(out, L"C:\\Windows");
}
static void build_sys_bin_path_local(const WCHAR *leaf, WCHAR *out, size_t cap) {
    WCHAR win[MAX_PATH]=L"";
    get_windows_dir_local(win, MAX_PATH);
    bool useSysnative = false;
    if (is_wow64_local()) {
        WCHAR probe[MAX_PATH]=L"";
        _snwprintf(probe, MAX_PATH, L"%s\\Sysnative\\%s", win, leaf);
        probe[MAX_PATH-1]=L'\0';
        DWORD a = GetFileAttributesW(probe);
        if (a != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND) useSysnative = true;
        else {
            WCHAR test[MAX_PATH]=L"";
            _snwprintf(test, MAX_PATH, L"%s\\Sysnative", win);
            test[MAX_PATH-1]=L'\0';
            if (!(GetFileAttributesW(test)==INVALID_FILE_ATTRIBUTES && GetLastError()==ERROR_PATH_NOT_FOUND)) useSysnative = true;
        }
        if (useSysnative) {
            WCHAR t2[MAX_PATH]=L"";
            _snwprintf(t2, MAX_PATH, L"%s\\Sysnative", win);
            t2[MAX_PATH-1]=L'\0';
            if (GetFileAttributesW(t2)==INVALID_FILE_ATTRIBUTES && GetLastError()==ERROR_PATH_NOT_FOUND) useSysnative = false;
        }
    }
    // 用 bool 标志而不是比较字符串字面量地址（后者依赖字面量合并，/W4 报 C4130）
    const WCHAR *sys = useSysnative ? L"Sysnative" : L"System32";
    _snwprintf(out, cap, L"%s\\%s\\%s", win, sys, leaf);
    out[cap-1]=L'\0';
}

static bool is_elevated_impl(void) {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el; DWORD ret = 0;
    BOOL ok = FALSE;
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &ret)) ok = el.TokenIsElevated;
    CloseHandle(tok);
    return ok != FALSE;
}
bool ntp_is_elevated(void) { return is_elevated_impl(); }

const char* ntp_state_name(ntp_state_t s) {
    switch (s) {
        case NTP_DISABLED: return "未开启";
        case NTP_ENABLED_RUNNING: return "已开启·运行中";
        case NTP_ENABLED_STOPPED: return "已开启·服务未运行";
        case NTP_ENABLED_FIREWALL_BLOCKED: return "已开启·防火墙未放行";
        default: return "未知";
    }
}

static DWORD reg_get_dword(HKEY root, const WCHAR *sub, const WCHAR *name, DWORD def) {
    HKEY hk; DWORD v=def, cb=sizeof(DWORD), type=0;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk)!=ERROR_SUCCESS) return def;
    if (RegQueryValueExW(hk, name, NULL, &type, (BYTE*)&v, &cb)!=ERROR_SUCCESS) v=def;
    else if (type!=REG_DWORD) v=def;
    RegCloseKey(hk);
    return v;
}
static bool reg_set_dword(HKEY root, const WCHAR *sub, const WCHAR *name, DWORD v) {
    HKEY hk;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL)!=ERROR_SUCCESS) return false;
    LONG r=RegSetValueExW(hk, name, 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    RegCloseKey(hk);
    return r==ERROR_SUCCESS;
}

static int w32_query(void) {
    SC_HANDLE mgr=OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return 99;
    SC_HANDLE svc=OpenServiceW(mgr, W32TIME_SVC, SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(mgr); return 98; }
    SERVICE_STATUS st;
    int out=99;
    if (QueryServiceStatus(svc, &st)) {
        switch (st.dwCurrentState) {
            case SERVICE_RUNNING: out=1; break;
            case SERVICE_STOPPED: out=0; break;
            default: out=2; break;
        }
    }
    CloseServiceHandle(svc); CloseServiceHandle(mgr);
    return out;
}

static DWORD w32_start_type(void) {
    SC_HANDLE mgr=OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return (DWORD)-1;
    SC_HANDLE svc=OpenServiceW(mgr, W32TIME_SVC, SERVICE_QUERY_CONFIG);
    if (!svc) { CloseServiceHandle(mgr); return (DWORD)-1; }
    DWORD need=0;
    QueryServiceConfigW(svc, NULL, 0, &need);
    if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER) { CloseServiceHandle(svc); CloseServiceHandle(mgr); return (DWORD)-1; }
    BYTE *buf=(BYTE*)malloc(need);
    if (!buf) { CloseServiceHandle(svc); CloseServiceHandle(mgr); return (DWORD)-1; }
    DWORD ok=0;
    if (QueryServiceConfigW(svc, (LPQUERY_SERVICE_CONFIGW)buf, need, &need)) {
        ok=((LPQUERY_SERVICE_CONFIGW)buf)->dwStartType;
        free(buf); CloseServiceHandle(svc); CloseServiceHandle(mgr);
        return ok;
    }
    free(buf); CloseServiceHandle(svc); CloseServiceHandle(mgr);
    return (DWORD)-1;
}

static bool w32_ensure_auto_start(void) {
    SC_HANDLE mgr=OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!mgr) return false;
    SC_HANDLE svc=OpenServiceW(mgr, W32TIME_SVC, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
    if (!svc) { CloseServiceHandle(mgr); return false; }
    DWORD st=w32_start_type();
    // Close and reopen with proper rights for ChangeServiceConfig (already have it)
    bool needChange=false;
    if (st==(DWORD)-1) needChange=true;
    else if (st==SERVICE_DISABLED) needChange=true;
    else if (st==SERVICE_DEMAND_START) needChange=true; // 改为自动，避免重启后不自启
    bool ok=true;
    if (needChange) {
        if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, NULL,NULL,NULL,NULL,NULL,NULL,NULL)) {
            ok=false;
        }
    }
    CloseServiceHandle(svc); CloseServiceHandle(mgr);
    return ok;
}

static void get_netsh_path_local(WCHAR *out, size_t cap) { build_sys_bin_path_local(L"netsh.exe", out, cap); }
static void get_w32tm_path_local(WCHAR *out, size_t cap) { build_sys_bin_path_local(L"w32tm.exe", out, cap); }

static bool firewall_has_rule_hidden(void) {
    WCHAR netsh[MAX_PATH]=L""; get_netsh_path_local(netsh, MAX_PATH);
    WCHAR cmd[1024];
    _snwprintf(cmd, 1024, L"\"%s\" advfirewall firewall show rule name=\"NTP Server\"", netsh);
    cmd[1023]=L'\0';
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si,0,sizeof(si)); si.cb=sizeof(si);
    memset(&pi,0,sizeof(pi));
    WCHAR mutableCmd[1024]; wcscpy(mutableCmd, cmd);
    if (!CreateProcessW(NULL, mutableCmd, NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) return true;
    WaitForSingleObject(pi.hProcess, 4000);
    DWORD code=1; GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code==0) return true; // 找到规则
    return false;
}

ntp_state_t ntp_query_state(void) {
    DWORD en = reg_get_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\TimeProviders\\NtpServer", L"Enabled", 0);
    DWORD af = reg_get_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Config", L"AnnounceFlags", 0);
    if (en!=1 || (af!=5 && af!=10 && af!=0x0A)) return NTP_DISABLED;

    int ws = w32_query();
    if (ws==0) return NTP_ENABLED_STOPPED;
    if (ws!=1) return NTP_ENABLED_RUNNING;

    if (!firewall_has_rule_hidden()) return NTP_ENABLED_FIREWALL_BLOCKED;
    return NTP_ENABLED_RUNNING;
}

static DWORD run_hidden(WCHAR *cmdline, DWORD timeoutMs) {
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si,0,sizeof(si)); si.cb=sizeof(si);
    memset(&pi,0,sizeof(pi));
    if (!CreateProcessW(NULL, cmdline, NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) return 0xFFFFFFFFu;
    DWORD w=WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code=0xFFFFFFFFu;
    if (w==WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess,&code);
    else TerminateProcess(pi.hProcess,1);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code;
}

static bool run_elevated_or_hidden(WCHAR *exe, WCHAR *params) {
    if (is_elevated_impl()) {
        WCHAR cmd[2048];
        _snwprintf(cmd,2048,L"\"%s\" %s", exe, params);
        cmd[2047]=L'\0';
        WCHAR mut[2048]; wcscpy(mut,cmd);
        DWORD c=run_hidden(mut, 10000);
        return c!=0xFFFFFFFFu && c==0;
    }
    SHELLEXECUTEINFOW sei; memset(&sei,0,sizeof(sei));
    sei.cbSize=sizeof(sei); sei.fMask=SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb=L"runas"; sei.lpFile=exe; sei.lpParameters=params; sei.nShow=SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (!sei.hProcess) return true;
    DWORD w=WaitForSingleObject(sei.hProcess, 12000);
    DWORD code=1;
    if (w==WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess,&code);
    CloseHandle(sei.hProcess);
    return code==0;
}

static bool ensure_firewall_allow(void) {
    WCHAR params[512];
    _snwprintf(params,512,L"advfirewall firewall add rule name=\"NTP Server\" dir=in protocol=udp localport=123 profile=any enable=yes action=allow");
    params[511]=L'\0';
    WCHAR netsh[MAX_PATH]=L""; get_netsh_path_local(netsh, MAX_PATH);
    return run_elevated_or_hidden(netsh, params);
}
static void remove_firewall_rule(void) {
    WCHAR params[512];
    _snwprintf(params,512,L"advfirewall firewall delete rule name=\"NTP Server\" protocol=udp localport=123");
    params[511]=L'\0';
    WCHAR netsh[MAX_PATH]=L""; get_netsh_path_local(netsh, MAX_PATH);
    run_elevated_or_hidden(netsh, params);
}

static bool do_enable_steps(void) {
    bool wasDisabled=false;
    DWORD st=w32_start_type();
    if (st==SERVICE_DISABLED) wasDisabled=true;

    // 若服务被禁用，先改回自动，否则后续 StartService 直接报 1058
    if (wasDisabled) {
        if (!w32_ensure_auto_start()) {
            SetLastError(ERROR_SERVICE_DISABLED);
            return false;
        }
        // 给 SCM 一点时间刷新
        Sleep(200);
    }

    // 停服务改注册表更稳（若正在运行）
    {
        SC_HANDLE mgr=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);
        SC_HANDLE svc=NULL;
        if (mgr) svc=OpenServiceW(mgr, W32TIME_SVC, SERVICE_STOP|SERVICE_QUERY_STATUS);
        if (svc) { SERVICE_STATUS ss; ControlService(svc,SERVICE_CONTROL_STOP,&ss); }
        if (svc) CloseServiceHandle(svc);
        if (mgr) CloseServiceHandle(mgr);
        for (int i=0;i<20;i++){ int s=w32_query(); if(s==0) break; Sleep(200); }
    }

    bool ok=true;
    if (!reg_set_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\TimeProviders\\NtpServer", L"Enabled", 1)) ok=false;
    if (!reg_set_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Config", L"AnnounceFlags", 5)) ok=false;
    reg_set_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Config", L"LocalClockDispersion", 0);
    // 确保自启（幂等）
    w32_ensure_auto_start();
    if (!ok) {
        if (GetLastError()==ERROR_SUCCESS) SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }

    {
        WCHAR w32tm[MAX_PATH]=L""; get_w32tm_path_local(w32tm, MAX_PATH);
        WCHAR p1[] = L"/config /reliable:yes /update";
        run_elevated_or_hidden(w32tm, p1);
    }
    // 启动服务：带禁用重试
    {
        SC_HANDLE m2=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);
        SC_HANDLE s2=m2?OpenServiceW(m2,W32TIME_SVC,SERVICE_START|SERVICE_CHANGE_CONFIG|SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG):NULL;
        if (s2) {
            if (!StartServiceW(s2,0,NULL)) {
                DWORD e=GetLastError();
                if (e==ERROR_SERVICE_DISABLED) {
                    // 再次尝试改自动后重试
                    ChangeServiceConfigW(s2, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, NULL,NULL,NULL,NULL,NULL,NULL,NULL);
                    Sleep(200);
                    if (!StartServiceW(s2,0,NULL)) {
                        e=GetLastError();
                        if (e==ERROR_SERVICE_ALREADY_RUNNING) e=ERROR_SUCCESS;
                        else { SetLastError(e); CloseServiceHandle(s2); if(m2) CloseServiceHandle(m2); return false; }
                    }
                } else if (e!=ERROR_SERVICE_ALREADY_RUNNING) {
                    SetLastError(e);
                    CloseServiceHandle(s2); if(m2) CloseServiceHandle(m2);
                    return false;
                }
            }
        } else {
            DWORD e=GetLastError(); SetLastError(e);
            if(m2) CloseServiceHandle(m2);
            return false;
        }
        if (s2) CloseServiceHandle(s2);
        if (m2) CloseServiceHandle(m2);
    }
    for (int i=0;i<25;i++){ int s=w32_query(); if(s==1) break; Sleep(200); }
    if (w32_query()!=1) {
        // 仍未起来，报具体错误
        DWORD s=w32_start_type();
        if (s==SERVICE_DISABLED) SetLastError(ERROR_SERVICE_DISABLED);
        else if (GetLastError()==ERROR_SUCCESS) SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
        return false;
    }

    ensure_firewall_allow();
    {
        WCHAR w32tm[MAX_PATH]=L""; get_w32tm_path_local(w32tm, MAX_PATH);
        WCHAR p2[] = L"/resync /rediscover";
        run_elevated_or_hidden(w32tm, p2);
    }
    return true;
}

static bool do_disable_steps(void) {
    reg_set_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\TimeProviders\\NtpServer", L"Enabled", 0);
    reg_set_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Config", L"AnnounceFlags", 10);
    {
        WCHAR w32tm[MAX_PATH]=L""; get_w32tm_path_local(w32tm, MAX_PATH);
        WCHAR p[] = L"/config /reliable:no /update";
        run_elevated_or_hidden(w32tm, p);
    }
    remove_firewall_rule();
    return true;
}

bool ntp_enable(void) {
    if (!is_elevated_impl()) {
        WCHAR exe[MAX_PATH]=L""; GetModuleFileNameW(NULL, exe, MAX_PATH);
        WCHAR params[] = L"--ntp-enable";
        SHELLEXECUTEINFOW sei; memset(&sei,0,sizeof(sei));
        sei.cbSize=sizeof(sei); sei.fMask=SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb=L"runas"; sei.lpFile=exe; sei.lpParameters=params; sei.nShow=SW_HIDE;
        if (!ShellExecuteExW(&sei)) return false;
        if (sei.hProcess) {
            DWORD w=WaitForSingleObject(sei.hProcess, 15000);
            DWORD code=1; if(w==WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess,&code);
            CloseHandle(sei.hProcess);
            if (code!=0) { SetLastError(code==0xFFFFFFFFu?ERROR_ACCESS_DENIED:code); return false; }
            Sleep(400);
            ntp_state_t st=ntp_query_state();
            if (st==NTP_ENABLED_RUNNING || st==NTP_ENABLED_FIREWALL_BLOCKED) return true;
            // 提权子进程已成功但最终状态仍不对，给出明确错误
            if (st==NTP_ENABLED_STOPPED) {
                DWORD tp=w32_start_type();
                if (tp==SERVICE_DISABLED) SetLastError(ERROR_SERVICE_DISABLED);
                else SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
            } else if (st==NTP_DISABLED) SetLastError(ERROR_ACCESS_DENIED);
            return false;
        }
        Sleep(900);
        ntp_state_t st=ntp_query_state();
        return st!=NTP_DISABLED;
    }
    if (!do_enable_steps()) return false;
    Sleep(300);
    ntp_state_t st=ntp_query_state();
    if (st==NTP_ENABLED_RUNNING || st==NTP_ENABLED_FIREWALL_BLOCKED) return true;
    if (st==NTP_ENABLED_STOPPED) {
        DWORD tp=w32_start_type();
        if (tp==SERVICE_DISABLED) SetLastError(ERROR_SERVICE_DISABLED);
        else SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
    } else if (st==NTP_DISABLED) {
        if (GetLastError()==ERROR_SUCCESS) SetLastError(ERROR_ACCESS_DENIED);
    }
    return false;
}

bool ntp_disable(void) {
    if (!is_elevated_impl()) {
        WCHAR exe[MAX_PATH]=L""; GetModuleFileNameW(NULL, exe, MAX_PATH);
        WCHAR params[] = L"--ntp-disable";
        SHELLEXECUTEINFOW sei; memset(&sei,0,sizeof(sei));
        sei.cbSize=sizeof(sei); sei.fMask=SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb=L"runas"; sei.lpFile=exe; sei.lpParameters=params; sei.nShow=SW_HIDE;
        if (!ShellExecuteExW(&sei)) return false;
        if (sei.hProcess) {
            DWORD w=WaitForSingleObject(sei.hProcess, 12000);
            DWORD code=1; if(w==WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess,&code);
            CloseHandle(sei.hProcess);
            if (code!=0) { SetLastError(code); return false; }
            Sleep(300);
            return ntp_query_state()==NTP_DISABLED;
        }
        Sleep(700);
        return ntp_query_state()==NTP_DISABLED;
    }
    if (!do_disable_steps()) return false;
    Sleep(250);
    return ntp_query_state()==NTP_DISABLED;
}
