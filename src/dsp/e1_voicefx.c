/*
 * e1_voicefx.c - VoiceFX voice transformation (spec 3.9; owner: dsp-voice).
 *
 * Parameters (E1CaptureParams.vfx[], e1fx_params.h):
 *   [0..2] F1/F2/F3 formant targets (Hz). The spectral envelope is warped piecewise-linearly
 *          so that 400 -> F1, 1400 -> F2, 2000 -> F3 (0 and >= 5 kHz fixed). 400/1400/2000 = identity.
 *   [3]    PitchMod 0.25..4: pitch ratio (formants preserved).
 *   [4]    EnvelopeMod 0.5..2: vocal-tract length factor; the envelope is scaled by 1/EnvelopeMod
 *          (2 = formants an octave lower, 0.6 = formants 1.67x higher).
 *   [5]    PitchVariability -1..5: intonation scaling around the speaker's running mean pitch,
 *          f0_out / f0_in = PitchMod * (f0_in / f0_mean)^(var - 1). 1 = natural, 0 = monotone,
 *          < 0 = inverted intonation, > 1 = exaggerated.
 *   [6]    Quiver 0..0.2: depth (octaves) of a ~5.5 Hz, slightly irregular pitch tremor.
 *   [7]    Contour 0..0.4: depth (octaves) of a rise-and-fall pitch glide (1.5 Hz) restarted at
 *          every voiced onset ("sing-song" melody).
 *
 * Algorithm (clean-room, from the literature):
 *   - 2048-point sqrt-Hann STFT at 44.1/48 kHz (scaled with fs), 87.5 % overlap (hop nfft/8).
 *   - Pitch scaling by phase-locked peak shifting (Laroche & Dolson 1999): spectral peaks and
 *     their regions of influence are moved as blocks to the scaled frequency; each region is
 *     rotated by an accumulated phase (r - 1) * omega_peak * hop, where omega_peak is the peak's
 *     instantaneous frequency measured from the inter-frame phase advance, so the output
 *     partial runs at exactly r * omega. Bins inside a region keep their phase relation to the
 *     peak (identity phase locking).
 *   - Spectral envelope by real-cepstrum smoothing (lifter ~1.6 ms). Each moved bin is
 *     re-weighted by target_envelope(dest) / envelope(src), which preserves the formants under
 *     pitch scaling and applies the formant warp / envelope scaling.
 *   - Pitch tracking (only when variability != 1 or contour > 0): YIN (de Cheveigne & Kawahara
 *     2002) on a ~11 kHz decimated mono mix, 25 ms window, 60..600 Hz, median-of-3 smoothing.
 *   - Neutral settings bypass all spectral processing: output = input delayed (float-exact WOLA).
 * Stereo is processed linked (common peaks, rotations and envelope), preserving the image.
 * Latency: nfft - 128 frames with 128-frame blocks (1920 at 44.1 kHz = 43.5 ms).
 */
#include "e1dsp_modules.h"
#include "e1_biquad.h"
#include "e1_fft.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VFX_PI     3.14159265358979323846
#define VFX_TWO_PI 6.28318530717958647692
#define VFX_NP     E1FX_VFX_PARAMS

