/*
 * main.c - e1panel.exe: borderless per-monitor-DPI window, input routing, painting with page
 * transitions, tray icon, title-bar menu, global hot keys, timers and single-instance handling.
 *
 * Command line:  /tray      start hidden in the notification area
 *                /exit      ask a running instance to quit (used by the uninstaller)
 *                /lang:en|pt  force the UI language      /page:N  open on page N (0..7)
 */
#include "panel.h"
#include "ids.h"
#include "settings.h"
#include "state.h"
#include "dialog.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <math.h>
#include <stdlib.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "shlwapi.lib")

#define WND_CLASS  L"E1StudioPanelWnd"
#define MUTEX_NAME L"Local\\E1Studio.Panel.SingleInstance"
#define ANIM_MS    500u

App g;

/* ---------------------------------------------------------------------------------------------- */
/* widget helpers                                                                                   */

Widget *app_widget(int id)
{
    for (int i = 0; i < g.nw; ++i)
        if (g.w[i].id == id) return &g.w[i];
    return NULL;
}

static void origin_of(const Widget *w, float *ox, float *oy)
{
    if (w->page == PG_CHROME) { *ox = 0; *oy = 0; }
    else { *ox = CONTENT_X; *oy = CONTENT_Y; }
}

static int widget_live(const Widget *w)
{
    return w->visible && (w->page == PG_CHROME || (w->page == g.page && !g.anim));
}

static int interactive(const Widget *w)
{
    return w->type != W_LABEL && w->type != W_PANEL && w->type != W_CUSTOM;
}

static void bounds_px(const Widget *w, RECT *rc)
{
    float ox, oy, bx, by, bw, bh;
    origin_of(w, &ox, &oy);
    ui_bounds(w, &bx, &by, &bw, &bh);
    rc->left = (LONG)floorf((bx + ox) * g.s) - 1;
    rc->top = (LONG)floorf((by + oy) * g.s) - 1;
    rc->right = (LONG)ceilf((bx + ox + bw) * g.s) + 1;
    rc->bottom = (LONG)ceilf((by + oy + bh) * g.s) + 1;
}

void app_invalidate(void)
{
    if (g.hwnd) InvalidateRect(g.hwnd, NULL, FALSE);
}

void app_invalidate_widget(int id)
{
    Widget *w = app_widget(id);
    RECT rc;
    if (!w || !g.hwnd) return;
    bounds_px(w, &rc);
    InvalidateRect(g.hwnd, &rc, FALSE);
}

void app_screen_rect_of(const Widget *w, RECT *out)
{
    float ox, oy;
    POINT a, b;
    origin_of(w, &ox, &oy);
    a.x = (LONG)((w->x + ox) * g.s);
    a.y = (LONG)((w->y + oy) * g.s);
    b.x = (LONG)((w->x + ox + w->w) * g.s);
    b.y = (LONG)((w->y + oy + w->h) * g.s);
    ClientToScreen(g.hwnd, &a);
    ClientToScreen(g.hwnd, &b);
    out->left = a.x;
    out->top = a.y;
    out->right = b.x;
    out->bottom = b.y;
}

/* topmost live widget under a logical window point (labels only when they carry a tip) */
static int hit_widget(float lx, float ly)
{
    for (int i = g.nw - 1; i >= 0; --i) {
        const Widget *w = &g.w[i];
        if (!widget_live(w)) continue;
        if (!interactive(w) && !(w->type == W_LABEL && w->tip)) continue;
        float ox, oy;
        origin_of(w, &ox, &oy);
        if (w->page != PG_CHROME && (lx < CONTENT_X || ly < CONTENT_Y || lx >= CONTENT_X + CONTENT_W || ly >= CONTENT_Y + CONTENT_H))
            continue;
        if (ui_hit(w, lx - ox, ly - oy)) return i;
    }
    return -1;
}

/* ---------------------------------------------------------------------------------------------- */
/* params                                                                                           */

void app_commit(void)
{
    g.p.version = E1FX_PARAMS_VERSION;
    g.p.size = sizeof(E1Params);
    e1fx_params_sanitize(&g.p);
    sync_publish(&g.sync, &g.p);
    g.save_pending = 1;
    SetTimer(g.hwnd, T_SAVE, 300, NULL);
    pages_sync();
    app_invalidate();
}

static void flush_save(void)
{
    if (!g.save_pending) return;
    g.save_pending = 0;
    KillTimer(g.hwnd, T_SAVE);
    sync_save(&g.sync, &g.p);
    app_invalidate_widget(ID_AD_DEVICE);
}

/* ---------------------------------------------------------------------------------------------- */
/* hot keys                                                                                         */

void app_register_hotkeys(void)
{
    UnregisterHotKey(g.hwnd, 1);
    UnregisterHotKey(g.hwnd, 2);
    for (int k = 0; k < 2; ++k) {
        g.hk_ok[k] = 0;
        if (g.hk[k] && g.hk_capture < 0)
            g.hk_ok[k] = RegisterHotKey(g.hwnd, k + 1, (g.hk[k] >> 16) | MOD_NOREPEAT, g.hk[k] & 0xFFFF) != 0;
    }
}

