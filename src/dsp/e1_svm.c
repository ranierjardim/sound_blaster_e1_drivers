/*
 * e1_svm.c - "Smart Volume": look-ahead, stereo-linked loudness AGC (also the mic AGC).
 * Owner: dsp-spatial.  Spec 3.5, re/apo-dsp.md 5.1.2.
 *
 * Detector: per channel loudness weighting = RLB-style high-pass (38 Hz, Q 0.5) followed by our
 * own tilt (low shelf +7 dB @ 150 Hz, high shelf +2.5 dB @ 12 kHz), channel powers summed
 * (stereo-linked), evaluated on frames of 64 samples (32 at fs <= 24 kHz).  The gain is applied to
 * the input delayed by a 96-sample look-ahead (48 at <= 24 kHz) and interpolated per sample.
 *
 * Gain computer (dB domain), two stages:
 *   slow AGC   : loudness Ls (400 ms average of gated frames, each frame's power clamped to
 *                Ls + 6 dB so short bursts cannot drag it up; frames 6 dB above target +
 *                headroom are left to the fast stage for up to 1 s) -> static curve
 *                G = (target - Ls)(1 - 1/ratio), boosts faded out between thr and thr-10 dB
 *                (thr = -52 dB from the level table), smoothed with tau_up (slow) / tau_down;
 *                both stages are frozen while the gate is closed (frame or momentary level below
 *                -60 dB, or the frame more than 15 dB under the momentary level, i.e. the
 *                programme just stopped), so silence and pauses are never pumped up.
 *   fast stage : momentary level (10 ms rise / 80 ms fall) + slow gain above target + headroom
 *                is compressed with the same ratio; attack / 15 ms hold / release smoothing.
 *   total gain clamped to +-range, range = level table {0:0, 0.5:25, 0.74:37, 1:50} dB
 *   (ramped at <= 100 dB/s so level changes never step).
 * Modes: Normal (ratio 8, 40/100 ms, slow 4 s), Loud (higher target, ratio 10, faster),
 *        Night (ratio 24, 40/200 ms, slow 1 s, small headroom).  Mic: speech tuned, gate = noise
 *        floor tracker + 10 dB, boost capped at 30 dB.
 */
#include "e1dsp_modules.h"
#include "e1_biquad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct SvmTuning {
    double target;      /* dB (LUFS-like) */
    double ratio;
    double tau_up;      /* s, slow boost */
    double tau_down;    /* s, slow cut */
    double headroom;    /* dB above target before the fast stage acts */
    double att, rel;    /* s, fast stage */
} SvmTuning;

static const SvmTuning svm_tunings[4] = {
    /* Normal */ {-18.0, 8.0, 4.0, 1.0, 8.0, 0.040, 0.100},
    /* Loud   */ {-13.0, 10.0, 2.0, 0.6, 6.0, 0.025, 0.080},
    /* Night  */ {-20.0, 24.0, 1.0, 0.4, 3.0, 0.040, 0.200},
    /* Mic    */ {-20.0, 6.0, 1.5, 1.5, 6.0, 0.010, 0.150}, /* slow cut as slow as the boost:
                    the fast stage handles loud speech, so a cough never ducks the voice */
};

static const double svm_level_tab[4][3] = {
    /* level, threshold dB, range dB */
    {0.0, -52.0, 0.0}, {0.5, -52.0, 25.0}, {0.74, -52.0, 37.0}, {1.0, -52.0, 50.0},
};

#define SVM_GATE_DB   -60.0
#define SVM_MIC_GATE  -65.0
#define SVM_MIC_MAX_BOOST 30.0
#define SVM_FLOOR_DB  -100.0
#define SVM_LS_CLAMP_LIN 3.9810717055349722   /* +6 dB (power): max per-frame pull on Ls */
#define SVM_HOT_DB    6.0     /* frame this far above target + headroom (after gain) = "hot" event */
#define SVM_HOT_MAX_S 1.0     /* hot frames are kept out of Ls for at most this long (then: programme) */

struct E1Svm {
    double fs;
    int nch, is_mic;
    int frame, look;
    int pos;
    float *dl;                      /* nch * look ring */
    int dpos;
    E1BqCoef w_hp, w_ls, w_hs;
    E1BqState ws[E1DSP_MAX_CH][3];
    double acc;

    /* detector state */
    double pm;                      /* momentary power */
    double ls;                      /* slow loudness power */
    int ls_valid;
    double nf_db;                   /* mic noise floor */
    int hot, hot_max;               /* consecutive active "hot" frames kept out of Ls, limit */
    double c_pm_up, c_pm_dn, c_ls, nf_rise;

    /* gain state (dB) */
    double gs, gf;
    int hold, hold_frames;
    double range_s, range_step;

    /* tuning */
    float tgt_level;
    int mode;
    SvmTuning tun;
    double thr, range_t;
    double c_up, c_dn, c_att, c_rel;

    double g0, g1;
    float gain_db;
};

static double frame_coef(const E1Svm *m, double tau_s)
{
    return 1.0 - exp(-(double)m->frame / (tau_s * m->fs));
}

