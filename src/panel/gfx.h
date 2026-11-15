/*
 * gfx.h - tiny 2D layer for e1panel: GDI+ flat API (declared here for C, the SDK header is C++ only)
 * for anti-aliased shapes, GDI for ClearType text. All coordinates are LOGICAL pixels (96 DPI);
 * a Gfx carries the DPI scale and an origin offset.
 */
#ifndef E1P_GFX_H
#define E1P_GFX_H

#include <windows.h>

/* ---- GDI+ flat API subset ------------------------------------------------------------------ */
typedef int GpStatus;
typedef struct GpGraphics GpGraphics;
typedef struct GpBrush GpBrush;
typedef struct GpPen GpPen;
typedef struct GpPath GpPath;
typedef struct GpImage GpImage;
typedef struct GpBitmap GpBitmap;
typedef struct { float X, Y; } GpPointF;
typedef DWORD ARGB;
typedef struct {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} GdipStartupInputC;

#define E1GP_API __declspec(dllimport) GpStatus WINAPI
E1GP_API GdiplusStartup(ULONG_PTR *token, const GdipStartupInputC *in, void *out);
__declspec(dllimport) void WINAPI GdiplusShutdown(ULONG_PTR token);
E1GP_API GdipCreateFromHDC(HDC hdc, GpGraphics **g);
E1GP_API GdipDeleteGraphics(GpGraphics *g);
E1GP_API GdipSetSmoothingMode(GpGraphics *g, int mode);
E1GP_API GdipSetPixelOffsetMode(GpGraphics *g, int mode);
E1GP_API GdipSetCompositingQuality(GpGraphics *g, int q);
E1GP_API GdipGraphicsClear(GpGraphics *g, ARGB c);
E1GP_API GdipCreateSolidFill(ARGB c, GpBrush **b);
E1GP_API GdipCreateLineBrush(const GpPointF *p1, const GpPointF *p2, ARGB c1, ARGB c2, int wrap, GpBrush **b);
E1GP_API GdipDeleteBrush(GpBrush *b);
E1GP_API GdipCreatePen1(ARGB c, float width, int unit, GpPen **p);
E1GP_API GdipDeletePen(GpPen *p);
E1GP_API GdipSetPenStartCap(GpPen *p, int cap);
E1GP_API GdipSetPenEndCap(GpPen *p, int cap);
E1GP_API GdipSetPenLineJoin(GpPen *p, int join);
E1GP_API GdipFillRectangle(GpGraphics *g, GpBrush *b, float x, float y, float w, float h);
E1GP_API GdipFillEllipse(GpGraphics *g, GpBrush *b, float x, float y, float w, float h);
E1GP_API GdipDrawEllipse(GpGraphics *g, GpPen *p, float x, float y, float w, float h);
E1GP_API GdipDrawLine(GpGraphics *g, GpPen *p, float x1, float y1, float x2, float y2);
E1GP_API GdipDrawLines(GpGraphics *g, GpPen *p, const GpPointF *pts, INT n);
E1GP_API GdipDrawArc(GpGraphics *g, GpPen *p, float x, float y, float w, float h, float a0, float sweep);
E1GP_API GdipFillPolygon(GpGraphics *g, GpBrush *b, const GpPointF *pts, INT n, int fillmode);
E1GP_API GdipFillPie(GpGraphics *g, GpBrush *b, float x, float y, float w, float h, float a0, float sweep);
E1GP_API GdipCreatePath(int fillmode, GpPath **p);
E1GP_API GdipDeletePath(GpPath *p);
E1GP_API GdipAddPathArc(GpPath *p, float x, float y, float w, float h, float a0, float sweep);
E1GP_API GdipAddPathLine(GpPath *p, float x1, float y1, float x2, float y2);
E1GP_API GdipAddPathLine2(GpPath *p, const GpPointF *pts, INT n);
E1GP_API GdipClosePathFigure(GpPath *p);
E1GP_API GdipFillPath(GpGraphics *g, GpBrush *b, GpPath *p);
E1GP_API GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *p);
E1GP_API GdipGetDC(GpGraphics *g, HDC *hdc);
E1GP_API GdipReleaseDC(GpGraphics *g, HDC hdc);
E1GP_API GdipSetClipRect(GpGraphics *g, float x, float y, float w, float h, int combine);
E1GP_API GdipResetClip(GpGraphics *g);
E1GP_API GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT fmt, BYTE *scan0, GpBitmap **bmp);
E1GP_API GdipGetImageGraphicsContext(GpImage *img, GpGraphics **g);
E1GP_API GdipDisposeImage(GpImage *img);
E1GP_API GdipCreateHICONFromBitmap(GpBitmap *bmp, HICON *icon);

#define E1GP_PIXFMT_32BPP_ARGB  0x0026200A
#define E1GP_PIXFMT_32BPP_PARGB 0x000E200B

/* ---- colours --------------------------------------------------------------------------------- */
#define ARGBC(a, r, g, b) ((ARGB)(((DWORD)(a) << 24) | ((DWORD)(r) << 16) | ((DWORD)(g) << 8) | (DWORD)(b)))
#define RGBX(hex) ((ARGB)(0xFF000000u | (DWORD)(hex)))
ARGB argb_alpha(ARGB c, float a);            /* multiply alpha */
ARGB argb_mix(ARGB a, ARGB b, float t);      /* a..b */

