/*
 * e1_limiter.c - stereo-linked look-ahead true-peak safety limiter (spec 3.8).
 *
 * Detector (per input frame n, all channels linked):
 *   - 4x polyphase interpolator (129-tap Kaiser-windowed sinc prototype, beta 8, cut-off at the
 *     input Nyquist, own design; 32 taps per phase). The prototype is a Nyquist filter: phase 0
 *     is exactly x[n-16], phases 1..3 give the values at n-16+j/4; the passband is flat to
 *     ~0.43*fs (19 kHz at 44.1 kHz). Every local extremum of the 4x stream is refined with a
 *     parabolic fit, so the inter-sample peak estimate of band-limited material is accurate to
 *     a few hundredths of a dB. (Content between ~0.43*fs and fs/2 is only partly seen - the
 *     same limitation as an ITU-R BS.1770 true-peak meter.)
 *   - p[n] = max over channels; p'[n] = max(p[n], p[n-1]) covers both sides of every sample.
 *   - required gain g_req = min(1, ceiling / p').
 * Gain computer:
 *   - instant attack / hold (20 ms, refreshed by every crest within 0.1 dB of the current gain,
 *     so steady low-frequency tones get a constant gain and no ripple distortion) / exponential
 *     release (250 ms) on g_req; env <= g_req always;
 *   - sliding minimum over H frames, then two cascaded moving averages (B1 + B2 - 1 <= H).
 *     This guarantees that the gain applied to a sample is <= the gain that sample requires,
 *     while the attack is a smooth S-shaped ramp spread over the whole look-ahead.
 * Audio delay = H + DET_D - 1 frames (= the look-ahead, 64 frames at 44.1 kHz, ~1.45 ms).
 * With no gain reduction the output is the input delayed, bit for bit (gain exactly 1.0).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "e1dsp_modules.h"
#include "e1_biquad.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__) || defined(__x86_64__)
#include <xmmintrin.h>
#define E1_LIM_SSE 1
#endif

#define OS       4
#ifndef E1_LIM_K
#define E1_LIM_K 32            /* taps per interpolator phase (multiple of 4) */
#endif
#ifndef E1_LIM_BETA
#define E1_LIM_BETA 8.0        /* Kaiser window beta */
#endif
#ifndef E1_LIM_CUT
#define E1_LIM_CUT 1.0         /* interpolator cut-off relative to the input Nyquist frequency */
#endif
#define PROTO_N  (OS * E1_LIM_K + 1)   /* odd prototype length */
#define PH_TAPS  E1_LIM_K              /* taps of phases 1..3 */
#define DET_D    (E1_LIM_K / 2)        /* detector delay in input frames */
#define HIST     64                    /* input history ring (> E1_LIM_K, power of two), stored twice */
#define SAFETY   0.9995                /* -0.004 dB margin for gain-modulation effects */
#define HOLD_TOL 1.0116                /* crests within 0.1 dB of the current gain refresh the hold */
#define REL_SNAP 1e-5                  /* release snaps to exact unity within 1e-5 */

struct E1Limiter {
    double   fs;
    int      nch;
    unsigned la;               /* total latency */
    int      H, B1, B2;
    double   ceil_lin;
    float    ph[OS - 1][PH_TAPS]; /* phases 1..3, time-reversed: ph[j-1][u] multiplies x[n-(K-1)+u] */
    float    hist[E1DSP_MAX_CH][2 * HIST]; /* every sample written at p and p+HIST: contiguous windows */
    unsigned hpos;
    double   v1[E1DSP_MAX_CH], v2[E1DSP_MAX_CH]; /* last two values of each 4x stream */
    double   p_prev;
    double   env;
    int      hold_len, hold_left;
    double   rel;
    /* sliding minimum (monotonic deque) */
    double  *dq_v;
    unsigned *dq_i;
    unsigned dq_mask, dq_head, dq_size;
    unsigned t;                /* frame counter */
    /* moving averages */
    double  *box1, *box2;
    int      b1pos, b2pos;
    double   sum1, sum2;
    /* audio delay */
    float   *dly[E1DSP_MAX_CH];
    unsigned dmask, dpos;
    double   gain;             /* last applied gain (meter) */
    float    *mem;
};

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

static void design_interp(E1Limiter *m)
{
    const double beta = E1_LIM_BETA, c = (PROTO_N - 1) / 2.0;
    double h[PROTO_N];
    for (int k = 0; k < PROTO_N; ++k) {
        double x = E1_LIM_CUT * (k - c) / OS;
        double s = (x == 0.0) ? 1.0 : sin(E1_PI * x) / (E1_PI * x);
        double r = (k - c) / c;
        double w = bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
        h[k] = s * w;
    }
    /* phase j uses h[j + 4t] with x[n - t]; normalise every phase to unity DC gain */
    for (int j = 1; j < OS; ++j) {
        double sum = 0.0;
        for (int t = 0; t < PH_TAPS; ++t) sum += h[j + OS * t];
        for (int t = 0; t < PH_TAPS; ++t) m->ph[j - 1][PH_TAPS - 1 - t] = (float)(h[j + OS * t] / sum);
    }
}

