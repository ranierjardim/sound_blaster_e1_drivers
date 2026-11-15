/*
 * e1_surround.c - "Surround": stereo M/S upmix + decorrelated ambience + binaural (headphones) or
 * crosstalk-shuffler (loudspeakers) virtualizer.  Owner: dsp-spatial.  Spec 3.4.
 *
 * Signal flow (per sample):
 *   m = (L+R)/2, s = (L-R)/2
 *   a = shelf(HPF(s))  -> 13 ms pre-delay -> four parallel Schroeder all-passes
 *       (4.7 / 3.7 / 6.5 / 7.0 ms, g = 0.465) -> side L/R (+-110 deg) and back L/R (+-145 deg, -3 dB)
 *   six shuffler-domain FIR streams: front m, front s, side sum/diff, back sum/diff
 *   E_sum = Ffs*m + Fss*side_sum + Fbs*back_sum ; E_dif = Ffd*s + Fsd*side_dif + Fbd*back_dif
 *   L = E_sum + E_dif, R = E_sum - E_dif
 *
 * There is no separate dry path: the immersion level selects (and interpolates between) filter
 * sets that go from an exact identity (level 0) to full rendering, each energy-normalised for mid
 * and side content (see e1_hrtf_data.c).  That gives constant loudness without the comb filtering a
 * dry/wet sum of coherent signals would cause.  All filters are minimum phase -> latency 0.
 *
 * Parameter changes: evaluated on 64-sample control boundaries (independent of the caller's block
 * size); a change builds a new coefficient bank and crossfades the outputs of the old and new banks
 * linearly over ~20 ms.  Level 0 at rest is a bit-exact pass-through.
 */
#include "e1dsp_modules.h"
#include "e1_biquad.h"
#include "e1_hrtf_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SR_CTRL 64                      /* control granularity and max internal segment */

#if defined(_MSC_VER)
#define E1_RESTRICT __restrict
#else
#define E1_RESTRICT restrict
#endif

struct E1Surround {
    double fs;
    int taps;
    float *design;                      /* [2][LEVELS][6][taps] */
    float *bank_a, *bank_b;             /* effective coefficient sets [6][taps] */
    float *hist[E1_VIRT_NFILT];         /* input histories, taps-1+SR_CTRL each */
    float ys_a[SR_CTRL], yd_a[SR_CTRL], ys_b[SR_CTRL], yd_b[SR_CTRL];

    /* ambience path */
    E1BqCoef hp, shelf;
    E1BqState hp_s, shelf_s;
    float *pre;
    int pre_len, pre_pos;
    float *ap[4];
    int ap_len[4], ap_pos[4];

    /* control */
    float tgt_level;
    int tgt_mode;
    float a_level, b_level;
    int a_mode, b_mode;
    int a_identity;
    int xf_active, xf_pos, xf_len;
    int ctrl_pos;
};

static float sanitize_level(float v)
{
    if (!(v > 0.0f)) return 0.0f;          /* also catches NaN */
    return v > 1.0f ? 1.0f : v;
}

static void build_bank(const E1Surround *m, float *bank, float level, int mode)
{
    const int L = m->taps;
    double p = (double)level * (E1_VIRT_LEVELS - 1);
    int j = (int)floor(p);
    if (j >= E1_VIRT_LEVELS - 1) j = E1_VIRT_LEVELS - 2;
    if (j < 0) j = 0;
    const float t = (float)(p - (double)j), u = 1.0f - t;
    for (int f = 0; f < E1_VIRT_NFILT; ++f) {
        const float *h0 = m->design + e1_virt_index(mode, j, f, L);
        const float *h1 = m->design + e1_virt_index(mode, j + 1, f, L);
        float *o = bank + (size_t)f * L;
        for (int k = 0; k < L; ++k) o[k] = u * h0[k] + t * h1[k];
    }
}

/* y[i] (+)= sum_k h[k] * x[i-k]; xh points at the history start (x[-(L-1)]).  Tap-major order so
   the inner loop is element-wise and vectorises without reassociating any sum. */
