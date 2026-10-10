#define UNICODE
#define _UNICODE
#include "watermark.h"
#include "util.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WM_WM_CLASS   L"LANTimeSyncWatermark"
#define WM_TEXT_MAX   192      /* 机器名 + 模板替换后的最终文本上限 */
#define WM_MARGIN     14       /* 距屏幕右上角的边距（像素） */
#define WM_MIN_FONT   12
#define WM_MAX_FONT   200

static HWND      s_hwnd    = NULL;
static HWND      s_host    = NULL;   /* 附着的桌面宿主窗口；NULL = 未附着 */
static bool      s_onTop   = false;  /* true = 置顶模式（layer=top 或附着失败回退） */
static bool      s_hasFont = false;
static HFONT     s_font    = NULL;

/* 当前生效的参数（watermark_apply 时从 config 拷进来，绘制时直接读） */
static WCHAR     s_text[WM_TEXT_MAX] = L"";
static int       s_size    = 0;
static int       s_opacity = 210;
static int       s_bg      = 0;
static bool      s_active  = false;

/* ------------------------------------------------------------------ */
/* 文本                                                               */
/* ------------------------------------------------------------------ */

/* 取本机机器名。优先 NetBIOS 名（资源管理器"计算机名"里显示的那个，
   也是机房贴纸上写的），拿不到再退环境变量，最后退 DNS 主机名。 */
static void wm_computer_name(WCHAR* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = L'\0';

    WCHAR buf[256] = L"";
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    if (GetComputerNameW(buf, &n) && buf[0]) {
        wcsncpy(out, buf, cap - 1); out[cap - 1] = L'\0';
        return;
    }
    if (GetEnvironmentVariableW(L"COMPUTERNAME", buf,
                                (DWORD)(sizeof(buf) / sizeof(buf[0]))) > 0 && buf[0]) {
        wcsncpy(out, buf, cap - 1); out[cap - 1] = L'\0';
        return;
    }
    n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n) && buf[0]) {
        wcsncpy(out, buf, cap - 1); out[cap - 1] = L'\0';
    }
}

/* 生成最终显示文本：配置为空则直接用机器名；否则把模板里的
   %COMPUTERNAME% 替换成机器名（大小写敏感，保持可预测）。 */
static void wm_build_text(const config_t* cfg) {
    WCHAR name[256] = L"";
    wm_computer_name(name, 256);
    if (!name[0]) wcscpy(name, L"-");

    const char* tmplA = (cfg && cfg->watermark_text[0]) ? cfg->watermark_text : NULL;
    if (!tmplA) {
        wcsncpy(s_text, name, WM_TEXT_MAX - 1);
        s_text[WM_TEXT_MAX - 1] = L'\0';
        return;
    }

    WCHAR tmpl[WM_TEXT_MAX] = L"";
    /* 程序自己写配置时用 UTF-8，但用户拿记事本改过之后可能被存成 ANSI/GBK
       （中文 Windows 上记事本另存为"ANSI"就是 GBK）。先按 UTF-8 解，
       失败再按本机 ACP 解一次——否则中文前缀会静默消失、只剩机器名，
       而用户完全看不出是编码问题。 */
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tmplA, -1,
                            tmpl, WM_TEXT_MAX - 1) == 0 &&
        MultiByteToWideChar(CP_ACP, 0, tmplA, -1, tmpl, WM_TEXT_MAX - 1) == 0) {
        /* 两种编码都解不出来（明显不是文本）：退回机器名，别显示乱码 */
        wcsncpy(s_text, name, WM_TEXT_MAX - 1);
        s_text[WM_TEXT_MAX - 1] = L'\0';
        return;
    }

    static const WCHAR KEY[] = L"%COMPUTERNAME%";
    const size_t klen = sizeof(KEY) / sizeof(KEY[0]) - 1;
    size_t oi = 0;
    const WCHAR* p = tmpl;
    while (*p && oi + 2 < WM_TEXT_MAX) {
        const WCHAR* hit = wcsstr(p, KEY);
        if (!hit) {
            while (*p && oi + 2 < WM_TEXT_MAX) s_text[oi++] = *p++;
            break;
        }
        while (p < hit && oi + 2 < WM_TEXT_MAX) s_text[oi++] = *p++;
        for (const WCHAR* q = name; *q && oi + 2 < WM_TEXT_MAX; q++) s_text[oi++] = *q;
        p = hit + klen;
    }
    s_text[oi] = L'\0';
    if (!s_text[0]) {
        wcsncpy(s_text, name, WM_TEXT_MAX - 1);
        s_text[WM_TEXT_MAX - 1] = L'\0';
    }
}