static void hotkey_capture_end(int store, DWORD value)
{
    int k = g.hk_capture;
    if (k < 0) return;
    if (store) {
        g.hk[k] = value;
        if (value && g.hk[k ^ 1] == value) g.hk[k ^ 1] = 0;
        settings_set_dword(L"HotkeyScoutOn", g.hk[0]);
        settings_set_dword(L"HotkeyScoutOff", g.hk[1]);
    }
    g.hk_capture = -1;
    app_register_hotkeys();
    pages_sync();
    app_invalidate();
}

static int is_modifier_vk(UINT vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN: case VK_LSHIFT: case VK_RSHIFT:
    case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return 1;
    }
    return 0;
}

static void hotkey_capture_key(UINT vk)
{
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
    if (vk == VK_ESCAPE && !mods) { hotkey_capture_end(0, 0); return; }
    if ((vk == VK_BACK || vk == VK_DELETE) && !mods) { hotkey_capture_end(1, 0); return; }
    if (is_modifier_vk(vk)) return;
    /* A system-wide hot key must not swallow ordinary typing: Shift alone (Shift+A = capital A,
       Shift+Tab, ...) is not enough; Ctrl, Alt or Win is required unless the key is F1..F24. */
    if (!panel_hotkey_allowed(mods, vk)) { MessageBeep(MB_ICONWARNING); return; }
    hotkey_capture_end(1, ((DWORD)mods << 16) | (vk & 0xFFFF));
}

/* ---------------------------------------------------------------------------------------------- */
/* tray                                                                                             */

static void tray_add(void)
{
    ZeroMemory(&g.nid, sizeof g.nid);
    g.nid.cbSize = sizeof g.nid;
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    g.nid.hIcon = g.icon_small;
    lstrcpynW(g.nid.szTip, tr(S_APP_NAME), ARRAYSIZE(g.nid.szTip));
    g.tray_added = Shell_NotifyIconW(NIM_ADD, &g.nid) != 0;
    if (g.tray_added) {
        g.nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
    }
}

static void tray_remove(void)
{
    if (g.tray_added) Shell_NotifyIconW(NIM_DELETE, &g.nid);
    g.tray_added = 0;
}

static void tray_balloon(const wchar_t *text)
{
    if (!g.tray_added) return;
    NOTIFYICONDATAW n = g.nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
    lstrcpynW(n.szInfoTitle, tr(S_APP_NAME), ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, text, ARRAYSIZE(n.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void show_main(void)
{
    if (IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
    else ShowWindow(g.hwnd, SW_SHOW);
    SetForegroundWindow(g.hwnd);
    pages_sync();
    app_invalidate();
}

static void hide_to_tray(void)
{
    if (g.hk_capture >= 0) hotkey_capture_end(0, 0);
    ShowWindow(g.hwnd, SW_HIDE);
    flush_save();
    if (!g.tray_added) tray_add();
    if (!settings_get_dword(L"TrayHintShown", 0)) {
        tray_balloon(tr(S_TRAY_STILL_RUNNING));
        settings_set_dword(L"TrayHintShown", 1);
    }
}

enum { CMD_SHOW = 1, CMD_STUDIO, CMD_SCOUT, CMD_EXIT };

static void tray_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, CMD_SHOW, tr(S_TRAY_SHOW));
    SetMenuDefaultItem(m, CMD_SHOW, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g.p.render.master_on ? MF_CHECKED : 0), CMD_STUDIO, tr(S_NAV_STUDIO));
    AppendMenuW(m, MF_STRING | (g.p.render.scout_on ? MF_CHECKED : 0), CMD_SCOUT, tr(S_NAV_SCOUT));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, tr(S_MENU_EXIT));
    SetForegroundWindow(g.hwnd);
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, g.hwnd, NULL);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case CMD_SHOW: show_main(); break;
    case CMD_STUDIO: g.p.render.master_on = !g.p.render.master_on; app_commit(); break;
    case CMD_SCOUT: g.p.render.scout_on = !g.p.render.scout_on; app_commit(); break;
    case CMD_EXIT: DestroyWindow(g.hwnd); break;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* title menu                                                                                       */

/* UI language preference (HKCU): 0 = Windows language, 1 = English, 2 = Portuguese (Brazil). */
#define LANG_PREF_VALUE L"Language"

static void language_menu(const RECT *rc)
{
    PopupItem it[3];
    ZeroMemory(it, sizeof it);
    const DWORD cur = settings_get_dword(LANG_PREF_VALUE, 0);
    lstrcpynW(it[0].text, tr(S_LANG_AUTO), 96);
    lstrcpynW(it[1].text, tr(S_LANG_EN), 96);
    lstrcpynW(it[2].text, tr(S_LANG_PT), 96);
    for (DWORD i = 0; i < 3; ++i) it[i].checked = (cur == i);
    int r = ui_popup(g.hwnd, rc, 260, it, 3, -1);
    if (r < 0 || r > 2 || (DWORD)r == cur) return;
    settings_set_dword(LANG_PREF_VALUE, (DWORD)r);
    dlg_message(g.hwnd, tr(S_LANG_TITLE), tr(S_LANG_RESTART), DLG_OK, 0);
}

