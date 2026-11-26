/*
 * GroupLauncher - macOS-style app groups ("stacks") for the Windows taskbar.
 * Copyright (C) 2026 sotadevop
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details: <https://www.gnu.org/licenses/>.
 */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000006
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <strsafe.h>
#include <wctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CLS_NAME   L"GroupLauncherPopup"
#define APP_NAME   L"GroupLauncher"
#define MAX_ITEMS  64
#define OPEN_MS    180.0
#define CLOSE_MS   120.0
#define LINGER_MS  400.0
#define TIMER_ANIM 1
#define TIMER_POLL 2

typedef struct {
    WCHAR path[MAX_PATH];
    WCHAR name[MAX_PATH];
    DWORD *icon; /* premultiplied BGRA, icon size x icon size */
} Item;

static Item  g_items[MAX_ITEMS];
static int   g_count;
static WCHAR g_folder[MAX_PATH];
static HWND  g_hwnd;

/* Layout in physical pixels (process is Per-Monitor DPI aware v2). */
static double g_scale = 1.0;
static int g_icon, g_cellW, g_cellH, g_pad, g_labelH, g_topPad, g_gapIL, g_radius;
static int g_cols, g_rows, g_w, g_h;
static int g_finalX, g_finalY, g_offX, g_offY, g_curX, g_curY;
static BYTE g_curA;

static HDC     g_memdc;
static HBITMAP g_dib, g_oldbmp;
static DWORD  *g_px;
static BYTE   *g_textcov;
static int     g_sel = -1;
static BOOL    g_dark = TRUE;

/* What to do after the popup window is gone (right-click menu actions). */
enum { ACT_NONE, ACT_RELAUNCH, ACT_ADD, ACT_NEWGROUP };
static int  g_after = ACT_NONE;
static BOOL g_hasAnchor = FALSE;
static int  g_anchorX;

static BOOL add_apps(HWND owner, const WCHAR *group);
static int  g_pinnedUpdated; /* pinned taskbar copies touched by the last update_group_shortcut() */
static void update_group_shortcut(const WCHAR *group, WCHAR *lnkOut);
static int  run_manager(void);

enum { ST_OPENING, ST_OPEN, ST_CLOSING, ST_LINGER };
static int g_state;
static LARGE_INTEGER g_t0, g_freq;
static BOOL g_animate = TRUE, g_wasActive = FALSE;

static const PROPERTYKEY PKEY_AUMID = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

static int S(double v) { return (int)(v * g_scale + 0.5); }

/* ---------- pixel helpers (premultiplied BGRA) ---------- */

static DWORD pm(int r, int g, int b, int a)
{
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (DWORD)(b * a / 255) | ((DWORD)(g * a / 255) << 8) |
           ((DWORD)(r * a / 255) << 16) | ((DWORD)a << 24);
}

static DWORD over(DWORD d, DWORD s)
{
    unsigned sa = s >> 24, inv, b, g, r, a;
    if (sa == 255) return s;
    if (sa == 0) return d;
    inv = 255 - sa;
    b = (s & 0xFF) + ((d & 0xFF) * inv + 127) / 255;
    g = ((s >> 8) & 0xFF) + (((d >> 8) & 0xFF) * inv + 127) / 255;
    r = ((s >> 16) & 0xFF) + (((d >> 16) & 0xFF) * inv + 127) / 255;
    a = sa + ((d >> 24) * inv + 127) / 255;
    if (b > 255) b = 255;
    if (g > 255) g = 255;
    if (r > 255) r = 255;
    if (a > 255) a = 255;
    return b | (g << 8) | (r << 16) | (a << 24);
}

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/* Signed distance to a rounded rectangle; used for anti-aliased edges. */
static float rr_dist(float px, float py, float x0, float y0, float x1, float y1, float r)
{
    float cx = px < x0 + r ? x0 + r : (px > x1 - r ? x1 - r : px);
    float cy = py < y0 + r ? y0 + r : (py > y1 - r ? y1 - r : py);
    float dx = px - cx, dy = py - cy;
    return sqrtf(dx * dx + dy * dy) - r;
}

static void draw_rr(DWORD *buf, int bw, int bh, float x0, float y0, float x1, float y1,
                    float r, float stroke, int R, int G, int B, int A)
{
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0);
    int ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > bw) ix1 = bw;
    if (iy1 > bh) iy1 = bh;
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++) {
            float d = rr_dist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r);
            float c = clamp01(0.5f - d);
            if (stroke > 0) c -= clamp01(0.5f - (d + stroke));
            if (c <= 0) continue;
            buf[y * bw + x] = over(buf[y * bw + x], pm(R, G, B, (int)(A * c + 0.5f)));
        }
}

static void blit(DWORD *dst, int dw, int dh, const DWORD *src, int sw, int sh, int ox, int oy)
{
    for (int y = 0; y < sh; y++) {
        int ty = oy + y;
        if (ty < 0 || ty >= dh) continue;
        for (int x = 0; x < sw; x++) {
            int tx = ox + x;
            if (tx < 0 || tx >= dw) continue;
            dst[ty * dw + tx] = over(dst[ty * dw + tx], src[y * sw + x]);
        }
    }
}

/* Supersampled bilinear resize of a premultiplied image. */
static void resample(const DWORD *src, int sw, int sh, DWORD *dst, int dw, int dh)
{
    float sx = (float)sw / dw, sy = (float)sh / dh;
    int k = (int)ceilf(sx > sy ? sx : sy);
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < k; j++)
                for (int i = 0; i < k; i++) {
                    float fx = (x + (i + 0.5f) / k) * sx - 0.5f;
                    float fy = (y + (j + 0.5f) / k) * sy - 0.5f;
                    if (fx < 0) fx = 0;
                    if (fy < 0) fy = 0;
                    if (fx > sw - 1) fx = (float)(sw - 1);
                    if (fy > sh - 1) fy = (float)(sh - 1);
                    int x0 = (int)fx, y0 = (int)fy;
                    int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
                    float wx = fx - x0, wy = fy - y0;
                    DWORD p[4] = {src[y0 * sw + x0], src[y0 * sw + x1], src[y1 * sw + x0], src[y1 * sw + x1]};
                    float w[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
                    for (int q = 0; q < 4; q++)
                        for (int c = 0; c < 4; c++)
                            acc[c] += ((p[q] >> (c * 8)) & 0xFF) * w[q];
                }
            DWORD out = 0;
            for (int c = 0; c < 4; c++) {
                int v = (int)(acc[c] / (k * k) + 0.5f);
                if (v > 255) v = 255;
                out |= (DWORD)v << (c * 8);
            }
            dst[y * dw + x] = out;
        }
}

/* ---------- icons ---------- */

/* Same icon source Explorer uses; returns a premultiplied size x size image. */
static DWORD *load_icon(const WCHAR *path, int size)
{
    IShellItem *si = NULL;
    IShellItemImageFactory *f = NULL;
    HBITMAP hbm = NULL;
    SIZE sz = {size, size};
    if (FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&si))) return NULL;
    HRESULT hr = IShellItem_QueryInterface(si, &IID_IShellItemImageFactory, (void **)&f);
    IShellItem_Release(si);
    if (FAILED(hr)) return NULL;
    hr = IShellItemImageFactory_GetImage(f, sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm);
    IShellItemImageFactory_Release(f);
    if (FAILED(hr) || !hbm) return NULL;

    BITMAP bm;
    GetObjectW(hbm, sizeof bm, &bm);
    int w = bm.bmWidth, h = abs(bm.bmHeight);
    if (w <= 0 || h <= 0) { DeleteObject(hbm); return NULL; }
    DWORD *src = (DWORD *)malloc((size_t)w * h * 4);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(NULL);
    int ok = GetDIBits(dc, hbm, 0, h, src, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    DeleteObject(hbm);
    if (!ok) { free(src); return NULL; }

    /* Normalize to premultiplied alpha: icons without alpha are opaque,
       straight-alpha images get premultiplied. */
    BOOL anyAlpha = FALSE, straight = FALSE;
    for (int i = 0; i < w * h; i++) {
        DWORD p = src[i];
        unsigned a = p >> 24;
        if (a) anyAlpha = TRUE;
        if ((p & 0xFF) > a || ((p >> 8) & 0xFF) > a || ((p >> 16) & 0xFF) > a) straight = TRUE;
    }
    for (int i = 0; i < w * h; i++) {
        DWORD p = src[i];
        if (!anyAlpha) src[i] = p | 0xFF000000;
        else if (straight) src[i] = pm((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF, p >> 24);
    }

    /* Fit into a square, keeping the aspect ratio. */
    DWORD *out = (DWORD *)calloc((size_t)size * size, 4);
    int dw = size, dh = size;
    if (w > h) dh = (int)((double)size * h / w + 0.5);
    else if (h > w) dw = (int)((double)size * w / h + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    DWORD *tmp = (DWORD *)malloc((size_t)dw * dh * 4);
    resample(src, w, h, tmp, dw, dh);
    blit(out, size, size, tmp, dw, dh, (size - dw) / 2, (size - dh) / 2);
    free(tmp);
    free(src);
    return out;
}

/* ---------- group folder ---------- */

static int cmp_items(const void *a, const void *b)
{
    return StrCmpLogicalW(((const Item *)a)->name, ((const Item *)b)->name);
}

static void enumerate(const WCHAR *folder)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    g_count = 0;
    PathCombineW(pattern, folder, L"*");
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (!lstrcmpiW(fd.cFileName, L"desktop.ini") || !lstrcmpiW(fd.cFileName, L"icon.ico") ||
            !lstrcmpiW(fd.cFileName, L"groupicon.txt") || !lstrcmpiW(fd.cFileName, L"groupid.txt")) continue;
        if (g_count >= MAX_ITEMS) break;
        Item *it = &g_items[g_count++];
        PathCombineW(it->path, folder, fd.cFileName);
        StringCchCopyW(it->name, MAX_PATH, fd.cFileName);
        PathRemoveExtensionW(it->name);
        it->icon = NULL;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(g_items, g_count, sizeof(Item), cmp_items);
}

/* AppUserModelID per group: keeps several pinned groups as separate taskbar buttons.
   The ID is stored in a hidden "groupid.txt" inside the group folder, so it survives renames
   (a pinned group keeps working). Groups from older versions keep the ID they already had. */
static void make_aumid(const WCHAR *folder, WCHAR *out, size_t n)
{
    WCHAR idf[MAX_PATH], id[64];
    DWORD rd = 0;
    ZeroMemory(id, sizeof id);
    PathCombineW(idf, folder, L"groupid.txt");
    HANDLE h = CreateFileW(idf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, id, sizeof id - sizeof(WCHAR), &rd, NULL);
        CloseHandle(h);
        id[rd / sizeof(WCHAR)] = 0;
        for (WCHAR *p = id; *p; p++)
            if (!(*p < 128 && (iswalnum(*p) || *p == L'.'))) { *p = 0; break; }
    }
    if (id[0]) {
        StringCchPrintfW(out, n, L"GroupLauncher.%s", id);
        return;
    }
    DWORD hsh = 2166136261u;
    WCHAR clean[48];
    int k = 0;
    for (const WCHAR *p = folder; *p; p++) {
        hsh ^= (DWORD)towlower(*p);
        hsh *= 16777619u;
    }
    for (const WCHAR *p = PathFindFileNameW(folder); *p && k < 40; p++)
        if (*p < 128 && iswalnum(*p)) clean[k++] = *p;
    clean[k] = 0;
    StringCchPrintfW(id, 64, L"%s.%08X", k ? clean : L"Group", hsh);
    StringCchPrintfW(out, n, L"GroupLauncher.%s", id);
    if (PathIsDirectoryW(folder)) {
        DWORD wr;
        h = CreateFileW(idf, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, id, (DWORD)lstrlenW(id) * sizeof(WCHAR), &wr, NULL);
            CloseHandle(h);
        }
    }
}

/* ---------- dark / light UI that follows Windows' app mode setting ---------- */

#define DWMWA_DARK_TITLEBAR 20
static BOOL g_uiDark;
static HBRUSH g_bgBrush, g_ctlBrush;
static COLORREF g_bgCol, g_ctlCol, g_fgCol;

static void theme_init(void)
{
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        v = 1;
    g_uiDark = (v == 0);
    if (g_uiDark) {
        g_bgCol = RGB(32, 32, 32);   /* Windows 11 dark window background */
        g_ctlCol = RGB(43, 43, 43);  /* lists and text fields */
        g_fgCol = RGB(255, 255, 255);
    } else {
        g_bgCol = RGB(243, 243, 243);
        g_ctlCol = RGB(255, 255, 255);
        g_fgCol = RGB(0, 0, 0);
    }
    g_bgBrush = CreateSolidBrush(g_bgCol);
    g_ctlBrush = CreateSolidBrush(g_ctlCol);

    /* Dark right-click menus: uxtheme's SetPreferredAppMode (ordinal 135) and FlushMenuThemes (136)
       are undocumented but used by many apps (e.g. Notepad++). If they are missing, menus stay light. */
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux) {
        typedef int(WINAPI * SetPreferredAppMode_t)(int);
        typedef void(WINAPI * FlushMenuThemes_t)(void);
        SetPreferredAppMode_t spam = (SetPreferredAppMode_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        FlushMenuThemes_t fmt = (FlushMenuThemes_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (spam) spam(g_uiDark ? 2 /* ForceDark */ : 0 /* Default */);
        if (fmt) fmt();
    }
}

static void theme_window(HWND h)
{
    BOOL dark = g_uiDark;
    DwmSetWindowAttribute(h, DWMWA_DARK_TITLEBAR, &dark, sizeof dark); /* documented since Windows 11 */
}

/* "DarkMode_Explorer" / "DarkMode_CFD" are the dark control styles Explorer itself uses. */
static void theme_control(HWND h)
{
    WCHAR cls[32];
    GetClassNameW(h, cls, 32);
    if (!lstrcmpiW(cls, WC_LISTVIEWW)) {
        SetWindowTheme(h, g_uiDark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        ListView_SetBkColor(h, g_ctlCol);
        ListView_SetTextBkColor(h, g_ctlCol);
        ListView_SetTextColor(h, g_fgCol);
    } else if (g_uiDark && !lstrcmpiW(cls, L"Edit")) {
        SetWindowTheme(h, L"DarkMode_CFD", NULL);
    } else if (g_uiDark && !lstrcmpiW(cls, L"Button")) {
        SetWindowTheme(h, L"DarkMode_Explorer", NULL);
    }
}

static BOOL theme_ctlcolor(UINT m, WPARAM w, LRESULT *out)
{
    HDC dc = (HDC)w;
    if (m != WM_CTLCOLORSTATIC && m != WM_CTLCOLOREDIT && m != WM_CTLCOLORBTN && m != WM_CTLCOLORLISTBOX) return FALSE;
    SetTextColor(dc, g_fgCol);
    if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX) {
        SetBkColor(dc, g_ctlCol);
        *out = (LRESULT)g_ctlBrush;
    } else {
        SetBkColor(dc, g_bgCol);
        *out = (LRESULT)g_bgBrush;
    }
    return TRUE;
}

static HFONT ui_font(double scale)
{
    return CreateFontW(-(int)(14 * scale + 0.5), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static double monitor_scale(HWND owner, MONITORINFO *mi)
{
    POINT pt;
    UINT dx = 96, dy = 96;
    GetCursorPos(&pt);
    HMONITOR mon = owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    mi->cbSize = sizeof *mi;
    GetMonitorInfoW(mon, mi);
    return dx / 96.0;
}

/* Creates a dialog-style window centered on the monitor, sized by its client area. */
static HWND ui_create(HWND owner, const WCHAR *cls, WNDPROC proc, const WCHAR *title, int cw, int ch, double scale,
                      const MONITORINFO *mi, void *param, DWORD style)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    DWORD ex = (style & WS_MINIMIZEBOX) ? 0 : (WS_EX_DLGMODALFRAME | (owner ? 0 : WS_EX_TOPMOST));
    RECT rc = {0, 0, cw, ch};
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, (UINT)(scale * 96 + 0.5));
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HWND hw = CreateWindowExW(ex, cls, title, style, (mi->rcWork.left + mi->rcWork.right - w) / 2,
                              (mi->rcWork.top + mi->rcWork.bottom - h) / 2, w, h, owner, NULL, wc.hInstance, param);
    if (hw) theme_window(hw);
    return hw;
}

/* Runs a window modally: the owner is disabled until it closes. */
static void ui_modal_loop(HWND owner, HWND hw, HWND focus)
{
    MSG msg;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hw, SW_SHOW);
    SetForegroundWindow(hw);
    if (focus) SetFocus(focus);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsWindow(hw) || !IsDialogMessageW(hw, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
}

/* Dark message box (the standard MessageBox can't be themed). Supports MB_OK and MB_YESNO. */
typedef struct {
    UINT flags;
    int result;
} MsgState;

static LRESULT CALLBACK msg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    MsgState *st = (MsgState *)GetWindowLongPtrW(h, GWLP_USERDATA);
    LRESULT r;
    if (theme_ctlcolor(m, w, &r)) return r;
    switch (m) {
    case WM_CREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDCANCEL) id = ((st->flags & 0xF) == MB_YESNO) ? IDNO : IDOK;
        st->result = id;
        DestroyWindow(h);
        return 0;
    }
    case WM_CLOSE:
        st->result = ((st->flags & 0xF) == MB_YESNO) ? IDNO : IDOK;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int ui_msg(HWND owner, const WCHAR *text, const WCHAR *caption, UINT flags)
{
    MsgState st = {flags, IDOK};
    MONITORINFO mi;
    double sc = monitor_scale(owner, &mi);
    HFONT font = ui_font(sc);
#define D(v) ((int)((v) * sc + 0.5))
    RECT tr = {0, 0, D(380), 0};
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    int pad = D(20), bw = D(86), bh = D(30), gap = D(8);
    int cw = D(380) + 2 * pad, ch = pad + tr.bottom + pad + bh + pad;
    HWND hw = ui_create(owner, L"GroupLauncherMsg", msg_proc, caption, cw, ch, sc, &mi, &st, WS_CAPTION | WS_SYSMENU);
    if (!hw) { DeleteObject(font); return MessageBoxW(owner, text, caption, flags); }
    HWND c[3], def;
    int n = 0;
    c[n++] = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, pad, pad, D(380), tr.bottom,
                             hw, NULL, NULL, NULL);
    if ((flags & 0xF) == MB_YESNO) {
        BOOL noDefault = (flags & MB_DEFBUTTON2) != 0;
        c[n++] = CreateWindowExW(0, L"BUTTON", L"Yes", WS_CHILD | WS_VISIBLE | WS_TABSTOP | (noDefault ? 0 : BS_DEFPUSHBUTTON),
                                 cw - pad - 2 * bw - gap, ch - pad - bh, bw, bh, hw, (HMENU)IDYES, NULL, NULL);
        c[n++] = CreateWindowExW(0, L"BUTTON", L"No", WS_CHILD | WS_VISIBLE | WS_TABSTOP | (noDefault ? BS_DEFPUSHBUTTON : 0),
                                 cw - pad - bw, ch - pad - bh, bw, bh, hw, (HMENU)IDNO, NULL, NULL);
        def = noDefault ? c[2] : c[1];
    } else {
        c[n++] = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                 cw - pad - bw, ch - pad - bh, bw, bh, hw, (HMENU)IDOK, NULL, NULL);
        def = c[1];
    }
