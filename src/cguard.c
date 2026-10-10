#define UNICODE
#define _UNICODE
#include "cguard.h"
#include "util.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CGUARD_LASTGOOD_NAME "config.lastgood.ini"
#define CGUARD_CHECK_MS      30000    /* 检查间隔 */
#define CGUARD_GRACE_MS      180000   /* 合法外部修改的宽限期：3 分钟 */

static char     s_cfgPath[MAX_PATH]     = "";
static char     s_lastGoodPath[MAX_PATH] = "";
static BYTE*    s_baseline = NULL;   /* 我们最后一次见到的 config.ini 内容 */
static size_t   s_baseLen  = 0;
static DWORD    s_lastCheck = 0;
static DWORD    s_extTick   = 0;     /* 合法外部修改生效时刻；0 = 无观察 */
static int      s_extOk     = 0;     /* 宽限期内同步成功次数 */
static int      s_extFail   = 0;     /* 宽限期内同步失败次数 */
static bool     s_wantUiReload = false; /* 外部改过配置，UI 该重填控件 */
static bool     s_active    = false;

/* ------------------------------------------------------------------ */
/* 文件读写                                                           */
/* ------------------------------------------------------------------ */

static BYTE* read_file(const char* path, size_t* outLen) {
    *outLen = 0;
    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || n > 256 * 1024) { fclose(f); return NULL; }  /* 上限 256KB，挡异常文件 */
    rewind(f);
    BYTE* buf = (BYTE*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { free(buf); return NULL; }
    buf[rd] = 0;
    *outLen = rd;
    return buf;
}

/* 原子写：先写 .tmp 再 MoveFileEx 改名，和 config_extra.c 同一套做法。
   读者（另一个进程）只可能看到完整文件，不会读到写一半的内容。 */
