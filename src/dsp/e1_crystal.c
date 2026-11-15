/*
 * e1_crystal.c - "Clarity" (Crystalizer-style): transient-driven dynamic LF/HF shelving expander
 * plus a level-dependent static loudness tilt.  Owner: dsp-spatial.  Spec 3.6, re/apo-dsp.md 5.2.
 *
 * Per channel:  y = gain * HFshelf( LFshelf( x ) ), first-order shelves in parallel form
 *   LFshelf(x) = x + (G_lf - 1) * LP(x),  HFshelf(x) = x + (G_hf - 1) * HP(x)
 * with topology-preserving (trapezoidal, zero-delay-feedback) one-poles, i.e. exactly the bilinear
 * shelf with a geometric-midpoint corner, but well behaved under per-sample parameter modulation
 * and an exact identity when G = 1.
 * Shelf gains in dB = static_tilt(level) + dynamic_boost(detector), recomputed every frame of
 * 32 samples (64/128/256 at higher rates so a frame stays ~0.7 ms), linearly interpolated per
 * sample and finally passed through a per-sample one-pole (0.36 ms) so the parameter trajectory
 * has neither steps nor slope corners at frame boundaries.
 *
 * Detector (mono sum of all channels, causal, zero latency): LF analysis = 2nd-order low-pass
 * 500 Hz, HF analysis = 2nd-order high-pass 6 kHz.  Per band a fast and a slow one-pole average of
 * the frame power (LF 20/250 ms so a bass period is integrated, HF 2/60 ms); the transient measure
 * is 0.5*log2(fast/slow) (one unit = 6 dB jump), with a floor 40 dB below the programme peak so
 * low-level noise never triggers.  Excess over the threshold times the sensitivity, capped at 0.5,
 * is the boost; it is smoothed with the per-level smoothing coefficient and mapped to a dynamic
 * shelf gain 1 + 2 * amount * boost.  Steady tones (any frequency) do not trigger it.
 *
 * Level map: x = L <= 0.5 ? 0.8 L : 0.4 + 1.2 (L - 0.5), interpolated over four rows (knots 0,
 * 0.38, 0.75, 1).  Row gains are taken relative to row 0 so level 0 is an exact identity.  The
 * shelf corner frequencies are the first-order corners implied by the original's per-row tilt
 * coefficients (our own designs at those corners).  Level changes: the derived filter parameters
 * (gain, shelf dB, log corner) follow the target through two cascaded one-pole smoothers
 * (~16 frames to settle), so the coefficient trajectory has no slope steps at the table knots.
 *
 * Loudness make-up: the static tilt alone would make the programme ~3 LU louder at the default
 * level, which biases every on/off comparison towards "on". The output gain is therefore not the
 * original's row gain but 1/sqrt(P(x)), where P(x) is the power gain of the static LF/HF tilt for
 * pink noise under BS.1770 K-weighting (20 Hz .. min(20 kHz, 0.45 fs)). P is tabulated over the
 * level map at create time (CR_MK_N points, linear interpolation), and is exactly 1 at level 0.
 * Only the transient-driven dynamic boost changes the loudness, and only on transients.
 */
#include "e1dsp_modules.h"
#include "e1_biquad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CR_GAIN, CR_FLF, CR_FHF, CR_AMT_LF, CR_AMT_HF, CR_SM_LF, CR_SM_HF, CR_SENS, CR_THR, CR_DB_LF, CR_DB_HF, CR_COLS };

static const double cr_knots[4] = {0.0, 0.38, 0.75, 1.0};
static const double cr_rows[4][CR_COLS] = {
    /* gain    fLF    fHF    amtLF   amtHF   smLF    smHF    sens    thr   LF dB  HF dB */
    {1.0417, 354.0, 2306.0, 0.0,    0.0,    0.1105, 0.3324, 1.2098, 0.45, 0.0,   0.0},
    {1.1000, 223.0, 2459.0, 0.1522, 0.6087, 0.0496, 0.1823, 0.9262, 0.45, 3.333, 2.800},
    {1.1000, 158.0, 2996.0, 0.2609, 0.8696, 0.0406, 0.0741, 0.7986, 0.50, 4.133, 3.600},
    {1.0000, 125.0, 3203.0, 0.3044, 1.1739, 0.0246, 0.0301, 0.7986, 0.20, 5.200, 5.466},
};
#define CR_REF_FRAME_RATE (44100.0 / 32.0) /* smoothing coefficients are per frame at this rate */
#define CR_MAX_FRAME 256
#define CR_MK_N 65                 /* loudness make-up table points over x = 0..1 */

