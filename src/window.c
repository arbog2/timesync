#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <time.h>

#include "window.h"
#include "resource.h"
#include "auth.h"
#include "config.h"
#include "startup.h"
#include "task.h"
#include "timeset.h"
#include "util.h"
#include "service.h"
#include "ntpserver.h"

// ---- 内部状态 ----
static config_t*     s_cfg = NULL;
static task_state_t* s_state = NULL;
static char          s_cfgPath[MAX_PATH] = "";
static char          s_exeDir[MAX_PATH] = "";
static HWND          s_hMain = NULL;
static HFONT         s_hFont = NULL;

#define WM_TRAYICON (WM_APP + 100)
#define ID_TRAY     1
#define TIMER_STATUS 1
#define TIMER_ONCE_RESULT 98
/* 提权 --once 的结果回传：300ms 一轮，最多 100 轮（30s）。
   UAC 确认 + 子进程启动 + 3 次采样（每轮最长 timeout_ms）远小于这个上限，
   上限只用来兜住"子进程被拦在别处 / 崩溃"的死等。 */
#define ONCE_RESULT_MAX_TRIES 100
#define WM_SVC_DONE  (WM_APP + 101)
#define WM_NTP_DONE  (WM_APP + 102)

static volatile LONG s_svcBusy = 0;

/* 提权 --once 子进程的结果文件与轮询状态（仅 UI 线程访问） */
static WCHAR s_onceResult[MAX_PATH] = L"";
static int   s_onceTries = 0;

/* 状态栏刷新：定义在文件后部，poll_once_result 要用它提前回填界面 */
static void refresh_status(HWND hwnd);

typedef enum { SVC_OP_INSTALL = 1, SVC_OP_UNINSTALL, SVC_OP_START, SVC_OP_STOP } svc_op_t;
typedef struct { HWND hwnd; svc_op_t op; DWORD result; DWORD err; } svc_job_t;

// ---- 本机时钟服务 (W32Time NTP Server) ----
static volatile LONG s_ntpBusy = 0;
typedef enum { NTP_OP_ENABLE = 1, NTP_OP_DISABLE, NTP_OP_CHECK } ntp_op_t;
typedef struct { HWND hwnd; ntp_op_t op; DWORD result; DWORD err; ntp_state_t st; } ntp_job_t;

static void ensure_font(HWND hwnd) {
    if (s_hFont) return;
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);
    int h = -MulDiv(9, dpi, 72);
    s_hFont = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!s_hFont)
        s_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

// ---- helpers ----
static void utf8_to_w(const char* s, WCHAR* out, int cap) {
    if (!out || cap <= 0) return;
    if (!s || !s[0]) { out[0]=L'\0'; return; }
    /* 转换失败（含空间不足）时不写入任何内容，必须显式置空，
       否则调用方会读到未初始化的栈缓冲。 */
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) == 0)
        out[0]=L'\0';
}
static void w_to_utf8(const WCHAR* w, char* out, int cap) {
    if (!out || cap <= 0) return;
    if (!w || !w[0]) { out[0]='\0'; return; }
    /* WideCharToMultiByte 空间不足时返回 0 且一个字节都不写；
       忽略返回值会让调用方对未初始化的 tmp 做 strlen。 */
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) == 0)
        out[0]='\0';
}
static int w_to_int(const WCHAR* w, int def) {
    if (!w || !w[0]) return def;
    WCHAR* end; long v = wcstol(w, &end, 10);
    if (end==w) return def;
    return (int)v;
}
static void open_path_w(const WCHAR* p) {
    ShellExecuteW(NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL);
}
static void show_info(const WCHAR* msg, const WCHAR* title) {
    MessageBoxW(s_hMain, msg, title, MB_OK | MB_ICONINFORMATION);
}
static void show_err(const WCHAR* msg, const WCHAR* title) {
    MessageBoxW(s_hMain, msg, title, MB_OK | MB_ICONWARNING);
}
/* 读一个 UTF-8 文本文件到 buf；文件不存在或读失败时 buf 置空串。
   结果文件由子进程以字节模式写出，无需转码即可按 UTF-8 解析。 */
static void wchar_to_utf8_file(const WCHAR* wpath, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    FILE* f = NULL;
    _wfopen_s(&f, wpath, L"rb");
    if (!f) return;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
}

// ---- 文件/目录操作 ----
static void do_open_dir_gui(void) {
    WCHAR wdir[MAX_PATH]=L"";
    utf8_to_w(s_exeDir, wdir, MAX_PATH);
    if (GetFileAttributesW(wdir)==INVALID_FILE_ATTRIBUTES) {
        show_err(L"程序目录不存在。", L"提示");
        return;
    }
    open_path_w(wdir);
}
static void do_view_log_gui(void) {
    if (!s_cfg || !s_cfg->log_file[0]) {
        show_info(L"未配置日志文件。\n请在配置中设置日志路径。", L"提示");
        return;
    }
    WCHAR wpath[MAX_PATH]=L"";
    utf8_to_w(s_cfg->log_file, wpath, MAX_PATH);
    if (GetFileAttributesW(wpath)==INVALID_FILE_ATTRIBUTES) {
        WCHAR msg[MAX_PATH+96];
        _snwprintf(msg, MAX_PATH+96, L"日志文件不存在：\n%s\n\n请确认已开启日志且程序有写入权限。", wpath);
        msg[MAX_PATH+95]=L'\0';
        show_info(msg, L"提示");
        return;
    }
    open_path_w(wpath);
}
static void do_edit_config_gui(void) {
    WCHAR wpath[MAX_PATH]=L"";
    utf8_to_w(s_cfgPath, wpath, MAX_PATH);
    if (!wpath[0]) { show_err(L"未找到配置文件路径。", L"错误"); return; }
    if (GetFileAttributesW(wpath)==INVALID_FILE_ATTRIBUTES) {
        WCHAR wdir[MAX_PATH]=L""; wcscpy(wdir, wpath);
        WCHAR* bs = wcsrchr(wdir, L'\\');
        if (bs) { *bs=L'\0'; CreateDirectoryW(wdir, NULL); }
        // 生成最小可用配置由 config_write_sample 也可，但此处由 window 负责
        // 注意：必须用字节模式 "w" 写 UTF-8，不能用 "w, ccs=UTF-8"——后者会在文件头
        // 写入 EF BB BF BOM，而 BOM 会让首行 "[server]" 变成 "\xEF\xBB\xBF[server]"。
        // config.c 现在也能容忍 BOM，但这里从源头避免更干净。
        FILE* f=NULL; _wfopen_s(&f, wpath, L"w");
        if (f) {
            fprintf(f, "[server]\naddress = \"192.168.1.100\"\nport = 123\n\n");
            fprintf(f, "[sync]\ninterval_minutes = 60\nretry_seconds = 10\ntimeout_ms = 2000\n");
            fprintf(f, "max_offset_ms = 5000\nmin_offset_ms = 200\nforce_enabled = true\nforce_flag = true\nsync_on_start = true\n");
            fprintf(f, "\n[service]\nenabled = false\n\n[log]\nfile_enabled = true\nfile_path = \"C:\\\\ProgramData\\\\LANTimeSync\\\\time_sync.log\"\nlevel = \"info\"\n");
            fclose(f);
        }
    }
    HINSTANCE r = ShellExecuteW(NULL, L"open", wpath, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) ShellExecuteW(NULL, L"open", L"notepad.exe", wpath, NULL, SW_SHOWNORMAL);
}
LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static void do_about_gui(void) {
    const WCHAR* cls = L"LANTimeSyncAboutDlg";
    UnregisterClassW(cls, GetModuleHandleW(NULL));
    WNDCLASSW wc; memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc = AboutWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    INITCOMMONCONTROLSEX icc; icc.dwSize=sizeof(icc); icc.dwICC=ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);
    RegisterClassW(&wc);

    int W=380, H=200;
    RECT rc={0,0,W,H}; AdjustWindowRect(&rc, WS_CAPTION|WS_SYSMENU|WS_POPUP, FALSE);
    int winW=rc.right-rc.left, winH=rc.bottom-rc.top;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-winW)/2;
    int sy=(GetSystemMetrics(SM_CYSCREEN)-winH)/2;
    HWND hwnd = CreateWindowExW(0, cls, L"关于 LAN Time Sync",
                                WS_CAPTION|WS_SYSMENU|WS_POPUP|WS_VISIBLE,
                                sx, sy, winW, winH, s_hMain, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    EnableWindow(s_hMain, FALSE);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsWindow(hwnd) && IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(hwnd)) break;
    }
    MSG tmp; while (PeekMessageW(&tmp,NULL,WM_QUIT,WM_QUIT,PM_REMOVE)) { if(tmp.wParam==0) PostQuitMessage(0); }
    EnableWindow(s_hMain, TRUE);
    SetForegroundWindow(s_hMain);
    UnregisterClassW(cls, GetModuleHandleW(NULL));
}

LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HDC hdc=GetDC(hwnd); int dpi=GetDeviceCaps(hdc,LOGPIXELSY); ReleaseDC(hwnd,hdc);
            int hfH=-MulDiv(9,dpi,72);
            HFONT hf=CreateFontW(hfH,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
            if(!hf) hf=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT hfBold=CreateFontW(hfH,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
            HWND s1=CreateWindowW(L"STATIC",L"LAN Time Sync",WS_CHILD|WS_VISIBLE|SS_CENTER,16,16,348,20,hwnd,NULL,GetModuleHandleW(NULL),NULL);
            if(hfBold) SendMessageW(s1,WM_SETFONT,(WPARAM)hfBold,TRUE);
            HWND s2=CreateWindowW(L"STATIC",L"局域网强制时钟同步（SNTP over UDP）",WS_CHILD|WS_VISIBLE|SS_CENTER,16,38,348,16,hwnd,NULL,GetModuleHandleW(NULL),NULL);
            SendMessageW(s2,WM_SETFONT,(WPARAM)hf,TRUE);
            HWND s3=CreateWindowW(L"STATIC",L"作者：arbog",WS_CHILD|WS_VISIBLE|SS_CENTER,16,64,348,16,hwnd,NULL,GetModuleHandleW(NULL),NULL);
            SendMessageW(s3,WM_SETFONT,(WPARAM)hf,TRUE);
            HWND s4=CreateWindowW(L"STATIC",L"江苏高中信息技术",WS_CHILD|WS_VISIBLE|SS_CENTER,16,84,348,16,hwnd,NULL,GetModuleHandleW(NULL),NULL);
            SendMessageW(s4,WM_SETFONT,(WPARAM)hf,TRUE);
            // 可点击链接：优先用 SysLink，失败回退为普通 Static + 手型光标
            INITCOMMONCONTROLSEX icc2; icc2.dwSize=sizeof(icc2); icc2.dwICC=ICC_LINK_CLASS; InitCommonControlsEx(&icc2);
            // SysLink 无居中样式，用父窗口宽度与链接文本宽度计算居中 x
            const WCHAR* aboutUrl = L"https://pd.qq.com/g/656378514005520142";
            int linkW = 0; { HDC hdcLink=GetDC(hwnd); HFONT old=(HFONT)SelectObject(hdcLink,hf); SIZE sz; GetTextExtentPoint32W(hdcLink, aboutUrl, (int)wcslen(aboutUrl), &sz); SelectObject(hdcLink,old); ReleaseDC(hwnd,hdcLink); linkW=sz.cx+8; }
            int linkX = (380 - linkW)/2; if(linkX<16) linkX=16;
            HWND link=CreateWindowW(WC_LINK,L"<a href=\"https://pd.qq.com/g/656378514005520142\">https://pd.qq.com/g/656378514005520142</a>",WS_CHILD|WS_VISIBLE|WS_TABSTOP,linkX,108,linkW,16,hwnd,(HMENU)1001,GetModuleHandleW(NULL),NULL);
            if(!link) {
                link=CreateWindowW(L"STATIC",L"https://pd.qq.com/g/656378514005520142",WS_CHILD|WS_VISIBLE|SS_CENTER|SS_NOTIFY,16,108,348,16,hwnd,(HMENU)1001,GetModuleHandleW(NULL),NULL);
                SendMessageW(link,WM_SETFONT,(WPARAM)hf,TRUE);
            } else {
                SendMessageW(link,WM_SETFONT,(WPARAM)hf,TRUE);
            }
            HWND ok=CreateWindowW(L"BUTTON",L"确定",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON|WS_TABSTOP,154,142,72,24,hwnd,(HMENU)IDOK,GetModuleHandleW(NULL),NULL);
            SendMessageW(ok,WM_SETFONT,(WPARAM)hf,TRUE);
            SetFocus(ok);
            return 0;
        }
        case WM_COMMAND:
            if(LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL) { DestroyWindow(hwnd); return 0; }
            if(LOWORD(wp)==1001) { ShellExecuteW(NULL,L"open",L"https://pd.qq.com/g/656378514005520142",NULL,NULL,SW_SHOWNORMAL); return 0; }
            break;
        case WM_NOTIFY: {
            LPNMHDR nm=(LPNMHDR)lp;
            if(nm->idFrom==1001 && (nm->code==NM_CLICK || nm->code==NM_RETURN)) {
                PNMLINK pl=(PNMLINK)lp;
                ShellExecuteW(NULL,L"open",pl->item.szUrl,NULL,NULL,SW_SHOWNORMAL);
                return 0;
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(1); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static bool apply_live_server_from_ui(HWND hwnd) {
    // 仅把服务器地址/端口从文本框实时同步到 s_cfg（不落盘），供立即同步使用
    WCHAR w[512]; char tmp[512];
    GetWindowTextW(GetDlgItem(hwnd?hwnd:s_hMain, IDC_EDIT_SERVER), w, 512);
    w_to_utf8(w, tmp, 512);
    char* p=tmp; while(*p==' '||*p=='\t') p++;
    char* e=p+strlen(p); while(e>p && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) e--; *e='\0';
    if (!p[0]) { show_err(L"服务器地址不能为空。", L"错误"); return false; }
    strncpy(s_cfg->server, p, sizeof(s_cfg->server)-1); s_cfg->server[sizeof(s_cfg->server)-1]='\0';
    GetWindowTextW(GetDlgItem(hwnd?hwnd:s_hMain, IDC_EDIT_PORT), w, 64);
    int port = w_to_int(w, s_cfg->port);
    if (port<=0||port>65535) { show_err(L"端口范围 1-65535。", L"错误"); return false; }
    s_cfg->port = port;
    return true;
}

static void do_sync_now_gui(HWND hwnd) {
    HWND h = hwnd ? hwnd : s_hMain;
    if (!apply_live_server_from_ui(h)) return;
    WCHAR wserver[128]=L""; utf8_to_w(s_cfg->server, wserver, 128);
    WCHAR hint[256]; _snwprintf(hint,256,L"正在同步 %s:%d ...", wserver, s_cfg->port);
    SetWindowTextW(GetDlgItem(s_hMain, IDC_LABEL_STATUS), hint);

    // 管理员身份运行（UAC 提权后）可直接同步，不应再弹 runas；
    // 仅当非提权且确实缺 SeSystemtimePrivilege 时才走 runas 提权。
    if (timeset_is_elevated() || timeset_enable_priv()) {
        task_request_sync();
        return;
    }
    // 无权限：用 runas 提权跑 --once。需让子进程用到文本框里的实时地址，
    // 因此先把当前 s_cfg（含实时 server/port）写入临时配置文件。
    // 同步结果由子进程写进 s_onceResult，父进程轮询取回——/SUBSYSTEM:WINDOWS
    // 的子进程没有控制台，printf 的结果无处可看。
    WCHAR wtmp[MAX_PATH]=L"";
    GetTempPathW(MAX_PATH, wtmp);
    WCHAR wtmpFile[MAX_PATH]=L"";
    GetTempFileNameW(wtmp, L"lts", 0, wtmpFile);
    WCHAR wresFile[MAX_PATH]=L"";
    GetTempFileNameW(wtmp, L"ltsres", 0, wresFile);
    /* GetTempFileName 会先建出一个空文件；子进程是原子改名落地的，
       留着这个空壳会让父进程立刻误判"结果已就绪"并读到空内容。 */
    DeleteFileW(wresFile);

    /* wide-char write, no ACP/UTF-8 loss; child deletes it via --temp-config */
    bool tmpOk = config_save_w(s_cfg, wtmpFile);
    WCHAR exe[MAX_PATH]=L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR params[1400]=L"";
    bool paramsOk = true;
    if (tmpOk) {
        /* _snwprintf 截断时返回 -1，会得到一个不完整的 --config 路径，
           提权子进程于是加载不到配置。宁可显式失败也不要静默用错配置。 */
        if (_snwprintf(params, 1400, L"--once --temp-config --config \"%s\" --result \"%s\"",
                       wtmpFile, wresFile) < 0) paramsOk = false;
    } else {
        WCHAR wcfg[MAX_PATH]=L""; utf8_to_w(s_cfgPath, wcfg, MAX_PATH);
        if (wcfg[0]) { if (_snwprintf(params, 1400, L"--once --config \"%s\" --result \"%s\"", wcfg, wresFile) < 0) paramsOk = false; }
        else if (_snwprintf(params, 1400, L"--once --result \"%s\"", wresFile) < 0) paramsOk = false;
    }
    params[1399]=L'\0';
    if (!paramsOk) {
        /* 子进程不会启动，--temp-config 的自我清理也就不会发生，
           此处自行删除临时配置，避免在 %TEMP% 留下垃圾。 */
        if (tmpOk) DeleteFileW(wtmpFile);
        DeleteFileW(wresFile);
        MessageBoxW(s_hMain, L"配置路径过长，无法发起提权同步。\n请把程序移到更短的路径后重试。", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    HINSTANCE r = ShellExecuteW(NULL, L"runas", exe, params, NULL, SW_HIDE);
    if ((INT_PTR)r <= 32) {
        if ((INT_PTR)r == 42) {   // SE_ERR_CANCELLED：用户拒了 UAC，什么都不会发生
            DeleteFileW(wtmpFile);
            DeleteFileW(wresFile);
            return;
        }
        DeleteFileW(wtmpFile);
        DeleteFileW(wresFile);
        MessageBoxW(s_hMain, L"需要管理员权限才能修改系统时间。\n请右键以管理员身份运行，或安装为系统服务。", L"提示", MB_OK | MB_ICONINFORMATION);
    } else {
        s_onceResult[0] = L'\0';
        wcscpy(s_onceResult, wresFile);
        s_onceTries = 0;
        SetTimer(hwnd, TIMER_ONCE_RESULT, 300, NULL);
        SetWindowTextW(GetDlgItem(s_hMain, IDC_LABEL_STATUS), L"已发起提权同步，等待结果...");
    }
}

/* 轮询提权子进程写回的结果文件。就绪后把 offset/rtt/成功标志灌进共享的
   task_state，界面上"偏移/RTT/上次同步/状态"随之刷新，并弹一次框把
   结果直接摆到用户眼前（此前这段信息在非提权路径下彻底无处可看）。 */
static void poll_once_result(HWND hwnd) {
    if (!s_onceResult[0]) return;
    if (++s_onceTries > ONCE_RESULT_MAX_TRIES) {
        KillTimer(hwnd, TIMER_ONCE_RESULT);
        DeleteFileW(s_onceResult);
        s_onceResult[0] = L'\0';
        SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_STATUS), L"提权同步超时：未收到子进程结果。");
        show_err(L"提权同步在 30 秒内没有返回结果。\n请确认 UAC 已通过，必要时查看日志。", L"提权同步");
        return;
    }

    HANDLE hf = CreateFileW(s_onceResult, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;   // 还没写好
    CloseHandle(hf);

    // 子进程以原子改名落地，看到的一定是完整文件
    char buf[1024] = "";
    wchar_to_utf8_file(s_onceResult, buf, sizeof(buf));
    KillTimer(hwnd, TIMER_ONCE_RESULT);
    DeleteFileW(s_onceResult);
    s_onceResult[0] = L'\0';

    long long off = 0, rtt = 0;
    int ok = 0;
    char err[256] = "";
    char key[32] = "", val[512] = "";
    char* line = buf;
    while (line && *line) {
        char* eol = strpbrk(line, "\r\n");
        if (eol) *eol = '\0';
        char* eq = strchr(line, '=');
        if (eq) {
            size_t kl = (size_t)(eq - line);
            if (kl < sizeof(key)) {
                memcpy(key, line, kl); key[kl] = '\0';
                strncpy(val, eq + 1, sizeof(val) - 1); val[sizeof(val) - 1] = '\0';
                if      (strcmp(key, "ok")     == 0) ok  = atoi(val);
                else if (strcmp(key, "offset") == 0) off = _strtoi64(val, NULL, 10);
                else if (strcmp(key, "rtt")    == 0) rtt = _strtoi64(val, NULL, 10);
                else if (strcmp(key, "error")  == 0) { strncpy(err, val, sizeof(err)-1); err[sizeof(err)-1]='\0'; }
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    if (strcmp(err, "-") == 0) err[0] = '\0';

    // 灌进共享状态：与 task_sync_once 用同一组原子写，worker 线程并发同步时也不会写坏 64 位量
    InterlockedExchange64(&s_state->last_offset_ms, off);
    InterlockedExchange64(&s_state->last_rtt_ms,    rtt);
    InterlockedExchange64(&s_state->last_sync_sec,  (LONGLONG)time(NULL));
    InterlockedIncrement(&s_state->synced_count);
    strncpy(s_state->last_error, err, sizeof(s_state->last_error) - 1);
    s_state->last_error[sizeof(s_state->last_error) - 1] = '\0';
    s_state->last_success = ok ? TRUE : FALSE;
    refresh_status(hwnd);

    WCHAR msg[512], werr[256];
    utf8_to_w(err[0] ? err : "-", werr, 256);
    if (ok) {
        _snwprintf(msg, 512, L"同步成功。\n\n偏移：%lld ms\nRTT：%lld ms", off, rtt);
        show_info(msg, L"立即同步");
    } else {
        _snwprintf(msg, 512, L"同步失败。\n\n原因：%s\n\n偏移：%lld ms\nRTT：%lld ms", werr, off, rtt);
        show_err(msg, L"立即同步");
    }
}

// ---- 托盘 ----
static void tray_add(HWND hwnd) {
    NOTIFYICONDATAW nid; memset(&nid,0,sizeof(nid));
    /* Windows 7 只认识 NOTIFYICONDATA_V3_SIZE（x64 968 / x86 952 字节）；
       直接传 sizeof(nid)（含 guidItem/hBalloonIcon 等 Vista 之后新增字段，
       x64 976 / x86 956）在旧系统上可能导致 Shell_NotifyIcon 失败、托盘图标不出现。
       这里按目标版本传对应大小。 */
    nid.cbSize = NOTIFYICONDATAW_V3_SIZE;
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(nid.szTip, L"LAN Time Sync");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        /* 极少数环境只接受 V2/V1：逐级回退重试，避免图标静默消失 */
        nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
            nid.cbSize = NOTIFYICONDATAW_V1_SIZE;
            Shell_NotifyIconW(NIM_ADD, &nid);
        }
    }
}
static void tray_remove(HWND hwnd) {
    NOTIFYICONDATAW nid; memset(&nid,0,sizeof(nid));
    nid.cbSize=NOTIFYICONDATAW_V3_SIZE; nid.hWnd=hwnd; nid.uID=ID_TRAY;
    if (!Shell_NotifyIconW(NIM_DELETE,&nid)) {
        nid.cbSize=NOTIFYICONDATAW_V2_SIZE;
        Shell_NotifyIconW(NIM_DELETE,&nid);
    }
}
static void tray_show_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    WCHAR wserver[128]=L""; if (s_cfg && s_cfg->server[0]) utf8_to_w(s_cfg->server, wserver, 128);
    WCHAR buf[256]=L"";
    if (wserver[0]) _snwprintf(buf,256,L"同步服务器: %s", wserver); else wcscpy(buf,L"LAN Time Sync");
    buf[255]=L'\0';
    AppendMenuW(m, MF_STRING|MF_GRAYED, 0, buf);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3001, L"显示主窗口");
    AppendMenuW(m, MF_STRING, 3002, L"立即同步");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3003, L"查看日志");
    AppendMenuW(m, MF_STRING, 3005, L"编辑配置");
    AppendMenuW(m, MF_STRING, 3006, L"关于");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3007, L"退出");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int id = (int)TrackPopupMenu(m, TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch(id){
        case 3001: {
            static DWORD lastMenuBring = 0;
            DWORD now = GetTickCount();
            if (now - lastMenuBring < 600) break;
            lastMenuBring = now;
            if (auth_is_set() && !auth_prompt_verify(hwnd)) break;
            window_bring_to_front(hwnd); break;
        }
        case 3002: do_sync_now_gui(hwnd); break;
        case 3003: do_view_log_gui(); break;
        case 3005: {
            if (auth_is_set() && !auth_prompt_verify(hwnd)) break;
            window_bring_to_front(hwnd); break;
        }
        case 3006: do_about_gui(); break;
        case 3007:
            PostMessageW(hwnd, WM_COMMAND, IDC_BTN_EXIT, 0);
            break;
        default: break;
    }
}

// ---- 配置读写到界面 ----
static void ui_load_from_cfg(HWND hwnd) {
    WCHAR w[512];
    utf8_to_w(s_cfg->server, w, 512); SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_SERVER), w);
    _snwprintf(w, 64, L"%d", s_cfg->port); SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_PORT), w);
    _snwprintf(w, 64, L"%d", s_cfg->interval_minutes); SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_INTERVAL), w);
    _snwprintf(w, 64, L"%d", s_cfg->retry_seconds); SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_RETRY), w);
    _snwprintf(w, 64, L"%d", s_cfg->timeout_ms); SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_TIMEOUT), w);
    _snwprintf(w, 64, L"%d", s_cfg->max_offset_ms); SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_MAXOFF), w);
    _snwprintf(w, 64, L"%d", s_cfg->min_offset_ms); SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_MINOFF), w);
    CheckDlgButton(hwnd, IDC_CHECK_FORCE, s_cfg->force_flag ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_CHECK_ALLOWLARGE, s_cfg->allow_large_offset ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_CHECK_SYNCONSTART, s_cfg->sync_on_start ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_CHECK_LOGENABLED, s_cfg->log_enabled ? BST_CHECKED : BST_UNCHECKED);
}