static unsigned pow2_at_least(unsigned v)
{
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}

E1Limiter *e1_limiter_create(double fs, int nch)
{
    if (!(fs >= 8000.0 && fs <= 384000.0) || nch < 1 || nch > E1DSP_MAX_CH) return NULL;
    E1Limiter *m = (E1Limiter *)calloc(1, sizeof(E1Limiter));
    if (!m) return NULL;
    m->fs = fs;
    m->nch = nch;
    unsigned la = (unsigned)floor(fs * 64.0 / 44100.0 + 0.5);
    if (la < 16) la = 16;
    m->la = la;
    m->H = (int)la - (DET_D - 1);
    m->B1 = (m->H + 1) / 2;
    m->B2 = m->H + 1 - m->B1;           /* B1 + B2 - 1 == H */
    m->ceil_lin = pow(10.0, -0.3 / 20.0);
    m->hold_len = (int)floor(fs * 0.020 + 0.5);
    m->rel = exp(-1.0 / (0.250 * fs));
    design_interp(m);

    m->dq_mask = pow2_at_least((unsigned)m->H + 2) - 1;
    m->dmask = pow2_at_least(la + 1) - 1;
    m->dq_v = (double *)calloc(m->dq_mask + 1, sizeof(double));
    m->dq_i = (unsigned *)calloc(m->dq_mask + 1, sizeof(unsigned));
    m->box1 = (double *)calloc((size_t)m->B1, sizeof(double));
    m->box2 = (double *)calloc((size_t)m->B2, sizeof(double));
    m->mem = (float *)calloc((size_t)(m->dmask + 1) * (size_t)nch, sizeof(float));
    if (!m->dq_v || !m->dq_i || !m->box1 || !m->box2 || !m->mem) { e1_limiter_destroy(m); return NULL; }
    for (int c = 0; c < nch; ++c) m->dly[c] = m->mem + (size_t)c * (m->dmask + 1);
    e1_limiter_reset(m);
    return m;
}

void e1_limiter_destroy(E1Limiter *m)
{
    if (!m) return;
    free(m->dq_v); free(m->dq_i); free(m->box1); free(m->box2); free(m->mem);
    free(m);
}

void e1_limiter_reset(E1Limiter *m)
{
    if (!m) return;
    memset(m->hist, 0, sizeof(m->hist));
    memset(m->v1, 0, sizeof(m->v1));
    memset(m->v2, 0, sizeof(m->v2));
    m->hpos = 0;
    m->p_prev = 0.0;
    m->env = 1.0;
    m->hold_left = 0;
    m->dq_head = 0; m->dq_size = 0; m->t = 0;
    for (int i = 0; i < m->B1; ++i) m->box1[i] = 1.0;
    for (int i = 0; i < m->B2; ++i) m->box2[i] = 1.0;
    m->b1pos = m->b2pos = 0;
    m->sum1 = (double)m->B1;
    m->sum2 = (double)m->B2;
    memset(m->mem, 0, (size_t)(m->dmask + 1) * (size_t)m->nch * sizeof(float));
    m->dpos = 0;
    m->gain = 1.0;
}

void e1_limiter_set(E1Limiter *m, float ceiling_dbfs)
{
    if (!m) return;
    double db = isfinite(ceiling_dbfs) ? ceiling_dbfs : -0.3;
    if (db > 0.0) db = 0.0;
    if (db < -24.0) db = -24.0;
    m->ceil_lin = pow(10.0, db / 20.0);
}

unsigned e1_limiter_latency(const E1Limiter *m) { return m ? m->la : 0; }

float e1_limiter_gr_db(const E1Limiter *m)
{
    if (!m || m->gain >= 1.0) return 0.f;
    return (float)e1_lin2db(m->gain);
}

/* Parabolic refinement of the extremum at the middle of (a, b, c); returns |peak| estimate. */
static double refine(double a, double b, double c)
{
    double ab = fabs(b);
    if (ab < fabs(a) || ab < fabs(c)) return ab;
    if (b < 0.0) { a = -a; b = -b; c = -c; }
    double den = 2.0 * b - a - c;
    if (den <= 1e-30) return ab;
    double d = a - c, pk = b + d * d / (8.0 * den);
    return pk > 1.5 * b ? 1.5 * b : pk;
}

/* Streams one 4x value of channel c, returns the best peak estimate it contributes. */
static double push_os(E1Limiter *m, int c, double v)
{
    double r = refine(m->v2[c], m->v1[c], v);
    m->v2[c] = m->v1[c];
    m->v1[c] = v;
    double av = fabs(v);
    return av > r ? av : r;
}

/* The three fractional-phase values for the window win[0..K-1] (float accumulation is ample
   for a peak detector: relative error ~1e-6). */
