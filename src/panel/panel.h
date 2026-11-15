/*
 * panel.h - application state of e1panel.exe shared by main.c and pages.c.
 */
#ifndef E1P_PANEL_H
#define E1P_PANEL_H

#include <windows.h>
#include <shellapi.h>
#include "../../include/e1fx_params.h"
#include "../../include/e1fx_ipc.h"
#include "../dsp/e1dsp_modules.h"
#include "gfx.h"
#include "ui.h"
#include "strings.h"
#include "audio.h"
#include "sync.h"
#include "profile.h"
#include "eqpreset.h"

#define E1P_VERSION L"1.0.0"

/* Window geometry, logical pixels (docs/re/control-panel.md 3). */
#define WIN_W      882
#define WIN_H      637
#define TITLE_Y    6
#define TITLE_H    36
#define NAV_X      6
#define NAV_W      292
#define BODY_Y     42
#define BODY_H     585
#define CONTENT_X  303
#define CONTENT_Y  47
#define CONTENT_W  564
#define CONTENT_H  575

/* Private messages. */
#define WM_APP_TRAY      (WM_APP + 1)
#define WM_APP_DEVCHANGE (WM_APP + 2)
#define WM_APP_VOLCHANGE (WM_APP + 3)
#define WM_APP_SHOWME    (WM_APP + 4)
#define WM_APP_QUIT      (WM_APP + 5)

/* Timers. */
enum { T_SAVE = 1, T_METER, T_STATUS, T_RESCAN, T_ANIM, T_SPATIAL };

typedef enum Page {
    PG_STUDIO, PG_VOICE, PG_SCOUT, PG_SPEAKERS, PG_MIXER, PG_EQ, PG_ADVANCED, PG_PROFILE, PG_COUNT
} Page;
#define PG_CHROME (-1)

#define MAX_WIDGETS 200

typedef struct App {
    HINSTANCE hinst;
    HWND      hwnd;
    UINT      dpi;
    float     s;
    int       start_tray;

    /* widgets */
    Widget    w[MAX_WIDGETS];
    int       nw;
    int       hover, pressed, focus;
    int       mouse_tracking;
    int       kbd_cues;          /* show focus rings after keyboard use */
    int       start_page;
    double    press_value;

    /* back buffer */
    HDC       memdc;
    HBITMAP   membmp, memold;
    void     *membits;
    int       bw, bh;

    /* pages and transition */
    int       page;
    int       anim;
    DWORD     anim_t0;
    HDC       adc[2];
    HBITMAP   abmp[2], aold[2];

    /* parameters */
    E1Params  p;
    ParamSync sync;
    int       save_pending;

    /* engine status */
    EngineState eng[2];
    E1ApoStatus st[2];
    float     meter_db[2];
    DWORD     meter_t;

    /* audio */
    SpatialInfo spatial;

    /* profiles */
    ProfileEntry *profiles;
    int       nprof;
    wchar_t   profile_name[PROFILE_NAME_MAX];
    int       profile_kind;      /* 0 none, 1 factory, 2 user */
    int       profile_index;     /* factory index or user index */
    E1Params  profile_snapshot;

    /* equalizer presets */
    EqPreset *eqp;
    int       neqp;
    int       eq_kind;           /* 0 custom (no preset), 1 standard, 2 user */
    int       eq_index;

    /* hot keys */
    DWORD     hk[2];             /* (modifiers << 16) | vk, 0 = none */
    int       hk_ok[2];
    int       hk_capture;        /* -1 or 0/1 */

    /* tray */
    NOTIFYICONDATAW nid;
    int       tray_added;
    int       tray_hint_shown;
    HICON     icon_big, icon_small;
    UINT      msg_taskbar_created;
} App;

extern App g;

/* main.c services used by pages.c */
void    app_commit(void);                 /* params changed: publish now, save debounced, refresh UI */
void    app_invalidate(void);
void    app_invalidate_widget(int id);
Widget *app_widget(int id);
void    app_switch_page(int page);
void    app_register_hotkeys(void);
void    app_screen_rect_of(const Widget *w, RECT *out);

/* pages.c */
void    pages_build(void);                /* creates every widget */
void    pages_sync(void);                 /* params / audio state -> widgets */
void    pages_on_change(Widget *w, int final);   /* slider moved / toggle clicked */
void    pages_on_click(Widget *w);        /* buttons, combos, cards */
void    pages_on_dblclick(Widget *w);     /* reset to default */
void    pages_draw_chrome(Gfx *x);        /* backgrounds behind chrome widgets */
void    pages_draw_page_bg(Gfx *x, int page);
void    pages_load_lists(void);           /* profiles + custom EQ presets from disk */
void    pages_init_selection(void);       /* EQ preset / profile selection from saved settings */
void    pages_restore_defaults(void);
void    pages_format_hotkey(DWORD hk, wchar_t *out, int cch);
const wchar_t *pages_title_status(void);

#endif /* E1P_PANEL_H */
