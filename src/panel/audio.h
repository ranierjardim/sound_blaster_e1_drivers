/*
 * audio.h - E1 endpoint discovery (MMDevice API), endpoint volume/mute/balance with change
 * notifications, device format and Windows Spatial Sound detection. All functions run on the UI
 * thread (STA); COM callbacks only post messages to the window given to audio_init.
 */
#ifndef E1P_AUDIO_H
#define E1P_AUDIO_H

#include <windows.h>

#define E1_HWID_MATCH L"VID_041E&PID_323B"

typedef enum EpKind { EP_RENDER = 0, EP_CAPTURE = 1 } EpKind;

typedef struct AudioEp {
    int     present;
    wchar_t id[128];
    wchar_t name[128];        /* endpoint friendly name, e.g. "Speakers (...)" */
    wchar_t device[96];       /* device (interface) friendly name, read from the system */
    wchar_t desc[64];         /* endpoint description alone, e.g. "Speakers" */
    int     has_volume;
    float   vol;              /* master scalar 0..1 */
    int     mute;
    float   balance;          /* -1 (left) .. +1 (right); render only */
    UINT    vol_channels;
    int     fmt_valid;
    DWORD   rate;
    WORD    bits;             /* container */
    WORD    valid_bits;
    WORD    channels;
    int     is_float;
} AudioEp;

typedef struct SpatialInfo {
    int     checked;          /* 0 = no render endpoint / could not query */
    int     active;           /* 1 = a spatial sound format is processing this endpoint */
    GUID    format;
    wchar_t name[64];         /* display name of the format ("" if unknown) */
    UINT32  max_objects;
} SpatialInfo;

/* Message posted by the callbacks (see main.c): WM_APP+2 device topology changed (coalesce),
   WM_APP+3 volume changed externally (wParam = EpKind). */
int            audio_init(HWND notify, UINT msg_devchange, UINT msg_volchange);
void           audio_shutdown(void);
void           audio_rescan(void);
const AudioEp *audio_ep(EpKind k);
void           audio_refresh_volume(EpKind k);
int            audio_set_volume(EpKind k, float scalar);
int            audio_set_mute(EpKind k, int mute);
int            audio_set_balance(EpKind k, float balance);
void           audio_spatial(SpatialInfo *out);

/* Format helpers for the UI: "44.1 kHz / 24-bit" and "44.1 kHz · 24-bit · 2 ch". */
void           audio_format_short(const AudioEp *e, wchar_t *out, int cch);
void           audio_format_long(const AudioEp *e, wchar_t *out, int cch);

#endif /* E1P_AUDIO_H */
