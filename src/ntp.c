#include <ws2tcpip.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "ntp.h"
#include "util.h"

/* RFC 4330 SNTP client (mode 3) over UDP. All-in-one request/response. */

/* seconds between 1900-01-01 (NTP epoch) and 1970-01-01 (unix epoch) */
#define NTP_ERA_OFFSET 2208988800ULL
#define NTP_ERA_OFFSET_FIXED (NTP_ERA_OFFSET << 32)

/* NTP packet layout (48 bytes) */
typedef struct {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    int8_t   poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint64_t ref_timestamp;          /* offset 16 */
    uint64_t originate_timestamp;    /* offset 24: client transmit */
    uint64_t receive_timestamp;      /* offset 32: server receive */
    uint64_t transmit_timestamp;     /* offset 40: server transmit */
} ntp_packet_t;

/* 32.32 fixed point: whole seconds in high 32, fraction in low 32.
   We keep values in unix seconds (1900 offset removed). */

static uint64_t now_unix_fixed(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* FILETIME: 100ns ticks since 1601-01-01. Convert to seconds since 1970. */
    uint64_t ticks = u.QuadPart - 116444736000000000ULL; /* 100ns since 1970 */
    uint64_t whole = ticks / 10000000ULL;
    uint64_t frac  = (ticks % 10000000ULL) * 4294967296ULL / 10000000ULL;
    return (whole << 32) | frac;
}

static uint64_t ntoh64(const void* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)v;
    return (((uint64_t)ntohl(lo)) << 32) | ntohl(hi);
}
static uint64_t hton64(uint64_t v) {
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)v;
    return (((uint64_t)htonl(lo)) << 32) | htonl(hi);
}

static void build_request(ntp_packet_t* p) {
    memset(p, 0, sizeof(*p));
    p->li_vn_mode = (uint8_t)((4u << 3) | 3u); /* v4, client mode 3 */
    p->poll = 4;                                /* 2^4 s */
    p->precision = -6;
    p->transmit_timestamp = now_unix_fixed() + NTP_ERA_OFFSET_FIXED;
}

