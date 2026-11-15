/*
 * ui.h - owner-drawn widget set of e1panel (one HWND, everything painted with gfx).
 * Widgets live in flat arrays; geometry is in logical pixels relative to the widget's container
 * (the window for chrome widgets, the content frame for page widgets).
 */
#ifndef E1P_UI_H
#define E1P_UI_H

#include "gfx.h"

typedef enum WType {
    W_LABEL, W_PANEL, W_SLIDER, W_VSLIDER, W_CHECK, W_POWER, W_RADIO, W_BUTTON, W_COMBO,
    W_ICONBTN, W_HOTKEY, W_CARD, W_SWITCH, W_NAV, W_CUSTOM
} WType;

typedef enum ValueFmt {
    VF_NONE, VF_PCT01,   /* 0..1 shown as 0..100 % */
    VF_PCT100,           /* 0..100 shown as % */
    VF_HZ, VF_DB, VF_DB_INT, VF_BAL, VF_VOL
} ValueFmt;

typedef struct Widget Widget;
typedef void (*WDrawFn)(Gfx *x, Widget *w);

struct Widget {
    int      id;
    int      page;          /* -1 = chrome */
    WType    type;
    float    x, y, w, h;
    int      visible, enabled;
    /* sliders */
    double   value, vmin, vmax, step, def, wheel_step;
    int      logscale, bipolar;
    ValueFmt fmt;
    float    value_w;       /* width of the value text right of a slider (0 = none) */
    /* toggles */
    int      checked;
    int      group;
    /* text */
    const wchar_t *text;
    wchar_t  dyn[160];      /* overrides text when non-empty */
    FontId   font;
    ARGB     color;         /* 0 = default */
    UINT     align;         /* DT_* for labels */
    Glyph    glyph, glyph_on;
    float    glyph_size;
    const wchar_t *tip;
    const wchar_t *sub;     /* card description */
    WDrawFn  draw;
    int      danger;        /* icon button: red hover (close) */
    int      capturing;     /* hot key field waiting for keys */
};

const wchar_t *ui_text(const Widget *w);
void   ui_draw(Gfx *x, const Widget *w, int hover, int pressed, int focus);
int    ui_hit(const Widget *w, float lx, float ly);        /* point relative to the container */
int    ui_focusable(const Widget *w);
int    ui_is_slider(const Widget *w);
double ui_slider_frac(const Widget *w, double v);
double ui_slider_value_at(const Widget *w, float lx, float ly);
double ui_snap(const Widget *w, double v);
void   ui_format_value(const Widget *w, double v, wchar_t *out, int cch);
/* Bounding box (logical, container-relative) including value text, focus ring and thumb overhang. */
void   ui_bounds(const Widget *w, float *x, float *y, float *bw, float *bh);

/* Dark popup list (combo drop-down, title-bar menu). anchor = screen rectangle in physical pixels of
   the control; returns the chosen index or -1. Runs a local modal loop. */
typedef struct PopupItem {
    wchar_t text[96];
    ARGB    color;     /* 0 = default text colour */
    int     checked;
    int     separator;
    int     disabled;
} PopupItem;
int    ui_popup(HWND owner, const RECT *anchor, float min_w, const PopupItem *items, int n, int sel);
void   ui_register_classes(HINSTANCE hi);

#endif /* E1P_UI_H */
