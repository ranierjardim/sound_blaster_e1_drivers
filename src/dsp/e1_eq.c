/*
 * e1_eq.c - 10-band graphic EQ (spec 3.7).
 *
 * Band 0: low shelf (RBJ, S = 1, half-gain point at the band centre).
 * Bands 1..8: peaking, RBJ cookbook with a one-octave digital bandwidth (bilinear-warp
 *             corrected, Q >= 0.7), symmetric boost/cut, exact gain at the centre frequency.
 * Band 9: high shelf (RBJ, S = 1, half-gain point at the band centre).
 * Centres above 0.45*fs are clamped to 0.45*fs.
 * The preamp is folded into section 0's numerator (as in the original engine), so preamp
 * changes ramp together with the filter coefficients and need no separate gain stage.
 *
 * Coefficient changes: every section ramps linearly, per sample, from its current to its new
 * coefficients over 10 ms. Linear interpolation of (a1, a2) stays inside the stability triangle
 * (it is convex), so every intermediate filter is stable. A new change during a ramp starts a
 * new 10 ms ramp from the current interpolated point.
 *
 * Precision: the whole cascade runs in double (one conversion in, one out per sample);
 * Transposed DF-II state in double, flushed below 1e-25 at block end.
 * A 0 dB band with 0 dB preamp is the exact identity section and is skipped once its state
 * has drained, so a flat EQ is bit-exact.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "e1dsp_modules.h"
#include "e1_biquad.h"

#define NB E1FX_EQ_BANDS

struct E1Eq {
    double   fs;
    int      nch;
    int      ramp_len;           /* samples per coefficient ramp (10 ms) */
    int      ramp_left;          /* samples left in the current ramp (all sections ramp together) */
    float    t_gain[NB];         /* target settings (sanitised) */
    float    t_pre;
    E1BqCoef cur[NB];            /* coefficients at the start of the next sample */
    E1BqCoef tgt[NB];
    E1BqCoef inc[NB];            /* per-sample increment while ramping */
    int      tgt_ident[NB];      /* target section is the exact identity */
    double   z[E1DSP_MAX_CH][NB][2];
};

static const double k_freqs[NB] = E1FX_EQ_FREQS;

static int coef_is_ident(const E1BqCoef *c)
{
    return c->b0 == 1.0 && c->b1 == 0.0 && c->b2 == 0.0 && c->a1 == 0.0 && c->a2 == 0.0;
}

static void design_band(const E1Eq *m, int b, double g_db, double pre_lin, E1BqCoef *c)
{
    double f = k_freqs[b];
    if (f > 0.45 * m->fs) f = 0.45 * m->fs;
    if (g_db == 0.0) {
        e1_bq_identity(c);
    } else if (b == 0) {
        e1_bq_lowshelf(c, m->fs, f, 0.70710678118654752, g_db);
    } else if (b == NB - 1) {
        e1_bq_highshelf(c, m->fs, f, 0.70710678118654752, g_db);
    } else {
        /* RBJ: alpha = sin(w0) * sinh(ln2/2 * BW * w0/sin(w0)), BW = 1 octave; Q = sin(w0)/(2 alpha) */
        double w0 = 2.0 * E1_PI * f / m->fs, sw = sin(w0);
        double alpha = sw * sinh(0.5 * log(2.0) * 1.0 * w0 / sw);
        double q = sw / (2.0 * alpha);
        /* near Nyquist (low fs, clamped centres) the octave edges fall beyond fs/2 and the
           formula degenerates to a very wide, ill-conditioned section: keep Q >= 0.7 */
        if (q < 0.7) q = 0.7;
        e1_bq_peaking(c, m->fs, f, q, g_db);
    }
    if (b == 0 && pre_lin != 1.0) { c->b0 *= pre_lin; c->b1 *= pre_lin; c->b2 *= pre_lin; }
}

static float clamp_db(float v, float lim)
{
    if (!isfinite(v)) return 0.f;
    return v < -lim ? -lim : (v > lim ? lim : v);
}

E1Eq *e1_eq_create(double fs, int nch)
{
    if (!(fs >= 8000.0 && fs <= 384000.0) || nch < 1 || nch > E1DSP_MAX_CH) return NULL;
    E1Eq *m = (E1Eq *)calloc(1, sizeof(E1Eq));
    if (!m) return NULL;
    m->fs = fs;
    m->nch = nch;
    m->ramp_len = (int)floor(fs * 0.010 + 0.5);
    if (m->ramp_len < 1) m->ramp_len = 1;
    for (int b = 0; b < NB; ++b) {
        e1_bq_identity(&m->cur[b]);
        e1_bq_identity(&m->tgt[b]);
        m->tgt_ident[b] = 1;
    }
    return m;
}