/* ------------------------------------------------------------------ */
/* 绘制                                                               */
/* ------------------------------------------------------------------ */

static void wm_drop_font(void) {
    if (s_font) { DeleteObject(s_font); s_font = NULL; }
    s_hasFont = false;
}

/* 目标区域：主显示器工作区右上角。
   用工作区而不是整屏，是为了在任务栏放顶部时不把水印压在任务栏底下
   （任务栏在底部时两者上边界相同，行为不变）。
   多显示器只画主屏，符合"桌面右上角"的直觉。 */
static void wm_work_area(RECT* out) {
    RECT r = {0, 0, 0, 0};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0) ||
        r.right <= r.left || r.bottom <= r.top) {
        r.left = 0; r.top = 0;
        r.right  = GetSystemMetrics(SM_CXSCREEN);
        r.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    *out = r;
}

/* 此处不再需要查找宿主窗口：水印始终是顶层窗口，“贴在桌面上”的效果
   靠 wm_place_desktop_zorder() 调整 Z 序实现（见其注释）。 */

static HFONT wm_make_font(int px) {
    if (px < WM_MIN_FONT) px = WM_MIN_FONT;
    if (px > WM_MAX_FONT) px = WM_MAX_FONT;
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei UI");
    if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    return f;
}

/* 按当前 s_text / 位置重画整个分层窗口。
   一次到位：DIB 里同时完成"抗锯齿覆盖度"和"预乘 alpha 着色"，
   然后 UpdateLayeredWindow 整块提交（没有脏矩形，也不闪）。 */
