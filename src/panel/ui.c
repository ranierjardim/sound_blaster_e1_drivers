/*
 * ui.c - widget drawing, hit testing, slider maths and the dark popup list.
 */
#include "ui.h"
#include "strings.h"
#include <math.h>
#include <stdio.h>

#define POPUP_CLASS L"E1StudioPopup"

const wchar_t *ui_text(const Widget *w)
{
    return w->dyn[0] ? w->dyn : (w->text ? w->text : L"");
}

int ui_is_slider(const Widget *w) { return w->type == W_SLIDER || w->type == W_VSLIDER; }

int ui_focusable(const Widget *w)
{
    if (!w->visible || !w->enabled) return 0;
    switch (w->type) {
    case W_SLIDER: case W_VSLIDER: case W_CHECK: case W_POWER: case W_RADIO: case W_BUTTON: case W_COMBO:
    case W_ICONBTN: case W_HOTKEY: case W_CARD: case W_SWITCH: case W_NAV:
        return 1;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* slider maths                                                                                     */

#define THUMB_HALF 7.0f

double ui_slider_frac(const Widget *w, double v)
{
    double f;
    if (w->vmax <= w->vmin) return 0;
    if (v < w->vmin) v = w->vmin;
    if (v > w->vmax) v = w->vmax;
    if (w->logscale && w->vmin > 0) f = log(v / w->vmin) / log(w->vmax / w->vmin);
    else f = (v - w->vmin) / (w->vmax - w->vmin);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

static double from_frac(const Widget *w, double f)
{
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    if (w->logscale && w->vmin > 0) return w->vmin * pow(w->vmax / w->vmin, f);
    return w->vmin + f * (w->vmax - w->vmin);
}

double ui_snap(const Widget *w, double v)
{
    if (w->step > 0) v = w->vmin + floor((v - w->vmin) / w->step + 0.5) * w->step;
    if (v < w->vmin) v = w->vmin;
    if (v > w->vmax) v = w->vmax;
    if (fabs(v) < 1e-9) v = 0;
    return v;
}

double ui_slider_value_at(const Widget *w, float lx, float ly)
{
    double f;
    if (w->type == W_VSLIDER) {
        float y0 = w->y + THUMB_HALF, y1 = w->y + w->h - THUMB_HALF;
        f = (y1 - ly) / (y1 - y0);
    } else {
        float x0 = w->x + THUMB_HALF, x1 = w->x + w->w - THUMB_HALF;
        f = (lx - x0) / (x1 - x0);
    }
    return ui_snap(w, from_frac(w, f));
}

void ui_format_value(const Widget *w, double v, wchar_t *out, int cch)
{
    switch (w->fmt) {
    case VF_PCT01: _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%d%%", (int)floor(v * 100 + 0.5)); break;
    case VF_PCT100: _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%d%%", (int)floor(v + 0.5)); break;
    case VF_HZ: _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%d Hz", (int)floor(v + 0.5)); break;
    case VF_DB:
        if (fabs(v) < 0.05) _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"0.0 dB");
        else _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%+.1f dB", v);
        str_decimal(out);
        break;
    case VF_DB_INT:
        if (fabs(v) < 0.05) _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"0");
        else if (fabs(v - floor(v + 0.5)) < 0.05) _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%+d", (int)floor(v + 0.5));
        else _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%+.1f", v);
        str_decimal(out);
        break;
    case VF_BAL: {
        int p = (int)floor(fabs(v) * 100 + 0.5);
        int pt = str_lang() == LANG_PT_;
        if (p == 0) _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"C");
        else _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%s %d", v < 0 ? (pt ? L"E" : L"L") : (pt ? L"D" : L"R"), p);
        break;
    }
    case VF_VOL: _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%d", (int)floor(v * 100 + 0.5)); break;
    default: out[0] = 0;
    }
}

void ui_bounds(const Widget *w, float *x, float *y, float *bw, float *bh)
{
    float m = 4;
    *x = w->x - m;
    *y = w->y - m;
    *bw = w->w + 2 * m;
    *bh = w->h + 2 * m;
    if (w->type == W_SLIDER) {
        *y = w->y - 10;
        *bh = w->h + 20;
        *x = w->x - 8;
        *bw = w->w + 16 + (w->value_w > 0 ? w->value_w + 14 : 0);
    } else if (w->type == W_VSLIDER) {
        *x = w->x - 12;
        *bw = w->w + 24;
        *y = w->y - 26;
        *bh = w->h + 34;
    }
}