static bool write_file_atomic(const char* path, const void* data, size_t len) {
    char tmp[MAX_PATH + 16] = "";
    _snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    tmp[sizeof(tmp) - 1] = '\0';

    FILE* f = NULL;
    if (fopen_s(&f, tmp, "wb") != 0 || !f) return false;
    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 篡改判定                                                           */
/* ------------------------------------------------------------------ */

/* server 指向本机/未指定时同步必然失败——这是"让校时失效"最省事的改法，
   而程序默认值恰好就是 127.0.0.1，学生改成它不会有任何异常痕迹。 */
static bool host_is_pointless(const char* s) {
    if (!s || !s[0]) return true;
    if (_stricmp(s, "localhost") == 0) return true;
    if (strcmp(s, "0.0.0.0") == 0) return true;
    if (strncmp(s, "127.", 4) == 0) return true;      /* 127.0.0.0/8 环回 */
    if (strcmp(s, "::1") == 0 || strcmp(s, "::") == 0) return true;
    return false;
}

/* 这份配置能不能用。语法/范围钳制由 config_load 负责，这里只补两条
   load 管不了的：server 是否环回、关键参数是否还在合理区间。 */
static bool config_is_usable(const config_t* c) {
    if (!c) return false;
    if (host_is_pointless(c->server)) return false;
    if (c->port < 1 || c->port > 65535) return false;
    if (c->timeout_ms < 100 || c->timeout_ms > 10000) return false;
    if (c->interval_minutes <= 0 || c->interval_minutes > 1440) return false;
    if (c->retry_seconds < 1 || c->retry_seconds > 3600) return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* 基线管理                                                           */
/* ------------------------------------------------------------------ */

static void set_baseline(const BYTE* data, size_t len) {
    if (s_baseline) { free(s_baseline); s_baseline = NULL; }
    s_baseLen = 0;
    if (!data || len == 0) return;
    s_baseline = (BYTE*)malloc(len + 1);
    if (s_baseline) {
        memcpy(s_baseline, data, len);
        s_baseline[len] = 0;
        s_baseLen = len;
    }
}

/* 把当前生效配置写成 lastgood，内容沿用 config_save 的写入逻辑。
   返回是否写成功（失败只影响后续能不能恢复，不阻断同步）。 */
static bool write_lastgood(const config_t* cfg) {
    if (!s_active || !cfg || !s_lastGoodPath[0]) return false;
    if (config_save(cfg, s_lastGoodPath)) {
        LOG_DEBUG("last-known-good config refreshed");
        return true;
    }
    LOG_DEBUG("last-known-good write failed: %s", s_lastGoodPath);
    return false;
}

/* 用 lastgood 覆盖 config.ini，并把基线更新为 lastgood 的内容。
   返回 true 表示调用方需要重新 load 配置。 */
static bool restore_from_lastgood(const char* why) {
    if (!s_lastGoodPath[0]) {
        LOG_ERROR("config tampered (%s) but last-good path unavailable", why);
        return false;
    }
    size_t lgLen = 0;
    BYTE* lg = read_file(s_lastGoodPath, &lgLen);
    if (!lg) {
        /* lastgood 也读不到（被删/磁盘满）：无从恢复，只能报警。
           不能拿 live 顶替——那正是可能已被篡改的那份。 */
        LOG_ERROR("config tampered (%s) but %s unreadable; cannot restore",
                  why, s_lastGoodPath);
        s_extTick = 0;
        return false;
    }
    if (!write_file_atomic(s_cfgPath, lg, lgLen)) {
        LOG_ERROR("config tampered (%s); restore write failed", why);
        free(lg);
        return false;
    }
    LOG_ERROR("config file was tampered (%s); restored from last known good", why);
    set_baseline(lg, lgLen);
    s_extTick = 0;
    s_extOk = 0;
    free(lg);
    return true;
}

/* 把一段字节写进临时文件再交给 config_load 解析，避免把未落盘的内容
   直接塞给解析器。写不进临时目录时按"不可用"处理——
   宁可恢复，也不要接受来路不明的内容。 */
static bool load_from_bytes(config_t* out, const BYTE* data, size_t len) {
    char dir[MAX_PATH] = "";
    if (!GetTempPathA(sizeof(dir), dir)) return false;
    char file[MAX_PATH] = "";
    _snprintf(file, sizeof(file), "%slants_cfg_%lu.ini", dir, GetCurrentProcessId());
    file[sizeof(file) - 1] = '\0';
    if (!write_file_atomic(file, data, len)) return false;
    bool ok = config_load(out, file);
    DeleteFileA(file);
    return ok;
}

/* config.ini 内容发生变化后的分类处理。返回 true 表示需要重新 load。 */
static cguard_result_t on_config_changed(const BYTE* data, size_t len) {
    /* 先把新内容记成基线：无论后续怎么处理，下一轮都不该再触发一次 */
    set_baseline(data, len);

    /* 1) 语法/结构是否还能解出可用配置 */
    config_t nc;
    config_init_defaults(&nc);
    if (!load_from_bytes(&nc, data, len))
        return restore_from_lastgood("unparsable or damaged");

    /* 2) 关键字段是否有效 */
    if (!config_is_usable(&nc)) {
        char why[80];
        _snprintf(why, sizeof(why), "unusable settings (server=%s)", nc.server);
        why[sizeof(why) - 1] = '\0';
        return restore_from_lastgood(why);
    }

    /* 3) 合法修改：进入观察期，先不采纳。
      GUI 主动保存走 cguard_note_saved，不会进这条分支。

      同时请求立即同步：宽限期比同步周期短得多（3 分钟 vs 默认 60 分钟），
      不主动催一次的话窗口内一次同步都轮不到，"零成功"就会被误读成
      "新服务器不可用"，把老师刚改的配置又静默回滚回去。 */
    LOG_INFO("config changed outside the program; observing %lu seconds before accepting",
             (unsigned long)(CGUARD_GRACE_MS / 1000));
    s_extTick   = GetTickCount();
    s_extOk     = 0;
    s_extFail   = 0;
    return CGUARD_LOAD;
}

/* ------------------------------------------------------------------ */
/* 对外                                                               */
/* ------------------------------------------------------------------ */

void cguard_init(const char* cfgPath, const config_t* live) {
    if (!cfgPath || !cfgPath[0]) return;
    _snprintf(s_cfgPath, sizeof(s_cfgPath), "%s", cfgPath);
    s_cfgPath[sizeof(s_cfgPath) - 1] = '\0';

    /* lastgood 与 config.ini 同目录，跟着配置文件走 */
    _snprintf(s_lastGoodPath, sizeof(s_lastGoodPath), "%s", cfgPath);
    {
        char* slash = strrchr(s_lastGoodPath, '\\');
        if (slash) *(slash + 1) = '\0';
        else       s_lastGoodPath[0] = '\0';
    }
    if (s_lastGoodPath[0] &&
        strlen(s_lastGoodPath) + sizeof(CGUARD_LASTGOOD_NAME) < sizeof(s_lastGoodPath))
        strcat(s_lastGoodPath, CGUARD_LASTGOOD_NAME);
    else
        s_lastGoodPath[0] = '\0';   /* 路径过长：放弃 lastgood，只保留基线 */

    s_active    = true;
    s_extTick   = 0;
    s_extOk     = 0;
    s_lastCheck = GetTickCount();

    size_t len = 0;
    BYTE* cur = read_file(s_cfgPath, &len);
    if (cur) {
        /* 基线 = 当前文件内容，本轮不动任何东西 */
        set_baseline(cur, len);

        /* 初始 lastgood：仅当它还不存在时才生成。
           为什么不能只在"同步成功"后才写：本程序最常见的故障态恰恰就是
           服务器不可达（机房刚装好还没配服务器、或刚被篡改），那种情况下
           同步永远不成功，lastgood 便永远是空的，等到真被篡改时反而无从
           恢复。所以启动时只要当前配置"可用"，就立刻固化成基线。

           只在 lastgood 不存在时才这么做：若已有一份来自先前运行的好配置，
           不能拿现在这份（可能已被篡改，只是恰好字段合法）覆盖掉它。 */
        if (s_lastGoodPath[0] &&
            GetFileAttributesA(s_lastGoodPath) == INVALID_FILE_ATTRIBUTES) {
            config_t nc;
            config_init_defaults(&nc);
            if (load_from_bytes(&nc, cur, len) && config_is_usable(&nc)) {
                if (write_lastgood(&nc))
                    LOG_INFO("initial last-known-good captured from current config");
            } else {
                /* 启动时配置就已不可用（上次被篡改且尚未恢复），且没有 lastgood：
                   记一条 ERROR，这本身就是要老师到现场的强信号。 */
                LOG_ERROR("current config unusable (server=%s) and no last-known-good exists",
                          nc.server);
            }
        }
        free(cur);
        LOG_DEBUG("config guard armed (cfg=%s, lastgood=%s)", s_cfgPath, s_lastGoodPath);
        return;
    }

    /* config.ini 读不到（不存在/被删）：用当前生效配置生成一份 lastgood，
       否则"文件被删"这条最常见的破坏会因无据可恢而失效。 */
    set_baseline(NULL, 0);
    if (live) {
        if (write_lastgood(live))
            LOG_WARN("config.ini not readable; generated last-known-good from running config");
        else
            LOG_WARN("config.ini not readable and last-known-good could not be written");
    }
}
void cguard_note_success(const config_t* cfg) {
    if (!s_active) return;
    if (s_extTick != 0) s_extOk++;      /* 宽限期内记一次成功 */
    write_lastgood(cfg);
}

/* 同步失败：只在宽限期内计数，用来区分"试过不行"和"压根没试" */
void cguard_note_failure(void) {
    if (!s_active) return;
    if (s_extTick != 0) s_extFail++;
}

void cguard_note_saved(const char* cfgPath) {
    if (!s_active || !cfgPath || !cfgPath[0]) return;
    if (_stricmp(cfgPath, s_cfgPath) != 0) return;   /* 不是同一份配置 */

    s_extTick   = 0;      /* 用户主动保存：取消宽限观察 */
    s_extOk     = 0;
    s_extFail   = 0;
    s_wantUiReload = false;
    size_t len = 0;
    BYTE* cur = read_file(s_cfgPath, &len);
    if (cur) { set_baseline(cur, len); free(cur); }
}

static cguard_result_t say(cguard_result_t r, char* outCfgPath, size_t outCap) {
    if (outCfgPath && outCap && r != CGUARD_NONE)
        _snprintf(outCfgPath, outCap, "%s", s_cfgPath);
    return r;
}

/* 需要调用方立刻加载配置时，也通知 UI 重新填一次控件：否则界面还显示旧值，
   用户既看不到自己改动的效果，之后点"保存"还会把旧值写回去。 */
static void ask_ui_reload(void) { s_wantUiReload = true; }

bool cguard_take_ui_reload(void) {
    bool r = s_wantUiReload;
    s_wantUiReload = false;
    return r;
}

cguard_result_t cguard_tick(const config_t* live, char* outCfgPath, size_t outCap) {
    if (!s_active) return CGUARD_NONE;
    if (outCfgPath && outCap) outCfgPath[0] = '\0';

    DWORD now = GetTickCount();
    if (now - s_lastCheck < CGUARD_CHECK_MS) return CGUARD_NONE;
    s_lastCheck = now;

    if (GetFileAttributesA(s_cfgPath) == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND) {
        /* 文件被删：这是最干脆的破坏手法 */
        bool had = (s_baseLen != 0);
        set_baseline(NULL, 0);
        if (!had) return CGUARD_NONE;
        ask_ui_reload();
        return restore_from_lastgood("config file deleted") ? say(CGUARD_RESTORED, outCfgPath, outCap)
                                                             : CGUARD_NONE;
    }

    size_t len = 0;
    BYTE* cur = read_file(s_cfgPath, &len);
    if (!cur) {
        /* 读不到但不是不存在（权限被改等）。此时也写不回 lastgood，
           报一条就好，别在日志里刷 ERROR 循环。 */
        LOG_WARN("config.ini unreadable this round; guard deferred");
        return CGUARD_NONE;
    }

    bool same = (len == s_baseLen) && (s_baseline != NULL) &&
                (memcmp(cur, s_baseline, len) == 0);
    if (same) {
        free(cur);
        if (s_extTick != 0 && (now - s_extTick) > CGUARD_GRACE_MS) {
            /* 只有"确实试过且一次都没成功"才回滚。
               零尝试不等于失败：同步周期可能远大于宽限期（默认 60 分钟 vs 3 分钟），
               那种情况下窗口内压根没轮到同步，用它判失败会把老师合法的改动
               静默回滚——看着就像程序见鬼了。CGUARD_LOAD 会触发立即同步，
               正常路径下这里一定有尝试记录。 */
            if (s_extOk == 0 && s_extFail > 0) {
                ask_ui_reload();
                return restore_from_lastgood("external change failed every sync")
                       ? say(CGUARD_RESTORED, outCfgPath, outCap) : CGUARD_NONE;
            }
            if (s_extOk > 0)
                LOG_INFO("external config change accepted after a successful sync");
            s_extTick = 0;
            s_extOk   = 0;
            s_extFail = 0;
            write_lastgood(live);
        }
        return CGUARD_NONE;
    }

    cguard_result_t r = on_config_changed(cur, len);
    free(cur);
    if (r == CGUARD_LOAD) ask_ui_reload();
    return say(r, outCfgPath, outCap);
}