static void title_menu(Widget *w)
{
    PopupItem it[9];
    ZeroMemory(it, sizeof it);
    lstrcpynW(it[0].text, tr(S_MENU_STARTUP), 96);
    it[0].checked = autostart_get();
    it[1].separator = 1;
    lstrcpynW(it[2].text, tr(S_MENU_RESTORE), 96);
    it[3].separator = 1;
    lstrcpynW(it[4].text, tr(S_MENU_LANGUAGE), 96);
    lstrcpynW(it[5].text, tr(S_MENU_ABOUT), 96);
    it[6].separator = 1;
    lstrcpynW(it[7].text, tr(S_MENU_EXIT), 96);
    RECT rc;
    app_screen_rect_of(w, &rc);
    g.pressed = -1;
    int r = ui_popup(g.hwnd, &rc, 230, it, 8, -1);
    switch (r) {
    case 0: autostart_set(!it[0].checked); break;
    case 2:
        if (dlg_message(g.hwnd, tr(S_RESTORE_TITLE), tr(S_RESTORE_CONFIRM), DLG_YESNO, 1)) pages_restore_defaults();
        break;
    case 4: language_menu(&rc); break;
    case 5: dlg_about(g.hwnd, E1P_VERSION); break;
    case 7: DestroyWindow(g.hwnd); break;
    default: break;
    }
    app_invalidate();
}

/* ---------------------------------------------------------------------------------------------- */
/* pages and transitions                                                                            */

static void free_anim_bitmaps(void)
{
    for (int i = 0; i < 2; ++i) {
        if (g.adc[i]) {
            SelectObject(g.adc[i], g.aold[i]);
            DeleteDC(g.adc[i]);
        }
        if (g.abmp[i]) DeleteObject(g.abmp[i]);
        g.adc[i] = NULL;
        g.abmp[i] = NULL;
    }
}

static void draw_page_widgets(Gfx *x, int page, int with_state)
{
    for (int i = 0; i < g.nw; ++i) {
        const Widget *w = &g.w[i];
        if (w->page != page || !w->visible) continue;
        ui_draw(x, w, with_state && g.hover == i, with_state && g.pressed == i,
                with_state && g.kbd_cues && g.focus == i && GetFocus() == g.hwnd);
    }
}

static int render_page_bitmap(int slot, int page)
{
    int w = (int)ceilf(CONTENT_W * g.s), h = (int)ceilf(CONTENT_H * g.s);
    HDC screen = GetDC(g.hwnd);
    if (!g.adc[slot]) {
        g.adc[slot] = CreateCompatibleDC(screen);
        g.abmp[slot] = CreateCompatibleBitmap(screen, w, h);
        g.aold[slot] = SelectObject(g.adc[slot], g.abmp[slot]);
    }
    ReleaseDC(g.hwnd, screen);
    if (!g.adc[slot] || !g.abmp[slot]) return 0;
    Gfx x;
    if (!gfx_begin(&x, g.adc[slot], g.s)) return 0;
    pages_draw_page_bg(&x, page);
    draw_page_widgets(&x, page, 0);
    gfx_end(&x);
    return 1;
}

void app_switch_page(int page)
{
    if (page < 0 || page >= PG_COUNT || page == g.page) return;
    if (g.hk_capture >= 0) hotkey_capture_end(0, 0);
    int animate = IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd) && !g.anim;
    if (g.anim) {
        g.anim = 0;
        KillTimer(g.hwnd, T_ANIM);
    }
    if (animate) animate = render_page_bitmap(0, g.page);
    g.page = page;
    g.hover = g.pressed = -1;
    if (g.focus >= 0 && g.w[g.focus].page != PG_CHROME) g.focus = -1;
    if (page == PG_ADVANCED) audio_spatial(&g.spatial);
    pages_sync();
    if (animate && render_page_bitmap(1, page)) {
        g.anim = 1;
        g.anim_t0 = GetTickCount();
        SetTimer(g.hwnd, T_ANIM, 15, NULL);
    }
    app_invalidate();
}

static void draw_transition(HDC dc)
{
    float t = (float)(GetTickCount() - g.anim_t0) / (float)ANIM_MS;
    if (t > 1) t = 1;
    float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    int cx = (int)floorf(CONTENT_X * g.s), cy = (int)floorf(CONTENT_Y * g.s);
    int cw = (int)ceilf(CONTENT_W * g.s), ch = (int)ceilf(CONTENT_H * g.s);
    RECT rc = {cx, cy, cx + cw, cy + ch};
    HBRUSH bg = CreateSolidBrush(RGB(0x19, 0x19, 0x19));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 0, 0};
    bf.SourceConstantAlpha = (BYTE)(255.0f * (1.0f - e));
    if (bf.SourceConstantAlpha) AlphaBlend(dc, cx, cy, cw, ch, g.adc[0], 0, 0, cw, ch, bf);
    float sc = 0.85f + 0.15f * e;
    int dw = (int)(cw * sc), dh = (int)(ch * sc);
    bf.SourceConstantAlpha = (BYTE)(255.0f * e);
    if (bf.SourceConstantAlpha && dw > 0 && dh > 0)
        AlphaBlend(dc, cx + (cw - dw) / 2, cy + (ch - dh) / 2, dw, dh, g.adc[1], 0, 0, cw, ch, bf);
}

