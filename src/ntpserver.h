#ifndef NTPSERVER_H
#define NTPSERVER_H

#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NTP_UNKNOWN = 0,
    NTP_DISABLED,
    NTP_ENABLED_RUNNING,
    NTP_ENABLED_STOPPED,
    NTP_ENABLED_FIREWALL_BLOCKED,
} ntp_state_t;

const char*  ntp_state_name(ntp_state_t s);
bool         ntp_is_elevated(void);
ntp_state_t  ntp_query_state(void);
bool         ntp_enable(void);   // 包含：写注册表+设自启+放行防火墙+可靠宣告
bool         ntp_disable(void);  // 关闭宣告并移除防火墙规则

#ifdef __cplusplus
}
#endif

#endif
