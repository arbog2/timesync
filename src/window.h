#ifndef WINDOW_H
#define WINDOW_H

#include <windows.h>
#include "config.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 创建并显示主窗口；返回 HWND，失败返回 NULL。
   cfg/state/cfgPath/exeDir 指针需在进程生命周期内保持有效（main.c 的全局）。 */
HWND window_create(HINSTANCE hInst, config_t* cfg, task_state_t* state,
                   const char* cfgPath, const char* exeDir);

/* 以 hwnd 为主窗口进入消息循环，直到收到退出。返回进程退出码（0）。 */
int window_message_loop(HWND hwnd);

/* 若主窗口存在则将其带到前台（用于单实例唤起）。 */
void window_bring_to_front(HWND hwnd);

#ifdef __cplusplus
}
#endif

#endif /* WINDOW_H */