/* ---------------------------------------------------------------------------------------------- */
/* painting                                                                                         */

static int ensure_backbuffer(int w, int h)
{
    if (g.memdc && g.bw == w && g.bh == h) return 1;
    if (g.memdc) {
        SelectObject(g.memdc, g.memold);
        DeleteObject(g.membmp);
        DeleteDC(g.memdc);
        g.memdc = NULL;
    }
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(g.hwnd);
    g.memdc = CreateCompatibleDC(screen);
    g.membmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g.membits, NULL, 0);
    ReleaseDC(g.hwnd, screen);
    if (!g.memdc || !g.membmp) return 0;
    g.memold = SelectObject(g.memdc, g.membmp);
    g.bw = w;
    g.bh = h;
    return 1;
}

static void paint(HDC hdc, const RECT *rp)
{
    RECT cr;
    GetClientRect(g.hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0 || !ensure_backbuffer(cr.right, cr.bottom)) return;
    RECT r = *rp;
    IntersectRect(&r, &r, &cr);
    if (IsRectEmpty(&r)) return;
    HRGN clip = CreateRectRgnIndirect(&r);
    SelectClipRgn(g.memdc, clip);
    Gfx x;
    if (gfx_begin(&x, g.memdc, g.s)) {
        pages_draw_chrome(&x);
        for (int i = 0; i < g.nw; ++i) {
            const Widget *w = &g.w[i];
            if (w->page != PG_CHROME || !w->visible) continue;
            RECT b, tmp;
            bounds_px(w, &b);
            if (!IntersectRect(&tmp, &b, &r)) continue;
            ui_draw(&x, w, g.hover == i, g.pressed == i, g.kbd_cues && g.focus == i && GetFocus() == g.hwnd);
        }
        if (!g.anim) {
            x.ox = CONTENT_X;
            x.oy = CONTENT_Y;
            gfx_clip(&x, 0, 0, CONTENT_W, CONTENT_H);
            for (int i = 0; i < g.nw; ++i) {
                const Widget *w = &g.w[i];
                if (w->page != g.page || !w->visible) continue;
                RECT b, tmp;
                bounds_px(w, &b);
                if (!IntersectRect(&tmp, &b, &r)) continue;
                ui_draw(&x, w, g.hover == i, g.pressed == i, g.kbd_cues && g.focus == i && GetFocus() == g.hwnd);
            }
            gfx_unclip(&x);
        }
        gfx_end(&x);
        if (g.anim) draw_transition(g.memdc);
    }
    SelectClipRgn(g.memdc, NULL);
    DeleteObject(clip);
    /* GDI text leaves alpha = 0; the 1-px DWM frame extension needs opaque pixels */
    DWORD *px = (DWORD *)g.membits;
    for (LONG y = r.top; y < r.bottom; ++y)
        for (LONG xx = r.left; xx < r.right; ++xx) px[(size_t)y * (size_t)g.bw + (size_t)xx] |= 0xFF000000u;
    BitBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, g.memdc, r.left, r.top, SRCCOPY);
}

/* ---------------------------------------------------------------------------------------------- */
/* input                                                                                            */

static void local_point(const Widget *w, LPARAM lp, float *lx, float *ly)
{
    float ox, oy;
    origin_of(w, &ox, &oy);
    *lx = (float)GET_X_LPARAM(lp) / g.s - ox;
    *ly = (float)GET_Y_LPARAM(lp) / g.s - oy;
}

static void activate(Widget *w)
{
    if (!w->enabled) return;
    switch (w->type) {
    case W_CHECK: case W_SWITCH: case W_POWER:
        w->checked = !w->checked;
        pages_on_change(w, 1);
        break;
    case W_RADIO:
        if (!w->checked) {
            for (int i = 0; i < g.nw; ++i)
                if (g.w[i].type == W_RADIO && g.w[i].group == w->group && g.w[i].page == w->page) g.w[i].checked = 0;
            w->checked = 1;
            pages_on_change(w, 1);
        }
        break;
    case W_ICONBTN:
        if (w->id == ID_TB_MIN) { ShowWindow(g.hwnd, SW_MINIMIZE); break; }
        if (w->id == ID_TB_CLOSE) { hide_to_tray(); break; }
        if (w->id == ID_TB_MENU) { title_menu(w); break; }
        pages_on_click(w);
        break;
    default:
        pages_on_click(w);
        break;
    }
    app_invalidate();
}

