/*
 * pages.c - page layouts (docs/re/control-panel.md 3-4 recreated with our own artwork), the
 * mapping between widgets and E1Params / endpoint state, presets, profiles and custom drawings.
 */
/* eqcurve.h (-> e1_biquad.h) first: its inline helpers use a parameter named g */
#include "eqcurve.h"
#include "panel.h"
#include "settings.h"
#include "dialog.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#pragma comment(lib, "comdlg32.lib")

#include "ids.h"
#include "state.h"

/* ---------------------------------------------------------------------------------------------- */
/* builders                                                                                         */

static Widget *add(int page, WType t, int id, float x, float y, float w, float h)
{
    if (g.nw >= MAX_WIDGETS) {
        static Widget dummy;
        ZeroMemory(&dummy, sizeof dummy);
        return &dummy;
    }
    Widget *wd = &g.w[g.nw++];
    ZeroMemory(wd, sizeof *wd);
    wd->id = id;
    wd->page = page;
    wd->type = t;
    wd->x = x;
    wd->y = y;
    wd->w = w;
    wd->h = h;
    wd->visible = wd->enabled = 1;
    wd->font = F_LABEL;
    return wd;
}

static Widget *label(int page, int id, float x, float y, float w, float h, const wchar_t *text, FontId f, ARGB c, UINT align)
{
    Widget *wd = add(page, W_LABEL, id, x, y, w, h);
    wd->text = text;
    wd->font = f;
    wd->color = c;
    wd->align = align;
    return wd;
}

static Widget *panel(int page, float x, float y, float w, float h)
{
    Widget *wd = add(page, W_PANEL, 0, x, y, w, h);
    wd->color = C_CARD;
    return wd;
}

static Widget *check(int page, int id, float x, float y, float w, float h, const wchar_t *text, const wchar_t *tip)
{
    Widget *wd = add(page, W_CHECK, id, x, y, w, h);
    wd->text = text;
    wd->tip = tip;
    return wd;
}

static Widget *hslider(int page, int id, float x, float y, float w, float h, double mn, double mx, double step,
                       double def, ValueFmt fmt, float value_w, const wchar_t *tip)
{
    Widget *wd = add(page, W_SLIDER, id, x, y, w, h);
    wd->vmin = mn;
    wd->vmax = mx;
    wd->step = step;
    wd->def = def;
    wd->value = def;
    wd->fmt = fmt;
    wd->value_w = value_w;
    wd->tip = tip;
    wd->wheel_step = step;
    return wd;
}

static Widget *custom(int page, int id, float x, float y, float w, float h, WDrawFn fn)
{
    Widget *wd = add(page, W_CUSTOM, id, x, y, w, h);
    wd->draw = fn;
    return wd;
}

static Widget *button(int page, int id, float x, float y, float w, float h, const wchar_t *text)
{
    Widget *wd = add(page, W_BUTTON, id, x, y, w, h);
    wd->text = text;
    wd->font = F_BODY_B;
    return wd;
}

#define LBL_SINGLE (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
#define LBL_WRAP   (DT_LEFT | DT_TOP | DT_WORDBREAK)

/* ---------------------------------------------------------------------------------------------- */
/* custom drawings                                                                                  */

static float lin2db(float v) { return v > 1e-6f ? 20.0f * log10f(v) : -120.0f; }

static void draw_meter_bar(Gfx *x, float lx, float ly, float w, float h, float db, int active)
{
    gfx_fill_rrect(x, lx, ly, w, h, h / 2, RGBX(0x2A2A2A));
    if (!active) return;
    float f = (db + 60.0f) / 60.0f;
    if (f <= 0) return;
    if (f > 1) f = 1;
    ARGB c = db > -1.0f ? C_ACCENT : (db > -12.0f ? RGBX(0xD9A21B) : RGBX(0x3FA34D));
    gfx_fill_rrect(x, lx, ly, w * f, h, h / 2, c);
}

static void draw_vol_meter(Gfx *x, Widget *w)
{
    int on = g.eng[0] == ENG_RUNNING;
    draw_meter_bar(x, w->x, w->y, w->w, 2.5f, g.meter_db[0], on);
    draw_meter_bar(x, w->x, w->y + 4, w->w, 2.5f, g.meter_db[1], on);
}

static ARGB engine_color(EngineState e) { return e == ENG_RUNNING ? C_GOOD : (e == ENG_IDLE ? RGBX(0x8A8A8A) : C_WARN); }
static const wchar_t *engine_text(EngineState e)
{
    return e == ENG_RUNNING ? tr(S_ENG_RUNNING) : (e == ENG_IDLE ? tr(S_ENG_IDLE) : tr(S_ENG_NOT_LOADED));
}

static void draw_nav_engine(Gfx *x, Widget *w)
{
    wchar_t t[128];
    gfx_fill_circle(x, w->x + 5, w->y + w->h / 2, 4, engine_color(g.eng[0]));
    _snwprintf_s(t, 128, _TRUNCATE, tr(S_ENGINE_LINE), engine_text(g.eng[0]));
    gfx_text(x, t, w->x + 16, w->y, w->w - 16, w->h, F_SMALL, C_TEXT_DIM, LBL_SINGLE);
}

static void draw_tip(Gfx *x, Widget *w)
{
    const wchar_t *t = NULL;
    if (g.hover >= 0 && g.w[g.hover].page == w->page && g.w[g.hover].tip) t = g.w[g.hover].tip;
    if (!t) t = w->tip;
    if (!t) return;
    gfx_fill_rect(x, w->x, w->y, w->w, 1, C_BORDER);
    gfx_glyph(x, G_INFO, w->x, w->y + 14, 18, C_TEXT_DIM);
    gfx_text(x, t, w->x + 28, w->y + 14, w->w - 28, w->h - 14, F_BODY, C_TEXT_DIM, LBL_WRAP);
}

/* Equalizer response */
static void eq_effective(float *pre, float gains[E1FX_EQ_BANDS], int *active)
{
    static const float scout[E1FX_EQ_BANDS] = E1FX_SCOUT_EQ;
    const E1RenderParams *r = &g.p.render;
    if (r->scout_on) {
        *pre = 0;
        memcpy(gains, scout, sizeof scout);
        *active = 1;
    } else {
        *pre = r->eq_preamp_db;
        memcpy(gains, r->eq_gain_db, sizeof r->eq_gain_db);
        *active = r->eq_on != 0;
    }
}