/* Per-frame filter parameters: shelf gain minus one and TPT one-pole coefficient g/(1+g). */
typedef struct ShelfPar { double k_lf, a_lf, k_hf, a_hf, gain; } ShelfPar;

struct E1Crystal {
    double fs;
    int nch;
    int frame;
    int pos;
    double rate_ratio;           /* reference frame rate / actual frame rate */

    E1BqCoef ana_lf, ana_hf;
    E1BqState ana_lf_s, ana_hf_s;
    float mono[CR_MAX_FRAME], blf[CR_MAX_FRAME], bhf[CR_MAX_FRAME];
    double acc_lf, acc_hf, acc_pk;   /* LF/HF power sums, peak |x| */

    double fast[2], slow[2], boost[2], peak;
    double c_fast[2], c_slow[2], c_peak_rel;

    float tgt_level;
    double x_cur;                /* level-map value used by the detector parameters */
    double ps1[5], ps2[5];       /* smoothed static parameters (2 cascaded one-poles) */
    double c_par;

    ShelfPar p0, p1;             /* parameters at frame start / frame end */
    double cur[5];               /* per-sample smoothed parameters (k_lf, a_lf, k_hf, a_hf, gain) */
    double c_smooth;
    double parr[5][CR_MAX_FRAME];
    double mk[CR_MK_N];          /* loudness make-up gain over the level map */
    double s_lf[E1DSP_MAX_CH], s_hf[E1DSP_MAX_CH];   /* TPT integrator states */
};

static double level_map(float L)
{
    double l = (double)L;
    return (l <= 0.5) ? 0.8 * l : 0.4 + 1.2 * (l - 0.5);
}

static void row_interp(double x, double out[CR_COLS])
{
    int seg = 0;
    if (x > cr_knots[1]) seg = 1;
    if (x > cr_knots[2]) seg = 2;
    double t = (x - cr_knots[seg]) / (cr_knots[seg + 1] - cr_knots[seg]);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    for (int c = 0; c < CR_COLS; ++c) out[c] = cr_rows[seg][c] + t * (cr_rows[seg + 1][c] - cr_rows[seg][c]);
}

/* First-order shelves with the corner fc at the geometric midpoint of the transition (boost and
   cut mirror each other).  In the prewarped analog domain the LF shelf (s + W sqrt(G))/(s + W/sqrt(G))
   equals 1 + (G-1) * wp/(s + wp) with wp = W/sqrt(G); the HF shelf equals 1 + (G-1) * s/(s + wp)
   with wp = W*sqrt(G).  Returns the TPT coefficient g/(1+g) for that pole. */
static double shelf_tpt(double fs, double fc, double g_lin, int high)
{
    if (fc > 0.45 * fs) fc = 0.45 * fs;
    const double w = tan(E1_PI * fc / fs), r = sqrt(g_lin);
    const double g = high ? w * r : w / r;
    return g / (1.0 + g);
}

static double frame_coef(double per_ref_frame, double ratio)
{
    /* convert a per-reference-frame smoothing coefficient to the actual frame rate */
    return 1.0 - pow(1.0 - per_ref_frame, ratio);
}

static double tau_coef(double tau_s, double frames_per_s)
{
    return 1.0 - exp(-1.0 / (tau_s * frames_per_s));
}

/* smoothed static parameters */
enum { SP_GAIN, SP_LOGF_LF, SP_LOGF_HF, SP_DB_LF, SP_DB_HF, SP_COUNT };

/* |H(f)|^2 of the static tilt exactly as process_seg implements it (TPT one-poles = bilinear
   transform of the prewarped analog shelves; Om = tan(pi f / fs) is the prewarped frequency). */
static double tilt_pow(double fs, double f, double db_lf, double f_lf, double db_hf, double f_hf)
{
    const double om = tan(E1_PI * f / fs);
    const double g_lf = pow(10.0, db_lf / 20.0), g_hf = pow(10.0, db_hf / 20.0);
    const double flf = f_lf > 0.45 * fs ? 0.45 * fs : f_lf, fhf = f_hf > 0.45 * fs ? 0.45 * fs : f_hf;
    const double p_lf = tan(E1_PI * flf / fs) / sqrt(g_lf), p_hf = tan(E1_PI * fhf / fs) * sqrt(g_hf);
    /* LF: 1 + (G-1) p/(j om + p) ; HF: 1 + (G-1) j om/(j om + p) */
    double d = p_lf * p_lf + om * om;
    double re1 = 1.0 + (g_lf - 1.0) * p_lf * p_lf / d, im1 = -(g_lf - 1.0) * p_lf * om / d;
    d = p_hf * p_hf + om * om;
    double re2 = 1.0 + (g_hf - 1.0) * om * om / d, im2 = (g_hf - 1.0) * om * p_hf / d;
    const double re = re1 * re2 - im1 * im2, im = re1 * im2 + im1 * re2;
    return re * re + im * im;
}