static double pow_db(double p)
{
    return (p > 1e-10) ? 10.0 * log10(p) - 0.691 : SVM_FLOOR_DB;
}

static void apply_tuning(E1Svm *m)
{
    m->tun = svm_tunings[m->is_mic ? 3 : m->mode];
    m->c_up = frame_coef(m, m->tun.tau_up);
    m->c_dn = frame_coef(m, m->tun.tau_down);
    m->c_att = frame_coef(m, m->tun.att);
    m->c_rel = frame_coef(m, m->tun.rel);
    const double lv = (double)m->tgt_level;
    int i = 0;
    while (i < 2 && lv > svm_level_tab[i + 1][0]) ++i;
    double t = (lv - svm_level_tab[i][0]) / (svm_level_tab[i + 1][0] - svm_level_tab[i][0]);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    m->thr = svm_level_tab[i][1] + t * (svm_level_tab[i + 1][1] - svm_level_tab[i][1]);
    m->range_t = svm_level_tab[i][2] + t * (svm_level_tab[i + 1][2] - svm_level_tab[i][2]);
}

static double static_curve(const E1Svm *m, double lin, double range)
{
    double g = (m->tun.target - lin) * (1.0 - 1.0 / m->tun.ratio);
    if (g > 0.0) {
        double fade = (lin - (m->thr - 10.0)) / 10.0;
        if (fade < 0.0) fade = 0.0;
        if (fade > 1.0) fade = 1.0;
        g *= fade;
        double cap = range;
        if (m->is_mic && cap > SVM_MIC_MAX_BOOST) cap = SVM_MIC_MAX_BOOST;
        if (g > cap) g = cap;
    } else if (g < -range) {
        g = -range;
    }
    return g;
}

static void frame_end(E1Svm *m)
{
    const double ms = m->acc / (double)m->frame;
    m->acc = 0.0;

    m->pm += ((ms > m->pm) ? m->c_pm_up : m->c_pm_dn) * (ms - m->pm);
    m->pm = e1_flush(m->pm);
    const double lm = pow_db(m->pm);
    const double lf = pow_db(ms);

    int active;
    if (m->is_mic) {
        if (lm < m->nf_db) m->nf_db = lm;
        else m->nf_db += m->nf_rise;
        double gate = m->nf_db + 10.0;
        if (gate < SVM_MIC_GATE) gate = SVM_MIC_GATE;
        active = lm > gate && lf > gate;
    } else {
        active = lm > SVM_GATE_DB && lf > SVM_GATE_DB;
    }
    if (lf < lm - 15.0) active = 0;    /* programme stopping: do not adapt on the decay */

    /* range ramp */
    if (m->range_s < m->range_t) { m->range_s += m->range_step; if (m->range_s > m->range_t) m->range_s = m->range_t; }
    else if (m->range_s > m->range_t) { m->range_s -= m->range_step; if (m->range_s < m->range_t) m->range_s = m->range_t; }
    const double range = m->range_s;

    if (active) {
        /* Frames far above the target (after the current slow gain) belong to the fast stage: a
           cough or a bang is kept out of the slow estimate entirely, as long as it lasts less
           than SVM_HOT_MAX_S; anything longer is programme and the slow stage follows it. */
        const double hot_db = (lf > lm ? lf : lm) + m->gs - (m->tun.target + m->tun.headroom + SVM_HOT_DB);
        int skip = 0;
        if (hot_db > 0.0) { if (m->hot < m->hot_max) { ++m->hot; skip = 1; } }
        else m->hot = 0;
        if (skip) {
            /* slow stage frozen for this frame */
        } else if (!m->ls_valid) { m->ls = m->pm; m->ls_valid = 1; }
        else {
            /* A short loud event (cough, desk bump, explosion) must not dominate the slow loudness
               estimate for seconds: each frame may pull the average up by at most a frame that is
               6 dB above it (SVM_LS_CLAMP_LIN). Sustained louder programme still raises it (up to
               ~30 dB/s), peaks are the fast stage's job. */
            const double cap = m->ls * SVM_LS_CLAMP_LIN;
            const double x = ms > cap ? cap : ms;
            m->ls += m->c_ls * (x - m->ls);
        }
        const double gt = static_curve(m, pow_db(m->ls), range);
        m->gs += ((gt > m->gs) ? m->c_up : m->c_dn) * (gt - m->gs);
    }
    if (m->gs > range) m->gs = range;
    if (m->gs < -range) m->gs = -range;

    /* fast stage (peaks / sudden loud passages).  Cuts always act; recovery waits for the gate. */
    const double over = lm + m->gs - (m->tun.target + m->tun.headroom);
    const double gft = (over > 0.0) ? -over * (1.0 - 1.0 / m->tun.ratio) : 0.0;
    if (gft < m->gf) { m->gf += m->c_att * (gft - m->gf); m->hold = m->hold_frames; }
    else if (m->hold > 0) --m->hold;
    else if (active) m->gf += m->c_rel * (gft - m->gf);
    if (m->gf > 0.0) m->gf = 0.0;
    if (m->gf < -range - 60.0) m->gf = -range - 60.0;

    double g = m->gs + m->gf;
    if (g > range) g = range;
    if (g < -range) g = -range;
    m->g0 = m->g1;
    m->g1 = (g == 0.0) ? 1.0 : pow(10.0, g / 20.0);
    m->gain_db = (float)g;
}

