/*
 * test_eq.c - graphic EQ: magnitude at band centres, flat identity, ramps, stability, tails.
 */
#include "test_common.h"

static const double k_f[E1FX_EQ_BANDS] = E1FX_EQ_FREQS;

/* Independent model of the intended design (spec 3.7) for the expected magnitude. */
static double model_mag_db(double fs, double pre_db, const float *g, double f)
{
    double m = pre_db;
    for (int b = 0; b < E1FX_EQ_BANDS; ++b) {
        if (g[b] == 0.f) continue;
        double fc = k_f[b] > 0.45 * fs ? 0.45 * fs : k_f[b];
        E1BqCoef c;
        if (b == 0) e1_bq_lowshelf(&c, fs, fc, sqrt(0.5), g[b]);
        else if (b == 9) e1_bq_highshelf(&c, fs, fc, sqrt(0.5), g[b]);
        else {
            double w0 = 2 * E1_PI * fc / fs;
            double q = 1.0 / (2.0 * sinh(log(2.0) / 2.0 * w0 / sin(w0)));
            e1_bq_peaking(&c, fs, fc, q, g[b]);
        }
        m += db(e1_bq_mag(&c, fs, f));
    }
    return m;
}

/* Measured steady-state gain (dB) of the module at f. */
static double measure_db(double fs, float pre, const float *g, double f)
{
    E1Eq *eq = e1_eq_create(fs, 2);
    e1_eq_set(eq, pre, g);
    e1_eq_reset(eq);
    const int settle = (int)(fs * 1.0), meas = (int)(fs * 0.5), total = settle + meas;
    float *l = (float *)malloc((size_t)total * sizeof(float)), *r = (float *)malloc((size_t)total * sizeof(float));
    for (int i = 0; i < total; ++i) l[i] = r[i] = (float)(0.25 * sin(2 * E1_PI * f * i / fs));
    for (int off = 0; off < total; off += E1DSP_BLOCK) {
        int n = total - off < E1DSP_BLOCK ? total - off : E1DSP_BLOCK;
        float *ch[2] = { l + off, r + off };
        e1_eq_process(eq, ch, 2, n);
    }
    double a = sine_amp(l + settle, (size_t)meas, 1, fs, f), b = sine_amp(r + settle, (size_t)meas, 1, fs, f);
    CHECK(fabs(a - b) < 1e-6, "channels differ at %g Hz", f);
    free(l); free(r);
    e1_eq_destroy(eq);
    return db(a / 0.25);
}

static void test_single_bands(double fs)
{
    static const float gains[] = { 6.f, -6.f, 12.f, -12.f, 24.f, -24.f, 3.f };
    double worst = 0;
    for (int b = 1; b <= 8; ++b) {
        for (size_t gi = 0; gi < sizeof gains / sizeof gains[0]; ++gi) {
            float g[E1FX_EQ_BANDS] = { 0 };
            g[b] = gains[gi];
            double fc = k_f[b] > 0.45 * fs ? 0.45 * fs : k_f[b];
            double m = measure_db(fs, 0.f, g, fc);
            double err = fabs(m - gains[gi]);
            if (err > worst) worst = err;
            CHECK(err <= 0.25, "fs %g band %d gain %+g: measured %+.3f dB at %g Hz", fs, b, gains[gi], m, fc);
        }
    }
    /* shelves: half gain at the centre, full gain far into the shelf */
    for (int sb = 0; sb < 2; ++sb) {
        int b = sb ? 9 : 0;
        float g[E1FX_EQ_BANDS] = { 0 };
        g[b] = 12.f;
        double fc = k_f[b] > 0.45 * fs ? 0.45 * fs : k_f[b];
        double m = measure_db(fs, 0.f, g, fc), want = model_mag_db(fs, 0, g, fc);
        CHECK(fabs(m - want) <= 0.25 && fabs(want - 6.0) < 0.05, "fs %g shelf %d centre: %.3f dB (model %.3f)", fs, b, m, want);
        double ff = sb ? 0.49 * fs : 5.0;
        m = measure_db(fs, 0.f, g, ff);
        CHECK(m > 10.0 && m <= 12.01, "fs %g shelf %d deep: %.3f dB at %g Hz", fs, b, m, ff);
    }
    printf("  fs %.0f: single peaking bands worst error %.4f dB\n", fs, worst);
}

