/*
 * gfx.c - drawing layer of e1panel (GDI+ flat API for shapes, GDI for text). All artwork is ours,
 * drawn from vector primitives at run time; no bitmaps are shipped.
 */
#include "gfx.h"
#include <math.h>
#include <string.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")

#define GP_SMOOTH_AA      4
#define GP_PIXOFF_HALF    4
#define GP_COMPQ_HQ       2
#define GP_UNIT_PIXEL     2
#define GP_CAP_ROUND      2
#define GP_JOIN_ROUND     2
#define GP_FILL_ALT       0
#define GP_FILL_WINDING   1
#define GP_WRAP_FLIPXY    3
#define GP_COMBINE_INTERSECT 1

static ULONG_PTR g_token;
static HFONT     g_fonts[F_COUNT];
static float     g_scale = 1.0f;
static HDC       g_measure_dc;

static const struct { float px; int weight; } k_font_def[F_COUNT] = {
    {11.0f, FW_NORMAL}, {13.0f, FW_NORMAL}, {13.0f, FW_SEMIBOLD}, {15.0f, FW_NORMAL}, {15.0f, FW_SEMIBOLD},
    {16.0f, FW_NORMAL}, {18.0f, FW_SEMIBOLD}, {22.0f, FW_SEMIBOLD}, {20.0f, FW_BOLD}, {10.0f, FW_NORMAL},
};

ARGB argb_alpha(ARGB c, float a)
{
    float al = (float)(c >> 24) * a;
    if (al < 0) al = 0;
    if (al > 255) al = 255;
    return (c & 0x00FFFFFFu) | ((DWORD)(al + 0.5f) << 24);
}

ARGB argb_mix(ARGB a, ARGB b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    DWORD r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        r |= (DWORD)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return r;
}

void gfx_startup(void)
{
    GdipStartupInputC in = {1, NULL, FALSE, FALSE};
    GdiplusStartup(&g_token, &in, NULL);
    g_measure_dc = CreateCompatibleDC(NULL);
    gfx_set_scale(1.0f);
}

void gfx_shutdown(void)
{
    for (int i = 0; i < F_COUNT; ++i)
        if (g_fonts[i]) { DeleteObject(g_fonts[i]); g_fonts[i] = NULL; }
    if (g_measure_dc) { DeleteDC(g_measure_dc); g_measure_dc = NULL; }
    if (g_token) { GdiplusShutdown(g_token); g_token = 0; }
}

