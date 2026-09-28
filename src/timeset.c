#include "timeset.h"
#include "util.h"
#include <windows.h>
#include <winnt.h>

/* Enable SeSystemtimePrivilege. For normal (non-elevated) users this often
   fails at AdjustTokenPrivileges with "not held". Under SYSTEM (service) the
   privilege is present by default. */
bool timeset_enable_priv(void) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;

    LUID luid;
    bool ok = false;
    if (LookupPrivilegeValueA(NULL, "SeSystemtimePrivilege", &luid)) {
        TOKEN_PRIVILEGES tp;
        memset(&tp, 0, sizeof(tp));
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(0);
        if (AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL))
            ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(tok);
    return ok;
}

bool timeset_is_elevated(void) {
    return util_is_elevated();
}

bool timeset_system_time(int64_t unix_ms) {
    timeset_enable_priv(); /* best effort */

    /* unix ms -> FILETIME */
    ULARGE_INTEGER u;
    u.QuadPart = ((uint64_t)unix_ms + 11644473600000ULL) * 10000ULL;

    SYSTEMTIME st;
    FILETIME ft;
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    if (!FileTimeToSystemTime(&ft, &st))
        return false;

    /* local time is derived from system UTC time + tz; SetSystemTime is UTC */
    return SetSystemTime(&st) != 0;
}
