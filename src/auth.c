#define UNICODE
#define _UNICODE
#include "auth.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define AUTH_FILE_NAMEA "\\auth.dat"
#define AUTH_SALT_LEN 16
#define AUTH_HASH_LEN 32
#define AUTH_ITER 120000

static void auth_path(char* out, size_t cap) {
    char prog[MAX_PATH]="";
    if (!GetEnvironmentVariableA("ProgramData", prog, sizeof(prog))) strcpy(prog, "C:\\ProgramData");
    _snprintf(out, cap, "%s\\LANTimeSync%s", prog, AUTH_FILE_NAMEA);
    out[cap-1]='\0';
    char dir[MAX_PATH]=""; strncpy(dir, out, sizeof(dir)-1);
    char* bs=strrchr(dir,'\\'); if(bs){*bs='\0'; CreateDirectoryA(dir,NULL);}
    // 兼容旧版本：若 ProgramData 下不存在但 exe 同目录下有，自动迁移
    HANDLE h = CreateFileA(out, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return; }
    char base[MAX_PATH]=""; GetModuleFileNameA(NULL, base, sizeof(base));
    char* b2=strrchr(base,'\\'); if(b2)*b2='\0';
    char cand[MAX_PATH]=""; _snprintf(cand, sizeof(cand), "%s%s", base, AUTH_FILE_NAMEA);
    HANDLE h2 = CreateFileA(cand, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h2 != INVALID_HANDLE_VALUE) {
        DWORD sz=GetFileSize(h2,NULL);
        if (sz==AUTH_SALT_LEN+AUTH_HASH_LEN) {
            CloseHandle(h2);
            if (MoveFileA(cand, out)) return;
            // 移动失败（可能跨卷），退化为复制后删除
            if (CopyFileA(cand, out, FALSE)) { DeleteFileA(cand); return; }
        } else CloseHandle(h2);
    }
}

static bool pbkdf2_sha256(const BYTE* pwd, DWORD pwdLen, const BYTE* salt, DWORD saltLen, DWORD iter, BYTE* out32) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, L"SHA256", NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(st)) return false;
    st = BCryptDeriveKeyPBKDF2(hAlg, (PUCHAR)pwd, pwdLen, (PUCHAR)salt, saltLen, (ULONGLONG)iter, out32, AUTH_HASH_LEN, 0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return BCRYPT_SUCCESS(st);
}

bool auth_is_set(void) {
    char path[MAX_PATH]=""; auth_path(path,sizeof(path));
    HANDLE h=CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(h==INVALID_HANDLE_VALUE) return false;
    DWORD sz=GetFileSize(h,NULL); CloseHandle(h);
    return sz == (AUTH_SALT_LEN+AUTH_HASH_LEN);
}

bool auth_verify(const char* password_utf8) {
    if(!password_utf8||!password_utf8[0]) return false;
    char path[MAX_PATH]=""; auth_path(path,sizeof(path));
    HANDLE h=CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(h==INVALID_HANDLE_VALUE) return false;
    BYTE salt[AUTH_SALT_LEN], expect[AUTH_HASH_LEN];
    DWORD rd=0;
    BOOL ok = ReadFile(h, salt, AUTH_SALT_LEN, &rd, NULL) && rd==AUTH_SALT_LEN;
    if(ok) ok = ReadFile(h, expect, AUTH_HASH_LEN, &rd, NULL) && rd==AUTH_HASH_LEN;
    CloseHandle(h);
    if(!ok) return false;
    BYTE out[AUTH_HASH_LEN]={0};
    // copy password to temp buffer so we can zero it after
    char tmp[256]={0}; strncpy(tmp, password_utf8, sizeof(tmp)-1);
    bool r = pbkdf2_sha256((BYTE*)tmp, (DWORD)strlen(tmp), salt, AUTH_SALT_LEN, AUTH_ITER, out);
    SecureZeroMemory(tmp, sizeof(tmp));
    if(!r) return false;
    bool eq = (memcmp(out, expect, AUTH_HASH_LEN)==0);
    SecureZeroMemory(out, sizeof(out));
    SecureZeroMemory(expect, sizeof(expect));
    return eq;
}

