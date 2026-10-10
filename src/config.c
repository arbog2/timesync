#include "config.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

void config_init_defaults(config_t* c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->server, "127.0.0.1");
    c->port            = 123;
    c->interval_minutes = 60;
    c->retry_seconds   = 10;
    c->timeout_ms      = 3000;
    c->max_offset_ms   = 5000;
    c->min_offset_ms   = 200;
    c->force_enabled   = true;
    c->force_flag      = true;
    c->service_enabled = false;
    c->sync_on_start   = true;
    c->allow_large_offset = true; /* default: allow year-scale jumps (forced sync) */
    c->log_enabled     = true;
    c->log_level       = LOG_LEVEL_INFO;
    strcpy(c->log_file, "C:\\ProgramData\\LANTimeSync\\time_sync.log");

    /* 桌面水印默认开启：这个功能的用途就是"装完就有"，学生一登录就能看到
       自己机器的名字，不需要老师逐台去开。不想用的人可以在界面上取消勾选，
       或把 [watermark] enabled 改成 false。 */
    c->watermark_enabled    = true;
    c->watermark_text[0]    = '\0';   /* 空 = 用本机机器名 */
    c->watermark_size       = 0;      /* 0 = 按屏幕高度自动 */
    c->watermark_opacity    = 210;    /* 约 82%：够醒目，又不糊住桌面 */
    c->watermark_background = 0;      /* 纯黄字，不加底衬 */
    /* 贴桌面而不是置顶：置顶会盖在打开的软件上挡视线，
       而水印的用途只是"让人知道自己在哪台机器上"。 */
    c->watermark_on_top     = false;
}

void config_default_path(char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    /* GetEnvironmentVariableA 在缓冲区不够时返回需要的字节数且不保证
       写入 NUL，紧接着 strlen(out) 会越界读。显式判返回值。 */
    DWORD n = GetEnvironmentVariableA("ProgramData", out, (DWORD)cap);
    if (n == 0 || n >= cap) {
        strncpy(out, "C:\\ProgramData", cap - 1);
        out[cap - 1] = '\0';
    }
    size_t len = strlen(out);
    if (len + 2 < cap)
        _snprintf(out + len, cap - len, "\\LANTimeSync\\config.ini");
    out[cap - 1] = '\0';
}

/* remove whitespace from both ends of s in place */
static void trim(char* s) {
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/* strip trailing ; or # comment outside quotes (keeps ; inside "..." or '...') */
static void strip_inline_comment(char* s) {
    bool in_s = false, in_d = false;
    for (char* p = s; *p; p++) {
        if (!in_s && !in_d && (*p == ';' || *p == '#')) { *p = '\0'; return; }
        else if (*p == '"' && !in_s) in_d = !in_d;
        else if (*p == '\'' && !in_d) in_s = !in_s;
    }
}

/* parse "x=y" with optional surrounding spaces; returns 1 if ok */
static bool parse_kv(const char* line, char* key, size_t kc, char* val, size_t vc) {
    const char* eq = strchr(line, '=');
    if (!eq) return false;
    size_t kl = (size_t)(eq - line);
    if (kl == 0 || kl >= kc) return false;
    memcpy(key, line, kl);
    key[kl] = '\0';
    trim(key);
    strncpy(val, eq + 1, vc);
    val[vc - 1] = '\0';
    trim(val);
    strip_inline_comment(val);
    trim(val);
    /* strip surrounding quotes if present */
    size_t vl = strlen(val);
    if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') ||
                    (val[0] == '\'' && val[vl - 1] == '\''))) {
        memmove(val, val + 1, vl - 2);
        val[vl - 2] = '\0';
    }
    return key[0] != '\0';
}

/* set integer field. strtol 会吃掉前导数字并把 end 指向首个非数字字符，
   所以 "60s" / "2000ms" 这类带单位后缀的值会被解析成 60 / 2000，
   后缀被静默忽略（不会报错，也不会保留）。
   各键的合法区间在这里就地钳制，越界的值一律丢弃、保留原值。 */