void e1_eq_destroy(E1Eq *m) { free(m); }

void e1_eq_reset(E1Eq *m)
{
    if (!m) return;
    memset(m->z, 0, sizeof(m->z));
    memcpy(m->cur, m->tgt, sizeof(m->cur));
    memset(m->inc, 0, sizeof(m->inc));
    m->ramp_left = 0;
}

void e1_eq_set(E1Eq *m, float preamp_db, const float gains_db[E1FX_EQ_BANDS])
{
    if (!m || !gains_db) return;
    float pre = clamp_db(preamp_db, 12.f);
    float g[NB];
    int changed = (pre != m->t_pre);
    for (int b = 0; b < NB; ++b) {
        g[b] = clamp_db(gains_db[b], 24.f);
        if (g[b] != m->t_gain[b]) changed = 1;
    }
    if (!changed) return;
    m->t_pre = pre;
    memcpy(m->t_gain, g, sizeof(g));
    double pre_lin = (pre == 0.f) ? 1.0 : e1_db2lin(pre);
    for (int b = 0; b < NB; ++b) {
        design_band(m, b, g[b], b == 0 ? pre_lin : 1.0, &m->tgt[b]);
        m->tgt_ident[b] = coef_is_ident(&m->tgt[b]);
        const double k = 1.0 / (double)m->ramp_len;
        m->inc[b].b0 = (m->tgt[b].b0 - m->cur[b].b0) * k;
        m->inc[b].b1 = (m->tgt[b].b1 - m->cur[b].b1) * k;
        m->inc[b].b2 = (m->tgt[b].b2 - m->cur[b].b2) * k;
        m->inc[b].a1 = (m->tgt[b].a1 - m->cur[b].a1) * k;
        m->inc[b].a2 = (m->tgt[b].a2 - m->cur[b].a2) * k;
    }
    m->ramp_left = m->ramp_len;
}

int e1_eq_is_flat(const E1Eq *m)
{
    if (!m) return 1;
    if (m->ramp_left > 0 || m->t_pre != 0.f) return 0;
    for (int b = 0; b < NB; ++b)
        if (m->t_gain[b] != 0.f) return 0;
    return 1;
}

unsigned e1_eq_latency(const E1Eq *m) { (void)m; return 0; }

/* Steady section over w[0..n). */
static void run_static(const E1BqCoef *c, double *z, double *w, int n)
{
    const double b0 = c->b0, b1 = c->b1, b2 = c->b2, a1 = c->a1, a2 = c->a2;
    double z1 = z[0], z2 = z[1];
    for (int i = 0; i < n; ++i) {
        double x = w[i];
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        w[i] = y;
    }
    z[0] = z1; z[1] = z2;
}

/* Ramping section: sample i uses c0 + inc*(i+1) for i < r, then the target. */
static void run_ramp(const E1BqCoef *c0, const E1BqCoef *inc, const E1BqCoef *t, double *z, double *w, int n, int r)
{
    double z1 = z[0], z2 = z[1];
    int i = 0;
    for (; i < r && i < n; ++i) {
        double k = (double)(i + 1);
        double b0 = c0->b0 + inc->b0 * k, b1 = c0->b1 + inc->b1 * k, b2 = c0->b2 + inc->b2 * k;
        double a1 = c0->a1 + inc->a1 * k, a2 = c0->a2 + inc->a2 * k;
        if (i == r - 1) { b0 = t->b0; b1 = t->b1; b2 = t->b2; a1 = t->a1; a2 = t->a2; }
        double x = w[i];
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        w[i] = y;
    }
    z[0] = z1; z[1] = z2;
    if (i < n) run_static(t, z, w + i, n - i);
}

/* Two channels through the same section in one loop: the two recursions are independent, so
   their latencies overlap (about 2x faster than two passes). */
static void run_static2(const E1BqCoef *c, double *za, double *zb, double *wa, double *wb, int n)
{
    const double b0 = c->b0, b1 = c->b1, b2 = c->b2, a1 = c->a1, a2 = c->a2;
    double p1 = za[0], p2 = za[1], q1 = zb[0], q2 = zb[1];
    for (int i = 0; i < n; ++i) {
        double x = wa[i], v = wb[i];
        double y = b0 * x + p1, u = b0 * v + q1;
        p1 = b1 * x - a1 * y + p2;  q1 = b1 * v - a1 * u + q2;
        p2 = b2 * x - a2 * y;       q2 = b2 * v - a2 * u;
        wa[i] = y; wb[i] = u;
    }
    za[0] = p1; za[1] = p2; zb[0] = q1; zb[1] = q2;
}