static bool ui_save_to_cfg(HWND hwnd) {
    WCHAR w[512]; char tmp[512];
    GetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_SERVER), w, 512); w_to_utf8(w, tmp, 512);
    // 去掉首尾空格
    char* p=tmp; while(*p==' '||*p=='\t') p++; char* e=p+strlen(p); while(e>p && (e[-1]==' '||e[-1]=='\t')) e--; *e='\0';
    if (!p[0]) { show_err(L"服务器地址不能为空。", L"错误"); return false; }
    strncpy(s_cfg->server, p, sizeof(s_cfg->server)-1); s_cfg->server[sizeof(s_cfg->server)-1]='\0';

    GetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_PORT), w, 64); s_cfg->port = w_to_int(w, s_cfg->port);
    if (s_cfg->port<=0||s_cfg->port>65535) { show_err(L"端口范围 1-65535。", L"错误"); return false; }
    GetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_INTERVAL), w, 64); s_cfg->interval_minutes = w_to_int(w, s_cfg->interval_minutes);
    GetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_RETRY), w, 64); s_cfg->retry_seconds = w_to_int(w, s_cfg->retry_seconds);
    GetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_TIMEOUT), w, 64); s_cfg->timeout_ms = w_to_int(w, s_cfg->timeout_ms);
    GetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_MAXOFF), w, 64); s_cfg->max_offset_ms = w_to_int(w, s_cfg->max_offset_ms);
    GetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_MINOFF), w, 64); s_cfg->min_offset_ms = w_to_int(w, s_cfg->min_offset_ms);
    s_cfg->force_flag = IsDlgButtonChecked(hwnd, IDC_CHECK_FORCE)==BST_CHECKED;
    /* force_enabled 是"是否写时钟"的总开关（config 中的 force_enabled），
       force_flag 是"激进/保守"模式选择。界面上的"强制模式"复选框对应 force_flag，
       不能把总开关一并赋值：否则用户在界面上取消勾选"强制模式"会静默关闭总开关，
       此后程序只测量不校时（日志里仍显示正常），排查起来非常隐蔽。 */
    s_cfg->allow_large_offset = IsDlgButtonChecked(hwnd, IDC_CHECK_ALLOWLARGE)==BST_CHECKED;
    s_cfg->sync_on_start = IsDlgButtonChecked(hwnd, IDC_CHECK_SYNCONSTART)==BST_CHECKED;
    s_cfg->log_enabled = IsDlgButtonChecked(hwnd, IDC_CHECK_LOGENABLED)==BST_CHECKED;
    // 日志路径与配置文件固定使用默认路径，不再由界面输入
    {
        config_t def; config_init_defaults(&def);
        strncpy(s_cfg->log_file, def.log_file, sizeof(s_cfg->log_file)-1);
        s_cfg->log_file[sizeof(s_cfg->log_file)-1]='\0';
    }
    // 保存到文件
    if (!config_save(s_cfg, s_cfgPath)) {
        WCHAR msg[600]; _snwprintf(msg,600,L"保存配置失败：\n%hs\n\n若路径在 ProgramData，请以管理员身份运行。", s_cfgPath);
        show_err(msg, L"保存失败");
        return false;
    }
    // 若启用日志则重建日志句柄
    util_log_init(s_cfg->log_enabled ? s_cfg->log_file : NULL, s_cfg->log_level);
    LOG_INFO("config saved via GUI to %s", s_cfgPath);
    return true;
}