static void process_seg(E1Svm *m, float *const *ch, int nch, int off, int n)
{
    const double inv = 1.0 / (double)m->frame;
    const double g0 = m->g0, dg = m->g1 - m->g0;
    const int unity = (m->g0 == 1.0 && m->g1 == 1.0);
    for (int c = 0; c < nch; ++c) {
        float *x = ch[c] + off;
        float *ring = m->dl + (size_t)c * m->look;
        int p = m->dpos;
        double acc = 0.0;
        for (int i = 0; i < n; ++i) {
            const float in = x[i];
            const float w = e1_bq_tick(&m->w_hs, &m->ws[c][2],
                            e1_bq_tick(&m->w_ls, &m->ws[c][1], e1_bq_tick(&m->w_hp, &m->ws[c][0], in)));
            acc += (double)w * (double)w;
            const float d = ring[p];
            ring[p] = in;
            if (++p >= m->look) p = 0;
            x[i] = unity ? d : (float)((double)d * (g0 + dg * (double)(m->pos + i + 1) * inv));
        }
        m->acc += acc;
        for (int k = 0; k < 3; ++k) { m->ws[c][k].z1 = e1_flush(m->ws[c][k].z1); m->ws[c][k].z2 = e1_flush(m->ws[c][k].z2); }
    }
    m->dpos = (m->dpos + n) % m->look;
}

E1Svm *e1_svm_create(double fs, int nch, int is_mic)
{
    if (!(fs >= 8000.0 && fs <= 384000.0) || nch < 1 || nch > E1DSP_MAX_CH) return NULL;
    E1Svm *m = (E1Svm *)calloc(1, sizeof(E1Svm));
    if (!m) return NULL;
    m->fs = fs;
    m->nch = nch;
    m->is_mic = is_mic ? 1 : 0;
    m->frame = (fs > 24000.0) ? 64 : 32;
    m->look = (fs > 24000.0) ? 96 : 48;
    m->dl = (float *)calloc((size_t)nch * (size_t)m->look, sizeof(float));
    if (!m->dl) { free(m); return NULL; }
    const double fmax = 0.45 * fs;
    e1_bq_highpass(&m->w_hp, fs, 38.0, 0.5);
    e1_bq_lowshelf(&m->w_ls, fs, 150.0, 0.6, 7.0);
    e1_bq_highshelf(&m->w_hs, fs, 12000.0 < fmax ? 12000.0 : fmax, 0.7, 2.5);
    m->c_pm_up = frame_coef(m, m->is_mic ? 0.005 : 0.010);
    m->c_pm_dn = frame_coef(m, 0.080);
    m->c_ls = frame_coef(m, 0.400);
    m->nf_rise = 2.0 * (double)m->frame / fs;                  /* noise floor may rise 2 dB/s */
    m->hold_frames = (int)floor(0.015 * fs / (double)m->frame + 0.5);
    m->hot_max = (int)floor(SVM_HOT_MAX_S * fs / (double)m->frame + 0.5);
    m->range_step = 100.0 * (double)m->frame / fs;             /* 100 dB/s */
    m->tgt_level = 0.0f;
    m->mode = E1FX_SVM_NORMAL;
    apply_tuning(m);
    e1_svm_reset(m);
    return m;
}

void e1_svm_destroy(E1Svm *m)
{
    if (!m) return;
    free(m->dl);
    free(m);
}

void e1_svm_reset(E1Svm *m)
{
    if (!m) return;
    memset(m->dl, 0, sizeof(float) * (size_t)m->nch * (size_t)m->look);
    memset(m->ws, 0, sizeof(m->ws));
    m->dpos = 0;
    m->pos = 0;
    m->acc = 0.0;
    m->pm = 0.0;
    m->ls = 0.0;
    m->ls_valid = 0;
    m->hot = 0;
    m->nf_db = SVM_FLOOR_DB;
    m->gs = m->gf = 0.0;
    m->hold = 0;
    m->range_s = m->range_t;
    m->g0 = m->g1 = 1.0;
    m->gain_db = 0.0f;
}

void e1_svm_set(E1Svm *m, float level_0_1, int mode)
{
    if (!m) return;
    if (!(level_0_1 > 0.0f)) level_0_1 = 0.0f;
    if (level_0_1 > 1.0f) level_0_1 = 1.0f;
    m->tgt_level = level_0_1;
    m->mode = (mode == E1FX_SVM_LOUD || mode == E1FX_SVM_NIGHT) ? mode : E1FX_SVM_NORMAL;
    apply_tuning(m);
}

void e1_svm_process(E1Svm *m, float *const *ch, int nch, int n)
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

unsigned e1_svm_latency(const E1Svm *m)
{
    return m ? (unsigned)m->look : 0u;
}

float e1_svm_gain_db(const E1Svm *m)
{
    return m ? m->gain_db : 0.0f;
}
