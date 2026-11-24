/*
 * icon_grabber.c - 把 exe 拖到本程序上，提取其主图标保存到 config.ini 指定的目录。
 *
 * 编译：gcc -municode -mwindows -O2 -o IconGrabber.exe icon_grabber.c
 * 说明：-mwindows 无控制台；用 WriteConsoleW 在附加的控制台输出诊断信息。
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <shlwapi.h>

/* 依赖库由编译命令行显式链接：-lshlwapi -lversion -ladvapi32 -lcomctl32 */

/* ------------------------- 控制台宽字符输出 ------------------------- */
static void say(const wchar_t *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0, mode = 0;
    if (!h || h == INVALID_HANDLE_VALUE) return;
    /* 控制台必须 WriteConsoleW；重定向/管道时才转 UTF-8 字节写 */
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s, (DWORD)wcslen(s), &written, NULL);
    } else {
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        if (n > 0) {
            char *buf = (char *)malloc((size_t)n);
            if (buf) {
                WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, n, NULL, NULL);
                WriteFile(h, buf, (DWORD)(n - 1), &written, NULL);
                free(buf);
            }
        }
    }
}

static void sayf(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);
    say(buf);
}

/* ------------------------- 反馈窗口 -------------------------
 * 自绘窗口替代 MessageBox：按钮用 BS_FLAT 做扁平样式，图标用 I.ico。
 */
#define NOTIFY_BTN   1002
#define NOTIFY_ICON  1003
#define NOTIFY_TEXT  1004

typedef struct {
    BOOL done;
    const wchar_t *text;
} NotifyCtx;