/* Palette (dark theme, see docs/re/control-panel.md 3). */
#define C_WINDOW     RGBX(0x0B0B0B)
#define C_PANEL      RGBX(0x191919)
#define C_CARD       RGBX(0x222222)
#define C_CARD_HI    RGBX(0x282828)
#define C_BORDER     RGBX(0x323232)
#define C_TEXT       RGBX(0xAFAFAF)
#define C_TEXT_HI    RGBX(0xFFFFFF)
#define C_TEXT_DIM   RGBX(0x7A7A7A)
#define C_TEXT_DIS   RGBX(0x464646)
#define C_NAV_TEXT   RGBX(0x979797)
#define C_ACCENT     RGBX(0xCB0202)
#define C_ACCENT_DK  RGBX(0x880709)
#define C_ACCENT_MID RGBX(0x980000)
#define C_POWER_ON   RGBX(0x9D0003)
#define C_POWER_OFF  RGBX(0x464545)
#define C_TRACK      RGBX(0x323232)
#define C_THUMB      RGBX(0x8A8282)
#define C_BUTTON     RGBX(0x262626)
#define C_ORANGE     RGBX(0xF0912A)
#define C_GOOD       RGBX(0x46B455)
#define C_WARN       RGBX(0xE8A317)

/* ---- fonts ----------------------------------------------------------------------------------- */
typedef enum FontId {
    F_SMALL = 0,  /* 11 px */
    F_BODY,       /* 13 px */
    F_BODY_B,     /* 13 px semibold */
    F_LABEL,      /* 15 px */
    F_LABEL_B,    /* 15 px semibold */
    F_NAV,        /* 16 px */
    F_HEAD,       /* 18 px semibold */
    F_TITLE,      /* 22 px semibold */
    F_LOGO,       /* 20 px bold */
    F_TINY,       /* 10 px */
    F_COUNT
} FontId;

typedef struct Gfx {
    HDC         hdc;
    GpGraphics *g;
    float       s;        /* DPI scale (1.0 = 96 dpi) */
    float       ox, oy;   /* logical origin offset */
} Gfx;

void  gfx_startup(void);
void  gfx_shutdown(void);
void  gfx_set_scale(float s);            /* (re)creates the fonts for this scale */
HFONT gfx_font(FontId f);
float gfx_scale(void);

int   gfx_begin(Gfx *x, HDC hdc, float s); /* creates the GDI+ graphics; returns 0 on failure */
void  gfx_end(Gfx *x);
void  gfx_clip(Gfx *x, float lx, float ly, float lw, float lh); /* logical clip (intersect with HDC clip) */
void  gfx_unclip(Gfx *x);

void  gfx_fill_rect(Gfx *x, float lx, float ly, float w, float h, ARGB c);
void  gfx_fill_rrect(Gfx *x, float lx, float ly, float w, float h, float r, ARGB c);
void  gfx_fill_rrect_vgrad(Gfx *x, float lx, float ly, float w, float h, float r, ARGB top, ARGB bottom);
void  gfx_fill_rect_hgrad(Gfx *x, float lx, float ly, float w, float h, ARGB left, ARGB right);
void  gfx_stroke_rrect(Gfx *x, float lx, float ly, float w, float h, float r, float lw, ARGB c);
void  gfx_fill_circle(Gfx *x, float cx, float cy, float r, ARGB c);
void  gfx_stroke_circle(Gfx *x, float cx, float cy, float r, float lw, ARGB c);
void  gfx_arc(Gfx *x, float cx, float cy, float r, float a0, float sweep, float lw, ARGB c);
void  gfx_line(Gfx *x, float x1, float y1, float x2, float y2, float lw, ARGB c);
void  gfx_polyline(Gfx *x, const GpPointF *pts, int n, float lw, ARGB c); /* logical points */
void  gfx_fill_polygon(Gfx *x, const GpPointF *pts, int n, ARGB c);       /* logical points */
void  gfx_fill_area_vgrad(Gfx *x, const GpPointF *pts, int n, float y0, float y1, ARGB top, ARGB bottom);

/* Text (GDI, ClearType). fmt = DT_* flags; DT_NOPREFIX is always added. */
void  gfx_text(Gfx *x, const wchar_t *s, float lx, float ly, float w, float h, FontId f, ARGB c, UINT fmt);
float gfx_text_width(const wchar_t *s, FontId f);  /* logical px */
float gfx_text_height(const wchar_t *s, FontId f, float w);  /* word-wrapped to w, logical px */

/* Vector glyphs used by the UI (all drawn in a box of size sz at lx,ly). */
typedef enum Glyph {
    G_STUDIO, G_VOICE, G_SCOUT, G_HEADPHONES, G_SPEAKERS, G_MIXER, G_EQ, G_ADVANCED, G_PROFILE,
    G_SPEAKER_ON, G_SPEAKER_MUTE, G_MIC_ON, G_MIC_MUTE, G_MENU, G_MINIMIZE, G_CLOSE, G_CHECK, G_WARN,
    G_POWER, G_CHEVRON_DOWN, G_INFO
} Glyph;
void  gfx_glyph(Gfx *x, Glyph gl, float lx, float ly, float sz, ARGB c);

/* App emblem (rounded red tile with level bars). Used for the window/tray icon and the title. */
void  gfx_draw_emblem(Gfx *x, float lx, float ly, float sz);
HICON gfx_make_app_icon(int px);   /* runtime icon, caller DestroyIcon */
/* Render the emblem into a top-down BGRA (straight alpha) buffer of px*px. Returns 0 on failure. */
int   gfx_render_emblem_bgra(int px, unsigned char *out_bgra);

#endif /* E1P_GFX_H */