static void fir_run(const float *E1_RESTRICT h, int L, const float *E1_RESTRICT xh,
                    float *E1_RESTRICT y, int n, int accumulate)
{
    if (!accumulate) memset(y, 0, sizeof(float) * (size_t)n);
    for (int k = 0; k < L; ++k) {
        const float hk = h[k];
        if (hk == 0.0f) continue;
        const float *xp = xh + (L - 1 - k);
        for (int i = 0; i < n; ++i) y[i] += hk * xp[i];
    }
}

static void bank_run(const E1Surround *m, const float *bank, float *ys, float *yd, int n)
{
    const int L = m->taps;
    fir_run(bank + (size_t)E1_VF_FRONT_SUM * L, L, m->hist[E1_VF_FRONT_SUM], ys, n, 0);
    fir_run(bank + (size_t)E1_VF_SIDE_SUM * L, L, m->hist[E1_VF_SIDE_SUM], ys, n, 1);
    fir_run(bank + (size_t)E1_VF_BACK_SUM * L, L, m->hist[E1_VF_BACK_SUM], ys, n, 1);
    fir_run(bank + (size_t)E1_VF_FRONT_DIF * L, L, m->hist[E1_VF_FRONT_DIF], yd, n, 0);
    fir_run(bank + (size_t)E1_VF_SIDE_DIF * L, L, m->hist[E1_VF_SIDE_DIF], yd, n, 1);
    fir_run(bank + (size_t)E1_VF_BACK_DIF * L, L, m->hist[E1_VF_BACK_DIF], yd, n, 1);
}

static inline float ap_tick(float *buf, int len, int *pos, float x)
{
    const float g = (float)E1_AMB_AP_G;
    const float v = g * x + buf[*pos];
    float w = x - g * v;
    if (fabsf(w) < 1e-20f) w = 0.0f;
    buf[*pos] = w;
    if (++*pos >= len) *pos = 0;
    return v;
}

static void control(E1Surround *m)
{
    /* A finished crossfade is committed only here, on a control boundary, so the moment the
       bank swaps (and a level-0 bank switches to the exact bypass) never depends on how the
       caller sliced its blocks. Until then the mix weight is clamped at 1 (= bank B). */
    if (m->xf_active) {
        if (m->xf_pos < m->xf_len) return;
        float *t = m->bank_a; m->bank_a = m->bank_b; m->bank_b = t;
        m->a_level = m->b_level;
        m->a_mode = m->b_mode;
        m->a_identity = (m->a_level == 0.0f);
        m->xf_active = 0;
    }
    if (m->tgt_level == m->a_level && m->tgt_mode == m->a_mode) return;
    if (m->tgt_level == 0.0f && m->a_level == 0.0f) {
        m->a_mode = m->tgt_mode;          /* both banks are the identity: switch silently */
        return;
    }
    m->b_level = m->tgt_level;
    m->b_mode = m->tgt_mode;
    build_bank(m, m->bank_b, m->b_level, m->b_mode);
    m->xf_active = 1;
    m->xf_pos = 0;
}