#undef D
    for (int i = 0; i < n; i++) {
        SendMessageW(c[i], WM_SETFONT, (WPARAM)font, TRUE);
        theme_control(c[i]);
    }
    ui_modal_loop(owner, hw, def);
    DeleteObject(font);
    return st.result;
}

/* ---------- popup ---------- */

static void layout(void)
{
    g_icon = S(48);
    g_cellW = S(88);
    g_topPad = S(10);
    g_gapIL = S(6);
    g_labelH = S(18);
    g_cellH = g_topPad + g_icon + g_gapIL + g_labelH + S(8);
    g_pad = S(12);
    g_radius = S(14);
    if (g_count <= 4) g_cols = g_count;
    else {
        g_cols = (int)ceil(sqrt((double)g_count));
        if (g_cols < 4) g_cols = 4;
        if (g_cols > 6) g_cols = 6;
    }
    g_rows = (g_count + g_cols - 1) / g_cols;
    g_w = g_cols * g_cellW + 2 * g_pad;
    g_h = g_rows * g_cellH + 2 * g_pad;
}

static void cell_origin(int i, int *x, int *y)
{
    *x = g_pad + (i % g_cols) * g_cellW;
    *y = g_pad + (i / g_cols) * g_cellH;
}

static void render_text_layer(void)
{
    BITMAPINFO bi;
    void *bits = NULL;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_w;
    bi.bmiHeader.biHeight = -g_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ oldb = SelectObject(dc, bmp);
    ZeroMemory(bits, (size_t)g_w * g_h * 4);
    /* Grayscale anti-aliasing so the result can be used as an alpha mask. */
    HFONT font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < g_count; i++) {
        int x, y;
        cell_origin(i, &x, &y);
        RECT rc = {x + S(4), y + g_topPad + g_icon + g_gapIL, x + g_cellW - S(4), y + g_topPad + g_icon + g_gapIL + g_labelH};
        DrawTextW(dc, g_items[i].name, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    GdiFlush();
    g_textcov = (BYTE *)malloc((size_t)g_w * g_h);
    for (int i = 0; i < g_w * g_h; i++) {
        DWORD p = ((DWORD *)bits)[i];
        BYTE r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        g_textcov[i] = r > g ? (r > b ? r : b) : (g > b ? g : b);
    }
    SelectObject(dc, oldf);
    DeleteObject(font);
    SelectObject(dc, oldb);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void render(void)
{
    int bg[4], br[4], hv[4], tx[4];
    if (g_dark) {
        int a[4] = {36, 36, 36, 238}, b[4] = {255, 255, 255, 30}, c[4] = {255, 255, 255, 30}, d[4] = {255, 255, 255, 235};
        memcpy(bg, a, sizeof bg); memcpy(br, b, sizeof br); memcpy(hv, c, sizeof hv); memcpy(tx, d, sizeof tx);
    } else {
        int a[4] = {246, 246, 246, 242}, b[4] = {0, 0, 0, 28}, c[4] = {0, 0, 0, 20}, d[4] = {26, 26, 26, 240};
        memcpy(bg, a, sizeof bg); memcpy(br, b, sizeof br); memcpy(hv, c, sizeof hv); memcpy(tx, d, sizeof tx);
    }
    ZeroMemory(g_px, (size_t)g_w * g_h * 4);
    draw_rr(g_px, g_w, g_h, 0, 0, (float)g_w, (float)g_h, (float)g_radius, 0, bg[0], bg[1], bg[2], bg[3]);
    draw_rr(g_px, g_w, g_h, 0, 0, (float)g_w, (float)g_h, (float)g_radius, (float)S(1), br[0], br[1], br[2], br[3]);
    if (g_sel >= 0 && g_sel < g_count) {
        int x, y;
        cell_origin(g_sel, &x, &y);
        draw_rr(g_px, g_w, g_h, (float)(x + S(4)), (float)(y + S(3)), (float)(x + g_cellW - S(4)),
                (float)(y + g_cellH - S(3)), (float)S(10), 0, hv[0], hv[1], hv[2], hv[3]);
    }
    for (int i = 0; i < g_count; i++) {
        int x, y;
        if (!g_items[i].icon) continue;
        cell_origin(i, &x, &y);
        blit(g_px, g_w, g_h, g_items[i].icon, g_icon, g_icon, x + (g_cellW - g_icon) / 2, y + g_topPad);
    }
    for (int i = 0; i < g_w * g_h; i++)
        if (g_textcov[i]) g_px[i] = over(g_px[i], pm(tx[0], tx[1], tx[2], tx[3] * g_textcov[i] / 255));
}

static void present(int x, int y, BYTE alpha)
{
    POINT dst = {x, y}, src = {0, 0};
    SIZE sz = {g_w, g_h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    g_curX = x;
    g_curY = y;
    g_curA = alpha;
    UpdateLayeredWindow(g_hwnd, NULL, &dst, &sz, g_memdc, &src, 0, &bf, ULW_ALPHA);
}

static double elapsed_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void start_linger(void)
{
    /* Stay invisible for a moment so a second click on the pinned icon
       toggles the group closed instead of reopening it. */
    ShowWindow(g_hwnd, SW_HIDE);
    g_state = ST_LINGER;
    QueryPerformanceCounter(&g_t0);
    KillTimer(g_hwnd, TIMER_POLL);
    SetTimer(g_hwnd, TIMER_ANIM, 50, NULL);
}

static void start_close(void)
{
    if (g_state == ST_CLOSING || g_state == ST_LINGER) return;
    KillTimer(g_hwnd, TIMER_POLL);
    if (!g_animate) { start_linger(); return; }
    g_state = ST_CLOSING;
    QueryPerformanceCounter(&g_t0);
    SetTimer(g_hwnd, TIMER_ANIM, 10, NULL);
}

static void launch(int i)
{
    SHELLEXECUTEINFOW sei;
    ShowWindow(g_hwnd, SW_HIDE);
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpFile = g_items[i].path; /* the shell honours "Run as administrator" on shortcuts */
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
    DestroyWindow(g_hwnd);
}

static int hit(int x, int y)
{
    if (x < g_pad || y < g_pad || x >= g_w - g_pad || y >= g_h - g_pad) return -1;
    int c = (x - g_pad) / g_cellW, r = (y - g_pad) / g_cellH;
    if (c >= g_cols) return -1;
    int i = r * g_cols + c;
    return i < g_count ? i : -1;
}

static void set_sel(int i)
{
    if (i == g_sel) return;
    g_sel = i;
    render();
    present(g_curX, g_curY, g_curA);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_POLL) {
            /* Fallback if Windows refused us foreground focus: close on outside click. */
            if (GetForegroundWindow() == hwnd) { g_wasActive = TRUE; KillTimer(hwnd, TIMER_POLL); return 0; }
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000 || GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
                POINT pt;
                RECT rc;
                GetCursorPos(&pt);
                GetWindowRect(hwnd, &rc);
                if (!PtInRect(&rc, pt)) start_close();
            }
            return 0;
        }
        if (g_state == ST_OPENING) {
            double p = elapsed_ms() / OPEN_MS;
            if (p > 1) p = 1;
            double q = 1 - pow(1 - p, 3); /* ease-out cubic */
            double a = p * 1.6;
            if (a > 1) a = 1;
            present(g_finalX + (int)(g_offX * (1 - q)), g_finalY + (int)(g_offY * (1 - q)), (BYTE)(255 * a));
            if (p >= 1) {
                g_state = ST_OPEN;
                KillTimer(hwnd, TIMER_ANIM); /* no timer while open: zero CPU when idle */
                if (!g_wasActive && GetForegroundWindow() != hwnd) SetTimer(hwnd, TIMER_POLL, 50, NULL);
            }
        } else if (g_state == ST_CLOSING) {
            double p = elapsed_ms() / CLOSE_MS;
            if (p > 1) p = 1;
            double q = p * p; /* ease-in */
            present(g_finalX + (int)(g_offX * 0.5 * q), g_finalY + (int)(g_offY * 0.5 * q), (BYTE)(255 * (1 - p)));
            if (p >= 1) start_linger();
        } else if (g_state == ST_LINGER) {
            if (elapsed_ms() >= LINGER_MS) DestroyWindow(hwnd);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) start_close();
        else g_wasActive = TRUE;
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        if (g_state == ST_OPEN || g_state == ST_OPENING) set_sel(hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_state == ST_OPEN) set_sel(-1);
        return 0;
    case WM_LBUTTONUP: {
        int i = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i >= 0 && (g_state == ST_OPEN || g_state == ST_OPENING)) launch(i);
        return 0;
    }
    case WM_RBUTTONUP: {
        /* Right-click menu: manage the group without opening any folder. */
        enum { ID_REMOVE = 1, ID_ADD, ID_NEW, ID_OPEN };
        int i = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        WCHAR label[MAX_PATH + 32];
        POINT pt;
        HMENU m = CreatePopupMenu();
        if (i >= 0) {
            StringCchPrintfW(label, MAX_PATH + 32, L"Remove \"%s\" from group", g_items[i].name);
            AppendMenuW(m, MF_STRING, ID_REMOVE, label);
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        }
        AppendMenuW(m, MF_STRING, ID_ADD, L"Add apps...");
        AppendMenuW(m, MF_STRING, ID_NEW, L"Manage groups...");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, ID_OPEN, L"Open group folder");
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd, NULL);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(m);
        g_anchorX = g_finalX + g_w / 2;
        if (cmd == ID_REMOVE) {
            /* Recycle Bin instead of a hard delete, so a mistake can be undone. */
            WCHAR from[MAX_PATH + 2];
            SHFILEOPSTRUCTW op;
            ZeroMemory(from, sizeof from);
            StringCchCopyW(from, MAX_PATH, g_items[i].path);
            ZeroMemory(&op, sizeof op);
            op.wFunc = FO_DELETE;
            op.pFrom = from;
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
            SHFileOperationW(&op);
            g_after = ACT_RELAUNCH;
        } else if (cmd == ID_ADD) g_after = ACT_ADD;
        else if (cmd == ID_NEW) g_after = ACT_NEWGROUP;
        else if (cmd == ID_OPEN) ShellExecuteW(NULL, L"open", g_folder, NULL, NULL, SW_SHOWNORMAL);
        if (cmd) {
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int s = g_sel < 0 ? 0 : g_sel;
        switch (wp) {
        case VK_ESCAPE: start_close(); break;
        case VK_RETURN: if (g_sel >= 0) launch(g_sel); break;
        case VK_LEFT:  set_sel(g_sel < 0 ? 0 : (s > 0 ? s - 1 : s)); break;
        case VK_RIGHT: set_sel(g_sel < 0 ? 0 : (s + 1 < g_count ? s + 1 : s)); break;
        case VK_UP:    set_sel(g_sel < 0 ? 0 : (s - g_cols >= 0 ? s - g_cols : s)); break;
        case VK_DOWN:  set_sel(g_sel < 0 ? 0 : (s + g_cols < g_count ? s + g_cols : s)); break;
        }
        return 0;
    }
    case WM_CLOSE:
        start_close();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL system_dark(void)
{
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        return TRUE;
    return v == 0;
}

