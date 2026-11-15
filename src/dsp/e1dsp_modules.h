/*
 * e1dsp_modules.h - internal module contract of libe1dsp.
 *
 * Every module:
 *   - is created once (may allocate) and destroyed once;
 *   - processes PLANAR float blocks of exactly E1DSP_BLOCK frames, in place: ch[c][0..n-1];
 *   - set_* functions are RT-safe and ramp/smooth internally (no zipper noise, no clicks);
 *   - keeps internal state in double where it matters (recursive filters, envelopes);
 *   - reports its fixed latency in frames (the chain compensates dry paths);
 *   - must be finite for any finite input; must decay to exact 0.0f for digital silence input
 *     within a bounded time (no denormal tails; use e1_flush_denormal or DC offset tricks).
 *
 * Ownership (one implementer per file):
 *   e1_eq.c, e1_bass.c, e1_limiter.c, e1dsp_chain.c, e1fx_params.c      -> "dsp-core"
 *   e1_surround.c, e1_crystal.c, e1_dialog.c, e1_svm.c, e1_hrtf_data.c   -> "dsp-spatial"
 *   e1_nr.c, e1_voicefx.c                                                -> "dsp-voice"
 * Mic Smart Volume reuses E1Svm (dsp-spatial) with is_mic = 1.
 */
#ifndef E1DSP_MODULES_H
#define E1DSP_MODULES_H

#include "e1dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------- Graphic EQ (10 band, spec 3.7) ------------------------------------------------ */
typedef struct E1Eq E1Eq;
E1Eq    *e1_eq_create(double fs, int nch);
void     e1_eq_destroy(E1Eq *m);
void     e1_eq_reset(E1Eq *m);
void     e1_eq_set(E1Eq *m, float preamp_db, const float gains_db[E1FX_EQ_BANDS]);
void     e1_eq_process(E1Eq *m, float *const *ch, int nch, int n);
int      e1_eq_is_flat(const E1Eq *m);           /* all targets 0 dB and ramps finished */
unsigned e1_eq_latency(const E1Eq *m);           /* 0 */

/* -------- X-Bass (spec 3.8) ------------------------------------------------------------- */
typedef struct E1Bass E1Bass;
E1Bass  *e1_bass_create(double fs, int nch);
void     e1_bass_destroy(E1Bass *m);
void     e1_bass_reset(E1Bass *m);
void     e1_bass_set(E1Bass *m, float level_0_100, float xover_hz, int headphones);
void     e1_bass_process(E1Bass *m, float *const *ch, int nch, int n);
unsigned e1_bass_latency(const E1Bass *m);

/* -------- Look-ahead true-peak safety limiter (spec 3.8, THX Limiter-like) -------------- */
typedef struct E1Limiter E1Limiter;
E1Limiter *e1_limiter_create(double fs, int nch);
void       e1_limiter_destroy(E1Limiter *m);
void       e1_limiter_reset(E1Limiter *m);
void       e1_limiter_set(E1Limiter *m, float ceiling_dbfs /* e.g. -0.3 */);
void       e1_limiter_process(E1Limiter *m, float *const *ch, int nch, int n);
unsigned   e1_limiter_latency(const E1Limiter *m);   /* look-ahead in frames */
float      e1_limiter_gr_db(const E1Limiter *m);     /* current gain reduction, <= 0 */

/* -------- SBX Surround: stereo upmix + decorrelation + binaural/crosstalk virtualizer ----- */
typedef struct E1Surround E1Surround;
E1Surround *e1_surround_create(double fs);            /* always stereo in -> stereo out */
void        e1_surround_destroy(E1Surround *m);
void        e1_surround_reset(E1Surround *m);
void        e1_surround_set(E1Surround *m, float level_0_1, int speaker_mode /* E1FX_SPK_* */);
void        e1_surround_process(E1Surround *m, float *l, float *r, int n);
unsigned    e1_surround_latency(const E1Surround *m);
/* Added (backward compatible): frames a freshly reset instance needs before its output is free of
   the onset transient of its internal delays (pre-delay + all-pass tails + FIR length). The chain
   holds the slot's crossfade weight at 0 for this long after a reset. */