static int resolve_host(const char* host, int port, SOCKADDR_IN* out) {
    out->sin_family = AF_INET;
    out->sin_port   = htons((u_short)port);

    /* 用 InetPtonA 而不是 inet_addr：后者对非法输入和合法的
       255.255.255.255 都返回 INADDR_NONE，调用方无法区分两者，
       于是广播地址会错误地落到下面的 getaddrinfo 分支（对
       "255.255.255.255" 而言那是无意义的：目标没有主机名可解析）。
       InetPtonA 以 1/0 明确表示成功与否。
       它不接受 "1.2.3" 这类简写，也不做 DNS——这正好由 getaddrinfo 兜底。 */
    if (InetPtonA(AF_INET, host, &out->sin_addr) == 1)
        return 0;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return -1;
    out->sin_addr = ((SOCKADDR_IN*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

int ntp_sync(const char* host, int port, int timeout_ms, ntp_result_t* r) {
    memset(r, 0, sizeof(*r));

    SOCKADDR_IN srv;
    if (resolve_host(host, port, &srv) != 0)
        return -4;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return -5;

    ntp_packet_t pkt;
    build_request(&pkt);
    pkt.transmit_timestamp = hton64(pkt.transmit_timestamp);

    uint64_t t0 = now_unix_fixed();              /* client send time (unix) */
    int sr = sendto(s, (const char*)&pkt, sizeof(pkt), 0,
                    (const struct sockaddr*)&srv, sizeof(srv));
    if (sr == SOCKET_ERROR) { closesocket(s); return -7; }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int sel = select(0, &fds, NULL, NULL, &tv);
    if (sel <= 0) { closesocket(s); return -2; }   /* timeout */

    char buf[512];
    int rl = recvfrom(s, buf, sizeof(buf), 0, NULL, NULL);
    if (rl < 48) { closesocket(s); return -3; }

    uint64_t t4 = now_unix_fixed();              /* client receive time */
    closesocket(s);

    uint8_t b0     = (uint8_t)buf[0];
    uint8_t li     = (uint8_t)((b0 >> 6) & 0x03);
    uint8_t vn     = (uint8_t)((b0 >> 3) & 0x07);
    uint8_t md     = (uint8_t)(b0 & 0x07);
    uint8_t stratum = (uint8_t)buf[1];

    r->li = li; r->version = vn; r->mode = md; r->stratum = stratum;

    if (li == 3)             return -8;  /* unsynchronized / kiss-o'-death */
    if (vn < 1 || vn > 4)    return -9;  /* unexpected version */
    if (stratum == 0)        return -11; /* kiss code, not real data */
    if (md != 4 && md != 5)  return -10; /* expect server or broadcast */

    /* server timestamps are 1900-epoch in network order; subtract era as 32.32 fixed */
    uint64_t t2 = ntoh64(buf + 32) - NTP_ERA_OFFSET_FIXED;  /* server receive = T2 (unix) */
    uint64_t t3 = ntoh64(buf + 40) - NTP_ERA_OFFSET_FIXED;  /* server transmit = T3 (unix) */

    /* offset = ((T2 - T1) + (T3 - T4)) / 2   (all in 32.32 fixed unix) */
    int64_t sum = (int64_t)(t2 - t0) + (int64_t)(t3 - t4);
    int64_t off = sum / 2;

    int64_t off_ms;
    if (off >= 0) {
        int64_t whole = off >> 32;
        uint64_t frac = (uint64_t)(off & 0xFFFFFFFFULL);
        off_ms = whole * 1000 + (int64_t)(frac * 1000ULL >> 32);
    } else {
        uint64_t neg = (uint64_t)(-(off + 1)) + 1ULL; // abs without UB on INT64_MIN
        int64_t whole = (int64_t)(neg >> 32);
        uint64_t frac = neg & 0xFFFFFFFFULL;
        off_ms = -(whole * 1000 + (int64_t)(frac * 1000ULL >> 32));
    }

    /* rtt = (T4 - T1) - (T3 - T2) */
    int64_t rtt = ((int64_t)(t4 - t0)) - ((int64_t)(t3 - t2));
    int64_t rtt_ms;
    if (rtt >= 0) {
        int64_t whole = rtt >> 32;
        uint64_t frac = (uint64_t)(rtt & 0xFFFFFFFFULL);
        rtt_ms = whole * 1000 + (int64_t)(frac * 1000ULL >> 32);
    } else {
        uint64_t neg = (uint64_t)(-(rtt + 1)) + 1ULL;
        int64_t whole = (int64_t)(neg >> 32);
        uint64_t frac = neg & 0xFFFFFFFFULL;
        rtt_ms = -(whole * 1000 + (int64_t)(frac * 1000ULL >> 32));
    }
    if (rtt_ms < 0) rtt_ms = 0;

    r->offset_ms = off_ms;
    r->rtt_ms    = rtt_ms;
    r->ok = true;
    return 0;
}

/* take the lowest-RTT sample from a short burst */
int ntp_sync_best(const char* host, int port, int timeout_ms,
                  int samples, ntp_result_t* out) {
    memset(out, 0, sizeof(*out));
    if (samples < 1) samples = 1;
    int best = INT32_MAX;
    int last_rc = -1; /* remember the last failure for the caller */
    for (int i = 0; i < samples; i++) {
        ntp_result_t r;
        int rc = ntp_sync(host, port, timeout_ms, &r);
        if (rc != 0) { last_rc = rc; continue; }
        if (r.ok) {
            if (r.rtt_ms < best) {
                best = (int)r.rtt_ms;
                *out = r;
            }
            if (r.rtt_ms <= 2) break;
        }
    }
    return (best == INT32_MAX) ? last_rc : 0;
}