static void set_svc_busy(HWND hwnd, BOOL busy) {
    InterlockedExchange(&s_svcBusy, busy ? 1 : 0);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_SVC_INSTALL),   !busy);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_SVC_UNINSTALL), !busy);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_SVC_START),     !busy);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_SVC_STOP),      !busy);
}

/* startup_is_enabled() may shell out to schtasks (seconds) when the task
   files are not readable; never call it on the 1s UI timer directly.
   Cache 30s, invalidate after toggle / service ops. */
static BOOL s_autoValid = FALSE;
static BOOL s_autoOn = FALSE;
static DWORD s_autoTick = 0;
static BOOL autostart_cached(void) {
    DWORD now = GetTickCount();
    if (s_autoValid && (now - s_autoTick) < 30000) return s_autoOn;
    s_autoOn = startup_is_enabled() ? TRUE : FALSE;
    s_autoTick = now;
    s_autoValid = TRUE;
    return s_autoOn;
}
static void autostart_invalidate(void) { s_autoValid = FALSE; }

static void refresh_service_status(HWND hwnd) {
    if (InterlockedCompareExchange(&s_svcBusy, 0, 0)) return; // 操作进行中不刷
    svc_state_t st = service_query_state();

    WCHAR w[128];
    WCHAR hint[256] = L"";
    if (st == SVC_NOT_INSTALLED) {
        wcscpy(w, L"服务状态：未安装");
        wcscpy(hint, L"需管理员权限安装。安装后服务将以 SYSTEM 权限自动校时。");
    } else if (st == SVC_RUNNING) {
        wcscpy(w, L"服务状态：运行中");
        wcscpy(hint, L"GUI 与服务同时运行会重复校时，建议退出 GUI。");
    } else if (st == SVC_STOPPED) {
        wcscpy(w, L"服务状态：已安装（已停止）");
        wcscpy(hint, L"可点击“启动服务”恢复自动校时。");
    } else if (st == SVC_START_PENDING) {
        wcscpy(w, L"服务状态：正在启动…");
    } else if (st == SVC_STOP_PENDING) {
        wcscpy(w, L"服务状态：正在停止…");
    } else {
        wcscpy(w, L"服务状态：未知");
        wcscpy(hint, L"无法查询服务控制管理器。");
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_SVC_STATUS), w);
    if (hint[0]) SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_SVC_HINT), hint);
    else SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_SVC_HINT), L"");

    BOOL installed = (st != SVC_NOT_INSTALLED && st != SVC_UNKNOWN);
    BOOL running   = (st == SVC_RUNNING);
    BOOL stopped   = (st == SVC_STOPPED);

    ShowWindow(GetDlgItem(hwnd, IDC_BTN_SVC_INSTALL),   (!installed) ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_BTN_SVC_UNINSTALL), installed ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_BTN_SVC_START),     stopped ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_BTN_SVC_STOP),      running ? SW_SHOW : SW_HIDE);

    // 开机自启动：服务已安装时置灰（服务自带自启动，无需 Run 键）
    HWND hAuto = GetDlgItem(hwnd, IDC_CHECK_AUTOSTART);
    if (hAuto) {
        EnableWindow(hAuto, !installed);
        if (installed) {
            // 置灰时给提示
            SetWindowTextW(hAuto, L"开机自启动（服务已接管）");
        } else {
            SetWindowTextW(hAuto, L"开机自启动");
            CheckDlgButton(hwnd, IDC_CHECK_AUTOSTART, autostart_cached() ? BST_CHECKED : BST_UNCHECKED);
        }
    }
}

