/*
 * e1_dialog.c - "Dialog+": mid/side dialogue enhancement with loudness normalisation.
 * Owner: dsp-spatial.  Spec 3.5 (level table), re/apo-dsp.md 5.1.2.
 *
 *   M = (L+R)/2, S = (L-R)/2
 *   Mv = BP_voice(M), Sv = BP_voice(S)      BP_voice = HPF 300 Hz * LPF 4 kHz (Butterworth);
 *                                           the two phase shifts cancel near 1.1 kHz
 *   Mp = BP_presence(M)                     0 dB-peak band-pass at 3 kHz, Q 0.9
 *   M' = M + (ga-1) Mv + (gc-1) Mp          dialogue gain a, presence gain c (parallel-EQ form:
 *   S' = S + (gb-1) Sv                      ambience gain b     exact identity when gains are 1)
 *   L = gn (M' + S'), R = gn (M' - S')
 *
 * Level table {level: a, b, c dB}: 0:(0,0,0) 0.3:(+6,-6,0) 0.5:(+9,-9,0) 0.8:(+12,-12,+3)
 * 1.0:(+12,-12,+6), linear interpolation.
 *
 * Loudness normalisation gn: the module keeps running K-weighted second-order statistics of the
 * input components (E[Mw^2], E[Mw Mvw], ... over ~1 s, gated below -70 dB, warm-up by cumulative
 * averaging).  Because the output is a linear combination of those components, the K-weighted
 * output power for ANY gain set can be predicted exactly, so gn = sqrt(Pin/Pout) tracks level
 * changes instantly (no loudness jump while the slider moves) and adapts to the programme slowly
 * (no pumping).  Before the first gated frame a dialogue-like prior is used; it is rescaled to the
 * first frame's power and blended out as real statistics accumulate.  gn is limited to [-18 dB, 0 dB]: Dialog+ never makes the programme louder.
 *
 * Control runs on 64-sample frames (block-size independent), gains are interpolated per sample.
 * Latency 0.
 */
#include "e1dsp_modules.h"
#include "e1_biquad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DL_FRAME 64

static const double dl_tab[5][4] = {
    /* level, dialogue dB, ambience dB, presence dB */
    {0.0, 0.0, 0.0, 0.0},
    {0.3, 6.0, -6.0, 0.0},
    {0.5, 9.0, -9.0, 0.0},
    {0.8, 12.0, -12.0, 3.0},
    {1.0, 12.0, -12.0, 6.0},
};

/* statistics indices */
enum { ST_MM, ST_MVMV, ST_MPMP, ST_MMV, ST_MMP, ST_MVMP, ST_SS, ST_SVSV, ST_SSV, ST_COUNT };
/* weighted signal indices */
enum { W_M, W_MV, W_MP, W_S, W_SV, W_COUNT };

struct E1Dialog {
    double fs;
    E1BqCoef v_hp, v_lp, pres, k_shelf, k_hp;
    E1BqState mv1, mv2, sv1, sv2, mp;
    E1BqState wst[W_COUNT][2];

    double acc[ST_COUNT];
    double st[ST_COUNT];
    double n_frames;              /* effective number of frames averaged so far (warm-up) */
    double alpha_min;             /* per-frame coefficient of the ~1 s average */
    double gate_ms;               /* frame mean-square gate */

    float tgt_level;
    double lvl, lvl_step;

    double ga0, gb0, gc0, gn0, ga1, gb1, gc1, gn1;
    int pos;
};

static void level_gains(double lvl, double *a_db, double *b_db, double *c_db)
{
    int i = 0;
    while (i < 3 && lvl > dl_tab[i + 1][0]) ++i;
    double t = (lvl - dl_tab[i][0]) / (dl_tab[i + 1][0] - dl_tab[i][0]);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    *a_db = dl_tab[i][1] + t * (dl_tab[i + 1][1] - dl_tab[i][1]);
    *b_db = dl_tab[i][2] + t * (dl_tab[i + 1][2] - dl_tab[i][2]);
    *c_db = dl_tab[i][3] + t * (dl_tab[i + 1][3] - dl_tab[i][3]);
}

static double predict_norm(const E1Dialog *m, double ga, double gb, double gc)
{
    if (ga == 1.0 && gb == 1.0 && gc == 1.0) return 1.0;
    const double *s = m->st;
    const double da = ga - 1.0, db = gb - 1.0, dc = gc - 1.0;
    const double p_in = s[ST_MM] + s[ST_SS];
    const double p_out = s[ST_MM] + da * da * s[ST_MVMV] + dc * dc * s[ST_MPMP] + 2.0 * da * s[ST_MMV] +
                         2.0 * dc * s[ST_MMP] + 2.0 * da * dc * s[ST_MVMP] +
                         s[ST_SS] + db * db * s[ST_SVSV] + 2.0 * db * s[ST_SSV];
    if (!(p_in > 1e-30) || !(p_out > 1e-30)) return 1.0;
    double g = sqrt(p_in / p_out);
    const double gmin = 0.12589254117941673; /* -18 dB */
    if (g > 1.0) g = 1.0;
    if (g < gmin) g = gmin;
    return g;
}