static void set_int(int* dst, const char* val, const char* key) {
    char* end;
    long v = strtol(val, &end, 10);
    if (end == val) return; /* not a number */
    if (_stricmp(key, "interval_minutes") == 0) {
        if (v > 0 && v <= 24 * 60) *dst = (int)v;
    } else if (_stricmp(key, "retry_seconds") == 0) {
        if (v >= 1 && v <= 3600) *dst = (int)v;
    } else if (_stricmp(key, "timeout_ms") == 0) {
        if (v >= 100 && v <= 10000) *dst = (int)v;
    } else if (_stricmp(key, "max_offset_ms") == 0) {
        if (v >= 0) *dst = (int)v;
    } else if (_stricmp(key, "min_offset_ms") == 0) {
        if (v >= 0) *dst = (int)v;
    } else if (_stricmp(key, "port") == 0) {
        if (v >= 1 && v <= 65535) *dst = (int)v;
    } else if (_stricmp(key, "watermark_size") == 0) {
        /* 0 表示自动；上限 400 像素，再大就不是水印而是横幅了 */
        if (v >= 0 && v <= 400) *dst = (int)v;
    } else if (_stricmp(key, "watermark_opacity") == 0 ||
               _stricmp(key, "watermark_background") == 0) {
        if (v >= 0 && v <= 255) *dst = (int)v;
    } else {
        /* generic untyped */
        *dst = (int)v;
    }
}

static void set_bool(bool* dst, const char* val) {
    if (_stricmp(val, "true") == 0 || strcmp(val, "1") == 0)
        *dst = true;
    else if (_stricmp(val, "false") == 0 || strcmp(val, "0") == 0)
        *dst = false;
}

void config_write_sample(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "; LAN Time Sync configuration\n"
        "; copy to %%ProgramData%%\\LANTimeSync\\config.ini\n"
        "\n"
        "[server]\n"
        "address = \"192.168.1.100\"   ; NTP/SNTP server in the LAN\n"
        "port = 123\n"
        "\n"
        "[sync]\n"
        "interval_minutes = 60         ; normal re-check period\n"
        "retry_seconds = 10            ; retry delay after a failed sync\n"
        "timeout_ms = 3000             ; per-request timeout\n"
        "max_offset_ms = 5000          ; drift above this forces a correction\n"
        "min_offset_ms = 200           ; drift below this is ignored\n"
        "force_enabled = true          ; master switch\n"
        "force_flag = true             ; aggressive: also step medium drift\n"
        "sync_on_start = true\n"
        "allow_large_offset = true     ; true=allow year-scale jumps, false=24h limit\n"
        "\n"
        "[service]\n"
        "; true = register & run as a Windows service (SYSTEM)\n"
        "; false = run as a normal tray process (task scheduler / manual)\n"
        "enabled = false\n"
        "\n"
        "[log]\n"
        "file_enabled = true\n"
        "file_path = \"C:\\\\ProgramData\\\\LANTimeSync\\\\time_sync.log\"\n"
        "level = \"info\"               ; debug | info | warn | error\n"
        "\n"
        "[watermark]\n"
        "; 桌面右上角的黄色水印，内容默认取本机机器名\n"
        "enabled = true\n"
        "text =                        ; 留空=机器名；也可写 \"机房A-%%COMPUTERNAME%%\"\n"
        "size = 0                      ; 字高像素，0=按分辨率自动\n"
        "opacity = 210                 ; 文字不透明度 0-255\n"
        "background = 0                ; 文字下深色底衬的不透明度，0=纯文字\n"
        "layer = desktop               ; desktop=贴桌面(在图标上、所有窗口下)；top=永远置顶\n");
    fclose(f);
}