static void interp3(const E1Limiter *m, const float *win, double v[OS - 1])
{
#ifdef E1_LIM_SSE
    __m128 a1 = _mm_setzero_ps(), a2 = _mm_setzero_ps(), a3 = _mm_setzero_ps();
    for (int t = 0; t < PH_TAPS; t += 4) {
        __m128 w = _mm_loadu_ps(win + t);
        a1 = _mm_add_ps(a1, _mm_mul_ps(w, _mm_loadu_ps(m->ph[0] + t)));
        a2 = _mm_add_ps(a2, _mm_mul_ps(w, _mm_loadu_ps(m->ph[1] + t)));
        a3 = _mm_add_ps(a3, _mm_mul_ps(w, _mm_loadu_ps(m->ph[2] + t)));
    }
    float r[3][4];
    _mm_storeu_ps(r[0], a1);
    _mm_storeu_ps(r[1], a2);
    _mm_storeu_ps(r[2], a3);
    for (int j = 0; j < 3; ++j) v[j] = ((double)r[j][0] + r[j][1]) + ((double)r[j][2] + r[j][3]);
#else
    for (int j = 0; j < OS - 1; ++j) {
        double acc = 0.0;
        for (int t = 0; t < PH_TAPS; ++t) acc += (double)m->ph[j][t] * (double)win[t];
        v[j] = acc;
    }
#endif
}

void e1_limiter_process(E1Limiter *m, float *const *ch, int nch, int n)
{
    if (!m || !ch || n <= 0) return;
    if (nch > m->nch) nch = m->nch;
    const double ceil_t = m->ceil_lin * SAFETY;
    const unsigned H = (unsigned)m->H, la = m->la;

    /* resynchronise the running sums (bounded float drift) */
    double s = 0.0;
    for (int i = 0; i < m->B1; ++i) s += m->box1[i];
    m->sum1 = s;
    s = 0.0;
    for (int i = 0; i < m->B2; ++i) s += m->box2[i];
    m->sum2 = s;

    for (int i = 0; i < n; ++i) {
        /* --- detector --- */
        const unsigned hp = m->hpos;
        double p = 0.0;
        for (int c = 0; c < nch; ++c) {
            float *hc = m->hist[c];
            hc[hp] = hc[hp + HIST] = ch[c][i];
            const float *win = hc + hp + HIST - (PH_TAPS - 1); /* x[n-(K-1)] .. x[n] */
            double v[OS - 1];
            interp3(m, win, v);
            double pk = push_os(m, c, (double)win[PH_TAPS - 1 - DET_D]);
            for (int j = 0; j < OS - 1; ++j) {
                double q = push_os(m, c, v[j]);
                if (q > pk) pk = q;
            }
            if (pk > p) p = pk;
        }
        m->hpos = (hp + 1) & (HIST - 1);
        double pp = p > m->p_prev ? p : m->p_prev;
        m->p_prev = p;
        double g_req = (pp > ceil_t) ? ceil_t / pp : 1.0;

        /* --- attack / hold / release --- */
        if (g_req < m->env) {
            m->env = g_req;
            m->hold_left = m->hold_len;
        } else if (g_req < 1.0 && g_req < m->env * HOLD_TOL) {
            m->hold_left = m->hold_len;   /* periodic crests at the same level: no gain ripple */
        } else if (m->hold_left > 0) {
            m->hold_left--;
        } else {
            m->env = g_req + (m->env - g_req) * m->rel;
            if (g_req - m->env < REL_SNAP) m->env = g_req;
        }

        /* --- sliding minimum over H frames --- */
        const unsigned t = m->t++;
        while (m->dq_size > 0) {
            unsigned back = (m->dq_head + m->dq_size - 1) & m->dq_mask;
            if (m->dq_v[back] >= m->env) m->dq_size--; else break;
        }
        {
            unsigned pos = (m->dq_head + m->dq_size) & m->dq_mask;
            m->dq_v[pos] = m->env;
            m->dq_i[pos] = t;
            m->dq_size++;
        }
        if (t - m->dq_i[m->dq_head] >= H) { m->dq_head = (m->dq_head + 1) & m->dq_mask; m->dq_size--; }
        double gmin = m->dq_v[m->dq_head];

        /* --- two moving averages --- */
        m->sum1 += gmin - m->box1[m->b1pos];
        m->box1[m->b1pos] = gmin;
        if (++m->b1pos == m->B1) m->b1pos = 0;
        double a1 = m->sum1 / m->B1;
        m->sum2 += a1 - m->box2[m->b2pos];
        m->box2[m->b2pos] = a1;
        if (++m->b2pos == m->B2) m->b2pos = 0;
        double g = m->sum2 / m->B2;
        if (g > 1.0 - 1e-12) g = 1.0;   /* exact unity when nothing is limited */
        m->gain = g;

        /* --- delayed audio --- */
        const unsigned wp = m->dpos, rp = (wp - la) & m->dmask;
        if (g == 1.0) {
            for (int c = 0; c < nch; ++c) {
                float x = ch[c][i];
                ch[c][i] = m->dly[c][rp];
                m->dly[c][wp] = x;
            }
        } else {
            const float gf = (float)g;
            for (int c = 0; c < nch; ++c) {
                float x = ch[c][i];
                ch[c][i] = m->dly[c][rp] * gf;
                m->dly[c][wp] = x;
            }
        }
        m->dpos = (wp + 1) & m->dmask;
    }
}
