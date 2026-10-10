#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <aclapi.h>
#include <sddl.h>

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */

static FILE*  g_logFile  = NULL;
static int    g_minLevel = LOG_LEVEL_INFO;
static volatile LONG g_logOpenFailed = 0;   /* 配了日志却打不开 */
static CRITICAL_SECTION g_lock;

static void log_lock_init(void) {
    static volatile LONG inited = 0;
    if (InterlockedCompareExchange(&inited, 1, 0) == 0)
        InitializeCriticalSectionAndSpinCount(&g_lock, 0x400);
}

/* create missing parent directories for path (recursive mkdir). */
static void ensure_parent_dir(const char* path) {
    char tmp[MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char* slash = strrchr(tmp, '\\');
    char* fslash = strrchr(tmp, '/');
    /* 两个 strrchr 都可能返回 NULL；直接比较指针大小是未定义行为。
       这里只在两者都非 NULL 时比较（同属一个数组，比较合法），
       保持"取最后一个分隔符"的原有语义。 */
    char* sep;
    if (slash && fslash) sep = (slash > fslash) ? slash : fslash;
    else                 sep = slash ? slash : fslash;
    if (!sep) return;
    *sep = '\0';
    size_t skip = 0;
    if (tmp[0] == '\\' && tmp[1] == '\\') {
        /* UNC: keep \\\\server\\share, create only beneath it */
        char* p = tmp + 2;
        char* s1 = strchr(p, '\\');
        if (!s1) return;
        char* s2 = strchr(s1 + 1, '\\');
        if (!s2) return;
        skip = (size_t)(s2 - tmp);
    } else if (tmp[1] == ':' && (tmp[2] == '\\' || tmp[2] == '/' || tmp[2] == '\0')) {
        skip = 3; /* "C:\\" already exists */
    }
    if (strlen(tmp) <= skip) return;
    for (char* p = tmp + skip + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char save = *p;
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = save;
        }
    }
    CreateDirectoryA(tmp, NULL);
}

void util_log_init(const char* logFile, int minLevel) {
    log_lock_init();
    if (logFile && logFile[0]) {
        /* open in append; create missing directories first */
        ensure_parent_dir(logFile);
    }
    /* 关旧句柄与换新句柄必须在锁内完成：worker 线程随时可能在 util_logf 里
       读 g_logFile，锁外赋值会撞上 fclose 之后的悬空 FILE*（UI 线程"应用并保存"
       时正好会发生）。文件打开放在锁外，避免持锁做 I/O。 */
    FILE* nf = (logFile && logFile[0]) ? fopen(logFile, "a") : NULL;
    if (logFile && logFile[0] && !nf) {
        /* 打不开就不再静默：记一条 OutputDebugString（此时还没有日志可写），
           并置标志让 GUI 的"查看日志"能把这个失败说出来。 */
        OutputDebugStringA("LANTimeSync: cannot open log file (permission/disk?): ");
        OutputDebugStringA(logFile);
        InterlockedExchange(&g_logOpenFailed, 1);
    } else if (nf) {
        InterlockedExchange(&g_logOpenFailed, 0);
    }
    EnterCriticalSection(&g_lock);
    if (g_logFile) fclose(g_logFile);
    g_logFile = nf;
    g_minLevel = minLevel;
    LeaveCriticalSection(&g_lock);
}

bool util_log_active(void) {
    return InterlockedCompareExchange(&g_logOpenFailed, 0, 0) == 0;
}

void util_log_set_min_level(int level) {
    g_minLevel = level;
}

void util_logf(int level, const char* fmt, ...) {
    static const char* lv[] = { "D", "I", "W", "E" };
    if (level < g_minLevel)
        return;
    log_lock_init();
    SYSTEMTIME st;
    GetLocalTime(&st);

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;

    int li = level;
    if (li < 0) li = 0;
    if (li > LOG_LEVEL_ERROR) li = LOG_LEVEL_ERROR;

    EnterCriticalSection(&g_lock);

    if (g_logFile) {
        fprintf(g_logFile, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] %.*s\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                lv[li], n, buf);
        fflush(g_logFile);
    }

    /* also surface to debugger for dev */
    if (IsDebuggerPresent()) {
        char line[2300];
        _snprintf(line, sizeof(line),
                  "[%02d:%02d:%02d.%03d] [%s] %.*s\n",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  lv[li], n, buf);
        OutputDebugStringA(line);
    }

    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------------ */
/* shared state lock                                                  */
/* ------------------------------------------------------------------ */

static CRITICAL_SECTION g_stateLock;
static volatile LONG    g_stateLockInited = 0;

/* 惰性初始化：util.c 没有构造函数，多个翻译单元在静态初始化阶段都可能引用它，
   用 InterlockedCompareExchange 保证只 InitializeCriticalSection 一次。 */
static void state_lock_init(void) {
    if (InterlockedCompareExchange(&g_stateLockInited, 1, 0) == 0)
        InitializeCriticalSectionAndSpinCount(&g_stateLock, 0x400);
}

void util_state_lock(void) {
    state_lock_init();
    EnterCriticalSection(&g_stateLock);
}

void util_state_unlock(void) {
    LeaveCriticalSection(&g_stateLock);
}

/* ------------------------------------------------------------------ */
/* program directory ACL                                              */
/* ------------------------------------------------------------------ */

/* SDDL：P = 受保护的 DACL（切断 ProgramData 的继承，否则继承来的 RX 会把
   下面这条 Users 条目盖掉）。四条 ACE 覆盖全部需要访问的身份：
   BA = Administrators(F)，SY = SYSTEM(F)，CO = 创建者所有者(F)，
   BU = BUILTIN\Users(GRGWGX：泛读 + 泛写 + 泛执行)。
   ConvertStringSecurityDescriptorToSecurityDescriptorW 自 XP 起可用，Win7 无虞。 */