static void slider_set(Widget *w, double v, int final)
{
    v = ui_snap(w, v);
    if (v == w->value && !final) return;
    w->value = v;
    pages_on_change(w, final);
}

static void on_mouse_down(LPARAM lp, int dbl)
{
    float lx = (float)GET_X_LPARAM(lp) / g.s, ly = (float)GET_Y_LPARAM(lp) / g.s;
    int i = hit_widget(lx, ly);
    SetFocus(g.hwnd);
    g.kbd_cues = 0;
    if (g.hk_capture >= 0) {
        int keep = i >= 0 && (g.w[i].id == ID_SC_HK_ON || g.w[i].id == ID_SC_HK_OFF);
        if (!keep) hotkey_capture_end(0, 0);
    }
    if (i < 0) return;
    Widget *w = &g.w[i];
    if (!w->enabled || !interactive(w)) return;
    if (ui_focusable(w)) g.focus = i;
    if (ui_is_slider(w)) {
        if (dbl) { pages_on_dblclick(w); app_invalidate(); return; }
        float x, y;
        local_point(w, lp, &x, &y);
        g.pressed = i;
        g.press_value = w->value;
        SetCapture(g.hwnd);
        slider_set(w, ui_slider_value_at(w, x, y), 0);
        app_invalidate();
        return;
    }
    if (w->type == W_COMBO || w->id == ID_TB_MENU) {
        app_invalidate();
        UpdateWindow(g.hwnd);
        activate(w);
        return;
    }
    g.pressed = i;
    SetCapture(g.hwnd);
    app_invalidate();
}

static void on_mouse_up(LPARAM lp)
{
    int p = g.pressed;
    if (p < 0) return;
    Widget *w = &g.w[p];
    g.pressed = -1;
    if (GetCapture() == g.hwnd) ReleaseCapture();
    if (ui_is_slider(w)) {
        pages_on_change(w, 1);
        app_invalidate();
        return;
    }
    float lx = (float)GET_X_LPARAM(lp) / g.s, ly = (float)GET_Y_LPARAM(lp) / g.s;
    if (hit_widget(lx, ly) == p) activate(w);
    app_invalidate();
}

static void on_mouse_move(LPARAM lp)
{
    if (!g.mouse_tracking) {
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, g.hwnd, 0};
        g.mouse_tracking = TrackMouseEvent(&t) != 0;
    }
    if (g.pressed >= 0) {
        Widget *w = &g.w[g.pressed];
        if (ui_is_slider(w)) {
            float x, y;
            local_point(w, lp, &x, &y);
            slider_set(w, ui_slider_value_at(w, x, y), 0);
            app_invalidate();
        }
        return;
    }
    float lx = (float)GET_X_LPARAM(lp) / g.s, ly = (float)GET_Y_LPARAM(lp) / g.s;
    int h = hit_widget(lx, ly);
    if (h != g.hover) {
        g.hover = h;
        app_invalidate();
    }
}

static void on_wheel(WPARAM wp, LPARAM lp)
{
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(g.hwnd, &pt);
    int i = hit_widget((float)pt.x / g.s, (float)pt.y / g.s);
    if (i < 0) return;
    Widget *w = &g.w[i];
    if (!ui_is_slider(w) || !w->enabled) return;
    double steps = (double)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
    double st = w->wheel_step > 0 ? w->wheel_step : (w->vmax - w->vmin) / 50.0;
    if (w->logscale) slider_set(w, w->value * pow(1.05, steps), 1);
    else slider_set(w, w->value + st * steps, 1);
    app_invalidate();
}

static void focus_next(int dir)
{
    int n = g.nw, start = g.focus;
    for (int k = 1; k <= n; ++k) {
        int i = ((start < 0 ? (dir > 0 ? -1 : 0) : start) + dir * k + n * 4) % n;
        if (widget_live(&g.w[i]) && ui_focusable(&g.w[i])) {
            g.focus = i;
            break;
        }
    }
    g.kbd_cues = 1;
    app_invalidate();
}

