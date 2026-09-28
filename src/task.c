#include "task.h"
#include "ntp.h"
#include "timeset.h"
#include "util.h"
#include "service.h"
#include <stdio.h>
#include <time.h>

static volatile LONG g_want_sync = 0;

void task_request_sync(void) {
    InterlockedExchange(&g_want_sync, 1);
}

/* ---- one synchronous sync attempt (shared by loop, tray "sync now", --once) ----
   returns true if the clock was checked+corrected without fatal error. */
bool task_sync_once(const config_t* cfg, task_state_t* st) {
    LOG_INFO("sending SNTP request to %s:%d ...", cfg->server, cfg->port);
    ntp_result_t r;
    int rc = ntp_sync_best(cfg->server, cfg->port, cfg->timeout_ms, 3, &r);
    if (rc != 0) {
        const char* why = (rc == -2)  ? "timeout"
                        : (rc == -3)  ? "short reply"
                        : (rc == -4)  ? "resolve failed"
                        : (rc == -5)  ? "socket failed"
                        : (rc == -7)  ? "send failed"
                        : (rc == -8)  ? "server unsynchronized"
                        : (rc == -9)  ? "bad protocol version"
                        : (rc == -11) ? "server kiss-o'-death"
                        : "ntp error";
        snprintf(st->last_error, sizeof(st->last_error), "%s", why);
        LOG_WARN("SNTP failed rc=%d (%s)", rc, why);
        InterlockedIncrement(&st->failed_count);
        st->last_success = FALSE;
        return false;
    }

    InterlockedExchange64(&st->last_offset_ms, r.offset_ms);
    InterlockedExchange64(&st->last_rtt_ms,    r.rtt_ms);
    LOG_INFO("offset=%lld ms  rtt=%lld ms  stratum=%u",
             (long long)r.offset_ms, (long long)r.rtt_ms, r.stratum);

    int64_t off = r.offset_ms;
    /* 用无符号求绝对值：off 可能为 INT64_MIN（服务器时间戳未做范围校验，
       畸形/恶意响应可以构造出来），-off 是有符号溢出（UB）。 */
    uint64_t abs_off = (off < 0) ? ((uint64_t)(-(off + 1)) + 1ULL) : (uint64_t)off;

    bool need_set = cfg->force_flag
                        ? (abs_off > (uint64_t)cfg->min_offset_ms)   /* aggressive */
                        : (abs_off > (uint64_t)cfg->max_offset_ms);  /* conservative */

    if (!cfg->force_enabled)
        need_set = false;      /* master switch off: measure only, never step */

    if (!need_set) {
        if (!cfg->force_enabled)
            LOG_INFO("force_enabled=false; offset %lld ms measured only, clock left as-is",
                     (long long)off);
        else
            LOG_INFO("offset %lld ms within tolerance; clock left as-is",
                     (long long)off);
        InterlockedIncrement(&st->synced_count);
        st->last_success = TRUE;
        InterlockedExchange64(&st->last_sync_sec, (LONGLONG)time(NULL));
        st->last_error[0] = '\0';
        return true;
    }

    /* sanity: if offset is implausibly huge the SNTP packet is corrupt;
       refuse to move the clock by days unless explicitly allowed.
       allow_large_offset=true (forced sync) bypasses the 24h limit
       so that BIOS resets / year-scale drift can be corrected. */
    if (!cfg->allow_large_offset && abs_off > 86400000) { /* > 24 h */
        snprintf(st->last_error, sizeof(st->last_error),
                 "offset %lld ms exceeds 24h limit; packet rejected (enable allow_large_offset to force)",
                 (long long)off);
        LOG_WARN("%s", st->last_error);
        InterlockedIncrement(&st->failed_count);
        st->last_success = FALSE;
        return false;
    }
    /* allow_large_offset 绕过 24h 限制是给 BIOS 归零/跨年漂移用的，
       但时间戳未经校验，畸形响应仍可给出接近 INT64_MIN/MAX 的偏移，
       令下面 now_ms + off 有符号溢出。这里用一个远超任何真实时钟偏差的
       上界（约 100 年）兜底拒绝。 */
    if (abs_off > 3155760000000ULL) { /* ~100 years in ms */
        snprintf(st->last_error, sizeof(st->last_error),
                 "offset %lld ms is implausible; packet rejected",
                 (long long)off);
        LOG_WARN("%s", st->last_error);
        InterlockedIncrement(&st->failed_count);
        st->last_success = FALSE;
        return false;
    }

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    int64_t now_ms = (int64_t)util_filetime_to_unix_ms(&now);
    int64_t target_ms = now_ms + off;

    LOG_INFO("stepping clock by %lld ms to %lld",
             (long long)off, (long long)target_ms);
    if (timeset_system_time(target_ms)) {
        LOG_INFO("system clock corrected");
        InterlockedIncrement(&st->synced_count);
        st->last_success = TRUE;
        InterlockedExchange64(&st->last_sync_sec, (LONGLONG)time(NULL));
        st->last_error[0] = '\0';
        return true;
    }
    snprintf(st->last_error, sizeof(st->last_error),
             "SetSystemTime failed (need SeSystemtimePrivilege? err=%lu)",
             GetLastError());
    LOG_ERROR("%s", st->last_error);
    InterlockedIncrement(&st->failed_count);
    st->last_success = FALSE;
    return false;
}

/* ---- infinite scheduling loop, shared by service and tray modes ----
   The loop exits when service_is_stopping() becomes true (SCM stop,
   tray "exit", or --once single pass via the call_once below). */
void task_run(const config_t* cfg, task_state_t* st) {
    int64_t now0       = (int64_t)time(NULL);
    int64_t next_sync  = cfg->sync_on_start ? 0 : now0 + (int64_t)cfg->interval_minutes * 60;
    int64_t next_retry = INT64_MAX;
    bool    first      = true;

    while (!service_is_stopping()) {
        int64_t now = (int64_t)time(NULL);

        bool should_sync = false;
        if (first) { should_sync = cfg->sync_on_start; first = false; }
        if (now >= next_sync)    should_sync = true;
        if (now >= next_retry)   should_sync = true;
        /* 取出并清除"请求立即同步"标志：InterlockedExchange 读旧值、
           无条件写 0，返回值非 0 即表示此前有挂起的请求。
           原先写的是 InterlockedCompareExchange(&g, 0, 1)：它只在旧值
           等于 1 时才写 0，旧值为 0 时反而写入 1——于是每轮 1 秒的循环
           都会把标志重新置 1，下一轮立刻又满足条件，语义虽能凑效但
           意图完全被掩盖。 */
        if (InterlockedExchange(&g_want_sync, 0) == 1)
            should_sync = true;

        if (should_sync) {
            bool ok = task_sync_once(cfg, st);
            /* task_sync_once 可能阻塞数秒（3 次采样 × timeout_ms），
               必须用返回后的时间重新计算下一次计划，否则周期会短掉
               整个请求耗时（stale `now`）。 */
            int64_t done = (int64_t)time(NULL);
            if (ok) {
                next_sync  = done + (int64_t)cfg->interval_minutes * 60;
                next_retry = INT64_MAX;
            } else {
                next_retry = done + cfg->retry_seconds;
                if (next_sync < done) next_sync = done + (int64_t)cfg->interval_minutes * 60;
            }
        }
        Sleep(1000);
    }
    LOG_INFO("sync worker stopped");
}
