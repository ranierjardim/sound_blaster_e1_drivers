/*
 * e1_nr.c - microphone noise reduction (spec 3.9, "Noise Reduction"; owner: dsp-voice).
 *
 * Short-time spectral suppressor, clean-room design from the published literature:
 *
 *   STFT        512-point sqrt-Hann WOLA at 44.1/48 kHz (11.6 ms), 75 % overlap, hop 128.
 *               The frame length scales with fs (about 11.6 ms, power of two); hop = nfft / 4.
 *   Noise PSD   MCRA (Cohen & Berdugo 2002): frequency-smoothed periodogram, recursively
 *               smoothed in the LOG domain (so it falls by tens of dB within a short speech
 *               pause even at high SNR), its minimum over ~1 s tracked with sub-windows
 *               (Martin 2001), speech-presence indicator S - Smin > delta (dB), recursively
 *               averaged presence probability that slows the noise update while speech is
 *               present. The noise estimate is bounded by a bias-compensated minimum, which
 *               also corrects a start-up estimate taken while somebody was already talking.
 *   Gain        decision-directed a-priori SNR (Ephraim & Malah 1984) with the log-spectral
 *               amplitude (LSA, Ephraim & Malah 1985) gain; per-band over-subtraction of the
 *               noise estimate (more in the rumble band and above 5 kHz, little in the speech
 *               band); lower bound = gain floor set by the strength (6 dB .. 24 dB attenuation).
 *               (OM-LSA presence weighting was evaluated and rejected: it cost ~5 dB of speech
 *               fidelity at 15 dB SNR for little extra noise reduction.)
 *               The DC and Nyquist bins are real-valued (1 degree of freedom, badly biased
 *               minimum) and take the gain of their neighbour.
 *   Musical     asymmetric temporal smoothing of the gain (instant rise, ~12 ms release) and
 *   noise       ERB-proportional frequency smoothing of the gain above 1 kHz.
 *
 * Stereo/multichannel input is processed linked: one noise estimate and one gain per bin
 * computed from the channel-averaged power, applied to every channel (keeps the image).
 * Latency: nfft - 128 frames with 128-frame blocks (384 at 44.1 kHz).
 * Digital silence: statistics freeze; output is exactly 0 within latency + nfft frames.
 * Strength 0..1 maps to a gain floor of -6 .. -24 dB and scales the over-subtraction.
 */
#include "e1dsp_modules.h"
#include "e1_fft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NR_PI    3.14159265358979323846
#define NR_U     4         /* minimum-statistics sub-windows */
#define NR_FLOOR 1e-20f    /* power floor (well above denormal range) */
#define NR_LFLOOR (-46.0517f) /* ln(NR_FLOOR) */
#define NR_DELTA_DB 8.0    /* presence threshold above the tracked minimum */
#define NR_CAP_DB 5.0      /* noise estimate never exceeds the minimum by more than this */
#define NR_FRAME_S 0.0116  /* analysis frame ~11.6 ms -> 512 points at 44.1/48 kHz */
#define NR_LO_F0 100.0     /* rumble band ends (raised-cosine, log f) between these */
#define NR_LO_F1 200.0
#define NR_OBE_LO 1.0      /* extra noise over-estimation at strength 1, per band */
#define NR_OBE_MID 0.3
#define NR_OBE_HI 0.8

struct E1Nr {
    double  fs;
    int     nch, nfft, hop, K;
    E1Wola *wola;
    float   pnorm;

    /* parameters */
    float strength_target, strength;
    float a_par;          /* per-frame smoothing of the strength */

    /* frame-rate constants */
    float a_s;            /* periodogram smoothing */
    float a_p;            /* presence probability smoothing */
    float a_d;            /* noise update */
    float a_dd;           /* decision-directed weight */
    float a_rel;          /* gain release */
    float delta;          /* presence threshold on S - Smin (natural log units) */
    float lcap;           /* log of the noise bound factor over exp(Smin) */
    float xi_min;
    int   V, sw_count, sw_idx, primed;

    /* per-bin state (K each) */
    float *P;                                    /* periodogram (linear) */
    float *S, *Ssub, *Smin, *swmin /* NR_U * K */; /* smoothed periodogram and minima (ln) */
    float *phat, *lambda, *A2prev, *Gs, *Gout;
    float *obe;           /* over-subtraction weight per bin (scaled by strength) */
    int   *hw;            /* frequency smoothing half width per bin */
    double *cum;          /* prefix sums for the frequency smoothing (K + 1) */
    void  *mem;
};