static LRESULT CALLBACK notify_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        NotifyCtx *c = (NotifyCtx *)((CREATESTRUCT *)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
        HINSTANCE hi = ((CREATESTRUCT *)lp)->hInstance;
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND icon = CreateWindowExW(0, L"Static", NULL, WS_CHILD | WS_VISIBLE | SS_ICON,
                                    16, 16, 48, 48, h, (HMENU)NOTIFY_ICON, hi, NULL);
        SendMessageW(icon, STM_SETICON, (WPARAM)LoadIconW(hi, MAKEINTRESOURCEW(1)), 0);

        HWND text = CreateWindowExW(0, L"Static", c->text,
                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                        76, 18, 368, 200, h, (HMENU)NOTIFY_TEXT, hi, NULL);
        SendMessageW(text, WM_SETFONT, (WPARAM)font, TRUE);

        HWND btn = CreateWindowExW(0, L"Button", L"确定",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_FLAT | BS_DEFPUSHBUTTON,
                        0, 0, 88, 26, h, (HMENU)NOTIFY_BTN, hi, NULL);
        SendMessageW(btn, WM_SETFONT, (WPARAM)font, TRUE);
        SetFocus(btn);
        RECT rc;
        GetClientRect(h, &rc);
        SetWindowPos(btn, NULL, (rc.right - 88) / 2, rc.bottom - 38, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == NOTIFY_BTN) {
            NotifyCtx *c = (NotifyCtx *)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (c) c->done = TRUE;
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE: {
        NotifyCtx *c = (NotifyCtx *)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (c) c->done = TRUE;
        DestroyWindow(h);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_RETURN) {
            NotifyCtx *c = (NotifyCtx *)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (c) c->done = TRUE;
            DestroyWindow(h);
        }
        return 0;
    case WM_CTLCOLORSTATIC: /* 文字区背景跟随窗口，默认 Static 是灰底会突兀 */
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------- 右键菜单管理窗口 -------------------------
 * -i / /i / i 都打开它：一个状态标签 + 安装 / 卸载两个扁平按钮。
 */
static const wchar_t *MENU_KEY = L"exefile\\shell\\IconGrabber";
static BOOL reg_write_menu(const wchar_t *exePath, const wchar_t *icoPath);
static BOOL reg_remove_menu(void);

#define MM_STATUS   1010
#define MM_INSTALL  1011
#define MM_UNINSTALL 1012
#define MM_CLOSE    1013

typedef struct {
    BOOL done;
    const wchar_t *exePath;
} MenuMgrCtx;

static void mm_refresh(HWND h, MenuMgrCtx *c, const wchar_t *extra)
{
    wchar_t status[512];
    HKEY hk;
    BOOL installed = (RegOpenKeyExW(HKEY_CLASSES_ROOT, MENU_KEY, 0,
                                    KEY_READ, &hk) == ERROR_SUCCESS);
    if (installed) RegCloseKey(hk);

    swprintf(status, 512,
             L"当前状态：%s\n\n程序：%s%s%s",
             installed ? L"已安装右键菜单" : L"未安装右键菜单",
             c->exePath,
             extra ? L"\n\n" : L"",
             extra ? extra : L"");
    SetWindowTextW(GetDlgItem(h, MM_STATUS), status);
    EnableWindow(GetDlgItem(h, MM_INSTALL), !installed);
    EnableWindow(GetDlgItem(h, MM_UNINSTALL), installed);
}

static LRESULT CALLBACK mm_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        MenuMgrCtx *c = (MenuMgrCtx *)((CREATESTRUCT *)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
        HINSTANCE hi = ((CREATESTRUCT *)lp)->hInstance;
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND icon = CreateWindowExW(0, L"Static", NULL, WS_CHILD | WS_VISIBLE | SS_ICON,
                                    16, 16, 48, 48, h, NULL, hi, NULL);
        SendMessageW(icon, STM_SETICON, (WPARAM)LoadIconW(hi, MAKEINTRESOURCEW(1)), 0);

        HWND st = CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  76, 18, 368, 90, h, (HMENU)MM_STATUS, hi, NULL);
        SendMessageW(st, WM_SETFONT, (WPARAM)font, TRUE);

        struct { const wchar_t *t; int id; } btns[] = {
            { L"安装右键菜单", MM_INSTALL },
            { L"卸载右键菜单", MM_UNINSTALL },
            { L"关闭",         MM_CLOSE },
        };
        for (int i = 0; i < 3; i++) {
            HWND b = CreateWindowExW(0, L"Button", btns[i].t,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_FLAT,
                        0, 0, 100, 26, h, (HMENU)(INT_PTR)btns[i].id, hi, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
        }
        return 0;
    }
    case WM_SIZE: {
        int cw = LOWORD(lp), ch = HIWORD(lp);
        MoveWindow(GetDlgItem(h, MM_STATUS), 76, 18, cw - 92, 90, TRUE);
        HWND b1 = GetDlgItem(h, MM_INSTALL), b2 = GetDlgItem(h, MM_UNINSTALL),
             b3 = GetDlgItem(h, MM_CLOSE);
        int y = ch - 38, w = 108, gap = 10;
        int totalW = w * 3 + gap * 2;
        int x = (cw - totalW) / 2;
        MoveWindow(b1, x, y, w, 26, TRUE);
        MoveWindow(b2, x + w + gap, y, w, 26, TRUE);
        MoveWindow(b3, x + (w + gap) * 2, y, w, 26, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        MenuMgrCtx *c = (MenuMgrCtx *)GetWindowLongPtrW(h, GWLP_USERDATA);
        int id = LOWORD(wp);
        if (id == MM_CLOSE) {
            if (c) c->done = TRUE;
            DestroyWindow(h);
        } else if (id == MM_INSTALL) {
            wchar_t ico[MAX_PATH], dir[MAX_PATH];
            wcscpy(dir, c->exePath);
            PathRemoveFileSpecW(dir);
            swprintf(ico, MAX_PATH, L"%s\\I.ico", dir);
            BOOL ok = PathFileExistsW(ico) ? reg_write_menu(c->exePath, ico)
                                           : reg_write_menu(c->exePath, NULL);
            mm_refresh(h, c, ok ? L"已安装。" : L"安装失败，可能没有管理员权限。");
        } else if (id == MM_UNINSTALL) {
            BOOL ok = reg_remove_menu();
            mm_refresh(h, c, ok ? L"已卸载。" : L"卸载失败，可能没有管理员权限。");
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE: {
        MenuMgrCtx *c = (MenuMgrCtx *)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (c) c->done = TRUE;
        DestroyWindow(h);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            MenuMgrCtx *c = (MenuMgrCtx *)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (c) c->done = TRUE;
            DestroyWindow(h);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void menu_manager(const wchar_t *exePath)
{
    int w = 460, h = 170;
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_BORDER;
    RECT rc = { 0, 0, w, h };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HINSTANCE hinst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = mm_wndproc;
    wc.hInstance = hinst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"IconGrabberMenuMgr";
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    MenuMgrCtx ctx;
    ctx.done = FALSE;
    ctx.exePath = exePath;
    HWND hwnd = CreateWindowExW(0, L"IconGrabberMenuMgr", L"右键菜单管理", style,
                                 x, y, w, h, NULL, NULL, hinst, &ctx);
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    mm_refresh(hwnd, &ctx, NULL);

    while (!ctx.done && IsWindow(hwnd)) {
        MSG msg;
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            Sleep(10);
        }
    }
}

static void notify(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);

    int lines = 1;
    for (const wchar_t *s = buf; *s; s++) if (*s == L'\n') lines++;
    int contentH = 16 + (lines * 20 > 48 ? lines * 20 : 48) + 14 + 26 + 12;
    int w = 460, h = contentH;
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_BORDER;
    RECT rc = { 0, 0, w, h };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HINSTANCE hinst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = notify_wndproc;
    wc.hInstance = hinst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"IconGrabberNotify";
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    NotifyCtx ctx;
    ctx.done = FALSE;
    ctx.text = buf;
    HWND hwnd = CreateWindowExW(0, L"IconGrabberNotify", L"图标获取器", style,
                                 x, y, w, h, NULL, NULL, hinst, &ctx);
    if (!hwnd) {
        MessageBoxW(NULL, buf, L"图标获取器", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(GetDlgItem(hwnd, NOTIFY_BTN));

    while (!ctx.done && IsWindow(hwnd)) {
        MSG msg;
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            Sleep(10);
        }
    }
}

/* ------------------------- 进度对话框 -------------------------
 * 一个顶层窗口：静态文本显示当前文件，进度条按总文件数推进。
 * 无控制台也能看见，处理完自动销毁。
 */
typedef struct {
    HWND hwnd;
    HWND bar;
    HWND label;   /* 成功部分 */
    HWND label2;  /* 失败部分，失败数>0 时标红 */
    int total;
    int cur;
    BOOL failRed;
} Progress;

#define PROG_LABEL 1001
#define PROG_LABEL2 1002

/* 按控件当前字型量文本宽度，用于把两个标签首尾拼接 */
static int prog_text_width(HWND ctl, const wchar_t *s)
{
    HDC dc = GetDC(ctl);
    HFONT old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(ctl, WM_GETFONT, 0, 0));
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    ReleaseDC(ctl, dc);
    return sz.cx;
}

static LRESULT CALLBACK prog_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE) {
        Progress *p = (Progress *)((CREATESTRUCT *)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
        p->hwnd = h;
        p->label = CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE,
                                   10, 12, 10, 22, h, (HMENU)PROG_LABEL,
                                   NULL, NULL);
        SendMessageW(p->label, WM_SETFONT,
                     (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        p->label2 = CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE,
                                    10, 12, 10, 22, h, (HMENU)PROG_LABEL2,
                                    NULL, NULL);
        SendMessageW(p->label2, WM_SETFONT,
                     (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        p->bar = CreateWindowExW(0, PROGRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE,
                                 10, 44, 10, 26, h, NULL, NULL, NULL);
        SendMessageW(p->bar, PBM_SETRANGE32, 0, p->total);
        SendMessageW(p->bar, PBM_SETPOS, 0, 0);
        return 0;
    }
    if (msg == WM_SIZE) { /* WM_CREATE 时客户区还没定，尺寸变化时重排子控件 */
        Progress *p = (Progress *)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (p) {
            int cw = LOWORD(lp), ch = HIWORD(lp);
            MoveWindow(p->label, 10, 10, cw - 20, 22, TRUE);
            MoveWindow(p->bar, 10, 38, cw - 20, 24, TRUE);
            (void)ch;
        }
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC) { /* 标签背景跟随窗口；失败行标红 */
        Progress *p = (Progress *)GetWindowLongPtrW(h, GWLP_USERDATA);
        HWND ctl = (HWND)lp;
        SetBkMode((HDC)wp, TRANSPARENT);
        if (p && p->failRed && ctl == p->label2)
            SetTextColor((HDC)wp, RGB(200, 0, 0));
        else
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(h);
        return 0;
    }
    if (msg == WM_DESTROY) {
        return 0; /* 不 PostQuitMessage：否则随后的完成弹窗会被 WM_QUIT 立即关掉 */
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 窗口居中（按客户区尺寸算，非客户区差异很小，忽略） */
static int prog_center_x(int w) { return (GetSystemMetrics(SM_CXSCREEN) - w) / 2; }
static int prog_center_y(int h) { return (GetSystemMetrics(SM_CYSCREEN) - h) / 2; }

static void prog_create(Progress *p, HINSTANCE hinst, int total)
{
    ZeroMemory(p, sizeof(*p));
    p->total = total;

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = prog_wndproc;
    wc.hInstance = hinst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"IconGrabberProgress";
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    int w = 420, h = 84; /* 标签 22 + 间距 + 进度条 24，不留大片空白 */
    RECT adj = { 0, 0, w, h };
    AdjustWindowRectEx(&adj, WS_CAPTION | WS_SYSMENU | WS_BORDER, FALSE, 0);
    w = adj.right - adj.left;
    h = adj.bottom - adj.top;

    p->hwnd = CreateWindowExW(0, L"IconGrabberProgress",
                  L"正在提取图标...", WS_CAPTION | WS_SYSMENU | WS_BORDER,
                  prog_center_x(w), prog_center_y(h), w, h, NULL, NULL, hinst, p);
    ShowWindow(p->hwnd, SW_SHOW);
    UpdateWindow(p->hwnd);
}

static void prog_step(Progress *p, const wchar_t *text)
{
    if (!p->hwnd) return;
    if (text) {
        SetWindowTextW(p->label, text);
        SetWindowTextW(p->label2, L""); /* 进行中不显示失败段 */
        p->failRed = FALSE;
    }
    SendMessageW(p->bar, PBM_SETPOS, (WPARAM)(++p->cur), 0);
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* 完成后停留在屏幕上的毫秒数，期间继续处理消息，窗口不假死。
   文案拆两段：成功段正常色，失败段在 fail>0 时标红并紧跟其后。 */
static void prog_hold(Progress *p, int ok, int fail, int ms)
{
    if (!p->hwnd) return;
    if (ms < 0) ms = 0;         /* ini 写负数会被转成 DWORD 巨值，等于卡死 */
    if (ms > 60000) ms = 60000; /* 上限 1 分钟，避免误配导致长驻 */

    wchar_t head[128], tail[128];
    swprintf(head, 128, L"完成：成功 %d 个", ok);
    if (fail > 0)
        swprintf(tail, 128, L"，失败 %d 个。", fail);
    else
        swprintf(tail, 128, L"，失败 0 个。");

    p->failRed = (fail > 0);
    SetWindowTextW(p->label, head);
    SetWindowTextW(p->label2, tail);

    int x = 10 + prog_text_width(p->label, head);
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    int w2 = rc.right - 10 - x;
    if (w2 < 10) w2 = 10;
    SetWindowPos(p->label2, NULL, x, 10, w2, 22, SWP_NOZORDER);
    InvalidateRect(p->label2, NULL, TRUE);

    SendMessageW(p->bar, PBM_SETPOS, (WPARAM)p->total, 0);
    DWORD start = GetTickCount();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        /* 无符号差值：GetTickCount 约 49.7 天回绕，直接比大小会算错 */
        if ((DWORD)(GetTickCount() - start) >= (DWORD)ms) break;
        Sleep(20);
    }
    if (p->hwnd) DestroyWindow(p->hwnd);
    p->hwnd = NULL;
}

/* ------------------------- INI 读取 ------------------------- */
static void ini_string(const wchar_t *file, const wchar_t *key,
                       const wchar_t *def, wchar_t *out, DWORD cch)
{
    GetPrivateProfileStringW(L"Settings", key, def, out, cch, file);
}

static int ini_int(const wchar_t *file, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(L"Settings", key, def, file);
}

/* ------------------------- PE 资源（RT_GROUP_ICON / RT_ICON） -------------------------
 * 用 LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE) 加载目标 exe，再用 FindResource/LoadResource
 * 读取图标资源。这样不会执行目标代码，也不要求目标是有效可运行程序。
 */
#pragma pack(push, 1)
typedef struct {
    BYTE bWidth, bHeight, bColorCount, bReserved;
    WORD wPlanes, wBitCount;
    DWORD dwBytesInRes, dwImageOffset;
} ICONDIRENTRY;

typedef struct {
    WORD idReserved, idType, idCount;
} ICONDIR;

/* GRPICONENTRY：组图标目录项（14 字节），末字段 nID 指向 RT_ICON 资源 ID。
   前 12 字节与 ICONDIRENTRY 一致，区别在最后是 WORD nID 而非 DWORD dwImageOffset。 */
typedef struct {
    BYTE bWidth, bHeight, bColorCount, bReserved;
    WORD wPlanes, wBitCount;
    DWORD dwBytesInRes;
    WORD nID;
} GRPICONENTRY;
#pragma pack(pop)

/* 单个 ico 允许的图像数据总上限：正常图标远低于此，拦住畸形/恶意 PE 的 32 位溢出 */
#define ICON_BYTES_LIMIT (64u * 1024u * 1024u)

/* 从 PE 句柄找出第一个组图标的资源 ID（MAKELANGID(0,0) 语言中性）。
   lpType 回调约定：MAKELONG(type, name) 传回；高位为 0 表示整数 ID。 */
typedef struct {
    HMODULE mod;
    BOOL found;
    BOOL isInt;
    WORD intId;
    WCHAR strName[256];
} EnumCtx;

static BOOL CALLBACK find_group(HMODULE mod, LPCWSTR lpType, LPWSTR lpName, LONG_PTR lParam)
{
    EnumCtx *ctx = (EnumCtx *)lParam;
    (void)mod;
    if (IS_INTRESOURCE(lpType) && (DWORD_PTR)lpType == (DWORD_PTR)RT_GROUP_ICON) {
        ctx->found = TRUE;
        if (IS_INTRESOURCE(lpName)) {
            ctx->isInt = TRUE;
            ctx->intId = (WORD)(DWORD_PTR)lpName;
        } else {
            wcsncpy(ctx->strName, lpName, 255);
            ctx->strName[255] = 0;
        }
        return FALSE; /* 找到第一个即停 */
    }
    return TRUE;
}

/* 取资源字节副本，返回 malloc 的缓冲区与大小；失败返回 NULL。 */
static void *load_resource_bytes(HMODULE mod, LPCWSTR type, LPCWSTR name, DWORD *outSize)
{
    HRSRC hRes = FindResourceW(mod, name, type);
    HGLOBAL hMem;
    const void *src;
    DWORD size;
    void *copy;
    if (!hRes) return NULL;
    size = SizeofResource(mod, hRes);
    if (!size) return NULL;
    hMem = LoadResource(mod, hRes);
    if (!hMem) return NULL;
    src = LockResource(hMem);
    if (!src) return NULL; /* 畸形 PE 下 LockResource 会失败，绝不能直接喂给 memcpy */
    copy = malloc(size);
    if (!copy) return NULL;
    memcpy(copy, src, size);
    if (outSize) *outSize = size;
    return copy;
}

/* 从 PE 提取主图标「完整组」（所有分辨率/位深），写成多图 .ico 文件。成功 TRUE。 */
static BOOL extract_icon_from_pe(const wchar_t *pePath, const wchar_t *outIco)
{
    BOOL ok = FALSE;
    HMODULE mod = LoadLibraryExW(pePath, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!mod) {
        sayf(L"  [!] 无法以数据方式加载：%s (err=%lu)\n", pePath, GetLastError());
        return FALSE;
    }

    EnumCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.mod = mod;
    EnumResourceNamesW(mod, RT_GROUP_ICON, find_group, (LONG_PTR)&ctx);
    if (!ctx.found) {
        ctx.found = TRUE;
        ctx.isInt = TRUE;
        ctx.intId = 1; /* 回退：约定主组图标 ID 为 1 */
    }

    LPCWSTR groupName = ctx.isInt ? (LPCWSTR)(DWORD_PTR)ctx.intId : ctx.strName;
    DWORD grpSize = 0;
    void *grp = load_resource_bytes(mod, RT_GROUP_ICON, groupName, &grpSize);
    if (!grp || grpSize < sizeof(ICONDIR) + sizeof(GRPICONENTRY)) {
        say(L"  [!] 主组图标 (RT_GROUP_ICON) 读取失败\n");
        free(grp);
        FreeLibrary(mod);
        return FALSE;
    }

    ICONDIR *dir = (ICONDIR *)grp;
    WORD count = dir->idCount;
    if (count == 0 || grpSize < sizeof(ICONDIR) + (DWORD)count * sizeof(GRPICONENTRY)) {
        free(grp);
        FreeLibrary(mod);
        return FALSE;
    }
    GRPICONENTRY *entries = (GRPICONENTRY *)((BYTE *)grp + sizeof(ICONDIR));

    /* 先把组内每张 RT_ICON 读进内存 */
    void **imgs = (void **)calloc(count, sizeof(void *));
    DWORD *sizes = (DWORD *)calloc(count, sizeof(DWORD));
    if (!imgs || !sizes) {
        free(imgs); free(sizes); free(grp); FreeLibrary(mod);
        return FALSE;
    }
    DWORD dataTotal = 0;
    BOOL overflow = FALSE;
    for (int i = 0; i < count; i++) {
        DWORD sz = 0;
        imgs[i] = load_resource_bytes(mod, RT_ICON,
                     (LPCWSTR)(DWORD_PTR)entries[i].nID, &sz);
        sizes[i] = sz;
        /* 32 位累加会回绕：畸形 PE 可借此让 calloc 偏小、后续 memcpy 越界写 */
        if (sz > ICON_BYTES_LIMIT - dataTotal) { overflow = TRUE; break; }
        dataTotal += sz;
    }
    if (overflow) {
        for (int i = 0; i < count; i++) free(imgs[i]);
        free(imgs); free(sizes); free(grp); FreeLibrary(mod);
        say(L"  [!] 图标数据超出上限，已放弃\n");
        return FALSE;
    }

    /* ico = ICONDIR + count*ICONDIRENTRY + 所有图像数据 */
    DWORD total = sizeof(ICONDIR) + (DWORD)count * sizeof(ICONDIRENTRY) + dataTotal;
    BYTE *ico = (BYTE *)calloc(1, total);
    if (ico) {
        ICONDIR outDir = { 0, 1, count };
        memcpy(ico, &outDir, sizeof(outDir));
        DWORD offset = sizeof(ICONDIR) + (DWORD)count * sizeof(ICONDIRENTRY);
        int kept = 0;
        for (int i = 0; i < count; i++) {
            if (!imgs[i] || !sizes[i]) continue;
            ICONDIRENTRY e;
            ZeroMemory(&e, sizeof(e));
            /* 宽高直接取组项 bWidth/bHeight（0 表示 256） */
            e.bWidth  = entries[i].bWidth;
            e.bHeight = entries[i].bHeight;
            e.bColorCount = entries[i].bColorCount;
            e.wPlanes    = entries[i].wPlanes;
            e.wBitCount  = entries[i].wBitCount;
            e.dwBytesInRes  = sizes[i];
            e.dwImageOffset = offset;
            memcpy(ico + sizeof(ICONDIR) + (DWORD)kept * sizeof(ICONDIRENTRY), &e, sizeof(e));
            memcpy(ico + offset, imgs[i], sizes[i]);
            offset += sizes[i];
            kept++;
        }
        ((ICONDIR *)ico)->idCount = (WORD)kept;

        HANDLE f = CreateFileW(outIco, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, ico, total, &written, NULL);
            CloseHandle(f);
            ok = (written == total);
            sayf(L"  [OK] 组图标含 %d 张尺寸\n", kept);
        } else {
            sayf(L"  [!] 写入失败：%s (err=%lu)\n", outIco, GetLastError());
        }
        free(ico);
    }

    for (int i = 0; i < count; i++) free(imgs[i]);
    free(imgs);
    free(sizes);
    free(grp);
    FreeLibrary(mod);
    return ok;
}

/* ------------------------- 文件版本资源 -> 产品名 ------------------------- */
static void get_product_name(const wchar_t *path, wchar_t *out, DWORD cch)
{
    *out = 0;
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return;
    void *buf = malloc(size);
    if (!buf) return;
    if (GetFileVersionInfoW(path, 0, size, buf)) {
        UINT len = 0;
        struct { WORD wLanguage, wCodePage; } *lang = NULL;
        if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&lang, &len) && len >= 4) {
            wchar_t sub[128];
            swprintf(sub, 128, L"\\StringFileInfo\\%04x%04x\\ProductName",
                     lang->wLanguage, lang->wCodePage);
            wchar_t *val = NULL;
            if (VerQueryValueW(buf, sub, (void **)&val, &len) && val && len) {
                wcsncpy(out, val, cch - 1);
                out[cch - 1] = 0;
            }
        }
    }
    free(buf);
}

/* 文件名非法字符 -> 下划线 */
static void sanitize(wchar_t *s)
{
    for (; *s; s++) {
        if (wcschr(L"\\/:*?\"<>|", *s)) *s = L'_';
    }
}

/* ------------------------- 右键菜单安装/卸载 -------------------------
 * 快捷键用菜单名里的 & 实现：Windows shell 右键菜单把 "&" 后面的字母当加速键。
 * 命令 "-grab" 是内部标记，后面跟所点 exe 的完整路径（带引号）。
 */
static BOOL reg_write_menu(const wchar_t *exePath, const wchar_t *icoPath)
{
    HKEY hk = NULL;
    DWORD disp = 0;
    BOOL ok = FALSE;
    wchar_t cmd[MAX_PATH * 2];
    if (swprintf(cmd, MAX_PATH * 2, L"\"%s\" -grab \"%%1\"", exePath) < 0)
        return FALSE; /* 截断的命令会指向错误路径，宁可不装 */

    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, MENU_KEY, 0, NULL, 0,
                        KEY_WRITE, NULL, &hk, &disp) != ERROR_SUCCESS)
        return FALSE;
    /* 任一次写入失败都必须报失败，否则管理窗会谎报"已安装" */
    ok = (RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)L"提取主图标 (&I)",
            (DWORD)((wcslen(L"提取主图标 (&I)") + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    /* 菜单项图标：使用同目录 I.ico */
    if (ok && icoPath)
        ok = (RegSetValueExW(hk, L"Icon", 0, REG_SZ, (const BYTE *)icoPath,
                (DWORD)((wcslen(icoPath) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    RegCloseKey(hk);
    if (!ok) {
        RegDeleteTreeW(HKEY_CLASSES_ROOT, MENU_KEY); /* 回滚半成品键 */
        return FALSE;
    }

    wchar_t sub[MAX_PATH];
    swprintf(sub, MAX_PATH, L"%s\\command", MENU_KEY);
    hk = NULL;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, sub, 0, NULL, 0,
                        KEY_WRITE, NULL, &hk, &disp) == ERROR_SUCCESS) {
        ok = (RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)cmd,
               (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
        RegCloseKey(hk);
    }
    return ok;
}

static BOOL reg_remove_menu(void)
{
    LONG r = RegDeleteTreeW(HKEY_CLASSES_ROOT, MENU_KEY);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

/* ------------------------- 主流程 ------------------------- */
int wmain(int argc, wchar_t **argv)
{
    /* 右键菜单调用：本程序 "-grab" "<exe路径>"，跳过开关当普通文件处理 */
    int start = 1;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t exeDir0[MAX_PATH];
    wcscpy(exeDir0, exePath);
    PathRemoveFileSpecW(exeDir0);

    if (argc >= 2 && wcscmp(argv[1], L"-grab") == 0) {
        start = 2;
    } else if (argc >= 2 && (wcscmp(argv[1], L"-i") == 0 ||
                             wcscmp(argv[1], L"/i") == 0 ||
                             _wcsicmp(argv[1], L"i") == 0)) {
        menu_manager(exePath);
        return 0;
    }

    if (argc <= start) {
        menu_manager(exePath); /* 无参数：直接打开管理窗口 */
        return 0;
    }

    wchar_t iniPath[MAX_PATH];
    swprintf(iniPath, MAX_PATH, L"%s\\config.ini", exeDir0);

    wchar_t outCfg[MAX_PATH];
    ini_string(iniPath, L"OutputDir", L"icons", outCfg, MAX_PATH);
    int overwrite = ini_int(iniPath, L"Overwrite", 1);
    int holdMs = ini_int(iniPath, L"ProgressHoldMs", 1000);
    int showFinish = ini_int(iniPath, L"ShowFinishDialog", 0);

    /* 解析输出目录：相对路径以 exe 所在目录为基准 */
    wchar_t outDir[MAX_PATH];
    if (PathIsRelativeW(outCfg)) {
        if (swprintf(outDir, MAX_PATH, L"%s\\%s", exeDir0, outCfg) < 0) {
            notify(L"配置错误：OutputDir 拼出的路径过长，已中止。\n\n目录：%s", exeDir0);
            return 1;
        }
    } else {
        wcsncpy(outDir, outCfg, MAX_PATH - 1);
        outDir[MAX_PATH - 1] = 0;
    }
    CreateDirectoryW(outDir, NULL);

    int total = argc - start;
    Progress prog;
    prog_create(&prog, GetModuleHandleW(NULL), total);

    int success = 0, fail = 0;
    for (int i = start; i < argc; i++) {
        wchar_t *src = argv[i];
        wchar_t status[512];
        swprintf(status, 512, L"[%d/%d] %s。", i - start + 1, total, PathFindFileNameW(src));
        if (!PathFileExistsW(src)) {
            sayf(L"[!] 文件不存在：%s\n", src);
            fail++;
            prog_step(&prog, status);
            continue;
        }
        sayf(L"[*] 处理：%s\n", src);

        wchar_t name[MAX_PATH];
        get_product_name(src, name, MAX_PATH);
        if (!*name) {
            const wchar_t *base = PathFindFileNameW(src);
            if (!base || !*base) base = L"icon"; /* 盘符根等异常输入 */
            wcsncpy(name, base, MAX_PATH - 1);
            name[MAX_PATH - 1] = 0;
            PathRemoveExtensionW(name);
            if (!*name) wcscpy(name, L"icon");
        }
        sanitize(name);
        /* 文件名留出目录 + "\" + ".ico" + NUL 的余量，否则拼 outIco 必溢出 */
        if (wcslen(name) > 100) name[100] = 0;

        wchar_t outIco[MAX_PATH];
        if (swprintf(outIco, MAX_PATH, L"%s\\%s.ico", outDir, name) < 0) {
            sayf(L"  [!] 文件名过长，跳过：%s\n", src);
            fail++;
            prog_step(&prog, status);
            continue;
        }
        if (!overwrite && PathFileExistsW(outIco)) {
            sayf(L"  [=] 已存在，跳过：%s\n", outIco);
        } else if (extract_icon_from_pe(src, outIco)) {
            sayf(L"  [OK] 已保存：%s\n", outIco);
            success++;
        } else {
            say(L"  [X] 未找到可提取的图标\n");
            fail++;
        }
        prog_step(&prog, status);
    }

    prog_hold(&prog, success, fail, holdMs);
    if (showFinish)
        notify(L"完成：成功 %d，失败 %d。\n\n图标目录：%s", success, fail, outDir);
    return 0;
}