static int run_popup(HINSTANCE inst, const WCHAR *arg)
{
    WCHAR aumid[128];
    if (!GetFullPathNameW(arg, MAX_PATH, g_folder, NULL)) return 1;
    PathRemoveBackslashW(g_folder);
    if (!PathIsDirectoryW(g_folder)) {
        ui_msg(NULL, L"This group folder does not exist anymore.\nOpen GroupLauncher from the Start menu to manage your groups.", APP_NAME, MB_ICONWARNING);
        return 1;
    }

    /* Second click on the pinned icon while the popup is open: close it (toggle). */
    HWND existing = FindWindowW(CLS_NAME, g_folder);
    if (existing) {
        PostMessageW(existing, WM_CLOSE, 0, 0);
        return 0;
    }

    make_aumid(g_folder, aumid, 128);
    SetCurrentProcessExplicitAppUserModelID(aumid);

    enumerate(g_folder);
    if (g_count == 0) {
        ShellExecuteW(NULL, L"open", g_folder, NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }

    /* Place the popup on the monitor under the cursor, inside its work area
       (screen minus taskbar), next to whichever edge the taskbar is on. */
    POINT pt;
    MONITORINFO mi;
    UINT dx = 96, dy = 96;
    GetCursorPos(&pt);
    if (g_hasAnchor) pt.x = g_anchorX; /* reopened after an edit: keep the same spot */
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    g_scale = dx / 96.0;
    g_dark = system_dark();
    layout();

    for (int i = 0; i < g_count; i++) g_items[i].icon = load_icon(g_items[i].path, g_icon);
    render_text_layer();

    RECT wa = mi.rcWork, mr = mi.rcMonitor;
    int gap = S(10), slide = S(24), x = pt.x - g_w / 2, y;
    g_offX = g_offY = 0;
    if (wa.bottom < mr.bottom) { y = wa.bottom - g_h - gap; g_offY = slide; }
    else if (wa.top > mr.top) { y = wa.top + gap; g_offY = -slide; }
    else if (wa.left > mr.left) { x = wa.left + gap; y = pt.y - g_h / 2; g_offX = -slide; }
    else if (wa.right < mr.right) { x = wa.right - g_w - gap; y = pt.y - g_h / 2; g_offX = slide; }
    else if (pt.y > (mr.top + mr.bottom) / 2) { y = pt.y - S(40) - g_h; g_offY = slide; } /* auto-hide taskbar */
    else { y = pt.y + S(40); g_offY = -slide; }
    if (x > wa.right - g_w - gap) x = wa.right - g_w - gap;
    if (x < wa.left + gap) x = wa.left + gap;
    if (y > wa.bottom - g_h - gap) y = wa.bottom - g_h - gap;
    if (y < wa.top + gap) y = wa.top + gap;
    g_finalX = x;
    g_finalY = y;

    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    g_animate = anim;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CLS_NAME;
    RegisterClassExW(&wc);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_w;
    bi.bmiHeader.biHeight = -g_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_memdc = CreateCompatibleDC(NULL);
    g_dib = CreateDIBSection(g_memdc, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
    g_oldbmp = (HBITMAP)SelectObject(g_memdc, g_dib);

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, CLS_NAME, g_folder, WS_POPUP,
                             x, y, g_w, g_h, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    render();
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    if (g_animate) {
        g_state = ST_OPENING;
        present(g_finalX + g_offX, g_finalY + g_offY, 0);
    } else {
        g_state = ST_OPEN;
        present(g_finalX, g_finalY, 255);
    }
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    if (g_animate) SetTimer(g_hwnd, TIMER_ANIM, 10, NULL);
    else if (GetForegroundWindow() != g_hwnd) SetTimer(g_hwnd, TIMER_POLL, 50, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    /* Actions from the right-click menu run after the popup is gone. */
    if (g_after == ACT_ADD && !add_apps(NULL, g_folder)) g_after = ACT_NONE;
    if (g_after == ACT_ADD || g_after == ACT_RELAUNCH) {
        WCHAR self[MAX_PATH], args[MAX_PATH + 48];
        update_group_shortcut(g_folder, NULL); /* refresh the group's icon */
        GetModuleFileNameW(NULL, self, MAX_PATH);
        StringCchPrintfW(args, MAX_PATH + 48, L"\"%s\" --anchor %d", g_folder, g_anchorX);
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(NULL, NULL, self, args, NULL, SW_SHOWNORMAL);
    } else if (g_after == ACT_NEWGROUP) {
        run_manager();
    }
    return 0;
}

/* ---------- setup ---------- */

static HRESULT make_lnk(const WCHAR *lnk, const WCHAR *target, const WCHAR *args, const WCHAR *icon, int iconIdx,
                        const WCHAR *aumid, const WCHAR *desc)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl);
    if (FAILED(hr)) return hr;
    IShellLinkW_SetPath(sl, target);
    {
        /* Start apps in their own folder; some programs expect that. */
        WCHAR dir[MAX_PATH];
        StringCchCopyW(dir, MAX_PATH, target);
        if (PathRemoveFileSpecW(dir)) IShellLinkW_SetWorkingDirectory(sl, dir);
    }
    if (args) IShellLinkW_SetArguments(sl, args);
    if (icon) IShellLinkW_SetIconLocation(sl, icon, iconIdx);
    if (desc) IShellLinkW_SetDescription(sl, desc);
    if (aumid) {
        IPropertyStore *ps = NULL;
        if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPropertyStore, (void **)&ps))) {
            PROPVARIANT pv;
            ZeroMemory(&pv, sizeof pv);
            pv.vt = VT_LPWSTR;
            pv.pwszVal = (LPWSTR)aumid;
            IPropertyStore_SetValue(ps, &PKEY_AUMID, &pv);
            IPropertyStore_Commit(ps);
            IPropertyStore_Release(ps);
        }
    }
    hr = IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) {
        hr = IPersistFile_Save(pf, lnk, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    return hr;
}

static void unpremul_bottom_up(const DWORD *src, int s, BYTE *dst)
{
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            DWORD p = src[(s - 1 - y) * s + x];
            unsigned a = p >> 24, b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
            if (a) {
                b = b * 255 / a; g = g * 255 / a; r = r * 255 / a;
                if (b > 255) b = 255;
                if (g > 255) g = 255;
                if (r > 255) r = 255;
            } else b = g = r = 0;
            BYTE *o = dst + ((size_t)y * s + x) * 4;
            o[0] = (BYTE)b; o[1] = (BYTE)g; o[2] = (BYTE)r; o[3] = (BYTE)a;
        }
}

/* Taskbar icon for a group: a rounded tile with up to four of its app icons. */
/* ---------- searchable Windows icons (Segoe Fluent Icons, built into Windows 11) ----------
   Code points and names are Microsoft's official ones from the Segoe Fluent Icons font documentation;
   the third column holds extra search words. */

typedef struct {
    WCHAR code;
    const WCHAR *name, *keys;
} Glyph;

static const Glyph g_glyphs[] = {
    {0xE713, L"Settings", L"gear options config"}, {0xE770, L"System", L""}, {0xE771, L"Personalize", L"theme paint"},
    {0xE7EF, L"Admin", L"administrator"}, {0xE80F, L"Home", L"house"}, {0xEA8A, L"HomeSolid", L"house"},
    {0xE71D, L"AllApps", L"applications"}, {0xED35, L"Apps", L"applications"}, {0xE710, L"Add", L"plus new"},
    {0xE8B7, L"Folder", L"directory"}, {0xE838, L"FolderOpen", L"directory"}, {0xE8D5, L"FolderFill", L"directory"},
    {0xE8F4, L"NewFolder", L"directory"}, {0xEC25, L"PersonalFolder", L"user directory"}, {0xF012, L"ZipFolder", L"archive zip"},
    {0xEC50, L"FileExplorer", L"explorer files"}, {0xEC4E, L"ThisPC", L"computer"}, {0xE8A5, L"Document", L"file"},
    {0xE7C3, L"Page", L"file document"}, {0xEA90, L"PDF", L"document"}, {0xE8F1, L"Library", L"books"},
    {0xE74E, L"Save", L"disk"}, {0xE896, L"Download", L""}, {0xE898, L"Upload", L""},
    {0xEBD3, L"CloudDownload", L""}, {0xE753, L"Cloud", L"onedrive"}, {0xE895, L"Sync", L""},
    {0xE72C, L"Refresh", L"reload"}, {0xE72D, L"Share", L""}, {0xE71B, L"Link", L"url chain"},
    {0xE723, L"Attach", L"paperclip"}, {0xE8C8, L"Copy", L""}, {0xE77F, L"Paste", L"clipboard"},
    {0xE70F, L"Edit", L"pencil write"}, {0xED63, L"Pencil", L"write draw"}, {0xE74D, L"Delete", L"trash bin remove"},
    {0xE721, L"Search", L"magnifier find"}, {0xE71C, L"Filter", L""}, {0xE8CB, L"Sort", L""},
    {0xE787, L"Calendar", L"date"}, {0xE8BF, L"CalendarDay", L"date"}, {0xE917, L"Clock", L"time"},
    {0xE916, L"Stopwatch", L"timer"}, {0xEB50, L"Reminder", L"bell alarm"}, {0xE81C, L"History", L""},
    {0xE823, L"Recent", L""}, {0xE7FC, L"Game", L"controller gaming gamepad"}, {0xE967, L"GameConsole", L"gaming"},
    {0xE990, L"XboxOneConsole", L"gaming"}, {0xF10E, L"Dpad", L"controller gaming"}, {0xF5B5, L"Chess", L"game"},
    {0xEA86, L"Puzzle", L"game piece"}, {0xE714, L"Video", L"film"}, {0xE8B2, L"Movies", L"film cinema"},
    {0xE768, L"Play", L""}, {0xEC4F, L"MusicNote", L"music song"}, {0xE93C, L"MusicAlbum", L"music"},
    {0xE8D6, L"Audio", L"sound music"}, {0xE7F6, L"Headphone", L"audio music"}, {0xE95B, L"Headset", L"audio"},
    {0xE7F5, L"Speakers", L"sound audio"}, {0xE767, L"Volume", L"sound"}, {0xE720, L"Microphone", L"mic record"},
    {0xE9E9, L"Equalizer", L"audio sound"}, {0xE93E, L"Streaming", L""}, {0xE722, L"Camera", L"photo"},
    {0xE8B8, L"Webcam", L"camera"}, {0xE91B, L"Photo", L"image picture"}, {0xE8B9, L"Picture", L"image photo"},
    {0xE7AA, L"PhotoCollection", L"images gallery"}, {0xE790, L"Color", L"palette paint"}, {0xEB3C, L"Design", L""},
    {0xEC87, L"Draw", L"brush paint"}, {0xEF3C, L"Eyedropper", L"color picker"}, {0xE8D2, L"Font", L"text"},
    {0xE7A8, L"Crop", L"image"}, {0xE715, L"Mail", L"email"}, {0xE8A8, L"MailFill", L"email"},
    {0xE8BD, L"Message", L"chat"}, {0xE8F2, L"ChatBubbles", L"chat message"}, {0xE90A, L"Comment", L"chat"},
    {0xE724, L"Send", L""}, {0xE716, L"People", L"users contacts group"}, {0xE77B, L"Contact", L"person user"},
    {0xEBDA, L"Family", L"people"}, {0xE717, L"Phone", L"call"}, {0xE8EA, L"CellPhone", L"mobile smartphone"},
    {0xE8AA, L"VideoChat", L"call meeting"}, {0xE789, L"Megaphone", L"announce"}, {0xE774, L"Globe", L"internet web world"},
    {0xE909, L"World", L"internet globe"}, {0xEB41, L"Website", L"browser internet"}, {0xE701, L"Wifi", L"wireless wlan"},
    {0xE702, L"Bluetooth", L""}, {0xE839, L"Ethernet", L"lan cable network"}, {0xE968, L"Network", L"lan"},
    {0xEC27, L"MyNetwork", L"lan"}, {0xEB77, L"GatewayRouter", L"router network"}, {0xEC05, L"NetworkTower", L"antenna"},
    {0xE705, L"VPN", L""}, {0xE703, L"Connect", L""}, {0xE88E, L"USB", L""},
    {0xE772, L"Devices", L"hardware"}, {0xE975, L"Devices2", L"hardware"}, {0xE977, L"PC1", L"computer desktop tower"},
    {0xE7F4, L"TVMonitor", L"screen display tv"}, {0xE7F8, L"DeviceLaptopNoPic", L"laptop notebook"}, {0xE70A, L"Tablet", L""},
    {0xE962, L"Mouse", L""}, {0xE765, L"KeyboardClassic", L"keyboard"}, {0xEFA5, L"Touchpad", L""},
    {0xE914, L"Printer3D", L"3d print"}, {0xE749, L"Print", L"printer"}, {0xE8FE, L"Scan", L"scanner"},
    {0xE95D, L"Projector", L"beamer"}, {0xEDA2, L"HardDrive", L"disk storage hdd ssd"}, {0xE965, L"MediaStorageTower", L"server nas storage"},
    {0xE958, L"StorageOptical", L"dvd cd"}, {0xE7F1, L"SDCard", L"memory card"}, {0xEEA1, L"CPU", L"processor"},
    {0xEEA0, L"RAM", L"memory"}, {0xEEA3, L"VirtualMachineGroup", L"vm virtual hyper-v"}, {0xE950, L"Component", L"hardware chip"},
    {0xE95F, L"Wire", L"cable"}, {0xE72E, L"Lock", L"security password"}, {0xE785, L"Unlock", L"security"},
    {0xEA18, L"Shield", L"security protection"}, {0xF5B4, L"ShieldLock", L"security"}, {0xE83D, L"DefenderApp", L"antivirus security"},
    {0xE928, L"Fingerprint", L"security"}, {0xE8D7, L"Permissions", L"key access"}, {0xEB95, L"Certificate", L"pki ssl"},
    {0xEE7E, L"FIDOPasskey", L"passkey security"}, {0xE943, L"Code", L"programming developer"},
    {0xE756, L"CommandPrompt", L"terminal cmd console powershell shell"}, {0xEC7A, L"DeveloperTools", L"dev programming"},
    {0xEBE8, L"Bug", L"debug"}, {0xE99A, L"Robot", L"ai bot"}, {0xE90F, L"Repair", L"tools wrench fix"},
    {0xE912, L"Manage", L"admin"}, {0xE9D9, L"Diagnostic", L"health check"}, {0xE9F3, L"Process", L""},
    {0xF120, L"TaskManagerApp", L"task manager"}, {0xF0C5, L"WindowsUpdate", L"update"}, {0xE777, L"UpdateRestore", L"update"},
    {0xE7E8, L"PowerButton", L"shutdown"}, {0xE83F, L"Battery10", L"battery"}, {0xEA80, L"Lightbulb", L"idea"},
    {0xE945, L"LightningBolt", L"power flash"}, {0xE754, L"Flashlight", L""}, {0xE706, L"Brightness", L"sun"},
    {0xE719, L"Shop", L"store"}, {0xE7BF, L"ShoppingCart", L"shop store"}, {0xE8C7, L"PaymentCard", L"credit card bank"},
    {0xE825, L"Bank", L"money finance"}, {0xEAFC, L"Market", L""}, {0xEB11, L"StockUp", L"chart finance"},
    {0xE9D2, L"AreaChart", L"chart graph statistics"}, {0xF246, L"ViewDashboard", L"dashboard"}, {0xE8EF, L"Calculator", L""},
    {0xE7BE, L"Education", L"school graduation"}, {0xF7BB, L"EducationIcon", L"school"}, {0xF7DB, L"LearningTools", L"school"},
    {0xE821, L"Work", L"briefcase job"}, {0xE804, L"Car", L"auto"}, {0xE806, L"Bus", L""},
    {0xE7C0, L"Train", L""}, {0xE709, L"Airplane", L"flight travel"}, {0xE707, L"MapPin", L"location map"},
    {0xE81D, L"Location", L"map"}, {0xE8F0, L"Directions", L"map"}, {0xEC32, L"Cafe", L"coffee"},
    {0xEC09, L"Groceries", L"food"}, {0xE95E, L"Health", L""}, {0xEB51, L"Heart", L"favorite love"},
    {0xEB52, L"HeartFill", L"favorite love"}, {0xE734, L"FavoriteStar", L"star"}, {0xE735, L"FavoriteStarFill", L"star"},
    {0xE8E1, L"Like", L"thumbs up"}, {0xE7C1, L"Flag", L""}, {0xE8EC, L"Tag", L"label"},
    {0xE718, L"Pin", L""}, {0xE8C9, L"Important", L"exclamation"}, {0xE946, L"Info", L"information"},
    {0xE897, L"Help", L"question"}, {0xE7BA, L"Warning", L"alert"}, {0xE783, L"Error", L""},
    {0xE8FB, L"Accept", L"check ok"}, {0xE930, L"Completed", L"check done"}, {0xE733, L"Blocked", L""},
    {0xE899, L"Emoji", L"smile"}, {0xE76E, L"Emoji2", L"smile"}, {0xE8BE, L"Leaf", L"nature plant"},
    {0xEB42, L"Drop", L"water"}, {0xE9CA, L"Frigid", L"snow cold"}, {0xEFA9, L"Speech", L"voice"},
    {0xE7B8, L"Package", L"box"}, {0xF133, L"GiftboxOpen", L"gift present"}, {0xECA5, L"Tiles", L"grid"},
    {0xF0E2, L"GridView", L"grid"}, {0xF034, L"Widget", L""}, {0xEA37, L"List", L""},
    {0xE9D5, L"CheckList", L"todo tasks"}, {0xF0E3, L"ClipboardList", L"todo tasks"}, {0xE77C, L"Memo", L"note"},
    {0xE70B, L"QuickNote", L"note"}, {0xE8A4, L"Bookmarks", L""}, {0xE7BC, L"ReadingList", L""},
    {0xE82D, L"Dictionary", L"book"}, {0xED15, L"Feedback", L""}, {0xED5E, L"Ruler", L""},
    {0xF196, L"Beaker", L"lab science"}, {0xEB44, L"Radar", L""}, {0xF272, L"Bullseye", L"target"},
    {0xF540, L"Safe", L"vault"}, {0xF427, L"IDBadge", L"identity"}, {0xE910, L"Accounts", L"user account"},
    {0xEE57, L"GuestUser", L"user"}, {0xF3B1, L"SignOut", L"logout"}, {0xE8AF, L"Remote", L"remote control"},
    {0xE95A, L"Communications", L""}, {0xE963, L"Smartcard", L""}, {0xE957, L"Sensor", L""},
    {0xF22C, L"IOT", L"smart home"}, {0xF119, L"HMD", L"vr headset"}, {0xF117, L"ProjectToDevice", L"cast"},
    {0xE786, L"Slideshow", L"presentation"}, {0xF71C, L"StartPresenting", L"presentation"}, {0xE740, L"FullScreen", L""},
    {0xE78B, L"NewWindow", L""}, {0xE7C4, L"TaskView", L"windows"}, {0xF406, L"ClippingTool", L"screenshot snip"},
    {0xE7C8, L"Record", L""}, {0xEF31, L"TrafficLight", L""}, {0xEC08, L"Courthouse", L"law building"},
    {0xE98F, L"ConstructionCone", L""}, {0xE822, L"Construction", L""}, {0xEE94, L"Wheel", L""},
};
#define NGLYPHS ((int)(sizeof g_glyphs / sizeof g_glyphs[0]))