int ui_hit(const Widget *w, float lx, float ly)
{
    if (!w->visible) return 0;
    float x = w->x, y = w->y, ww = w->w, hh = w->h;
    if (w->type == W_SLIDER) { y -= 6; hh += 12; x -= 4; ww += 8; }
    if (w->type == W_VSLIDER) { x -= 8; ww += 16; y -= 4; hh += 8; }
    return lx >= x && lx < x + ww && ly >= y && ly < y + hh;
}

/* ---------------------------------------------------------------------------------------------- */
/* drawing                                                                                          */

static void focus_ring(Gfx *x, float lx, float ly, float w, float h, float r)
{
    gfx_stroke_rrect(x, lx - 3, ly - 3, w + 6, h + 6, r + 3, 1.5f, argb_alpha(C_ACCENT, 0.9f));
}

static void draw_hslider(Gfx *x, const Widget *w, int hover, int pressed, int focus)
{
    float x0 = w->x + THUMB_HALF, x1 = w->x + w->w - THUMB_HALF, cy = w->y + w->h / 2;
    float f = (float)ui_slider_frac(w, w->value);
    float tx = x0 + (x1 - x0) * f;
    int en = w->enabled;
    gfx_fill_rrect(x, x0 - 2, cy - 2, (x1 - x0) + 4, 4, 2, en ? C_TRACK : RGBX(0x2A2A2A));
    float fz = w->bipolar ? x0 + (x1 - x0) * (float)ui_slider_frac(w, 0.0) : x0 - 2;
    float a = fz < tx ? fz : tx, b = fz < tx ? tx : fz;
    if (b - a > 0.5f) gfx_fill_rrect(x, a, cy - 2, b - a, 4, 2, en ? C_ACCENT_DK : RGBX(0x3A3A3A));
    if (w->bipolar) gfx_fill_rect(x, fz - 0.5f, cy - 6, 1, 12, RGBX(0x555555));
    ARGB t0 = en ? (hover || pressed ? RGBX(0xC4BCBC) : RGBX(0xA39B9B)) : RGBX(0x4A4A4A);
    ARGB t1 = en ? (hover || pressed ? RGBX(0x8A8282) : RGBX(0x6F6767)) : RGBX(0x3A3A3A);
    gfx_fill_rrect(x, tx - 6.5f, cy - 11.5f, 13, 23, 3.5f, RGBX(0x111111));
    gfx_fill_rrect_vgrad(x, tx - 6, cy - 11, 12, 22, 3, t0, t1);
    gfx_line(x, tx - 2.5f, cy, tx + 2.5f, cy, 1.2f, en ? RGBX(0x4A4444) : RGBX(0x333333));
    if (pressed && en) gfx_stroke_rrect(x, tx - 6, cy - 11, 12, 22, 3, 1.2f, C_ACCENT);
    if (focus) focus_ring(x, tx - 6, cy - 11, 12, 22, 3);
    if (w->value_w > 0) {
        wchar_t v[32];
        ui_format_value(w, w->value, v, 32);
        gfx_text(x, v, w->x + w->w + 12, w->y, w->value_w, w->h, F_BODY, en ? C_TEXT_HI : C_TEXT_DIS,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void draw_vslider(Gfx *x, const Widget *w, int hover, int pressed, int focus)
{
    float y0 = w->y + THUMB_HALF, y1 = w->y + w->h - THUMB_HALF, cx = w->x + w->w / 2;
    float f = (float)ui_slider_frac(w, w->value);
    float ty = y1 - (y1 - y0) * f;
    int en = w->enabled;
    gfx_fill_rrect(x, cx - 2, y0 - 2, 4, (y1 - y0) + 4, 2, en ? C_TRACK : RGBX(0x2A2A2A));
    float fz = w->bipolar ? y1 - (y1 - y0) * (float)ui_slider_frac(w, 0.0) : y1 + 2;
    float a = fz < ty ? fz : ty, b = fz < ty ? ty : fz;
    if (b - a > 0.5f) gfx_fill_rrect(x, cx - 2, a, 4, b - a, 2, en ? C_ACCENT_DK : RGBX(0x3A3A3A));
    ARGB t0 = en ? (hover || pressed ? RGBX(0xC4BCBC) : RGBX(0xA39B9B)) : RGBX(0x4A4A4A);
    ARGB t1 = en ? (hover || pressed ? RGBX(0x8A8282) : RGBX(0x6F6767)) : RGBX(0x3A3A3A);
    gfx_fill_rrect(x, cx - 11.5f, ty - 6.5f, 23, 13, 3.5f, RGBX(0x111111));
    gfx_fill_rrect_vgrad(x, cx - 11, ty - 6, 22, 12, 3, t0, t1);
    gfx_line(x, cx - 4, ty, cx + 4, ty, 1.2f, en ? RGBX(0x4A4444) : RGBX(0x333333));
    if (pressed && en) gfx_stroke_rrect(x, cx - 11, ty - 6, 22, 12, 3, 1.2f, C_ACCENT);
    if (focus) focus_ring(x, cx - 11, ty - 6, 22, 12, 3);
    if (w->value_w > 0) {
        wchar_t v[32];
        ui_format_value(w, w->value, v, 32);
        gfx_text(x, v, cx - w->value_w / 2, w->y - 22, w->value_w, 16, F_SMALL, en ? C_TEXT_HI : C_TEXT_DIS,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static ARGB label_color(const Widget *w, int hover)
{
    if (!w->enabled) return C_TEXT_DIS;
    if (hover) return C_TEXT_HI;
    return w->color ? w->color : C_TEXT;
}

static void draw_check(Gfx *x, const Widget *w, int hover, int focus)
{
    float bs = 22, by = w->y + (w->h - bs) / 2;
    int en = w->enabled;
    gfx_fill_rrect(x, w->x, by, bs, bs, 4, en ? RGBX(0x262626) : RGBX(0x1E1E1E));
    gfx_stroke_rrect(x, w->x, by, bs, bs, 4, 1.2f,
                     !en ? RGBX(0x333333) : (w->checked ? C_ACCENT_DK : (hover ? RGBX(0x8A8A8A) : RGBX(0x555555))));
    if (w->checked) gfx_glyph(x, G_CHECK, w->x + 1, by + 1, 20, en ? C_ACCENT : RGBX(0x5A2A2A));
    if (focus) focus_ring(x, w->x, by, bs, bs, 4);
    const wchar_t *t = ui_text(w);
    if (*t)
        gfx_text(x, t, w->x + bs + 12, w->y, w->w - bs - 12, w->h, w->font, label_color(w, hover && en),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_radio(Gfx *x, const Widget *w, int hover, int focus)
{
    float r = 9, cx = w->x + r, cy = w->y + w->h / 2;
    int en = w->enabled;
    gfx_fill_circle(x, cx, cy, r, en ? RGBX(0x262626) : RGBX(0x1E1E1E));
    gfx_stroke_circle(x, cx, cy, r - 0.6f, 1.2f,
                      !en ? RGBX(0x333333) : (w->checked ? C_ACCENT_DK : (hover ? RGBX(0x8A8A8A) : RGBX(0x555555))));
    if (w->checked) gfx_fill_circle(x, cx, cy, 4.5f, en ? C_ACCENT : RGBX(0x5A2A2A));
    if (focus) focus_ring(x, cx - r, cy - r, 2 * r, 2 * r, r);
    gfx_text(x, ui_text(w), w->x + 2 * r + 8, w->y, w->w - 2 * r - 8, w->h, w->font, label_color(w, hover && en),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_power(Gfx *x, const Widget *w, int hover, int focus)
{
    float r = w->w / 2, cx = w->x + r, cy = w->y + w->h / 2;
    if (w->checked) {
        gfx_fill_circle(x, cx, cy, r + 4, argb_alpha(C_ACCENT, hover ? 0.32f : 0.22f));
        gfx_fill_circle(x, cx, cy, r, hover ? RGBX(0xB80005) : C_POWER_ON);
        gfx_fill_circle(x, cx, cy - r * 0.15f, r * 0.82f, ARGBC(26, 255, 255, 255));
        gfx_glyph(x, G_POWER, cx - r * 0.55f, cy - r * 0.55f, r * 1.1f, RGBX(0xFFFFFF));
    } else {
        gfx_fill_circle(x, cx, cy, r, hover ? RGBX(0x555454) : C_POWER_OFF);
        gfx_glyph(x, G_POWER, cx - r * 0.55f, cy - r * 0.55f, r * 1.1f, hover ? RGBX(0xDDDDDD) : RGBX(0x9A9A9A));
    }
    if (focus) focus_ring(x, w->x, w->y, w->w, w->h, r);
}

static void draw_button(Gfx *x, const Widget *w, int hover, int pressed, int focus)
{
    ARGB bg = !w->enabled ? RGBX(0x1F1F1F) : (pressed ? C_ACCENT_DK : (hover ? C_ACCENT : C_BUTTON));
    gfx_fill_rrect(x, w->x, w->y, w->w, w->h, 4, bg);
    if (w->enabled && !hover && !pressed) gfx_stroke_rrect(x, w->x, w->y, w->w, w->h, 4, 1, RGBX(0x343434));
    if (focus) focus_ring(x, w->x, w->y, w->w, w->h, 4);
    gfx_text(x, ui_text(w), w->x + 4, w->y, w->w - 8, w->h, w->font ? w->font : F_BODY_B,
             w->enabled ? RGBX(0xEDEDED) : C_TEXT_DIS, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_combo(Gfx *x, const Widget *w, int hover, int focus)
{
    gfx_fill_rrect(x, w->x, w->y, w->w, w->h, 4, w->enabled ? C_BUTTON : RGBX(0x1F1F1F));
    gfx_stroke_rrect(x, w->x, w->y, w->w, w->h, 4, 1, hover && w->enabled ? RGBX(0x5A5A5A) : RGBX(0x363636));
    if (focus) focus_ring(x, w->x, w->y, w->w, w->h, 4);
    gfx_text(x, ui_text(w), w->x + 12, w->y, w->w - 44, w->h, w->font ? w->font : F_BODY,
             !w->enabled ? C_TEXT_DIS : (w->color ? w->color : C_TEXT_HI), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    gfx_glyph(x, G_CHEVRON_DOWN, w->x + w->w - 28, w->y + (w->h - 18) / 2, 18, w->enabled ? C_TEXT : C_TEXT_DIS);
}

static void draw_iconbtn(Gfx *x, const Widget *w, int hover, int pressed, int focus)
{
    if (hover || pressed) {
        ARGB bg = w->danger ? (pressed ? C_ACCENT_DK : C_ACCENT) : (pressed ? RGBX(0x333333) : RGBX(0x2A2A2A));
        gfx_fill_rrect(x, w->x, w->y, w->w, w->h, 4, bg);
    }
    if (focus) focus_ring(x, w->x, w->y, w->w, w->h, 4);
    float gs = w->glyph_size > 0 ? w->glyph_size : 18;
    Glyph g = (w->checked && w->glyph_on != w->glyph) ? w->glyph_on : w->glyph;
    ARGB c = !w->enabled ? C_TEXT_DIS : (hover ? C_TEXT_HI : (w->checked && w->glyph_on != w->glyph ? C_ACCENT : C_TEXT));
    gfx_glyph(x, g, w->x + (w->w - gs) / 2, w->y + (w->h - gs) / 2, gs, c);
}

static void draw_hotkey(Gfx *x, const Widget *w, int hover, int focus)
{
    gfx_fill_rrect(x, w->x, w->y, w->w, w->h, 4, w->capturing ? RGBX(0x2A1515) : C_BUTTON);
    gfx_stroke_rrect(x, w->x, w->y, w->w, w->h, 4, w->capturing ? 1.5f : 1.0f,
                     w->capturing ? C_ACCENT : (hover ? RGBX(0x5A5A5A) : RGBX(0x363636)));
    if (focus && !w->capturing) focus_ring(x, w->x, w->y, w->w, w->h, 4);
    gfx_text(x, ui_text(w), w->x + 12, w->y, w->w - 24, w->h, F_BODY, w->capturing ? C_TEXT_HI : (w->color ? w->color : C_TEXT_HI),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_card(Gfx *x, const Widget *w, int hover, int focus)
{
    ARGB bg = w->checked ? RGBX(0x2A1414) : (hover ? RGBX(0x2A2A2A) : RGBX(0x242424));
    gfx_fill_rrect(x, w->x, w->y, w->w, w->h, 8, bg);
    gfx_stroke_rrect(x, w->x, w->y, w->w, w->h, 8, w->checked ? 2.0f : 1.0f,
                     w->checked ? C_ACCENT : (hover ? RGBX(0x4A4A4A) : C_BORDER));
    if (focus) focus_ring(x, w->x, w->y, w->w, w->h, 8);
    float gs = 72;
    gfx_glyph(x, w->glyph, w->x + (w->w - gs) / 2, w->y + 34, gs, w->checked ? C_TEXT_HI : C_TEXT);
    gfx_text(x, ui_text(w), w->x + 12, w->y + 126, w->w - 24, 24, F_LABEL_B, w->checked ? C_TEXT_HI : C_TEXT,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (w->sub) gfx_text(x, w->sub, w->x + 20, w->y + 156, w->w - 40, 48, F_BODY, C_TEXT_DIM, DT_CENTER | DT_WORDBREAK);
    if (w->checked) {
        gfx_fill_circle(x, w->x + w->w - 22, w->y + 22, 11, C_ACCENT);
        gfx_glyph(x, G_CHECK, w->x + w->w - 31, w->y + 13, 18, RGBX(0xFFFFFF));
    }
}

static void draw_switch(Gfx *x, const Widget *w, int hover, int focus)
{
    float sw = 46, sh = 24, sy = w->y + (w->h - sh) / 2;
    int en = w->enabled;
    ARGB bg = !en ? RGBX(0x2A2A2A) : (w->checked ? (hover ? C_ACCENT : C_POWER_ON) : (hover ? RGBX(0x4A4A4A) : RGBX(0x3A3A3A)));
    gfx_fill_rrect(x, w->x, sy, sw, sh, sh / 2, bg);
    float kx = w->checked ? w->x + sw - sh / 2 : w->x + sh / 2;
    gfx_fill_circle(x, kx, sy + sh / 2, sh / 2 - 3, !en ? RGBX(0x555555) : (w->checked ? RGBX(0xFFFFFF) : RGBX(0xA0A0A0)));
    if (focus) focus_ring(x, w->x, sy, sw, sh, sh / 2);
    const wchar_t *t = ui_text(w);
    if (*t)
        gfx_text(x, t, w->x + sw + 14, w->y, w->w - sw - 14, w->h, w->font, label_color(w, hover && en),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_nav(Gfx *x, const Widget *w, int hover, int pressed, int focus)
{
    int sel = w->checked;
    if (sel) {
        gfx_fill_rect_hgrad(x, w->x, w->y, w->w, w->h, C_ACCENT_DK, RGBX(0x4A0305));
        gfx_fill_rect(x, w->x, w->y, 3, w->h, C_ACCENT);
    } else if (hover || pressed) {
        gfx_fill_rect_hgrad(x, w->x, w->y, w->w, w->h, RGBX(0x800000), argb_alpha(RGBX(0x980000), 0.35f));
    }
    if (focus) gfx_stroke_rrect(x, w->x + 2, w->y + 2, w->w - 4, w->h - 4, 2, 1.2f, argb_alpha(C_ACCENT, 0.9f));
    ARGB c = (sel || hover) ? C_TEXT_HI : C_NAV_TEXT;
    gfx_glyph(x, w->glyph, w->x + 20, w->y + (w->h - 24) / 2, 24, c);
    gfx_text(x, ui_text(w), w->x + 60, w->y, w->w - 70, w->h, F_NAV, c, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void ui_draw(Gfx *x, const Widget *w, int hover, int pressed, int focus)
{
    if (!w->visible) return;
    switch (w->type) {
    case W_LABEL:
        gfx_text(x, ui_text(w), w->x, w->y, w->w, w->h, w->font, w->enabled ? (w->color ? w->color : C_TEXT) : C_TEXT_DIS,
                 w->align ? w->align : (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS));
        break;
    case W_PANEL:
        gfx_fill_rrect(x, w->x, w->y, w->w, w->h, 6, w->color ? w->color : C_CARD);
        break;
    case W_SLIDER: draw_hslider(x, w, hover, pressed, focus); break;
    case W_VSLIDER: draw_vslider(x, w, hover, pressed, focus); break;
    case W_CHECK: draw_check(x, w, hover, focus); break;
    case W_RADIO: draw_radio(x, w, hover, focus); break;
    case W_POWER: draw_power(x, w, hover, focus); break;
    case W_BUTTON: draw_button(x, w, hover, pressed, focus); break;
    case W_COMBO: draw_combo(x, w, hover, focus); break;
    case W_ICONBTN: draw_iconbtn(x, w, hover, pressed, focus); break;
    case W_HOTKEY: draw_hotkey(x, w, hover, focus); break;
    case W_CARD: draw_card(x, w, hover, focus); break;
    case W_SWITCH: draw_switch(x, w, hover, focus); break;
    case W_NAV: draw_nav(x, w, hover, pressed, focus); break;
    case W_CUSTOM:
        if (w->draw) w->draw(x, (Widget *)w);
        break;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* popup list                                                                                       */

#define POP_ITEM_H 30.0f
#define POP_SEP_H  9.0f
#define POP_MAX_VIS 14
#define POP_PAD 4.0f

typedef struct PopState {
    const PopupItem *items;
    int   n, sel, hover, top, vis, done, result, scrollable;
    float s, w;
} PopState;

static float item_h(const PopState *p, int i) { return p->items[i].separator ? POP_SEP_H : POP_ITEM_H; }

static int pop_hit(const PopState *p, float ly)
{
    float y = POP_PAD;
    for (int i = p->top; i < p->n && i < p->top + p->vis; ++i) {
        float h = item_h(p, i);
        if (ly >= y && ly < y + h) return i;
        y += h;
    }
    return -1;
}

static float pop_height(const PopState *p)
{
    float h = 2 * POP_PAD;
    for (int i = p->top; i < p->n && i < p->top + p->vis; ++i) h += item_h(p, i);
    return h;
}

static void pop_paint(HWND hwnd, PopState *p)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    Gfx x;
    if (gfx_begin(&x, mem, p->s)) {
        float W = (float)rc.right / p->s, H = (float)rc.bottom / p->s;
        gfx_fill_rect(&x, 0, 0, W, H, RGBX(0x242424));
        gfx_stroke_rrect(&x, 0, 0, W, H, 0, 1, RGBX(0x3E3E3E));
        float y = POP_PAD;
        int any_check = 0;
        for (int i = 0; i < p->n; ++i) any_check |= p->items[i].checked;
        for (int i = p->top; i < p->n && i < p->top + p->vis; ++i) {
            const PopupItem *it = &p->items[i];
            float h = item_h(p, i);
            if (it->separator) {
                gfx_fill_rect(&x, 10, y + h / 2, W - 20, 1, RGBX(0x3A3A3A));
            } else {
                int hot = i == p->hover && !it->disabled;
                if (hot) gfx_fill_rect(&x, 3, y, W - 6, h, C_ACCENT_DK);
                else if (i == p->sel) gfx_fill_rect(&x, 3, y, W - 6, h, RGBX(0x2E2E2E));
                float tx = 12;
                if (any_check) {
                    if (it->checked) gfx_glyph(&x, G_CHECK, 8, y + (h - 16) / 2, 16, hot ? RGBX(0xFFFFFF) : C_ACCENT);
                    tx = 32;
                }
                ARGB c = it->disabled ? C_TEXT_DIS : (hot ? RGBX(0xFFFFFF) : (it->color ? it->color : RGBX(0xD8D8D8)));
                gfx_text(&x, it->text, tx, y, W - tx - (p->scrollable ? 18 : 10), h, F_BODY, c,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            y += h;
        }
        if (p->scrollable) {
            float track = H - 2 * POP_PAD;
            float th = track * (float)p->vis / (float)p->n;
            float ty = POP_PAD + track * (float)p->top / (float)p->n;
            gfx_fill_rrect(&x, W - 8, ty, 4, th, 2, RGBX(0x5A5A5A));
        }
        gfx_end(&x);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK pop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PopState *p = (PopState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT:
        if (p) { pop_paint(hwnd, p); return 0; }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (p) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
            int h = (mx >= 0 && my >= 0 && mx < rc.right && my < rc.bottom) ? pop_hit(p, (float)my / p->s) : -1;
            if (h >= 0 && (p->items[h].separator || p->items[h].disabled)) h = -1;
            if (h != p->hover) { p->hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (p) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
            if (mx < 0 || my < 0 || mx >= rc.right || my >= rc.bottom) { p->done = 1; p->result = -1; }
        }
        return 0;
    case WM_LBUTTONUP:
        if (p) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
            if (mx >= 0 && my >= 0 && mx < rc.right && my < rc.bottom) {
                int h = pop_hit(p, (float)my / p->s);
                if (h >= 0 && !p->items[h].separator && !p->items[h].disabled) { p->done = 1; p->result = h; }
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (p && (HWND)lp != hwnd) { p->done = 1; p->result = -1; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ui_register_classes(HINSTANCE hi)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = pop_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = POPUP_CLASS;
    RegisterClassExW(&wc);
}

static void pop_ensure_visible(PopState *p, int i)
{
    if (!p->scrollable || i < 0) return;
    if (i < p->top) p->top = i;
    if (i >= p->top + p->vis) p->top = i - p->vis + 1;
}

static int pop_step(const PopState *p, int from, int dir)
{
    int i = from;
    for (int k = 0; k < p->n; ++k) {
        i += dir;
        if (i < 0 || i >= p->n) return from;
        if (!p->items[i].separator && !p->items[i].disabled) return i;
    }
    return from;
}

int ui_popup(HWND owner, const RECT *anchor, float min_w, const PopupItem *items, int n, int sel)
{
    PopState p;
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    if (n <= 0) return -1;
    ZeroMemory(&p, sizeof p);
    p.items = items;
    p.n = n;
    p.sel = sel;
    p.hover = (sel >= 0 && sel < n) ? sel : -1;
    p.s = gfx_scale();
    int has_sep = 0;
    for (int i = 0; i < n; ++i) has_sep |= items[i].separator;
    p.vis = (!has_sep && n > POP_MAX_VIS) ? POP_MAX_VIS : n;
    p.scrollable = p.vis < n;
    pop_ensure_visible(&p, p.hover);

    float maxw = min_w;
    for (int i = 0; i < n; ++i) {
        float tw = gfx_text_width(items[i].text, F_BODY) + 60;
        if (tw > maxw) maxw = tw;
    }
    int wpx = (int)(maxw * p.s + 0.5f);
    if (wpx < anchor->right - anchor->left) wpx = anchor->right - anchor->left;
    int hpx = (int)(pop_height(&p) * p.s + 0.5f);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromRect(anchor, MONITOR_DEFAULTTONEAREST), &mi);
    int px = anchor->left, py = anchor->bottom + 2;
    if (py + hpx > mi.rcWork.bottom) py = anchor->top - 2 - hpx;
    if (px + wpx > mi.rcWork.right) px = mi.rcWork.right - wpx;
    if (px < mi.rcWork.left) px = mi.rcWork.left;

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, POPUP_CLASS, L"", WS_POPUP, px, py,
                                wpx, hpx, owner, NULL, hi, &p);
    if (!hwnd) return -1;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    SetCapture(hwnd);
    while (!p.done) {
        MSG m;
        int r = GetMessageW(&m, NULL, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)m.wParam);
            p.result = -1;
            break;
        }
        if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
            switch (m.wParam) {
            case VK_ESCAPE: p.done = 1; p.result = -1; break;
            case VK_UP: p.hover = pop_step(&p, p.hover < 0 ? n : p.hover, -1); break;
            case VK_DOWN: p.hover = pop_step(&p, p.hover, +1); break;
            case VK_HOME: p.hover = pop_step(&p, -1, +1); break;
            case VK_END: p.hover = pop_step(&p, n, -1); break;
            case VK_RETURN: case VK_SPACE:
                if (p.hover >= 0) { p.done = 1; p.result = p.hover; }
                break;
            case VK_MENU: case VK_F10: p.done = 1; p.result = -1; break;
            default: break;
            }
            pop_ensure_visible(&p, p.hover);
            InvalidateRect(hwnd, NULL, FALSE);
            continue;
        }
        if (m.message == WM_MOUSEWHEEL) {
            if (p.scrollable) {
                int d = GET_WHEEL_DELTA_WPARAM(m.wParam) > 0 ? -3 : 3;
                p.top += d;
                if (p.top < 0) p.top = 0;
                if (p.top > n - p.vis) p.top = n - p.vis;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            continue;
        }
        if (m.message == WM_CHAR || m.message == WM_SYSCHAR || m.message == WM_KEYUP || m.message == WM_SYSKEYUP) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (!p.done && GetCapture() != hwnd) { p.done = 1; p.result = -1; }
    }
    if (GetCapture() == hwnd) ReleaseCapture();
    DestroyWindow(hwnd);
    return p.result;
}