static bool wm_paint(void) {
    if (!s_hwnd || !s_text[0]) return false;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return false;

    RECT work;
    wm_work_area(&work);
    const int availW = work.right - work.left - WM_MARGIN * 2;
    const int availH = work.bottom - work.top - WM_MARGIN * 2;

    /* 字号：配置给了就用，否则按屏高取一个比例；再大也不超过可用宽度 */
    int pt = s_size > 0 ? s_size : (work.bottom - work.top) / 22;
    if (pt < WM_MIN_FONT) pt = WM_MIN_FONT;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) { ReleaseDC(NULL, hdcScreen); return false; }

    HFONT oldFont = NULL;
    SIZE  sz = {0, 0};
    const int len = (int)wcslen(s_text);

    /* 量宽度；若超出了可用宽度就按比例缩字号重试（最多 3 轮）。
       超长机器名/模板不至于把窗口撑到屏幕之外。 */
    for (int attempt = 0; attempt < 3; attempt++) {
        wm_drop_font();
        s_font = wm_make_font(pt);
        s_hasFont = true;
        if (oldFont) SelectObject(hdcMem, oldFont);
        oldFont = (HFONT)SelectObject(hdcMem, s_font);
        sz.cx = 0; sz.cy = 0;
        if (!GetTextExtentPoint32W(hdcMem, s_text, len, &sz)) { sz.cx = 0; sz.cy = 0; }
        if (sz.cx <= availW || pt <= WM_MIN_FONT) break;
        int scaled = (int)((long long)pt * availW / (sz.cx > 0 ? sz.cx : 1));
        if (scaled >= pt) scaled = pt - 1;      /* 保证每轮都真变小 */
        pt = scaled < WM_MIN_FONT ? WM_MIN_FONT : scaled;
    }

    const int PAD = pt / 3;
    int W = sz.cx + PAD * 2;
    int H = sz.cy + PAD * 2;
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (W > work.right - work.left) W = work.right - work.left;
    if (H > work.bottom - work.top) H = work.bottom - work.top;

    /* 32bpp 自上而下 DIB。biHeight 取负值，行序与下面的线性像素遍历一致，
       省掉一次翻转。 */
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void*   bits = NULL;
    HBITMAP bmp  = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        SelectObject(hdcMem, oldFont);
        wm_drop_font();
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(hdcMem, bmp);

    /* 先把整块涂黑。GDI 绘制文字时不会写 32bpp DIB 的 alpha 通道，
       所以这里用"黑底白字 + 灰度抗锯齿"把亮度当作覆盖度，
       稍后再整块像素改写成预乘 alpha 的黄色。 */
    RECT all = {0, 0, W, H};
    FillRect(hdcMem, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(255, 255, 255));
    TextOutW(hdcMem, PAD, PAD, s_text, len);

    /* 亮度 -> alpha -> 预乘黄色。
       注意字节序：32 位 BI_RGB 的 DIB 每像素是 BGRX（不是 RGBA），
       索引 0=蓝、1=绿、2=红、3=alpha。写反红蓝会得到青色。
       纯黄是 (255,255,0)，预乘 alpha 后 R=G=alpha、B=0。
       底衬（若有）是黑色，预乘后 RGB 全 0、alpha = s_bg；
       按 source-over 与文字合成：A = ta + bg*(255-ta)/255。 */
    BYTE* px = (BYTE*)bits;
    const long total = (long)W * (long)H;
    for (long i = 0; i < total; i++) {
        BYTE c0 = px[i * 4 + 0];   /* 蓝 */
        BYTE c1 = px[i * 4 + 1];   /* 绿 */
        BYTE c2 = px[i * 4 + 2];   /* 红 */
        /* 白字经灰度抗锯齿后三通道相同，取最大值即覆盖度 */
        BYTE cov = c0 > c1 ? c0 : c1;
        if (c2 > cov) cov = c2;
        int ta = (int)cov * s_opacity / 255;
        int a  = ta + s_bg * (255 - ta) / 255;
        px[i * 4 + 0] = 0;          /* B =   0 * ta/255 */
        px[i * 4 + 1] = (BYTE)ta;   /* G = 255 * ta/255 */
        px[i * 4 + 2] = (BYTE)ta;   /* R = 255 * ta/255 */
        px[i * 4 + 3] = (BYTE)a;
    }

    POINT dst;
    POINT src = {0, 0};
    SIZE  siz = {W, H};
    /* 本窗口始终是顶层窗口（不做 SetParent）：Win7 及更早对"作为子窗口的
       分层窗口"调用 UpdateLayeredWindow 会失败，而那是这套水印唯一能用的
       渲染路径。按下桌面的效果靠 Z 序达成（见 wm_place_desktop_zorder），
       所以这里始终用屏幕坐标。 */
    dst.x = work.right - W - WM_MARGIN;
    dst.y = work.top + WM_MARGIN;
    if (dst.x < 0) dst.x = 0;
    if (dst.y < 0) dst.y = 0;

    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;   /* 不透明度已经烘进 alpha 通道 */
    bf.AlphaFormat         = AC_SRC_ALPHA;

    /* 用 Screen DC 作为目标，一次性提交位置+尺寸+像素：
       分层窗口不会先画后挪，所以不会闪。 */
    BOOL ok = UpdateLayeredWindow(s_hwnd, hdcScreen, &dst, &siz,
                                  hdcMem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, oldBmp);
    DeleteObject(bmp);
    SelectObject(hdcMem, oldFont);
    wm_drop_font();
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return ok != FALSE;
}

