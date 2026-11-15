/*
 * e1_biquad.h - shared filter helpers for libe1dsp (header-only, C99).
 *
 * Convention (textbook sign): H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
 * NOTE: Creative's engine stores negated a1/a2 (feedback added); when porting constants from
 * docs/re/apo-dsp.md negate their a1/a2.
 * Implementation: Transposed Direct Form II with DOUBLE state (good numerical behaviour for
 * low-frequency sections at 44.1 kHz).
 */
#ifndef E1_BIQUAD_H
#define E1_BIQUAD_H

#include <math.h>

#ifndef E1_PI
#define E1_PI 3.14159265358979323846
#endif

typedef struct E1BqCoef { double b0, b1, b2, a1, a2; } E1BqCoef;
typedef struct E1BqState { double z1, z2; } E1BqState;

static inline double e1_db2lin(double db) { return pow(10.0, db / 20.0); }
static inline double e1_lin2db(double g) { return 20.0 * log10(g > 1e-30 ? g : 1e-30); }

/* Flush values below ~1e-25 to 0 (denormal guard for recursive state). */
static inline double e1_flush(double x) { return (fabs(x) < 1e-25) ? 0.0 : x; }

static inline float e1_bq_tick(const E1BqCoef *c, E1BqState *s, float xin)
{
    double x = (double)xin;
    double y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return (float)y;
}

static inline void e1_bq_run(const E1BqCoef *c, E1BqState *s, float *buf, int n)
{
    double z1 = s->z1, z2 = s->z2;
    const double b0 = c->b0, b1 = c->b1, b2 = c->b2, a1 = c->a1, a2 = c->a2;
    for (int i = 0; i < n; ++i) {
        double x = (double)buf[i];
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        buf[i] = (float)y;
    }
    s->z1 = e1_flush(z1);
    s->z2 = e1_flush(z2);
}

static inline void e1_bq_identity(E1BqCoef *c) { c->b0 = 1; c->b1 = c->b2 = c->a1 = c->a2 = 0; }

static inline void e1_bq_norm(E1BqCoef *c, double b0, double b1, double b2, double a0, double a1, double a2)
{
    c->b0 = b0 / a0; c->b1 = b1 / a0; c->b2 = b2 / a0; c->a1 = a1 / a0; c->a2 = a2 / a0;
}

/* RBJ Audio EQ Cookbook designs. f0 in Hz, q > 0, gain in dB. */
static inline void e1_bq_lowpass(E1BqCoef *c, double fs, double f0, double q)
{
    double w = 2 * E1_PI * f0 / fs, cw = cos(w), al = sin(w) / (2 * q);
    e1_bq_norm(c, (1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + al, -2 * cw, 1 - al);
}
static inline void e1_bq_highpass(E1BqCoef *c, double fs, double f0, double q)
{
    double w = 2 * E1_PI * f0 / fs, cw = cos(w), al = sin(w) / (2 * q);
    e1_bq_norm(c, (1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + al, -2 * cw, 1 - al);
}
static inline void e1_bq_bandpass(E1BqCoef *c, double fs, double f0, double q) /* 0 dB peak */
{
    double w = 2 * E1_PI * f0 / fs, cw = cos(w), al = sin(w) / (2 * q);
    e1_bq_norm(c, al, 0, -al, 1 + al, -2 * cw, 1 - al);
}
static inline void e1_bq_peaking(E1BqCoef *c, double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0), w = 2 * E1_PI * f0 / fs, cw = cos(w), al = sin(w) / (2 * q);
    e1_bq_norm(c, 1 + al * A, -2 * cw, 1 - al * A, 1 + al / A, -2 * cw, 1 - al / A);
}
static inline void e1_bq_lowshelf(E1BqCoef *c, double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0), w = 2 * E1_PI * f0 / fs, cw = cos(w), sw = sin(w);
    double al = sw / (2 * q), sa = 2 * sqrt(A) * al;
    e1_bq_norm(c, A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa),
               (A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa);
}
static inline void e1_bq_highshelf(E1BqCoef *c, double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0), w = 2 * E1_PI * f0 / fs, cw = cos(w), sw = sin(w);
    double al = sw / (2 * q), sa = 2 * sqrt(A) * al;
    e1_bq_norm(c, A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa),
               (A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa);
}
static inline void e1_bq_allpass(E1BqCoef *c, double fs, double f0, double q)
{
    double w = 2 * E1_PI * f0 / fs, cw = cos(w), al = sin(w) / (2 * q);
    e1_bq_norm(c, 1 - al, -2 * cw, 1 + al, 1 + al, -2 * cw, 1 - al);
}

/* Magnitude response |H(e^jw)| at f (for tests and auto-normalisation). */
static inline double e1_bq_mag(const E1BqCoef *c, double fs, double f)
{
    double w = 2 * E1_PI * f / fs;
    double cr = cos(w), ci = -sin(w), c2r = cos(2 * w), c2i = -sin(2 * w);
    double nr = c->b0 + c->b1 * cr + c->b2 * c2r, ni = c->b1 * ci + c->b2 * c2i;
    double dr = 1 + c->a1 * cr + c->a2 * c2r, di = c->a1 * ci + c->a2 * c2i;
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

/* One-pole smoothing coefficient for time constant tau (seconds) at update rate fs_update. */
static inline double e1_onepole_coef(double tau_s, double fs_update)
{
    return (tau_s <= 0) ? 0.0 : exp(-1.0 / (tau_s * fs_update));
}

#endif /* E1_BIQUAD_H */
