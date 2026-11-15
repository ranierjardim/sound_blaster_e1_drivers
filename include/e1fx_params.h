/*
 * e1fx_params.h - E1 Studio shared parameter contract.
 *
 * Shared by: libe1dsp (src/dsp), the APO (src/apo), the control panel (src/panel)
 * and the tools. Pure C99, no Windows headers. Plain-old-data, fixed layout,
 * no pointers: the structs are copied verbatim through shared memory and the
 * settings file, so NEVER reorder fields; append new fields at the end and bump
 * E1FX_PARAMS_VERSION.
 *
 * Ranges follow the original Creative control panel (docs/RECONSTRUCTION_SPEC.md 3.1).
 * Out-of-range values must be clamped by the consumer (e1fx_params_sanitize).
 */
#ifndef E1FX_PARAMS_H
#define E1FX_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define E1FX_PARAMS_VERSION 1u
#define E1FX_EQ_BANDS 10
#define E1FX_VFX_PARAMS 8

/* Speaker / headphone mode (feature 0x01000002 in the original panel). */
enum {
    E1FX_SPK_HEADPHONES = 0, /* binaural virtualizer */
    E1FX_SPK_STEREO = 1      /* 2.0/2.1 speakers: crosstalk virtualizer */
};

/* Smart Volume mode. */
enum { E1FX_SVM_NORMAL = 0, E1FX_SVM_LOUD = 1, E1FX_SVM_NIGHT = 2 };

/* Center frequencies of the 10-band graphic EQ, Hz (spec 3.7). */
#define E1FX_EQ_FREQS {31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0}

typedef struct E1RenderParams {
    uint32_t master_on;        /* "SBX" master switch. 0 => bit-exact passthrough of the whole render chain */
    uint32_t surround_on;      /* SBX Surround */
    float    surround_level;   /* 0..1, default 0.40 ("Immersion") */
    uint32_t crystal_on;       /* Crystalizer */
    float    crystal_level;    /* 0..1, default 0.65 */
    uint32_t bass_on;          /* X-Bass */
    float    bass_level;       /* 0..100, default 50 */
    float    bass_xover_hz;    /* 10..1000, default 80 */
    uint32_t svm_on;           /* Smart Volume */
    float    svm_level;        /* 0..1, default 0.74 */
    uint32_t svm_mode;         /* E1FX_SVM_* */
    uint32_t dialog_on;        /* Dialog Plus */
    float    dialog_level;     /* 0..1, default 0.5 */
    uint32_t eq_on;            /* Graphic EQ (NOT switched by master_on in the original; here: eq runs only if eq_on, independent of master_on) */
    float    eq_preamp_db;     /* -12..+12 */
    float    eq_gain_db[E1FX_EQ_BANDS]; /* -24..+24 */
    uint32_t speaker_mode;     /* E1FX_SPK_* */
    uint32_t scout_on;         /* Scout Mode: overrides EQ with the Scout curve and forces surround on */
    float    output_gain_db;   /* -24..+12, default 0. Final trim before the limiter */
    uint32_t limiter_on;       /* safety look-ahead limiter; default 1. Only runs when some stage is active */
} E1RenderParams;

typedef struct E1CaptureParams {
    uint32_t master_on;        /* "CrystalVoice" master. 0 => bit-exact passthrough */
    uint32_t nr_on;            /* Noise Reduction, default on (when master on) */
    float    nr_strength;      /* 0..1, default 0.5 (not exposed by Creative; ours) */
    uint32_t svm_on;           /* Mic Smart Volume */
    float    svm_level;        /* 0..1, default 0.74 */
    uint32_t vfx_on;           /* VoiceFX */
    uint32_t vfx_preset;       /* index into the preset table (0 = Neutral), informational for the UI */
    /* VoiceFX raw parameters, spec 3.1 capture table:
       [0] F1 200..800 Hz (400)   [1] F2 800..1800 (1400)  [2] F3 1600..2750 (2000)
       [3] PitchMod 0.25..4 (1)   [4] EnvelopeMod 0.5..2 (1)
       [5] PitchVariability -1..5 (1)  [6] Quiver 0..0.2 (0)  [7] Contour 0..0.4 (0) */
    float    vfx[E1FX_VFX_PARAMS];
    float    output_gain_db;   /* -24..+24, default 0 */
} E1CaptureParams;

typedef struct E1Params {
    uint32_t       version;    /* E1FX_PARAMS_VERSION */
    uint32_t       size;       /* sizeof(E1Params) */
    E1RenderParams render;
    E1CaptureParams capture;
} E1Params;

/* Fills defaults (master switches OFF => bit-exact; sub-effects at Creative defaults). */
void e1fx_params_default(E1Params *p);
/* Clamps every field into its legal range, replaces NaN/Inf with defaults. */
void e1fx_params_sanitize(E1Params *p);

/* Scout Mode EQ curve (dB), spec 3.7. */
#define E1FX_SCOUT_EQ {2.f, 3.f, 3.f, -2.f, -2.f, 0.f, 3.f, 4.f, 3.f, 2.f}

#ifdef __cplusplus
}
#endif
#endif /* E1FX_PARAMS_H */
