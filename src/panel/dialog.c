/*
 * dialog.c - see dialog.h. One window class, three layouts, local modal loop.
 */
#include "dialog.h"
#include "ui.h"
#include "strings.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#define DLG_CLASS L"E1StudioDialog"
#define DLG_W 440.0f
#define PAD 24.0f
#define BTN_W 104.0f
#define BTN_H 34.0f

enum { K_MESSAGE, K_INPUT, K_ABOUT };

typedef struct Dlg {
    int            kind, warn, buttons;
    const wchar_t *title, *text;
    wchar_t       *buf;
    int            cch, maxlen;
    DlgValidate    validate;
    void          *ctx;
    Widget         btn[2];
    int            nbtn, hover, pressed, focus_btn;
    HWND           hwnd, edit;
    HBRUSH         edit_brush;
    float          s, H, text_y, text_x, text_h, edit_y, btn_y;
    const wchar_t *err;
    int            done, result;
} Dlg;

static float measure_text_h(const wchar_t *t, float width, FontId f)
{
    float s = gfx_scale();
    HDC dc = CreateCompatibleDC(NULL);
    RECT rc = {0, 0, (LONG)(width * s), 0};
    HGDIOBJ of = SelectObject(dc, gfx_font(f));
    DrawTextW(dc, t, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteDC(dc);
    return (float)rc.bottom / s;
}

static void layout(Dlg *d)
{
    d->text_x = PAD;
    if (d->kind == K_ABOUT) d->text_x = PAD + 72;
    else if (d->warn) d->text_x = PAD + 42;
    float tw = DLG_W - d->text_x - PAD;
    d->text_y = 62;
    d->text_h = d->text && *d->text ? measure_text_h(d->text, tw, F_BODY) : 0;
    float y = d->text_y + d->text_h;
    if (d->kind == K_ABOUT && y < d->text_y + 64) y = d->text_y + 64;
    if (d->kind == K_INPUT) {
        d->edit_y = y + 10;
        y = d->edit_y + 36 + 22;
    }
    d->btn_y = y + 18;
    d->H = d->btn_y + BTN_H + 20;

    const wchar_t *l1 = tr(S_BTN_OK), *l2 = NULL;
    if (d->buttons == DLG_OKCANCEL) { l1 = tr(S_BTN_OK); l2 = tr(S_CANCEL); }
    else if (d->buttons == DLG_YESNO) { l1 = tr(S_YES); l2 = tr(S_NO); }
    d->nbtn = l2 ? 2 : 1;
    for (int i = 0; i < 2; ++i) {
        Widget *b = &d->btn[i];
        ZeroMemory(b, sizeof *b);
        b->type = W_BUTTON;
        b->visible = b->enabled = 1;
        b->w = BTN_W;
        b->h = BTN_H;
        b->y = d->btn_y;
        b->font = F_BODY_B;
    }
    if (d->nbtn == 2) {
        d->btn[0].text = l1;
        d->btn[1].text = l2;
        d->btn[1].x = DLG_W - PAD - BTN_W;
        d->btn[0].x = d->btn[1].x - BTN_W - 10;
    } else {
        d->btn[0].text = l1;
        d->btn[0].x = DLG_W - PAD - BTN_W;
    }
}

static void paint(Dlg *d)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(d->hwnd, &ps);
    RECT rc;
    GetClientRect(d->hwnd, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    Gfx x;
    if (gfx_begin(&x, mem, d->s)) {
        float W = DLG_W, H = d->H;
        gfx_fill_rect(&x, 0, 0, W, H, C_PANEL);
        gfx_fill_rect(&x, 0, 0, W, 4, d->warn ? C_WARN : C_ACCENT);
        gfx_stroke_rrect(&x, 0, 0, W, H, 0, 1, C_BORDER);
        gfx_text(&x, d->title, PAD, 18, W - 2 * PAD, 28, F_HEAD, C_TEXT_HI, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (d->kind == K_ABOUT) gfx_draw_emblem(&x, PAD, d->text_y, 56);
        else if (d->warn) gfx_glyph(&x, G_WARN, PAD, d->text_y - 2, 30, C_WARN);
        if (d->text_h > 0)
            gfx_text(&x, d->text, d->text_x, d->text_y, W - d->text_x - PAD, d->text_h + 4, F_BODY, C_TEXT, DT_LEFT | DT_WORDBREAK);
        if (d->kind == K_INPUT) {
            int focused = GetFocus() == d->edit;
            gfx_fill_rrect(&x, PAD, d->edit_y, W - 2 * PAD, 36, 4, C_BUTTON);
            gfx_stroke_rrect(&x, PAD, d->edit_y, W - 2 * PAD, 36, 4, focused ? 1.5f : 1.0f, focused ? C_ACCENT : RGBX(0x3A3A3A));
            if (d->err) gfx_text(&x, d->err, PAD, d->edit_y + 40, W - 2 * PAD, 18, F_SMALL, RGBX(0xFF5A4F),
                                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        for (int i = 0; i < d->nbtn; ++i)
            ui_draw(&x, &d->btn[i], d->hover == i, d->pressed == i, d->focus_btn == i && GetFocus() == d->hwnd);
        gfx_end(&x);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(d->hwnd, &ps);
}

static void finish(Dlg *d, int ok)
{
    if (ok && d->kind == K_INPUT) {
        wchar_t tmp[256];
        GetWindowTextW(d->edit, tmp, 256);
        /* trim surrounding blanks */
        wchar_t *s = tmp;
        while (*s == L' ' || *s == L'\t') ++s;
        int n = lstrlenW(s);
        while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
        const wchar_t *e = d->validate ? d->validate(s, d->ctx) : NULL;
        if (e) {
            d->err = e;
            InvalidateRect(d->hwnd, NULL, FALSE);
            SetFocus(d->edit);
            return;
        }
        lstrcpynW(d->buf, s, d->cch);
    }
    d->result = ok;
    d->done = 1;
}

static int hit_btn(Dlg *d, LPARAM lp)
{
    float lx = (float)(short)LOWORD(lp) / d->s, ly = (float)(short)HIWORD(lp) / d->s;
    for (int i = 0; i < d->nbtn; ++i)
        if (ui_hit(&d->btn[i], lx, ly)) return i;
    return -1;
}

static LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Dlg *d = (Dlg *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_PAINT:
        if (d) { paint(d); return 0; }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
        if (r == HTCLIENT && d) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(hwnd, &pt);
            if ((float)pt.y / d->s < 52) return HTCAPTION;
        }
        return r;
    }
    case WM_MOUSEMOVE:
        if (d) {
            int h = hit_btn(d, lp);
            if (h != d->hover) { d->hover = h; InvalidateRect(hwnd, NULL, FALSE); }
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (d) { d->hover = -1; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        if (d) {
            d->pressed = hit_btn(d, lp);
            if (d->pressed >= 0) SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (d) {
            int h = hit_btn(d, lp), p = d->pressed;
            d->pressed = -1;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            if (h >= 0 && h == p) finish(d, h == 0);
        }
        return 0;
    case WM_COMMAND:
        if (d && (HWND)lp == d->edit && HIWORD(wp) == EN_CHANGE && d->err) {
            d->err = NULL;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (d && (HWND)lp == d->edit && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS))
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CTLCOLOREDIT:
        if (d) {
            SetTextColor((HDC)wp, RGB(0xF0, 0xF0, 0xF0));
            SetBkColor((HDC)wp, RGB(0x26, 0x26, 0x26));
            return (LRESULT)d->edit_brush;
        }
        break;
    case WM_CLOSE:
        if (d) finish(d, 0);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void dlg_register(HINSTANCE hi)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = dlg_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = DLG_CLASS;
    RegisterClassExW(&wc);
}

static int run(Dlg *d, HWND owner)
{
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    d->s = gfx_scale();
    d->hover = d->pressed = -1;
    d->focus_btn = 0;
    layout(d);
    int wpx = (int)(DLG_W * d->s + 0.5f), hpx = (int)(d->H * d->s + 0.5f);
    RECT orc;
    if (!owner || !IsWindowVisible(owner) || IsIconic(owner)) {
        MONITORINFO mi = {sizeof mi};
        POINT pt;
        GetCursorPos(&pt);
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
        orc = mi.rcWork;
    } else GetWindowRect(owner, &orc);
    int px = orc.left + (orc.right - orc.left - wpx) / 2, py = orc.top + (orc.bottom - orc.top - hpx) / 2;

    d->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, DLG_CLASS, d->title, WS_POPUP, px, py, wpx, hpx, owner, NULL, hi, d);
    if (!d->hwnd) return 0;
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(d->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    }
    if (d->kind == K_INPUT) {
        d->edit_brush = CreateSolidBrush(RGB(0x26, 0x26, 0x26));
        int eh = (int)(22 * d->s + 0.5f);
        int ex = (int)((PAD + 10) * d->s), ew = (int)((DLG_W - 2 * PAD - 20) * d->s);
        int ey = (int)(d->edit_y * d->s + (36 * d->s - (float)eh) / 2);
        d->edit = CreateWindowExW(0, L"EDIT", d->buf, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, ex, ey, ew, eh, d->hwnd,
                                  (HMENU)100, hi, NULL);
        SendMessageW(d->edit, WM_SETFONT, (WPARAM)gfx_font(F_LABEL), TRUE);
        SendMessageW(d->edit, EM_SETLIMITTEXT, (WPARAM)d->maxlen, 0);
        SendMessageW(d->edit, EM_SETSEL, 0, -1);
    }
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(d->hwnd, SW_SHOW);
    SetForegroundWindow(d->hwnd);
    SetFocus(d->edit ? d->edit : d->hwnd);

    while (!d->done) {
        MSG m;
        int r = GetMessageW(&m, NULL, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)m.wParam);
            d->result = 0;
            break;
        }
        if (m.message == WM_KEYDOWN && (m.hwnd == d->hwnd || m.hwnd == d->edit)) {
            if (m.wParam == VK_RETURN) {
                finish(d, d->nbtn == 1 || m.hwnd == d->edit ? 1 : d->focus_btn == 0);
                continue;
            }
            if (m.wParam == VK_ESCAPE) { finish(d, d->nbtn == 1 ? 1 : 0); continue; }
            if ((m.wParam == VK_TAB || m.wParam == VK_LEFT || m.wParam == VK_RIGHT) && m.hwnd == d->hwnd && d->nbtn == 2) {
                d->focus_btn ^= 1;
                InvalidateRect(d->hwnd, NULL, FALSE);
                continue;
            }
            if (m.wParam == VK_TAB && d->edit) {
                SetFocus(GetFocus() == d->edit ? d->hwnd : d->edit);
                continue;
            }
            if (m.wParam == VK_SPACE && m.hwnd == d->hwnd) {
                finish(d, d->nbtn == 1 ? 1 : d->focus_btn == 0);
                continue;
            }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(d->hwnd);
    if (d->edit_brush) DeleteObject(d->edit_brush);
    if (owner && IsWindowVisible(owner)) SetForegroundWindow(owner);
    return d->result;
}

int dlg_message(HWND owner, const wchar_t *title, const wchar_t *text, int buttons, int warn)
{
    Dlg d;
    ZeroMemory(&d, sizeof d);
    d.kind = K_MESSAGE;
    d.title = title;
    d.text = text;
    d.buttons = buttons;
    d.warn = warn;
    return run(&d, owner);
}

int dlg_input(HWND owner, const wchar_t *title, const wchar_t *prompt, wchar_t *buf, int cch, int maxlen,
              DlgValidate validate, void *ctx)
{
    Dlg d;
    ZeroMemory(&d, sizeof d);
    d.kind = K_INPUT;
    d.title = title;
    d.text = prompt;
    d.buttons = DLG_OKCANCEL;
    d.buf = buf;
    d.cch = cch;
    d.maxlen = maxlen;
    d.validate = validate;
    d.ctx = ctx;
    return run(&d, owner);
}

void dlg_about(HWND owner, const wchar_t *version)
{
    Dlg d;
    wchar_t text[1024];
    ZeroMemory(&d, sizeof d);
    wsprintfW(text, tr(S_ABOUT_TEXT), version);
    d.kind = K_ABOUT;
    d.title = tr(S_ABOUT_TITLE);
    d.text = text;
    d.buttons = DLG_OK;
    run(&d, owner);
}
