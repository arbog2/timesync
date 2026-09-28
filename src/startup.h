#ifndef STARTUP_H
#define STARTUP_H
#include <windows.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool startup_is_enabled(void);
bool startup_set(bool enable);
// 若已存在旧版自启任务（TR 不含 --tray），静默迁移为 --tray 托盘启动，避免下次开机弹主窗口
void startup_ensure_tray_args(void);
// 服务托盘伴随任务：服务本身跑在 Session 0 看不到图标，需额外一个 ONLOGON 托盘任务进桌面
bool startup_tray_for_service(bool enable);
#ifdef __cplusplus
}
#endif
#endif
