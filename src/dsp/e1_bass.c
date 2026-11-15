/*
 * e1_bass.c - "Bass" low-frequency enhancer (spec 3.8, X-Bass-like).
 *
 * Signal flow per channel (all filters double precision, zero latency):
 *
 *   x ─┬─ LP4(fc) ──┬──────────────────────────────────────────────────────┐
 *      │            └─ BP(fb) = b ─┬─ ×gb ─────────────────────────────────┐ │
 *      │                           └─ harmonic gen ─ HP2(fb) ─ LP2(4.5fb) ─ ×gh ┐
 *      └─ HP4(fc) ───────────────────────────────────────────────────────── Σ ──► y
 *
 *   LP4/HP4: Linkwitz-Riley 4th order (two cascaded Butterworth biquads each); LP4 + HP4 is an
 *            all-pass, so with the enhancement at 0 the magnitude response is exactly flat.
 *   b:       0 dB-peak band-pass of the low band centred on fb = 0.875*fc. Adding gb*b to the
 *            low band is a peaking boost of 20*log10(1+gb) dB at fb (parallel form, so the boost
 *            amount is smoothed per sample without recomputing coefficients).
 *   harmonics: b is normalised by its own peak envelope (|u| <= 1) and shaped by an asymmetric
 *            cubic soft-clip polynomial written in Chebyshev form,
 *              r(u) = c2*T2(u) - c3*T3(u) = c2*(2u^2 - 1) - c3*(4u^3 - 3u),
 *            i.e. the clipper's non-linear part with its linear/DC terms chosen so that a steady
 *            sine yields only its 2nd and 3rd harmonics (no fundamental leakage, no aliasing since
 *            the band is low). The result is re-scaled by the envelope (level independent
 *            harmonic ratio), high-passed at fb (DC / intermodulation below the band) and
 *            low-passed at 4.5*fb. These "missing fundamental" cues are reproduced better by
 *            small drivers and closed headphones than the fundamental itself.
 *
 * Boost bound: |LP4|+|HP4| = 1 (in phase), |b| <= |LP4|, |r| <= c2 + c3 for |u| <= 1, so the
 * steady-state gain is <= 1 + gb + gh*(c2+c3) <= 3.66 (+11.3 dB) at level 100 (speakers),
 * <= 3.06 (+9.7 dB) for headphones: never more than +12 dB.
 *
 * Voicing: headphones = broader, gentler boost with fewer harmonics; speakers (2.0/2.1) =
 * tighter boost and more harmonic content (small drivers roll off the fundamental).
 * Smoothing: level 50 ms (per sample), crossover 50 ms (per block, coefficients interpolated
 * per sample across the block).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "e1dsp_modules.h"
#include "e1_biquad.h"

enum { F_LP = 0, F_HP, F_BP, F_HH, F_HL, F_COUNT };

typedef struct BassCh {
    double s[7][2];     /* states: LP4 (2), HP4 (2), BP, harmonic HP, harmonic LP */
    double env;
} BassCh;

struct E1Bass {
    double   fs;
    int      nch;
    /* targets */
    double   t_level;   /* 0..1 */
    double   t_fc;
    int      hp_voicing;
    /* smoothed */
    double   fc;        /* smoothed crossover */
    double   gb, gh;    /* smoothed gains */
    double   t_gb, t_gh;
    double   c2, c3;    /* harmonic polynomial weights (voicing) */
    double   a_gain;    /* per-sample one-pole coefficient (50 ms) */
    double   a_fc;      /* per-block coefficient (50 ms) */
    double   env_rel;   /* envelope release per sample */
    E1BqCoef co[F_COUNT];
    int      have_co;   /* co[] matches fc / co_hp */
    int      co_hp;
    BassCh   ch[E1DSP_MAX_CH];
};

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static double band_centre(double fc)
{
    double fb = 0.875 * fc;
    if (fb < 40.0) fb = (40.0 < 0.95 * fc) ? 40.0 : 0.95 * fc;
    return fb;
}