/* Exponential integral E1(x), x > 0 (Abramowitz & Stegun 5.1.53 / 5.1.56). */
static double nr_expint(double x)
{
    if (x <= 0.0)
        return 40.0;
    if (x <= 1.0) {
        return -log(x) - 0.57721566 +
               x * (0.99999193 + x * (-0.24991055 + x * (0.05519968 + x * (-0.00976004 + x * 0.00107857))));
    }
    if (x > 40.0)
        return 0.0;
    double num = x * x + 2.334733 * x + 0.250621;
    double den = x * x + 3.330657 * x + 1.681534;
    return exp(-x) / x * (num / den);
}

static float nr_coef(double tau_s, double frame_rate)
{
    return (float)exp(-1.0 / (tau_s * frame_rate));
}

/* raised-cosine weight that is 0 at/below f0 and 1 at/above f1 (log-frequency) */
static double nr_ramp(double f, double f0, double f1)
{
    if (f <= f0)
        return 0.0;
    if (f >= f1)
        return 1.0;
    double t = log(f / f0) / log(f1 / f0);
    return 0.5 - 0.5 * cos(NR_PI * t);
}

E1Nr *e1_nr_create(double fs, int nch)
{
    if (!(fs >= 8000.0 && fs <= 384000.0) || nch < 1 || nch > E1DSP_MAX_CH)
        return NULL;
    E1Nr *m = (E1Nr *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->fs = fs;
    m->nch = nch;
    int nfft = 256;
    while (nfft < 4096 && (double)nfft * 1.41421356 < fs * NR_FRAME_S)
        nfft <<= 1;
    m->nfft = nfft;
    m->hop = nfft / 4;
    m->K = nfft / 2 + 1;
    m->wola = e1_wola_create(nfft, m->hop, nch, E1DSP_BLOCK);
    const size_t K = (size_t)m->K;
    const size_t nfl = K * (10 + NR_U);
    const size_t bytes = nfl * sizeof(float) + K * sizeof(int) + (K + 1) * sizeof(double) + 64;
    m->mem = calloc(1, bytes);
    if (!m->wola || !m->mem) {
        e1_nr_destroy(m);
        return NULL;
    }
    double *dp = (double *)m->mem;
    m->cum = dp;
    float *p = (float *)(dp + K + 1);
    m->P = p; p += K;
    m->S = p; p += K;
    m->Ssub = p; p += K;
    m->Smin = p; p += K;
    m->phat = p; p += K;
    m->lambda = p; p += K;
    m->A2prev = p; p += K;
    m->Gs = p; p += K;
    m->Gout = p; p += K;
    m->obe = p; p += K;
    m->swmin = p; p += K * NR_U;
    m->hw = (int *)p;

    m->pnorm = e1_wola_power_norm(m->wola);
    const double fr = fs / (double)m->hop;  /* frames per second */
    m->a_par = nr_coef(0.05, fr);
    m->a_s = nr_coef(0.025, fr);
    m->a_p = nr_coef(0.02, fr);
    m->a_d = nr_coef(0.12, fr);
    m->a_dd = 0.93f;
    m->a_rel = nr_coef(0.012, fr);
    m->delta = (float)(NR_DELTA_DB * 0.230258509);  /* dB -> ln */
    m->lcap = (float)(NR_CAP_DB * 0.230258509);
    m->xi_min = 0.0031623f; /* -25 dB */

    m->V = (int)floor(1.0 * fr / NR_U + 0.5);
    if (m->V < 4)
        m->V = 4;

    const double binhz = fs / (double)nfft;
    for (int k = 0; k < m->K; ++k) {
        double f = k * binhz;
        double hi = nr_ramp(f, 3000.0, 5000.0);
        double lo = 1.0 - nr_ramp(f > 1.0 ? f : 1.0, NR_LO_F0, NR_LO_F1);
        double mid = 1.0 - hi - lo;
        if (mid < 0.0)
            mid = 0.0;
        /* power over-subtraction at strength 1 (rumble / speech / high band) */
        m->obe[k] = (float)(NR_OBE_LO * lo + NR_OBE_MID * mid + NR_OBE_HI * hi);
        /* half-width in bins: 0 below 1 kHz, about a quarter ERB above */
        int h = 0;
        if (f > 1000.0) {
            double erb = 24.7 * (4.37 * f / 1000.0 + 1.0);
            h = (int)floor(0.25 * erb / binhz + 0.5);
            if (h > 8)
                h = 8;
        }
        m->hw[k] = h;
    }
    m->strength_target = m->strength = 0.5f;
    e1_nr_reset(m);
    return m;
}

void e1_nr_destroy(E1Nr *m)
{
    if (!m)
        return;
    e1_wola_destroy(m->wola);
    free(m->mem);
    free(m);
}

void e1_nr_reset(E1Nr *m)
{
    if (!m)
        return;
    e1_wola_reset(m->wola);
    const size_t K = (size_t)m->K;
    memset(m->P, 0, K * sizeof(float));
    memset(m->S, 0, K * sizeof(float));
    memset(m->Ssub, 0, K * sizeof(float));
    memset(m->Smin, 0, K * sizeof(float));
    memset(m->swmin, 0, K * NR_U * sizeof(float));
    memset(m->phat, 0, K * sizeof(float));
    memset(m->lambda, 0, K * sizeof(float));
    memset(m->A2prev, 0, K * sizeof(float));
    for (size_t k = 0; k < K; ++k) {
        m->Gs[k] = 1.0f;
        m->Gout[k] = 1.0f;
    }
    m->sw_count = 0;
    m->sw_idx = 0;
    m->primed = 0;
    m->strength = m->strength_target;
}

void e1_nr_set(E1Nr *m, float strength_0_1)
{
    if (!m)
        return;
    float s = strength_0_1;
    if (!(s == s))
        s = 0.5f;
    if (s < 0.0f)
        s = 0.0f;
    if (s > 1.0f)
        s = 1.0f;
    m->strength_target = s;
}

unsigned e1_nr_latency(const E1Nr *m) { return m ? e1_wola_latency(m->wola) : 0u; }

static void nr_apply(E1Nr *m, float *const *spec, int nch)
{
    const int K = m->K;
    for (int c = 0; c < nch; ++c) {
        float *X = spec[c];
        for (int k = 0; k < K; ++k) {
            X[2 * k] *= m->Gout[k];
            X[2 * k + 1] *= m->Gout[k];
        }
    }
}

static void nr_frame(void *user, float *const *spec, const float *const *frame, int nch)
{
    E1Nr *m = (E1Nr *)user;
    (void)frame;
    const int K = m->K;

    /* parameter smoothing (per frame) */
    m->strength += (1.0f - m->a_par) * (m->strength_target - m->strength);
    if (fabsf(m->strength - m->strength_target) < 1e-5f)
        m->strength = m->strength_target;
    const float s = m->strength;
    const float gmin = (float)pow(10.0, -(6.0 + 18.0 * s) / 20.0);

    /* channel-averaged power spectrum */
    const float pn = m->pnorm / (float)nch;
    double tot = 0.0;
    for (int k = 0; k < K; ++k) {
        float p = 0.0f;
        for (int c = 0; c < nch; ++c) {
            const float re = spec[c][2 * k], im = spec[c][2 * k + 1];
            p += re * re + im * im;
        }
        p *= pn;
        m->P[k] = p;
        tot += p;
    }
    /* digital silence (below -130 dBFS per bin): freeze the statistics, keep the last gains */
    if (tot < (double)K * 1e-13) {
        nr_apply(m, spec, nch);
        return;
    }

    float *P = m->P, *S = m->S, *Smin = m->Smin, *Ssub = m->Ssub;
    if (!m->primed) {
        for (int k = 0; k < K; ++k) {
            float pk = P[k] > NR_FLOOR ? P[k] : NR_FLOOR;
            float lp = logf(pk);
            S[k] = Ssub[k] = Smin[k] = lp;
            for (int u = 0; u < NR_U; ++u)
                m->swmin[u * K + k] = lp;
            m->lambda[k] = pk;
            m->A2prev[k] = pk;
            m->phat[k] = 0.0f;
            m->Gs[k] = 1.0f;
        }
        m->primed = 1;
    }

    /* 1) smoothed periodogram: 3-bin frequency window, then first-order recursion on ln */
    const float as = m->a_s;
    for (int k = 0; k < K; ++k) {
        float pl = P[k > 0 ? k - 1 : k + 1];
        float pr = P[k < K - 1 ? k + 1 : k - 1];
        float sf = 0.25f * pl + 0.5f * P[k] + 0.25f * pr;
        float lsf = sf > NR_FLOOR ? logf(sf) : NR_LFLOOR;
        S[k] = as * S[k] + (1.0f - as) * lsf;
    }

    /* 2) minimum statistics over NR_U sub-windows of V frames */
    for (int k = 0; k < K; ++k)
        if (S[k] < Ssub[k])
            Ssub[k] = S[k];
    if (++m->sw_count >= m->V) {
        memcpy(m->swmin + (size_t)m->sw_idx * K, Ssub, sizeof(float) * (size_t)K);
        m->sw_idx = (m->sw_idx + 1) % NR_U;
        memcpy(Ssub, S, sizeof(float) * (size_t)K);
        m->sw_count = 0;
    }
    for (int k = 0; k < K; ++k) {
        float mn = Ssub[k];
        for (int u = 0; u < NR_U; ++u) {
            float v = m->swmin[u * K + k];
            if (v < mn)
                mn = v;
        }
        Smin[k] = mn;
    }

    /* 3) speech presence probability and noise update */
    const float ap = m->a_p, ad = m->a_d, delta = m->delta, lcap = m->lcap;
    for (int k = 0; k < K; ++k) {
        float ind = (S[k] > Smin[k] + delta) ? 1.0f : 0.0f;
        float ph = ap * m->phat[k] + (1.0f - ap) * ind;
        m->phat[k] = ph;
        float adk = ad + (1.0f - ad) * ph;
        float l = adk * m->lambda[k] + (1.0f - adk) * P[k];
        float cap = expf(Smin[k] + lcap);
        if (l > cap)
            l = cap;
        m->lambda[k] = l > NR_FLOOR ? l : NR_FLOOR;
    }

    /* 4) gain: decision-directed a-priori SNR, LSA gain, presence weighting, floor */
    const float add = m->a_dd, xi_min = m->xi_min, arel = m->a_rel;
    for (int k = 0; k < K; ++k) {
        const float ln = m->lambda[k] * (1.0f + s * m->obe[k]);
        float gamma = P[k] / ln;
        if (gamma > 1e6f)
            gamma = 1e6f;
        float xi = add * (m->A2prev[k] / ln) + (1.0f - add) * (gamma > 1.0f ? gamma - 1.0f : 0.0f);
        if (xi < xi_min)
            xi = xi_min;
        if (xi > 1e6f)
            xi = 1e6f;
        const double r = (double)xi / (1.0 + (double)xi);
        /* LSA factor exp(E1(v)/2) bounded to <= 2.5 (v >= 0.1): for gamma << 1 the unbounded
           factor gives large gains on near-empty bins; harmless alone, but the release
           smoothing below would carry those gains into the next frames (musical noise). */
        double v = r * (double)gamma;
        if (v < 0.1)
            v = 0.1;
        double g = r * exp(0.5 * nr_expint(v));
        if (g > 1.0)
            g = 1.0;
        if (g < (double)gmin)
            g = (double)gmin;
        m->A2prev[k] = (float)(g * g) * P[k];
        /* temporal smoothing: rise immediately, fall with ~12 ms */
        float gs = (float)g;
        if (gs < m->Gs[k])
            gs = arel * m->Gs[k] + (1.0f - arel) * gs;
        m->Gs[k] = gs;
    }

    /* 5) ERB-proportional frequency smoothing of the gain */
    double acc = 0.0;
    m->cum[0] = 0.0;
    for (int k = 0; k < K; ++k) {
        acc += (double)m->Gs[k];
        m->cum[k + 1] = acc;
    }
    for (int k = 0; k < K; ++k) {
        const int h = m->hw[k];
        if (h == 0) {
            m->Gout[k] = m->Gs[k];
            continue;
        }
        int a = k - h, b = k + h;
        if (a < 0)
            a = 0;
        if (b > K - 1)
            b = K - 1;
        float g = (float)((m->cum[b + 1] - m->cum[a]) / (double)(b - a + 1));
        m->Gout[k] = g < gmin ? gmin : (g > 1.0f ? 1.0f : g);
    }
    m->Gout[0] = m->Gout[1];
    m->Gout[K - 1] = m->Gout[K - 2];

    nr_apply(m, spec, nch);
}

void e1_nr_process(E1Nr *m, float *const *ch, int nch, int n)
{
    if (!m || !ch || n <= 0)
        return;
    if (nch > m->nch)
        nch = m->nch;
    e1_wola_process(m->wola, ch, nch, n, nr_frame, m);
}