// ---- 本机时钟服务 (W32Time NTP Server) ----
static void set_ntp_busy(HWND hwnd, BOOL busy) {
    InterlockedExchange(&s_ntpBusy, busy ? 1 : 0);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_NTP_ENABLE),  !busy);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_NTP_DISABLE), !busy);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_NTP_CHECK),   !busy);
}
static void refresh_ntp_status(HWND hwnd) {
    if (InterlockedCompareExchange(&s_ntpBusy, 0, 0)) return;
    ntp_state_t st = ntp_query_state();
    const char* nameA = ntp_state_name(st);
    WCHAR wName[64]; utf8_to_w(nameA, wName, 64);
    WCHAR w[128];
    _snwprintf(w, 128, L"本机时钟服务：%s", wName);
    SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_NTP_STATUS), w);

    WCHAR hint[256] = L"";
    BOOL enabled = (st != NTP_DISABLED && st != NTP_UNKNOWN);
    if (st == NTP_DISABLED) wcscpy(hint, L"未开启，学生机无法从本机同步时间。");
    else if (st == NTP_ENABLED_RUNNING) wcscpy(hint, L"正常，本机正在通过 UDP 123 对外提供时间。");
    else if (st == NTP_ENABLED_STOPPED) wcscpy(hint, L"W32Time 服务未运行，点击“检测”重试或手动启动。");
    else if (st == NTP_ENABLED_FIREWALL_BLOCKED) wcscpy(hint, L"防火墙未放行 UDP 123，局域网可能无法访问。");
    else wcscpy(hint, L"");
    SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_NTP_HINT), hint);

    ShowWindow(GetDlgItem(hwnd, IDC_BTN_NTP_ENABLE),  (!enabled) ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_BTN_NTP_DISABLE), enabled ? SW_SHOW : SW_HIDE);
}
static DWORD WINAPI ntp_worker_thread(LPVOID param) {
    ntp_job_t* job = (ntp_job_t*)param;
    job->result = 0; job->err = 0; job->st = NTP_UNKNOWN;
    if (job->op == NTP_OP_ENABLE) {
        if (ntp_enable()) { job->result = 1; job->st = ntp_query_state(); }
        else job->err = GetLastError();
    } else if (job->op == NTP_OP_DISABLE) {
        if (ntp_disable()) { job->result = 1; job->st = ntp_query_state(); }
        else job->err = GetLastError();
    } else if (job->op == NTP_OP_CHECK) {
        job->st = ntp_query_state();
        job->result = 1;
    }
    PostMessageW(job->hwnd, WM_NTP_DONE, (WPARAM)job->op, (LPARAM)job);
    return 0;
}
static void ntp_launch(HWND hwnd, ntp_op_t op) {
    if (InterlockedCompareExchange(&s_ntpBusy, 1, 0) != 0) return;
    if (op == NTP_OP_CHECK) {
        ntp_job_t* job = (ntp_job_t*)calloc(1, sizeof(ntp_job_t));
        if (!job) { InterlockedExchange(&s_ntpBusy, 0); return; }
        job->hwnd = hwnd; job->op = op;
        set_ntp_busy(hwnd, TRUE);
        SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_NTP_STATUS), L"本机时钟服务：检测中…");
        HANDLE th = CreateThread(NULL, 0, ntp_worker_thread, job, 0, NULL);
        if (!th) { free(job); set_ntp_busy(hwnd, FALSE); refresh_ntp_status(hwnd); }
        else CloseHandle(th);
        return;
    }
    // 开启/关闭：未提权时 ntp_enable/disable 内部会自行 runas 提权，无需在此分发
    ntp_job_t* job = (ntp_job_t*)calloc(1, sizeof(ntp_job_t));
    if (!job) { InterlockedExchange(&s_ntpBusy, 0); return; }
    job->hwnd = hwnd; job->op = op;
    set_ntp_busy(hwnd, TRUE);
    if (op == NTP_OP_ENABLE) SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_NTP_STATUS), L"本机时钟服务：正在开启…");
    else SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_NTP_STATUS), L"本机时钟服务：正在关闭…");
    HANDLE th = CreateThread(NULL, 0, ntp_worker_thread, job, 0, NULL);
    if (!th) { free(job); set_ntp_busy(hwnd, FALSE); refresh_ntp_status(hwnd); }
    else CloseHandle(th);
}
static void on_ntp_done(HWND hwnd, ntp_op_t op, ntp_job_t* job) {
    set_ntp_busy(hwnd, FALSE);
    DWORD err = job ? job->err : 0;
    BOOL ok = job ? (job->result != 0) : FALSE;
    ntp_state_t st = job ? job->st : NTP_UNKNOWN;
    if (job) free(job);
    if (op == NTP_OP_CHECK) {
        refresh_ntp_status(hwnd);
        return;
    }
    if (op == NTP_OP_ENABLE) {
        if (ok) show_info(L"本机时钟服务已开启。\n已放行防火墙 UDP 123，局域网内可通过本机 IP 同步时间。", L"成功");
        else {
            if (err == ERROR_SERVICE_DISABLED || err == 1058) show_err(L"开启失败：W32Time 服务被禁用。\n已尝试将其改为“自动”仍失败，请手动打开 services.msc 将 Windows Time 设为“自动”后重试。", L"错误");
            else if (err == ERROR_SERVICE_REQUEST_TIMEOUT || err == 1053) show_err(L"开启失败：W32Time 服务启动超时。\n请稍后点击“检测”重试，或手动在 services.msc 中启动 Windows Time。", L"错误");
            else if (err == ERROR_ACCESS_DENIED || err == 5) show_err(L"开启失败：需要管理员权限。\n请在 UAC 弹窗中点“是”，或右键以管理员身份运行。", L"错误");
            else if (err == 0) show_err(L"开启失败：未能完成配置。\n请确认已在 UAC 中允许提权，或右键以管理员身份运行后重试。", L"错误");
            else { WCHAR m[256]; _snwprintf(m,256,L"开启失败，错误码 %lu。\n请以管理员身份重试。", err); show_err(m, L"错误"); }
        }
    } else if (op == NTP_OP_DISABLE) {
        if (ok) show_info(L"本机时钟服务已关闭。", L"成功");
        else { WCHAR m[256]; _snwprintf(m,256,L"关闭失败，错误码 %lu。", err); show_err(m, L"错误"); }
    }
    (void)st;
    refresh_ntp_status(hwnd);
}

static DWORD WINAPI svc_worker_thread(LPVOID param) {
    svc_job_t* job = (svc_job_t*)param;
    job->result = 0; job->err = 0;
    switch (job->op) {
        case SVC_OP_INSTALL: {
            char exeA[MAX_PATH]=""; GetModuleFileNameA(NULL, exeA, sizeof(exeA));
            if (service_install(exeA, true)) {
                // 安装后尝试启动，失败不算安装失败
                service_start_svc();
                job->result = 1;
            } else {
                job->err = GetLastError();
            }
            break;
        }
        case SVC_OP_UNINSTALL: {
            svc_state_t st = service_query_state();
            if (st == SVC_RUNNING || st == SVC_START_PENDING) {
                service_stop_svc();
                for (int i=0;i<30;i++) { Sleep(200); if (service_query_state()==SVC_STOPPED) break; }
            }
            if (service_uninstall()) job->result = 1;
            else job->err = GetLastError();
            break;
        }
        case SVC_OP_START:
            if (service_start_svc()) job->result = 1;
            else job->err = GetLastError();
            break;
        case SVC_OP_STOP: {
            if (service_stop_svc()) {
                for (int i=0;i<15;i++) { Sleep(200); if (service_query_state()==SVC_STOPPED) break; }
                job->result = 1;
            } else job->err = GetLastError();
            break;
        }
    }
    PostMessageW(job->hwnd, WM_SVC_DONE, (WPARAM)job->op, (LPARAM)job);
    return 0;
}