bool auth_set(const char* password_utf8) {
    if(!password_utf8||!password_utf8[0]) return false;
    BYTE salt[AUTH_SALT_LEN];
    BCRYPT_ALG_HANDLE hAlg=NULL;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, L"RNG", NULL, 0);
    if(!BCRYPT_SUCCESS(st)) return false;
    st = BCryptGenRandom(hAlg, salt, AUTH_SALT_LEN, 0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if(!BCRYPT_SUCCESS(st)) return false;
    BYTE hash[AUTH_HASH_LEN]={0};
    char tmp[256]={0}; strncpy(tmp, password_utf8, sizeof(tmp)-1);
    if(!pbkdf2_sha256((BYTE*)tmp,(DWORD)strlen(tmp),salt,AUTH_SALT_LEN,AUTH_ITER,hash)) {
        SecureZeroMemory(tmp,sizeof(tmp));
        return false;
    }
    SecureZeroMemory(tmp,sizeof(tmp));
    char path[MAX_PATH]=""; auth_path(path,sizeof(path));
    HANDLE h=CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(h==INVALID_HANDLE_VALUE) { SecureZeroMemory(hash,sizeof(hash)); return false; }
    DWORD wr=0;
    BOOL w1=WriteFile(h,salt,AUTH_SALT_LEN,&wr,NULL);
    BOOL w2=w1 && WriteFile(h,hash,AUTH_HASH_LEN,&wr,NULL);
    CloseHandle(h);
    SecureZeroMemory(hash,sizeof(hash));
    SecureZeroMemory(salt,sizeof(salt));
    if(!w2) DeleteFileA(path);
    return w2!=0;
}

bool auth_clear(void) {
    char path[MAX_PATH]=""; auth_path(path,sizeof(path));
    return DeleteFileA(path)!=0;
}

#define IDC_PWD_EDIT    100
#define IDC_PWD_EDIT2   101
#define IDC_PWD_OK      102
#define IDC_PWD_CANCEL  103
#define IDC_PWD_LABEL   104
#define IDC_PWD_LABEL2  105

typedef struct { bool isSet; bool ok; WCHAR pwd[128]; WCHAR pwd2[128]; } PwdCtx;

