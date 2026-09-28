#ifndef AUTH_H
#define AUTH_H

#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 密码是否已设置（auth.dat 存在且合法） */
bool auth_is_set(void);

/* 校验明文密码是否正确；未设置时返回 false */
bool auth_verify(const char* password_utf8);

/* 设置新密码（覆盖旧的）；password 为 UTF-8 明文，空串返回 false */
bool auth_set(const char* password_utf8);

/* 清除密码（删除 auth.dat） */
bool auth_clear(void);

/* 供 GUI 调用的模态对话框：
   - auth_prompt_verify: 弹出“请输入密码”框，校验通过返回 true
   - auth_prompt_set:    弹出“设置密码”框（含确认），设置成功返回 true
   parent 为主窗口，用于居中和模态
 */
bool auth_prompt_verify(HWND parent);
bool auth_prompt_set(HWND parent);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_H */
