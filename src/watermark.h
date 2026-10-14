#ifndef WATERMARK_H
#define WATERMARK_H

#include <windows.h>
#include <stdbool.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 桌面水印：在桌面右上角以黄色文字显示本机机器名。

   实现要点（决定了它为什么"看得见但不碍事"）：
     - WS_EX_LAYERED + UpdateLayeredWindow：逐像素 alpha，文字边缘抗锯齿；
     - 默认贴桌面：Z 序插到 Progman 正上方，位于所有应用窗口之下、
       桌面之上，只在桌面可见，不会盖在打开的软件上；
       layer=top 时才加 WS_EX_TOPMOST 常驻所有窗口之上；
     - WS_EX_TRANSPARENT + WM_NCHITTEST 返回 HTTRANSPARENT：鼠标完全穿透，
       不会挡住右上角任何按钮的点击；
     - WS_EX_TOOLWINDOW：不进任务栏、不进 Alt+Tab；
     - WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE：永不抢焦点。

   服务模式（Session 0）没有桌面，水印由登录时拉起的托盘伴随进程绘制。 */

/* 按配置应用：enabled=false 时销毁已有水印；true 时创建或就地刷新。
   hInst 可为 NULL（已创建过时不需要）。返回是否处于"已显示"状态。 */
bool watermark_apply(HINSTANCE hInst, const config_t* cfg);

/* 销毁水印窗口并释放字体。进程退出前调用。 */
void watermark_destroy(void);

/* 强制重画（分辨率/工作区变化、机器名变化后调用）。 */
void watermark_refresh(void);

/* 桌面模式下的重锚定：Explorer 未就绪时创建的水印稍后回到桌面正上方，
   并清除可能残留的 TOPMOST 位。供 TaskbarCreated 与托盘重试定时器调用。 */
void watermark_reanchor(void);

/* 当前水印窗口句柄；未显示时为 NULL。 */
HWND watermark_hwnd(void);

#ifdef __cplusplus
}
#endif

#endif /* WATERMARK_H */