static void svc_launch(HWND hwnd, svc_op_t op) {
    if (InterlockedCompareExchange(&s_svcBusy, 1, 0) != 0) return; // 已有操作进行中
    // 非提权下的安装/卸载/启停：直接用 runas 另起提权进程，不在本进程阻塞
    if (!service_is_elevated()) {
        InterlockedExchange(&s_svcBusy, 0);
        WCHAR exe[MAX_PATH]=L"";
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        const WCHAR* arg = L"";
        if (op == SVC_OP_INSTALL)   arg = L"--install-service";
        else if (op == SVC_OP_UNINSTALL) arg = L"--uninstall-service";
        else if (op == SVC_OP_START)     arg = L"--service-start";
        else if (op == SVC_OP_STOP)      arg = L"--service-stop";
        HINSTANCE r = ShellExecuteW(NULL, L"runas", exe, arg, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32 && (INT_PTR)r != 42) {
            show_err(L"需要管理员权限才能执行此操作。\n请右键以管理员身份运行本程序。", L"提示");
        } else if ((INT_PTR)r > 32) {
            // 提权子进程已启动，1.2s 后刷新状态
            SetTimer(hwnd, 99, 1200, NULL);
        }
        return;
    }
    // 已提权：后台线程执行，避免卡死界面
    svc_job_t* job = (svc_job_t*)calloc(1, sizeof(svc_job_t));
    if (!job) { InterlockedExchange(&s_svcBusy, 0); return; }
    job->hwnd = hwnd; job->op = op;
    set_svc_busy(hwnd, TRUE);
    // 临时把状态文案改为“正在…”
    if (op == SVC_OP_INSTALL)   SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_SVC_STATUS), L"服务状态：正在安装…");
    if (op == SVC_OP_UNINSTALL) SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_SVC_STATUS), L"服务状态：正在卸载…");
    if (op == SVC_OP_START)     SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_SVC_STATUS), L"服务状态：正在启动…");
    if (op == SVC_OP_STOP)      SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_SVC_STATUS), L"服务状态：正在停止…");
    HANDLE th = CreateThread(NULL, 0, svc_worker_thread, job, 0, NULL);
    if (!th) { free(job); set_svc_busy(hwnd, FALSE); refresh_service_status(hwnd); }
    else CloseHandle(th);
}

static void on_svc_done(HWND hwnd, svc_op_t op, svc_job_t* job) {
    set_svc_busy(hwnd, FALSE);
    DWORD err = job ? job->err : 0;
    BOOL ok = job ? (job->result != 0) : FALSE;
    if (job) free(job);

    if (op == SVC_OP_INSTALL) {
        if (ok) show_info(L"系统服务已安装并启动。\nGUI 可退出，服务将在后台以 SYSTEM 权限持续校时。", L"成功");
        else {
            if (err == ERROR_SERVICE_EXISTS || err == ERROR_ALREADY_EXISTS)
                show_info(L"服务已存在，已更新可执行路径。", L"提示");
            else if (err == ERROR_ACCESS_DENIED) show_err(L"安装失败：需要管理员权限。", L"错误");
            else { WCHAR m[256]; _snwprintf(m,256,L"安装失败，错误码 %lu。", err); show_err(m, L"错误"); }
        }
    } else if (op == SVC_OP_UNINSTALL) {
        if (ok) show_info(L"系统服务已卸载。", L"成功");
        else {
            // 常见：服务仍被标记为删除（需重启或关闭服务管理器）
            if (err == ERROR_SERVICE_MARKED_FOR_DELETE)
                show_info(L"服务已标记为删除。\n请关闭“服务”管理器后刷新，或重启后生效。", L"提示");
            else { WCHAR m[256]; _snwprintf(m,256,L"卸载失败，错误码 %lu。\n若服务仍在运行，请先停止服务。", err); show_err(m, L"错误"); }
        }
    } else if (op == SVC_OP_START) {
        if (!ok) { WCHAR m[256]; _snwprintf(m,256,L"启动服务失败，错误码 %lu。", err); show_err(m, L"错误"); }
    } else if (op == SVC_OP_STOP) {
        if (!ok) { WCHAR m[256]; _snwprintf(m,256,L"停止服务失败，错误码 %lu。", err); show_err(m, L"错误"); }
    }
    autostart_invalidate();
    refresh_service_status(hwnd);
}

static void do_svc_install_gui(HWND hwnd) { svc_launch(hwnd, SVC_OP_INSTALL); }
static void do_svc_uninstall_gui(HWND hwnd) {
    if (MessageBoxW(hwnd, L"确定要卸载系统服务 LANTimeSync 吗？\n卸载后将停止后台校时（GUI 仍可手动同步）。", L"确认卸载", MB_YESNO|MB_ICONQUESTION) != IDYES) return;
    svc_launch(hwnd, SVC_OP_UNINSTALL);
}
static void do_svc_start_gui(HWND hwnd) { svc_launch(hwnd, SVC_OP_START); }
static void do_svc_stop_gui(HWND hwnd)  { svc_launch(hwnd, SVC_OP_STOP); }

/* ---- 状态刷新 ---- */
static void refresh_status(HWND hwnd){
    if (!s_state) return;
    LONGLONG off = s_state->last_offset_ms;
    LONGLONG rtt = s_state->last_rtt_ms;
    LONGLONG last = s_state->last_sync_sec;
    BOOL ok = s_state->last_success;
    const char* err = s_state->last_error;

    WCHAR w[256];
    // 上次同步
    if (last==0) wcscpy(w, L"上次同步：从未");
    else {
        time_t t=(time_t)last; struct tm tmv; localtime_s(&tmv, &t);
        WCHAR ts[64]; wcsftime(ts,64,L"%Y-%m-%d %H:%M:%S", &tmv);
        _snwprintf(w,256,L"上次同步：%s  %s", ts, ok?L"成功":L"失败");
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_LASTSYNC), w);

    _snwprintf(w,256,L"偏移：%lld ms", (long long)off); SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_OFFSET), w);
    _snwprintf(w,256,L"RTT：%lld ms", (long long)rtt); SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_RTT), w);

    // 下次同步估算：用 interval - elapsed
    if (s_cfg) {
        time_t now=time(NULL);
        LONGLONG elapsed = last? (LONGLONG)(now - (time_t)last) : 0;
        LONGLONG remain = (LONGLONG)s_cfg->interval_minutes*60 - elapsed;
        if (last==0) wcscpy(w, L"下次同步：--");
        else if (remain<=0) wcscpy(w, L"下次同步：即将");
        else {
            int mm=(int)(remain/60), ss=(int)(remain%60);
            _snwprintf(w,256,L"下次同步：%02d:%02d 后", mm, ss);
        }
        SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_NEXTSYNC), w);
    }

    if (err && err[0]) {
        WCHAR werr[256]; utf8_to_w(err, werr, 256);
        _snwprintf(w,256,L"状态：%s", werr); SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_STATUS), w);
    } else if (last==0) {
        SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_STATUS), L"状态：等待首次同步...");
    } else if (ok) {
        SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_STATUS), L"状态：正常");
    } else {
        SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_STATUS), L"状态：异常");
    }
}