static LRESULT CALLBACK PwdWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    PwdCtx* ctx=(PwdCtx*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    switch(msg){
        case WM_CREATE: {
            CREATESTRUCTW* cs=(CREATESTRUCTW*)lp;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)cs->lpCreateParams);
            ctx=(PwdCtx*)cs->lpCreateParams;
            HFONT hf=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HDC hdc=GetDC(hwnd); int dpi=GetDeviceCaps(hdc,LOGPIXELSY); ReleaseDC(hwnd,hdc);
            int h=-MulDiv(9,dpi,72);
            HFONT hf2=CreateFontW(h,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
            if(hf2) hf=hf2;
            if(ctx->isSet){
                CreateWindowW(L"STATIC",L"请输入密码：",WS_CHILD|WS_VISIBLE,16,16,260,16,hwnd,(HMENU)IDC_PWD_LABEL,GetModuleHandleW(NULL),NULL);
                HWND e=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_PASSWORD|WS_TABSTOP,16,36,260,22,hwnd,(HMENU)IDC_PWD_EDIT,GetModuleHandleW(NULL),NULL);
                SendMessageW(e,WM_SETFONT,(WPARAM)hf,TRUE); SendMessageW(e,EM_SETLIMITTEXT,64,0);
            } else {
                CreateWindowW(L"STATIC",L"设置密码：",WS_CHILD|WS_VISIBLE,16,12,260,16,hwnd,(HMENU)IDC_PWD_LABEL,GetModuleHandleW(NULL),NULL);
                HWND e1=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_PASSWORD|WS_TABSTOP,16,30,260,22,hwnd,(HMENU)IDC_PWD_EDIT,GetModuleHandleW(NULL),NULL);
                SendMessageW(e1,WM_SETFONT,(WPARAM)hf,TRUE); SendMessageW(e1,EM_SETLIMITTEXT,64,0);
                CreateWindowW(L"STATIC",L"确认密码：",WS_CHILD|WS_VISIBLE,16,58,260,16,hwnd,(HMENU)IDC_PWD_LABEL2,GetModuleHandleW(NULL),NULL);
                HWND e2=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_PASSWORD|WS_TABSTOP,16,76,260,22,hwnd,(HMENU)IDC_PWD_EDIT2,GetModuleHandleW(NULL),NULL);
                SendMessageW(e2,WM_SETFONT,(WPARAM)hf,TRUE); SendMessageW(e2,EM_SETLIMITTEXT,64,0);
            }
            HWND b1=CreateWindowW(L"BUTTON",L"确定",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON|WS_TABSTOP,100, ctx->isSet?68:108,70,24,hwnd,(HMENU)IDC_PWD_OK,GetModuleHandleW(NULL),NULL);
            HWND b2=CreateWindowW(L"BUTTON",L"取消",WS_CHILD|WS_VISIBLE|WS_TABSTOP,176, ctx->isSet?68:108,70,24,hwnd,(HMENU)IDC_PWD_CANCEL,GetModuleHandleW(NULL),NULL);
            SendMessageW(b1,WM_SETFONT,(WPARAM)hf,TRUE); SendMessageW(b2,WM_SETFONT,(WPARAM)hf,TRUE);
            SetFocus(GetDlgItem(hwnd,IDC_PWD_EDIT));
            return 0;
        }
        case WM_COMMAND: {
            int id=LOWORD(wp);
            // IsDialogMessage 在回车/ESC 时发送的是 IDOK(1)/IDCANCEL(2)，兼容自定义 ID
            if(id==IDC_PWD_OK || id==IDOK){
                WCHAR w1[128]=L"",w2[128]=L"";
                GetWindowTextW(GetDlgItem(hwnd,IDC_PWD_EDIT),w1,128);
                if(!ctx->isSet) GetWindowTextW(GetDlgItem(hwnd,IDC_PWD_EDIT2),w2,128);
                if(w1[0]==L'\0'){ MessageBoxW(hwnd,L"密码不能为空。",L"提示",MB_OK|MB_ICONWARNING); return 0; }
                if(!ctx->isSet && wcscmp(w1,w2)!=0){ MessageBoxW(hwnd,L"两次输入的密码不一致。",L"提示",MB_OK|MB_ICONWARNING); return 0; }
                wcscpy(ctx->pwd,w1); if(!ctx->isSet) wcscpy(ctx->pwd2,w2);
                ctx->ok=true;
                DestroyWindow(hwnd); return 0;
            }
            if(id==IDC_PWD_CANCEL || id==IDCANCEL){ ctx->ok=false; DestroyWindow(hwnd); return 0; }
            break;
        }
        case WM_CLOSE: ctx->ok=false; DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(1); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static volatile LONG s_pwdActive = 0;