/* ---- preset table (docs/re/control-panel.md 4.3; parameter values only) ------------------ */
const E1VfxPreset e1_vfx_presets[] = {
    {"Neutral",           {400.0f, 1400.0f, 2000.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f}},
    {"Female to Male",    {400.0f, 1400.0f, 2000.0f, 0.60f, 1.05f, 1.0f, 0.0f, 0.0f}},
    {"Male to Female",    {400.0f, 1400.0f, 2220.0f, 2.37f, 0.92f, 1.0f, 0.0f, 0.0f}},
    {"Scrappy Kid",       {400.0f, 1400.0f, 2000.0f, 3.00f, 0.66f, 1.0f, 0.0f, 0.0f}},
    {"Elderly",           {713.0f, 1500.0f, 1800.0f, 1.40f, 1.45f, 1.0f, 0.1818f, 0.0f}},
    {"Orc",               {468.0f, 1321.0f, 2200.0f, 0.65f, 1.50f, 1.0f, 0.0f, 0.0f}},
    {"Elf",               {398.0f, 1395.0f, 2451.0f, 1.11f, 0.96f, 1.0f, 0.0f, 0.0f}},
    {"Dwarf",             {294.0f, 1527.0f, 2055.0f, 0.77f, 0.97f, 1.0f, 0.0f, 0.0f}},
    {"Alien Brute",       {383.54f, 1428.50f, 2550.0f, 0.65f, 1.3113f, 1.0f, 0.03f, 0.0f}},
    {"Alien Infiltrator", {200.0f, 1370.50f, 2000.0f, 1.24f, 1.06f, 1.0f, 0.13f, 0.0f}},
    {"Robot",             {400.0f, 1400.0f, 2000.0f, 1.39409f, 1.0f, -0.00824f, 0.0f, 0.0f}},
    {"Marine",            {388.0f, 1155.0f, 1848.0f, 0.80f, 1.08f, 0.54f, 0.0f, 0.0f}},
    {"Emo",               {400.0f, 1400.0f, 2000.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f}},
    {"Unstable",          {400.0f, 1400.0f, 2000.0f, 2.37f, 0.92f, 2.5f, 0.0f, 0.0f}},
    {"From Up North",     {400.0f, 1400.0f, 2000.0f, 1.0f, 1.0f, -0.20661f, 0.0f, 0.26446f}},
    {"Deep Voice",        {339.54f, 1362.50f, 2046.0f, 0.42912f, 1.20132f, 1.0f, 0.0f, 0.0f}},
    {"Munchkin",          {400.0f, 1400.0f, 2000.0f, 1.0f, 0.60163f, 1.0f, 0.0f, 0.0f}},
    {"Demon",             {453.0f, 1544.0f, 2600.0f, 0.69f, 2.00f, 1.0f, 0.0f, 0.0f}},
};
const int e1_vfx_preset_count = (int)(sizeof(e1_vfx_presets) / sizeof(e1_vfx_presets[0]));

static const float vfx_def[VFX_NP] = {400.0f, 1400.0f, 2000.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f};
static const float vfx_min[VFX_NP] = {200.0f, 800.0f, 1600.0f, 0.25f, 0.5f, -1.0f, 0.0f, 0.0f};
static const float vfx_max[VFX_NP] = {800.0f, 1800.0f, 2750.0f, 4.0f, 2.0f, 5.0f, 0.2f, 0.4f};

/* envelope ratio limits (natural log of amplitude): -60 dB .. +24 dB */
#define VFX_LR_MIN (-6.9078f)
#define VFX_LR_MAX (2.7631f)

struct E1VoiceFx {
    double  fs;
    int     nch, nfft, hop, K;
    E1Wola *wola;
    E1Fft  *fft;          /* envelope (cepstrum) transforms */

    float tgt[VFX_NP], cur[VFX_NP];
    float a_par;

    /* spectral work (K = nfft/2 + 1) */
    float *Pm, *LE, *LEt, *lifter;
    float *cbuf;          /* nfft + 2 */
    float *cep;           /* nfft */
    float *Y;             /* nfft + 2 */
    float *prevX[E1DSP_MAX_CH];
    float *thetaPrev, *thetaCur, *gr, *gi;
    int   *dst, *peaks;
    int    theta_zero;

    /* pitch tracker */
    int       D, dcnt, W, tmin, tmax, rsize, rpos;
    double    fsd;
    E1BqCoef  lpc[2];
    E1BqState lps[2];
    float    *mono, *ring, *ybuf, *dbuf;
    float     f0hist[3];
    int       nhist, voiced, was_voiced, have_mean;
    double    lmean;
    float     a_mean, a_lv_on, a_lv_off, a_cont;
    float     lv_s, cont_s;
    double    cont_t;

