#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char  server[128];       /* ntp server host or ip */
    int   port;
    int   interval_minutes;  /* regular resync interval */
    int   retry_seconds;     /* retry after a failed sync */
    int   timeout_ms;        /* udp rtt timeout */
    int   max_offset_ms;     /* offset beyond this -> correct; else skip */
    int   min_offset_ms;     /* offset below this -> skip set (hysteresis) */
    bool  force_enabled;     /* master switch from config */
    bool  force_flag;        /* true -> always SetSystemTime even if small */
    bool  service_enabled;   /* run as windows service instead of tray */
    bool  sync_on_start;     /* do an immediate sync at startup */
    bool  allow_large_offset;/* true -> allow year-scale jumps (bypass 24h sanity) */
    char  log_file[MAX_PATH];
    bool  log_enabled;
    int   log_level;
} config_t;

/* defaults for missing keys */
void config_init_defaults(config_t* c);
/* path of the ini file (ProgramData\LANTimeSync\config.ini) */
void config_default_path(char* out, size_t cap);
/* parse ini file; returns true on success. file may be absent -> defaults. */
bool config_load(config_t* c, const char* path);

/* write a commented sample config (used by an install helper) */
void config_write_sample(const char* path);
/* save current config to path (atomic via .tmp + MoveFileEx) */
bool config_save(const config_t* c, const char* path);
/* wide-path variant (no ACP/UTF-8 loss); content bytes identical to config_save */
bool config_save_w(const config_t* c, const WCHAR* wpath);

#endif /* CONFIG_H */