// ---- 布局 ----
static HWND create_label(HWND parent, int id, const WCHAR* text, int x,int y,int w,int h){
    HWND c = CreateWindowW(L"STATIC", text, WS_CHILD|WS_VISIBLE, x,y,w,h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_hFont, TRUE);
    return c;
}
static HWND create_edit(HWND parent, int id, int x,int y,int w,int h){
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, x,y,w,h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_hFont, TRUE);
    return c;
}
static HWND create_button(HWND parent, int id, const WCHAR* text, int x,int y,int w,int h, DWORD style){
    HWND c = CreateWindowW(L"BUTTON", text, WS_CHILD|WS_VISIBLE|style, x,y,w,h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_hFont, TRUE);
    return c;
}
static HWND create_check(HWND parent, int id, const WCHAR* text, int x,int y,int w,int h){
    return create_button(parent, id, text, x,y,w,h, BS_AUTOCHECKBOX);
}
static HWND create_group(HWND parent, int id, const WCHAR* text, int x,int y,int w,int h){
    HWND c = CreateWindowW(L"BUTTON", text, WS_CHILD|WS_VISIBLE|BS_GROUPBOX, x,y,w,h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_hFont, TRUE);
    return c;
}

static void build_ui(HWND hwnd){
    ensure_font(hwnd);
    // 整体留白 12
    int W=640, H=520;
    (void)W; (void)H;
    // 组：状态
    create_group(hwnd, IDC_GROUP_STATUS, L"状态", 12, 8, 616, 96);
    create_label(hwnd, IDC_LABEL_STATUS,  L"状态：--",  24, 28, 590, 16);
    create_label(hwnd, IDC_LABEL_LASTSYNC,L"上次同步：--", 24, 48, 300, 16);
    create_label(hwnd, IDC_LABEL_NEXTSYNC,L"下次同步：--", 340, 48, 260, 16);
    create_label(hwnd, IDC_LABEL_OFFSET,  L"偏移：--", 24, 68, 160, 16);
    create_label(hwnd, IDC_LABEL_RTT,     L"RTT：--",  200,68, 160, 16);
    create_button(hwnd, IDC_BTN_SYNC_NOW, L"立即同步", 460, 64, 100, 26, BS_PUSHBUTTON);

    // 组：配置（日志路径与配置文件路径固定使用默认值，不在界面展示）
    create_group(hwnd, IDC_GROUP_CONFIG, L"配置", 12, 112, 616, 140);
    int y=130;
    create_label(hwnd, IDC_LABEL_SERVER, L"服务器地址", 24, y, 80, 16);
    create_edit(hwnd, IDC_EDIT_SERVER, 108, y-2, 260, 20);
    create_label(hwnd, IDC_LABEL_PORT, L"端口", 380, y, 36, 16);
    create_edit(hwnd, IDC_EDIT_PORT, 420, y-2, 70, 20);
    y+=24;
    create_label(hwnd, IDC_LABEL_INTERVAL, L"同步周期", 24, y, 70, 16);
    create_edit(hwnd, IDC_EDIT_INTERVAL, 100, y-2, 60, 20);
    create_label(hwnd, -1, L"分钟", 164, y, 36, 16);
    create_label(hwnd, IDC_LABEL_RETRY, L"重试", 210, y, 36, 16);
    create_edit(hwnd, IDC_EDIT_RETRY, 250, y-2, 50, 20);
    create_label(hwnd, -1, L"秒", 304, y, 24, 16);
    create_label(hwnd, IDC_LABEL_TIMEOUT, L"超时", 340, y, 36, 16);
    create_edit(hwnd, IDC_EDIT_TIMEOUT, 380, y-2, 60, 20);
    create_label(hwnd, -1, L"ms", 444, y, 30, 16);
    y+=24;
    create_label(hwnd, IDC_LABEL_MAXOFF, L"阈值 max", 24, y, 70, 16);
    create_edit(hwnd, IDC_EDIT_MAXOFF, 100, y-2, 60, 20);
    create_label(hwnd, -1, L"ms", 164, y, 30, 16);
    create_label(hwnd, IDC_LABEL_MINOFF, L"min", 200, y, 30, 16);
    create_edit(hwnd, IDC_EDIT_MINOFF, 234, y-2, 60, 20);
    create_label(hwnd, -1, L"ms", 298, y, 30, 16);
    create_check(hwnd, IDC_CHECK_FORCE, L"强制模式", 340, y, 90, 16);
    create_check(hwnd, IDC_CHECK_ALLOWLARGE, L"允许跨年校时", 440, y, 120, 16);
    y+=20;
    create_check(hwnd, IDC_CHECK_SYNCONSTART, L"启动即同步", 24, y, 110, 16);
    create_check(hwnd, IDC_CHECK_LOGENABLED, L"启用日志", 150, y, 90, 16);
    y+=24;
    create_button(hwnd, IDC_BTN_SAVE, L"应用并保存", 24, y, 110, 24, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_RELOAD, L"重载配置", 144, y, 90, 24, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_EDIT_CONFIG, L"用记事本编辑", 244, y, 120, 24, BS_PUSHBUTTON);

    // 组：系统服务（配置区压缩后整体上移）
    create_group(hwnd, IDC_GROUP_SERVICE, L"系统服务", 12, 270, 616, 64);
    create_label(hwnd, IDC_LABEL_SVC_STATUS, L"服务状态：检测中…", 24, 290, 220, 16);
    create_label(hwnd, IDC_LABEL_SVC_HINT, L"", 24, 310, 420, 16);
    create_button(hwnd, IDC_BTN_SVC_INSTALL,   L"安装为系统服务", 460, 286, 130, 26, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_SVC_UNINSTALL, L"卸载服务",       460, 286, 130, 26, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_SVC_START,     L"启动服务",       460, 316, 62, 22, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_SVC_STOP,      L"停止服务",       530, 316, 62, 22, BS_PUSHBUTTON);

    // 组：安全（移至快捷上方）
    create_group(hwnd, -1, L"安全", 12, 342, 616, 48);
    create_label(hwnd, IDC_LABEL_PWD_STATUS, auth_is_set() ? L"密码保护：已启用" : L"密码保护：未设置", 24, 360, 200, 16);
    create_button(hwnd, IDC_BTN_SET_PWD,   L"设置密码", 260, 358, 90, 24, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_CLEAR_PWD, L"清除密码", 360, 358, 90, 24, BS_PUSHBUTTON);
    create_check(hwnd, IDC_CHECK_AUTOSTART, L"开机自启动", 470, 360, 110, 16);

    // 组：本机时钟服务 (W32Time NTP Server)
    create_group(hwnd, IDC_GROUP_NTPSERVER, L"本机时钟服务", 12, 398, 616, 64);
    create_label(hwnd, IDC_LABEL_NTP_STATUS, L"本机时钟服务：检测中…", 24, 418, 260, 16);
    create_label(hwnd, IDC_LABEL_NTP_HINT, L"", 24, 438, 340, 16);
    create_button(hwnd, IDC_BTN_NTP_ENABLE,  L"开启时钟服务", 430, 414, 120, 26, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_NTP_DISABLE, L"关闭时钟服务", 430, 414, 120, 26, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_NTP_CHECK,   L"检测", 560, 414, 52, 26, BS_PUSHBUTTON);

    // 组：快捷
    create_group(hwnd, IDC_GROUP_ACTIONS, L"快捷", 12, 470, 616, 52);
    create_button(hwnd, IDC_BTN_VIEW_LOG, L"查看日志", 24, 490, 90, 24, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_OPEN_DIR, L"打开程序目录", 124, 490, 120, 24, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_ABOUT, L"关于", 254, 490, 70, 24, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_MINIMIZE, L"最小化到托盘", 420, 490, 110, 24, BS_PUSHBUTTON);
    create_button(hwnd, IDC_BTN_EXIT, L"退出", 540, 490, 70, 24, BS_PUSHBUTTON);
}