static void set_prior_stats(E1Dialog *m)
{
    /* Prior used until the first gated frame: dialogue-like programme (most energy centred and
       in the voice band, some ambience).  Replaced entirely by the first real frame. */
    m->st[ST_MM] = 1.0;   m->st[ST_MVMV] = 0.6;  m->st[ST_MMV] = 0.6;
    m->st[ST_MPMP] = 0.12; m->st[ST_MMP] = 0.12; m->st[ST_MVMP] = 0.10;
    m->st[ST_SS] = 0.25;  m->st[ST_SVSV] = 0.12; m->st[ST_SSV] = 0.12;
    m->n_frames = 0.0;
}

static void frame_end(E1Dialog *m)
{
    const double inv = 1.0 / DL_FRAME;
    const double p_frame = (m->acc[ST_MM] + m->acc[ST_SS]) * inv;
    if (p_frame > m->gate_ms) {
        if (m->n_frames == 0.0) {
            /* first real frame: rescale the prior to this programme level, weight 1:1 */
            const double sc = p_frame / (m->st[ST_MM] + m->st[ST_SS]);
            for (int k = 0; k < ST_COUNT; ++k) m->st[k] = 0.5 * m->st[k] * sc + 0.5 * m->acc[k] * inv;
            m->n_frames = 2.0;
        } else {
            m->n_frames += 1.0;
            double a = 1.0 / m->n_frames;
            if (a < m->alpha_min) a = m->alpha_min;
            for (int k = 0; k < ST_COUNT; ++k) m->st[k] += a * (m->acc[k] * inv - m->st[k]);
        }
    }
    for (int k = 0; k < ST_COUNT; ++k) m->acc[k] = 0.0;

    const double tgt = (double)m->tgt_level;
    if (m->lvl < tgt) { m->lvl += m->lvl_step; if (m->lvl > tgt) m->lvl = tgt; }
    else if (m->lvl > tgt) { m->lvl -= m->lvl_step; if (m->lvl < tgt) m->lvl = tgt; }

    double a_db, b_db, c_db;
    level_gains(m->lvl, &a_db, &b_db, &c_db);
    m->ga0 = m->ga1; m->gb0 = m->gb1; m->gc0 = m->gc1; m->gn0 = m->gn1;
    m->ga1 = e1_db2lin(a_db);
    m->gb1 = e1_db2lin(b_db);
    m->gc1 = e1_db2lin(c_db);
    m->gn1 = predict_norm(m, m->ga1, m->gb1, m->gc1);
}

static inline double wtick(const E1Dialog *m, E1BqState st[2], double x)
{
    /* K-weighting-like loudness filter: +4 dB high shelf above ~1.5 kHz, then 38 Hz high-pass */
    return (double)e1_bq_tick(&m->k_hp, &st[1], e1_bq_tick(&m->k_shelf, &st[0], (float)x));
}

static void process_seg(E1Dialog *m, float *l, float *r, int n)
{
    const int identity = (m->ga0 == 1.0 && m->gb0 == 1.0 && m->gc0 == 1.0 && m->gn0 == 1.0 &&
                          m->ga1 == 1.0 && m->gb1 == 1.0 && m->gc1 == 1.0 && m->gn1 == 1.0);
    const double inv = 1.0 / DL_FRAME;
    for (int i = 0; i < n; ++i) {
        const double xl = (double)l[i], xr = (double)r[i];
        const double M = 0.5 * (xl + xr), S = 0.5 * (xl - xr);
        const double Mv = (double)e1_bq_tick(&m->v_lp, &m->mv2, e1_bq_tick(&m->v_hp, &m->mv1, (float)M));
        const double Sv = (double)e1_bq_tick(&m->v_lp, &m->sv2, e1_bq_tick(&m->v_hp, &m->sv1, (float)S));
        const double Mp = (double)e1_bq_tick(&m->pres, &m->mp, (float)M);

        const double w_m = wtick(m, m->wst[W_M], M), w_mv = wtick(m, m->wst[W_MV], Mv);
        const double w_mp = wtick(m, m->wst[W_MP], Mp), w_s = wtick(m, m->wst[W_S], S);
        const double w_sv = wtick(m, m->wst[W_SV], Sv);
        m->acc[ST_MM] += w_m * w_m;     m->acc[ST_MVMV] += w_mv * w_mv; m->acc[ST_MPMP] += w_mp * w_mp;
        m->acc[ST_MMV] += w_m * w_mv;   m->acc[ST_MMP] += w_m * w_mp;   m->acc[ST_MVMP] += w_mv * w_mp;
        m->acc[ST_SS] += w_s * w_s;     m->acc[ST_SVSV] += w_sv * w_sv; m->acc[ST_SSV] += w_s * w_sv;

        if (!identity) {
            const double t = (double)(m->pos + i + 1) * inv;
            const double ga = m->ga0 + t * (m->ga1 - m->ga0), gb = m->gb0 + t * (m->gb1 - m->gb0);
            const double gc = m->gc0 + t * (m->gc1 - m->gc0), gn = m->gn0 + t * (m->gn1 - m->gn0);
            const double Mo = M + (ga - 1.0) * Mv + (gc - 1.0) * Mp;
            const double So = S + (gb - 1.0) * Sv;
            l[i] = (float)(gn * (Mo + So));
            r[i] = (float)(gn * (Mo - So));
        }
    }
    E1BqState *all[5] = {&m->mv1, &m->mv2, &m->sv1, &m->sv2, &m->mp};
    for (int k = 0; k < 5; ++k) { all[k]->z1 = e1_flush(all[k]->z1); all[k]->z2 = e1_flush(all[k]->z2); }
    for (int w = 0; w < W_COUNT; ++w)
        for (int k = 0; k < 2; ++k) { m->wst[w][k].z1 = e1_flush(m->wst[w][k].z1); m->wst[w][k].z2 = e1_flush(m->wst[w][k].z2); }
}

