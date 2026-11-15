/*
 * test_bass.c - Bass enhancer: flat at level 0, <= +12 dB effective boost, harmonics present,
 * smooth parameter changes, silent tail.
 */
#include "test_common.h"

typedef struct { double gain_db; double rms_gain_db; double h2_db; } BassMeas;

static BassMeas run_sine(double fs, float level, float xover, int hp, double f, double amp)
{
    E1Bass *m = e1_bass_create(fs, 1);
    e1_bass_set(m, level, xover, hp);
    e1_bass_reset(m);
    const int settle = (int)(fs * 1.0), meas = (int)(fs * 1.0), total = settle + meas;
    float *x = (float *)malloc((size_t)total * sizeof(float));
    for (int i = 0; i < total; ++i) x[i] = (float)(amp * sin(2 * E1_PI * f * i / fs));
    float *ch[1];
    for (int off = 0; off < total; off += E1DSP_BLOCK) {
        ch[0] = x + off;
        e1_bass_process(m, ch, 1, total - off < E1DSP_BLOCK ? total - off : E1DSP_BLOCK);
    }
    BassMeas r;
    double a = sine_amp(x + settle, (size_t)meas, 1, fs, f);
    double e = 0;
    for (int i = settle; i < total; ++i) e += (double)x[i] * x[i];
    r.gain_db = db(a / amp);
    r.rms_gain_db = db(sqrt(e / meas) / (amp / sqrt(2.0)));
    r.h2_db = db(sine_amp(x + settle, (size_t)meas, 1, fs, 2 * f) / (a > 0 ? a : 1));
    free(x);
    e1_bass_destroy(m);
    return r;
}

static void test_level0_flat(double fs)
{
    double worst = 0;
    for (double f = 10; f < 0.45 * fs; f *= 1.6) {
        BassMeas r = run_sine(fs, 0.f, 80.f, 1, f, 0.5);
        worst = fmax(worst, fabs(r.gain_db));
        CHECK(fabs(r.gain_db) < 0.05, "fs %g level 0: %.3f dB at %.1f Hz (LR4 sum must be flat)", fs, r.gain_db, f);
    }
    printf("  fs %.0f level 0: max deviation from flat %.4f dB\n", fs, worst);
}

static void test_max_boost(double fs)
{
    static const float xovers[] = { 10.f, 40.f, 80.f, 150.f, 300.f, 1000.f };
    double worst = -100, worst_f = 0, worst_x = 0;
    for (int hp = 0; hp < 2; ++hp)
        for (size_t xi = 0; xi < sizeof xovers / sizeof xovers[0]; ++xi)
            for (double f = 8; f < 4000; f *= 1.25) {
                BassMeas r = run_sine(fs, 100.f, xovers[xi], hp, f, 0.2);
                double g = fmax(r.gain_db, r.rms_gain_db);
                if (g > worst) { worst = g; worst_f = f; worst_x = xovers[xi]; }
                CHECK(g <= 12.0, "fs %g hp %d xover %g: %.2f dB at %.1f Hz exceeds +12 dB", fs, hp, xovers[xi], g, f);
            }
    printf("  fs %.0f level 100: max effective boost %.2f dB (at %.1f Hz, xover %.0f)\n", fs, worst, worst_f, worst_x);
}

static void test_boost_and_harmonics(void)
{
    const double fs = 44100;
    for (int hp = 0; hp < 2; ++hp) {
        double fb = 0.875 * 80;
        BassMeas r = run_sine(fs, 100.f, 80.f, hp, fb, 0.2);
        CHECK(r.gain_db > 5.0, "hp %d: boost at fb only %.2f dB", hp, r.gain_db);
        BassMeas r50 = run_sine(fs, 50.f, 80.f, hp, fb, 0.2);
        CHECK(r50.gain_db > 2.0 && r50.gain_db < r.gain_db, "hp %d: level 50 boost %.2f dB", hp, r50.gain_db);
        BassMeas h = run_sine(fs, 100.f, 80.f, hp, 50.0, 0.2);
        CHECK(h.h2_db > -40.0 && h.h2_db < -6.0, "hp %d: 2nd harmonic %.1f dB re fundamental", hp, h.h2_db);
        BassMeas hi = run_sine(fs, 100.f, 80.f, hp, 1000.0, 0.2);
        CHECK(fabs(hi.gain_db) < 0.1, "hp %d: 1 kHz must be untouched (%.3f dB)", hp, hi.gain_db);
        printf("  voicing %s: +%.2f dB at fb (level 100), +%.2f dB (level 50), H2 at 50 Hz %.1f dB\n",
               hp ? "headphones" : "speakers", r.gain_db, r50.gain_db, h.h2_db);
    }
}

