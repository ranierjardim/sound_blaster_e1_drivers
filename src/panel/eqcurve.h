/*
 * eqcurve.h - frequency response of the 10-band graphic EQ for the panel's curve display.
 * Section designs follow docs/RECONSTRUCTION_SPEC.md 3.7 (low-shelf 31.25 Hz, eight one-octave
 * constant-Q peaking sections, high-shelf 16 kHz, preamp folded into section 0) and are evaluated
 * with the shared biquad helpers in src/dsp/e1_biquad.h (E1BqCoef + e1_bq_mag).
 */
#ifndef E1P_EQCURVE_H
#define E1P_EQCURVE_H

#include "../dsp/e1_biquad.h"
#include "../../include/e1fx_params.h"

#define EQC_FS 44100.0   /* the E1 runs at 44.1 kHz only */

/* Regalia-Mitra peaking section: unity at DC/Nyquist, gain_db at f0, bandwidth bw_hz. */
void   eqc_peaking(E1BqCoef *c, double fs, double f0, double bw_hz, double gain_db);
/* All 10 sections; preamp is multiplied into section 0's numerator. */
void   eqc_design(double fs, float preamp_db, const float gains_db[E1FX_EQ_BANDS], E1BqCoef out[E1FX_EQ_BANDS]);
/* Total response in dB at f. */
double eqc_response_db(const E1BqCoef c[E1FX_EQ_BANDS], double fs, double f);

#endif /* E1P_EQCURVE_H */
