#ifndef CGUARD_H
#define CGUARD_H

#include <stdbool.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 配置完整性守卫：防止 config.ini 被程序外部篡改后，校时功能被悄悄废掉。
   典型场景：学生是本地管理员，把 server 改成 127.0.0.1（程序默认值，
   指向本机必然同步失败）或直接删掉配置文件，从此时间不再校正。

   "上次正确值"的定义：最近一次**同步成功**时生效的那份配置，
   固化在 config.ini 同目录的 config.lastgood.ini 里。

   篡改判定分三档（详见 cguard.c 顶部注释）：
     破坏性（语法坏/文件丢失/server 环回）   -> 立即恢复
     合法但指向无效服务器                    -> 3 分钟宽限期内零成功则恢复
     合法且能同步成功                        -> 接受为新基线，不拦

   重要：GUI 主动保存的配置不受宽限期约束（cguard_note_saved 会刷新基线），
   否则老师刚改的设置可能在 3 分钟后被回滚，变成"见鬼了"。

   线程约定：cguard_tick 只在 worker 线程调用；cguard_take_ui_reload 只在
   UI 线程调用（取走式，两者不共享可变状态）。 */

typedef enum {
    CGUARD_NONE = 0,     /* 无变化 */
    CGUARD_LOAD,         /* 程序外改动且判定可用，调用方应加载并立即验证 */
    CGUARD_RESTORED      /* 已从 lastgood 覆盖回去，调用方必须加载使其生效 */
} cguard_result_t;

/* 用当前生效的配置文件路径启动守卫。传入的 live 是当前内存配置，
   用于在 config.ini 读不到时兜底生成初始 lastgood。 */
void cguard_init(const char* cfgPath, const config_t* live);

/* 本轮同步成功：把此刻生效的配置固化为"上次正确值"。 */
void cguard_note_success(const config_t* cfg);

/* 本轮同步失败。仅用于宽限期判定，区分"试过不行"与"压根没试过"——
   同步周期可能远大于宽限期，没尝试过不能当作失败。 */
void cguard_note_failure(void);

/* GUI 侧"应用并保存"成功后调用：把当前文件内容设为基线，
   并清掉宽限期观察，避免误回滚用户主动做的修改。 */
void cguard_note_saved(const char* cfgPath);

/* 周期性检查（建议每轮循环都调，内部自带 30 秒节流）。
   需要调用方加载配置时返回非 CGUARD_NONE，并通过 outCfgPath 给出路径；
   两种情况都应当立即同步一次，用新配置验证它到底能不能用。 */
cguard_result_t cguard_tick(const config_t* live, char* outCfgPath, size_t outCap);

/* UI 线程每轮问一次：配置文件刚在程序外变过，界面控件需要重新填充——
   否则用户看不到自己改动的效果，之后点"保存"还会把旧值写回去。
   取走即清零。只填控件，不写配置。 */
bool cguard_take_ui_reload(void);

#ifdef __cplusplus
}
#endif

#endif /* CGUARD_H */