unsigned    e1_surround_settle_frames(const E1Surround *m);

/* -------- Crystalizer (spec 3.6) ------------------------------------------------------- */
typedef struct E1Crystal E1Crystal;
E1Crystal *e1_crystal_create(double fs, int nch);
void       e1_crystal_destroy(E1Crystal *m);
void       e1_crystal_reset(E1Crystal *m);
void       e1_crystal_set(E1Crystal *m, float level_0_1);
void       e1_crystal_process(E1Crystal *m, float *const *ch, int nch, int n);
unsigned   e1_crystal_latency(const E1Crystal *m);

/* -------- Dialog Plus (spec 3.5) -------------------------------------------------------- */
typedef struct E1Dialog E1Dialog;
E1Dialog *e1_dialog_create(double fs);                /* stereo */
void      e1_dialog_destroy(E1Dialog *m);
void      e1_dialog_reset(E1Dialog *m);
void      e1_dialog_set(E1Dialog *m, float level_0_1);
void      e1_dialog_process(E1Dialog *m, float *l, float *r, int n);
unsigned  e1_dialog_latency(const E1Dialog *m);

/* -------- Smart Volume (spec 3.5), also used for the mic -------------------------------- */
typedef struct E1Svm E1Svm;
E1Svm   *e1_svm_create(double fs, int nch, int is_mic);
void     e1_svm_destroy(E1Svm *m);
void     e1_svm_reset(E1Svm *m);
void     e1_svm_set(E1Svm *m, float level_0_1, int mode /* E1FX_SVM_* ; ignored for mic */);
void     e1_svm_process(E1Svm *m, float *const *ch, int nch, int n);
unsigned e1_svm_latency(const E1Svm *m);              /* look-ahead (96 frames @ >24k) */
float    e1_svm_gain_db(const E1Svm *m);

/* -------- Mic noise reduction (STFT spectral suppressor, spec 3.9) --------------------- */
typedef struct E1Nr E1Nr;
E1Nr    *e1_nr_create(double fs, int nch);
void     e1_nr_destroy(E1Nr *m);
void     e1_nr_reset(E1Nr *m);
void     e1_nr_set(E1Nr *m, float strength_0_1);
void     e1_nr_process(E1Nr *m, float *const *ch, int nch, int n);
unsigned e1_nr_latency(const E1Nr *m);

/* -------- VoiceFX (pitch + formant transformation, spec 3.9) ---------------------------- */
typedef struct E1VoiceFx E1VoiceFx;
E1VoiceFx *e1_vfx_create(double fs, int nch);
void       e1_vfx_destroy(E1VoiceFx *m);
void       e1_vfx_reset(E1VoiceFx *m);
void       e1_vfx_set(E1VoiceFx *m, const float p[E1FX_VFX_PARAMS]);
void       e1_vfx_process(E1VoiceFx *m, float *const *ch, int nch, int n);
unsigned   e1_vfx_latency(const E1VoiceFx *m);

/* VoiceFX preset table (18 presets, values from docs/re/control-panel.md 4.3). Owner: dsp-voice. */
typedef struct E1VfxPreset { const char *name; float p[E1FX_VFX_PARAMS]; } E1VfxPreset;
extern const E1VfxPreset e1_vfx_presets[];
extern const int e1_vfx_preset_count;

/* EQ preset table (spec 3.7). Owner: dsp-core (in e1fx_params.c). */
typedef struct E1EqPreset { const char *name; float gain_db[E1FX_EQ_BANDS]; } E1EqPreset;
extern const E1EqPreset e1_eq_presets[];
extern const int e1_eq_preset_count;

#ifdef __cplusplus
}
#endif
#endif /* E1DSP_MODULES_H */