/* Segoe Fluent Icons ships with Windows 11; Windows 10 has the older Segoe MDL2 Assets with most of the same glyphs. */
static const WCHAR *glyph_font_face(void)
{
    static WCHAR face[LF_FACESIZE];
    if (!face[0]) {
        WCHAR got[LF_FACESIZE] = L"";
        HDC dc = GetDC(NULL);
        HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe Fluent Icons");
        HGDIOBJ o = SelectObject(dc, f);
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteObject(f);
        ReleaseDC(NULL, dc);
        StringCchCopyW(face, LF_FACESIZE, !lstrcmpiW(got, L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
    }
    return face;
}

/* Coverage mask (0..255) of a glyph centered in an s x s square. */
static BYTE *glyph_mask(WCHAR code, int s, int px)
{
    BITMAPINFO bi;
    void *bits = NULL;
    WCHAR t[2] = {code, 0};
    RECT rc = {0, 0, s, s};
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    ZeroMemory(bits, (size_t)s * s * 4);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, glyph_font_face());
    HGDIOBJ of = SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    BYTE *mask = (BYTE *)malloc((size_t)s * s);
    for (int i = 0; i < s * s; i++) {
        DWORD p = ((DWORD *)bits)[i];
        BYTE r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        mask[i] = r > g ? (r > b ? r : b) : (g > b ? g : b);
    }
    SelectObject(dc, of);
    DeleteObject(font);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    return mask;
}

/* Windows accent color (Settings > Personalization > Colors), stored as 0xAABBGGRR. */
static COLORREF accent_color(void)
{
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return RGB(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
    return RGB(0, 120, 212);
}

/* Writes a multi-size .ico; compose() draws each size (premultiplied BGRA). */
typedef void (*ComposeFn)(DWORD *img, int s, void *ctx);

static BOOL write_ico(const WCHAR *icoPath, ComposeFn compose, void *ctx)
{
    static const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 256};
    const int nsz = sizeof sizes / sizeof sizes[0];
    HANDLE f = CreateFileW(icoPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr, offset = 6 + 16 * nsz;
    WORD hdr[3] = {0, 1, (WORD)nsz};
    WriteFile(f, hdr, sizeof hdr, &wr, NULL);
    for (int k = 0; k < nsz; k++) {
        int s = sizes[k];
        DWORD bytes = 40 + s * s * 4 + ((s + 31) / 32 * 4) * s;
        BYTE e[16] = {(BYTE)(s >= 256 ? 0 : s), (BYTE)(s >= 256 ? 0 : s), 0, 0, 1, 0, 32, 0};
        memcpy(e + 8, &bytes, 4);
        memcpy(e + 12, &offset, 4);
        WriteFile(f, e, 16, &wr, NULL);
        offset += bytes;
    }
    for (int k = 0; k < nsz; k++) {
        int s = sizes[k], mrow = (s + 31) / 32 * 4;
        DWORD *img = (DWORD *)calloc((size_t)s * s, 4);
        compose(img, s, ctx);
        BITMAPINFOHEADER bh;
        ZeroMemory(&bh, sizeof bh);
        bh.biSize = 40;
        bh.biWidth = s;
        bh.biHeight = s * 2;
        bh.biPlanes = 1;
        bh.biBitCount = 32;
        bh.biCompression = BI_RGB;
        bh.biSizeImage = s * s * 4 + mrow * s;
        BYTE *pix = (BYTE *)malloc((size_t)s * s * 4);
        BYTE *mask = (BYTE *)calloc((size_t)mrow * s, 1);
        unpremul_bottom_up(img, s, pix);
        WriteFile(f, &bh, 40, &wr, NULL);
        WriteFile(f, pix, s * s * 4, &wr, NULL);
        WriteFile(f, mask, mrow * s, &wr, NULL);
        free(pix);
        free(mask);
        free(img);
    }
    CloseHandle(f);
    return TRUE;
}

typedef struct {
    DWORD *src[4];
    int n, srcSize;
} GridCtx;

static void compose_grid(DWORD *img, int s, void *p)
{
    GridCtx *g = (GridCtx *)p;
    draw_rr(img, s, s, 0, 0, (float)s, (float)s, s * 0.22f, 0, 128, 128, 128, 110);
    if (g->n == 1 || (s <= 20 && g->n > 0)) {
        int c = (int)(s * 0.72f + 0.5f);
        DWORD *t = (DWORD *)malloc((size_t)c * c * 4);
        resample(g->src[0], g->srcSize, g->srcSize, t, c, c);
        blit(img, s, s, t, c, c, (s - c) / 2, (s - c) / 2);
        free(t);
    } else if (g->n > 1) {
        int c = (int)(s * 0.36f + 0.5f), gp = s - 2 * c - 2 * (int)(s * 0.1f + 0.5f);
        int m = (s - 2 * c - gp) / 2;
        DWORD *t = (DWORD *)malloc((size_t)c * c * 4);
        for (int i = 0; i < g->n; i++) {
            resample(g->src[i], g->srcSize, g->srcSize, t, c, c);
            blit(img, s, s, t, c, c, m + (i % 2) * (c + gp), m + (i / 2) * (c + gp));
        }
        free(t);
    }
}

/* Mode 2: taskbar icon for a group made of up to four of its app icons. */
static BOOL write_group_icon(const WCHAR *folder, const WCHAR *icoPath)
{
    GridCtx g;
    ZeroMemory(&g, sizeof g);
    g.srcSize = 96;
    enumerate(folder);
    for (int i = 0; i < g_count && g.n < 4; i++)
        if ((g.src[g.n] = load_icon(g_items[i].path, g.srcSize)) != NULL) g.n++;
    BOOL ok = write_ico(icoPath, compose_grid, &g);
    for (int i = 0; i < g.n; i++) free(g.src[i]);
    for (int i = 0; i < g_count; i++) { free(g_items[i].icon); g_items[i].icon = NULL; }
    return ok;
}

typedef struct {
    WCHAR code;
    COLORREF tile;
} GlyphCtx;

static void compose_glyph(DWORD *img, int s, void *p)
{
    GlyphCtx *g = (GlyphCtx *)p;
    draw_rr(img, s, s, 0, 0, (float)s, (float)s, s * 0.22f, 0, GetRValue(g->tile), GetGValue(g->tile), GetBValue(g->tile), 255);
    BYTE *m = glyph_mask(g->code, s, (int)(s * 0.58f + 0.5f));
    for (int i = 0; i < s * s; i++)
        if (m[i]) img[i] = over(img[i], pm(255, 255, 255, m[i]));
    free(m);
}

/* Mode 1: a Windows icon (white glyph on a tile in the Windows accent color). */
static BOOL write_glyph_icon(const WCHAR *icoPath, WCHAR code, COLORREF tile)
{
    GlyphCtx g = {code, tile};
    return write_ico(icoPath, compose_glyph, &g);
}

/* "glyph:E7FC" (accent color) or "glyph:E7FC:RRGGBB" */
static COLORREF parse_glyph_color(const WCHAR *spec)
{
    const WCHAR *c = wcschr(spec + 6, L':');
    if (!c) return accent_color();
    unsigned long v = wcstoul(c + 1, NULL, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}


/* ---------- groups: paths, install, creation ---------- */

typedef struct {
    WCHAR instDir[MAX_PATH], instExe[MAX_PATH], iconDir[MAX_PATH];
    WCHAR groupsRoot[MAX_PATH], smGroups[MAX_PATH], smApp[MAX_PATH];
} Paths;

static BOOL get_paths(Paths *p)
{
    PWSTR la = NULL, prof = NULL, progs = NULL;
    BOOL ok = SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la)) &&
              SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &prof)) &&
              SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Programs, 0, NULL, &progs));
    if (ok) {
        PathCombineW(p->instDir, la, L"Programs\\GroupLauncher");
        PathCombineW(p->instExe, p->instDir, L"GroupLauncher.exe");
        PathCombineW(p->iconDir, p->instDir, L"icons");
        PathCombineW(p->groupsRoot, prof, L"Groups");
        PathCombineW(p->smGroups, progs, L"Groups");
        PathCombineW(p->smApp, progs, L"GroupLauncher.lnk");
    }
    CoTaskMemFree(la);
    CoTaskMemFree(prof);
    CoTaskMemFree(progs);
    return ok;
}

/* Per-user install (no admin rights): copy the exe to %LOCALAPPDATA%\Programs
   and add a "GroupLauncher" Start menu entry for creating more groups later. */
static BOOL install_self(const Paths *p)
{
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    SHCreateDirectoryExW(NULL, p->iconDir, NULL);
    SHCreateDirectoryExW(NULL, p->groupsRoot, NULL);
    SHCreateDirectoryExW(NULL, p->smGroups, NULL);
    if (lstrcmpiW(self, p->instExe) != 0 && !CopyFileW(self, p->instExe, FALSE) &&
        GetFileAttributesW(p->instExe) == INVALID_FILE_ATTRIBUTES) {
        ui_msg(NULL, L"Could not install GroupLauncher into your user folder.", APP_NAME, MB_ICONERROR);
        return FALSE;
    }
    make_lnk(p->smApp, p->instExe, NULL, NULL, 0, NULL, L"Manage your taskbar groups");
    return TRUE;
}

/* Windows caches icons by file path, so every new group icon gets a fresh file name
   (the shortcut is then pointed at it); older icon files of the group are removed. */
static void delete_group_icons(const WCHAR *iconDir, const WCHAR *aumid)
{
    WCHAR pattern[MAX_PATH], file[MAX_PATH], mask[160];
    WIN32_FIND_DATAW fd;
    StringCchPrintfW(mask, 160, L"%s*.ico", aumid);
    PathCombineW(pattern, iconDir, mask);
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        PathCombineW(file, iconDir, fd.cFileName);
        DeleteFileW(file);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void new_icon_path(const WCHAR *iconDir, const WCHAR *aumid, WCHAR *out)
{
    WCHAR name[192];
    delete_group_icons(iconDir, aumid);
    StringCchPrintfW(name, 192, L"%s-%llX.ico", aumid, (unsigned long long)GetTickCount64());
    PathCombineW(out, iconDir, name);
}

/* Mode 1: an icon the user picked, stored as "path,index" in a hidden file in the group folder,
   so it moves with the group (rename, backup). Hidden files are not shown as apps. */
#define ICON_CHOICE_FILE L"groupicon.txt"

static BOOL read_icon_choice(const WCHAR *group, WCHAR *icon, int *idx)
{
    WCHAR f[MAX_PATH], buf[MAX_PATH + 16];
    char raw[(MAX_PATH + 16) * 2];
    DWORD rd = 0;
    PathCombineW(f, group, ICON_CHOICE_FILE);
    HANDLE h = CreateFileW(f, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(h, raw, sizeof raw - 2, &rd, NULL);
    CloseHandle(h);
    raw[rd] = raw[rd + 1] = 0;
    StringCchCopyW(buf, MAX_PATH + 16, (const WCHAR *)raw);
    if (!wcsncmp(buf, L"glyph:", 6)) {
        /* Searchable Windows icon: render it to an .ico next to the generated grid icons. */
        Paths p;
        WCHAR aumid[128];
        if (!get_paths(&p)) return FALSE;
        SHCreateDirectoryExW(NULL, p.iconDir, NULL);
        make_aumid(group, aumid, 128);
        new_icon_path(p.iconDir, aumid, icon);
        *idx = 0;
        return write_glyph_icon(icon, (WCHAR)wcstoul(buf + 6, NULL, 16), parse_glyph_color(buf));
    }
    WCHAR *comma = wcsrchr(buf, L',');
    if (!comma) return FALSE;
    *comma = 0;
    *idx = _wtoi(comma + 1);
    ExpandEnvironmentStringsW(buf, icon, MAX_PATH);
    return GetFileAttributesW(icon) != INVALID_FILE_ATTRIBUTES;
}

/* icon = NULL: back to the app grid; icon = "glyph:XXXX": searchable Windows icon; else "file,index". */
static void write_icon_choice(const WCHAR *group, const WCHAR *icon, int idx)
{
    WCHAR f[MAX_PATH], buf[MAX_PATH + 16];
    DWORD wr;
    PathCombineW(f, group, ICON_CHOICE_FILE);
    if (!icon) {
        SetFileAttributesW(f, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(f);
        return;
    }
    if (!wcsncmp(icon, L"glyph:", 6)) StringCchCopyW(buf, MAX_PATH + 16, icon);
    else StringCchPrintfW(buf, MAX_PATH + 16, L"%s,%d", icon, idx);
    SetFileAttributesW(f, FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, buf, (DWORD)(lstrlenW(buf) + 1) * sizeof(WCHAR), &wr, NULL);
    CloseHandle(h);
}

/* Windows keeps its own copy of every pinned shortcut in "User Pinned\TaskBar". Updating that copy
   (matched by the group's AppUserModelID) lets a pinned group pick up a new icon or folder path
   without unpinning and pinning again. Returns how many pinned copies were updated. */
static int update_pinned_copies(const WCHAR *aumid, const WCHAR *args, const WCHAR *icon, int iconIdx)
{
    PWSTR roaming = NULL;
    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    int updated = 0;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &roaming))) return 0;
    PathCombineW(dir, roaming, L"Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar");
    CoTaskMemFree(roaming);
    PathCombineW(pattern, dir, L"*.lnk");
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        WCHAR path[MAX_PATH];
        IShellLinkW *sl = NULL;
        IPersistFile *pf = NULL;
        IPropertyStore *ps = NULL;
        BOOL match = FALSE;
        PathCombineW(path, dir, fd.cFileName);
        if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) continue;
        if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) &&
            SUCCEEDED(IPersistFile_Load(pf, path, STGM_READWRITE)) &&
            SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPropertyStore, (void **)&ps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_AUMID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal &&
                !lstrcmpiW(pv.pwszVal, aumid))
                match = TRUE;
            PropVariantClear(&pv);
        }
        if (match) {
            IShellLinkW_SetIconLocation(sl, icon, iconIdx);
            IShellLinkW_SetArguments(sl, args);
            if (SUCCEEDED(IPersistFile_Save(pf, path, TRUE))) {
                updated++;
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path, NULL);
            }
        }
        if (ps) IPropertyStore_Release(ps);
        if (pf) IPersistFile_Release(pf);
        IShellLinkW_Release(sl);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return updated;
}

