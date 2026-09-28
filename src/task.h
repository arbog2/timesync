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

/* one synchronous sync attempt. Shared by the worker loop and --once. */
bool task_sync_once(const config_t* cfg, task_state_t* st);

/* infinite scheduling loop. run until service_is_stopping() (service stop
   flag is also what tray mode sets on Exit). */
void task_run(const config_t* cfg, task_state_t* st);

#endif /* TASK_H */