static bool pwd_dialog(HWND parent, bool isVerify, WCHAR* outPwd, int cap){
    // 防止重入：托盘双击/单机可能在密码框消息循环内再次触发 WM_TRAYICON，
    // 若此时再次进入 pwd_dialog，会产生两个重叠的密码框且销毁时残留
    if (InterlockedCompareExchange(&s_pwdActive, 1, 0) != 0) {
        HWND exist = FindWindowW(L"LANTimeSyncPwdDlg", NULL);
        if (exist) SetForegroundWindow(exist);
        return false;
    }
    bool isSet = isVerify;
    PwdCtx ctx; memset(&ctx,0,sizeof(ctx));
    ctx.isSet=isSet; ctx.ok=false;
    const WCHAR* cls=L"LANTimeSyncPwdDlg";
    WNDCLASSW wc; memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=PwdWndProc; wc.hInstance=GetModuleHandleW(NULL);
    wc.lpszClassName=cls; wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassW(&wc);
    int W=300, H=isSet?120:160;
    RECT rc={0,0,W,H}; AdjustWindowRect(&rc, WS_CAPTION|WS_SYSMENU|WS_POPUP, FALSE);
    int winW=rc.right-rc.left, winH=rc.bottom-rc.top;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-winW)/2;
    int sy=(GetSystemMetrics(SM_CYSCREEN)-winH)/2;
    const WCHAR* title = isSet?L"请输入密码":L"设置密码";
    BOOL parentWasVisible = parent && IsWindowVisible(parent);
    HWND hwnd=CreateWindowExW(0,cls,title, WS_CAPTION|WS_SYSMENU|WS_POPUP|WS_VISIBLE,
                              sx,sy,winW,winH, parent?parent:NULL,NULL,GetModuleHandleW(NULL),&ctx);
    if(!hwnd) { InterlockedExchange(&s_pwdActive, 0); return false; }
    SetWindowPos(hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
    if (parent) EnableWindow(parent,FALSE);
    MSG msg;
    while(GetMessageW(&msg,NULL,0,0)>0){
        if (IsWindow(hwnd) && IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if(!IsWindow(hwnd)) break;
    }
    if (parent) EnableWindow(parent,TRUE);
    if (parentWasVisible) ShowWindow(parent, SW_SHOW);
    if (parent) SetForegroundWindow(parent);
    MSG tmp; while(PeekMessageW(&tmp,NULL,WM_QUIT,WM_QUIT,PM_REMOVE)) {
        if(tmp.wParam==0) { PostQuitMessage(0); break; }
    }
    UnregisterClassW(cls, GetModuleHandleW(NULL));
    InterlockedExchange(&s_pwdActive, 0);
    if(!ctx.ok) return false;
    wcsncpy(outPwd, ctx.pwd, cap); outPwd[cap-1]=L'\0';
    SecureZeroMemory(ctx.pwd,sizeof(ctx.pwd));
    SecureZeroMemory(ctx.pwd2,sizeof(ctx.pwd2));
    return true;
}

bool auth_prompt_verify(HWND parent){
    if(!auth_is_set()) return true;
    WCHAR wpwd[128]=L"";
    if(!pwd_dialog(parent, true, wpwd, 128)) return false;
    char utf8[256]=""; WideCharToMultiByte(CP_UTF8,0,wpwd,-1,utf8,sizeof(utf8),NULL,NULL);
    SecureZeroMemory(wpwd,sizeof(wpwd));
    bool ok = auth_verify(utf8);
    SecureZeroMemory(utf8,sizeof(utf8));
    if(!ok) MessageBoxW(parent, L"密码错误。", L"验证失败", MB_OK|MB_ICONWARNING);
    return ok;
}

bool auth_prompt_set(HWND parent){
    WCHAR wpwd[128]=L"";
    if(!pwd_dialog(parent, false, wpwd, 128)) return false;
    char utf8[256]=""; WideCharToMultiByte(CP_UTF8,0,wpwd,-1,utf8,sizeof(utf8),NULL,NULL);
    SecureZeroMemory(wpwd,sizeof(wpwd));
    bool ok = auth_set(utf8);
    SecureZeroMemory(utf8,sizeof(utf8));
    if(!ok) {
        DWORD e=GetLastError();
        WCHAR msg[256]; _snwprintf(msg,256,L"设置密码失败。错误码 %lu。", e);
        MessageBoxW(parent, msg, L"错误", MB_OK|MB_ICONERROR);
    }
    else MessageBoxW(parent, L"密码已设置。\n下次从托盘打开主窗口或退出时将要求输入密码。", L"成功", MB_OK|MB_ICONINFORMATION);
    return ok;
}
