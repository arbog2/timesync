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

/* windows FILETIME helpers */
uint64_t util_filetime_to_unix_ms(const FILETIME* ft);
void     util_unix_ms_to_filetime(uint64_t ms, FILETIME* ft);

#endif /* UTIL_H */