bool config_load(config_t* c, const char* path) {
    config_init_defaults(c);
    FILE* f = fopen(path, "r");
    if (!f) return false; /* absent -> defaults */

    char section[64] = { 0 };
    char line[1024];
    bool firstLine = true;
    while (fgets(line, sizeof(line), f)) {
        /* strip CR/LF */
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        /* 首行可能是 UTF-8 BOM（记事本"另存为 UTF-8"会写入 EF BB BF）。
           不去掉的话，第一行会变成 "\xEF\xBB\xBF[server]"，
           既匹配不上 '[' 开头的段头判断，也解析不出键值，
           结果 [server] 段被整体丢弃、服务器地址静默回退为 127.0.0.1。 */
        char* t = line;
        if (firstLine) {
            firstLine = false;
            if ((unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB &&
                (unsigned char)t[2] == 0xBF)
                t += 3;
        }

        while (*t && isspace((unsigned char)*t)) t++;
        if (*t == '\0' || *t == '#' || *t == ';') continue;

        if (*t == '[') {
            char* end = strchr(t, ']');
            if (end) {
                *end = '\0';
                trim(t + 1);
                strncpy(section, t + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        char key[64], val[512];
        if (!parse_kv(t, key, sizeof(key), val, sizeof(val))) continue;

        if (strcmp(section, "server") == 0) {
            if (strcmp(key, "address") == 0) {
                strncpy(c->server, val, sizeof(c->server) - 1);
                c->server[sizeof(c->server) - 1] = '\0';
            } else if (strcmp(key, "port") == 0) {
                set_int(&c->port, val, key);
            }
        } else if (strcmp(section, "sync") == 0) {
            if (strcmp(key, "interval_minutes") == 0)
                set_int(&c->interval_minutes, val, key);
            else if (strcmp(key, "retry_seconds") == 0)
                set_int(&c->retry_seconds, val, key);
            else if (strcmp(key, "timeout_ms") == 0)
                set_int(&c->timeout_ms, val, key);
            else if (strcmp(key, "max_offset_ms") == 0)
                set_int(&c->max_offset_ms, val, key);
            else if (strcmp(key, "min_offset_ms") == 0)
                set_int(&c->min_offset_ms, val, key);
            else if (strcmp(key, "force_enabled") == 0)
                set_bool(&c->force_enabled, val);
            else if (strcmp(key, "force_flag") == 0)
                set_bool(&c->force_flag, val);
            else if (strcmp(key, "sync_on_start") == 0)
                set_bool(&c->sync_on_start, val);
            else if (strcmp(key, "allow_large_offset") == 0)
                set_bool(&c->allow_large_offset, val);
        } else if (strcmp(section, "service") == 0) {
            if (strcmp(key, "enabled") == 0)
                set_bool(&c->service_enabled, val);
        } else if (strcmp(section, "log") == 0) {
            if (strcmp(key, "file_enabled") == 0)
                set_bool(&c->log_enabled, val);
            else if (strcmp(key, "file_path") == 0) {
                strncpy(c->log_file, val, sizeof(c->log_file) - 1);
                c->log_file[sizeof(c->log_file) - 1] = '\0';
            } else if (strcmp(key, "level") == 0) {
                if (_stricmp(val, "debug") == 0)      c->log_level = LOG_LEVEL_DEBUG;
                else if (_stricmp(val, "info") == 0)  c->log_level = LOG_LEVEL_INFO;
                else if (_stricmp(val, "warn") == 0)  c->log_level = LOG_LEVEL_WARN;
                else if (_stricmp(val, "error") == 0) c->log_level = LOG_LEVEL_ERROR;
            }
        } else if (strcmp(section, "watermark") == 0) {
            if (strcmp(key, "enabled") == 0)
                set_bool(&c->watermark_enabled, val);
            else if (strcmp(key, "text") == 0) {
                strncpy(c->watermark_text, val, sizeof(c->watermark_text) - 1);
                c->watermark_text[sizeof(c->watermark_text) - 1] = '\0';
            }
            else if (strcmp(key, "size") == 0)
                set_int(&c->watermark_size, val, "watermark_size");
            else if (strcmp(key, "opacity") == 0)
                set_int(&c->watermark_opacity, val, "watermark_opacity");
            else if (strcmp(key, "background") == 0)
                set_int(&c->watermark_background, val, "watermark_background");
            else if (strcmp(key, "layer") == 0) {
                /* desktop（默认）贴桌面；top 永远置顶。
                   认不出的值保持默认，不猜。 */
                if (_stricmp(val, "desktop") == 0)      c->watermark_on_top = false;
                else if (_stricmp(val, "top") == 0)     c->watermark_on_top = true;
            }
        }
        /* unknown sections/keys ignored */
    }
    fclose(f);
    return true;
}
