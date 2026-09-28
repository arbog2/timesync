#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */

static FILE*  g_logFile  = NULL;
static int    g_minLevel = LOG_LEVEL_INFO;
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
    EnterCriticalSection(&g_lock);
    if (g_logFile) fclose(g_logFile);
    g_logFile = nf;
    g_minLevel = minLevel;
    LeaveCriticalSection(&g_lock);
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