/* (Re)creates the shortcut that represents a group on the taskbar, with its icon. */
static void update_group_shortcut(const WCHAR *group, WCHAR *lnkOut)
{
    Paths p;
    WCHAR aumid[128], icon[MAX_PATH], args[MAX_PATH + 4];
    WCHAR lnkName[MAX_PATH], lnk[MAX_PATH], desc[MAX_PATH];
    const WCHAR *name = PathFindFileNameW(group);
    if (!get_paths(&p)) return;
    SHCreateDirectoryExW(NULL, p.iconDir, NULL);
    SHCreateDirectoryExW(NULL, p.smGroups, NULL);
    make_aumid(group, aumid, 128);
    int iconIdx = 0;
    if (!read_icon_choice(group, icon, &iconIdx)) {
        /* Mode 2 (default): a grid made from the group's app icons. */
        new_icon_path(p.iconDir, aumid, icon);
        write_group_icon(group, icon);
    }
    StringCchPrintfW(args, MAX_PATH + 4, L"\"%s\"", group);
    StringCchPrintfW(lnkName, MAX_PATH, L"%s.lnk", name);
    StringCchPrintfW(desc, MAX_PATH, L"Open the %s group", name);
    PathCombineW(lnk, p.smGroups, lnkName);
    make_lnk(lnk, p.instExe, args, icon, iconIdx, aumid, desc);
    g_pinnedUpdated = update_pinned_copies(aumid, args, icon, iconIdx);
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, icon, NULL);
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, lnk, NULL);
    if (lnkOut) StringCchCopyW(lnkOut, MAX_PATH, lnk);
}

/* File picker fallback for programs that are not in the app list (e.g. portable tools).
   Shortcuts are copied into the group, programs get a new shortcut. */
static int add_files(HWND owner, const WCHAR *group)
{
    IFileOpenDialog *dlg = NULL;
    IShellItemArray *arr = NULL;
    IShellItem *start = NULL;
    PWSTR common = NULL;
    WCHAR title[MAX_PATH + 64];
    DWORD opts = 0, n = 0;
    int added = 0;
    COMDLG_FILTERSPEC types[] = {
        {L"Apps and shortcuts", L"*.lnk;*.exe;*.url;*.msc;*.cpl;*.bat;*.cmd"},
        {L"All files", L"*.*"},
    };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return FALSE;
    IFileOpenDialog_SetFileTypes(dlg, 2, types);
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_ALLOWMULTISELECT | FOS_NODEREFERENCELINKS | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    StringCchPrintfW(title, MAX_PATH + 64, L"Choose apps for \"%s\"  (hold Ctrl to pick several)", PathFindFileNameW(group));
    IFileOpenDialog_SetTitle(dlg, title);
    IFileOpenDialog_SetOkButtonLabel(dlg, L"Add to group");
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_CommonPrograms, 0, NULL, &common)) &&
        SUCCEEDED(SHCreateItemFromParsingName(common, NULL, &IID_IShellItem, (void **)&start))) {
        IFileOpenDialog_SetFolder(dlg, start);
        IShellItem_Release(start);
    }
    CoTaskMemFree(common);
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner)) && SUCCEEDED(IFileOpenDialog_GetResults(dlg, &arr))) {
        IShellItemArray_GetCount(arr, &n);
        for (DWORD k = 0; k < n; k++) {
            IShellItem *it = NULL;
            PWSTR path = NULL;
            if (FAILED(IShellItemArray_GetItemAt(arr, k, &it))) continue;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
                WCHAR dst[MAX_PATH], nm[MAX_PATH];
                const WCHAR *ext = PathFindExtensionW(path);
                if (!lstrcmpiW(ext, L".lnk") || !lstrcmpiW(ext, L".url")) {
                    PathCombineW(dst, group, PathFindFileNameW(path));
                    if (CopyFileW(path, dst, FALSE)) added++;
                } else {
                    StringCchCopyW(nm, MAX_PATH, PathFindFileNameW(path));
                    PathRemoveExtensionW(nm);
                    StringCchCatW(nm, MAX_PATH, L".lnk");
                    PathCombineW(dst, group, nm);
                    if (SUCCEEDED(make_lnk(dst, path, NULL, NULL, 0, NULL, NULL))) added++;
                }
                CoTaskMemFree(path);
            }
            IShellItem_Release(it);
        }
        IShellItemArray_Release(arr);
    }
    IFileOpenDialog_Release(dlg);
    return added;
}

/* ---------- app picker: every app from the Applications folder (shell:AppsFolder) ---------- */

typedef struct {
    WCHAR *name;
    PIDLIST_ABSOLUTE pidl;
    int icon;
    BOOL checked;
} AppEntry;

static struct {
    AppEntry *apps;
    int n;
    HWND wnd, lv, search, okBtn;
    HFONT font;
    double scale;
    BOOL ok, browse, rebuilding;
    WCHAR title[MAX_PATH + 64];
} g_pk;

static int cmp_apps(const void *a, const void *b)
{
    return StrCmpLogicalW(((const AppEntry *)a)->name, ((const AppEntry *)b)->name);
}

/* The Applications folder is the same list Start's "All apps" shows, including Store apps. */
static void load_apps(void)
{
    IShellItem *folder = NULL, *it = NULL;
    IEnumShellItems *en = NULL;
    int cap = 256;
    g_pk.n = 0;
    g_pk.apps = (AppEntry *)calloc(cap, sizeof(AppEntry));
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", NULL, &IID_IShellItem, (void **)&folder))) return;
    if (SUCCEEDED(IShellItem_BindToHandler(folder, NULL, &BHID_EnumItems, &IID_IEnumShellItems, (void **)&en))) {
        while (IEnumShellItems_Next(en, 1, &it, NULL) == S_OK) {
            PWSTR name = NULL;
            PIDLIST_ABSOLUTE pidl = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_NORMALDISPLAY, &name)) &&
                SUCCEEDED(SHGetIDListFromObject((IUnknown *)it, &pidl))) {
                if (g_pk.n == cap) {
                    cap *= 2;
                    g_pk.apps = (AppEntry *)realloc(g_pk.apps, cap * sizeof(AppEntry));
                }
                g_pk.apps[g_pk.n].name = name;
                g_pk.apps[g_pk.n].pidl = pidl;
                g_pk.apps[g_pk.n].icon = -1;
                g_pk.apps[g_pk.n].checked = FALSE;
                g_pk.n++;
            } else {
                CoTaskMemFree(name);
                CoTaskMemFree(pidl);
            }
            IShellItem_Release(it);
        }
        IEnumShellItems_Release(en);
    }
    IShellItem_Release(folder);
    qsort(g_pk.apps, g_pk.n, sizeof(AppEntry), cmp_apps);
}

static void update_ok_label(void)
{
    int c = 0;
    WCHAR t[48];
    for (int i = 0; i < g_pk.n; i++) c += g_pk.apps[i].checked;
    if (c) StringCchPrintfW(t, 48, L"Add (%d)", c);
    else StringCchCopyW(t, 48, L"Add");
    SetWindowTextW(g_pk.okBtn, t);
}

static void fill_list(void)
{
    WCHAR q[128];
    int row = 0;
    RECT rc;
    GetWindowTextW(g_pk.search, q, 128);
    g_pk.rebuilding = TRUE;
    SendMessageW(g_pk.lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_pk.lv);
    for (int i = 0; i < g_pk.n; i++) {
        LVITEMW it;
        if (q[0] && !StrStrIW(g_pk.apps[i].name, q)) continue;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem = row;
        it.pszText = g_pk.apps[i].name;
        it.iImage = I_IMAGECALLBACK; /* icons load lazily while scrolling */
        it.lParam = i;
        ListView_InsertItem(g_pk.lv, &it);
        ListView_SetCheckState(g_pk.lv, row, g_pk.apps[i].checked);
        row++;
    }
    GetClientRect(g_pk.lv, &rc);
    ListView_SetColumnWidth(g_pk.lv, 0, rc.right - rc.left);
    SendMessageW(g_pk.lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_pk.lv, NULL, TRUE);
    g_pk.rebuilding = FALSE;
}

static LRESULT CALLBACK picker_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LRESULT cr;
    if (theme_ctlcolor(m, w, &cr)) return cr;
    switch (m) {
    case WM_CREATE: {
        HWND c[6];
        SHFILEINFOW sfi;
        LVCOLUMNW col;
#define D(v) ((int)((v) * g_pk.scale + 0.5))
        c[0] = CreateWindowExW(0, L"STATIC", g_pk.title, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                               D(16), D(14), D(408), D(20), h, NULL, NULL, NULL);
        c[1] = g_pk.search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                             D(16), D(40), D(408), D(28), h, (HMENU)101, NULL, NULL);
        c[2] = g_pk.lv = CreateWindowExW(g_uiDark ? 0 : WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER |
                                             LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                                         D(16), D(78), D(408), D(400), h, (HMENU)102, NULL, NULL);
        c[3] = CreateWindowExW(0, L"BUTTON", L"Browse for a file...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               D(16), D(492), D(150), D(30), h, (HMENU)103, NULL, NULL);
        c[4] = g_pk.okBtn = CreateWindowExW(0, L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                            D(244), D(492), D(86), D(30), h, (HMENU)IDOK, NULL, NULL);
        c[5] = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               D(338), D(492), D(86), D(30), h, (HMENU)IDCANCEL, NULL, NULL);
#undef D
        for (int i = 0; i < 6; i++) {
            SendMessageW(c[i], WM_SETFONT, (WPARAM)g_pk.font, TRUE);
            theme_control(c[i]); /* before the checkbox style, so the checkboxes are drawn dark too */
        }
        SendMessageW(g_pk.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search apps");
        ListView_SetExtendedListViewStyle(g_pk.lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        ListView_SetImageList(g_pk.lv, il, LVSIL_SMALL);
        ZeroMemory(&col, sizeof col);
        col.mask = LVCF_WIDTH;
        col.cx = 100;
        ListView_InsertColumn(g_pk.lv, 0, &col);
        fill_list();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 101 && HIWORD(w) == EN_CHANGE) fill_list();
        else if (LOWORD(w) == IDOK) { g_pk.ok = TRUE; DestroyWindow(h); }
        else if (LOWORD(w) == 103) { g_pk.ok = TRUE; g_pk.browse = TRUE; DestroyWindow(h); }
        else if (LOWORD(w) == IDCANCEL) DestroyWindow(h);
        return 0;
    case WM_NOTIFY: {
        NMHDR *hd = (NMHDR *)l;
        if (hd->hwndFrom != g_pk.lv) break;
        if (hd->code == LVN_GETDISPINFOW) {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)l;
            if (di->item.mask & LVIF_IMAGE) {
                AppEntry *a = &g_pk.apps[di->item.lParam];
                if (a->icon < 0) {
                    SHFILEINFOW sfi;
                    ZeroMemory(&sfi, sizeof sfi);
                    SHGetFileInfoW((LPCWSTR)a->pidl, 0, &sfi, sizeof sfi, SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
                    a->icon = sfi.iIcon;
                }
                di->item.iImage = a->icon;
            }
        } else if (hd->code == LVN_ITEMCHANGED && !g_pk.rebuilding) {
            NMLISTVIEW *nm = (NMLISTVIEW *)l;
            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && nm->iItem >= 0) {
                LVITEMW it;
                ZeroMemory(&it, sizeof it);
                it.mask = LVIF_PARAM;
                it.iItem = nm->iItem;
                ListView_GetItem(g_pk.lv, &it);
                g_pk.apps[it.lParam].checked = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                update_ok_label();
            }
        } else if (hd->code == NM_DBLCLK) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)l;
            if (ia->iItem >= 0) ListView_SetCheckState(g_pk.lv, ia->iItem, !ListView_GetCheckState(g_pk.lv, ia->iItem));
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Shows the app list; checked apps become shortcuts in the group. Returns TRUE if anything was added. */
static BOOL add_apps(HWND owner, const WCHAR *group)
{
    MONITORINFO mi;
    int added = 0;
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    ZeroMemory(&g_pk, sizeof g_pk);
    load_apps();
    StringCchPrintfW(g_pk.title, MAX_PATH + 64, L"Tick the apps for \"%s\":", PathFindFileNameW(group));
    g_pk.scale = monitor_scale(owner, &mi);
    g_pk.font = ui_font(g_pk.scale);
    g_pk.wnd = ui_create(owner, L"GroupLauncherPicker", picker_proc, L"Add apps", (int)(440 * g_pk.scale + 0.5),
                         (int)(538 * g_pk.scale + 0.5), g_pk.scale, &mi, NULL, WS_CAPTION | WS_SYSMENU);
    if (g_pk.wnd) ui_modal_loop(owner, g_pk.wnd, g_pk.search);

    if (g_pk.ok) {
        for (int i = 0; i < g_pk.n; i++) {
            WCHAR clean[MAX_PATH], lnk[MAX_PATH];
            IShellLinkW *sl = NULL;
            IPersistFile *pf = NULL;
            int k = 0;
            if (!g_pk.apps[i].checked) continue;
            for (WCHAR *p = g_pk.apps[i].name; *p && k < 120; p++)
                if (*p >= 32 && !wcschr(L"<>:\"/\\|?*", *p)) clean[k++] = *p;
            clean[k] = 0;
            StringCchCatW(clean, MAX_PATH, L".lnk");
            PathCombineW(lnk, group, clean);
            /* A shortcut to the app's shell item works for Store apps and desktop apps alike. */
            if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
                IShellLinkW_SetIDList(sl, g_pk.apps[i].pidl);
                if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
                    if (SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE))) added++;
                    IPersistFile_Release(pf);
                }
                IShellLinkW_Release(sl);
            }
        }
    }
    BOOL browse = g_pk.browse;
    for (int i = 0; i < g_pk.n; i++) {
        CoTaskMemFree(g_pk.apps[i].name);
        CoTaskMemFree(g_pk.apps[i].pidl);
    }
    free(g_pk.apps);
    DeleteObject(g_pk.font);
    if (browse) added += add_files(owner, group);
    return added > 0;
}

/* ---------- small "group name" window ---------- */

typedef struct {
    WCHAR text[128];
    const WCHAR *initial;
    BOOL ok;
    HWND edit;
    HFONT font;
    double scale;
} NameDlg;