    /* quiver modulator */
    double   q_phase;
    float    q_jit;
    uint32_t rng;

    void *mem;
};

static float vfx_coef(double tau_s, double rate) { return (float)exp(-1.0 / (tau_s * rate)); }

static double vfx_princarg(double x) { return x - VFX_TWO_PI * floor(x / VFX_TWO_PI + 0.5); }

static float vfx_sanitize(int i, float v)
{
    if (!(v == v) || v > 1e30f || v < -1e30f)
        return vfx_def[i];
    if (v < vfx_min[i])
        v = vfx_min[i];
    if (v > vfx_max[i])
        v = vfx_max[i];
    return v;
}

E1VoiceFx *e1_vfx_create(double fs, int nch)
{
    if (!(fs >= 8000.0 && fs <= 384000.0) || nch < 1 || nch > E1DSP_MAX_CH)
        return NULL;
    E1VoiceFx *m = (E1VoiceFx *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->fs = fs;
    m->nch = nch;
    int nfft = 512;
    while (nfft < 16384 && (double)nfft * 1.41421356 < fs * 0.0464)
        nfft <<= 1;
    m->nfft = nfft;
    m->hop = nfft / 8;
    m->K = nfft / 2 + 1;
    m->wola = e1_wola_create(nfft, m->hop, nch, E1DSP_BLOCK);
    m->fft = e1_fft_create(nfft);

    /* pitch tracker geometry */
    m->D = (int)floor(fs / 11025.0 + 0.5);
    if (m->D < 1)
        m->D = 1;
    m->fsd = fs / (double)m->D;
    m->W = (int)floor(0.025 * m->fsd + 0.5);
    m->tmax = (int)ceil(m->fsd / 60.0);
    m->tmin = (int)floor(m->fsd / 600.0);
    if (m->tmin < 2)
        m->tmin = 2;
    m->rsize = 1;
    while (m->rsize < 2 * (m->W + m->tmax + 2))
        m->rsize <<= 1;

    const size_t K = (size_t)m->K, N = (size_t)nfft;
    const size_t nfl = K * 8 + (N + 2) * 2 + N + (N + 2) * (size_t)nch + (size_t)m->hop +
                       (size_t)m->rsize + (size_t)(m->W + m->tmax + 1) + (size_t)(m->tmax + 2);
    const size_t bytes = nfl * sizeof(float) + 2 * K * sizeof(int) + 64;
    m->mem = calloc(1, bytes);
    if (!m->wola || !m->fft || !m->mem) {
        e1_vfx_destroy(m);
        return NULL;
    }
    float *p = (float *)m->mem;
    m->Pm = p; p += K;
    m->LE = p; p += K;
    m->LEt = p; p += K;
    m->lifter = p; p += K;
    m->thetaPrev = p; p += K;
    m->thetaCur = p; p += K;
    m->gr = p; p += K;
    m->gi = p; p += K;
    m->cbuf = p; p += N + 2;
    m->Y = p; p += N + 2;
    m->cep = p; p += N;
    for (int c = 0; c < nch; ++c) {
        m->prevX[c] = p;
        p += N + 2;
    }
    m->mono = p; p += m->hop;
    m->ring = p; p += m->rsize;
    m->ybuf = p; p += m->W + m->tmax + 1;
    m->dbuf = p; p += m->tmax + 2;
    m->dst = (int *)p;
    m->peaks = m->dst + K;

    /* cepstral lifter: flat to 0.6 Q, raised-cosine to zero at Q (Q ~ 1.6 ms, below the
       shortest pitch period of interest so harmonics do not leak into the envelope) */
    {
        int Q = (int)floor(0.0016 * fs + 0.5);
        if (Q < 8)
            Q = 8;
        if (Q > (int)K - 1)
            Q = (int)K - 1;
        int Q0 = (int)(0.6 * Q);
        for (int q = 0; q < (int)K; ++q) {
            double v;
            if (q <= Q0)
                v = 1.0;
            else if (q >= Q)
                v = 0.0;
            else
                v = 0.5 + 0.5 * cos(VFX_PI * (double)(q - Q0) / (double)(Q - Q0));
            m->lifter[q] = (float)v;
        }
    }

    /* decimation low-pass: 4th-order Butterworth at 0.25 * fsd */
    if (m->D > 1) {
        e1_bq_lowpass(&m->lpc[0], fs, 0.25 * m->fsd, 0.54119610);
        e1_bq_lowpass(&m->lpc[1], fs, 0.25 * m->fsd, 1.30656296);
    } else {
        e1_bq_identity(&m->lpc[0]);
        e1_bq_identity(&m->lpc[1]);
    }

    const double fr = fs / (double)m->hop;
    m->a_par = vfx_coef(0.04, fr);
    m->a_mean = vfx_coef(2.0, fr);
    m->a_lv_on = vfx_coef(0.015, fr);
    m->a_lv_off = vfx_coef(0.06, fr);
    m->a_cont = vfx_coef(0.03, fr);

    for (int i = 0; i < VFX_NP; ++i)
        m->tgt[i] = m->cur[i] = vfx_def[i];
    e1_vfx_reset(m);
    return m;
}

void e1_vfx_destroy(E1VoiceFx *m)
{
    if (!m)
        return;
    e1_wola_destroy(m->wola);
    e1_fft_destroy(m->fft);
    free(m->mem);
    free(m);
}

void e1_vfx_reset(E1VoiceFx *m)
{
    if (!m)
        return;
    e1_wola_reset(m->wola);
    const size_t K = (size_t)m->K, N = (size_t)m->nfft;
    for (int c = 0; c < m->nch; ++c)
        memset(m->prevX[c], 0, (N + 2) * sizeof(float));
    memset(m->thetaPrev, 0, K * sizeof(float));
    memset(m->thetaCur, 0, K * sizeof(float));
    m->theta_zero = 1;
    memset(m->ring, 0, (size_t)m->rsize * sizeof(float));
    memset(m->lps, 0, sizeof(m->lps));
    m->rpos = 0;
    m->dcnt = 0;
    m->nhist = 0;
    m->voiced = m->was_voiced = 0;
    m->have_mean = 0;
    m->lmean = 0.0;
    m->lv_s = 0.0f;
    m->cont_s = 0.0f;
    m->cont_t = 0.0;
    m->q_phase = 0.0;
    m->q_jit = 0.0f;
    m->rng = 0x9E3779B9u;
    for (int i = 0; i < VFX_NP; ++i)
        m->cur[i] = m->tgt[i];
}

void e1_vfx_set(E1VoiceFx *m, const float p[E1FX_VFX_PARAMS])
{
    if (!m)
        return;
    for (int i = 0; i < VFX_NP; ++i)
        m->tgt[i] = vfx_sanitize(i, p ? p[i] : vfx_def[i]);
}

unsigned e1_vfx_latency(const E1VoiceFx *m) { return m ? e1_wola_latency(m->wola) : 0u; }

/* ---- pitch tracker ---------------------------------------------------------------------- */

static void vfx_feed_tracker(E1VoiceFx *m, const float *const *frame, int nch)
{
    const int hop = m->hop, off = m->nfft - hop;
    const float g = 1.0f / (float)nch;
    for (int i = 0; i < hop; ++i) {
        float s = 0.0f;
        for (int c = 0; c < nch; ++c)
            s += frame[c][off + i];
        m->mono[i] = s * g;
    }
    e1_bq_run(&m->lpc[0], &m->lps[0], m->mono, hop);
    e1_bq_run(&m->lpc[1], &m->lps[1], m->mono, hop);
    const int mask = m->rsize - 1;
    for (int i = 0; i < hop; ++i) {
        if (m->dcnt == 0) {
            m->ring[m->rpos] = m->mono[i];
            m->rpos = (m->rpos + 1) & mask;
        }
        if (++m->dcnt >= m->D)
            m->dcnt = 0;
    }
}

/* YIN on the newest W + tmax decimated samples. Returns f0 in Hz or 0 when unvoiced. */
static float vfx_yin(E1VoiceFx *m)
{
    const int W = m->W, tmax = m->tmax, L = W + tmax, mask = m->rsize - 1;
    float *x = m->ybuf, *d = m->dbuf;
    int rp = (m->rpos - L) & mask;
    double e = 0.0;
    for (int i = 0; i < L; ++i) {
        x[i] = m->ring[rp];
        rp = (rp + 1) & mask;
    }
    for (int i = 0; i < W; ++i)
        e += (double)x[i] * x[i];
    if (e / W < 1e-7)
        return 0.0f;
    d[0] = 1.0f;
    double run = 0.0;
    for (int t = 1; t <= tmax; ++t) {
        float acc = 0.0f;
        const float *a = x, *b = x + t;
        for (int j = 0; j < W; ++j) {
            const float df = a[j] - b[j];
            acc += df * df;
        }
        run += acc;
        d[t] = run > 0.0 ? (float)(acc * (double)t / run) : 1.0f;
    }
    int best = -1;
    for (int t = m->tmin; t <= tmax; ++t) {
        if (d[t] < 0.15f) {
            while (t + 1 <= tmax && d[t + 1] < d[t])
                ++t;
            best = t;
            break;
        }
    }
    if (best < 0) {
        float mn = 1e30f;
        for (int t = m->tmin; t <= tmax; ++t)
            if (d[t] < mn) {
                mn = d[t];
                best = t;
            }
        if (best < 0 || mn > 0.35f)
            return 0.0f;
    }
    double tau = (double)best;
    if (best > 1 && best < tmax) {
        double a = d[best - 1], b = d[best], c = d[best + 1];
        double den = a - 2.0 * b + c;
        if (den > 1e-12) {
            double off = 0.5 * (a - c) / den;
            if (off > -1.0 && off < 1.0)
                tau += off;
        }
    }
    return (float)(m->fsd / tau);
}

static float vfx_median3(const float *h, int n)
{
    if (n == 1)
        return h[0];
    if (n == 2)
        return 0.5f * (h[0] + h[1]);
    float a = h[0], b = h[1], c = h[2];
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { float t = a; a = b; b = t; }
    return b;
}

static float vfx_rand(E1VoiceFx *m) /* uniform in [-1, 1) */
{
    m->rng = m->rng * 1664525u + 1013904223u;
    return (float)((double)(m->rng >> 8) * (1.0 / 8388608.0) - 1.0);
}

/* ---- per-frame processing --------------------------------------------------------------- */

static void vfx_smooth_params(E1VoiceFx *m)
{
    for (int i = 0; i < VFX_NP; ++i) {
        float t = m->tgt[i];
        float v = t + m->a_par * (m->cur[i] - t);
        float tol = 1e-6f * (fabsf(t) > 1.0f ? fabsf(t) : 1.0f);
        m->cur[i] = (fabsf(v - t) <= tol) ? t : v;
    }
}

/* pitch ratio for this frame */
static double vfx_ratio(E1VoiceFx *m, const float *const *frame, int nch)
{
    const float var = m->cur[5], quiver = m->cur[6], contour = m->cur[7];
    vfx_feed_tracker(m, frame, nch);
    const int track = (var != 1.0f) || (contour > 0.0f);
    float f0 = 0.0f;
    if (track) {
        float f = vfx_yin(m);
        if (f > 0.0f) {
            if (m->nhist < 3)
                m->f0hist[m->nhist++] = f;
            else {
                m->f0hist[0] = m->f0hist[1];
                m->f0hist[1] = m->f0hist[2];
                m->f0hist[2] = f;
            }
            f0 = vfx_median3(m->f0hist, m->nhist);
        } else {
            m->nhist = 0;
        }
    } else {
        m->nhist = 0;
    }
    m->was_voiced = m->voiced;
    m->voiced = f0 > 0.0f;

    /* intonation scaling */
    if (m->voiced) {
        double l = log2((double)f0);
        if (!m->have_mean) {
            m->lmean = l;
            m->have_mean = 1;
        } else {
            m->lmean += (1.0 - m->a_mean) * (l - m->lmean);
        }
        double t = ((double)var - 1.0) * (l - m->lmean);
        if (t > 2.0)
            t = 2.0;
        if (t < -2.0)
            t = -2.0;
        m->lv_s = (float)(m->a_lv_on * m->lv_s + (1.0f - m->a_lv_on) * t);
    } else {
        m->lv_s *= m->a_lv_off;
    }
    if (fabsf(m->lv_s) < 1e-7f)
        m->lv_s = 0.0f;

    /* contour: rise-and-fall glide restarted at voiced onsets */
    float ct = 0.0f;
    if (m->voiced && contour > 0.0f) {
        if (!m->was_voiced)
            m->cont_t = 0.0;
        m->cont_t += (double)m->hop / m->fs;
        ct = (float)sin(VFX_TWO_PI * 1.5 * m->cont_t);
    }
    m->cont_s = m->a_cont * m->cont_s + (1.0f - m->a_cont) * ct;
    if (fabsf(m->cont_s) < 1e-7f)
        m->cont_s = 0.0f;

    /* quiver: ~5.5 Hz tremor with a slowly wandering rate */
    float q = 0.0f;
    if (quiver > 0.0f) {
        m->q_jit += 0.05f * vfx_rand(m);
        if (m->q_jit > 1.0f)
            m->q_jit = 1.0f;
        if (m->q_jit < -1.0f)
            m->q_jit = -1.0f;
        const double fq = 5.5 * (1.0 + 0.25 * m->q_jit);
        m->q_phase += VFX_TWO_PI * fq * (double)m->hop / m->fs;
        if (m->q_phase > VFX_TWO_PI)
            m->q_phase -= VFX_TWO_PI;
        q = (float)sin(m->q_phase);
    }

    const double expo = (double)m->lv_s + (double)contour * m->cont_s + (double)quiver * q;
    double r = (double)m->cur[3];
    if (expo != 0.0)
        r *= exp2(expo);
    if (r < 0.2)
        r = 0.2;
    if (r > 5.0)
        r = 5.0;
    return r;
}

static int vfx_warp_identity(const E1VoiceFx *m)
{
    return m->cur[0] == 400.0f && m->cur[1] == 1400.0f && m->cur[2] == 2000.0f && m->cur[4] == 1.0f;
}

/* log-envelope LE[k] (natural log amplitude) of the channel-summed power by cepstral smoothing */
static void vfx_envelope(E1VoiceFx *m, float *const *spec, int nch)
{
    const int K = m->K, N = m->nfft;
    float mx = 0.0f;
    for (int k = 0; k < K; ++k) {
        float p = 0.0f;
        for (int c = 0; c < nch; ++c) {
            const float re = spec[c][2 * k], im = spec[c][2 * k + 1];
            p += re * re + im * im;
        }
        m->Pm[k] = p;
        if (p > mx)
            mx = p;
    }
    const float fl = mx * 1e-9f + 1e-30f;
    for (int k = 0; k < K; ++k) {
        m->cbuf[2 * k] = 0.5f * logf(m->Pm[k] + fl);
        m->cbuf[2 * k + 1] = 0.0f;
    }
    e1_fft_c2r(m->fft, m->cbuf, m->cep);
    m->cep[0] *= m->lifter[0];
    for (int q = 1; q < N / 2; ++q) {
        m->cep[q] *= m->lifter[q];
        m->cep[N - q] *= m->lifter[q];
    }
    m->cep[N / 2] *= m->lifter[N / 2];
    e1_fft_r2c(m->fft, m->cep, m->cbuf);
    for (int k = 0; k < K; ++k)
        m->LE[k] = m->cbuf[2 * k];
}

/* target envelope LEt[j] = LE(src(j)), src(f) = Winv(f * EnvelopeMod) */
static void vfx_target_envelope(E1VoiceFx *m)
{
    const int K = m->K;
    double fa[5] = {0.0, 400.0, 1400.0, 2000.0, 5000.0};
    double fb[5] = {0.0, m->cur[0], m->cur[1], m->cur[2], 5000.0};
    if (fb[2] < fb[1] + 10.0)
        fb[2] = fb[1] + 10.0;
    if (fb[3] < fb[2] + 10.0)
        fb[3] = fb[2] + 10.0;
    if (fb[3] > 4900.0)
        fb[3] = 4900.0;
    const double env = m->cur[4];
    const double hz_per_bin = m->fs / (double)m->nfft;
    for (int j = 0; j < K; ++j) {
        double f = (double)j * hz_per_bin * env;
        double src;
        if (f >= fb[4]) {
            src = f;
        } else {
            int s = 0;
            while (s < 3 && f >= fb[s + 1])
                ++s;
            src = fa[s] + (f - fb[s]) * (fa[s + 1] - fa[s]) / (fb[s + 1] - fb[s]);
        }
        double b = src / hz_per_bin;
        if (b > (double)(K - 1))
            b = (double)(K - 1);
        if (b < 0.0)
            b = 0.0;
        int i0 = (int)b;
        if (i0 >= K - 1) {
            m->LEt[j] = m->LE[K - 1];
        } else {
            float fr = (float)(b - i0);
            m->LEt[j] = m->LE[i0] + fr * (m->LE[i0 + 1] - m->LE[i0]);
        }
    }
}

static void vfx_frame(void *user, float *const *spec, const float *const *frame, int nch)
{
    E1VoiceFx *m = (E1VoiceFx *)user;
    const int K = m->K, N = m->nfft, hop = m->hop;

    vfx_smooth_params(m);
    const double r = vfx_ratio(m, frame, nch);
    const int warp_id = vfx_warp_identity(m);

    if (r == 1.0 && m->theta_zero && warp_id) {
        for (int c = 0; c < nch; ++c)
            memcpy(m->prevX[c], spec[c], sizeof(float) * (size_t)(N + 2));
        return; /* transparent */
    }

    vfx_envelope(m, spec, nch);
    const float *LEt = m->LE;
    if (!warp_id) {
        vfx_target_envelope(m);
        LEt = m->LEt;
    }

    if (r == 1.0 && m->theta_zero) {
        /* envelope warp only: a time-varying zero-phase filter */
        for (int c = 0; c < nch; ++c)
            memcpy(m->prevX[c], spec[c], sizeof(float) * (size_t)(N + 2));
        for (int k = 0; k < K; ++k) {
            float lr = LEt[k] - m->LE[k];
            lr = lr < VFX_LR_MIN ? VFX_LR_MIN : (lr > VFX_LR_MAX ? VFX_LR_MAX : lr);
            m->gr[k] = expf(lr);
        }
        for (int c = 0; c < nch; ++c) {
            float *X = spec[c];
            for (int k = 0; k < K; ++k) {
                X[2 * k] *= m->gr[k];
                X[2 * k + 1] *= m->gr[k];
            }
            X[1] = 0.0f;
            X[N + 1] = 0.0f;
        }
        return;
    }

    /* ---- peak picking on the channel-summed power (Pm filled by vfx_envelope) ---- */
    float mx = 0.0f;
    int amax = 1;
    for (int k = 1; k < K - 1; ++k)
        if (m->Pm[k] > mx) {
            mx = m->Pm[k];
            amax = k;
        }
    const float thr = mx * 1e-8f;
    int np = 0;
    for (int k = 1; k < K - 1; ++k) {
        const float v = m->Pm[k];
        if (v > thr && v > m->Pm[k - 1] && v >= m->Pm[k + 1])
            m->peaks[np++] = k;
    }
    if (np == 0)
        m->peaks[np++] = amax;

    /* ---- per region: instantaneous frequency, shift, phase rotation, envelope ratio ---- */
    const double rm1 = r - 1.0;
    const double bin_w = VFX_TWO_PI / (double)N;
    float thmax = 0.0f;
    int a = 0;
    for (int i = 0; i < np; ++i) {
        const int p = m->peaks[i];
        int b;
        if (i + 1 < np) {
            const int pn = m->peaks[i + 1];
            b = p;
            float mn = m->Pm[p];
            for (int k = p + 1; k < pn; ++k)
                if (m->Pm[k] < mn) {
                    mn = m->Pm[k];
                    b = k;
                }
        } else {
            b = K - 1;
        }
        /* instantaneous frequency from the summed cross-spectrum with the previous frame */
        double cr = 0.0, ci = 0.0;
        for (int c = 0; c < nch; ++c) {
            const double xr = spec[c][2 * p], xi = spec[c][2 * p + 1];
            const double pr = m->prevX[c][2 * p], pi = m->prevX[c][2 * p + 1];
            cr += xr * pr + xi * pi;
            ci += xi * pr - xr * pi;
        }
        double omega = bin_w * (double)p;
        if (cr != 0.0 || ci != 0.0) {
            const double dev = vfx_princarg(atan2(ci, cr) - omega * (double)hop);
            omega += dev / (double)hop;
        }
        const double kf = omega / bin_w;
        const int s = (int)lrint(rm1 * kf);
        double th;
        if (r == 1.0)
            th = 0.7 * (double)m->thetaPrev[p]; /* glide back to zero rotation */
        else
            th = vfx_princarg((double)m->thetaPrev[p] + rm1 * omega * (double)hop);
        const float thf = (float)th;
        if (fabsf(thf) > thmax)
            thmax = fabsf(thf);
        const float cs = (float)cos(th), sn = (float)sin(th);
        for (int k = a; k <= b; ++k) {
            m->thetaCur[k] = thf;
            const int j = k + s;
            if (j < 0 || j >= K) {
                m->dst[k] = -1;
                continue;
            }
            float lr = LEt[j] - m->LE[k];
            lr = lr < VFX_LR_MIN ? VFX_LR_MIN : (lr > VFX_LR_MAX ? VFX_LR_MAX : lr);
            const float g = expf(lr);
            m->dst[k] = j;
            m->gr[k] = g * cs;
            m->gi[k] = g * sn;
        }
        a = b + 1;
    }

    /* ---- resynthesis spectra ---- */
    for (int c = 0; c < nch; ++c) {
        float *X = spec[c];
        memcpy(m->prevX[c], X, sizeof(float) * (size_t)(N + 2));
        float *Y = m->Y;
        memset(Y, 0, sizeof(float) * (size_t)(N + 2));
        const float *Xs = m->prevX[c];
        for (int k = 0; k < K; ++k) {
            const int j = m->dst[k];
            if (j < 0)
                continue;
            const float xr = Xs[2 * k], xi = Xs[2 * k + 1];
            Y[2 * j] += xr * m->gr[k] - xi * m->gi[k];
            Y[2 * j + 1] += xr * m->gi[k] + xi * m->gr[k];
        }
        Y[1] = 0.0f;
        Y[N + 1] = 0.0f;
        memcpy(X, Y, sizeof(float) * (size_t)(N + 2));
    }

    /* rotations for the next frame */
    float *t = m->thetaPrev;
    m->thetaPrev = m->thetaCur;
    m->thetaCur = t;
    if (r == 1.0 && thmax < 1e-4f) {
        memset(m->thetaPrev, 0, sizeof(float) * (size_t)K);
        m->theta_zero = 1;
    } else {
        m->theta_zero = 0;
    }
}

void e1_vfx_process(E1VoiceFx *m, float *const *ch, int nch, int n)
{
    if (!m || !ch || n <= 0)
        return;
    if (nch > m->nch)
        nch = m->nch;
    e1_wola_process(m->wola, ch, nch, n, vfx_frame, m);
}