static int on_key(UINT vk)
{
    if (g.hk_capture >= 0) {
        hotkey_capture_key(vk);
        return 1;
    }
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && (vk == VK_TAB || vk == VK_NEXT || vk == VK_PRIOR)) {
        int d = (vk == VK_PRIOR || (vk == VK_TAB && shift)) ? -1 : 1;
        app_switch_page((g.page + d + PG_COUNT) % PG_COUNT);
        return 1;
    }
    if (vk == VK_TAB) { focus_next(shift ? -1 : 1); return 1; }
    if (g.focus < 0 || g.focus >= g.nw || !widget_live(&g.w[g.focus])) return 0;
    Widget *w = &g.w[g.focus];
    if (!w->enabled) return 0;
    g.kbd_cues = 1;
    if (ui_is_slider(w)) {
        double st = w->step > 0 ? w->step : (w->vmax - w->vmin) / 100.0;
        double big = (w->vmax - w->vmin) / 10.0;
        switch (vk) {
        case VK_RIGHT: case VK_UP: slider_set(w, w->value + st, 1); break;
        case VK_LEFT: case VK_DOWN: slider_set(w, w->value - st, 1); break;
        case VK_PRIOR: slider_set(w, w->value + big, 1); break;
        case VK_NEXT: slider_set(w, w->value - big, 1); break;
        case VK_HOME: slider_set(w, w->vmin, 1); break;
        case VK_END: slider_set(w, w->vmax, 1); break;
        default: return 0;
        }
        app_invalidate();
        return 1;
    }
    if (vk == VK_SPACE || vk == VK_RETURN) {
        activate(w);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* timers                                                                                           */

static void tick_meters(void)
{
    E1ApoStatus st;
    EngineState e = sync_engine(&g.sync, 0, &st);
    DWORD now = GetTickCount();
    float dt = (float)(now - g.meter_t) / 1000.0f;
    if (dt > 0.5f || dt < 0) dt = 0.5f;
    g.meter_t = now;
    int changed = e != g.eng[0];
    g.eng[0] = e;
    g.st[0] = st;
    for (int ch = 0; ch < 2; ++ch) {
        float target = e == ENG_RUNNING ? 20.0f * log10f(st.peak_out[ch] > 1e-6f ? st.peak_out[ch] : 1e-6f) : -120.0f;
        float fall = g.meter_db[ch] - 30.0f * dt;
        float nv = target > fall ? target : fall;
        if (nv < -120.0f) nv = -120.0f;
        if (fabsf(nv - g.meter_db[ch]) > 0.1f) changed = 1;
        g.meter_db[ch] = nv;
    }
    if (changed) {
        app_invalidate_widget(ID_VOL_METER);
        app_invalidate_widget(ID_NAV_ENGINE);
        if (g.page == PG_ADVANCED) {
            app_invalidate_widget(ID_AD_ENGINE);
            app_invalidate_widget(ID_AD_QUALITY);
        }
    }
}

static void tick_status(void)
{
    if (sync_poll(&g.sync, &g.p)) app_invalidate();
    E1ApoStatus st;
    EngineState e = sync_engine(&g.sync, 1, &st);
    g.st[1] = st;
    if (e != g.eng[1]) {
        g.eng[1] = e;
        app_invalidate();
    }
    if (!IsWindowVisible(g.hwnd) || IsIconic(g.hwnd)) {
        /* keep the render heartbeat tracker fresh while the meters are not ticking */
        g.eng[0] = sync_engine(&g.sync, 0, &g.st[0]);
    }
}

static void tick_spatial(void)
{
    SpatialInfo s;
    audio_spatial(&s);
    if (memcmp(&s, &g.spatial, sizeof s) != 0) {
        g.spatial = s;
        app_invalidate();
    }
}

static void start_visible_timers(void)
{
    g.meter_t = GetTickCount();
    SetTimer(g.hwnd, T_METER, 33, NULL);
    SetTimer(g.hwnd, T_SPATIAL, 3000, NULL);
}

static void stop_visible_timers(void)
{
    KillTimer(g.hwnd, T_METER);
    KillTimer(g.hwnd, T_SPATIAL);
}

/* ---------------------------------------------------------------------------------------------- */
/* window                                                                                           */

static void apply_dpi(UINT dpi)
{
    g.dpi = dpi ? dpi : 96;
    g.s = (float)g.dpi / 96.0f;
    gfx_set_scale(g.s);
    free_anim_bitmaps();
    if (g.anim) {
        g.anim = 0;
        KillTimer(g.hwnd, T_ANIM);
    }
}

static void make_icons(void)
{
    if (g.icon_big) DestroyIcon(g.icon_big);
    if (g.icon_small) DestroyIcon(g.icon_small);
    g.icon_big = gfx_make_app_icon(GetSystemMetricsForDpi(SM_CXICON, g.dpi));
    g.icon_small = gfx_make_app_icon(GetSystemMetricsForDpi(SM_CXSMICON, g.dpi));
    if (g.icon_big) SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, (LPARAM)g.icon_big);
    if (g.icon_small) SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g.icon_small);
    if (g.tray_added && g.icon_small) {
        g.nid.uFlags = NIF_ICON;
        g.nid.hIcon = g.icon_small;
        Shell_NotifyIconW(NIM_MODIFY, &g.nid);
    }
}