static void process_seg(E1Surround *m, float *l, float *r, int n)
{
    const int L = m->taps, off = L - 1;
    float *hm = m->hist[E1_VF_FRONT_SUM] + off, *hs = m->hist[E1_VF_FRONT_DIF] + off;
    float *h2 = m->hist[E1_VF_SIDE_SUM] + off, *h3 = m->hist[E1_VF_SIDE_DIF] + off;
    float *h4 = m->hist[E1_VF_BACK_SUM] + off, *h5 = m->hist[E1_VF_BACK_DIF] + off;
    const float bg = (float)E1_AMB_BACK_GAIN;

    for (int i = 0; i < n; ++i) {
        const float xl = l[i], xr = r[i];
        const float s = 0.5f * (xl - xr);
        hm[i] = 0.5f * (xl + xr);
        hs[i] = s;
        float a = e1_bq_tick(&m->shelf, &m->shelf_s, e1_bq_tick(&m->hp, &m->hp_s, s));
        const float d = m->pre[m->pre_pos];
        m->pre[m->pre_pos] = a;
        if (++m->pre_pos >= m->pre_len) m->pre_pos = 0;
        const float sl = ap_tick(m->ap[0], m->ap_len[0], &m->ap_pos[0], d);
        const float sr = -ap_tick(m->ap[1], m->ap_len[1], &m->ap_pos[1], d);
        const float bl = ap_tick(m->ap[2], m->ap_len[2], &m->ap_pos[2], bg * d);
        const float br = -ap_tick(m->ap[3], m->ap_len[3], &m->ap_pos[3], bg * d);
        h2[i] = 0.5f * (sl + sr);
        h3[i] = 0.5f * (sl - sr);
        h4[i] = 0.5f * (bl + br);
        h5[i] = 0.5f * (bl - br);
    }
    m->hp_s.z1 = e1_flush(m->hp_s.z1); m->hp_s.z2 = e1_flush(m->hp_s.z2);
    m->shelf_s.z1 = e1_flush(m->shelf_s.z1); m->shelf_s.z2 = e1_flush(m->shelf_s.z2);

    if (!(m->a_identity && !m->xf_active)) {
        bank_run(m, m->bank_a, m->ys_a, m->yd_a, n);
        if (m->xf_active) {
            bank_run(m, m->bank_b, m->ys_b, m->yd_b, n);
            const float inv = 1.0f / (float)m->xf_len;
            for (int i = 0; i < n; ++i) {
                int p = m->xf_pos + i + 1;
                if (p > m->xf_len) p = m->xf_len;
                const float w = (float)p * inv;
                m->ys_a[i] += w * (m->ys_b[i] - m->ys_a[i]);
                m->yd_a[i] += w * (m->yd_b[i] - m->yd_a[i]);
            }
        }
        for (int i = 0; i < n; ++i) {
            l[i] = m->ys_a[i] + m->yd_a[i];
            r[i] = m->ys_a[i] - m->yd_a[i];
        }
    }

    if (m->xf_active && m->xf_pos < m->xf_len) m->xf_pos += n;
    for (int f = 0; f < E1_VIRT_NFILT; ++f) memmove(m->hist[f], m->hist[f] + n, sizeof(float) * (size_t)off);
}

void e1_surround_destroy(E1Surround *m)
{
    if (!m) return;
    free(m->design);
    free(m->bank_a);
    free(m->bank_b);
    for (int f = 0; f < E1_VIRT_NFILT; ++f) free(m->hist[f]);
    free(m->pre);
    for (int i = 0; i < 4; ++i) free(m->ap[i]);
    free(m);
}