static void test_curves(double fs)
{
    static const float scout[E1FX_EQ_BANDS] = E1FX_SCOUT_EQ;
    double worst = 0;
    for (int p = 0; p <= e1_eq_preset_count; ++p) {
        const float *g = p < e1_eq_preset_count ? e1_eq_presets[p].gain_db : scout;
        float pre = (p % 3 == 1) ? -3.f : (p % 3 == 2 ? 2.5f : 0.f);
        for (int b = 0; b < E1FX_EQ_BANDS; ++b) {
            double fc = k_f[b] > 0.45 * fs ? 0.45 * fs : k_f[b];
            double m = measure_db(fs, pre, g, fc), want = model_mag_db(fs, pre, g, fc);
            double err = fabs(m - want);
            if (err > worst) worst = err;
            CHECK(err <= 0.25, "fs %g curve %s band %d: %.3f dB, target %.3f", fs,
                  p < e1_eq_preset_count ? e1_eq_presets[p].name : "Scout", b, m, want);
        }
    }
    /* extreme curve */
    float g[E1FX_EQ_BANDS];
    for (int b = 0; b < E1FX_EQ_BANDS; ++b) g[b] = (b & 1) ? 24.f : -24.f;
    for (int b = 0; b < E1FX_EQ_BANDS; ++b) {
        double fc = k_f[b] > 0.45 * fs ? 0.45 * fs : k_f[b];
        double m = measure_db(fs, -12.f, g, fc), want = model_mag_db(fs, -12, g, fc);
        double err = fabs(m - want);
        if (err > worst) worst = err;
        CHECK(err <= 0.25, "fs %g zigzag band %d: %.3f dB, target %.3f", fs, b, m, want);
    }
    printf("  fs %.0f: presets/Scout/zigzag curves worst error %.4f dB\n", fs, worst);
}

static void test_flat_identity(void)
{
    const int n = 44100;
    float *x = (float *)malloc(n * sizeof(float)), *y = (float *)malloc(n * sizeof(float));
    rng_seed(7);
    for (int i = 0; i < n; ++i) x[i] = rng_sym();
    E1Eq *eq = e1_eq_create(44100, 1);
    float zero[E1FX_EQ_BANDS] = { 0 };
    e1_eq_set(eq, 0.f, zero);
    memcpy(y, x, n * sizeof(float));
    float *ch[1] = { y };
    for (int off = 0; off < n; off += E1DSP_BLOCK) { ch[0] = y + off; e1_eq_process(eq, ch, 1, n - off < E1DSP_BLOCK ? n - off : E1DSP_BLOCK); }
    double md = 0;
    for (int i = 0; i < n; ++i) md = fmax(md, fabs((double)y[i] - x[i]));
    CHECK(md <= 1e-6, "flat EQ max deviation %g", md);
    CHECK(memcmp(x, y, n * sizeof(float)) == 0, "flat EQ is not bit-exact");
    CHECK(e1_eq_is_flat(eq), "is_flat should be 1");

    /* non-flat -> flat: after the ramp and two samples of drain it is exact again */
    float g[E1FX_EQ_BANDS] = { 3, -4, 5, 6, -7, 8, 2, 1, -3, 4 };
    e1_eq_set(eq, 2.f, g);
    CHECK(!e1_eq_is_flat(eq), "is_flat should be 0 while non-flat");
    memcpy(y, x, n * sizeof(float));
    for (int off = 0; off < 22016; off += E1DSP_BLOCK) { ch[0] = y + off; e1_eq_process(eq, ch, 1, E1DSP_BLOCK); }
    e1_eq_set(eq, 0.f, zero);
    int off = 22016;
    for (int k = 0; k < 8; ++k, off += E1DSP_BLOCK) { ch[0] = y + off; e1_eq_process(eq, ch, 1, E1DSP_BLOCK); }
    CHECK(e1_eq_is_flat(eq), "is_flat after 10 ms ramp");
    /* 8 blocks = 23 ms > 10 ms ramp: state of identity sections drained after 2 samples */
    memcpy(y + off, x + off, (size_t)(n - off) * sizeof(float));
    for (; off + E1DSP_BLOCK <= n; off += E1DSP_BLOCK) { ch[0] = y + off; e1_eq_process(eq, ch, 1, E1DSP_BLOCK); }
    int exact = memcmp(y + 22016 + 8 * E1DSP_BLOCK + E1DSP_BLOCK, x + 22016 + 8 * E1DSP_BLOCK + E1DSP_BLOCK,
                       (size_t)(off - (22016 + 9 * E1DSP_BLOCK)) * sizeof(float)) == 0;
    CHECK(exact, "EQ back to flat is not bit-exact");
    e1_eq_destroy(eq);
    free(x); free(y);
}

