#ifndef NTP_H
#define NTP_H

/* win32 sockets: must include winsock2.h before windows.h (windows.h would
   otherwise pull in winsock1 and the struct timeval used by select()). */
#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  li;    /* leap indicator */
    uint8_t  version;
    uint8_t  mode;  /* 3 = client, 4 = server */
    uint8_t  stratum;
    int64_t  rtt_ms;   /* round trip, valid if ok */
    int64_t  offset_ms;/* local clock error: server - local (ms) */
    bool     ok;
} ntp_result_t;

/* send a single SNTP client request and wait for reply.
   Returns 0 on success with r->ok true. */
int ntp_sync(const char* host, int port, int timeout_ms, ntp_result_t* r);

/* roundtrip-averaged two-sample sync; returns 0 if any sample ok.
   cancel：可选的中止判断，每个采样之间调用；返回 true 时立刻停止后续采样并
   返回错误码 -99。传 NULL 表示不可取消。
   为什么需要它：一次同步最坏 3 × timeout_ms(最大 10s) = 30s，而 SCM 的
   SERVICE_CONTROL_SHUTDOWN 时限也就 30s 上下。没有采样间的检查点，
   服务停止/退出时 worker 会被硬杀在一轮同步的中间。 */
#define NTP_ERR_CANCELLED (-99)
typedef bool (*ntp_cancel_fn)(void);
int ntp_sync_best(const char* host, int port, int timeout_ms,
                  int samples, ntp_cancel_fn cancel, ntp_result_t* out);

#endif /* NTP_H */