static LRESULT CALLBACK name_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    NameDlg *d = (NameDlg *)GetWindowLongPtrW(h, GWLP_USERDATA);
    LRESULT cr;
    if (theme_ctlcolor(m, w, &cr)) return cr;
    switch (m) {
    case WM_CREATE: {
        HWND c[4];
        d = (NameDlg *)((CREATESTRUCTW *)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
#define D(v) ((int)((v) * d->scale + 0.5))
        c[0] = CreateWindowExW(0, L"STATIC", L"Name of the group (for example Gaming):", WS_CHILD | WS_VISIBLE,
                               D(16), D(16), D(328), D(20), h, NULL, NULL, NULL);
        c[1] = d->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", d->initial ? d->initial : L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                         D(16), D(42), D(328), D(26), h, (HMENU)100, NULL, NULL);
        c[2] = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               D(164), D(84), D(86), D(30), h, (HMENU)IDOK, NULL, NULL);
        c[3] = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               D(258), D(84), D(86), D(30), h, (HMENU)IDCANCEL, NULL, NULL);
#undef D
        for (int i = 0; i < 4; i++) {
            SendMessageW(c[i], WM_SETFONT, (WPARAM)d->font, TRUE);
            theme_control(c[i]);
        }
        SendMessageW(d->edit, EM_LIMITTEXT, 60, 0);
        SendMessageW(d->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            GetWindowTextW(d->edit, d->text, 128);
            d->ok = TRUE;
            DestroyWindow(h);
        } else if (LOWORD(w) == IDCANCEL) {
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Returns 1 = name entered, 0 = empty name, -1 = cancelled. */
static int ask_group_name(HWND owner, const WCHAR *caption, const WCHAR *initial, WCHAR *out, size_t n)
{
    NameDlg d;
    MONITORINFO mi;
    WCHAR clean[128];
    int k = 0;

    ZeroMemory(&d, sizeof d);
    d.initial = initial;
    d.scale = monitor_scale(owner, &mi);
    d.font = ui_font(d.scale);
    HWND hw = ui_create(owner, L"GroupLauncherName", name_proc, caption, (int)(360 * d.scale + 0.5),
                        (int)(130 * d.scale + 0.5), d.scale, &mi, &d, WS_CAPTION | WS_SYSMENU);
    if (!hw) { DeleteObject(d.font); return -1; }
    ui_modal_loop(owner, hw, d.edit);
    DeleteObject(d.font);
    if (!d.ok) return -1;

    /* Folder names can't contain these characters. */
    for (WCHAR *p = d.text; *p && k < 127; p++)
        if (*p >= 32 && !wcschr(L"<>:\"/\\|?*", *p)) clean[k++] = *p;
    clean[k] = 0;
    StrTrimW(clean, L" .");
    if (!clean[0]) return 0;
    StringCchCopyW(out, n, clean);
    return 1;
}

/* ---------- manager window: see and edit all groups ---------- */

enum {
    IDC_GROUPS = 200, IDC_APPS, IDC_APPSLABEL,
    IDB_NEW = 210, IDB_RENAME, IDB_DELETE, IDB_ADD, IDB_REMOVE, IDB_OPEN, IDB_PIN, IDB_ICON
};

#define MAX_GROUPS 128
static struct {
    HWND wnd, groups, apps, label;
    HFONT font;
    double scale;
    Paths p;
    WCHAR names[MAX_GROUPS][MAX_PATH];
    int ng;
} g_mg;

static int cmp_names(const void *a, const void *b) { return StrCmpLogicalW((const WCHAR *)a, (const WCHAR *)b); }

static BOOL current_group(WCHAR *out)
{
    int sel = ListView_GetNextItem(g_mg.groups, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= g_mg.ng) return FALSE;
    PathCombineW(out, g_mg.p.groupsRoot, g_mg.names[sel]);
    return TRUE;
}

static void group_files(const WCHAR *name, WCHAR *lnk, WCHAR *ico)
{
    WCHAR group[MAX_PATH], aumid[128], tmp[MAX_PATH];
    PathCombineW(group, g_mg.p.groupsRoot, name);
    if (lnk) { StringCchPrintfW(tmp, MAX_PATH, L"%s.lnk", name); PathCombineW(lnk, g_mg.p.smGroups, tmp); }
    if (ico) { make_aumid(group, aumid, 128); StringCchPrintfW(tmp, MAX_PATH, L"%s.ico", aumid); PathCombineW(ico, g_mg.p.iconDir, tmp); }
}

static void enable_buttons(void)
{
    WCHAR g[MAX_PATH];
    BOOL hasGroup = current_group(g);
    BOOL hasApp = ListView_GetSelectedCount(g_mg.apps) > 0;
    EnableWindow(GetDlgItem(g_mg.wnd, IDB_RENAME), hasGroup);
    EnableWindow(GetDlgItem(g_mg.wnd, IDB_DELETE), hasGroup);
    EnableWindow(GetDlgItem(g_mg.wnd, IDB_ADD), hasGroup);
    EnableWindow(GetDlgItem(g_mg.wnd, IDB_OPEN), hasGroup);
    EnableWindow(GetDlgItem(g_mg.wnd, IDB_PIN), hasGroup);
    EnableWindow(GetDlgItem(g_mg.wnd, IDB_ICON), hasGroup);
    EnableWindow(GetDlgItem(g_mg.wnd, IDB_REMOVE), hasGroup && hasApp);
}

static void load_app_list(void)
{
    WCHAR group[MAX_PATH], label[MAX_PATH + 64];
    RECT rc;
    ListView_DeleteAllItems(g_mg.apps);
    if (!current_group(group)) {
        SetWindowTextW(g_mg.label, g_mg.ng ? L"Select a group" : L"No groups yet. Click \"New group\" to create one.");
        enable_buttons();
        return;
    }
    enumerate(group);
    for (int i = 0; i < g_count; i++) {
        SHFILEINFOW sfi;
        LVITEMW it;
        ZeroMemory(&sfi, sizeof sfi);
        SHGetFileInfoW(g_items[i].path, 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        it.iItem = i;
        it.pszText = g_items[i].name;
        it.iImage = sfi.iIcon;
        it.lParam = i;
        ListView_InsertItem(g_mg.apps, &it);
    }
    GetClientRect(g_mg.apps, &rc);
    ListView_SetColumnWidth(g_mg.apps, 0, rc.right - rc.left);
    StringCchPrintfW(label, MAX_PATH + 64, L"Apps in \"%s\" (%d)  -  double-click to start", PathFindFileNameW(group), g_count);
    SetWindowTextW(g_mg.label, label);
    enable_buttons();
}

static void load_group_list(const WCHAR *select)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    RECT rc;
    int sel = 0;
    g_mg.ng = 0;
    PathCombineW(pattern, g_mg.p.groupsRoot, L"*");
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && g_mg.ng < MAX_GROUPS)
                StringCchCopyW(g_mg.names[g_mg.ng++], MAX_PATH, fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(g_mg.names, g_mg.ng, sizeof g_mg.names[0], cmp_names);
    ListView_DeleteAllItems(g_mg.groups);
    for (int i = 0; i < g_mg.ng; i++) {
        WCHAR lnk[MAX_PATH], group[MAX_PATH];
        SHFILEINFOW sfi;
        LVITEMW it;
        group_files(g_mg.names[i], lnk, NULL);
        PathCombineW(group, g_mg.p.groupsRoot, g_mg.names[i]);
        ZeroMemory(&sfi, sizeof sfi);
        SHGetFileInfoW(GetFileAttributesW(lnk) != INVALID_FILE_ATTRIBUTES ? lnk : group, 0, &sfi, sizeof sfi,
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = i;
        it.pszText = g_mg.names[i];
        it.iImage = sfi.iIcon;
        ListView_InsertItem(g_mg.groups, &it);
        if (select && !lstrcmpiW(select, g_mg.names[i])) sel = i;
    }
    GetClientRect(g_mg.groups, &rc);
    ListView_SetColumnWidth(g_mg.groups, 0, rc.right - rc.left);
    if (g_mg.ng) {
        ListView_SetItemState(g_mg.groups, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_mg.groups, sel, FALSE);
    }
    load_app_list();
}

/* ---------- pinning through Windows' official TaskbarManager API ----------
   Windows.UI.Shell.TaskbarManager.RequestPinCurrentAppAsync shows Windows' own "pin to taskbar?"
   confirmation. Unpackaged apps may use it without a Microsoft token since KB5074105
   (builds 26100.7705 / 26200.7705). "Current app" = this process's AppUserModelID, which must match
   a Start menu shortcut - that is why --pin runs as a separate process with the group's ID.
   Interface IDs and method order are taken from Microsoft's Windows metadata (windows crate). */

static const GUID IID_TaskbarStatics = {0xdb32ab74, 0xde52, 0x4fe6, {0xb7, 0xb6, 0x95, 0xff, 0x9f, 0x83, 0x95, 0xdf}};
static const GUID IID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

typedef HRESULT(WINAPI *RoGetActivationFactory_t)(void *, REFIID, void **);
typedef HRESULT(WINAPI *WindowsCreateString_t)(LPCWSTR, UINT32, void **);
typedef HRESULT(WINAPI *WindowsDeleteString_t)(void *);

#define VT(obj, i) ((*(void ***)(obj))[i])
static void rt_release(void *o) { if (o) ((ULONG(WINAPI *)(void *))VT(o, 2))(o); }

/* Waits for an IAsyncOperation<bool> while keeping the window responsive. Returns -1 on failure. */
static int await_bool(void *op)
{
    void *info = NULL;
    int status = 0, result = -1;
    unsigned char value = 0;
    if (FAILED(((HRESULT(WINAPI *)(void *, REFIID, void **))VT(op, 0))(op, &IID_AsyncInfo, &info))) return -1;
    for (;;) {
        if (FAILED(((HRESULT(WINAPI *)(void *, int *))VT(info, 7))(info, &status))) break; /* IAsyncInfo.get_Status */
        if (status != 0) break;                                                               /* 0 = Started */
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (status == 1 && SUCCEEDED(((HRESULT(WINAPI *)(void *, unsigned char *))VT(op, 8))(op, &value))) /* GetResults */
        result = value ? 1 : 0;
    rt_release(info);
    return result;
}

/* 0 = pinned (or already pinned), 1 = user declined / not allowed, 2 = API not available here */
static int request_pin_current_app(void)
{
    HMODULE cb = LoadLibraryExW(L"combase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cb) return 2;
    RoGetActivationFactory_t getFactory = (RoGetActivationFactory_t)(void *)GetProcAddress(cb, "RoGetActivationFactory");
    WindowsCreateString_t mkStr = (WindowsCreateString_t)(void *)GetProcAddress(cb, "WindowsCreateString");
    WindowsDeleteString_t delStr = (WindowsDeleteString_t)(void *)GetProcAddress(cb, "WindowsDeleteString");
    if (!getFactory || !mkStr || !delStr) return 2;

    static const WCHAR cls[] = L"Windows.UI.Shell.TaskbarManager";
    void *hs = NULL, *statics = NULL, *mgr = NULL, *op = NULL;
    unsigned char supported = 0, allowed = 0;
    int rc = 2;
    if (FAILED(mkStr(cls, (UINT32)(sizeof cls / sizeof(WCHAR) - 1), &hs))) return 2;
    if (SUCCEEDED(getFactory(hs, &IID_TaskbarStatics, &statics)) &&
        SUCCEEDED(((HRESULT(WINAPI *)(void *, void **))VT(statics, 6))(statics, &mgr)) && /* GetDefault */
        SUCCEEDED(((HRESULT(WINAPI *)(void *, unsigned char *))VT(mgr, 6))(mgr, &supported)) && supported &&
        SUCCEEDED(((HRESULT(WINAPI *)(void *, unsigned char *))VT(mgr, 7))(mgr, &allowed))) {
        if (!allowed) rc = 1;
        else if (SUCCEEDED(((HRESULT(WINAPI *)(void *, void **))VT(mgr, 10))(mgr, &op))) { /* RequestPinCurrentAppAsync */
            int r = await_bool(op);
            rc = r == 1 ? 0 : (r == 0 ? 1 : 2);
        }
    }
    rt_release(op);
    rt_release(mgr);
    rt_release(statics);
    delStr(hs);
    return rc;
}

static LRESULT CALLBACK pin_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LRESULT cr;
    if (theme_ctlcolor(m, w, &cr)) return cr;
    return DefWindowProcW(h, m, w, l);
}

/* --pin "<group>": runs with the group's AppUserModelID so Windows pins that group. */
static int run_pin(const WCHAR *arg)
{
    WCHAR folder[MAX_PATH], aumid[128], text[MAX_PATH + 64];
    MONITORINFO mi;
    if (!GetFullPathNameW(arg, MAX_PATH, folder, NULL)) return 2;
    PathRemoveBackslashW(folder);
    make_aumid(folder, aumid, 128);
    SetCurrentProcessExplicitAppUserModelID(aumid);

    /* The request must come from a foreground window, so show a small status window. */
    double sc = monitor_scale(NULL, &mi);
    HFONT font = ui_font(sc);
    StringCchPrintfW(text, MAX_PATH + 64, L"Pinning \"%s\" to the taskbar...", PathFindFileNameW(folder));
    HWND hw = ui_create(NULL, L"GroupLauncherPin", pin_proc, L"GroupLauncher", (int)(320 * sc + 0.5),
                        (int)(64 * sc + 0.5), sc, &mi, NULL, WS_CAPTION);
    if (hw) {
        HWND t = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0,
                                 (int)(320 * sc + 0.5), (int)(64 * sc + 0.5), hw, NULL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)font, TRUE);
        ShowWindow(hw, SW_SHOW);
        SetForegroundWindow(hw);
        UpdateWindow(hw);
    }
    int rc = request_pin_current_app();
    if (hw) DestroyWindow(hw);
    DeleteObject(font);
    return rc;
}

/* Pins a group: Windows' pin dialog if available, otherwise one drag by the user. */
static void pin_group(const WCHAR *group)
{
    WCHAR lnk[MAX_PATH], msg[1024], args[MAX_PATH + 16];
    PIDLIST_ABSOLUTE pidl = NULL;
    SHELLEXECUTEINFOW sei;
    DWORD code = 2;
    update_group_shortcut(group, lnk);
    /* Ask Windows to pin it (separate process with the group's ID); Windows shows its own confirmation. */
    StringCchPrintfW(args, MAX_PATH + 16, L"--pin \"%s\"", group);
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_mg.wnd;
    sei.lpFile = g_mg.p.instExe;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    AllowSetForegroundWindow(ASFW_ANY);
    EnableWindow(g_mg.wnd, FALSE);
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        while (MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    EnableWindow(g_mg.wnd, TRUE);
    SetForegroundWindow(g_mg.wnd);
    if (code == 0 || code == 1) return; /* pinned, or the user said no in Windows' dialog */

    /* Fallback for Windows versions without the pin API: the user drags the shortcut once. */
    StringCchPrintfW(msg, 1024,
                     L"Windows didn't allow automatic pinning on this PC.\n\n"
                     L"After you click OK, a folder opens with the \"%s\" icon highlighted. "
                     L"Drag that icon onto your taskbar to pin the group.",
                     PathFindFileNameW(group));
    ui_msg(g_mg.wnd, msg, L"Pin to taskbar", MB_ICONINFORMATION);
    if (SUCCEEDED(SHParseDisplayName(lnk, NULL, &pidl, 0, NULL))) {
        SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        CoTaskMemFree(pidl);
    }
}

static void recycle(const WCHAR *list /* double-null-terminated */)
{
    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof op);
    op.hwnd = g_mg.wnd;
    op.wFunc = FO_DELETE;
    op.pFrom = list;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    SHFileOperationW(&op);
}

static void on_new_group(void)
{
    WCHAR name[128], group[MAX_PATH], msg[512];
    for (;;) {
        int r = ask_group_name(g_mg.wnd, L"New group", NULL, name, 128);
        if (r < 0) return;
        if (r > 0) break;
        ui_msg(g_mg.wnd, L"Please type a name for the group.", APP_NAME, MB_ICONINFORMATION);
    }
    PathCombineW(group, g_mg.p.groupsRoot, name);
    BOOL existed = PathIsDirectoryW(group);
    SHCreateDirectoryExW(NULL, group, NULL);
    if (!add_apps(g_mg.wnd, group) && !existed) {
        RemoveDirectoryW(group); /* cancelled before picking anything */
        return;
    }
    update_group_shortcut(group, NULL);
    load_group_list(name);
    StringCchPrintfW(msg, 512, L"The group \"%s\" is ready.\n\nPin it to the taskbar now?", name);
    if (ui_msg(g_mg.wnd, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) pin_group(group);
}

static void on_rename(void)
{
    WCHAR oldGroup[MAX_PATH], oldName[MAX_PATH], name[128], group[MAX_PATH], lnk[MAX_PATH], ico[MAX_PATH];
    if (!current_group(oldGroup)) return;
    {
        /* Make sure the group's ID is stored in its folder before the path changes,
           so a pinned group keeps working after the rename. */
        WCHAR keep[128];
        make_aumid(oldGroup, keep, 128);
    }
    StringCchCopyW(oldName, MAX_PATH, PathFindFileNameW(oldGroup));
    if (ask_group_name(g_mg.wnd, L"Rename group", oldName, name, 128) <= 0 || !lstrcmpW(name, oldName)) return;
    PathCombineW(group, g_mg.p.groupsRoot, name);
    if (lstrcmpiW(name, oldName) && PathFileExistsW(group)) {
        ui_msg(g_mg.wnd, L"A group with that name already exists.", APP_NAME, MB_ICONWARNING);
        return;
    }
    if (!MoveFileW(oldGroup, group)) {
        ui_msg(g_mg.wnd, L"The group could not be renamed. Is one of its files open?", APP_NAME, MB_ICONWARNING);
        return;
    }
    {
        WCHAR g2[MAX_PATH], aumid[128];
        PathCombineW(g2, g_mg.p.groupsRoot, oldName);
        make_aumid(g2, aumid, 128);
        group_files(oldName, lnk, NULL);
        DeleteFileW(lnk);
        delete_group_icons(g_mg.p.iconDir, aumid);
        (void)ico;
    }
    update_group_shortcut(group, NULL);
    load_group_list(name);
    (void)0; /* the pinned copy was updated to the new folder by update_group_shortcut() */
}

static void on_delete(void)
{
    WCHAR group[MAX_PATH + 2], name[MAX_PATH], msg[768], lnk[MAX_PATH], ico[MAX_PATH];
    if (!current_group(group)) return;
    StringCchCopyW(name, MAX_PATH, PathFindFileNameW(group));
    StringCchPrintfW(msg, 768,
                     L"Delete the group \"%s\"?\n\nIts shortcuts go to the Recycle Bin. The apps themselves are not touched.\n"
                     L"If the group is pinned to your taskbar, right-click it there and choose Unpin.",
                     name);
    if (ui_msg(g_mg.wnd, msg, L"Delete group", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    group[lstrlenW(group) + 1] = 0; /* SHFileOperation needs a double-null-terminated list */
    recycle(group);
    {
        WCHAR g2[MAX_PATH], aumid[128];
        PathCombineW(g2, g_mg.p.groupsRoot, name);
        make_aumid(g2, aumid, 128);
        group_files(name, lnk, NULL);
        DeleteFileW(lnk);
        delete_group_icons(g_mg.p.iconDir, aumid);
        (void)ico;
    }
    load_group_list(NULL);
}

static void on_remove_apps(void)
{
    WCHAR group[MAX_PATH], *list;
    size_t cap = 4096, len = 0;
    int i = -1;
    if (!current_group(group)) return;
    list = (WCHAR *)calloc(cap, sizeof(WCHAR));
    while ((i = ListView_GetNextItem(g_mg.apps, i, LVNI_SELECTED)) >= 0) {
        size_t l = (size_t)lstrlenW(g_items[i].path) + 1;
        if (len + l + 1 > cap) {
            cap = (len + l + 1) * 2;
            list = (WCHAR *)realloc(list, cap * sizeof(WCHAR));
        }
        memcpy(list + len, g_items[i].path, l * sizeof(WCHAR));
        len += l;
    }
    list[len] = 0;
    if (len) recycle(list);
    free(list);
    update_group_shortcut(group, NULL);
    load_app_list();
}

/* ---------- dark icon picker (grid of icons from imageres.dll or any .ico/.dll/.exe) ---------- */

static struct {
    HWND wnd, lv, label;
    HIMAGELIST il;
    HFONT font;
    double scale;
    int isz, sel;
    BOOL ok;
    WCHAR file[MAX_PATH];
} g_ip;

static void ip_load(void)
{
    WCHAR label[MAX_PATH + 32];
    ListView_DeleteAllItems(g_ip.lv);
    ImageList_RemoveAll(g_ip.il);
    UINT n = ExtractIconExW(g_ip.file, -1, NULL, NULL, 0);
    int selRow = -1;
    SendMessageW(g_ip.lv, WM_SETREDRAW, FALSE, 0);
    for (UINT i = 0; i < n && i < 2000; i++) {
        HICON hi = NULL;
        if (SHDefExtractIconW(g_ip.file, (int)i, 0, &hi, NULL, MAKELONG(g_ip.isz, 16)) != S_OK || !hi) continue;
        int img = ImageList_AddIcon(g_ip.il, hi);
        DestroyIcon(hi);
        LVITEMW it;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_IMAGE | LVIF_PARAM | LVIF_TEXT;
        it.iItem = ListView_GetItemCount(g_ip.lv);
        it.iImage = img;
        it.lParam = (LPARAM)i;
        it.pszText = L"";
        int row = ListView_InsertItem(g_ip.lv, &it);
        if ((int)i == g_ip.sel) selRow = row;
    }
    SendMessageW(g_ip.lv, WM_SETREDRAW, TRUE, 0);
    if (selRow >= 0) {
        ListView_SetItemState(g_ip.lv, selRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_ip.lv, selRow, FALSE);
    }
    StringCchPrintfW(label, MAX_PATH + 32, L"Icons from %s:", PathFindFileNameW(g_ip.file));
    SetWindowTextW(g_ip.label, label);
}

static void ip_browse(void)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *res = NULL;
    COMDLG_FILTERSPEC types[] = {{L"Icons (*.ico, *.dll, *.exe)", L"*.ico;*.dll;*.exe"}};
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg))) return;
    IFileOpenDialog_SetFileTypes(dlg, 1, types);
    IFileOpenDialog_SetTitle(dlg, L"Choose an icon file");
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, g_ip.wnd)) && SUCCEEDED(IFileOpenDialog_GetResult(dlg, &res))) {
        PWSTR path = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(res, SIGDN_FILESYSPATH, &path))) {
            StringCchCopyW(g_ip.file, MAX_PATH, path);
            g_ip.sel = 0;
            CoTaskMemFree(path);
            ip_load();
        }
        IShellItem_Release(res);
    }
    IFileOpenDialog_Release(dlg);
}

