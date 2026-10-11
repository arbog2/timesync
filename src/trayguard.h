#ifndef TRAYGUARD_H
#define TRAYGUARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 服务侧反向看门狗：负责把被杀掉的托盘进程拉回来。
   托盘进程（--tray）画着托盘图标和水印，它在用户会话里；服务在 Session 0
   看不见也碰不到它，所以只能用"发现它没了就用活动会话的用户身份重新起一个"
   的办法间接守护。

   服务的失败恢复动作（SCM auto-restart）只保护服务自己，看门狗补上另一半：
   杀了当前用户身份的 timesync.exe，几秒后托盘图标和水印重新出现。

   线程约定：start/stop 只在服务主线程调用；note_user_quit 在托盘进程
   自己的线程里调用，双方通过磁盘上的标记文件通信，不共享内存。 */

/* 启动看门狗线程（仅服务模式调用）。 */
void trayguard_start(void);

/* 停止看门狗线程并等待其退出；服务停止时调用。 */
void trayguard_stop(void);

/* 用户在托盘菜单点了"退出"：写自愿退出标记。
   看门狗见到标记就不再拉回——用户主动不想用托盘时反复拉起会很烦。 */
void trayguard_note_user_quit(void);

/* 托盘正常启动时调用：清掉自愿退出标记。
   这样一次自愿退出只影响一次，托盘重新起来之后恢复看护。 */
void trayguard_clear_quit_mark(void);

#ifdef __cplusplus
}
#endif

#endif /* TRAYGUARD_H */