static void design(const E1Bass *m, double fc, int hp_voicing, E1BqCoef co[F_COUNT])
{
    const double fs = m->fs, nyq_lim = 0.45 * fs;
    const double q = 0.70710678118654752;
    double f = clampd(fc, 10.0, nyq_lim);
    double fb = band_centre(f);
    e1_bq_lowpass(&co[F_LP], fs, f, q);
    e1_bq_highpass(&co[F_HP], fs, f, q);
    double qb = hp_voicing ? 0.8 : clampd(fb / 40.0, 0.7, 1.4);
    e1_bq_bandpass(&co[F_BP], fs, fb, qb);
    e1_bq_highpass(&co[F_HH], fs, clampd(fb, 10.0, nyq_lim), q);
    e1_bq_lowpass(&co[F_HL], fs, clampd(4.5 * fb, 20.0, nyq_lim), q);
}

/* envelope release: spans at least two periods of the band centre, >= 20 ms */
static double env_release(const E1Bass *m)
{
    double tau = 2.0 / band_centre(m->fc);
    if (tau < 0.020) tau = 0.020;
    return exp(-1.0 / (tau * m->fs));
}

static void update_targets(E1Bass *m)
{
    double maxdb = m->hp_voicing ? 9.0 : 10.0;
    m->t_gb = pow(10.0, m->t_level * maxdb / 20.0) - 1.0;
    m->t_gh = m->t_level * (m->hp_voicing ? 0.30 : 0.50);
    if (m->hp_voicing) { m->c2 = 0.60; m->c3 = 0.20; } /* warmer: mostly even harmonics */
    else               { m->c2 = 0.60; m->c3 = 0.40; }
}

E1Bass *e1_bass_create(double fs, int nch)
{
    if (!(fs >= 8000.0 && fs <= 384000.0) || nch < 1 || nch > E1DSP_MAX_CH) return NULL;
    E1Bass *m = (E1Bass *)calloc(1, sizeof(E1Bass));
    if (!m) return NULL;
    m->fs = fs;
    m->nch = nch;
    m->t_level = 0.5;
    m->t_fc = 80.0;
    m->hp_voicing = 1;
    m->a_gain = e1_onepole_coef(0.050, fs);
    m->a_fc = e1_onepole_coef(0.050, fs / E1DSP_BLOCK);
    update_targets(m);
    e1_bass_reset(m);
    return m;
}

void e1_bass_destroy(E1Bass *m) { free(m); }

void e1_bass_reset(E1Bass *m)
{
    if (!m) return;
    memset(m->ch, 0, sizeof(m->ch));
    m->fc = m->t_fc;
    m->gb = m->t_gb;
    m->gh = m->t_gh;
    design(m, m->fc, m->hp_voicing, m->co);
    m->co_hp = m->hp_voicing;
    m->have_co = 1;
    m->env_rel = env_release(m);
}

void e1_bass_set(E1Bass *m, float level_0_100, float xover_hz, int headphones)
{
    if (!m) return;
    double lv = isfinite(level_0_100) ? clampd(level_0_100, 0.0, 100.0) / 100.0 : 0.5;
    double fc = isfinite(xover_hz) ? clampd(xover_hz, 10.0, 1000.0) : 80.0;
    m->t_level = lv;
    m->t_fc = fc;
    m->hp_voicing = headphones ? 1 : 0;
    update_targets(m);
}

unsigned e1_bass_latency(const E1Bass *m) { (void)m; return 0; }

#define BQ(c, st, x, y) do { double yy_ = (c).b0 * (x) + (st)[0]; \
    (st)[0] = (c).b1 * (x) - (c).a1 * yy_ + (st)[1]; (st)[1] = (c).b2 * (x) - (c).a2 * yy_; (y) = yy_; } while (0)