#define LANTS_SDDL L"D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FA;;;CO)(A;OICI;GRGWGX;;;BU)"

static void grant_users_modify(const WCHAR* dirW) {
    if (!dirW || !dirW[0]) return;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            LANTS_SDDL, SDDL_REVISION_1, &sd, NULL))
        return;
    /* PSECURITY_DESCRIPTOR 是自描述的安全描述符（Win32 里表现为一个
       不透明的指针），没有公开的结构体定义，不能直接 ->Dacl；
       必须用 GetSecurityDescriptorDacl 从里面取出 DACL 指针。 */
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !dacl) {
        LocalFree(sd);
        return;
    }
    /* 整条 DACL 覆盖式设置：幂等，重复调用结果一致 */
    DWORD rc = SetNamedSecurityInfoW((LPWSTR)dirW, SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION,
                                     NULL, NULL, dacl, NULL);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS)
        LOG_DEBUG("SetNamedSecurityInfo(%ls) rc=%lu (ignored)", dirW, rc);
}

void util_ensure_program_dir(void) {
    char prog[MAX_PATH] = "";
    if (!GetEnvironmentVariableA("ProgramData", prog, sizeof(prog)))
        return;
    char dir[MAX_PATH + 32] = "";
    _snprintf(dir, sizeof(dir), "%s\\LANTimeSync", prog);
    dir[sizeof(dir) - 1] = '\0';

    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) {
        ensure_parent_dir(dir);        /* 递归建父目录 */
        CreateDirectoryA(dir, NULL);
    }
    /* 无论刚才新建的还是已存在的，都把 DACL 设一遍。
       目录可能由 SYSTEM（开机先跑的服务）或 /RL HIGHEST 任务先建好，
       那种情况下属主是 SYSTEM、普通用户写不进去——这正是要修的静默失败。 */
    WCHAR dirW[MAX_PATH] = L"";
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, dirW, MAX_PATH) > 0)
        grant_users_modify(dirW);
}

/* ------------------------------------------------------------------ */
/* single instance                                                    */
/* ------------------------------------------------------------------ */

HANDLE util_single_instance(const char* name) {
    HANDLE h = CreateMutexA(NULL, TRUE, name);
    if (h && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return NULL;
    }
    return h; /* keep open for process lifetime */
}

/* ------------------------------------------------------------------ */
/* privilege / architecture probes                                    */
/* ------------------------------------------------------------------ */

/* IsWow64Process 在 Win7 起由 kernel32 导出，但为了不把它变成静态导入项
   （它只在本模块用一次），这里照旧走 GetProcAddress。 */
bool util_is_wow64(void) {
    BOOL wow = FALSE;
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (h) {
        typedef BOOL (WINAPI *PFN_IsWow64)(HANDLE, PBOOL);
        PFN_IsWow64 p = (PFN_IsWow64)GetProcAddress(h, "IsWow64Process");
        if (p) p(GetCurrentProcess(), &wow);
    }
    return wow != FALSE;
}

bool util_is_elevated(void) {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el;
    DWORD ret = 0;
    BOOL elevated = FALSE;
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &ret))
        elevated = el.TokenIsElevated;
    CloseHandle(tok);
    return elevated != FALSE;
}

void util_get_windows_dir(WCHAR* out, size_t cap) {
    if (!out || cap == 0) return;
    UINT n = GetWindowsDirectoryW(out, (DWORD)cap);
    if (n == 0 || n >= cap) wcscpy(out, L"C:\\Windows");
}

void util_build_sys_bin_path(const WCHAR* leaf, WCHAR* out, size_t cap) {
    if (!out || cap == 0) return;
    WCHAR win[MAX_PATH] = L"";
    util_get_windows_dir(win, MAX_PATH);

    /* 只有 WOW64 时才可能需要 Sysnative。探针失败（属性读不到）时按"没有
       Sysnative"处理即可：32 位纯 32 位系统上拼接出 Sysnative 路径会得到
       ERROR_PATH_NOT_FOUND，两处失败分支都会收敛到 System32。 */
    bool useSysnative = false;
    if (util_is_wow64()) {
        WCHAR probe[MAX_PATH] = L"";
        _snwprintf(probe, MAX_PATH, L"%s\\Sysnative", win);
        probe[MAX_PATH - 1] = L'\0';
        if (!(GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES &&
              GetLastError() == ERROR_PATH_NOT_FOUND))
            useSysnative = true;
    }
    /* 用 bool 标志而不是比较字符串字面量地址（后者依赖字面量合并，/W4 报 C4130） */
    const WCHAR* sys = useSysnative ? L"Sysnative" : L"System32";
    _snwprintf(out, cap, L"%s\\%s\\%s", win, sys, leaf);
    out[cap - 1] = L'\0';
}

/* ------------------------------------------------------------------ */
/* time helpers                                                       */
/* ------------------------------------------------------------------ */

/* FILETIME epoch: 1601-01-01; unix epoch: 1970-01-01 -> 11644473600 s */
uint64_t util_filetime_to_unix_ms(const FILETIME* ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (u.QuadPart / 10000ULL) - 11644473600000ULL;
}

void util_unix_ms_to_filetime(uint64_t ms, FILETIME* ft) {
    ULARGE_INTEGER u;
    u.QuadPart = (ms + 11644473600000ULL) * 10000ULL;
    ft->dwLowDateTime  = u.LowPart;
    ft->dwHighDateTime = u.HighPart;
}