static void place_window(void)
{
    int w = (int)ceilf(WIN_W * g.s), h = (int)ceilf(WIN_H * g.s);
    POINT pt = {(LONG)settings_get_dword(L"WindowX", 0x7FFFFFFF), (LONG)settings_get_dword(L"WindowY", 0x7FFFFFFF)};
    HMONITOR mon = (pt.x != 0x7FFFFFFF) ? MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) : NULL;
    if (!mon) {
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        pt.x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        pt.y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    }
    SetWindowPos(g.hwnd, NULL, pt.x, pt.y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    /* the saved point may be on a monitor with another DPI */
    UINT dpi = GetDpiForWindow(g.hwnd);
    if (dpi && dpi != g.dpi) {
        apply_dpi(dpi);
        SetWindowPos(g.hwnd, NULL, 0, 0, (int)ceilf(WIN_W * g.s), (int)ceilf(WIN_H * g.s), SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
}

static void on_create(HWND hwnd)
{
    g.hwnd = hwnd;
    apply_dpi(GetDpiForWindow(hwnd));
    {
        BOOL dark = TRUE;
        DWORD corner = 2; /* DWMWCP_ROUND */
        COLORREF border = RGB(0x32, 0x32, 0x32);
        MARGINS m = {0, 0, 0, 1};
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
        DwmExtendFrameIntoClientArea(hwnd, &m);
    }
    make_icons();
    audio_init(hwnd, WM_APP_DEVCHANGE, WM_APP_VOLCHANGE);
    audio_spatial(&g.spatial);
    pages_load_lists();
    pages_build();
    pages_init_selection();
    app_register_hotkeys();
    pages_sync();
    tray_add();
    place_window();
    SetTimer(hwnd, T_STATUS, 500, NULL);
    g.eng[0] = sync_engine(&g.sync, 0, &g.st[0]);
    g.eng[1] = sync_engine(&g.sync, 1, &g.st[1]);
    for (int ch = 0; ch < 2; ++ch) g.meter_db[ch] = -120.0f;
}

static void on_destroy(void)
{
    KillTimer(g.hwnd, T_STATUS);
    stop_visible_timers();
    KillTimer(g.hwnd, T_ANIM);
    KillTimer(g.hwnd, T_RESCAN);
    flush_save();
    UnregisterHotKey(g.hwnd, 1);
    UnregisterHotKey(g.hwnd, 2);
    tray_remove();
    audio_shutdown();
    sync_close(&g.sync);
    free_anim_bitmaps();
    if (g.memdc) {
        SelectObject(g.memdc, g.memold);
        DeleteObject(g.membmp);
        DeleteDC(g.memdc);
        g.memdc = NULL;
    }
    free(g.profiles);
    free(g.eqp);
    g.profiles = NULL;
    g.eqp = NULL;
    PostQuitMessage(0);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g.msg_taskbar_created && msg == g.msg_taskbar_created) {
        g.tray_added = 0;
        tray_add();
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        on_create(hwnd);
        return 0;
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float lx = (float)pt.x / g.s, ly = (float)pt.y / g.s;
        if (ly < BODY_Y && hit_widget(lx, ly) < 0) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCACTIVATE:
        return DefWindowProcW(hwnd, msg, wp, -1); /* no non-client repaint */
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        float s = g.s > 0 ? g.s : 1.0f;
        int w = (int)ceilf(WIN_W * s), h = (int)ceilf(WIN_H * s);
        mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = mm->ptMaxSize.x = w;
        mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = mm->ptMaxSize.y = h;
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MAXIMIZE || (wp & 0xFFF0) == SC_SIZE) return 0;
        if ((wp & 0xFFF0) == SC_CLOSE) { hide_to_tray(); return 0; }
        break;
    case WM_DPICHANGED: {
        const RECT *sr = (const RECT *)lp;
        apply_dpi(HIWORD(wp));
        make_icons();
        SetWindowPos(hwnd, NULL, sr->left, sr->top, (int)ceilf(WIN_W * g.s), (int)ceilf(WIN_H * g.s), SWP_NOZORDER | SWP_NOACTIVATE);
        app_invalidate();
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        RECT rc;
        if (GetWindowRect(hwnd, &rc) && !IsIconic(hwnd)) {
            settings_set_dword(L"WindowX", (DWORD)rc.left);
            settings_set_dword(L"WindowY", (DWORD)rc.top);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint(hdc, &ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SHOWWINDOW:
        if (wp) start_visible_timers();
        else stop_visible_timers();
        break;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) stop_visible_timers();
        else if (wp == SIZE_RESTORED && IsWindowVisible(hwnd)) start_visible_timers();
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        app_invalidate();
        return 0;
    case WM_ACTIVATEAPP:
        if (!wp && g.hk_capture >= 0) hotkey_capture_end(0, 0);
        break;
    case WM_MOUSEMOVE:
        on_mouse_move(lp);
        return 0;
    case WM_MOUSELEAVE:
        g.mouse_tracking = 0;
        if (g.hover >= 0 && g.pressed < 0) {
            g.hover = -1;
            app_invalidate();
        }
        return 0;
    case WM_LBUTTONDOWN:
        on_mouse_down(lp, 0);
        return 0;
    case WM_LBUTTONDBLCLK:
        on_mouse_down(lp, 1);
        return 0;
    case WM_LBUTTONUP:
        on_mouse_up(lp);
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd && g.pressed >= 0) {
            Widget *w = &g.w[g.pressed];
            g.pressed = -1;
            if (ui_is_slider(w)) pages_on_change(w, 1);
            app_invalidate();
        }
        return 0;
    case WM_MOUSEWHEEL:
        on_wheel(wp, lp);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (on_key((UINT)wp)) return 0;
        break;
    case WM_SYSCHAR:
        if (g.hk_capture >= 0) return 0;
        break;
    case WM_HOTKEY:
        if (wp == 1 || wp == 2) {
            g.p.render.scout_on = wp == 1;
            app_commit();
            if (GetForegroundWindow() != hwnd) tray_balloon(tr(wp == 1 ? S_TRAY_SCOUT_ON : S_TRAY_SCOUT_OFF));
        }
        return 0;
    case WM_TIMER:
        switch (wp) {
        case T_SAVE: flush_save(); break;
        case T_METER: tick_meters(); break;
        case T_STATUS: tick_status(); break;
        case T_SPATIAL: tick_spatial(); break;
        case T_RESCAN:
            KillTimer(hwnd, T_RESCAN);
            audio_rescan();
            audio_spatial(&g.spatial);
            pages_sync();
            app_invalidate();
            break;
        case T_ANIM:
            if (GetTickCount() - g.anim_t0 >= ANIM_MS) {
                g.anim = 0;
                KillTimer(hwnd, T_ANIM);
                app_invalidate();
            } else {
                RECT rc = {(LONG)(CONTENT_X * g.s), (LONG)(CONTENT_Y * g.s), (LONG)ceilf((CONTENT_X + CONTENT_W) * g.s),
                           (LONG)ceilf((CONTENT_Y + CONTENT_H) * g.s)};
                InvalidateRect(hwnd, &rc, FALSE);
            }
            break;
        }
        return 0;
    case WM_APP_DEVCHANGE:
        SetTimer(hwnd, T_RESCAN, 400, NULL);
        return 0;
    case WM_APP_VOLCHANGE:
        audio_refresh_volume((EpKind)wp);
        pages_sync();
        app_invalidate();
        return 0;
    case WM_APP_SHOWME:
        show_main();
        return 0;
    case WM_APP_QUIT:
        DestroyWindow(hwnd);
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP:
            show_main();
            break;
        case WM_CONTEXTMENU:
            tray_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        }
        return 0;
    case WM_CLOSE:
        hide_to_tray();
        return 0;
    case WM_QUERYENDSESSION:
        flush_save();
        return TRUE;
    case WM_ENDSESSION:
        if (wp) flush_save();
        return 0;
    case WM_DESTROY:
        on_destroy();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------------------------------------- */
/* entry                                                                                            */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev;
    (void)cmdline;
    int argc = 0, opt_tray = 0, opt_exit = 0, opt_lang = -1, opt_page = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const wchar_t *a = argv[i];
        if (*a == L'/' || *a == L'-') ++a;
        if (!lstrcmpiW(a, L"tray")) opt_tray = 1;
        else if (!lstrcmpiW(a, L"exit")) opt_exit = 1;
        else if (!StrCmpNIW(a, L"lang:", 5)) opt_lang = !lstrcmpiW(a + 5, L"pt") || !lstrcmpiW(a + 5, L"pt-BR") ? LANG_PT_ : LANG_EN_;
        else if (!StrCmpNIW(a, L"page:", 5)) opt_page = _wtoi(a + 5);
    }
    if (argv) LocalFree(argv);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int tries = 0; tries < 20; ++tries) {
            HWND other = FindWindowW(WND_CLASS, NULL);
            if (other) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                if (opt_exit) PostMessageW(other, WM_APP_QUIT, 0, 0);
                else if (!opt_tray) PostMessageW(other, WM_APP_SHOWME, 0, 0);
                break;
            }
            Sleep(100);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (opt_exit) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    gfx_startup();
    {
        const DWORD pref = settings_get_dword(LANG_PREF_VALUE, 0);   /* /lang: overrides the menu choice */
        str_set_lang(opt_lang >= 0 ? opt_lang : pref == 1 ? LANG_EN_ : pref == 2 ? LANG_PT_ : str_detect_lang());
    }
    ui_register_classes(hi);
    dlg_register(hi);

    ZeroMemory(&g, sizeof g);
    g.hinst = hi;
    g.hover = g.pressed = g.focus = -1;
    g.hk_capture = -1;
    g.start_tray = opt_tray;
    g.page = (opt_page >= 0 && opt_page < PG_COUNT) ? opt_page : 0;
    g.msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g.hk[0] = settings_get_dword(L"HotkeyScoutOn", 0);
    g.hk[1] = settings_get_dword(L"HotkeyScoutOff", 0);
    sync_init(&g.sync, &g.p);
    /* allow the tray / taskbar of an elevated shell to talk to us */
    ChangeWindowMessageFilter(g.msg_taskbar_created, MSGFLT_ADD);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.lpszClassName = WND_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, WND_CLASS, tr(S_APP_NAME),
                                WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, WIN_W, WIN_H, NULL, NULL, hi, NULL);
    if (!hwnd) return 1;
    if (!opt_tray) {
        ShowWindow(hwnd, show == SW_SHOWMINNOACTIVE || show == SW_SHOWMINIMIZED || show == SW_MINIMIZE ? SW_SHOWMINIMIZED : SW_SHOW);
        UpdateWindow(hwnd);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    gfx_shutdown();
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return (int)m.wParam;
}