/* ------------------------------------------------------------------ */
/* 窗口                                                               */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WmWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        /* 让鼠标事件整体穿透到下面的窗口。分层窗口本身对 alpha=0 的像素
           就是穿透的，但黄色字本身有 alpha，不返回 HTTRANSPARENT 的话
           点在字上会被水印吃掉——那样右上角就成了一个"死区"。 */
        case WM_NCHITTEST:    return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_ERASEBKGND:   return 1;      /* 内容全由 UpdateLayeredWindow 提供 */
        case WM_NCPAINT:      return 0;      /* 无边框，别画系统边框 */

        case WM_DISPLAYCHANGE:
            /* 分辨率变了：重新按新工作区排版 */
            wm_paint();
            return 0;

        case WM_SETTINGCHANGE:
            /* 工作区变化（任务栏移动/尺寸变化）时上边距/右边距的参照点变了 */
            if (lp && wcscmp((LPCWSTR)lp, L"WorkArea") == 0)
                wm_paint();
            return 0;

        case WM_DESTROY:
            wm_drop_font();
            s_hwnd   = NULL;
            s_host   = NULL;
            s_active = false;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 把水印窗口插入桌面窗口（Progman）正上方的 Z 序位置。
   Progman 是所有顶层窗口里最靠下的一个（壁纸与桌面图标都画在它里面），
   紧贴它上方 = 位于所有应用窗口之下、桌面之上。
   不需要也不应该 SetParent：那样会把窗口变成子窗口，而 Win7 及更早对
   子窗口调用 UpdateLayeredWindow 会失败（这正是 Win7 上水印消失的原因）。 */