static LRESULT CALLBACK iconpick_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LRESULT cr;
    if (theme_ctlcolor(m, w, &cr)) return cr;
    switch (m) {
    case WM_CREATE: {
        HWND c[5];
        g_ip.wnd = h;
#define D(v) ((int)((v) * g_ip.scale + 0.5))
        c[0] = g_ip.label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                                            D(16), D(14), D(488), D(20), h, NULL, NULL, NULL);
        c[1] = g_ip.lv = CreateWindowExW(g_uiDark ? 0 : WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                             LVS_AUTOARRANGE,
                                         D(16), D(40), D(488), D(380), h, (HMENU)300, NULL, NULL);
        c[2] = CreateWindowExW(0, L"BUTTON", L"Other file...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               D(16), D(432), D(110), D(30), h, (HMENU)301, NULL, NULL);
        c[3] = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               D(324), D(432), D(86), D(30), h, (HMENU)IDOK, NULL, NULL);
        c[4] = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               D(418), D(432), D(86), D(30), h, (HMENU)IDCANCEL, NULL, NULL);
        g_ip.isz = D(32);
        ListView_SetIconSpacing(g_ip.lv, g_ip.isz + D(20), g_ip.isz + D(20));
#undef D
        for (int i = 0; i < 5; i++) {
            SendMessageW(c[i], WM_SETFONT, (WPARAM)g_ip.font, TRUE);
            theme_control(c[i]);
        }
        ListView_SetExtendedListViewStyle(g_ip.lv, LVS_EX_DOUBLEBUFFER);
        g_ip.il = ImageList_Create(g_ip.isz, g_ip.isz, ILC_COLOR32, 64, 64);
        ListView_SetImageList(g_ip.lv, g_ip.il, LVSIL_NORMAL);
        ip_load();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 301) ip_browse();
        else if (LOWORD(w) == IDOK) {
            int row = ListView_GetNextItem(g_ip.lv, -1, LVNI_SELECTED);
            if (row >= 0) {
                LVITEMW it;
                ZeroMemory(&it, sizeof it);
                it.mask = LVIF_PARAM;
                it.iItem = row;
                ListView_GetItem(g_ip.lv, &it);
                g_ip.sel = (int)it.lParam;
                g_ip.ok = TRUE;
            }
            DestroyWindow(h);
        } else if (LOWORD(w) == IDCANCEL) DestroyWindow(h);
        return 0;
    case WM_NOTIFY: {
        NMHDR *hd = (NMHDR *)l;
        if (hd->hwndFrom == g_ip.lv && hd->code == NM_DBLCLK && ((NMITEMACTIVATE *)l)->iItem >= 0)
            PostMessageW(h, WM_COMMAND, IDOK, 0);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        ImageList_Destroy(g_ip.il);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* file/idx: in = current choice, out = new choice. */
static BOOL pick_icon(HWND owner, WCHAR *file, int *idx)
{
    MONITORINFO mi;
    ZeroMemory(&g_ip, sizeof g_ip);
    StringCchCopyW(g_ip.file, MAX_PATH, file);
    g_ip.sel = *idx;
    g_ip.scale = monitor_scale(owner, &mi);
    g_ip.font = ui_font(g_ip.scale);
    HWND hw = ui_create(owner, L"GroupLauncherIcons", iconpick_proc, L"Choose an icon", (int)(520 * g_ip.scale + 0.5),
                        (int)(478 * g_ip.scale + 0.5), g_ip.scale, &mi, NULL, WS_CAPTION | WS_SYSMENU);
    if (hw) ui_modal_loop(owner, hw, g_ip.lv);
    DeleteObject(g_ip.font);
    if (!g_ip.ok) return FALSE;
    StringCchCopyW(file, MAX_PATH, g_ip.file);
    *idx = g_ip.sel;
    return TRUE;
}

/* ---------- searchable icon picker (Windows icons with names) ---------- */

/* Tile colors: index 0 = Windows accent color, the rest are fixed presets. */
static const struct { const WCHAR *name; COLORREF c; } g_tileColors[] = {
    {L"Accent", 0},           {L"Blue", RGB(0, 120, 212)},  {L"Teal", RGB(3, 131, 135)},
    {L"Green", RGB(16, 124, 16)}, {L"Yellow", RGB(193, 156, 0)}, {L"Orange", RGB(202, 80, 16)},
    {L"Red", RGB(209, 52, 56)},   {L"Pink", RGB(227, 0, 140)},   {L"Purple", RGB(135, 100, 184)},
    {L"Graphite", RGB(76, 74, 72)},
};
#define NCOLORS ((int)(sizeof g_tileColors / sizeof g_tileColors[0]))
#define IDC_SWATCH0 420
#define IDC_PREVIEW 440

static COLORREF tile_color(int i) { return i == 0 ? accent_color() : g_tileColors[i].c; }

static struct {
    HWND wnd, lv, search, okBtn, preview;
    HIMAGELIST il;
    HFONT font;
    double scale;
    int isz, sel; /* sel = index into g_glyphs */
    int color;    /* index into g_tileColors */
    BOOL ok;
} g_gp;

/* Live preview of the chosen icon on the chosen tile color. */
static void gp_draw_preview(const DRAWITEMSTRUCT *di)
{
    int w = di->rcItem.right - di->rcItem.left, s = di->rcItem.bottom - di->rcItem.top;
    FillRect(di->hDC, &di->rcItem, g_bgBrush);
    if (g_gp.sel < 0) return;
    BITMAPINFO bi;
    void *bits = NULL;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC mdc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    ZeroMemory(bits, (size_t)s * s * 4);
    GlyphCtx g = {g_glyphs[g_gp.sel].code, tile_color(g_gp.color)};
    compose_glyph((DWORD *)bits, s, &g);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(di->hDC, di->rcItem.left + (w - s) / 2, di->rcItem.top, s, s, mdc, 0, 0, s, s, bf);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
}

static void gp_draw_swatch(const DRAWITEMSTRUCT *di)
{
    int i = (int)di->CtlID - IDC_SWATCH0;
    RECT r = di->rcItem;
    HBRUSH b = CreateSolidBrush(tile_color(i));
    FillRect(di->hDC, &r, g_bgBrush);
    InflateRect(&r, -3, -3);
    HGDIOBJ ob = SelectObject(di->hDC, b);
    HGDIOBJ op = SelectObject(di->hDC, GetStockObject(NULL_PEN));
    RoundRect(di->hDC, r.left, r.top, r.right, r.bottom, 8, 8);
    SelectObject(di->hDC, op);
    SelectObject(di->hDC, ob);
    DeleteObject(b);
    if (i == g_gp.color || (di->itemState & ODS_FOCUS)) {
        /* ring around the selected color */
        HPEN pen = CreatePen(PS_SOLID, 2, g_fgCol);
        op = SelectObject(di->hDC, pen);
        ob = SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
        RoundRect(di->hDC, di->rcItem.left + 1, di->rcItem.top + 1, di->rcItem.right - 1, di->rcItem.bottom - 1, 10, 10);
        SelectObject(di->hDC, ob);
        SelectObject(di->hDC, op);
        DeleteObject(pen);
    }
    if (i == 0) {
        /* "A" marks the accent color, which follows Windows' color setting */
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, RGB(255, 255, 255));
        HGDIOBJ of = SelectObject(di->hDC, g_gp.font);
        DrawTextW(di->hDC, L"A", 1, (RECT *)&di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(di->hDC, of);
    }
}

static void gp_fill(void)
{
    WCHAR q[64];
    int selRow = -1;
    GetWindowTextW(g_gp.search, q, 64);
    StrTrimW(q, L" ");
    SendMessageW(g_gp.lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_gp.lv);
    for (int i = 0; i < NGLYPHS; i++) {
        if (q[0] && !StrStrIW(g_glyphs[i].name, q) && !StrStrIW(g_glyphs[i].keys, q)) continue;
        LVITEMW it;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_IMAGE | LVIF_PARAM | LVIF_TEXT;
        it.iItem = ListView_GetItemCount(g_gp.lv);
        it.iImage = i;
        it.lParam = i;
        it.pszText = (LPWSTR)g_glyphs[i].name;
        int row = ListView_InsertItem(g_gp.lv, &it);
        if (i == g_gp.sel) selRow = row;
    }
    SendMessageW(g_gp.lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_gp.lv, NULL, TRUE);
    if (selRow < 0 && ListView_GetItemCount(g_gp.lv) > 0) selRow = 0;
    if (selRow >= 0) {
        ListView_SetItemState(g_gp.lv, selRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_gp.lv, selRow, FALSE);
    }
    EnableWindow(g_gp.okBtn, ListView_GetItemCount(g_gp.lv) > 0);
}

/* Renders every glyph once into the image list (text color, transparent background). */
static void gp_build_images(void)
{
    BITMAPINFO bi;
    int s = g_gp.isz;
    g_gp.il = ImageList_Create(s, s, ILC_COLOR32, NGLYPHS, 8);
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    for (int i = 0; i < NGLYPHS; i++) {
        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        BYTE *m = glyph_mask(g_glyphs[i].code, s, (int)(s * 0.8f + 0.5f));
        for (int k = 0; k < s * s; k++)
            ((DWORD *)bits)[k] = pm(GetRValue(g_fgCol), GetGValue(g_fgCol), GetBValue(g_fgCol), m[k]);
        free(m);
        ImageList_Add(g_gp.il, bmp, NULL);
        DeleteObject(bmp);
    }
}

static LRESULT CALLBACK glyphpick_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LRESULT cr;
    if (theme_ctlcolor(m, w, &cr)) return cr;
    switch (m) {
    case WM_CREATE: {
        HWND c[5];
        g_gp.wnd = h;
#define D(v) ((int)((v) * g_gp.scale + 0.5))
        c[0] = CreateWindowExW(0, L"STATIC", L"Search Windows icons (English names, e.g. game, folder, terminal):",
                               WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, D(16), D(14), D(568), D(20), h, NULL, NULL, NULL);
        c[1] = g_gp.search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                             D(16), D(40), D(568), D(28), h, (HMENU)401, NULL, NULL);
        c[2] = g_gp.lv = CreateWindowExW(g_uiDark ? 0 : WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                             LVS_AUTOARRANGE,
                                         D(16), D(78), D(568), D(330), h, (HMENU)402, NULL, NULL);
        c[3] = g_gp.okBtn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                            D(404), D(490), D(86), D(30), h, (HMENU)IDOK, NULL, NULL);
        c[4] = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               D(498), D(490), D(86), D(30), h, (HMENU)IDCANCEL, NULL, NULL);
        {
            HWND t = CreateWindowExW(0, L"STATIC", L"Tile color:", WS_CHILD | WS_VISIBLE, D(16), D(426), D(84), D(20),
                                     h, NULL, NULL, NULL);
            SendMessageW(t, WM_SETFONT, (WPARAM)g_gp.font, TRUE);
            for (int i = 0; i < NCOLORS; i++)
                CreateWindowExW(0, L"BUTTON", g_tileColors[i].name, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                D(100 + i * 36), D(418), D(32), D(32), h, (HMENU)(INT_PTR)(IDC_SWATCH0 + i), NULL, NULL);
            g_gp.preview = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, D(16), D(460), D(64),
                                           D(64), h, (HMENU)IDC_PREVIEW, NULL, NULL);
        }
        g_gp.isz = D(32);
        ListView_SetIconSpacing(g_gp.lv, D(92), g_gp.isz + D(40));
