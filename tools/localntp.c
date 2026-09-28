/* localntp.c - throwaway SNTP server used ONLY to test the client offline. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define NTP_ERA_OFFSET 2208988800ULL
/* 纪元偏移必须落在 32.32 定点的高 32 位（整秒段）。now_fixed() 返回的是
   unix 纪元的 32.32 定点值，直接加裸的 NTP_ERA_OFFSET 会把 2208988800
   加进小数段，客户端减去 NTP_ERA_OFFSET<<32 后得到约 -28 年的荒谬偏移。
   客户端 src/ntp.c 用的正是下面这个 FIXED 宏。 */
#define NTP_ERA_OFFSET_FIXED (NTP_ERA_OFFSET << 32)

static uint64_t ntoh64_(const void* p) {
    uint64_t v; memcpy(&v, p, 8);
    return (((uint64_t)ntohl((uint32_t)(v >> 32))) << 32) | ntohl((uint32_t)v);
}
static uint64_t hton64_(uint64_t v) {
    return (((uint64_t)htonl((uint32_t)(v >> 32))) << 32) | htonl((uint32_t)v);
}
static uint64_t now_fixed(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    uint64_t t = u.QuadPart - 116444736000000000ULL;
    return ((t / 10000000ULL) << 32) | ((t % 10000000ULL) * 4294967296ULL / 10000000ULL);
}

int main(int argc, char** argv) {
    /* 端口可由命令行指定：绑定 123 需要管理员权限，不提权时用
       localntp 12300 之类的端口即可在开发机上验证客户端的成功路径。
       不传参数时保持原来的 123 行为。 */
    unsigned short port = 123;
    if (argc > 1) {
        long v = strtol(argv[1], NULL, 10);
        if (v > 0 && v <= 65535) port = (unsigned short)v;
    }

    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKADDR_IN a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        if (port == 123) printf("bind UDP 123 failed (need admin)\n");
        else printf("bind UDP %u failed\n", (unsigned)port);
        return 1;
    }
    printf("local SNTP server listening on :%u (Ctrl+C to stop)\n", (unsigned)port);
    for (;;) {
        char buf[512];
        SOCKADDR_IN from; int fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
        if (n < 48) continue;
        if (((buf[0] >> 3) & 7) != 4) continue;   /* v4 */
        if ((buf[0] & 7) != 3) continue;          /* client mode */
        {
            uint64_t now = now_fixed() + NTP_ERA_OFFSET_FIXED;
            buf[0] = (unsigned char)((4u << 3) | 4u);
            buf[1] = 1;
            buf[2] = 4; buf[3] = (char)-6;
            memset(buf + 4, 0, 12);
            memcpy(buf + 16, buf + 40, 8);
            uint64_t tx = hton64_(now);
            memcpy(buf + 32, &tx, 8);
            memcpy(buf + 40, &tx, 8);
            sendto(s, buf, 48, 0, (struct sockaddr*)&from, fl);
        }
    }
    return 0;
}