static void wm_place_desktop_zorder(void) {
    if (!s_hwnd || !IsWindow(s_hwnd)) return;

    HWND desktop = FindWindowW(L"Progman", NULL);
    if (desktop && IsWindow(desktop)) {
        /* hWndInsertAfter 用桌面窗口本身：插入到它正上方。
           SWP_NOACTIVATE 保持不变，绝不抢焦点。 */
        SetWindowPos(s_hwnd, desktop, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        s_host = desktop;      /* 记下来，供 Explorer 重启后重新插入 */
        return;
    }

    /* 拿不到桌面对话（Explorer 未就绪/已崩溃）：退回置顶，至少看得见 */
    LOG_WARN("desktop window not found; watermark falls back to topmost");
    SetWindowLongPtrW(s_hwnd, GWL_EXSTYLE,
                      (LONG_PTR)(GetWindowLongPtrW(s_hwnd, GWL_EXSTYLE) | WS_EX_TOPMOST));
    SetWindowPos(s_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    s_host = NULL;
}

static bool wm_create_window(HINSTANCE hInst) {
    if (s_hwnd && IsWindow(s_hwnd)) return true;
    if (!hInst) hInst = GetModuleHandleW(NULL);

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = WmWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = WM_WM_CLASS;
        wc.hCursor       = NULL;      /* 鼠标到不了这里（HTTRANSPARENT） */
        wc.hbrBackground = NULL;
        if (!RegisterClassW(&wc)) {
            /* 类已存在（同进程二次注册）不算失败 */
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                LOG_WARN("watermark RegisterClass failed err=%lu", GetLastError());
                return false;
            }
        }
        registered = true;
    }

    /* 基础样式：分层(逐像素 alpha) + 点击穿透 + 不进任务栏/Alt+Tab + 不抢焦点。
       水位(TOPMOST)不在这里决定：贴桌面模式下不能有它，
       只在 layer=top 或附着失败回退时才加。 */
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT
                  | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (s_onTop) exStyle |= WS_EX_TOPMOST;

    HWND h = CreateWindowExW(exStyle, WM_WM_CLASS, L"", WS_POPUP,
                             0, 0, 0, 0,
                             NULL, NULL, hInst, NULL);
    if (!h) {
        LOG_WARN("watermark CreateWindowEx failed err=%lu", GetLastError());
        return false;
    }
    s_hwnd = h;

    /* 先把像素准备好再显示：UpdateLayeredWindow 之前窗口是空的，
       这样不会出现"先闪一个空框再出字"。
       注意渲染必须在任何 Z 序调整之前完成——分层窗口的位置/尺寸是
       UpdateLayeredWindow 一次性提交的，先插 Z 序会让它短暂出现在桌面上
       被遮住的位置。 */
    if (!wm_paint()) {
        LOG_WARN("watermark first paint failed err=%lu", GetLastError());
        DestroyWindow(h);
        return false;
    }

    if (!s_onTop) {
        /* 贴桌面：插到 Progman 正上方，位于所有应用窗口之下。
           注意不能用 SetParent 做到这一点——那会让本窗口变成子窗口，
           而 Win7 及更早对子窗口调用 UpdateLayeredWindow 会失败。 */
        wm_place_desktop_zorder();
        LOG_INFO("watermark placed above desktop window (host=%p)",
                 (void*)s_host);
    }
    /* layer=top 时样式里已带 WS_EX_TOPMOST，不需额外处理 */

    ShowWindow(h, SW_SHOWNOACTIVATE);
    s_active = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* 对外                                                               */
/* ------------------------------------------------------------------ */

void watermark_destroy(void) {
    if (s_hwnd && IsWindow(s_hwnd)) {
        /* DestroyWindow 会走到 WM_DESTROY，那里回收字体和句柄。
           附着在桌面宿主上时，DestroyWindow 自身也会把它从宿主的子窗口
           链表里摘掉，不用先 SetParent 回桌面。 */
        DestroyWindow(s_hwnd);
    }
    s_hwnd   = NULL;
    s_host   = NULL;
    s_active = false;
    wm_drop_font();
}

bool watermark_apply(HINSTANCE hInst, const config_t* cfg) {
    if (!cfg || !cfg->watermark_enabled) {
        watermark_destroy();
        return false;
    }

    /* 水印参数每次都从配置重新取：调用方可能在"应用并保存"后来刷新，
       这时字号/透明度/文本模板/层级都可能已经变了。 */
    s_size    = cfg->watermark_size;
    s_opacity = cfg->watermark_opacity;
    s_bg      = cfg->watermark_background;
    if (s_opacity < 0)   s_opacity = 0;
    if (s_opacity > 255) s_opacity = 255;
    if (s_bg      < 0)   s_bg      = 0;
    if (s_bg      > 255) s_bg      = 255;
    if (s_opacity == 0 && s_bg == 0) {
        /* 全透明等于看不见：与其留个空窗口，不如直接不显示 */
        watermark_destroy();
        return false;
    }

    wm_build_text(cfg);
    if (!s_text[0]) {
        watermark_destroy();
        return false;
    }

    /* 宿主可能已经没了：Explorer 重启会连带销毁挂在它下面的水印窗口
       （父窗口销毁时子窗口一并销毁），此时 s_hwnd 指向一个失效句柄。
       先把这类残留清掉，后面的分支会重新创建。 */
    const bool wantOnTop = cfg->watermark_on_top;
    if (s_hwnd && (!IsWindow(s_hwnd) || (s_host && !IsWindow(s_host)))) {
        LOG_INFO("watermark parent gone (explorer restarted?); recreating");
        watermark_destroy();
    }

    if (s_hwnd && IsWindow(s_hwnd)) {
        /* 已存在：层级变了要重建（样式/父子关系不能就地改），
           其余情况按新参数重画即可。 */
        if (wantOnTop == s_onTop) return wm_paint();
        watermark_destroy();
    }

    s_onTop = wantOnTop;
    return wm_create_window(hInst);
}

void watermark_refresh(void) {
    if (s_hwnd && IsWindow(s_hwnd)) wm_paint();
}

HWND watermark_hwnd(void) {
    return s_hwnd;
}