static void process_chunk(E1Bass *m, float *const *chp, int nch, int n)
{
    /* crossover smoothing (log domain, once per chunk); coefficients are interpolated per
       sample from the previous chunk's set to the new one */
    E1BqCoef c0[F_COUNT], inc[F_COUNT];
    int ramp = 0;
    if (m->fc != m->t_fc) {
        double lf = log(m->fc), lt = log(m->t_fc);
        double nf = exp(lt + (lf - lt) * m->a_fc);
        if (fabs(nf - m->t_fc) < 1e-4 * m->t_fc) nf = m->t_fc;
        m->fc = nf;
        m->have_co = 0;
    }
    if (m->hp_voicing != m->co_hp) m->have_co = 0;
    if (!m->have_co) {
        E1BqCoef nco[F_COUNT];
        design(m, m->fc, m->hp_voicing, nco);
        m->co_hp = m->hp_voicing;
        m->have_co = 1;
        memcpy(c0, m->co, sizeof(c0));
        const double k = 1.0 / n;
        for (int f = 0; f < F_COUNT; ++f) {
            inc[f].b0 = (nco[f].b0 - c0[f].b0) * k; inc[f].b1 = (nco[f].b1 - c0[f].b1) * k;
            inc[f].b2 = (nco[f].b2 - c0[f].b2) * k; inc[f].a1 = (nco[f].a1 - c0[f].a1) * k;
            inc[f].a2 = (nco[f].a2 - c0[f].a2) * k;
        }
        memcpy(m->co, nco, sizeof(nco));
        m->env_rel = env_release(m);
        ramp = 1;
    }

    const double ag = m->a_gain, tgb = m->t_gb, tgh = m->t_gh, c2 = m->c2, c3 = m->c3, rel = m->env_rel;
    double gb_end = m->gb, gh_end = m->gh;
    for (int c = 0; c < nch; ++c) {
        BassCh *st = &m->ch[c];
        float *buf = chp[c];
        double gb = m->gb, gh = m->gh, env = st->env;
        E1BqCoef co[F_COUNT];
        memcpy(co, ramp ? c0 : m->co, sizeof(co));
        for (int i = 0; i < n; ++i) {
            if (ramp) {
                for (int f = 0; f < F_COUNT; ++f) {
                    co[f].b0 += inc[f].b0; co[f].b1 += inc[f].b1; co[f].b2 += inc[f].b2;
                    co[f].a1 += inc[f].a1; co[f].a2 += inc[f].a2;
                }
                if (i == n - 1) memcpy(co, m->co, sizeof(co));
            }
            double x = (double)buf[i], lp, hp, b, h;
            BQ(co[F_LP], st->s[0], x, lp);
            BQ(co[F_LP], st->s[1], lp, lp);
            BQ(co[F_HP], st->s[2], x, hp);
            BQ(co[F_HP], st->s[3], hp, hp);
            BQ(co[F_BP], st->s[4], lp, b);
            double ab = fabs(b);
            env = (ab > env) ? ab : env * rel;
            double u = (env > 1e-12) ? b / env : 0.0;
            double u2 = u * u;
            double r = c2 * (2.0 * u2 - 1.0) - c3 * (4.0 * u2 - 3.0) * u;
            h = r * env;
            BQ(co[F_HH], st->s[5], h, h);
            BQ(co[F_HL], st->s[6], h, h);
            gb = tgb + (gb - tgb) * ag;
            gh = tgh + (gh - tgh) * ag;
            double y = hp + lp + gb * b + gh * h;
            buf[i] = (fabs(y) < 1e-30) ? 0.0f : (float)y; /* no subnormal output */
        }
        for (int s = 0; s < 7; ++s) { st->s[s][0] = e1_flush(st->s[s][0]); st->s[s][1] = e1_flush(st->s[s][1]); }
        st->env = (env < 1e-20) ? 0.0 : env;
        gb_end = gb;
        gh_end = gh;
    }
    if (fabs(gb_end - tgb) < 1e-9) gb_end = tgb;
    if (fabs(gh_end - tgh) < 1e-9) gh_end = tgh;
    m->gb = gb_end;
    m->gh = gh_end;
}

void e1_bass_process(E1Bass *m, float *const *ch, int nch, int n)
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