/* BS.1770 K-weighting power (analog prototypes: +4 dB high shelf at 1682 Hz, HPF 38 Hz Q 0.5). */
static double kweight_pow(double f)
{
    const double A = pow(10.0, 4.0 / 40.0), q = 0.7071752369554193;
    double s = f / 1681.974450955533;           /* s = j s */
    /* A (A s^2 + sqrt(A)/q s + 1) / (s^2 + sqrt(A)/q s + A) at s = j*s */
    double nr = A * (1.0 - A * s * s), ni = A * sqrt(A) / q * s;
    double dr = A - s * s, di = sqrt(A) / q * s;
    const double shelf = (nr * nr + ni * ni) / (dr * dr + di * di);
    s = f / 38.13547087602444;
    const double q2 = 0.5003270373238773;
    dr = 1.0 - s * s; di = s / q2;
    const double hp = (s * s) * (s * s) / (dr * dr + di * di);
    return shelf * hp;
}

static void build_makeup(E1Crystal *m)
{
    const int NF = 240;
    const double f0 = 20.0, f1 = (0.45 * m->fs < 20000.0) ? 0.45 * m->fs : 20000.0;
    for (int i = 0; i < CR_MK_N; ++i) {
        const double x = (double)i / (double)(CR_MK_N - 1);
        double row[CR_COLS];
        row_interp(x, row);
        if (row[CR_DB_LF] == 0.0 && row[CR_DB_HF] == 0.0) { m->mk[i] = 1.0; continue; }
        double num = 0.0, den = 0.0;
        for (int k = 0; k < NF; ++k) {           /* pink: equal weight per log-frequency step */
            const double f = f0 * pow(f1 / f0, ((double)k + 0.5) / (double)NF);
            const double w = kweight_pow(f);
            num += w * tilt_pow(m->fs, f, row[CR_DB_LF], row[CR_FLF], row[CR_DB_HF], row[CR_FHF]);
            den += w;
        }
        m->mk[i] = 1.0 / sqrt(num / den);
    }
}

static double makeup_at(const E1Crystal *m, double x)
{
    if (!(x > 0.0)) return m->mk[0];
    double p = x * (double)(CR_MK_N - 1);
    int j = (int)p;
    if (j >= CR_MK_N - 1) return m->mk[CR_MK_N - 1];
    const double t = p - (double)j;
    return m->mk[j] + t * (m->mk[j + 1] - m->mk[j]);
}

static void static_targets(const E1Crystal *m, double x, double p[SP_COUNT])
{
    double row[CR_COLS];
    row_interp(x, row);
    p[SP_GAIN] = makeup_at(m, x);
    p[SP_LOGF_LF] = log(row[CR_FLF]);
    p[SP_LOGF_HF] = log(row[CR_FHF]);
    p[SP_DB_LF] = row[CR_DB_LF];
    p[SP_DB_HF] = row[CR_DB_HF];
}

static void compute_targets(E1Crystal *m)
{
    double row[CR_COLS];
    row_interp(m->x_cur, row);
    const double *p = m->ps2;
    const double dyn_lf = 1.0 + 2.0 * row[CR_AMT_LF] * m->boost[0];
    const double dyn_hf = 1.0 + 2.0 * row[CR_AMT_HF] * m->boost[1];
    const double g_lf = pow(10.0, p[SP_DB_LF] / 20.0) * dyn_lf;
    const double g_hf = pow(10.0, p[SP_DB_HF] / 20.0) * dyn_hf;
    m->p1.k_lf = g_lf - 1.0;
    m->p1.a_lf = shelf_tpt(m->fs, exp(p[SP_LOGF_LF]), g_lf, 0);
    m->p1.k_hf = g_hf - 1.0;
    m->p1.a_hf = shelf_tpt(m->fs, exp(p[SP_LOGF_HF]), g_hf, 1);
    m->p1.gain = p[SP_GAIN];
}