static void draw_eq_curve(Gfx *x, Widget *w)
{
    static const double freqs[E1FX_EQ_BANDS] = E1FX_EQ_FREQS;
    E1BqCoef c[E1FX_EQ_BANDS];
    float pre, gains[E1FX_EQ_BANDS];
    int active;
    eq_effective(&pre, gains, &active);
    eqc_design(EQC_FS, pre, gains, c);

    gfx_fill_rrect(x, w->x, w->y, w->w, w->h, 6, RGBX(0x141414));
    gfx_stroke_rrect(x, w->x, w->y, w->w, w->h, 6, 1, C_BORDER);
    float px0 = w->x + 40, px1 = w->x + w->w - 14, py0 = w->y + 12, py1 = w->y + w->h - 22;
    const double f0 = 20.0, f1 = 20000.0, lr = log(f1 / f0);

    enum { NPTS = 240 };
    GpPointF pts[NPTS + 2];
    double maxabs = 0;
    double dbv[NPTS];
    for (int i = 0; i < NPTS; ++i) {
        double f = f0 * exp(lr * (double)i / (NPTS - 1));
        dbv[i] = eqc_response_db(c, EQC_FS, f);
        if (fabs(dbv[i]) > maxabs) maxabs = fabs(dbv[i]);
    }
    double range = maxabs <= 11.5 ? 12.0 : (maxabs <= 23.5 ? 24.0 : 36.0);
    float ymid = (py0 + py1) / 2;
#define YDB(d) (ymid - (float)((d) / range) * (py1 - py0) / 2)
    /* grid */
    for (int k = -2; k <= 2; ++k) {
        double d = range / 2 * k;
        float yy = YDB(d);
        gfx_fill_rect(x, px0, yy, px1 - px0, 1, k == 0 ? RGBX(0x3A3A3A) : RGBX(0x242424));
        wchar_t t[16];
        _snwprintf_s(t, 16, _TRUNCATE, k == 0 ? L"0" : L"%+d", (int)d);
        gfx_text(x, t, w->x + 4, yy - 8, 32, 16, F_TINY, C_TEXT_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    static const struct { double f; const wchar_t *t; } fl[] = {
        {50, L"50"}, {100, L"100"}, {200, L"200"}, {500, L"500"}, {1000, L"1k"}, {2000, L"2k"}, {5000, L"5k"}, {10000, L"10k"}};
    for (size_t k = 0; k < sizeof fl / sizeof fl[0]; ++k) {
        float xx = px0 + (px1 - px0) * (float)(log(fl[k].f / f0) / lr);
        gfx_fill_rect(x, xx, py0, 1, py1 - py0, RGBX(0x202020));
        gfx_text(x, fl[k].t, xx - 20, py1 + 4, 40, 14, F_TINY, C_TEXT_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    for (int i = 0; i < NPTS; ++i) {
        pts[i].X = px0 + (px1 - px0) * (float)i / (NPTS - 1);
        pts[i].Y = YDB(dbv[i]);
    }
    ARGB line = active ? RGBX(0xE8322A) : RGBX(0x6A6A6A);
    pts[NPTS].X = px1;
    pts[NPTS].Y = ymid;
    pts[NPTS + 1].X = px0;
    pts[NPTS + 1].Y = ymid;
    gfx_fill_area_vgrad(x, pts, NPTS + 2, py0, py1, argb_alpha(line, 0.30f), argb_alpha(line, 0.30f));
    gfx_polyline(x, pts, NPTS, 2.0f, line);
    for (int b = 0; b < E1FX_EQ_BANDS; ++b) {
        float xx = px0 + (px1 - px0) * (float)(log(freqs[b] / f0) / lr);
        float yy = YDB(eqc_response_db(c, EQC_FS, freqs[b]));
        gfx_fill_circle(x, xx, yy, 3, active ? RGBX(0xFFFFFF) : RGBX(0x7A7A7A));
    }
#undef YDB
    if (g.p.render.scout_on) {
        const wchar_t *t = tr(S_EQ_SCOUT_OVERRIDE);
        float tw = gfx_text_width(t, F_SMALL) + 24;
        gfx_fill_rrect(x, px1 - tw, py0 + 2, tw, 22, 11, argb_alpha(C_ACCENT_DK, 0.9f));
        gfx_text(x, t, px1 - tw, py0 + 2, tw, 22, F_SMALL, RGBX(0xFFFFFF), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (!g.p.render.eq_on) {
        const wchar_t *t = tr(S_OFF);
        gfx_text(x, t, px1 - 120, py0 + 2, 116, 20, F_SMALL, C_TEXT_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

#define EQ_SL_Y   318.0f
#define EQ_SL_H   196.0f
#define EQ_COL0   84.0f
#define EQ_COLW   40.0f
#define EQ_PRE_CX 508.0f

static void draw_eq_axis(Gfx *x, Widget *w)
{
    static const wchar_t *const bands[E1FX_EQ_BANDS] = {L"31", L"62", L"125", L"250", L"500", L"1k", L"2k", L"4k", L"8k", L"16k"};
    (void)w;
    float y0 = EQ_SL_Y + 7, y1 = EQ_SL_Y + EQ_SL_H - 7;
    for (int k = 0; k <= 4; ++k) {
        float yy = y0 + (y1 - y0) * (float)k / 4.0f;
        gfx_fill_rect(x, 62, yy, 404, 1, k == 2 ? RGBX(0x3A3A3A) : RGBX(0x2C2C2C));
        gfx_fill_rect(x, 490, yy, 36, 1, k == 2 ? RGBX(0x3A3A3A) : RGBX(0x2C2C2C));
    }
    gfx_text(x, L"+24", 18, y0 - 8, 38, 16, F_SMALL, C_TEXT_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    gfx_text(x, L"0 dB", 18, (y0 + y1) / 2 - 8, 38, 16, F_SMALL, C_TEXT_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    gfx_text(x, L"−24", 18, y1 - 8, 38, 16, F_SMALL, C_TEXT_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    gfx_text(x, L"+12", 524, y0 - 8, 30, 16, F_SMALL, C_TEXT_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    gfx_text(x, L"−12", 524, y1 - 8, 30, 16, F_SMALL, C_TEXT_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    gfx_fill_rect(x, 478, EQ_SL_Y - 20, 1, EQ_SL_H + 44, C_BORDER);
    for (int i = 0; i < E1FX_EQ_BANDS; ++i)
        gfx_text(x, bands[i], EQ_COL0 + EQ_COLW * (float)i - 20, EQ_SL_Y + EQ_SL_H + 6, 40, 16, F_SMALL, C_TEXT,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    gfx_text(x, tr(S_LEVEL), EQ_PRE_CX - 30, EQ_SL_Y + EQ_SL_H + 6, 60, 16, F_SMALL, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* Advanced: key/value cards. The key column is as wide as the longest key (at most half the card)
   so long translations do not squeeze the values. */
static float key_col(float w, const StrId *keys, int n)
{
    float m = 0;
    for (int i = 0; i < n; ++i) {
        float tw = gfx_text_width(tr(keys[i]), F_BODY);
        if (tw > m) m = tw;
    }
    m += 12;
    return m > w * 0.5f ? w * 0.5f : m;
}

static void kv_row(Gfx *x, float lx, float ly, float w, float kw, const wchar_t *k, const wchar_t *v, ARGB vc)
{
    gfx_text(x, k, lx, ly, kw - 6, 20, F_BODY, C_TEXT_DIM, LBL_SINGLE);
    gfx_text(x, v, lx + kw, ly, w - kw, 20, F_BODY, vc, LBL_SINGLE);
}

static void draw_adv_engine(Gfx *x, Widget *w)
{
    static const StrId keys[] = {S_STATUS, S_PATH, S_STREAM, S_GR, S_XRUNS};
    wchar_t t[96];
    const E1ApoStatus *st = &g.st[0];
    EngineState e = g.eng[0];
    float y = w->y, kw = key_col(w->w, keys, 5);
    gfx_fill_circle(x, w->x + kw + 5, y + 10, 4, engine_color(e));
    kv_row(x, w->x, y, w->w, kw, tr(S_STATUS), L"", C_TEXT_HI);
    gfx_text(x, engine_text(e), w->x + kw + 14, y, w->w - kw - 14, 20, F_BODY, C_TEXT_HI, LBL_SINGLE);
    y += 22;
    const wchar_t *path = e == ENG_RUNNING ? (st->bypass ? tr(S_PATH_BITEXACT) : tr(S_PATH_PROCESSING)) : tr(S_PATH_UNKNOWN);
    kv_row(x, w->x, y, w->w, kw, tr(S_PATH), path, e == ENG_RUNNING && st->bypass ? C_GOOD : C_TEXT_HI);
    y += 22;
    if (e != ENG_NOT_LOADED && st->sample_rate)
        _snwprintf_s(t, 96, _TRUNCATE, L"%u Hz · %u %s", (unsigned)st->sample_rate, (unsigned)st->channels, tr(S_CH_UNIT));
    else lstrcpyW(t, tr(S_PATH_UNKNOWN));
    kv_row(x, w->x, y, w->w, kw, tr(S_STREAM), t, C_TEXT_HI);
    y += 22;
    if (e == ENG_RUNNING) { _snwprintf_s(t, 96, _TRUNCATE, L"%.1f dB", (double)st->gain_reduction_db); str_decimal(t); }
    else lstrcpyW(t, tr(S_PATH_UNKNOWN));
    kv_row(x, w->x, y, w->w, kw, tr(S_GR), t, C_TEXT_HI);
    y += 22;
    _snwprintf_s(t, 96, _TRUNCATE, L"%u", (unsigned)st->xruns);
    kv_row(x, w->x, y, w->w, kw, tr(S_XRUNS), e == ENG_NOT_LOADED ? tr(S_PATH_UNKNOWN) : t, st->xruns ? C_WARN : C_TEXT_HI);
    y += 26;
    for (int ch = 0; ch < 2; ++ch) {
        gfx_text(x, tr(ch ? S_METER_R : S_METER_L), w->x, y - 3, 12, 12, F_TINY, C_TEXT_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_meter_bar(x, w->x + 14, y, w->w - 64, 6, g.meter_db[ch], e == ENG_RUNNING);
        if (e == ENG_RUNNING && g.meter_db[ch] > -99) { _snwprintf_s(t, 96, _TRUNCATE, L"%.1f", (double)g.meter_db[ch]); str_decimal(t); }
        else lstrcpyW(t, L"−∞");
        gfx_text(x, t, w->x + w->w - 46, y - 5, 46, 16, F_TINY, C_TEXT_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        y += 11;
    }
}

static void draw_adv_device(Gfx *x, Widget *w)
{
    static const StrId keys[] = {S_DEVICE, S_ENDPOINT, S_OUTPUT_FMT, S_MIC_FMT, S_SETTINGS_FILE};
    const AudioEp *r = audio_ep(EP_RENDER), *c = audio_ep(EP_CAPTURE);
    wchar_t t[160];
    float y = w->y, kw = key_col(w->w, keys, 5);
    kv_row(x, w->x, y, w->w, kw, tr(S_DEVICE), r->present ? (r->device[0] ? r->device : r->name) : tr(S_NOT_CONNECTED),
           r->present ? C_TEXT_HI : C_WARN);
    y += 22;
    kv_row(x, w->x, y, w->w, kw, tr(S_ENDPOINT), r->present ? (r->desc[0] ? r->desc : r->name) : tr(S_PATH_UNKNOWN), C_TEXT_HI);
    y += 22;
    audio_format_long(r, t, 160);
    kv_row(x, w->x, y, w->w, kw, tr(S_OUTPUT_FMT), r->present && t[0] ? t : tr(S_PATH_UNKNOWN), C_TEXT_HI);
    y += 22;
    audio_format_long(c, t, 160);
    kv_row(x, w->x, y, w->w, kw, tr(S_MIC_FMT), c->present && t[0] ? t : tr(S_PATH_UNKNOWN), C_TEXT_HI);
    y += 22;
    if (g.sync.save_err) _snwprintf_s(t, 160, _TRUNCATE, tr(S_SETTINGS_ERR), g.sync.save_err);
    else lstrcpynW(t, tr(S_SETTINGS_OK), 160);
    kv_row(x, w->x, y, w->w, kw, tr(S_SETTINGS_FILE), t, g.sync.save_err ? C_WARN : C_TEXT_HI);
}

static float quality_row(Gfx *x, float lx, float ly, float w, int state /*1 ok, 0 warn, -1 info*/, const wchar_t *text)
{
    float h = gfx_text_height(text, F_BODY, w - 32);
    h = (h < 22 ? 22 : h) + 10;
    if (state > 0) {
        gfx_fill_circle(x, lx + 10, ly + 10, 10, C_GOOD);
        gfx_glyph(x, G_CHECK, lx + 2, ly + 2, 16, RGBX(0x0F2A12));
    } else if (state == 0) {
        gfx_glyph(x, G_WARN, lx - 1, ly - 1, 22, C_WARN);
    } else {
        gfx_glyph(x, G_INFO, lx, ly, 20, C_TEXT_DIM);
    }
    gfx_text(x, text, lx + 32, ly + 1, w - 32, h, F_BODY, state == 0 ? RGBX(0xF0D9A8) : C_TEXT, LBL_WRAP);
    return ly + h;
}

static void draw_adv_quality(Gfx *x, Widget *w)
{
    wchar_t t[400], f[96];
    const AudioEp *r = audio_ep(EP_RENDER);
    float y = w->y;
    if (!r->present) {
        quality_row(x, w->x, y, w->w, -1, tr(S_Q_NO_DEVICE));
        return;
    }
    if (g.spatial.checked && g.spatial.active) {
        _snwprintf_s(t, 400, _TRUNCATE, tr(S_Q_SPATIAL_ON), g.spatial.name[0] ? g.spatial.name : tr(S_Q_SPATIAL_GENERIC));
        y = quality_row(x, w->x, y, w->w, 0, t);
    } else {
        y = quality_row(x, w->x, y, w->w, 1, tr(S_Q_SPATIAL_OFF));
    }
    if (r->fmt_valid) {
        audio_format_short(r, f, 96);
        int ok = r->valid_bits >= 24 && !r->is_float;
        _snwprintf_s(t, 400, _TRUNCATE, tr(ok ? S_Q_24_OK : S_Q_24_BAD), f);
        y = quality_row(x, w->x, y, w->w, ok, t);
    } else {
        y = quality_row(x, w->x, y, w->w, -1, tr(S_Q_FMT_UNKNOWN));
    }
    int eng_ok = g.eng[0] != ENG_NOT_LOADED;
    quality_row(x, w->x, y, w->w, eng_ok, tr(eng_ok ? S_Q_ENGINE_OK : S_Q_ENGINE_BAD));
}

/* Profile summary */
static void sum_row(Gfx *x, float lx, float *y, float w, const wchar_t *k, const wchar_t *v, int on)
{
    gfx_text(x, k, lx, *y, 196, 20, F_BODY, C_TEXT_DIM, LBL_SINGLE);
    gfx_text(x, v, lx + 204, *y, w - 204, 20, F_BODY, on ? C_TEXT_HI : RGBX(0x8A8A8A), LBL_SINGLE);
    *y += 21.5f;
}

static void onoff_pct(wchar_t *o, int cch, uint32_t on, float v01)
{
    _snwprintf_s(o, (size_t)cch, _TRUNCATE, L"%s · %d%%", on ? tr(S_ON) : tr(S_OFF), (int)floorf(v01 * 100 + 0.5f));
}

static void eq_selection_text(wchar_t *out, int cch);

static void draw_pr_summary(Gfx *x, Widget *w)
{
    const E1RenderParams *r = &g.p.render;
    const E1CaptureParams *c = &g.p.capture;
    wchar_t t[128], e[96];
    float cw = w->w, y = w->y;
    float lx = w->x;
    sum_row(x, lx, &y, cw, tr(S_STUDIO_TITLE), r->master_on ? tr(S_ON) : tr(S_OFF), r->master_on);
    onoff_pct(t, 128, r->surround_on, r->surround_level);
    sum_row(x, lx, &y, cw, tr(S_SURROUND), t, r->master_on && r->surround_on);
    onoff_pct(t, 128, r->crystal_on, r->crystal_level);
    sum_row(x, lx, &y, cw, tr(S_CLARITY), t, r->master_on && r->crystal_on);
    _snwprintf_s(t, 128, _TRUNCATE, L"%s · %d%% · %d Hz", r->bass_on ? tr(S_ON) : tr(S_OFF), (int)floorf(r->bass_level + 0.5f),
                 (int)floorf(r->bass_xover_hz + 0.5f));
    sum_row(x, lx, &y, cw, tr(S_BASS), t, r->master_on && r->bass_on);
    _snwprintf_s(t, 128, _TRUNCATE, L"%s · %d%% · %s", r->svm_on ? tr(S_ON) : tr(S_OFF), (int)floorf(r->svm_level * 100 + 0.5f),
                 tr(r->svm_mode == E1FX_SVM_LOUD ? S_SVM_LOUD : (r->svm_mode == E1FX_SVM_NIGHT ? S_SVM_NIGHT : S_SVM_NORMAL)));
    sum_row(x, lx, &y, cw, tr(S_SMARTVOL), t, r->master_on && r->svm_on);
    onoff_pct(t, 128, r->dialog_on, r->dialog_level);
    sum_row(x, lx, &y, cw, tr(S_DIALOG), t, r->master_on && r->dialog_on);
    eq_selection_text(e, 96);
    _snwprintf_s(t, 128, _TRUNCATE, L"%s · %s", r->eq_on ? tr(S_ON) : tr(S_OFF), e);
    sum_row(x, lx, &y, cw, tr(S_EQ_TITLE), t, r->eq_on);

    y += 4;
    gfx_fill_rect(x, lx, y - 3, cw, 1, RGBX(0x2C2C2C));
    y += 4;
    sum_row(x, lx, &y, cw, tr(S_VOICE_TITLE), c->master_on ? tr(S_ON) : tr(S_OFF), c->master_on);
    {
        const wchar_t *vn = (int)c->vfx_preset < e1_vfx_preset_count ? tr_preset_name(e1_vfx_presets[c->vfx_preset].name) : L"";
        _snwprintf_s(t, 128, _TRUNCATE, L"%s · %s", c->vfx_on ? tr(S_ON) : tr(S_OFF), vn);
        sum_row(x, lx, &y, cw, tr(S_VFX), t, c->master_on && c->vfx_on);
    }
    onoff_pct(t, 128, c->svm_on, c->svm_level);
    sum_row(x, lx, &y, cw, tr(S_SMARTVOL), t, c->master_on && c->svm_on);
    onoff_pct(t, 128, c->nr_on, c->nr_strength);
    sum_row(x, lx, &y, cw, tr(S_NR), t, c->master_on && c->nr_on);
    sum_row(x, lx, &y, cw, tr(S_SCOUT_TITLE), r->scout_on ? tr(S_ON) : tr(S_OFF), r->scout_on);
    sum_row(x, lx, &y, cw, tr(S_NAV_SPEAKERS), tr(r->speaker_mode == E1FX_SPK_STEREO ? S_SPK_STEREO : S_SPK_HEADPHONES), 1);
    _snwprintf_s(t, 128, _TRUNCATE, L"%+.1f dB · %s %s", (double)r->output_gain_db, tr(S_LIMITER),
                 r->limiter_on ? tr(S_ON) : tr(S_OFF));
    str_decimal(t);
    sum_row(x, lx, &y, cw, tr(S_OUTPUT), t, 1);
}

/* ---------------------------------------------------------------------------------------------- */
/* chrome background                                                                                */

const wchar_t *pages_title_status(void)
{
    static wchar_t buf[200];
    const AudioEp *r = audio_ep(EP_RENDER);
    if (!r->present) return tr(S_NOT_CONNECTED);
    wchar_t f[64];
    audio_format_short(r, f, 64);
    const wchar_t *dev = r->device[0] ? r->device : r->name;
    if (f[0]) _snwprintf_s(buf, 200, _TRUNCATE, L"%s — %s", dev, f);
    else lstrcpynW(buf, dev, 200);
    return buf;
}

static void draw_device_art(Gfx *x, float lx, float ly)
{
    /* our own stylised pocket amplifier: rounded body, big volume knob, two jacks */
    gfx_fill_rrect(x, lx + 2, ly + 3, 58, 46, 10, ARGBC(120, 0, 0, 0));
    gfx_fill_rrect_vgrad(x, lx, ly, 58, 46, 10, RGBX(0x3C3C3C), RGBX(0x1A1A1A));
    gfx_stroke_rrect(x, lx, ly, 58, 46, 10, 1, RGBX(0x4E4E4E));
    float cx = lx + 36, cy = ly + 23, r = 15;
    gfx_fill_circle(x, cx, cy, r + 1, RGBX(0x0E0E0E));
    gfx_fill_rrect_vgrad(x, cx - r, cy - r, 2 * r, 2 * r, r, RGBX(0x343434), RGBX(0x151515));
    float vol = audio_ep(EP_RENDER)->present ? audio_ep(EP_RENDER)->vol : 0.0f;
    gfx_arc(x, cx, cy, r - 3.5f, 135, 270, 2.2f, RGBX(0x2C2C2C));
    if (vol > 0.005f) gfx_arc(x, cx, cy, r - 3.5f, 135, 270 * vol, 2.2f, C_ACCENT);
    gfx_fill_circle(x, lx + 10, ly + 15, 3.2f, RGBX(0x0A0A0A));
    gfx_fill_circle(x, lx + 10, ly + 31, 3.2f, RGBX(0x0A0A0A));
    gfx_stroke_circle(x, lx + 10, ly + 15, 3.6f, 0.8f, RGBX(0x5A5A5A));
    gfx_stroke_circle(x, lx + 10, ly + 31, 3.6f, 0.8f, RGBX(0x5A5A5A));
}

void pages_draw_chrome(Gfx *x)
{
    const AudioEp *r = audio_ep(EP_RENDER);
    gfx_fill_rect(x, 0, 0, WIN_W, WIN_H, C_WINDOW);
    /* title bar */
    gfx_draw_emblem(x, 14, 13, 22);
    gfx_text(x, tr(S_LOGO), 44, TITLE_Y, 130, TITLE_H, F_LOGO, C_TEXT_HI, LBL_SINGLE);
    gfx_fill_circle(x, 176, TITLE_Y + TITLE_H / 2.0f, 3.5f, r->present ? C_GOOD : C_WARN);
    gfx_text(x, pages_title_status(), 186, TITLE_Y, 540, TITLE_H, F_BODY, C_TEXT, LBL_SINGLE);
    /* navigation panel */
    gfx_fill_rect(x, NAV_X, BODY_Y, NAV_W, BODY_H, C_PANEL);
    gfx_fill_rect_hgrad(x, NAV_X, BODY_Y, NAV_W, 72, RGBX(0x242424), RGBX(0x191919));
    draw_device_art(x, NAV_X + 18, BODY_Y + 13);
    gfx_text(x, tr(S_LOGO), NAV_X + 92, BODY_Y + 12, 190, 26, F_HEAD, C_TEXT_HI, LBL_SINGLE);
    {
        wchar_t t[96], f[64];
        if (r->present) {
            audio_format_short(r, f, 64);
            _snwprintf_s(t, 96, _TRUNCATE, f[0] ? L"%s · %s" : L"%s", tr(S_CONNECTED), f);
        } else lstrcpynW(t, tr(S_NOT_CONNECTED), 96);
        gfx_text(x, t, NAV_X + 92, BODY_Y + 38, 194, 20, F_SMALL, r->present ? C_TEXT : C_WARN, LBL_SINGLE);
    }
    gfx_fill_rect(x, NAV_X, BODY_Y + 72, NAV_W, 1, C_BORDER);
    gfx_fill_rect(x, NAV_X + 20, 440, NAV_W - 40, 1, C_BORDER);
    /* master volume strip */
    gfx_fill_rect(x, NAV_X, 575, NAV_W, 52, RGBX(0x202020));
    gfx_fill_rect(x, NAV_X, 575, NAV_W, 1, C_BORDER);
    /* content frame */
    gfx_fill_rect(x, CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H, C_PANEL);
}

void pages_draw_page_bg(Gfx *x, int page)
{
    (void)page;
    gfx_fill_rect(x, 0, 0, CONTENT_W, CONTENT_H, C_PANEL);
}

/* ---------------------------------------------------------------------------------------------- */
/* build                                                                                            */

static const StrId k_nav_text[7] = {S_NAV_STUDIO, S_NAV_VOICE, S_NAV_SCOUT, S_NAV_SPEAKERS, S_NAV_MIXER, S_NAV_EQ, S_NAV_ADVANCED};
static const Glyph k_nav_glyph[7] = {G_STUDIO, G_VOICE, G_SCOUT, G_HEADPHONES, G_MIXER, G_EQ, G_ADVANCED};

static void build_chrome(void)
{
    Widget *w;
    w = add(PG_CHROME, W_ICONBTN, ID_TB_MENU, 740, 8, 44, 31);
    w->glyph = w->glyph_on = G_MENU;
    w->tip = tr(S_TIP_MENU);
    w = add(PG_CHROME, W_ICONBTN, ID_TB_MIN, 784, 8, 44, 31);
    w->glyph = w->glyph_on = G_MINIMIZE;
    w->tip = tr(S_TIP_MIN);
    w = add(PG_CHROME, W_ICONBTN, ID_TB_CLOSE, 828, 8, 44, 31);
    w->glyph = w->glyph_on = G_CLOSE;
    w->danger = 1;
    w->tip = tr(S_TIP_CLOSE);
    for (int i = 0; i < 7; ++i) {
        w = add(PG_CHROME, W_NAV, ID_NAV0 + i, NAV_X, 124 + 41.0f * (float)i, NAV_W, 41);
        w->text = tr(k_nav_text[i]);
        w->glyph = k_nav_glyph[i];
    }
    w = add(PG_CHROME, W_NAV, ID_NAV_PROFILE, NAV_X, 450, NAV_W, 41);
    w->text = tr(S_NAV_PROFILE);
    w->glyph = G_PROFILE;
    label(PG_CHROME, ID_NAV_PROFILE_NAME, NAV_X + 60, 490, NAV_W - 76, 18, tr(S_PROFILE_NONE), F_SMALL, C_NAV_TEXT, LBL_SINGLE);
    custom(PG_CHROME, ID_NAV_ENGINE, NAV_X + 22, 540, NAV_W - 40, 20, draw_nav_engine);
    w = add(PG_CHROME, W_ICONBTN, ID_VOL_MUTE, NAV_X + 10, 580, 36, 32);
    w->glyph = G_SPEAKER_ON;
    w->glyph_on = G_SPEAKER_MUTE;
    w->tip = tr(S_TIP_MUTE);
    w = hslider(PG_CHROME, ID_VOL_SLIDER, NAV_X + 54, 582, 182, 28, 0, 1, 0.01, 0.5, VF_VOL, 34, tr(S_TIP_VOLUME));
    w->wheel_step = 0.02;
    custom(PG_CHROME, ID_VOL_METER, NAV_X + 61, 612, 168, 7, draw_vol_meter);
}

static void build_power_header(int page, int id_power, int id_title, int id_status, const wchar_t *title, const wchar_t *tip)
{
    Widget *w = add(page, W_POWER, id_power, 24, 22, 42, 42);
    w->tip = tip;
    label(page, id_title, 82, 18, 440, 30, title, F_TITLE, C_TEXT_HI, LBL_SINGLE);
    label(page, id_status, 82, 46, 460, 20, L"", F_BODY, C_TEXT_DIM, LBL_SINGLE);
}

static void build_studio(void)
{
    const int P = PG_STUDIO;
    Widget *w;
    build_power_header(P, ID_ST_POWER, ID_ST_TITLE, ID_ST_STATUS, tr(S_STUDIO_TITLE), tr(S_TIP_STUDIO_POWER));
    panel(P, 16, 84, 532, 404);
    const float cx = 34, cw = 172, sx = 214, sw = 266, vw = 50;
    check(P, ID_ST_SUR_ON, cx, 102, cw, 32, tr(S_SURROUND), tr(S_TIP_SURROUND));
    hslider(P, ID_ST_SUR, sx, 102, sw, 32, 0, 1, 0.01, 0.40, VF_PCT01, vw, tr(S_TIP_SURROUND));
    check(P, ID_ST_CRY_ON, cx, 156, cw, 32, tr(S_CLARITY), tr(S_TIP_CLARITY));
    hslider(P, ID_ST_CRY, sx, 156, sw, 32, 0, 1, 0.01, 0.65, VF_PCT01, vw, tr(S_TIP_CLARITY));
    check(P, ID_ST_BASS_ON, cx, 210, cw, 32, tr(S_BASS), tr(S_TIP_BASS));
    hslider(P, ID_ST_BASS, sx, 210, sw, 32, 0, 100, 1, 50, VF_PCT100, vw, tr(S_TIP_BASS));
    w = label(P, ID_ST_XOVER_LBL, cx + 34, 248, cw - 34, 30, tr(S_CROSSOVER), F_BODY, C_TEXT_DIM, LBL_SINGLE);
    w->tip = tr(S_TIP_CROSSOVER);
    w = hslider(P, ID_ST_XOVER, sx, 248, sw, 30, 10, 1000, 1, 80, VF_HZ, vw + 10, tr(S_TIP_CROSSOVER));
    w->logscale = 1;
    panel(P, 30, 292, 504, 1)->color = RGBX(0x2C2C2C);
    check(P, ID_ST_SVM_ON, cx, 306, cw, 32, tr(S_SMARTVOL), tr(S_TIP_SMARTVOL));
    hslider(P, ID_ST_SVM, sx, 306, sw, 32, 0, 1, 0.01, 0.74, VF_PCT01, vw, tr(S_TIP_SMARTVOL));
    w = add(P, W_RADIO, ID_ST_SVM_N, sx, 346, 92, 28);
    w->text = tr(S_SVM_NORMAL); w->group = 1; w->font = F_BODY; w->tip = tr(S_TIP_SVM_NORMAL);
    w = add(P, W_RADIO, ID_ST_SVM_L, sx + 96, 346, 84, 28);
    w->text = tr(S_SVM_LOUD); w->group = 1; w->font = F_BODY; w->tip = tr(S_TIP_SVM_LOUD);
    w = add(P, W_RADIO, ID_ST_SVM_NI, sx + 184, 346, 100, 28);
    w->text = tr(S_SVM_NIGHT); w->group = 1; w->font = F_BODY; w->tip = tr(S_TIP_SVM_NIGHT);
    panel(P, 30, 390, 504, 1)->color = RGBX(0x2C2C2C);
    check(P, ID_ST_DLG_ON, cx, 410, cw, 32, tr(S_DIALOG), tr(S_TIP_DIALOG));
    hslider(P, ID_ST_DLG, sx, 410, sw, 32, 0, 1, 0.01, 0.5, VF_PCT01, vw, tr(S_TIP_DIALOG));
    w = custom(P, ID_ST_TIP, 24, 500, 516, 66, draw_tip);
    w->tip = tr(S_TIP_STUDIO_POWER);
}

static void build_voice(void)
{
    const int P = PG_VOICE;
    Widget *w;
    build_power_header(P, ID_VO_POWER, ID_VO_TITLE, ID_VO_STATUS, tr(S_VOICE_TITLE), tr(S_TIP_VOICE_POWER));
    panel(P, 16, 84, 532, 100);
    label(P, ID_VO_MICLBL, 34, 94, 200, 24, tr(S_MIC_VOLUME), F_LABEL_B, C_TEXT_HI, LBL_SINGLE);
    label(P, ID_VO_DEVICE, 220, 94, 314, 24, L"", F_SMALL, C_TEXT_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    w = add(P, W_ICONBTN, ID_VO_MUTE, 28, 130, 38, 36);
    w->glyph = G_MIC_ON;
    w->glyph_on = G_MIC_MUTE;
    w->tip = tr(S_TIP_MIC_MUTE);
    {
        float bw = gfx_text_width(tr(S_VO_WINSET), F_BODY_B) + 36;
        if (bw < 150) bw = 150;
        if (bw > 230) bw = 230;
        w = hslider(P, ID_VO_VOL, 80, 132, 534 - bw - 12 - 80, 32, 0, 1, 0.01, 0.5, VF_PCT01, 50, tr(S_TIP_MIC_VOL));
        w->wheel_step = 0.02;
        w = button(P, ID_VO_WINSET, 534 - bw, 132, bw, 32, tr(S_VO_WINSET));
        w->tip = tr(S_TIP_VO_WINSET);
    }
    panel(P, 16, 196, 532, 292);
    const float cx = 34, cw = 172, sx = 214, sw = 266;
    check(P, ID_VO_VFX_ON, cx, 214, cw, 34, tr(S_VFX), tr(S_TIP_VFX));
    w = add(P, W_COMBO, ID_VO_VFX, sx, 214, 316, 34);
    w->tip = tr(S_TIP_VFX);
    w->font = F_BODY;
    panel(P, 30, 268, 504, 1)->color = RGBX(0x2C2C2C);
    check(P, ID_VO_SVM_ON, cx, 286, cw, 32, tr(S_SMARTVOL), tr(S_TIP_VSVM));
    hslider(P, ID_VO_SVM, sx, 286, sw, 32, 0, 1, 0.01, 0.74, VF_PCT01, 50, tr(S_TIP_VSVM));
    panel(P, 30, 338, 504, 1)->color = RGBX(0x2C2C2C);
    check(P, ID_VO_NR_ON, cx, 356, cw + 20, 32, tr(S_NR), tr(S_TIP_NR));
    w = label(P, ID_VO_NR_LBL, cx + 34, 398, cw - 34, 30, tr(S_NR_STRENGTH), F_BODY, C_TEXT_DIM, LBL_SINGLE);
    w->tip = tr(S_TIP_NR_STRENGTH);
    hslider(P, ID_VO_NR, sx, 398, sw, 30, 0, 1, 0.01, 0.5, VF_PCT01, 50, tr(S_TIP_NR_STRENGTH));
    w = custom(P, ID_VO_TIP, 24, 500, 516, 66, draw_tip);
    w->tip = tr(S_TIP_VOICE_POWER);
}

static void build_scout(void)
{
    const int P = PG_SCOUT;
    Widget *w;
    label(P, ID_SC_TITLE, 24, 18, 500, 30, tr(S_SCOUT_TITLE), F_TITLE, C_TEXT_HI, LBL_SINGLE);
    label(P, ID_SC_DESC, 24, 52, 516, 40, tr(S_SCOUT_DESC), F_BODY, C_TEXT_DIM, LBL_WRAP);
    panel(P, 16, 102, 532, 66);
    w = add(P, W_SWITCH, ID_SC_SWITCH, 34, 116, 400, 38);
    w->text = tr(S_SCOUT_TITLE);
    w->font = F_LABEL_B;
    panel(P, 16, 180, 532, 232);
    label(P, ID_SC_HKLBL, 34, 192, 300, 24, tr(S_HOTKEYS), F_LABEL_B, C_TEXT_HI, LBL_SINGLE);
    label(P, ID_SC_TASK, 34, 224, 240, 20, tr(S_TASK), F_SMALL, C_TEXT_DIM, LBL_SINGLE);
    label(P, ID_SC_KEY, 290, 224, 240, 20, tr(S_HOTKEY), F_SMALL, C_TEXT_DIM, LBL_SINGLE);
    panel(P, 30, 248, 504, 1)->color = RGBX(0x2C2C2C);
    label(P, ID_SC_ON_LBL, 34, 260, 250, 36, tr(S_HK_ON), F_LABEL, C_TEXT, LBL_SINGLE);
    add(P, W_HOTKEY, ID_SC_HK_ON, 290, 260, 240, 36);
    label(P, ID_SC_HK_ON_ST, 292, 298, 238, 18, L"", F_SMALL, C_TEXT_DIM, LBL_SINGLE);
    label(P, ID_SC_OFF_LBL, 34, 330, 250, 36, tr(S_HK_OFF), F_LABEL, C_TEXT, LBL_SINGLE);
    add(P, W_HOTKEY, ID_SC_HK_OFF, 290, 330, 240, 36);
    label(P, ID_SC_HK_OFF_ST, 292, 368, 238, 18, L"", F_SMALL, C_TEXT_DIM, LBL_SINGLE);
    label(P, ID_SC_HINT, 24, 428, 516, 120, tr(S_HK_HINT), F_BODY, C_TEXT_DIM, LBL_WRAP);
}

static void build_speakers(void)
{
    const int P = PG_SPEAKERS;
    Widget *w;
    label(P, ID_SP_TITLE, 24, 18, 500, 30, tr(S_SPK_TITLE), F_TITLE, C_TEXT_HI, LBL_SINGLE);
    label(P, ID_SP_DESC, 24, 52, 516, 40, tr(S_SPK_DESC), F_BODY, C_TEXT_DIM, LBL_WRAP);
    w = add(P, W_CARD, ID_SP_HP, 16, 100, 258, 230);
    w->text = tr(S_SPK_HEADPHONES);
    w->sub = tr(S_SPK_HEADPHONES_DESC);
    w->glyph = G_HEADPHONES;
    w = add(P, W_CARD, ID_SP_ST, 290, 100, 258, 230);
    w->text = tr(S_SPK_STEREO);
    w->sub = tr(S_SPK_STEREO_DESC);
    w->glyph = G_SPEAKERS;
    label(P, ID_SP_NOTE, 24, 350, 516, 80, tr(S_SPK_NOTE), F_BODY, C_TEXT_DIM, LBL_WRAP);
}

static void build_mixer(void)
{
    const int P = PG_MIXER;
    Widget *w;
    label(P, ID_MX_TITLE, 24, 18, 500, 30, tr(S_MIXER_TITLE), F_TITLE, C_TEXT_HI, LBL_SINGLE);
    for (int side = 0; side < 2; ++side) {
        float ox = side ? 290.0f : 16.0f;
        panel(P, ox, 62, 258, 498);
        label(P, side ? ID_MX_REC_LBL : ID_MX_PB_LBL, ox + 18, 74, 222, 24, tr(side ? S_RECORDING : S_PLAYBACK), F_LABEL_B,
              side ? RGBX(0xE04A42) : C_TEXT_HI, LBL_SINGLE);
        label(P, side ? ID_MX_REC_DEV : ID_MX_PB_DEV, ox + 18, 98, 222, 18, L"", F_SMALL, C_TEXT_DIM, LBL_SINGLE);
        w = add(P, W_VSLIDER, side ? ID_MX_REC_VOL : ID_MX_PB_VOL, ox + 117, 140, 24, 250);
        w->vmin = 0; w->vmax = 1; w->step = 0.01; w->def = 0.5; w->wheel_step = 0.02; w->fmt = VF_VOL;
        w->tip = tr(side ? S_TIP_MIC_VOL : S_TIP_VOLUME);
        label(P, side ? ID_MX_REC_VAL : ID_MX_PB_VAL, ox + 18, 398, 222, 30, L"", F_TITLE, C_TEXT_HI,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        w = add(P, W_ICONBTN, side ? ID_MX_REC_MUTE : ID_MX_PB_MUTE, ox + 108, 434, 42, 38);
        w->glyph = side ? G_MIC_ON : G_SPEAKER_ON;
        w->glyph_on = side ? G_MIC_MUTE : G_SPEAKER_MUTE;
        w->glyph_size = 22;
        w->tip = tr(side ? S_TIP_MIC_MUTE : S_TIP_MUTE);
    }
    label(P, ID_MX_BAL_LBL, 34, 488, 200, 22, tr(S_BALANCE), F_BODY, C_TEXT_DIM, LBL_SINGLE);
    w = hslider(P, ID_MX_BAL, 34, 512, 162, 30, -1, 1, 0.01, 0, VF_BAL, 44, tr(S_TIP_BALANCE));
    w->bipolar = 1;
}

static void build_eq(void)
{
    const int P = PG_EQ;
    Widget *w;
    w = check(P, ID_EQ_ENABLE, 24, 18, 300, 32, tr(S_EQ_TITLE), tr(S_TIP_EQ_ENABLE));
    w->font = F_TITLE;
    w = add(P, W_COMBO, ID_EQ_PRESET, 24, 62, 262, 34);
    w->font = F_BODY;
    button(P, ID_EQ_SAVE, 298, 62, 110, 34, tr(S_SAVE));
    button(P, ID_EQ_DELETE, 418, 62, 110, 34, tr(S_DELETE));
    custom(P, ID_EQ_CURVE, 16, 108, 532, 158, draw_eq_curve);
    panel(P, 16, 278, 532, 284);
    custom(P, ID_EQ_AXIS, 16, 278, 532, 284, draw_eq_axis);
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) {
        w = add(P, W_VSLIDER, ID_EQ_B0 + i, EQ_COL0 + EQ_COLW * (float)i - 12, EQ_SL_Y, 24, EQ_SL_H);
        w->vmin = -24; w->vmax = 24; w->step = 0.5; w->def = 0; w->wheel_step = 0.5;
        w->bipolar = 1; w->fmt = VF_DB_INT; w->value_w = 40;
    }
    w = add(P, W_VSLIDER, ID_EQ_PRE, EQ_PRE_CX - 12, EQ_SL_Y, 24, EQ_SL_H);
    w->vmin = -12; w->vmax = 12; w->step = 0.5; w->def = 0; w->wheel_step = 0.5;
    w->bipolar = 1; w->fmt = VF_DB_INT; w->value_w = 40;
}

static void build_advanced(void)
{
    const int P = PG_ADVANCED;
    Widget *w;
    label(P, ID_AD_TITLE, 24, 18, 500, 30, tr(S_ADV_TITLE), F_TITLE, C_TEXT_HI, LBL_SINGLE);
    panel(P, 16, 58, 532, 112);
    label(P, ID_AD_OUT_LBL, 34, 66, 200, 24, tr(S_OUTPUT), F_LABEL_B, C_TEXT_HI, LBL_SINGLE);
    w = label(P, ID_AD_TRIM_LBL, 34, 94, 170, 32, tr(S_OUTPUT_TRIM), F_LABEL, C_TEXT, LBL_SINGLE);
    w->tip = tr(S_TIP_TRIM);
    w = hslider(P, ID_AD_TRIM, 214, 94, 256, 32, -24, 12, 0.5, 0, VF_DB, 64, tr(S_TIP_TRIM));
    w->bipolar = 1;
    check(P, ID_AD_LIMITER, 34, 132, 400, 30, tr(S_LIMITER), tr(S_TIP_LIMITER));
    panel(P, 16, 180, 262, 176);
    label(P, ID_AD_ENG_LBL, 32, 188, 230, 24, tr(S_ENGINE), F_LABEL_B, C_TEXT_HI, LBL_SINGLE);
    w = custom(P, ID_AD_ENGINE, 32, 216, 232, 136, draw_adv_engine);
    w->tip = tr(S_TIP_ENGINE_RAW);
    panel(P, 286, 180, 262, 176);
    label(P, ID_AD_DEV_LBL, 302, 188, 230, 24, tr(S_DEVICE), F_LABEL_B, C_TEXT_HI, LBL_SINGLE);
    custom(P, ID_AD_DEVICE, 302, 216, 232, 136, draw_adv_device);
    panel(P, 16, 366, 532, 196);
    label(P, ID_AD_Q_LBL, 32, 374, 300, 24, tr(S_QUALITY), F_LABEL_B, C_TEXT_HI, LBL_SINGLE);
    {
        float bw = gfx_text_width(tr(S_OPEN_SOUND_SETTINGS), F_BODY_B) + 36;
        if (bw < 150) bw = 150;
        if (bw > 300) bw = 300;
        button(P, ID_AD_OPEN_SOUND, 532 - bw, 372, bw, 30, tr(S_OPEN_SOUND_SETTINGS));
    }
    custom(P, ID_AD_QUALITY, 32, 410, 500, 148, draw_adv_quality);
}

static void build_profile(void)
{
    const int P = PG_PROFILE;
    Widget *w;
    label(P, ID_PR_TITLE, 24, 18, 500, 30, tr(S_PROFILE_TITLE), F_TITLE, C_TEXT_HI, LBL_SINGLE);
    label(P, ID_PR_DESC, 24, 52, 516, 36, tr(S_PROFILE_DESC), F_BODY, C_TEXT_DIM, LBL_WRAP);
    w = add(P, W_COMBO, ID_PR_COMBO, 24, 90, 516, 36);
    w->font = F_LABEL;
    button(P, ID_PR_SAVE, 24, 138, 120, 34, tr(S_SAVE));
    button(P, ID_PR_DELETE, 156, 138, 120, 34, tr(S_DELETE));
    button(P, ID_PR_IMPORT, 288, 138, 120, 34, tr(S_IMPORT));
    button(P, ID_PR_EXPORT, 420, 138, 120, 34, tr(S_EXPORT));
    panel(P, 16, 188, 532, 374);
    label(P, ID_PR_SUMMARY_LBL, 34, 198, 400, 24, tr(S_CURRENT_SETTINGS), F_LABEL_B, C_TEXT_HI, LBL_SINGLE);
    custom(P, ID_PR_SUMMARY, 34, 232, 496, 320, draw_pr_summary);
}

void pages_build(void)
{
    g.nw = 0;
    build_chrome();
    build_studio();
    build_voice();
    build_scout();
    build_speakers();
    build_mixer();
    build_eq();
    build_advanced();
    build_profile();
}

/* ---------------------------------------------------------------------------------------------- */
/* presets / profiles                                                                               */

static int eq_gains_match(const float *a, const float *b, float pa, float pb)
{
    if (fabsf(pa - pb) > 0.01f) return 0;
    for (int i = 0; i < E1FX_EQ_BANDS; ++i)
        if (fabsf(a[i] - b[i]) > 0.01f) return 0;
    return 1;
}

static void eq_selection_text(wchar_t *out, int cch)
{
    if (g.eq_kind == 1 && g.eq_index < e1_eq_preset_count) lstrcpynW(out, tr_preset_name(e1_eq_presets[g.eq_index].name), cch);
    else if (g.eq_kind == 2 && g.eq_index < g.neqp) lstrcpynW(out, g.eqp[g.eq_index].name, cch);
    else lstrcpynW(out, tr(S_EQ_CUSTOM), cch);
}

static void eq_save_selection(void)
{
    wchar_t t[64];
    if (g.eq_kind == 1) _snwprintf_s(t, 64, _TRUNCATE, L"s:%d", g.eq_index);
    else if (g.eq_kind == 2 && g.eq_index < g.neqp) _snwprintf_s(t, 64, _TRUNCATE, L"u:%s", g.eqp[g.eq_index].name);
    else t[0] = 0;
    settings_set_str(L"EqSelection", t);
}

/* Picks the preset that matches the current curve (standard first). */
static void eq_match_selection(void)
{
    const E1RenderParams *r = &g.p.render;
    for (int i = 0; i < e1_eq_preset_count; ++i)
        if (eq_gains_match(r->eq_gain_db, e1_eq_presets[i].gain_db, r->eq_preamp_db, 0)) {
            g.eq_kind = 1;
            g.eq_index = i;
            return;
        }
    for (int i = 0; i < g.neqp; ++i)
        if (eq_gains_match(r->eq_gain_db, g.eqp[i].gain_db, r->eq_preamp_db, g.eqp[i].preamp_db)) {
            g.eq_kind = 2;
            g.eq_index = i;
            return;
        }
    g.eq_kind = 0;
    g.eq_index = 0;
}

static void profile_save_selection(void)
{
    wchar_t t[96];
    if (g.profile_kind == 1) _snwprintf_s(t, 96, _TRUNCATE, L"f:%d", g.profile_index);
    else if (g.profile_kind == 2) _snwprintf_s(t, 96, _TRUNCATE, L"u:%s", g.profile_name);
    else t[0] = 0;
    settings_set_str(L"ProfileSelection", t);
}

void pages_load_lists(void)
{
    wchar_t dir[MAX_PATH];
    if (!g.profiles) g.profiles = (ProfileEntry *)calloc(PROFILE_MAX_USER, sizeof(ProfileEntry));
    if (!g.eqp) g.eqp = (EqPreset *)calloc(EQP_MAX, sizeof(EqPreset));
    g.nprof = 0;
    g.neqp = 0;
    if (g.profiles && profile_dir(dir, MAX_PATH, 0)) g.nprof = profile_list(dir, g.profiles, PROFILE_MAX_USER);
    if (g.eqp && eqp_default_path(dir, MAX_PATH, 0)) g.neqp = eqp_load(dir, g.eqp, EQP_MAX);
}

static int find_user_profile(const wchar_t *name)
{
    for (int i = 0; i < g.nprof; ++i)
        if (CompareStringOrdinal(g.profiles[i].name, -1, name, -1, TRUE) == CSTR_EQUAL) return i;
    return -1;
}

void pages_init_selection(void)
{
    wchar_t t[128];
    /* equalizer */
    g.eq_kind = 0;
    if (settings_get_str(L"EqSelection", t, 128) && t[0] && t[1] == L':') {
        const E1RenderParams *r = &g.p.render;
        if (t[0] == L's') {
            int i = _wtoi(t + 2);
            if (i >= 0 && i < e1_eq_preset_count && eq_gains_match(r->eq_gain_db, e1_eq_presets[i].gain_db, r->eq_preamp_db, 0)) {
                g.eq_kind = 1;
                g.eq_index = i;
            }
        } else if (t[0] == L'u') {
            int i = eqp_find(g.eqp, g.neqp, t + 2);
            if (i >= 0 && eq_gains_match(r->eq_gain_db, g.eqp[i].gain_db, r->eq_preamp_db, g.eqp[i].preamp_db)) {
                g.eq_kind = 2;
                g.eq_index = i;
            }
        }
    }
    if (g.eq_kind == 0) eq_match_selection();
    /* profile */
    g.profile_kind = 0;
    g.profile_name[0] = 0;
    if (settings_get_str(L"ProfileSelection", t, 128) && t[0] && t[1] == L':') {
        E1Profile *pr = (E1Profile *)malloc(sizeof *pr);
        if (pr) {
            if (t[0] == L'f') {
                int i = _wtoi(t + 2);
                if (i >= 0 && i < profile_factory_count()) {
                    profile_factory(i, &g.p, pr);
                    g.profile_kind = 1;
                    g.profile_index = i;
                    lstrcpynW(g.profile_name, tr_factory(i), PROFILE_NAME_MAX);
                    g.profile_snapshot = pr->params;
                }
            } else if (t[0] == L'u') {
                int i = find_user_profile(t + 2);
                if (i >= 0 && profile_load(g.profiles[i].path, &g.p, pr)) {
                    g.profile_kind = 2;
                    g.profile_index = i;
                    lstrcpynW(g.profile_name, g.profiles[i].name, PROFILE_NAME_MAX);
                    g.profile_snapshot = pr->params;
                }
            }
            free(pr);
        }
    }
}

static int profile_modified(void)
{
    if (g.profile_kind == 0) return 0;
    /* Fields a profile does not store (see profile_to_xml) never make it "modified". */
    E1Params cur = g.p;
    cur.render.speaker_mode = g.profile_snapshot.render.speaker_mode;
    return memcmp(&g.profile_snapshot, &cur, sizeof cur) != 0;
}

static void apply_profile(const E1Profile *pr, int kind, int index, const wchar_t *display)
{
    g.p = pr->params;
    e1fx_params_sanitize(&g.p);
    g.profile_kind = kind;
    g.profile_index = index;
    lstrcpynW(g.profile_name, display, PROFILE_NAME_MAX);
    g.profile_snapshot = g.p;
    eq_match_selection();
    eq_save_selection();
    profile_save_selection();
    app_commit();
}

void pages_restore_defaults(void)
{
    e1fx_params_default(&g.p);
    g.profile_kind = 0;
    g.profile_name[0] = 0;
    eq_match_selection();
    eq_save_selection();
    profile_save_selection();
    app_commit();
}

/* ---------------------------------------------------------------------------------------------- */
/* sync widgets from state                                                                          */

static void set_enabled(int id, int en)
{
    Widget *w = app_widget(id);
    if (w) w->enabled = en;
}
static void set_checked(int id, int c)
{
    Widget *w = app_widget(id);
    if (w) w->checked = c ? 1 : 0;
}
static void set_value(int id, double v)
{
    Widget *w = app_widget(id);
    if (w && g.pressed != (int)(w - g.w)) w->value = v;
}
static void set_dyn(int id, const wchar_t *t)
{
    Widget *w = app_widget(id);
    if (w) lstrcpynW(w->dyn, t ? t : L"", 160);
}

void pages_format_hotkey(DWORD hk, wchar_t *out, int cch)
{
    UINT mods = hk >> 16, vk = hk & 0xFFFF;
    wchar_t key[64] = L"";
    out[0] = 0;
    if (!vk) { lstrcpynW(out, tr(S_HK_NONE), cch); return; }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 0x100;
        break;
    }
    if (!GetKeyNameTextW((LONG)(sc << 16), key, 64) || !key[0]) _snwprintf_s(key, 64, _TRUNCATE, L"0x%02X", vk);
    _snwprintf_s(out, (size_t)cch, _TRUNCATE, L"%s%s%s%s%s", (mods & MOD_CONTROL) ? L"Ctrl + " : L"",
                 (mods & MOD_ALT) ? L"Alt + " : L"", (mods & MOD_SHIFT) ? L"Shift + " : L"", (mods & MOD_WIN) ? L"Win + " : L"", key);
}

void pages_sync(void)
{
    const E1RenderParams *r = &g.p.render;
    const E1CaptureParams *c = &g.p.capture;
    const AudioEp *er = audio_ep(EP_RENDER), *ec = audio_ep(EP_CAPTURE);
    wchar_t t[200];

    /* chrome */
    for (int i = 0; i < 7; ++i) set_checked(ID_NAV0 + i, g.page == i);
    set_checked(ID_NAV_PROFILE, g.page == PG_PROFILE);
    if (g.profile_kind == 0) set_dyn(ID_NAV_PROFILE_NAME, tr(S_PROFILE_NONE));
    else if (profile_modified()) {
        _snwprintf_s(t, 200, _TRUNCATE, tr(S_PROFILE_MODIFIED), g.profile_name);
        set_dyn(ID_NAV_PROFILE_NAME, t);
    } else set_dyn(ID_NAV_PROFILE_NAME, g.profile_name);
    {
        Widget *pn = app_widget(ID_NAV_PROFILE_NAME);
        if (pn) pn->color = g.profile_kind == 2 ? C_ORANGE : C_NAV_TEXT;
    }
    set_enabled(ID_VOL_MUTE, er->present && er->has_volume);
    set_enabled(ID_VOL_SLIDER, er->present && er->has_volume);
    set_checked(ID_VOL_MUTE, er->mute);
    set_value(ID_VOL_SLIDER, er->vol);

    /* studio */
    int m = r->master_on != 0;
    set_checked(ID_ST_POWER, m);
    {
        /* "bit-exact" only when the render chain really bypasses (EQ / Scout Mode run without
           the Studio master) */
        int scout_sfx = 0;
        lstrcpynW(t, tr(panel_studio_status(r, &scout_sfx)), 200);
        if (scout_sfx) StrCatBuffW(t, tr(S_SCOUT_FORCED), 200);
    }
    set_dyn(ID_ST_STATUS, t);
    set_checked(ID_ST_SUR_ON, r->surround_on);   set_value(ID_ST_SUR, r->surround_level);
    set_checked(ID_ST_CRY_ON, r->crystal_on);    set_value(ID_ST_CRY, r->crystal_level);
    set_checked(ID_ST_BASS_ON, r->bass_on);      set_value(ID_ST_BASS, r->bass_level);
    set_value(ID_ST_XOVER, r->bass_xover_hz);
    set_checked(ID_ST_SVM_ON, r->svm_on);        set_value(ID_ST_SVM, r->svm_level);
    set_checked(ID_ST_SVM_N, r->svm_mode == E1FX_SVM_NORMAL);
    set_checked(ID_ST_SVM_L, r->svm_mode == E1FX_SVM_LOUD);
    set_checked(ID_ST_SVM_NI, r->svm_mode == E1FX_SVM_NIGHT);
    set_checked(ID_ST_DLG_ON, r->dialog_on);     set_value(ID_ST_DLG, r->dialog_level);
    set_enabled(ID_ST_SUR_ON, m);  set_enabled(ID_ST_SUR, m && r->surround_on);
    set_enabled(ID_ST_CRY_ON, m);  set_enabled(ID_ST_CRY, m && r->crystal_on);
    set_enabled(ID_ST_BASS_ON, m); set_enabled(ID_ST_BASS, m && r->bass_on);
    set_enabled(ID_ST_XOVER_LBL, m && r->bass_on); set_enabled(ID_ST_XOVER, m && r->bass_on);
    set_enabled(ID_ST_SVM_ON, m);  set_enabled(ID_ST_SVM, m && r->svm_on);
    set_enabled(ID_ST_SVM_N, m && r->svm_on); set_enabled(ID_ST_SVM_L, m && r->svm_on); set_enabled(ID_ST_SVM_NI, m && r->svm_on);
    set_enabled(ID_ST_DLG_ON, m);  set_enabled(ID_ST_DLG, m && r->dialog_on);

    /* voice */
    int vm = c->master_on != 0;
    set_checked(ID_VO_POWER, vm);
    set_dyn(ID_VO_STATUS, vm ? tr(S_VOICE_ON) : tr(S_VOICE_OFF));
    set_dyn(ID_VO_DEVICE, ec->present ? ec->name : tr(S_MIC_NOT_CONNECTED));
    {
        Widget *dv = app_widget(ID_VO_DEVICE);
        if (dv) dv->color = ec->present ? C_TEXT_DIM : C_WARN;
    }
    set_enabled(ID_VO_MUTE, ec->present && ec->has_volume);
    set_enabled(ID_VO_VOL, ec->present && ec->has_volume);
    set_checked(ID_VO_MUTE, ec->mute);
    set_value(ID_VO_VOL, ec->vol);
    set_checked(ID_VO_VFX_ON, c->vfx_on);
    set_dyn(ID_VO_VFX, (int)c->vfx_preset < e1_vfx_preset_count ? tr_preset_name(e1_vfx_presets[c->vfx_preset].name) : L"");
    set_checked(ID_VO_SVM_ON, c->svm_on);  set_value(ID_VO_SVM, c->svm_level);
    set_checked(ID_VO_NR_ON, c->nr_on);    set_value(ID_VO_NR, c->nr_strength);
    set_enabled(ID_VO_VFX_ON, vm); set_enabled(ID_VO_VFX, vm && c->vfx_on);
    set_enabled(ID_VO_SVM_ON, vm); set_enabled(ID_VO_SVM, vm && c->svm_on);
    set_enabled(ID_VO_NR_ON, vm);  set_enabled(ID_VO_NR, vm && c->nr_on); set_enabled(ID_VO_NR_LBL, vm && c->nr_on);

    /* scout */
    set_checked(ID_SC_SWITCH, r->scout_on);
    for (int k = 0; k < 2; ++k) {
        Widget *hw = app_widget(k ? ID_SC_HK_OFF : ID_SC_HK_ON);
        Widget *st = app_widget(k ? ID_SC_HK_OFF_ST : ID_SC_HK_ON_ST);
        if (!hw || !st) continue;
        hw->capturing = g.hk_capture == k;
        if (hw->capturing) lstrcpynW(hw->dyn, tr(S_HK_PRESS), 160);
        else pages_format_hotkey(g.hk[k], hw->dyn, 160);
        hw->color = g.hk[k] ? C_TEXT_HI : C_TEXT_DIM;
        if (!g.hk[k] || hw->capturing) st->dyn[0] = 0;
        else {
            lstrcpynW(st->dyn, g.hk_ok[k] ? tr(S_HK_ACTIVE) : tr(S_HK_FAILED), 160);
            st->color = g.hk_ok[k] ? C_GOOD : C_WARN;
        }
    }

    /* speakers */
    set_checked(ID_SP_HP, r->speaker_mode == E1FX_SPK_HEADPHONES);
    set_checked(ID_SP_ST, r->speaker_mode == E1FX_SPK_STEREO);

    /* mixer */
    set_dyn(ID_MX_PB_DEV, er->present ? er->name : tr(S_NOT_CONNECTED));
    set_dyn(ID_MX_REC_DEV, ec->present ? ec->name : tr(S_MIC_NOT_CONNECTED));
    set_enabled(ID_MX_PB_VOL, er->present && er->has_volume);
    set_enabled(ID_MX_PB_MUTE, er->present && er->has_volume);
    set_enabled(ID_MX_BAL, er->present && er->vol_channels == 2);
    set_enabled(ID_MX_BAL_LBL, er->present && er->vol_channels == 2);
    set_enabled(ID_MX_REC_VOL, ec->present && ec->has_volume);
    set_enabled(ID_MX_REC_MUTE, ec->present && ec->has_volume);
    set_value(ID_MX_PB_VOL, er->vol);
    set_value(ID_MX_REC_VOL, ec->vol);
    set_checked(ID_MX_PB_MUTE, er->mute);
    set_checked(ID_MX_REC_MUTE, ec->mute);
    set_value(ID_MX_BAL, er->balance);
    _snwprintf_s(t, 200, _TRUNCATE, L"%d", (int)floorf(er->vol * 100 + 0.5f));
    set_dyn(ID_MX_PB_VAL, er->present ? t : L"—");
    _snwprintf_s(t, 200, _TRUNCATE, L"%d", (int)floorf(ec->vol * 100 + 0.5f));
    set_dyn(ID_MX_REC_VAL, ec->present ? t : L"—");

    /* equalizer */
    set_checked(ID_EQ_ENABLE, r->eq_on);
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) set_value(ID_EQ_B0 + i, r->eq_gain_db[i]);
    set_value(ID_EQ_PRE, r->eq_preamp_db);
    {
        Widget *cb = app_widget(ID_EQ_PRESET);
        if (cb) {
            eq_selection_text(cb->dyn, 160);
            cb->color = g.eq_kind == 2 ? C_ORANGE : (g.eq_kind == 0 ? C_TEXT : 0);
        }
    }
    set_enabled(ID_EQ_DELETE, g.eq_kind == 2);

    /* advanced */
    set_value(ID_AD_TRIM, r->output_gain_db);
    set_checked(ID_AD_LIMITER, r->limiter_on);

    /* profile */
    {
        Widget *cb = app_widget(ID_PR_COMBO);
        if (cb) {
            if (g.profile_kind == 0) lstrcpynW(cb->dyn, tr(S_PROFILE_SELECT), 160);
            else if (profile_modified()) _snwprintf_s(cb->dyn, 160, _TRUNCATE, tr(S_PROFILE_MODIFIED), g.profile_name);
            else lstrcpynW(cb->dyn, g.profile_name, 160);
            cb->color = g.profile_kind == 2 ? C_ORANGE : (g.profile_kind == 0 ? C_TEXT_DIM : 0);
        }
    }
    set_enabled(ID_PR_DELETE, g.profile_kind == 2);
}

/* ---------------------------------------------------------------------------------------------- */
/* interaction                                                                                      */

static void endpoint_volume(EpKind k, double v)
{
    audio_set_volume(k, (float)v);
    if (audio_ep(k)->mute && v > 0.0) audio_set_mute(k, 0);
    pages_sync();
    app_invalidate();
}

void pages_on_change(Widget *w, int final)
{
    E1RenderParams *r = &g.p.render;
    E1CaptureParams *c = &g.p.capture;
    (void)final;
    switch (w->id) {
    case ID_VOL_SLIDER: case ID_MX_PB_VOL: endpoint_volume(EP_RENDER, w->value); return;
    case ID_VO_VOL: case ID_MX_REC_VOL: endpoint_volume(EP_CAPTURE, w->value); return;
    case ID_MX_BAL:
        audio_set_balance(EP_RENDER, (float)w->value);
        pages_sync();
        app_invalidate();
        return;

    case ID_ST_POWER: r->master_on = w->checked; break;
    case ID_ST_SUR_ON: r->surround_on = w->checked; break;
    case ID_ST_SUR: r->surround_level = (float)w->value; break;
    case ID_ST_CRY_ON: r->crystal_on = w->checked; break;
    case ID_ST_CRY: r->crystal_level = (float)w->value; break;
    case ID_ST_BASS_ON: r->bass_on = w->checked; break;
    case ID_ST_BASS: r->bass_level = (float)w->value; break;
    case ID_ST_XOVER: r->bass_xover_hz = (float)w->value; break;
    case ID_ST_SVM_ON: r->svm_on = w->checked; break;
    case ID_ST_SVM: r->svm_level = (float)w->value; break;
    case ID_ST_SVM_N: r->svm_mode = E1FX_SVM_NORMAL; break;
    case ID_ST_SVM_L: r->svm_mode = E1FX_SVM_LOUD; break;
    case ID_ST_SVM_NI: r->svm_mode = E1FX_SVM_NIGHT; break;
    case ID_ST_DLG_ON: r->dialog_on = w->checked; break;
    case ID_ST_DLG: r->dialog_level = (float)w->value; break;

    case ID_VO_POWER: c->master_on = w->checked; break;
    case ID_VO_VFX_ON: c->vfx_on = w->checked; break;
    case ID_VO_SVM_ON: c->svm_on = w->checked; break;
    case ID_VO_SVM: c->svm_level = (float)w->value; break;
    case ID_VO_NR_ON: c->nr_on = w->checked; break;
    case ID_VO_NR: c->nr_strength = (float)w->value; break;

    case ID_SC_SWITCH: r->scout_on = w->checked; break;

    case ID_EQ_ENABLE: r->eq_on = w->checked; break;
    case ID_EQ_PRE:
        r->eq_preamp_db = (float)w->value;
        g.eq_kind = 0;
        if (final) eq_save_selection();
        break;
    case ID_AD_TRIM: r->output_gain_db = (float)w->value; break;
    case ID_AD_LIMITER: r->limiter_on = w->checked; break;
    default:
        if (w->id >= ID_EQ_B0 && w->id < ID_EQ_B0 + E1FX_EQ_BANDS) {
            r->eq_gain_db[w->id - ID_EQ_B0] = (float)w->value;
            g.eq_kind = 0;
            if (final) eq_save_selection();
            break;
        }
        return;
    }
    app_commit();
}

void pages_on_dblclick(Widget *w)
{
    if (!ui_is_slider(w) || !w->enabled) return;
    if (w->id == ID_VOL_SLIDER || w->id == ID_MX_PB_VOL || w->id == ID_MX_REC_VOL || w->id == ID_VO_VOL) return;
    w->value = w->def;
    pages_on_change(w, 1);
}

static void open_popup_for(Widget *w, PopupItem *items, int n, int sel, int *out)
{
    RECT rc;
    app_screen_rect_of(w, &rc);
    *out = ui_popup(g.hwnd, &rc, w->w, items, n, sel);
}

static const wchar_t *validate_eq_name(const wchar_t *s, void *ctx)
{
    (void)ctx;
    if (!e1_name_valid(s)) return tr(S_NAME_INVALID);
    for (int i = 0; i < e1_eq_preset_count; ++i) {
        wchar_t en[64];
        MultiByteToWideChar(CP_UTF8, 0, e1_eq_presets[i].name, -1, en, 64);
        if (CompareStringOrdinal(en, -1, s, -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(tr_preset_name(e1_eq_presets[i].name), -1, s, -1, TRUE) == CSTR_EQUAL)
            return tr(S_NAME_RESERVED);
    }
    if (CompareStringOrdinal(tr(S_EQ_CUSTOM), -1, s, -1, TRUE) == CSTR_EQUAL) return tr(S_NAME_RESERVED);
    return NULL;
}

static const wchar_t *validate_profile_name(const wchar_t *s, void *ctx)
{
    (void)ctx;
    if (!e1_name_valid(s)) return tr(S_NAME_INVALID);
    for (int i = 0; i < profile_factory_count(); ++i) {
        E1Profile pr;
        profile_factory(i, NULL, &pr);
        if (CompareStringOrdinal(pr.name, -1, s, -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(tr_factory(i), -1, s, -1, TRUE) == CSTR_EQUAL)
            return tr(S_NAME_RESERVED);
    }
    return NULL;
}

static void show_error_file(const wchar_t *path)
{
    wchar_t t[600];
    _snwprintf_s(t, 600, _TRUNCATE, tr(S_SAVE_FAILED), path);
    dlg_message(g.hwnd, tr(S_APP_NAME), t, DLG_OK, 1);
}

static int confirm(const wchar_t *fmt, const wchar_t *name)
{
    wchar_t t[300];
    _snwprintf_s(t, 300, _TRUNCATE, fmt, name);
    return dlg_message(g.hwnd, tr(S_APP_NAME), t, DLG_YESNO, 1);
}

static void eq_click_preset(Widget *w)
{
    int n = e1_eq_preset_count + g.neqp, sel = -1, res = -1;
    PopupItem *items = (PopupItem *)calloc((size_t)(n > 0 ? n : 1), sizeof(PopupItem));
    if (!items) return;
    for (int i = 0; i < e1_eq_preset_count; ++i) lstrcpynW(items[i].text, tr_preset_name(e1_eq_presets[i].name), 96);
    for (int i = 0; i < g.neqp; ++i) {
        lstrcpynW(items[e1_eq_preset_count + i].text, g.eqp[i].name, 96);
        items[e1_eq_preset_count + i].color = C_ORANGE;
    }
    if (g.eq_kind == 1) sel = g.eq_index;
    else if (g.eq_kind == 2) sel = e1_eq_preset_count + g.eq_index;
    open_popup_for(w, items, n, sel, &res);
    free(items);
    if (res < 0) return;
    E1RenderParams *r = &g.p.render;
    if (res < e1_eq_preset_count) {
        memcpy(r->eq_gain_db, e1_eq_presets[res].gain_db, sizeof r->eq_gain_db);
        r->eq_preamp_db = 0;
        g.eq_kind = 1;
        g.eq_index = res;
    } else {
        int i = res - e1_eq_preset_count;
        memcpy(r->eq_gain_db, g.eqp[i].gain_db, sizeof r->eq_gain_db);
        r->eq_preamp_db = g.eqp[i].preamp_db;
        g.eq_kind = 2;
        g.eq_index = i;
    }
    if (!r->eq_on) r->eq_on = 1;
    eq_save_selection();
    app_commit();
}

static void eq_save_preset(void)
{
    wchar_t name[64] = L"", path[MAX_PATH];
    if (g.eq_kind == 2 && g.eq_index < g.neqp) lstrcpynW(name, g.eqp[g.eq_index].name, 64);
    if (!dlg_input(g.hwnd, tr(S_EQ_SAVE_TITLE), tr(S_NAME_PROMPT), name, 64, 32, validate_eq_name, NULL)) return;
    if (eqp_find(g.eqp, g.neqp, name) >= 0 && !confirm(tr(S_CONFIRM_OVERWRITE), name)) return;
    EqPreset p;
    ZeroMemory(&p, sizeof p);
    lstrcpynW(p.name, name, EQP_NAME_MAX);
    p.preamp_db = g.p.render.eq_preamp_db;
    memcpy(p.gain_db, g.p.render.eq_gain_db, sizeof p.gain_db);
    int idx = eqp_upsert(g.eqp, &g.neqp, EQP_MAX, &p);
    if (idx < 0) return;
    if (!eqp_default_path(path, MAX_PATH, 1) || !eqp_save(path, g.eqp, g.neqp)) {
        show_error_file(path);
        pages_load_lists();
        eq_match_selection();
    } else {
        g.eq_kind = 2;
        g.eq_index = idx;
    }
    eq_save_selection();
    pages_sync();
    app_invalidate();
}

static void eq_delete_preset(void)
{
    wchar_t name[EQP_NAME_MAX], path[MAX_PATH];
    if (g.eq_kind != 2 || g.eq_index >= g.neqp) return;
    lstrcpynW(name, g.eqp[g.eq_index].name, EQP_NAME_MAX);
    if (!confirm(tr(S_CONFIRM_DELETE), name)) return;
    eqp_remove(g.eqp, &g.neqp, name);
    if (!eqp_default_path(path, MAX_PATH, 1) || !eqp_save(path, g.eqp, g.neqp)) show_error_file(path);
    g.eq_kind = 0;
    eq_match_selection();
    eq_save_selection();
    pages_sync();
    app_invalidate();
}

static void vfx_click(Widget *w)
{
    int n = e1_vfx_preset_count, res = -1;
    PopupItem *items = (PopupItem *)calloc((size_t)(n > 0 ? n : 1), sizeof(PopupItem));
    if (!items) return;
    for (int i = 0; i < n; ++i) lstrcpynW(items[i].text, tr_preset_name(e1_vfx_presets[i].name), 96);
    open_popup_for(w, items, n, (int)g.p.capture.vfx_preset, &res);
    free(items);
    if (res < 0) return;
    g.p.capture.vfx_preset = (uint32_t)res;
    memcpy(g.p.capture.vfx, e1_vfx_presets[res].p, sizeof g.p.capture.vfx);
    app_commit();
}

static void profile_click(Widget *w)
{
    int nf = profile_factory_count(), n = nf + g.nprof, sel = -1, res = -1;
    PopupItem *items = (PopupItem *)calloc((size_t)n, sizeof(PopupItem));
    if (!items) return;
    for (int i = 0; i < nf; ++i) lstrcpynW(items[i].text, tr_factory(i), 96);
    for (int i = 0; i < g.nprof; ++i) {
        lstrcpynW(items[nf + i].text, g.profiles[i].name, 96);
        items[nf + i].color = C_ORANGE;
    }
    if (g.profile_kind == 1) sel = g.profile_index;
    else if (g.profile_kind == 2) sel = nf + g.profile_index;
    open_popup_for(w, items, n, sel, &res);
    free(items);
    if (res < 0) return;
    E1Profile *pr = (E1Profile *)malloc(sizeof *pr);
    if (!pr) return;
    if (res < nf) {
        profile_factory(res, &g.p, pr);
        apply_profile(pr, 1, res, tr_factory(res));
    } else {
        int i = res - nf;
        if (profile_load(g.profiles[i].path, &g.p, pr)) apply_profile(pr, 2, i, g.profiles[i].name);
        else dlg_message(g.hwnd, tr(S_APP_NAME), tr(S_PROFILE_BAD_FILE), DLG_OK, 1);
    }
    free(pr);
}

static void current_as_profile(E1Profile *pr, const wchar_t *name)
{
    ZeroMemory(pr, sizeof *pr);
    lstrcpynW(pr->name, name, PROFILE_NAME_MAX);
    pr->profile_id = 100;
    pr->eq_preset_id = g.eq_kind == 1 ? g.eq_index + 1 : 0;
    pr->params = g.p;
}

static int write_user_profile(const wchar_t *name, const E1Profile *pr)
{
    wchar_t dir[MAX_PATH], file[MAX_PATH], path[MAX_PATH];
    int existing = find_user_profile(name);
    if (existing >= 0) lstrcpynW(path, g.profiles[existing].path, MAX_PATH);
    else {
        if (!profile_dir(dir, MAX_PATH, 1)) { show_error_file(L"%APPDATA%\\E1Studio\\profiles"); return 0; }
        profile_filename(name, file, MAX_PATH);
        if (lstrlenW(dir) + lstrlenW(file) + 2 >= MAX_PATH) return 0;
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, file);
        /* two names can map to the same file name: never clobber another profile silently */
        for (int k = 2; PathFileExistsW(path) && k < 100; ++k) {
            wchar_t base[MAX_PATH];
            lstrcpynW(base, file, MAX_PATH);
            PathRemoveExtensionW(base);
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s (%d).xml", dir, base, k);
        }
    }
    if (!profile_save(path, pr)) { show_error_file(path); return 0; }
    pages_load_lists();
    return 1;
}

static void profile_save_cmd(void)
{
    wchar_t name[64] = L"";
    if (g.profile_kind == 2) lstrcpynW(name, g.profile_name, 64);
    if (!dlg_input(g.hwnd, tr(S_PROFILE_SAVE_TITLE), tr(S_NAME_PROMPT), name, 64, 32, validate_profile_name, NULL)) return;
    if (find_user_profile(name) >= 0 && !confirm(tr(S_CONFIRM_OVERWRITE), name)) return;
    E1Profile pr;
    current_as_profile(&pr, name);
    if (!write_user_profile(name, &pr)) return;
    int i = find_user_profile(name);
    g.profile_kind = i >= 0 ? 2 : 0;
    g.profile_index = i;
    lstrcpynW(g.profile_name, name, PROFILE_NAME_MAX);
    g.profile_snapshot = g.p;
    profile_save_selection();
    pages_sync();
    app_invalidate();
}

static void profile_delete_cmd(void)
{
    if (g.profile_kind != 2 || g.profile_index < 0 || g.profile_index >= g.nprof) return;
    if (!confirm(tr(S_CONFIRM_DELETE), g.profiles[g.profile_index].name)) return;
    DeleteFileW(g.profiles[g.profile_index].path);
    pages_load_lists();
    g.profile_kind = 0;
    g.profile_name[0] = 0;
    profile_save_selection();
    pages_sync();
    app_invalidate();
}

static void make_filter(wchar_t *out, int cch)
{
    /* "Profile (*.xml)\0*.xml\0\0" */
    int n = lstrlenW(tr(S_XML_FILTER));
    if (n + 10 >= cch) n = cch - 11;
    memcpy(out, tr(S_XML_FILTER), (size_t)n * sizeof(wchar_t));
    out[n] = 0;
    memcpy(out + n + 1, L"*.xml\0", 7 * sizeof(wchar_t));
}

static void profile_import_cmd(void)
{
    wchar_t file[MAX_PATH] = L"", filter[128];
    OPENFILENAMEW ofn;
    make_filter(filter, 128);
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"xml";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    E1Profile *pr = (E1Profile *)malloc(sizeof *pr);
    if (!pr) return;
    if (!profile_load(file, &g.p, pr)) {
        dlg_message(g.hwnd, tr(S_APP_NAME), tr(S_PROFILE_BAD_FILE), DLG_OK, 1);
        free(pr);
        return;
    }
    wchar_t name[PROFILE_NAME_MAX];
    lstrcpynW(name, pr->name, PROFILE_NAME_MAX);
    if (validate_profile_name(name, NULL)) {
        /* fall back to the file name, then to a generic one */
        lstrcpynW(name, PathFindFileNameW(file), 33);
        PathRemoveExtensionW(name);
        if (validate_profile_name(name, NULL)) lstrcpynW(name, tr(S_IMPORTED), PROFILE_NAME_MAX);
    }
    if (find_user_profile(name) >= 0 && !confirm(tr(S_CONFIRM_OVERWRITE), name)) { free(pr); return; }
    lstrcpynW(pr->name, name, PROFILE_NAME_MAX);
    pr->factory = 0;
    pr->profile_id = 100;
    if (write_user_profile(name, pr)) {
        int i = find_user_profile(name);
        apply_profile(pr, i >= 0 ? 2 : 0, i, name);
    }
    free(pr);
}

static void profile_export_cmd(void)
{
    wchar_t file[MAX_PATH], filter[128];
    const wchar_t *name = g.profile_kind ? g.profile_name : tr(S_APP_NAME);
    OPENFILENAMEW ofn;
    profile_filename(name, file, MAX_PATH);
    make_filter(filter, 128);
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"xml";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;
    E1Profile pr;
    current_as_profile(&pr, name);
    if (!profile_save(file, &pr)) show_error_file(file);
}

void pages_on_click(Widget *w)
{
    E1RenderParams *r = &g.p.render;
    if (w->id >= ID_NAV0 && w->id < ID_NAV0 + 7) { app_switch_page(w->id - ID_NAV0); return; }
    switch (w->id) {
    case ID_NAV_PROFILE: app_switch_page(PG_PROFILE); return;
    case ID_VOL_MUTE: case ID_MX_PB_MUTE:
        audio_set_mute(EP_RENDER, !audio_ep(EP_RENDER)->mute);
        pages_sync();
        app_invalidate();
        return;
    case ID_VO_MUTE: case ID_MX_REC_MUTE:
        audio_set_mute(EP_CAPTURE, !audio_ep(EP_CAPTURE)->mute);
        pages_sync();
        app_invalidate();
        return;
    case ID_SP_HP: r->speaker_mode = E1FX_SPK_HEADPHONES; app_commit(); return;
    case ID_SP_ST: r->speaker_mode = E1FX_SPK_STEREO; app_commit(); return;
    case ID_SC_HK_ON: case ID_SC_HK_OFF:
        g.hk_capture = w->id == ID_SC_HK_ON ? 0 : 1;
        UnregisterHotKey(g.hwnd, 1);
        UnregisterHotKey(g.hwnd, 2);
        pages_sync();
        app_invalidate();
        return;
    case ID_EQ_PRESET: eq_click_preset(w); return;
    case ID_EQ_SAVE: eq_save_preset(); return;
    case ID_EQ_DELETE: eq_delete_preset(); return;
    case ID_VO_VFX: vfx_click(w); return;
    case ID_AD_OPEN_SOUND: ShellExecuteW(g.hwnd, L"open", L"ms-settings:sound", NULL, NULL, SW_SHOWNORMAL); return;
    case ID_VO_WINSET: ShellExecuteW(g.hwnd, L"open", L"control.exe", L"mmsys.cpl,,1", NULL, SW_SHOWNORMAL); return;
    case ID_PR_COMBO: profile_click(w); return;
    case ID_PR_SAVE: profile_save_cmd(); return;
    case ID_PR_DELETE: profile_delete_cmd(); return;
    case ID_PR_IMPORT: profile_import_cmd(); return;
    case ID_PR_EXPORT: profile_export_cmd(); return;
    default: return;
    }
}
