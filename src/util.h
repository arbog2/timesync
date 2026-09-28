#ifndef UTIL_H
#define UTIL_H

/* windows.h first so timeval/winsock conflicts don't arise; windows.h
   defines its own, winsock2 must be included before windows.h if used.
   config.h -> windows.h is fine. */
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------- logging ---------- */
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

void util_log_init(const char* logFile, int minLevel);
void util_log_set_min_level(int level);
void util_logf(int level, const char* fmt, ...);

#define LOG_DEBUG(...) util_logf(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  util_logf(LOG_LEVEL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  util_logf(LOG_LEVEL_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) util_logf(LOG_LEVEL_ERROR, __VA_ARGS__)

/* single instance lock via named mutex; returns NULL if acquired else existing handle */
HANDLE util_single_instance(const char* name);

/* ---------- privilege / architecture probes ----------
   这些探针此前在 service.c / startup.c / ntpserver.c / timeset.c 里各存一份
   几乎逐字相同的实现（4 份 is_elevated、2 份 is_wow64、2 份 System32 路径拼接）。
   统一收敛到此处，各模块只保留自己的语义包装。 */

/* 当前进程是否已提权（TokenElevation）。非提权但持有管理员凭据时返回 false。 */
bool util_is_elevated(void);

/* 32 位进程是否运行在 64 位系统上（WOW64）。用 GetProcAddress 动态取
   IsWow64Process，Win7 起可用，不产生 kernel32 的静态导入。 */
bool util_is_wow64(void);

/* GetWindowsDirectoryW，失败或被截断时回退 "C:\\Windows"。 */
void util_get_windows_dir(WCHAR* out, size_t cap);

/* 拼出 %WINDIR%\<System32|Sysnative>\<leaf> 的完整路径。
   32 位进程在 64 位系统上访问 System32 会被重定向到 SysWOW64，
   里面的 netsh.exe / schtasks.exe 是 32 位的，对 64 位 SCM 会失败，
   因此必须走 Sysnative（仅 32 位可见）。 */
void util_build_sys_bin_path(const WCHAR* leaf, WCHAR* out, size_t cap);

/* windows FILETIME helpers */
uint64_t util_filetime_to_unix_ms(const FILETIME* ft);
void     util_unix_ms_to_filetime(uint64_t ms, FILETIME* ft);

#endif /* UTIL_H */