E1Surround *e1_surround_create(double fs)
{
    if (!(fs >= 8000.0 && fs <= 384000.0)) return NULL;
    E1Surround *m = (E1Surround *)calloc(1, sizeof(E1Surround));
    if (!m) return NULL;
    m->fs = fs;
    m->taps = e1_virt_taps(fs);
    const int L = m->taps;
    m->design = (float *)calloc((size_t)2 * E1_VIRT_LEVELS * E1_VIRT_NFILT * L, sizeof(float));
    m->bank_a = (float *)calloc((size_t)E1_VIRT_NFILT * L, sizeof(float));
    m->bank_b = (float *)calloc((size_t)E1_VIRT_NFILT * L, sizeof(float));
    if (!m->design || !m->bank_a || !m->bank_b) goto fail;
    for (int f = 0; f < E1_VIRT_NFILT; ++f)
        if (!(m->hist[f] = (float *)calloc((size_t)(L - 1 + SR_CTRL), sizeof(float)))) goto fail;

    e1_amb_filters(fs, &m->hp, &m->shelf);
    m->pre_len = e1_amb_delay_samples(E1_AMB_PREDELAY_MS, fs);
    m->ap_len[0] = e1_amb_delay_samples(E1_AMB_AP0_MS, fs);
    m->ap_len[1] = e1_amb_delay_samples(E1_AMB_AP1_MS, fs);
    m->ap_len[2] = e1_amb_delay_samples(E1_AMB_AP2_MS, fs);
    m->ap_len[3] = e1_amb_delay_samples(E1_AMB_AP3_MS, fs);
    if (!(m->pre = (float *)calloc((size_t)m->pre_len, sizeof(float)))) goto fail;
    for (int i = 0; i < 4; ++i)
        if (!(m->ap[i] = (float *)calloc((size_t)m->ap_len[i], sizeof(float)))) goto fail;

    if (e1_virt_design(fs, L, m->design) != 0) goto fail;

    m->xf_len = (int)floor(0.020 * fs + 0.5);
    if (m->xf_len < SR_CTRL) m->xf_len = SR_CTRL;
    m->tgt_level = 0.0f;
    m->tgt_mode = E1FX_SPK_HEADPHONES;
    e1_surround_reset(m);
    return m;

fail:
    e1_surround_destroy(m);
    return NULL;
}

void e1_surround_reset(E1Surround *m)
{
    if (!m) return;
    const int L = m->taps;
    for (int f = 0; f < E1_VIRT_NFILT; ++f) memset(m->hist[f], 0, sizeof(float) * (size_t)(L - 1 + SR_CTRL));
    memset(m->pre, 0, sizeof(float) * (size_t)m->pre_len);
    for (int i = 0; i < 4; ++i) {
        memset(m->ap[i], 0, sizeof(float) * (size_t)m->ap_len[i]);
        m->ap_pos[i] = 0;
    }
    m->pre_pos = 0;
    memset(&m->hp_s, 0, sizeof(m->hp_s));
    memset(&m->shelf_s, 0, sizeof(m->shelf_s));
    m->a_level = m->b_level = m->tgt_level;
    m->a_mode = m->b_mode = m->tgt_mode;
    build_bank(m, m->bank_a, m->a_level, m->a_mode);
    m->a_identity = (m->a_level == 0.0f);
    m->xf_active = 0;
    m->xf_pos = 0;
    m->ctrl_pos = 0;
}

void e1_surround_set(E1Surround *m, float level_0_1, int speaker_mode)
{
    if (!m) return;
    m->tgt_level = sanitize_level(level_0_1);
    m->tgt_mode = (speaker_mode == E1FX_SPK_STEREO) ? E1FX_SPK_STEREO : E1FX_SPK_HEADPHONES;
}

void e1_surround_process(E1Surround *m, float *l, float *r, int n)
{
    if (!m || !l || !r || n <= 0) return;
    int i = 0;
    while (i < n) {
        if (m->ctrl_pos == 0) control(m);
        int seg = SR_CTRL - m->ctrl_pos;
        if (seg > n - i) seg = n - i;
        process_seg(m, l + i, r + i, seg);
        m->ctrl_pos = (m->ctrl_pos + seg) % SR_CTRL;
        i += seg;
    }
}

unsigned e1_surround_latency(const E1Surround *m)
{
    (void)m;
    return 0u; /* minimum-phase filters, no look-ahead */
}

unsigned e1_surround_settle_frames(const E1Surround *m)
{
    if (!m) return 0u;
    /* The ambience starts pre_len frames after a reset; every all-pass then re-injects its delayed
       tap once per period, attenuated by g per round trip. After n round trips of the longest
       all-pass the remaining onset echoes are g^n below the ambience: n = 60 dB / -20log10(g). */
    int apmax = 0;
    for (int i = 0; i < 4; ++i) if (m->ap_len[i] > apmax) apmax = m->ap_len[i];
    const int rounds = (int)ceil(60.0 / (-20.0 * log10(E1_AMB_AP_G)));
    return (unsigned)(m->pre_len + rounds * apmax + m->taps);
}
