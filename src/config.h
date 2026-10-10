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

    /* ---- 桌面水印 ----
       在桌面右上角以黄色文字显示本机机器名，机房/考场里让学生一眼看出
       自己坐在哪台机器上。实现是 topmost + 点击穿透 + 不进任务栏的
       分层窗口，只挡视线、不挡操作。 */
    bool  watermark_enabled;
    char  watermark_text[160];   /* 空 = 直接用机器名；否则为模板，%COMPUTERNAME% 会被替换 */
    int   watermark_size;        /* 字高（像素）；<=0 = 按屏幕高度自动 */
    int   watermark_opacity;     /* 文字不透明度 0-255 */
    int   watermark_background;  /* 文字下方深色底衬的不透明度；0 = 纯文字无底衬 */
    bool  watermark_on_top;      /* false(默认)=贴桌面，只在桌面可见；
                                    true=永远置顶，盖在所有窗口之上 */
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