static void test_ramp_smooth(void)
{
    const double fs = 44100;
    const int n = (int)fs;
    float *y = (float *)malloc(n * sizeof(float));
    for (int i = 0; i < n; ++i) y[i] = (float)(0.25 * sin(2 * E1_PI * 1000.0 * i / fs));
    E1Eq *eq = e1_eq_create(fs, 1);
    float g[E1FX_EQ_BANDS] = { 0 };
    float *ch[1];
    double maxd = 0;
    for (int off = 0; off + E1DSP_BLOCK <= n; off += E1DSP_BLOCK) {
        if (off == 128 * 40) { g[5] = 12.f; e1_eq_set(eq, 0.f, g); }
        if (off == 128 * 80) { g[5] = -12.f; e1_eq_set(eq, 6.f, g); }
        if (off == 128 * 83) { g[5] = 24.f; e1_eq_set(eq, -12.f, g); } /* change during a ramp */
        ch[0] = y + off;
        e1_eq_process(eq, ch, 1, E1DSP_BLOCK);
    }
    for (int i = 1; i < 128 * 300; ++i) maxd = fmax(maxd, fabs((double)y[i] - y[i - 1]));
    /* steepest legitimate slope: 0.25 * 10^(24/20) at 1 kHz = 3.97 amplitude -> 2*sin(pi*1000/fs)*3.97 = 0.565 */
    double bound = 2 * sin(E1_PI * 1000.0 / fs) * 0.25 * pow(10, 24 / 20.0) * 1.05;
    CHECK(maxd <= bound, "EQ ramp max step %.4f > %.4f", maxd, bound);
    printf("  ramp: max sample step %.4f (bound %.4f)\n", maxd, bound);
    e1_eq_destroy(eq);
    free(y);
}

static void test_stability_and_tail(double fs)
{
    E1Eq *eq = e1_eq_create(fs, 2);
    float g[E1FX_EQ_BANDS];
    for (int b = 0; b < E1FX_EQ_BANDS; ++b) g[b] = 24.f;
    e1_eq_set(eq, 12.f, g);
    const int n = (int)(fs * 0.5), tail = (int)(fs * 2.0);
    float l[E1DSP_BLOCK], r[E1DSP_BLOCK];
    float *ch[2] = { l, r };
    int finite = 1, sub = 0;
    rng_seed(99);
    for (int off = 0; off < n; off += E1DSP_BLOCK) {
        for (int i = 0; i < E1DSP_BLOCK; ++i) { l[i] = rng_sym(); r[i] = rng_sym() * 0.5f; }
        e1_eq_process(eq, ch, 2, E1DSP_BLOCK);
        for (int i = 0; i < E1DSP_BLOCK; ++i) finite &= isfinite(l[i]) && isfinite(r[i]);
        if (off == 128 * 20) { for (int b = 0; b < E1FX_EQ_BANDS; ++b) g[b] = (b & 1) ? -24.f : 24.f; e1_eq_set(eq, -12.f, g); }
    }
    int nonzero_at_end = 0;
    for (int off = 0; off < tail; off += E1DSP_BLOCK) {
        memset(l, 0, sizeof l); memset(r, 0, sizeof r);
        e1_eq_process(eq, ch, 2, E1DSP_BLOCK);
        for (int i = 0; i < E1DSP_BLOCK; ++i) {
            sub += is_subnormal_f(l[i]) || is_subnormal_f(r[i]);
            if (off + E1DSP_BLOCK >= tail - E1DSP_BLOCK) nonzero_at_end += (l[i] != 0.f) || (r[i] != 0.f);
        }
    }
    CHECK(finite, "fs %g extreme EQ produced non-finite output", fs);
    CHECK(sub == 0, "fs %g: %d subnormal output samples in the tail", fs, sub);
    CHECK(nonzero_at_end == 0, "fs %g: EQ tail not exactly zero after 2 s", fs);
    e1_eq_destroy(eq);
}

int main(void)
{
    printf("test_eq\n");
    test_single_bands(44100);
    test_single_bands(48000);
    test_curves(44100);
    test_curves(48000);
    test_flat_identity();
    test_ramp_smooth();
    test_stability_and_tail(8000);
    test_stability_and_tail(44100);
    test_stability_and_tail(384000);
    return test_summary("test_eq");
}