void gfx_set_scale(float s)
{
    g_scale = s;
    for (int i = 0; i < F_COUNT; ++i) {
        if (g_fonts[i]) DeleteObject(g_fonts[i]);
        int h = (int)floorf(k_font_def[i].px * s + 0.5f);
        g_fonts[i] = CreateFontW(-h, 0, 0, 0, k_font_def[i].weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }
}

HFONT gfx_font(FontId f) { return g_fonts[f]; }
float gfx_scale(void) { return g_scale; }

int gfx_begin(Gfx *x, HDC hdc, float s)
{
    x->hdc = hdc;
    x->s = s;
    x->ox = x->oy = 0;
    x->g = NULL;
    if (GdipCreateFromHDC(hdc, &x->g) != 0 || !x->g) return 0;
    GdipSetSmoothingMode(x->g, GP_SMOOTH_AA);
    GdipSetPixelOffsetMode(x->g, GP_PIXOFF_HALF);
    GdipSetCompositingQuality(x->g, GP_COMPQ_HQ);
    return 1;
}

void gfx_end(Gfx *x)
{
    if (x->g) GdipDeleteGraphics(x->g);
    x->g = NULL;
}

#define DX(v) (((v) + x->ox) * x->s)
#define DY(v) (((v) + x->oy) * x->s)
#define DS(v) ((v) * x->s)

void gfx_clip(Gfx *x, float lx, float ly, float lw, float lh)
{
    if (x->g) GdipSetClipRect(x->g, DX(lx), DY(ly), DS(lw), DS(lh), GP_COMBINE_INTERSECT);
}

void gfx_unclip(Gfx *x)
{
    if (x->g) GdipResetClip(x->g);
}

static GpBrush *solid(ARGB c)
{
    GpBrush *b = NULL;
    GdipCreateSolidFill(c, &b);
    return b;
}

static GpPen *pen(Gfx *x, ARGB c, float lw)
{
    GpPen *p = NULL;
    GdipCreatePen1(c, DS(lw), GP_UNIT_PIXEL, &p);
    if (p) {
        GdipSetPenStartCap(p, GP_CAP_ROUND);
        GdipSetPenEndCap(p, GP_CAP_ROUND);
        GdipSetPenLineJoin(p, GP_JOIN_ROUND);
    }
    return p;
}

static GpPath *rrect_path(float X, float Y, float W, float H, float R)
{
    GpPath *p = NULL;
    if (GdipCreatePath(GP_FILL_ALT, &p) != 0) return NULL;
    if (R * 2 > W) R = W / 2;
    if (R * 2 > H) R = H / 2;
    if (R < 0.5f) {
        GpPointF pts[4] = {{X, Y}, {X + W, Y}, {X + W, Y + H}, {X, Y + H}};
        GdipAddPathLine2(p, pts, 4);
    } else {
        float d = 2 * R;
        GdipAddPathArc(p, X, Y, d, d, 180, 90);
        GdipAddPathArc(p, X + W - d, Y, d, d, 270, 90);
        GdipAddPathArc(p, X + W - d, Y + H - d, d, d, 0, 90);
        GdipAddPathArc(p, X, Y + H - d, d, d, 90, 90);
    }
    GdipClosePathFigure(p);
    return p;
}

void gfx_fill_rect(Gfx *x, float lx, float ly, float w, float h, ARGB c)
{
    GpBrush *b = solid(c);
    if (!b) return;
    GdipFillRectangle(x->g, b, DX(lx), DY(ly), DS(w), DS(h));
    GdipDeleteBrush(b);
}

void gfx_fill_rrect(Gfx *x, float lx, float ly, float w, float h, float r, ARGB c)
{
    GpBrush *b = solid(c);
    GpPath *p = rrect_path(DX(lx), DY(ly), DS(w), DS(h), DS(r));
    if (b && p) GdipFillPath(x->g, b, p);
    if (p) GdipDeletePath(p);
    if (b) GdipDeleteBrush(b);
}

void gfx_fill_rrect_vgrad(Gfx *x, float lx, float ly, float w, float h, float r, ARGB top, ARGB bottom)
{
    GpPointF p1 = {0, DY(ly) - 1}, p2 = {0, DY(ly + h) + 1};
    GpBrush *b = NULL;
    GdipCreateLineBrush(&p1, &p2, top, bottom, GP_WRAP_FLIPXY, &b);
    GpPath *p = rrect_path(DX(lx), DY(ly), DS(w), DS(h), DS(r));
    if (b && p) GdipFillPath(x->g, b, p);
    if (p) GdipDeletePath(p);
    if (b) GdipDeleteBrush(b);
}

void gfx_fill_rect_hgrad(Gfx *x, float lx, float ly, float w, float h, ARGB left, ARGB right)
{
    GpPointF p1 = {DX(lx) - 1, 0}, p2 = {DX(lx + w) + 1, 0};
    GpBrush *b = NULL;
    GdipCreateLineBrush(&p1, &p2, left, right, GP_WRAP_FLIPXY, &b);
    if (!b) return;
    GdipFillRectangle(x->g, b, DX(lx), DY(ly), DS(w), DS(h));
    GdipDeleteBrush(b);
}

void gfx_stroke_rrect(Gfx *x, float lx, float ly, float w, float h, float r, float lw, ARGB c)
{
    GpPen *pn = pen(x, c, lw);
    float in = lw / 2;
    GpPath *p = rrect_path(DX(lx + in), DY(ly + in), DS(w - lw), DS(h - lw), DS(r));
    if (pn && p) GdipDrawPath(x->g, pn, p);
    if (p) GdipDeletePath(p);
    if (pn) GdipDeletePen(pn);
}

void gfx_fill_circle(Gfx *x, float cx, float cy, float r, ARGB c)
{
    GpBrush *b = solid(c);
    if (!b) return;
    GdipFillEllipse(x->g, b, DX(cx - r), DY(cy - r), DS(2 * r), DS(2 * r));
    GdipDeleteBrush(b);
}

void gfx_stroke_circle(Gfx *x, float cx, float cy, float r, float lw, ARGB c)
{
    GpPen *p = pen(x, c, lw);
    if (!p) return;
    GdipDrawEllipse(x->g, p, DX(cx - r), DY(cy - r), DS(2 * r), DS(2 * r));
    GdipDeletePen(p);
}

void gfx_arc(Gfx *x, float cx, float cy, float r, float a0, float sweep, float lw, ARGB c)
{
    GpPen *p = pen(x, c, lw);
    if (!p) return;
    GdipDrawArc(x->g, p, DX(cx - r), DY(cy - r), DS(2 * r), DS(2 * r), a0, sweep);
    GdipDeletePen(p);
}

void gfx_line(Gfx *x, float x1, float y1, float x2, float y2, float lw, ARGB c)
{
    GpPen *p = pen(x, c, lw);
    if (!p) return;
    GdipDrawLine(x->g, p, DX(x1), DY(y1), DX(x2), DY(y2));
    GdipDeletePen(p);
}

#define MAX_POLY 1100
void gfx_polyline(Gfx *x, const GpPointF *pts, int n, float lw, ARGB c)
{
    GpPointF d[MAX_POLY];
    if (n < 2) return;
    if (n > MAX_POLY) n = MAX_POLY;
    for (int i = 0; i < n; ++i) { d[i].X = DX(pts[i].X); d[i].Y = DY(pts[i].Y); }
    GpPen *p = pen(x, c, lw);
    if (!p) return;
    GdipDrawLines(x->g, p, d, n);
    GdipDeletePen(p);
}

void gfx_fill_polygon(Gfx *x, const GpPointF *pts, int n, ARGB c)
{
    GpPointF d[MAX_POLY];
    if (n < 3) return;
    if (n > MAX_POLY) n = MAX_POLY;
    for (int i = 0; i < n; ++i) { d[i].X = DX(pts[i].X); d[i].Y = DY(pts[i].Y); }
    GpBrush *b = solid(c);
    if (!b) return;
    GdipFillPolygon(x->g, b, d, n, GP_FILL_WINDING);
    GdipDeleteBrush(b);
}

void gfx_fill_area_vgrad(Gfx *x, const GpPointF *pts, int n, float y0, float y1, ARGB top, ARGB bottom)
{
    GpPointF d[MAX_POLY];
    if (n < 3) return;
    if (n > MAX_POLY) n = MAX_POLY;
    for (int i = 0; i < n; ++i) { d[i].X = DX(pts[i].X); d[i].Y = DY(pts[i].Y); }
    GpPointF p1 = {0, DY(y0) - 1}, p2 = {0, DY(y1) + 1};
    GpBrush *b = NULL;
    GdipCreateLineBrush(&p1, &p2, top, bottom, GP_WRAP_FLIPXY, &b);
    if (!b) return;
    GdipFillPolygon(x->g, b, d, n, GP_FILL_WINDING);
    GdipDeleteBrush(b);
}

static COLORREF to_cref(ARGB c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

void gfx_text(Gfx *x, const wchar_t *s, float lx, float ly, float w, float h, FontId f, ARGB c, UINT fmt)
{
    if (!s || !*s) return;
    HDC dc = NULL;
    int via_gp = 0;
    if (x->g && GdipGetDC(x->g, &dc) == 0 && dc) via_gp = 1;
    else dc = x->hdc;
    if (!dc) return;
    RECT rc;
    rc.left = (LONG)floorf(DX(lx) + 0.5f);
    rc.top = (LONG)floorf(DY(ly) + 0.5f);
    rc.right = (LONG)floorf(DX(lx + w) + 0.5f);
    rc.bottom = (LONG)floorf(DY(ly + h) + 0.5f);
    HGDIOBJ of = SelectObject(dc, g_fonts[f]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, to_cref(c));
    DrawTextW(dc, s, -1, &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
    if (via_gp) GdipReleaseDC(x->g, dc);
}

float gfx_text_width(const wchar_t *s, FontId f)
{
    SIZE sz = {0, 0};
    if (!s || !*s || !g_measure_dc) return 0;
    HGDIOBJ of = SelectObject(g_measure_dc, g_fonts[f]);
    GetTextExtentPoint32W(g_measure_dc, s, lstrlenW(s), &sz);
    SelectObject(g_measure_dc, of);
    return (float)sz.cx / g_scale;
}

float gfx_text_height(const wchar_t *s, FontId f, float w)
{
    RECT rc = {0, 0, 0, 0};
    if (!s || !*s || !g_measure_dc || w <= 0) return 0;
    rc.right = (LONG)floorf(w * g_scale + 0.5f);
    HGDIOBJ of = SelectObject(g_measure_dc, g_fonts[f]);
    DrawTextW(g_measure_dc, s, -1, &rc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(g_measure_dc, of);
    return (float)(rc.bottom - rc.top) / g_scale;
}

/* ---------------------------------------------------------------------------------------------- */
/* Glyphs: all coordinates relative to a sz x sz box at (lx, ly).                                  */

void gfx_glyph(Gfx *x, Glyph gl, float lx, float ly, float sz, ARGB c)
{
    const float u = sz / 24.0f;  /* design grid: 24 units */
#define P(a) ((a) * u)
#define X0(a) (lx + P(a))
#define Y0(a) (ly + P(a))
    float lw = P(1.8f);
    switch (gl) {
    case G_STUDIO:  /* source dot with waves both sides */
        gfx_fill_circle(x, X0(12), Y0(12), P(2.6f), c);
        gfx_arc(x, X0(12), Y0(12), P(6), -50, 100, lw, c);
        gfx_arc(x, X0(12), Y0(12), P(6), 130, 100, lw, c);
        gfx_arc(x, X0(12), Y0(12), P(10), -45, 90, lw, argb_alpha(c, 0.7f));
        gfx_arc(x, X0(12), Y0(12), P(10), 135, 90, lw, argb_alpha(c, 0.7f));
        break;
    case G_VOICE:
    case G_MIC_ON:
    case G_MIC_MUTE:
        gfx_stroke_rrect(x, X0(8.5f), Y0(2.5f), P(7), P(12), P(3.5f), lw, c);
        gfx_arc(x, X0(12), Y0(10), P(6.5f), 10, 160, lw, c);
        gfx_line(x, X0(12), Y0(16.5f), X0(12), Y0(20.5f), lw, c);
        gfx_line(x, X0(8.5f), Y0(21), X0(15.5f), Y0(21), lw, c);
        if (gl == G_MIC_MUTE) gfx_line(x, X0(4), Y0(3), X0(20), Y0(21), lw, c);
        break;
    case G_SCOUT:
        gfx_stroke_circle(x, X0(12), Y0(12), P(7), lw, c);
        gfx_line(x, X0(12), Y0(1.5f), X0(12), Y0(7), lw, c);
        gfx_line(x, X0(12), Y0(17), X0(12), Y0(22.5f), lw, c);
        gfx_line(x, X0(1.5f), Y0(12), X0(7), Y0(12), lw, c);
        gfx_line(x, X0(17), Y0(12), X0(22.5f), Y0(12), lw, c);
        gfx_fill_circle(x, X0(12), Y0(12), P(1.6f), c);
        break;
    case G_HEADPHONES:
        gfx_arc(x, X0(12), Y0(13), P(8.5f), 180, 180, lw, c);
        gfx_fill_rrect(x, X0(2.5f), Y0(13), P(5), P(8), P(1.8f), c);
        gfx_fill_rrect(x, X0(16.5f), Y0(13), P(5), P(8), P(1.8f), c);
        break;
    case G_SPEAKERS:
        gfx_stroke_rrect(x, X0(5.5f), Y0(2), P(13), P(20), P(2.5f), lw, c);
        gfx_stroke_circle(x, X0(12), Y0(7), P(2), lw, c);
        gfx_stroke_circle(x, X0(12), Y0(15), P(4), lw, c);
        break;
    case G_MIXER:
        for (int i = 0; i < 3; ++i) {
            float cx = 5 + 7 * (float)i;
            float k = (i == 0) ? 14.0f : (i == 1 ? 7.0f : 11.0f);
            gfx_line(x, X0(cx), Y0(3), X0(cx), Y0(21), lw * 0.9f, argb_alpha(c, 0.75f));
            gfx_fill_rrect(x, X0(cx - 3), Y0(k - 1.8f), P(6), P(3.6f), P(1.2f), c);
        }
        break;
    case G_EQ: {
        const float hgt[5] = {9, 15, 11, 18, 7};
        for (int i = 0; i < 5; ++i) {
            float cx = 3 + 4.5f * (float)i;
            gfx_fill_rrect(x, X0(cx), Y0(21 - hgt[i]), P(3), P(hgt[i]), P(1.2f), c);
        }
        break;
    }
    case G_ADVANCED: { /* gear */
        for (int i = 0; i < 8; ++i) {
            double a = i * 3.14159265358979 / 4;
            float ca = (float)cos(a), sa = (float)sin(a);
            gfx_line(x, X0(12 + 6.5f * ca), Y0(12 + 6.5f * sa), X0(12 + 9.5f * ca), Y0(12 + 9.5f * sa), P(3), c);
        }
        gfx_stroke_circle(x, X0(12), Y0(12), P(6.5f), P(2.4f), c);
        gfx_stroke_circle(x, X0(12), Y0(12), P(2.4f), lw, c);
        break;
    }
    case G_PROFILE:
        gfx_stroke_circle(x, X0(12), Y0(8), P(4.2f), lw, c);
        gfx_arc(x, X0(12), Y0(23), P(8.5f), 200, 140, lw, c);
        break;
    case G_SPEAKER_ON:
    case G_SPEAKER_MUTE: {
        GpPointF cone[6] = {{X0(3), Y0(9)}, {X0(7), Y0(9)}, {X0(12), Y0(4.5f)}, {X0(12), Y0(19.5f)}, {X0(7), Y0(15)}, {X0(3), Y0(15)}};
        gfx_fill_polygon(x, cone, 6, c);
        if (gl == G_SPEAKER_ON) {
            gfx_arc(x, X0(12), Y0(12), P(4.5f), -45, 90, lw, c);
            gfx_arc(x, X0(12), Y0(12), P(8.5f), -45, 90, lw, c);
        } else {
            gfx_line(x, X0(15.5f), Y0(9), X0(21.5f), Y0(15), lw, c);
            gfx_line(x, X0(21.5f), Y0(9), X0(15.5f), Y0(15), lw, c);
        }
        break;
    }
    case G_MENU:
        gfx_line(x, X0(5), Y0(7), X0(19), Y0(7), lw, c);
        gfx_line(x, X0(5), Y0(12), X0(19), Y0(12), lw, c);
        gfx_line(x, X0(5), Y0(17), X0(19), Y0(17), lw, c);
        break;
    case G_MINIMIZE:
        gfx_line(x, X0(6), Y0(12.5f), X0(18), Y0(12.5f), lw, c);
        break;
    case G_CLOSE:
        gfx_line(x, X0(6.5f), Y0(6.5f), X0(17.5f), Y0(17.5f), lw, c);
        gfx_line(x, X0(17.5f), Y0(6.5f), X0(6.5f), Y0(17.5f), lw, c);
        break;
    case G_CHECK: {
        GpPointF pts[3] = {{X0(5), Y0(12.5f)}, {X0(10), Y0(17.5f)}, {X0(19.5f), Y0(6.5f)}};
        gfx_polyline(x, pts, 3, P(2.6f), c);
        break;
    }
    case G_WARN: {
        GpPointF tri[3] = {{X0(12), Y0(2.5f)}, {X0(22.5f), Y0(21)}, {X0(1.5f), Y0(21)}};
        gfx_fill_polygon(x, tri, 3, c);
        gfx_line(x, X0(12), Y0(9), X0(12), Y0(14.5f), P(2.4f), RGBX(0x1A1A1A));
        gfx_fill_circle(x, X0(12), Y0(18), P(1.4f), RGBX(0x1A1A1A));
        break;
    }
    case G_POWER:
        gfx_arc(x, X0(12), Y0(13), P(8), -60, 300, P(2.4f), c);
        gfx_line(x, X0(12), Y0(2.5f), X0(12), Y0(11.5f), P(2.4f), c);
        break;
    case G_CHEVRON_DOWN: {
        GpPointF pts[3] = {{X0(6), Y0(9)}, {X0(12), Y0(15)}, {X0(18), Y0(9)}};
        gfx_polyline(x, pts, 3, lw, c);
        break;
    }
    case G_INFO:
        gfx_stroke_circle(x, X0(12), Y0(12), P(9.5f), lw, c);
        gfx_line(x, X0(12), Y0(10.5f), X0(12), Y0(17), P(2.2f), c);
        gfx_fill_circle(x, X0(12), Y0(7), P(1.4f), c);
        break;
    }
#undef P
#undef X0
#undef Y0
}

void gfx_draw_emblem(Gfx *x, float lx, float ly, float sz)
{
    float r = sz * 0.24f;
    gfx_fill_rrect_vgrad(x, lx, ly, sz, sz, r, RGBX(0xD8170F), RGBX(0x7C0508));
    /* soft top highlight */
    gfx_fill_rrect(x, lx + sz * 0.06f, ly + sz * 0.05f, sz * 0.88f, sz * 0.36f, r * 0.8f, ARGBC(34, 255, 255, 255));
    /* four level bars */
    const float hgt[4] = {0.36f, 0.62f, 0.46f, 0.28f};
    float bw = sz * 0.12f, gap = sz * 0.07f;
    float total = 4 * bw + 3 * gap;
    float x0 = lx + (sz - total) / 2, base = ly + sz * 0.78f;
    for (int i = 0; i < 4; ++i) {
        float h = sz * hgt[i];
        gfx_fill_rrect(x, x0 + (float)i * (bw + gap), base - h, bw, h, bw * 0.45f, RGBX(0xFFFFFF));
    }
}

int gfx_render_emblem_bgra(int px, unsigned char *out)
{
    GpBitmap *bmp = NULL;
    Gfx x;
    memset(out, 0, (size_t)px * (size_t)px * 4);
    if (GdipCreateBitmapFromScan0(px, px, px * 4, E1GP_PIXFMT_32BPP_ARGB, out, &bmp) != 0 || !bmp) return 0;
    x.hdc = NULL;
    x.s = 1.0f;
    x.ox = x.oy = 0;
    x.g = NULL;
    if (GdipGetImageGraphicsContext((GpImage *)bmp, &x.g) != 0 || !x.g) { GdipDisposeImage((GpImage *)bmp); return 0; }
    GdipSetSmoothingMode(x.g, GP_SMOOTH_AA);
    GdipSetPixelOffsetMode(x.g, GP_PIXOFF_HALF);
    GdipGraphicsClear(x.g, 0);
    float inset = px <= 16 ? 0.0f : (float)px * 0.03f;
    gfx_draw_emblem(&x, inset, inset, (float)px - 2 * inset);
    GdipDeleteGraphics(x.g);
    GdipDisposeImage((GpImage *)bmp); /* the pixels stay in `out` (caller-owned scan0) */
    return 1;
}

HICON gfx_make_app_icon(int px)
{
    BITMAPV5HEADER bi;
    void *bits = NULL;
    unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)px * px * 4);
    if (!buf) return NULL;
    if (!gfx_render_emblem_bgra(px, buf)) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    ZeroMemory(&bi, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = px;
    bi.bV5Height = -px;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    HBITMAP mask = CreateBitmap(px, px, 1, 1, NULL);
    HICON ic = NULL;
    if (color && mask && bits) {
        memcpy(bits, buf, (size_t)px * px * 4);
        ICONINFO ii = {TRUE, 0, 0, mask, color};
        ic = CreateIconIndirect(&ii);
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    HeapFree(GetProcessHeap(), 0, buf);
    return ic;
}
