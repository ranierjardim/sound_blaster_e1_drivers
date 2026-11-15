/*
 * e1_fft.h - private helpers of libe1dsp's voice modules (owner: dsp-voice).
 *
 *   E1Fft  : real FFT of power-of-two length (own radix-2 implementation, no external code).
 *   E1Wola : streaming multi-channel STFT framework (sqrt-Hann analysis + synthesis window,
 *            weighted overlap-add). With an identity frame callback the output equals the
 *            input delayed by e1_wola_latency() frames (perfect reconstruction up to float
 *            rounding).
 *
 * Both objects allocate only in *_create; all other calls are real-time safe.
 * Pure C99, no OS headers.
 */
#ifndef E1_FFT_H
#define E1_FFT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------- */
/* Real FFT. Spectrum layout: n + 2 floats, interleaved {re, im} for bins 0 .. n/2.           */
/* ---------------------------------------------------------------------------------------- */
typedef struct E1Fft E1Fft;

E1Fft *e1_fft_create(int n);                 /* n: power of two, 8 .. 65536; NULL on failure */
void   e1_fft_destroy(E1Fft *f);
int    e1_fft_size(const E1Fft *f);
/* Forward DFT, no scaling: X[k] = sum x[t] e^{-2 pi i k t / n}. in (n) and out (n+2) must not alias. */
void   e1_fft_r2c(const E1Fft *f, const float *in, float *out);
/* Inverse DFT including the 1/n factor. Imaginary parts of bins 0 and n/2 are ignored.
   in (n+2) is not modified; in and out must not alias. */
void   e1_fft_c2r(const E1Fft *f, const float *in, float *out);

/* ---------------------------------------------------------------------------------------- */
/* Streaming STFT / weighted overlap-add.                                                     */
/* ---------------------------------------------------------------------------------------- */
typedef struct E1Wola E1Wola;

/* Called once per hop with the analysis spectra of every channel (n + 2 floats each, editable
   in place: whatever is left there is resynthesised) and the raw, un-windowed time-domain
   input frame of every channel (nfft samples, oldest first, read-only). */
typedef void (*E1WolaFrameFn)(void *user, float *const *spec, const float *const *frame, int nch);

/* nfft: power of two >= 16. hop: power of two, nfft/hop in {2,4,8,16,...}.
   block_hint: the caller's usual call size (E1DSP_BLOCK). Calls whose sizes are multiples of
   block_hint keep the latency at its nominal value; other sizes still work but may grow the
   latency once (never beyond nfft - 1), which e1_wola_latency() then reports. */
E1Wola  *e1_wola_create(int nfft, int hop, int nch, int block_hint);
void     e1_wola_destroy(E1Wola *w);
void     e1_wola_reset(E1Wola *w);
/* In place on planar buffers ch[0..nch-1][0..n-1]; nch must be <= the created count.
   Input samples are sanitised on entry (NaN -> 0, clamped to +-1e4) so spectra stay finite. */
void     e1_wola_process(E1Wola *w, float *const *ch, int nch, int n, E1WolaFrameFn fn, void *user);
unsigned e1_wola_latency(const E1Wola *w);
int      e1_wola_nfft(const E1Wola *w);
int      e1_wola_hop(const E1Wola *w);
/* Scale that turns |X[k]|^2 of an analysis spectrum into a per-bin power estimate whose mean
   equals the variance of white input (1 / sum(window^2)). */
float    e1_wola_power_norm(const E1Wola *w);

#ifdef __cplusplus
}
#endif
#endif /* E1_FFT_H */