// ---- 窗口过程 ----
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
        case WM_CREATE:
            build_ui(hwnd);
            ui_load_from_cfg(hwnd);
            tray_add(hwnd);
            SetTimer(hwnd, TIMER_STATUS, 1000, NULL);
            refresh_status(hwnd);
            refresh_service_status(hwnd);
            refresh_ntp_status(hwnd);
            // 首次进入后台静默检测一次，避免 1s 轮询里拉起 netsh 卡顿
            ntp_launch(hwnd, NTP_OP_CHECK);
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (code==BN_CLICKED || code==0) {
                switch(id){
                    case IDC_BTN_SYNC_NOW: do_sync_now_gui(hwnd); break;
                    case IDC_BTN_SAVE: if (ui_save_to_cfg(hwnd)) { show_info(L"配置已保存。", L"成功"); ui_load_from_cfg(hwnd); refresh_service_status(hwnd); } break;
                    case IDC_BTN_RELOAD: {
                        config_t tmp; if (config_load(&tmp, s_cfgPath)) { *s_cfg=tmp; ui_load_from_cfg(hwnd); refresh_service_status(hwnd); show_info(L"已重载配置。", L"提示"); } else show_err(L"重载失败。", L"错误");
                        break;
                    }
                    case IDC_BTN_VIEW_LOG: do_view_log_gui(); break;
                    case IDC_BTN_OPEN_DIR: do_open_dir_gui(); break;
                    case IDC_BTN_EDIT_CONFIG: do_edit_config_gui(); break;
                    case IDC_BTN_ABOUT: do_about_gui(); break;
                    case IDC_BTN_MINIMIZE: ShowWindow(hwnd, SW_HIDE); break;
                    case IDC_BTN_SVC_INSTALL:   do_svc_install_gui(hwnd); break;
                    case IDC_BTN_SVC_UNINSTALL: do_svc_uninstall_gui(hwnd); break;
                    case IDC_BTN_SVC_START:     do_svc_start_gui(hwnd); break;
                    case IDC_BTN_SVC_STOP:      do_svc_stop_gui(hwnd); break;
                    case IDC_BTN_EXIT: {
                        if (auth_is_set() && !auth_prompt_verify(hwnd)) break;
                        // 真退出。worker 线程 task_run 的退出条件就是
                        // service_is_stopping()，而它同时也是 SCM 停止标志，
                        // 所以这里只需 service_set_stopping()，主线程随后
                        // DestroyWindow -> PostQuitMessage 结束消息循环，
                        // main.c 的 run_as_tray 会等 worker 收尾。
                        service_set_stopping();
                        DestroyWindow(hwnd);
                        break;
                    }
                    case IDC_CHECK_LOGENABLED:
                        break;
                    case IDC_BTN_SET_PWD: {
                        if (!auth_is_set()) {
                            if (auth_prompt_set(hwnd))
                                SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_PWD_STATUS), L"密码保护：已启用");
                        } else {
                            if (!auth_prompt_verify(hwnd)) break;
                            if (auth_prompt_set(hwnd))
                                SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_PWD_STATUS), L"密码保护：已启用");
                        }
                        break;
                    }
                    case IDC_BTN_CLEAR_PWD: {
                        if (!auth_is_set()) { show_info(L"尚未设置密码。", L"提示"); break; }
                        if (!auth_prompt_verify(hwnd)) break;
                        if (MessageBoxW(hwnd, L"确定要清除密码保护吗？", L"确认", MB_YESNO|MB_ICONQUESTION)!=IDYES) break;
                        if (auth_clear()) SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL_PWD_STATUS), L"密码保护：未设置");
                        else show_err(L"清除失败。", L"错误");
                        break;
                    }
                    case IDC_CHECK_AUTOSTART: {
                        BOOL checked = IsDlgButtonChecked(hwnd, IDC_CHECK_AUTOSTART)==BST_CHECKED;
                        // 服务已安装时不应允许通过 Run 键/计划任务自启，刷新会置灰，
                        // 此处防御性再拦一次。service_query_state 每次都要开关一次
                        // SCM，判两次会白付一份开销，这里只查一次。
                        svc_state_t st = service_query_state();
                        if (st != SVC_NOT_INSTALLED && st != SVC_UNKNOWN) {
                            CheckDlgButton(hwnd, IDC_CHECK_AUTOSTART, BST_UNCHECKED);
                            show_info(L"系统服务已安装，无需再设置开机自启动。\n服务本身即为开机自启。", L"提示");
                            break;
                        }
                        if (!startup_set(checked)) {
                            // 回滚复选框
                            CheckDlgButton(hwnd, IDC_CHECK_AUTOSTART, checked?BST_UNCHECKED:BST_CHECKED);
                            show_err(L"设置开机自启动失败。", L"错误");
                        }
                        autostart_invalidate();
                        break;
                    }
                    case IDC_BTN_NTP_ENABLE:  ntp_launch(hwnd, NTP_OP_ENABLE); break;
                    case IDC_BTN_NTP_DISABLE: {
                        if (MessageBoxW(hwnd, L"确定要关闭本机时钟服务吗？\n关闭后学生机将无法从本机同步时间。", L"确认", MB_YESNO|MB_ICONQUESTION)!=IDYES) break;
                        ntp_launch(hwnd, NTP_OP_DISABLE); break;
                    }
                    case IDC_BTN_NTP_CHECK:   ntp_launch(hwnd, NTP_OP_CHECK); break;
                    default: break;
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wp==TIMER_STATUS) { refresh_status(hwnd); refresh_service_status(hwnd); }
            else if (wp==TIMER_ONCE_RESULT) poll_once_result(hwnd);
            else if (wp==99) { KillTimer(hwnd, 99); refresh_service_status(hwnd); refresh_ntp_status(hwnd); }
            return 0;
        case WM_SVC_DONE: {
            svc_op_t op = (svc_op_t)wp;
            svc_job_t* job = (svc_job_t*)lp;
            on_svc_done(hwnd, op, job);
            return 0;
        }
        case WM_NTP_DONE: {
            ntp_op_t op = (ntp_op_t)wp;
            ntp_job_t* job = (ntp_job_t*)lp;
            on_ntp_done(hwnd, op, job);
            return 0;
        }
        case WM_TRAYICON:
            if (LOWORD(lp)==WM_RBUTTONUP || LOWORD(lp)==WM_CONTEXTMENU) tray_show_menu(hwnd);
            else if (LOWORD(lp)==WM_LBUTTONDBLCLK || LOWORD(lp)==WM_LBUTTONUP) {
                // 最小化状态下单击即视为唤起，避免用户误双击产生两次密码框
                static DWORD lastBring = 0;
                DWORD now = GetTickCount();
                if (now - lastBring < 600) break;
                lastBring = now;
                if (auth_is_set() && !auth_prompt_verify(hwnd)) break;
                window_bring_to_front(hwnd);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0)==SC_MINIMIZE) { ShowWindow(hwnd, SW_HIDE); return 0; }
            break;
        case WM_CLOSE:
            // 关闭按钮 -> 隐藏到托盘，而不是退出
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_STATUS);
            KillTimer(hwnd, TIMER_ONCE_RESULT);
            if (s_onceResult[0]) { DeleteFileW(s_onceResult); s_onceResult[0] = L'\0'; }
            tray_remove(hwnd);
            PostQuitMessage(0);
            return 0;
        case WM_ENDSESSION:
            // 系统关机时确保托盘图标移除
            tray_remove(hwnd);
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void window_bring_to_front(HWND hwnd){
    if (!hwnd) hwnd=s_hMain;
    if (!hwnd) return;
    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
}

HWND window_create(HINSTANCE hInst, config_t* cfg, task_state_t* state,
                   const char* cfgPath, const char* exeDir){
    s_cfg=cfg; s_state=state;
    if (cfgPath) strncpy(s_cfgPath, cfgPath, sizeof(s_cfgPath)-1);
    if (exeDir)  strncpy(s_exeDir,  exeDir,  sizeof(s_exeDir)-1);

    InitCommonControls();

    WNDCLASSW wc; memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"LANTimeSyncMainWnd";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassW(&wc);

    int W=640, H=520;
    RECT rc={0,0,W,H};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, FALSE);
    int winW = rc.right-rc.left, winH = rc.bottom-rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN)-winW)/2;
    int sy = (GetSystemMetrics(SM_CYSCREEN)-winH)/2;

    HWND hwnd = CreateWindowExW(0, L"LANTimeSyncMainWnd", L"LAN Time Sync",
                                WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                                sx, sy, winW, winH,
                                NULL, NULL, hInst, NULL);
    s_hMain = hwnd;
    return hwnd;
}

int window_message_loop(HWND hwnd){
    (void)hwnd;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