static void test_smooth_changes(void)
{
    const double fs = 44100;
    const int n = (int)fs * 2;
    float *x = (float *)malloc((size_t)n * sizeof(float));
    for (int i = 0; i < n; ++i) x[i] = (float)(0.25 * sin(2 * E1_PI * 60.0 * i / fs));
    E1Bass *m = e1_bass_create(fs, 1);
    e1_bass_set(m, 0.f, 80.f, 1);
    e1_bass_reset(m);
    float *ch[1];
    for (int off = 0; off + E1DSP_BLOCK <= n; off += E1DSP_BLOCK) {
        if (off == 128 * 100) e1_bass_set(m, 100.f, 80.f, 1);
        if (off == 128 * 200) e1_bass_set(m, 100.f, 300.f, 0);
        if (off == 128 * 250) e1_bass_set(m, 30.f, 20.f, 1);
        ch[0] = x + off;
        e1_bass_process(m, ch, 1, E1DSP_BLOCK);
    }
    double maxd = 0, pk = 0;
    for (int i = 1; i < 128 * 600; ++i) { maxd = fmax(maxd, fabs((double)x[i] - x[i - 1])); pk = fmax(pk, fabs(x[i])); }
    /* a 60 Hz sine of peak pk moves at most 2*sin(pi*60/fs)*pk per sample; allow harmonics (x3) */
    double bound = 2 * sin(E1_PI * 60.0 * 3 / fs) * pk;
    CHECK(maxd <= bound, "bass parameter change step %.5f > %.5f", maxd, bound);
    printf("  parameter sweep: max step %.5f (bound %.5f), peak %.3f\n", maxd, bound, pk);
    free(x);
    e1_bass_destroy(m);
}

static void test_tail(double fs)
{
    E1Bass *m = e1_bass_create(fs, 2);
    e1_bass_set(m, 100.f, 120.f, 0);
    e1_bass_reset(m);
    float l[E1DSP_BLOCK], r[E1DSP_BLOCK];
    float *ch[2] = { l, r };
    int finite = 1, sub = 0, nz = 0;
    rng_seed(5);
    for (int off = 0; off < (int)fs; off += E1DSP_BLOCK) {
        for (int i = 0; i < E1DSP_BLOCK; ++i) { l[i] = rng_sym(); r[i] = rng_sym(); }
        e1_bass_process(m, ch, 2, E1DSP_BLOCK);
        for (int i = 0; i < E1DSP_BLOCK; ++i) finite &= isfinite(l[i]) && isfinite(r[i]);
    }
    const int tail = (int)(fs * 2);
    for (int off = 0; off < tail; off += E1DSP_BLOCK) {
        memset(l, 0, sizeof l); memset(r, 0, sizeof r);
        e1_bass_process(m, ch, 2, E1DSP_BLOCK);
        for (int i = 0; i < E1DSP_BLOCK; ++i) {
            sub += is_subnormal_f(l[i]) || is_subnormal_f(r[i]);
            if (off + 2 * E1DSP_BLOCK >= tail) nz += l[i] != 0.f || r[i] != 0.f;
        }
    }
    CHECK(finite, "fs %g: non-finite bass output", fs);
    CHECK(sub == 0, "fs %g: %d subnormal samples", fs, sub);
    CHECK(nz == 0, "fs %g: bass tail not exactly zero after 2 s", fs);
    e1_bass_destroy(m);
}

int main(void)
{
    printf("test_bass\n");
    test_level0_flat(44100);
    test_level0_flat(48000);
    test_max_boost(44100);
    test_max_boost(48000);
    test_boost_and_harmonics();
    test_smooth_changes();
    test_tail(44100);
    test_tail(8000);
    test_tail(192000);
    return test_summary("test_bass");
}