/* End of an analysis frame: update the detector, smooth the parameters, compute new targets. */
static void frame_end(E1Crystal *m)
{
    const double inv = 1.0 / (double)m->frame;
    const double e[2] = {m->acc_lf * inv, m->acc_hf * inv};
    const double pk = m->acc_pk;
    m->acc_lf = m->acc_hf = m->acc_pk = 0.0;

    m->peak = (pk > m->peak) ? pk : m->peak + m->c_peak_rel * (pk - m->peak);
    m->peak = e1_flush(m->peak);
    const double floor_ = 1e-4 * m->peak * m->peak + 1e-14;   /* power, -40 dB re programme peak */

    m->x_cur = level_map(m->tgt_level);
    double row[CR_COLS];
    row_interp(m->x_cur, row);

    for (int b = 0; b < 2; ++b) {
        m->fast[b] = e1_flush(m->fast[b] + m->c_fast[b] * (e[b] - m->fast[b]));
        m->slow[b] = e1_flush(m->slow[b] + m->c_slow[b] * (e[b] - m->slow[b]));
        const double meas = 0.5 * log2((m->fast[b] + floor_) / (m->slow[b] + floor_));
        double bt = (meas - row[CR_THR]) * row[CR_SENS];
        if (bt < 0.0) bt = 0.0;
        if (bt > 0.5) bt = 0.5;
        const double c = frame_coef(row[b == 0 ? CR_SM_LF : CR_SM_HF], m->rate_ratio);
        m->boost[b] = e1_flush(m->boost[b] + c * (bt - m->boost[b]));
    }

    double pt[SP_COUNT];
    static_targets(m, m->x_cur, pt);
    for (int k = 0; k < SP_COUNT; ++k) {
        m->ps1[k] += m->c_par * (pt[k] - m->ps1[k]);
        m->ps2[k] += m->c_par * (m->ps1[k] - m->ps2[k]);
        if (fabs(m->ps1[k] - pt[k]) < 1e-9 && fabs(m->ps2[k] - pt[k]) < 1e-9) m->ps1[k] = m->ps2[k] = pt[k];
    }

    m->p0 = m->p1;
    compute_targets(m);
}

static void process_seg(E1Crystal *m, float *const *ch, int nch, int off, int n)
{
    /* analysis on the mono sum */
    const float sc = 1.0f / (float)nch;
    for (int i = 0; i < n; ++i) {
        float s = 0.0f;
        for (int c = 0; c < nch; ++c) s += ch[c][off + i];
        m->mono[i] = s * sc;
    }
    memcpy(m->blf, m->mono, sizeof(float) * (size_t)n);
    memcpy(m->bhf, m->mono, sizeof(float) * (size_t)n);
    e1_bq_run(&m->ana_lf, &m->ana_lf_s, m->blf, n);
    e1_bq_run(&m->ana_hf, &m->ana_hf_s, m->bhf, n);
    for (int i = 0; i < n; ++i) {
        const double a = fabs((double)m->mono[i]);
        m->acc_lf += (double)m->blf[i] * (double)m->blf[i];
        m->acc_hf += (double)m->bhf[i] * (double)m->bhf[i];
        if (a > m->acc_pk) m->acc_pk = a;
    }

    /* signal path with per-sample coefficient interpolation inside the frame */
    const double inv = 1.0 / (double)m->frame;
    const double pa[5] = {m->p0.k_lf, m->p0.a_lf, m->p0.k_hf, m->p0.a_hf, m->p0.gain};
    const double pb[5] = {m->p1.k_lf, m->p1.a_lf, m->p1.k_hf, m->p1.a_hf, m->p1.gain};
    for (int i = 0; i < n; ++i) {
        const double t = (double)(m->pos + i + 1) * inv;
        for (int k = 0; k < 5; ++k) {
            const double tgt = pa[k] + t * (pb[k] - pa[k]);
            double v = m->cur[k] + m->c_smooth * (tgt - m->cur[k]);
            if (fabs(v - tgt) < 1e-12) v = tgt;
            m->cur[k] = v;
            m->parr[k][i] = v;
        }
    }
    for (int c = 0; c < nch; ++c) {
        float *x = ch[c] + off;
        double sl = m->s_lf[c], sh = m->s_hf[c];
        for (int i = 0; i < n; ++i) {
            const double k_lf = m->parr[0][i], a_lf = m->parr[1][i];
            const double k_hf = m->parr[2][i], a_hf = m->parr[3][i];
            const double gain = m->parr[4][i];
            const double xi = (double)x[i];
            double v = (xi - sl) * a_lf;
            const double lp1 = v + sl;
            sl = lp1 + v;
            const double y1 = xi + k_lf * lp1;
            v = (y1 - sh) * a_hf;
            const double lp2 = v + sh;
            sh = lp2 + v;
            const double y2 = y1 + k_hf * (y1 - lp2);
            x[i] = (float)(gain * y2);
        }
        m->s_lf[c] = e1_flush(sl);
        m->s_hf[c] = e1_flush(sh);
    }
}

