/*
 * test_limiter.c - look-ahead true-peak limiter: ceiling (sample and inter-sample), latency,
 * transparency below the ceiling, low distortion on steady overdriven sines, silent tail.
 */
#include "test_common.h"

#define CEIL_DB  (-0.3)
#define TOL_DB   0.05

/* Runs a mono/stereo interleaved signal through the limiter in ragged chunks (planar inside). */
static void run_lim(E1Limiter *m, float *x, int nch, size_t frames)
{
    float bufs[2][1024];
    float *ch[2] = { bufs[0], bufs[1] };
    size_t off = 0;
    rng_seed(1234);
    while (off < frames) {
        size_t n = (size_t)rng_int(1, 1024);
        if (n > frames - off) n = frames - off;
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) bufs[c][i] = x[(off + i) * nch + c];
        e1_limiter_process(m, ch, nch, (int)n);
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) x[(off + i) * nch + c] = bufs[c][i];
        off += n;
    }
}

/* strict: also enforce the near-ideal (16x, 128-tap) meter; otherwise it is only reported.
   The BS.1770-style 4x meter and the sample peak are always enforced. */
static void check_ceiling(double fs, const char *what, float *x, int nch, size_t frames, size_t skip, int strict)
{
    double sp = 0, tp = 0, tp4 = 0;
    for (int c = 0; c < nch; ++c) {
        for (size_t i = skip; i < frames; ++i) sp = fmax(sp, fabs((double)x[i * nch + c]));
        tp = fmax(tp, true_peak(x + skip * nch + c, frames - skip, (size_t)nch));
        tp4 = fmax(tp4, true_peak_bs1770(x + c, frames, (size_t)nch));
    }
    CHECK(db(sp) <= CEIL_DB + TOL_DB, "fs %g %s: sample peak %.3f dBFS", fs, what, db(sp));
    CHECK(db(tp4) <= CEIL_DB + TOL_DB, "fs %g %s: 4x true peak %.3f dBTP", fs, what, db(tp4));
    if (strict) CHECK(db(tp) <= CEIL_DB + TOL_DB, "fs %g %s: near-ideal true peak %.3f dBFS", fs, what, db(tp));
    printf("  fs %.0f %-34s sample %+.3f, 4x TP %+.3f, near-ideal TP %+.3f dBFS%s\n", fs, what, db(sp), db(tp4), db(tp),
           strict ? "" : " (info)");
}

static void test_overdrive(double fs)
{
    const double amp = pow(10.0, 12.0 / 20.0); /* +12 dBFS */
    const size_t frames = (size_t)(fs * 2.0), skip = (size_t)(fs * 0.1);
    float *x = (float *)malloc(frames * 2 * sizeof(float));
    static const double freqs[] = { 30, 100, 1000, 3000, 7000, 11025, 15000, 18000 };
    char name[64];
    for (size_t k = 0; k < sizeof freqs / sizeof freqs[0]; ++k) {
        if (freqs[k] > 0.41 * fs) continue;
        E1Limiter *m = e1_limiter_create(fs, 2);
        for (size_t i = 0; i < frames; ++i) {
            double ph = 2 * E1_PI * freqs[k] * (double)i / fs;
            x[2 * i] = (float)(amp * sin(ph + 0.3));
            x[2 * i + 1] = (float)(amp * 0.7 * sin(ph + 1.1));
        }
        run_lim(m, x, 2, frames);
        /* the abrupt start of the test tone is itself a full-band transient: steady state is strict */
        snprintf(name, sizeof name, "+12 dB sine %.0f Hz", freqs[k]);
        check_ceiling(fs, name, x, 2, frames, skip, 1);
        e1_limiter_destroy(m);
    }
    /* noise band-limited to 0.41 fs (18 kHz @ 44.1k: everything a real programme/DAC carries) */
    {
        E1Limiter *m = e1_limiter_create(fs, 2);
        rng_seed(41);
        for (size_t i = 0; i < 2 * frames; ++i) x[i] = rng_sym();
        lowpass_fir(x, frames, 2, 0.41);
        lowpass_fir(x + 1, frames, 2, 0.41);
        double pk = 0;
        for (size_t i = 0; i < 2 * frames; ++i) pk = fmax(pk, fabs(x[i]));
        for (size_t i = 0; i < 2 * frames; ++i) x[i] = (float)(x[i] * amp / pk);
        run_lim(m, x, 2, frames);
        check_ceiling(fs, "+12 dB noise, 0.41 fs band", x, 2, frames, 0, 1);
        e1_limiter_destroy(m);
    }
    /* band-limited noise bursts (silence -> +12 dB), worst case for the attack */
    {
        E1Limiter *m = e1_limiter_create(fs, 2);
        rng_seed(43);
        for (size_t i = 0; i < frames; ++i) {
            int on = ((i / (size_t)(fs * 0.05)) % 3) == 0;
            float v = on ? rng_sym() : 0.0025f * rng_sym();
            x[2 * i] = v;
            x[2 * i + 1] = -v;
        }
        lowpass_fir(x, frames, 2, 0.41);
        lowpass_fir(x + 1, frames, 2, 0.41);
        double pk = 0;
        for (size_t i = 0; i < 2 * frames; ++i) pk = fmax(pk, fabs(x[i]));
        for (size_t i = 0; i < 2 * frames; ++i) x[i] = (float)(x[i] * amp / pk);
        run_lim(m, x, 2, frames);
        check_ceiling(fs, "+12 dB band-limited noise bursts", x, 2, frames, 0, 1);
        e1_limiter_destroy(m);
    }
    /* full-band white noise (energy up to fs/2) */
    {
        E1Limiter *m = e1_limiter_create(fs, 2);
        rng_seed(42);
        for (size_t i = 0; i < 2 * frames; ++i) x[i] = (float)(amp * rng_sym());
        run_lim(m, x, 2, frames);
        check_ceiling(fs, "+12 dB full-band white noise", x, 2, frames, 0, 0);
        e1_limiter_destroy(m);
    }
    /* log sine sweep 20 Hz .. 0.41 fs at +12 dB (smooth start: 1st 5 ms faded in) */
    {
        E1Limiter *m = e1_limiter_create(fs, 1);
        double f0 = 20, f1 = 0.41 * fs, T = (double)frames / fs, kk = log(f1 / f0);
        for (size_t i = 0; i < frames; ++i) {
            double t = (double)i / fs, fade = t < 0.005 ? t / 0.005 : 1.0;
            x[i] = (float)(fade * amp * sin(2 * E1_PI * f0 * T / kk * (exp(t / T * kk) - 1.0)));
        }
        run_lim(m, x, 1, frames);
        check_ceiling(fs, "+12 dB log sweep 20 Hz..0.41 fs", x, 1, frames, 0, 1);
        e1_limiter_destroy(m);
    }
    free(x);
}