#undef D
        for (int i = 0; i < 5; i++) {
            SendMessageW(c[i], WM_SETFONT, (WPARAM)g_gp.font, TRUE);
            theme_control(c[i]);
        }
        SendMessageW(g_gp.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search icons");
        ListView_SetExtendedListViewStyle(g_gp.lv, LVS_EX_DOUBLEBUFFER);
        gp_build_images();
        ListView_SetImageList(g_gp.lv, g_gp.il, LVSIL_NORMAL);
        gp_fill();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 401 && HIWORD(w) == EN_CHANGE) gp_fill();
        else if (LOWORD(w) >= IDC_SWATCH0 && LOWORD(w) < IDC_SWATCH0 + NCOLORS) {
            g_gp.color = LOWORD(w) - IDC_SWATCH0;
            for (int i = 0; i < NCOLORS; i++) InvalidateRect(GetDlgItem(h, IDC_SWATCH0 + i), NULL, TRUE);
            InvalidateRect(g_gp.preview, NULL, TRUE);
        } else if (LOWORD(w) == IDOK) {
            int row = ListView_GetNextItem(g_gp.lv, -1, LVNI_SELECTED);
            if (row >= 0) {
                LVITEMW it;
                ZeroMemory(&it, sizeof it);
                it.mask = LVIF_PARAM;
                it.iItem = row;
                ListView_GetItem(g_gp.lv, &it);
                g_gp.sel = (int)it.lParam;
                g_gp.ok = TRUE;
                DestroyWindow(h);
            }
        } else if (LOWORD(w) == IDCANCEL) DestroyWindow(h);
        return 0;
    case WM_NOTIFY: {
        NMHDR *hd = (NMHDR *)l;
        if (hd->hwndFrom == g_gp.lv && hd->code == NM_DBLCLK && ((NMITEMACTIVATE *)l)->iItem >= 0)
            PostMessageW(h, WM_COMMAND, IDOK, 0);
        else if (hd->hwndFrom == g_gp.lv && hd->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nm = (NMLISTVIEW *)l;
            if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) {
                LVITEMW it;
                ZeroMemory(&it, sizeof it);
                it.mask = LVIF_PARAM;
                it.iItem = nm->iItem;
                ListView_GetItem(g_gp.lv, &it);
                g_gp.sel = (int)it.lParam;
                InvalidateRect(g_gp.preview, NULL, TRUE);
            }
        }
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)l;
        if (di->CtlID == IDC_PREVIEW) gp_draw_preview(di);
        else if (di->CtlID >= IDC_SWATCH0 && di->CtlID < IDC_SWATCH0 + NCOLORS) gp_draw_swatch(di);
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        ImageList_Destroy(g_gp.il);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Returns the chosen glyph code, or 0 if cancelled. current = previously chosen code (or 0);
   color = in: current tile color index, out: chosen one. */
static WCHAR pick_glyph(HWND owner, WCHAR current, int *color)
{
    MONITORINFO mi;
    ZeroMemory(&g_gp, sizeof g_gp);
    g_gp.sel = -1;
    g_gp.color = *color;
    for (int i = 0; i < NGLYPHS; i++)
        if (g_glyphs[i].code == current) g_gp.sel = i;
    g_gp.scale = monitor_scale(owner, &mi);
    g_gp.font = ui_font(g_gp.scale);
    HWND hw = ui_create(owner, L"GroupLauncherGlyphs", glyphpick_proc, L"Choose an icon", (int)(600 * g_gp.scale + 0.5),
                        (int)(536 * g_gp.scale + 0.5), g_gp.scale, &mi, NULL, WS_CAPTION | WS_SYSMENU);
    if (hw) ui_modal_loop(owner, hw, g_gp.search);
    DeleteObject(g_gp.font);
    *color = g_gp.color;
    return (g_gp.ok && g_gp.sel >= 0) ? g_glyphs[g_gp.sel].code : 0;
}

static BOOL choose_icon_file(HWND owner, WCHAR *out)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *res = NULL;
    BOOL ok = FALSE;
    COMDLG_FILTERSPEC types[] = {{L"Icons (*.ico, *.dll, *.exe)", L"*.ico;*.dll;*.exe"}, {L"All files", L"*.*"}};
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg))) return FALSE;
    IFileOpenDialog_SetFileTypes(dlg, 2, types);
    IFileOpenDialog_SetTitle(dlg, L"Choose an icon file");
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner)) && SUCCEEDED(IFileOpenDialog_GetResult(dlg, &res))) {
        PWSTR path = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(res, SIGDN_FILESYSPATH, &path))) {
            StringCchCopyW(out, MAX_PATH, path);
            CoTaskMemFree(path);
            ok = TRUE;
        }
        IShellItem_Release(res);
    }
    IFileOpenDialog_Release(dlg);
    return ok;
}

static void on_icon(void)
{
    enum { ID_GLYPH = 1, ID_FILE, ID_GRID };
    WCHAR group[MAX_PATH], icon[MAX_PATH], f[MAX_PATH], raw[64] = L"";
    int idx = 0;
    RECT rc;
    if (!current_group(group)) return;

    /* Which mode is active right now? */
    WCHAR current = 0;
    PathCombineW(f, group, ICON_CHOICE_FILE);
    HANDLE hf = CreateFileW(f, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD rd = 0;
        ReadFile(hf, raw, sizeof raw - sizeof(WCHAR), &rd, NULL);
        raw[rd / sizeof(WCHAR)] = 0;
        CloseHandle(hf);
    }
    BOOL isGlyph = !wcsncmp(raw, L"glyph:", 6);
    int color = 0;
    if (isGlyph) {
        current = (WCHAR)wcstoul(raw + 6, NULL, 16);
        const WCHAR *cpart = wcschr(raw + 6, L':');
        if (cpart) {
            COLORREF want = parse_glyph_color(raw);
            for (int i = 1; i < NCOLORS; i++)
                if (g_tileColors[i].c == want) color = i;
        }
    }
    BOOL isFile = !isGlyph && read_icon_choice(group, icon, &idx);

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (isGlyph ? MF_CHECKED : 0), ID_GLYPH, L"Windows icon (searchable)...");
    AppendMenuW(m, MF_STRING | (isFile ? MF_CHECKED : 0), ID_FILE, L"Icon from a file (.ico, .dll, .exe)...");
    AppendMenuW(m, MF_STRING | (!isGlyph && !isFile ? MF_CHECKED : 0), ID_GRID, L"Grid of the group's app icons");
    GetWindowRect(GetDlgItem(g_mg.wnd, IDB_ICON), &rc);
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, rc.left, rc.top, g_mg.wnd, NULL);
    DestroyMenu(m);
    if (cmd == ID_GLYPH) {
        WCHAR code = pick_glyph(g_mg.wnd, current, &color), choice[32];
        if (!code) return;
        if (color == 0) StringCchPrintfW(choice, 32, L"glyph:%04X", (unsigned)code);
        else {
            COLORREF c = g_tileColors[color].c;
            StringCchPrintfW(choice, 32, L"glyph:%04X:%02X%02X%02X", (unsigned)code, GetRValue(c), GetGValue(c), GetBValue(c));
        }
        write_icon_choice(group, choice, 0);
    } else if (cmd == ID_FILE) {
        /* File picker first; the icon grid only appears when the file holds several icons (.dll/.exe). */
        if (!choose_icon_file(g_mg.wnd, icon)) return;
        idx = 0;
        if (ExtractIconExW(icon, -1, NULL, NULL, 0) > 1 && !pick_icon(g_mg.wnd, icon, &idx)) return;
        write_icon_choice(group, icon, idx);
    } else if (cmd == ID_GRID) {
        write_icon_choice(group, NULL, 0);
    } else {
        return;
    }
    update_group_shortcut(group, NULL);
    load_group_list(PathFindFileNameW(group));
    if (g_pinnedUpdated)
        ui_msg(g_mg.wnd,
               L"Icon changed, also on the taskbar.\n\nIf the taskbar still shows the old picture, "
               L"it updates the next time Explorer restarts (or after signing out and in).",
               APP_NAME, MB_ICONINFORMATION);
}

static LRESULT CALLBACK manager_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LRESULT cr;
    if (theme_ctlcolor(m, w, &cr)) return cr;
    switch (m) {
    case WM_CREATE: {
        static const struct { int id; const WCHAR *text; int x, y, w; } btns[] = {
            {IDB_NEW, L"New group", 16, 432, 82},  {IDB_RENAME, L"Rename", 104, 432, 70},
            {IDB_DELETE, L"Delete", 180, 432, 64}, {IDB_ADD, L"Add apps...", 260, 432, 96},
            {IDB_REMOVE, L"Remove", 362, 432, 76}, {IDB_OPEN, L"Open folder", 444, 432, 96},
            {IDB_ICON, L"Icon...", 556, 432, 74}, {IDB_PIN, L"Pin to taskbar", 636, 432, 124},
        };
        SHFILEINFOW sfi;
        LVCOLUMNW col;
        HWND t;
        g_mg.wnd = h;
#define D(v) ((int)((v) * g_mg.scale + 0.5))
        t = CreateWindowExW(0, L"STATIC", L"Groups", WS_CHILD | WS_VISIBLE, D(16), D(14), D(228), D(20), h, NULL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)g_mg.font, TRUE);
        g_mg.label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, D(260), D(14), D(500), D(20),
                                     h, (HMENU)IDC_APPSLABEL, NULL, NULL);
        SendMessageW(g_mg.label, WM_SETFONT, (WPARAM)g_mg.font, TRUE);
        g_mg.groups = CreateWindowExW(g_uiDark ? 0 : WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL |
                                          LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                                      D(16), D(40), D(228), D(380), h, (HMENU)IDC_GROUPS, NULL, NULL);
        g_mg.apps = CreateWindowExW(g_uiDark ? 0 : WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS |
                                        LVS_SHAREIMAGELISTS,
                                    D(260), D(40), D(500), D(380), h, (HMENU)IDC_APPS, NULL, NULL);
        HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        HWND lists[2] = {g_mg.groups, g_mg.apps};
        for (int i = 0; i < 2; i++) {
            SendMessageW(lists[i], WM_SETFONT, (WPARAM)g_mg.font, TRUE);
            theme_control(lists[i]);
            ListView_SetExtendedListViewStyle(lists[i], LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_SetImageList(lists[i], il, LVSIL_SMALL);
            ZeroMemory(&col, sizeof col);
            col.mask = LVCF_WIDTH;
            col.cx = 100;
            ListView_InsertColumn(lists[i], 0, &col);
        }
        for (size_t i = 0; i < sizeof btns / sizeof btns[0]; i++) {
            t = CreateWindowExW(0, L"BUTTON", btns[i].text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                D(btns[i].x), D(btns[i].y), D(btns[i].w), D(30), h, (HMENU)(INT_PTR)btns[i].id, NULL, NULL);
            SendMessageW(t, WM_SETFONT, (WPARAM)g_mg.font, TRUE);
            theme_control(t);
        }
#undef D
        load_group_list(NULL);
        return 0;
    }
    case WM_COMMAND: {
        WCHAR group[MAX_PATH];
        switch (LOWORD(w)) {
        case IDB_NEW: on_new_group(); break;
        case IDB_RENAME: on_rename(); break;
        case IDB_DELETE: on_delete(); break;
        case IDB_REMOVE: on_remove_apps(); break;
        case IDB_ADD:
            if (current_group(group) && add_apps(h, group)) {
                update_group_shortcut(group, NULL);
                load_app_list();
            }
            break;
        case IDB_OPEN:
            if (current_group(group)) ShellExecuteW(h, L"open", group, NULL, NULL, SW_SHOWNORMAL);
            break;
        case IDB_PIN:
            if (current_group(group)) pin_group(group);
            break;
        case IDB_ICON:
            on_icon();
            break;
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *hd = (NMHDR *)l;
        if (hd->hwndFrom == g_mg.groups && hd->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nm = (NMLISTVIEW *)l;
            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED) && (nm->uNewState & LVIS_SELECTED))
                load_app_list();
        } else if (hd->hwndFrom == g_mg.apps && hd->code == LVN_ITEMCHANGED) {
            enable_buttons();
        } else if (hd->hwndFrom == g_mg.apps && hd->code == NM_DBLCLK) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)l;
            if (ia->iItem >= 0 && ia->iItem < g_count) ShellExecuteW(h, NULL, g_items[ia->iItem].path, NULL, NULL, SW_SHOWNORMAL);
        } else if (hd->hwndFrom == g_mg.groups && hd->code == NM_DBLCLK) {
            on_rename();
        } else if (hd->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)l)->wVKey == VK_DELETE) {
            if (hd->hwndFrom == g_mg.apps) on_remove_apps();
            else on_delete();
        }
        return 0;
    }
    case WM_ACTIVATE:
        /* Groups may have been edited elsewhere (popup menu, Explorer): refresh on focus. */
        if (LOWORD(w) != WA_INACTIVE && g_mg.groups && IsWindowEnabled(h)) {
            WCHAR group[MAX_PATH];
            if (current_group(group)) load_group_list(PathFindFileNameW(group));
            else load_group_list(NULL);
        }
        break; /* let Windows restore keyboard focus */
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int run_manager(void)
{
    MSG msg;
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES};

    /* Only one manager window: bring the existing one to the front. */
    HWND existing = FindWindowW(L"GroupLauncherManager", NULL);
    if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return 0;
    }
    ZeroMemory(&g_mg, sizeof g_mg);
    if (!get_paths(&g_mg.p) || !install_self(&g_mg.p)) return 1;
    InitCommonControlsEx(&icc);

    MONITORINFO mi;
    g_mg.scale = monitor_scale(NULL, &mi);
    g_mg.font = ui_font(g_mg.scale);
    HWND hw = ui_create(NULL, L"GroupLauncherManager", manager_proc, L"GroupLauncher - Taskbar groups",
                        (int)(776 * g_mg.scale + 0.5), (int)(478 * g_mg.scale + 0.5), g_mg.scale, &mi, NULL,
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    if (!hw) return 1;
    ShowWindow(hw, SW_SHOW);
    SetForegroundWindow(hw);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hw, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g_mg.font);
    return 0;
}

/* Rebuild the taskbar shortcuts of all group folders (e.g. after creating folders by hand). */
static int refresh_all(void)
{
    Paths p;
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    if (!get_paths(&p) || !install_self(&p)) return 1;
    PathCombineW(pattern, p.groupsRoot, L"*");
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
            WCHAR group[MAX_PATH];
            PathCombineW(group, p.groupsRoot, fd.cFileName);
            update_group_shortcut(group, NULL);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    int argc = 0, rc;
    (void)prev; (void)cmd; (void)show;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    theme_init();
    if (argc < 2 || argv[1][0] == L'-') {
        if (argc >= 2 && !lstrcmpiW(argv[1], L"--refresh")) rc = refresh_all();
        else if (argc >= 3 && !lstrcmpiW(argv[1], L"--pin")) rc = run_pin(argv[2]);
        else rc = run_manager();
    } else {
        if (argc >= 4 && !lstrcmpiW(argv[2], L"--anchor")) {
            g_hasAnchor = TRUE;
            g_anchorX = _wtoi(argv[3]);
        }
        rc = run_popup(inst, argv[1]);
    }
    CoUninitialize();
    LocalFree(argv);
    return rc;
}
