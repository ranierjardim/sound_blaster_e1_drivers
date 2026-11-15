/*
 * test_common.h - tiny test framework and measurement helpers for the libe1dsp tests.
 * Header-only, C11. Not part of the shipped library.
 */
#ifndef E1_TEST_COMMON_H
#define E1_TEST_COMMON_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "../../src/dsp/e1dsp_modules.h"
#include "../../src/dsp/e1_biquad.h"

static int g_checks = 0, g_failed = 0;

#define CHECK(cond, ...) do { \
        ++g_checks; \
        if (!(cond)) { ++g_failed; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static int test_summary(const char *name)
{
    printf("== %s: %d checks, %d failed -> %s\n", name, g_checks, g_failed, g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}

/* xorshift64* */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static void rng_seed(uint64_t s) { g_rng = s ? s : 0x9E3779B97F4A7C15ull; }
static uint64_t rng_u64(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static double rng_unit(void) { return (double)(rng_u64() >> 11) * (1.0 / 9007199254740992.0); }
static float rng_sym(void) { return (float)(2.0 * rng_unit() - 1.0); }
static int rng_int(int lo, int hi) { return lo + (int)(rng_unit() * (double)(hi - lo + 1)); }

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int is_subnormal_f(float v) { return v != 0.0f && fabsf(v) < 1.17549435e-38f; }

/* Least-squares amplitude of a sinusoid of frequency f in x[i*stride], i in [0, n). */
static double sine_amp(const float *x, size_t n, size_t stride, double fs, double f)
{
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0, w = 2.0 * E1_PI * f / fs;
    for (size_t i = 0; i < n; ++i) {
        double s = sin(w * (double)i), c = cos(w * (double)i), v = (double)x[i * stride];
        ss += s * s; sc += s * c; cc += c * c; xs += v * s; xc += v * c;
    }
    double det = ss * cc - sc * sc;
    if (fabs(det) < 1e-30) return 0.0;
    double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    return sqrt(a * a + b * b);
}

/* Residual RMS after removing the best-fit sinusoid at f (and DC), relative to its amplitude. */
static double sine_residual_db(const float *x, size_t n, size_t stride, double fs, double f)
{
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0, w = 2.0 * E1_PI * f / fs;
    for (size_t i = 0; i < n; ++i) {
        double s = sin(w * (double)i), c = cos(w * (double)i), v = (double)x[i * stride];
        ss += s * s; sc += s * c; cc += c * c; xs += v * s; xc += v * c;
    }
    double det = ss * cc - sc * sc;
    double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double e = 0;
    for (size_t i = 0; i < n; ++i) {
        double r = (double)x[i * stride] - a * sin(w * (double)i) - b * cos(w * (double)i);
        e += r * r;
    }
    double amp = sqrt(a * a + b * b);
    return 20.0 * log10(sqrt(e / (double)n) / (amp / sqrt(2.0)) + 1e-30);
}

/* Oversampling peak meter: os-times windowed-sinc interpolation with 2K taps per phase
   (Kaiser window, cut-off at the input Nyquist, each phase normalised to unity DC gain).
   true_peak():        16x, 128 taps, beta 9: flat to ~0.48 fs ("near-ideal" reconstruction).
   true_peak_bs1770(): 4x, 48 taps, beta 5: an ITU-R BS.1770-style dBTP meter (own design). */
static double tp_i0(double x)
{
    double s = 1, t = 1, q = x * x / 4;
    for (int k = 1; k < 80; ++k) { t *= q / ((double)k * k); s += t; if (t < s * 1e-17) break; }
    return s;
}
static double peak_os(const float *x, size_t n, size_t stride, int os, int K, double beta)
{
    double *tab = (double *)malloc((size_t)os * 2 * (size_t)K * sizeof(double));
    for (int j = 0; j < os; ++j) {
        double fr = (double)j / os, sum = 0;
        for (int k = 0; k < 2 * K; ++k) {
            double t = (double)(k - K + 1) - fr; /* tap position relative to the interpolated point */
            double sn = fabs(t) < 1e-12 ? 1.0 : sin(E1_PI * t) / (E1_PI * t);
            double r = t / (double)K;
            double w = fabs(r) < 1.0 ? tp_i0(beta * sqrt(1.0 - r * r)) / tp_i0(beta) : 0.0;
            tab[j * 2 * K + k] = sn * w;
            sum += sn * w;
        }
        for (int k = 0; k < 2 * K; ++k) tab[j * 2 * K + k] /= sum;
    }
    double pk = 0;
    for (size_t i = 0; i < n; ++i) {
        double a = fabs((double)x[i * stride]);
        if (a > pk) pk = a;
    }
    for (size_t i = (size_t)K - 1; i + (size_t)K < n; ++i) {
        const float *b = x + (i - ((size_t)K - 1)) * stride;
        for (int j = 1; j < os; ++j) {
            const double *t = tab + j * 2 * K;
            double acc = 0;
            for (int k = 0; k < 2 * K; ++k) acc += t[k] * (double)b[(size_t)k * stride];
            acc = fabs(acc);
            if (acc > pk) pk = acc;
        }
    }
    free(tab);
    return pk;
}
static double true_peak(const float *x, size_t n, size_t stride) { return peak_os(x, n, stride, 16, 64, 9.0); }
static double true_peak_bs1770(const float *x, size_t n, size_t stride) { return peak_os(x, n, stride, 4, 6, 5.0); }

/* In-place linear-phase low-pass (windowed sinc, 255 taps) of an interleaved channel. */
static void lowpass_fir(float *x, size_t n, size_t stride, double fc_norm)
{
    enum { L = 255 };
    double h[L], sum = 0;
    for (int k = 0; k < L; ++k) {
        double t = k - (L - 1) / 2.0, r = t / ((L - 1) / 2.0);
        double sn = t == 0 ? 2 * fc_norm : sin(2 * E1_PI * fc_norm * t) / (E1_PI * t);
        h[k] = sn * tp_i0(8.0 * sqrt(fmax(0.0, 1 - r * r))) / tp_i0(8.0);
        sum += h[k];
    }
    float *y = (float *)calloc(n, sizeof(float));
    for (size_t i = 0; i < n; ++i) {
        double acc = 0;
        for (int k = 0; k < L; ++k) {
            long j = (long)i - k + (L - 1) / 2;
            if (j >= 0 && (size_t)j < n) acc += h[k] * x[(size_t)j * stride];
        }
        y[i] = (float)(acc / sum);
    }
    for (size_t i = 0; i < n; ++i) x[i * stride] = y[i];
    free(y);
}

static double db(double lin) { return 20.0 * log10(lin > 1e-30 ? lin : 1e-30); }

#endif
