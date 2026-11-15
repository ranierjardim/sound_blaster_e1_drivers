/*
 * e1_hrtf_data.h - analytic HRTF model and virtualizer filter-bank designer (owner: dsp-spatial).
 *
 * Nothing in here is measured or copied data: every filter is generated at create() time for the
 * actual sample rate from a parametric spherical-head model (Brown & Duda style head shadow and
 * Woodworth/Brown-Duda ear delays, plus simple pinna echoes, a torso reflection, a concha
 * resonance and a rear pinna-shadow), diffuse-field equalised, and converted to minimum phase.
 *
 * The virtualizer works in the "shuffler" (sum/difference) domain.  For a symmetric pair of virtual
 * speakers at +-az fed with A_L, A_R, the ear signals are
 *     E_L = Fsum * (A_L + A_R)/2 + Fdif * (A_L - A_R)/2
 *     E_R = Fsum * (A_L + A_R)/2 - Fdif * (A_L - A_R)/2
 * with Fsum = H_ipsi + H_contra * z^-ITD and Fdif = H_ipsi - H_contra * z^-ITD.  Both are (very
 * nearly) minimum phase, so they are designed from magnitude and reconstructed as minimum-phase
 * FIRs (peak at tap 0, no added latency).  Three pairs are rendered: front (direct M/S), side
 * ambience and back ambience.
 *
 * Bank layout: coef[mode][level][filter][tap], mode = E1FX_SPK_HEADPHONES / E1FX_SPK_STEREO,
 * level index j <-> immersion j / (E1_VIRT_LEVELS - 1).  Every level set is energy normalised
 * (pink-noise weighted) for mid and side content separately, so loudness is independent of the
 * immersion setting and of the stereo correlation of the programme.
 */
#ifndef E1_HRTF_DATA_H
#define E1_HRTF_DATA_H

#include <stddef.h>
#include "e1_biquad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Virtual loudspeaker geometry, degrees (positive = left). */
#define E1_VIRT_FRONT_DEG     30.0
#define E1_VIRT_SIDE_DEG     110.0
#define E1_VIRT_BACK_DEG     145.0
#define E1_VIRT_SPK_WIDE_DEG  45.0   /* speaker mode: front image widened from +-30 to +-45 */

#define E1_VIRT_LEVELS 11            /* immersion grid 0.0, 0.1, ... 1.0 */
#define E1_VIRT_NFILT  6
enum {
    E1_VF_FRONT_SUM = 0, E1_VF_FRONT_DIF = 1,
    E1_VF_SIDE_SUM  = 2, E1_VF_SIDE_DIF  = 3,
    E1_VF_BACK_SUM  = 4, E1_VF_BACK_DIF  = 5
};
#define E1_VIRT_MIN_TAPS  16
#define E1_VIRT_MAX_TAPS 256

/* Immersion 0..1 is scaled by 0.6 internally (same convention as the original engine): it is the
   exponent applied to the log-magnitude of the front HRTF pair, i.e. the "wet amount". */
#define E1_VIRT_WET_SCALE 0.6
/* At immersion 1.0 the decorrelated ambience carries this fraction of the side (L-R) energy;
   the direct side path is reduced by the same amount so the total stays constant. */
#define E1_VIRT_AMB_MAX_FRACTION 0.45

/* ---- ambience (rear feed) path, shared by the runtime and the designer ------------------- */
#define E1_AMB_HPF_HZ      100.0
#define E1_AMB_SHELF_HZ   6000.0
#define E1_AMB_SHELF_DB     -3.0
#define E1_AMB_PREDELAY_MS  13.0
#define E1_AMB_AP_G          0.465
#define E1_AMB_AP0_MS        4.7     /* side left   */
#define E1_AMB_AP1_MS        3.7     /* side right  */
#define E1_AMB_AP2_MS        6.5     /* back left   */
#define E1_AMB_AP3_MS        7.0     /* back right  */
#define E1_AMB_BACK_GAIN     0.70710678118654752

static inline int e1_amb_delay_samples(double ms, double fs)
{
    int d = (int)floor(ms * 0.001 * fs + 0.5);
    return d < 1 ? 1 : d;
}

static inline void e1_amb_filters(double fs, E1BqCoef *hp, E1BqCoef *shelf)
{
    double fsh = E1_AMB_SHELF_HZ;
    if (fsh > 0.45 * fs) fsh = 0.45 * fs;
    e1_bq_highpass(hp, fs, E1_AMB_HPF_HZ, 0.70710678118654752);
    e1_bq_highshelf(shelf, fs, fsh, 0.70710678118654752, E1_AMB_SHELF_DB);
}

static inline size_t e1_virt_index(int mode, int level, int filt, int taps)
{
    return ((((size_t)mode * E1_VIRT_LEVELS + (size_t)level) * E1_VIRT_NFILT) + (size_t)filt) * (size_t)taps;
}

/* FIR length used at this sample rate (about 1.45 ms, multiple of 4, clamped). */
int e1_virt_taps(double fs);

/* Designs the complete bank (2 * E1_VIRT_LEVELS * E1_VIRT_NFILT * taps floats).
   Allocates and frees temporary memory; NOT real-time safe. Returns 0 on success. */
int e1_virt_design(double fs, int taps, float *coef);

/* The analytic model itself (exposed for tests and tools).
   ear: +1 = left ear, -1 = right ear.  Magnitude is linear, NOT diffuse-field equalised. */
double e1_hrtf_model_mag(double f_hz, double az_deg, int ear);
/* Arrival time relative to the head centre in seconds (negative = earlier). */
double e1_hrtf_model_delay(double az_deg, int ear);

#ifdef __cplusplus
}
#endif
#endif /* E1_HRTF_DATA_H */