static void run_ramp2(const E1BqCoef *c0, const E1BqCoef *inc, const E1BqCoef *t, double *za, double *zb,
                      double *wa, double *wb, int n, int r)
{
    double p1 = za[0], p2 = za[1], q1 = zb[0], q2 = zb[1];
    int i = 0;
    for (; i < r && i < n; ++i) {
        double k = (double)(i + 1);
        double b0 = c0->b0 + inc->b0 * k, b1 = c0->b1 + inc->b1 * k, b2 = c0->b2 + inc->b2 * k;
        double a1 = c0->a1 + inc->a1 * k, a2 = c0->a2 + inc->a2 * k;
        if (i == r - 1) { b0 = t->b0; b1 = t->b1; b2 = t->b2; a1 = t->a1; a2 = t->a2; }
        double x = wa[i], v = wb[i];
        double y = b0 * x + p1, u = b0 * v + q1;
        p1 = b1 * x - a1 * y + p2;  q1 = b1 * v - a1 * u + q2;
        p2 = b2 * x - a2 * y;       q2 = b2 * v - a2 * u;
        wa[i] = y; wb[i] = u;
    }
    za[0] = p1; za[1] = p2; zb[0] = q1; zb[1] = q2;
    if (i < n) run_static2(t, za, zb, wa + i, wb + i, n - i);
}

static void process_chunk(E1Eq *m, float *const *ch, int nch, int n)
{
    double w[2][E1DSP_BLOCK];
    const int r = m->ramp_left;
    for (int c = 0; c < nch; c += 2) {
        const int nc = (c + 1 < nch) ? 2 : 1;
        int any = 0;
        for (int b = 0; b < NB; ++b) {
            double *za = m->z[c][b], *zb = m->z[c + nc - 1][b];
            if (r == 0 && m->tgt_ident[b] && za[0] == 0.0 && za[1] == 0.0 && zb[0] == 0.0 && zb[1] == 0.0) continue;
            if (!any) {
                for (int k = 0; k < nc; ++k)
                    for (int i = 0; i < n; ++i) w[k][i] = (double)ch[c + k][i];
                any = 1;
            }
            if (nc == 2) {
                if (r > 0) run_ramp2(&m->cur[b], &m->inc[b], &m->tgt[b], za, zb, w[0], w[1], n, r);
                else run_static2(&m->cur[b], za, zb, w[0], w[1], n);
            } else {
                if (r > 0) run_ramp(&m->cur[b], &m->inc[b], &m->tgt[b], za, w[0], n, r);
                else run_static(&m->cur[b], za, w[0], n);
            }
            for (int k = 0; k < nc; ++k) {
                double *z = m->z[c + k][b];
                z[0] = e1_flush(z[0]);
                z[1] = e1_flush(z[1]);
            }
        }
        if (any)
            for (int k = 0; k < nc; ++k)
                for (int i = 0; i < n; ++i) /* no subnormal output */
                    ch[c + k][i] = (fabs(w[k][i]) < 1e-30) ? 0.0f : (float)w[k][i];
    }
    if (r > 0) {
        if (n >= r) {
            memcpy(m->cur, m->tgt, sizeof(m->cur));
            m->ramp_left = 0;
        } else {
            for (int b = 0; b < NB; ++b) {
                m->cur[b].b0 += m->inc[b].b0 * n; m->cur[b].b1 += m->inc[b].b1 * n;
                m->cur[b].b2 += m->inc[b].b2 * n; m->cur[b].a1 += m->inc[b].a1 * n;
                m->cur[b].a2 += m->inc[b].a2 * n;
            }
            m->ramp_left = r - n;
        }
    }
}

void e1_eq_process(E1Eq *m, float *const *ch, int nch, int n)
{
    if (!m || !ch || n <= 0) return;
    if (nch > m->nch) nch = m->nch;
    float *p[E1DSP_MAX_CH];
    for (int off = 0; off < n; off += E1DSP_BLOCK) {
        int k = n - off < E1DSP_BLOCK ? n - off : E1DSP_BLOCK;
        for (int c = 0; c < nch; ++c) p[c] = ch[c] + off;
        process_chunk(m, p, nch, k);
    }
}