E1Dialog *e1_dialog_create(double fs)
{
    if (!(fs >= 8000.0 && fs <= 384000.0)) return NULL;
    E1Dialog *m = (E1Dialog *)calloc(1, sizeof(E1Dialog));
    if (!m) return NULL;
    m->fs = fs;
    const double fmax = 0.45 * fs;
    e1_bq_highpass(&m->v_hp, fs, 300.0, 0.70710678118654752);
    e1_bq_lowpass(&m->v_lp, fs, 4000.0 < fmax ? 4000.0 : fmax, 0.70710678118654752);
    e1_bq_bandpass(&m->pres, fs, 3000.0 < fmax ? 3000.0 : fmax, 0.9);
    e1_bq_highshelf(&m->k_shelf, fs, 1500.0 < fmax ? 1500.0 : fmax, 0.70710678118654752, 4.0);
    e1_bq_highpass(&m->k_hp, fs, 38.0, 0.5);
    const double frames_per_s = fs / DL_FRAME;
    m->alpha_min = 1.0 - exp(-1.0 / (1.0 * frames_per_s));     /* ~1 s statistics */
    m->lvl_step = (double)DL_FRAME / (0.050 * fs);             /* full level swing in 50 ms */
    m->gate_ms = 1e-7;                                         /* -70 dB */
    m->tgt_level = 0.0f;
    e1_dialog_reset(m);
    return m;
}

void e1_dialog_destroy(E1Dialog *m) { free(m); }

void e1_dialog_reset(E1Dialog *m)
{
    if (!m) return;
    memset(&m->mv1, 0, sizeof(m->mv1)); memset(&m->mv2, 0, sizeof(m->mv2));
    memset(&m->sv1, 0, sizeof(m->sv1)); memset(&m->sv2, 0, sizeof(m->sv2));
    memset(&m->mp, 0, sizeof(m->mp));
    memset(m->wst, 0, sizeof(m->wst));
    memset(m->acc, 0, sizeof(m->acc));
    set_prior_stats(m);
    m->lvl = (double)m->tgt_level;
    double a_db, b_db, c_db;
    level_gains(m->lvl, &a_db, &b_db, &c_db);
    m->ga0 = m->ga1 = e1_db2lin(a_db);
    m->gb0 = m->gb1 = e1_db2lin(b_db);
    m->gc0 = m->gc1 = e1_db2lin(c_db);
    m->gn0 = m->gn1 = predict_norm(m, m->ga1, m->gb1, m->gc1);
    m->pos = 0;
}

void e1_dialog_set(E1Dialog *m, float level_0_1)
{
    if (!m) return;
    if (!(level_0_1 > 0.0f)) level_0_1 = 0.0f;
    if (level_0_1 > 1.0f) level_0_1 = 1.0f;
    m->tgt_level = level_0_1;
}

void e1_dialog_process(E1Dialog *m, float *l, float *r, int n)
{
    if (!m || !l || !r || n <= 0) return;
    int i = 0;
    while (i < n) {
        int seg = DL_FRAME - m->pos;
        if (seg > n - i) seg = n - i;
        process_seg(m, l + i, r + i, seg);
        m->pos += seg;
        i += seg;
        if (m->pos == DL_FRAME) {
            m->pos = 0;
            frame_end(m);
        }
    }
}

unsigned e1_dialog_latency(const E1Dialog *m)
{
    (void)m;
    return 0u;
}