E1Crystal *e1_crystal_create(double fs, int nch)
{
    if (!(fs >= 8000.0 && fs <= 384000.0) || nch < 1 || nch > E1DSP_MAX_CH) return NULL;
    E1Crystal *m = (E1Crystal *)calloc(1, sizeof(E1Crystal));
    if (!m) return NULL;
    m->fs = fs;
    m->nch = nch;
    m->frame = 32;
    while (m->frame < CR_MAX_FRAME && fs / (double)m->frame > 1600.0) m->frame *= 2;
    const double frame_rate = fs / (double)m->frame;
    m->rate_ratio = CR_REF_FRAME_RATE / frame_rate;

    double fl = 500.0, fh = 6000.0;
    if (fh > 0.45 * fs) fh = 0.45 * fs;
    e1_bq_lowpass(&m->ana_lf, fs, fl, 0.70710678118654752);
    e1_bq_highpass(&m->ana_hf, fs, fh, 0.70710678118654752);

    m->c_fast[0] = tau_coef(0.020, frame_rate);
    m->c_slow[0] = tau_coef(0.250, frame_rate);
    m->c_fast[1] = tau_coef(0.002, frame_rate);
    m->c_slow[1] = tau_coef(0.060, frame_rate);
    m->c_peak_rel = tau_coef(1.0, frame_rate);
    m->c_par = 1.0 - exp(-1.0 / 3.4);       /* 2 x one-pole, ~95 % after 16 frames */
    m->c_smooth = 1.0 - exp(-1.0 / (0.00036 * fs));
    m->tgt_level = 0.0f;
    build_makeup(m);
    e1_crystal_reset(m);
    return m;
}

void e1_crystal_destroy(E1Crystal *m) { free(m); }

void e1_crystal_reset(E1Crystal *m)
{
    if (!m) return;
    memset(&m->ana_lf_s, 0, sizeof(m->ana_lf_s));
    memset(&m->ana_hf_s, 0, sizeof(m->ana_hf_s));
    m->acc_lf = m->acc_hf = m->acc_pk = 0.0;
    for (int b = 0; b < 2; ++b) m->fast[b] = m->slow[b] = m->boost[b] = 0.0;
    m->peak = 0.0;
    for (int c = 0; c < E1DSP_MAX_CH; ++c) m->s_lf[c] = m->s_hf[c] = 0.0;
    m->pos = 0;
    m->x_cur = level_map(m->tgt_level);
    static_targets(m, m->x_cur, m->ps1);
    memcpy(m->ps2, m->ps1, sizeof(m->ps2));
    compute_targets(m);
    m->p0 = m->p1;
    m->cur[0] = m->p1.k_lf; m->cur[1] = m->p1.a_lf; m->cur[2] = m->p1.k_hf; m->cur[3] = m->p1.a_hf; m->cur[4] = m->p1.gain;
}

void e1_crystal_set(E1Crystal *m, float level_0_1)
{
    if (!m) return;
    if (!(level_0_1 > 0.0f)) level_0_1 = 0.0f;
    if (level_0_1 > 1.0f) level_0_1 = 1.0f;
    m->tgt_level = level_0_1;
}

void e1_crystal_process(E1Crystal *m, float *const *ch, int nch, int n)
{
    if (!m || !ch || n <= 0) return;
    if (nch > m->nch) nch = m->nch;
    if (nch < 1) return;
    int i = 0;
    while (i < n) {
        int seg = m->frame - m->pos;
        if (seg > n - i) seg = n - i;
        process_seg(m, ch, nch, i, seg);
        m->pos += seg;
        i += seg;
        if (m->pos == m->frame) {
            m->pos = 0;
            frame_end(m);
        }
    }
}

unsigned e1_crystal_latency(const E1Crystal *m)
{
    (void)m;
    return 0u;
}