static void test_latency_and_transparency(double fs)
{
    E1Limiter *m = e1_limiter_create(fs, 2);
    unsigned la = e1_limiter_latency(m);
    unsigned want = (unsigned)floor(fs * 64.0 / 44100.0 + 0.5);
    CHECK(la == want, "fs %g latency %u, expected %u", fs, la, want);
    const size_t frames = (size_t)fs;
    float *x = (float *)calloc(frames * 2, sizeof(float)), *ref = (float *)calloc(frames * 2, sizeof(float));
    rng_seed(3);
    for (size_t i = 0; i < frames * 2; ++i) ref[i] = x[i] = 0.3f * rng_sym(); /* true peak well below the ceiling */
    x[2 * 1000] = ref[2 * 1000] = 0.6f;
    run_lim(m, x, 2, frames);
    int exact = 1;
    for (size_t i = la; i < frames; ++i)
        for (int c = 0; c < 2; ++c) exact &= x[i * 2 + c] == ref[(i - la) * 2 + c];
    CHECK(exact, "fs %g: limiter is not a bit-exact delay below the ceiling", fs);
    CHECK(e1_limiter_gr_db(m) == 0.f, "fs %g: gain reduction reported without limiting", fs);
    free(x); free(ref);
    e1_limiter_destroy(m);
}

static void test_distortion(void)
{
    const double fs = 44100;
    static const double freqs[] = { 50, 100, 1000, 5000 };
    for (size_t k = 0; k < 4; ++k) {
        E1Limiter *m = e1_limiter_create(fs, 1);
        const size_t frames = (size_t)(fs * 2);
        float *x = (float *)malloc(frames * sizeof(float));
        for (size_t i = 0; i < frames; ++i) x[i] = (float)(2.0 * sin(2 * E1_PI * freqs[k] * (double)i / fs)); /* +6 dB */
        run_lim(m, x, 1, frames);
        size_t s0 = (size_t)fs;
        double res = sine_residual_db(x + s0, frames - s0, 1, fs, freqs[k]);
        double gr = e1_limiter_gr_db(m);
        CHECK(res < -60.0, "%g Hz: residual (THD+N) %.1f dB", freqs[k], res);
        CHECK(fabs(gr - (CEIL_DB - 6.0206)) < 0.6, "%g Hz: gain reduction %.2f dB", freqs[k], gr);
        printf("  steady +6 dB sine %5.0f Hz: THD+N %.1f dB, GR %.2f dB\n", freqs[k], res, gr);
        free(x);
        e1_limiter_destroy(m);
    }
}

static void test_release_and_tail(void)
{
    const double fs = 44100;
    E1Limiter *m = e1_limiter_create(fs, 2);
    const size_t frames = (size_t)(fs * 5.5);
    const size_t loud_end = (size_t)(fs * 0.5), quiet_end = (size_t)(fs * 4.5);
    float *x = (float *)calloc(frames * 2, sizeof(float)), *ref = (float *)calloc(frames * 2, sizeof(float));
    rng_seed(11);
    for (size_t i = 0; i < loud_end; ++i) x[2 * i] = x[2 * i + 1] = (float)(4.0 * rng_sym());
    for (size_t i = loud_end; i < quiet_end; ++i) x[2 * i] = x[2 * i + 1] = ref[2 * i] = 0.1f * rng_sym();
    run_lim(m, x, 2, frames);
    unsigned la = e1_limiter_latency(m);
    /* 12 dB of gain reduction released with a 250 ms time constant: exact unity again ~3.4 s later */
    int exact = 1;
    for (size_t i = quiet_end - (size_t)(fs * 0.1); i < quiet_end; ++i) exact &= x[2 * i] == ref[2 * (i - la)];
    CHECK(exact, "limiter did not return to exact unity gain after release");
    int nz = 0;
    for (size_t i = quiet_end + la; i < frames; ++i) nz += x[2 * i] != 0.f || x[2 * i + 1] != 0.f;
    CHECK(nz == 0, "limiter silence tail not exactly zero (%d samples)", nz);
    free(x); free(ref);
    e1_limiter_destroy(m);
}

int main(void)
{
    printf("test_limiter\n");
    test_latency_and_transparency(44100);
    test_latency_and_transparency(48000);
    test_latency_and_transparency(96000);
    test_overdrive(44100);
    test_overdrive(48000);
    test_distortion();
    test_release_and_tail();
    return test_summary("test_limiter");
}
