#ifndef TASK_H
#define TASK_H

#include <windows.h>
#include <stdbool.h>
#include "config.h"

/* results shared with the tray / console
   NOTE: offset/rtt/sync_sec use 64-bit (LONGLONG) so that year-scale
   jumps (e.g. BIOS 2018 vs 2026) do not overflow 32-bit LONG (~24 days). */
typedef struct {
    volatile LONGLONG last_offset_ms;
    volatile LONGLONG last_rtt_ms;
    volatile LONGLONG last_sync_sec;   /* 0 = never, else time(NULL) epoch */
    volatile LONG  synced_count;
    volatile LONG  failed_count;
    volatile BOOL  last_success;
    char           last_error[256];
} task_state_t;

/* request a sync right now (from tray thread) */
void task_request_sync(void);

/* 开关 worker 的"定时同步"。false 时 worker 只响应 task_request_sync()
   （手动"立即同步"）和启动时那一次，不再按 interval/retry 自己排期。
   GUI 在系统服务已运行时会调它，避免两个进程同时校时。 */
void task_set_periodic(bool on);

/* one synchronous sync attempt. Shared by the worker loop and --once.
   cancel 可为 NULL；非 NULL 时在每轮 NTP 采样之间求值，返回 true 则中止。 */
typedef bool (*task_cancel_fn)(void);
bool task_sync_once_c(const config_t* cfg, task_state_t* st, task_cancel_fn cancel);
/* 不带取消判断的旧入口，保留给 --once 等一次性调用方 */
bool task_sync_once(const config_t* cfg, task_state_t* st);

/* infinite scheduling loop. run until service_is_stopping() (service stop
   flag is also what tray mode sets on Exit). */
void task_run(const config_t* cfg, task_state_t* st);

#endif /* TASK_H */
