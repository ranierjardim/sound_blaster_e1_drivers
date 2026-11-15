/*
 * test_spatial.c - unit / measurement tests for the dsp-spatial modules of libe1dsp:
 * Surround (e1_surround.c + e1_hrtf_data.c), Clarity (e1_crystal.c), Dialog+ (e1_dialog.c),
 * Smart Volume (e1_svm.c).  Pure C11, no OS headers.  Exit code = number of failed checks.
 *
 * Usage: test_spatial [quick]      ("quick" skips the long performance runs)
 */
#include "e1dsp_modules.h"
#include "e1_biquad.h"
#include "e1_hrtf_data.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------------------------------ */
/* mini framework                                                                              */
/* ------------------------------------------------------------------------------------------ */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                                     \
    do {                                                                                     \
        if (cond) { ++g_pass; }                                                              \
        else { ++g_fail; printf("  FAIL (%s:%d): ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static uint32_t g_rng = 0x12345678u;
static void rng_seed(uint32_t s) { g_rng = s ? s : 0x9E3779B9u; }
static float white(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (float)((double)g_rng / 2147483648.0 - 1.0);
}

/* Paul Kellet's pink filter on white noise, then scaled to the requested RMS (dBFS). */
static void gen_pink(float *x, size_t n, uint32_t seed, double rms_dbfs)
{
    rng_seed(seed);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, acc = 0;
    for (size_t i = 0; i < n; ++i) {
        const double w = (double)white();
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520; b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        const double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        x[i] = (float)p;
        acc += p * p;
    }
    const double g = pow(10.0, rms_dbfs / 20.0) / sqrt(acc / (double)n);
    for (size_t i = 0; i < n; ++i) x[i] = (float)((double)x[i] * g);
}

static double pow_of(const float *x, size_t a, size_t b)
{
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += (double)x[i] * (double)x[i];
    return s / (double)(b - a);
}

static double db10(double p) { return 10.0 * log10(p > 1e-30 ? p : 1e-30); }

/* Goertzel amplitude of a frequency over x[a..b) */
static double tone_amp(const float *x, size_t a, size_t b, double f, double fs)
{
    const double w = 2.0 * E1_PI * f / fs, c = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = a; i < b; ++i) {
        const double s0 = (double)x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double re = s1 - s2 * cos(w), im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double)(b - a);
}

static size_t count_bad(const float *x, size_t n)
{
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) {
        const float v = x[i];
        if (!isfinite(v) || (v != 0.0f && fabsf(v) < FLT_MIN)) ++bad;
    }
    return bad;
}

/* Click detector for a pure tone at f: r[n] = y[n] - 2cos(w) y[n-1] + y[n-2] is exactly 0 for any
   sinusoid of that frequency, whatever its amplitude or phase.  Smooth gain/filter changes leave
   r ~ 1e-5; a step or click of size J gives |r| ~ J.  Returns max |r| inside the switch windows
   ([a, a+win) and [b, b+win)) and elsewhere after `settle`. */
static void click_metric(const float *y, size_t n, double f, double fs, size_t a, size_t b, size_t win,
                         size_t settle, double *steady, double *trans)
{
    const double c = 2.0 * cos(2.0 * E1_PI * f / fs);
    *steady = 0.0; *trans = 0.0;
    for (size_t i = 2; i < n; ++i) {
        const double r = fabs((double)y[i] - c * (double)y[i - 1] + (double)y[i - 2]);
        const int in_t = (i >= a && i < a + win) || (i >= b && i < b + win);
        if (in_t) { if (r > *trans) *trans = r; }
        else if (i > settle) { if (r > *steady) *steady = r; }
    }
}

static float *fbuf(size_t n)
{
    float *p = (float *)calloc(n ? n : 1, sizeof(float));
    if (!p) { printf("out of memory\n"); exit(99); }
    return p;
}

/* ------------------------------------------------------------------------------------------ */
/* generic module adapter so the shared tests can drive all four modules                       */
/* ------------------------------------------------------------------------------------------ */
typedef enum { MOD_SURROUND, MOD_CRYSTAL, MOD_DIALOG, MOD_SVM, MOD_SVM_MIC, MOD_COUNT } ModKind;
static const char *mod_name[MOD_COUNT] = {"surround", "clarity", "dialog+", "smartvol", "smartvol-mic"};

typedef struct Mod {
    ModKind kind;
    int nch;
    void *p;
} Mod;

static Mod mod_create(ModKind k, double fs)
{
    Mod m;
    m.kind = k;
    m.nch = (k == MOD_SVM_MIC) ? 1 : 2;
    switch (k) {
    case MOD_SURROUND: m.p = e1_surround_create(fs); break;
    case MOD_CRYSTAL: m.p = e1_crystal_create(fs, 2); break;
    case MOD_DIALOG: m.p = e1_dialog_create(fs); break;
    case MOD_SVM: m.p = e1_svm_create(fs, 2, 0); break;
    default: m.p = e1_svm_create(fs, 1, 1); break;
    }
    return m;
}

static void mod_destroy(Mod *m)
{
    switch (m->kind) {
    case MOD_SURROUND: e1_surround_destroy((E1Surround *)m->p); break;
    case MOD_CRYSTAL: e1_crystal_destroy((E1Crystal *)m->p); break;
    case MOD_DIALOG: e1_dialog_destroy((E1Dialog *)m->p); break;
    default: e1_svm_destroy((E1Svm *)m->p); break;
    }
    m->p = NULL;
}

/* level 0..1, opt = speaker mode / svm mode */
static void mod_set(Mod *m, float level, int opt)
{
    switch (m->kind) {
    case MOD_SURROUND: e1_surround_set((E1Surround *)m->p, level, opt); break;
    case MOD_CRYSTAL: e1_crystal_set((E1Crystal *)m->p, level); break;
    case MOD_DIALOG: e1_dialog_set((E1Dialog *)m->p, level); break;
    default: e1_svm_set((E1Svm *)m->p, level, opt); break;
    }
}

static void mod_reset(Mod *m)
{
    switch (m->kind) {
    case MOD_SURROUND: e1_surround_reset((E1Surround *)m->p); break;
    case MOD_CRYSTAL: e1_crystal_reset((E1Crystal *)m->p); break;
    case MOD_DIALOG: e1_dialog_reset((E1Dialog *)m->p); break;
    default: e1_svm_reset((E1Svm *)m->p); break;
    }
}

static unsigned mod_latency(const Mod *m)
{
    switch (m->kind) {
    case MOD_SURROUND: return e1_surround_latency((const E1Surround *)m->p);
    case MOD_CRYSTAL: return e1_crystal_latency((const E1Crystal *)m->p);
    case MOD_DIALOG: return e1_dialog_latency((const E1Dialog *)m->p);
    default: return e1_svm_latency((const E1Svm *)m->p);
    }
}

static void mod_process(Mod *m, float *l, float *r, int n)
{
    float *ch[2] = {l, r};
    switch (m->kind) {
    case MOD_SURROUND: e1_surround_process((E1Surround *)m->p, l, r, n); break;
    case MOD_CRYSTAL: e1_crystal_process((E1Crystal *)m->p, ch, 2, n); break;
    case MOD_DIALOG: e1_dialog_process((E1Dialog *)m->p, l, r, n); break;
    case MOD_SVM: e1_svm_process((E1Svm *)m->p, ch, 2, n); break;
    default: e1_svm_process((E1Svm *)m->p, ch, 1, n); break;
    }
}

/* Event = parameter change applied before sample index `at`. */
typedef struct Ev { size_t at; float level; int opt; } Ev;

/* Runs the whole buffer in place. block > 0: fixed block size; block == 0: random sizes 1..300. */
static void run(Mod *m, float *l, float *r, size_t n, int block, const Ev *ev, int nev, uint32_t seed)
{
    size_t pos = 0;
    int e = 0;
    uint32_t s = seed ? seed : 1u;
    while (pos < n) {
        while (e < nev && ev[e].at <= pos) { mod_set(m, ev[e].level, ev[e].opt); ++e; }
        size_t len;
        if (block > 0) len = (size_t)block;
        else { s ^= s << 13; s ^= s >> 17; s ^= s << 5; len = 1 + (size_t)(s % 300u); }
        if (len > n - pos) len = n - pos;
        if (e < nev && ev[e].at < pos + len) len = ev[e].at - pos;
        mod_process(m, l + pos, r + pos, (int)len);
        pos += len;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Surround                                                                                    */
/* ------------------------------------------------------------------------------------------ */
static void test_hrtf_model(void)
{
    printf("[hrtf] analytic model sanity\n");
    const double itd90 = e1_hrtf_model_delay(90.0, -1) - e1_hrtf_model_delay(90.0, +1);
    const double itd30 = e1_hrtf_model_delay(30.0, -1) - e1_hrtf_model_delay(30.0, +1);
    printf("  ITD 30 deg = %.3f ms, ITD 90 deg = %.3f ms\n", itd30 * 1e3, itd90 * 1e3);
    CHECK(itd90 > 0.60e-3 && itd90 < 0.72e-3, "ITD(90) %.3f ms out of the human range", itd90 * 1e3);
    CHECK(itd30 > 0.20e-3 && itd30 < 0.32e-3, "ITD(30) %.3f ms", itd30 * 1e3);
    const double ild8k = 20.0 * log10(e1_hrtf_model_mag(8000.0, 90.0, +1) / e1_hrtf_model_mag(8000.0, 90.0, -1));
    const double ild200 = 20.0 * log10(e1_hrtf_model_mag(200.0, 90.0, +1) / e1_hrtf_model_mag(200.0, 90.0, -1));
    printf("  ILD 90 deg: %.1f dB @200 Hz, %.1f dB @8 kHz\n", ild200, ild8k);
    CHECK(ild8k > 10.0 && ild8k < 30.0, "ILD at 8 kHz %.1f dB", ild8k);
    CHECK(fabs(ild200) < 2.0, "ILD at 200 Hz %.1f dB", ild200);
    CHECK(e1_virt_taps(44100.0) == 64, "taps at 44.1k = %d (expected 64)", e1_virt_taps(44100.0));
}

static void make_stereo(float *l, float *r, size_t n, int corr_case, uint32_t seed, double dbfs)
{
    /* corr_case: 0 mono (L=R), 1 uncorrelated, 2 correlation 0.5, 3 anti-correlated */
    float *b = fbuf(n);
    gen_pink(l, n, seed, dbfs);
    gen_pink(b, n, seed * 7u + 3u, dbfs);
    for (size_t i = 0; i < n; ++i) {
        switch (corr_case) {
        case 0: r[i] = l[i]; break;
        case 1: r[i] = b[i]; break;
        case 2: r[i] = 0.5f * l[i] + 0.8660254f * b[i]; break;
        default: r[i] = -l[i]; break;
        }
    }
    free(b);
}

static void test_surround(int quick)
{
    printf("[surround]\n");
    const double fs = 44100.0;
    const size_t n = (size_t)(fs * (quick ? 2.0 : 4.0));
    const size_t skip = (size_t)(0.3 * fs);
    float *il = fbuf(n), *ir = fbuf(n), *ol = fbuf(n), *or_ = fbuf(n);

    double t0 = now_s();
    E1Surround *s = e1_surround_create(fs);
    const double tc = now_s() - t0;
    CHECK(s != NULL, "create failed");
    if (!s) return;
    printf("  create (filter design) at 44.1 kHz: %.1f ms, latency reported %u\n", tc * 1e3, e1_surround_latency(s));
    Mod m = {MOD_SURROUND, 2, s};

    static const char *cname[4] = {"mono", "uncorr", "corr0.5", "anti"};
    double worst = 0.0;
    for (int mode = 0; mode < 2; ++mode) {
        printf("  pink-noise loudness change (dB) vs immersion, %s:\n", mode ? "speakers" : "headphones");
        printf("    level ");
        for (int lv = 0; lv <= 10; ++lv) printf(" %5.1f", lv / 10.0);
        printf("\n");
        for (int cc = 0; cc < 4; ++cc) {
            make_stereo(il, ir, n, cc, 1000u + (uint32_t)cc, -20.0);
            printf("    %-7s", cname[cc]);
            for (int lv = 0; lv <= 10; ++lv) {
                memcpy(ol, il, n * sizeof(float));
                memcpy(or_, ir, n * sizeof(float));
                e1_surround_set(s, (float)lv / 10.0f, mode);
                e1_surround_reset(s);
                run(&m, ol, or_, n, 128, NULL, 0, 0);
                const double pin = pow_of(il, skip, n) + pow_of(ir, skip, n);
                const double pout = pow_of(ol, skip, n) + pow_of(or_, skip, n);
                const double d = db10(pout / pin);
                printf(" %+5.2f", d);
                if (cc < 3 && fabs(d) > worst) worst = fabs(d);
                if (cc < 3) CHECK(fabs(d) <= 1.5, "loudness %s mode %d level %.1f: %+.2f dB", cname[cc], mode, lv / 10.0, d);
                if (lv == 0) {
                    CHECK(memcmp(ol, il, n * sizeof(float)) == 0 && memcmp(or_, ir, n * sizeof(float)) == 0,
                          "level 0 is not a bit-exact pass-through");
                }
                if (cc == 0) {
                    int same = memcmp(ol, or_, n * sizeof(float)) == 0;
                    CHECK(same, "mono input does not give identical L/R (mode %d level %.1f)", mode, lv / 10.0);
                }
                if (cc == 3 && lv == 10) {
                    /* anti-phase input: the mono sum of the output must not collapse completely */
                    double ps = 0.0, pd = 0.0;
                    for (size_t i = skip; i < n; ++i) {
                        const double a = 0.5 * ((double)ol[i] + (double)or_[i]), b = 0.5 * ((double)ol[i] - (double)or_[i]);
                        ps += a * a; pd += b * b;
                    }
                    printf(" [mono-sum of anti-phase input: %.1f dB rel. side]", db10(ps / pd));
                }
                CHECK(count_bad(ol, n) == 0 && count_bad(or_, n) == 0, "non-finite or denormal output");
            }
            printf("\n");
        }
    }
    printf("  worst |loudness change| (mono/uncorr/corr0.5, all levels, both modes) = %.2f dB (spec target +-1 dB)\n", worst);

    /* uncorrelated mono-compatibility: power of (L+R)/2 relative to input at full immersion */
    for (int mode = 0; mode < 2; ++mode) {
        make_stereo(il, ir, n, 1, 77u, -20.0);
        memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
        e1_surround_set(s, 1.0f, mode); e1_surround_reset(s);
        run(&m, ol, or_, n, 128, NULL, 0, 0);
        double pi = 0, po = 0;
        for (size_t i = skip; i < n; ++i) {
            const double a = 0.5 * ((double)il[i] + (double)ir[i]), b = 0.5 * ((double)ol[i] + (double)or_[i]);
            pi += a * a; po += b * b;
        }
        printf("  mono-sum power change, uncorrelated input, level 1, %s: %+.2f dB\n", mode ? "speakers" : "headphones", db10(po / pi));
        CHECK(fabs(db10(po / pi)) < 3.0, "mono sum changes by %.2f dB", db10(po / pi));
    }

    /* latency: cross-correlation peak, mono pink, level 1 */
    for (int mode = 0; mode < 2; ++mode) {
        make_stereo(il, ir, n, 0, 55u, -20.0);
        memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
        e1_surround_set(s, 1.0f, mode); e1_surround_reset(s);
        run(&m, ol, or_, n, 128, NULL, 0, 0);
        int best = 0; double bv = -1e300;
        for (int lag = -16; lag <= 200; ++lag) {
            double acc = 0.0;
            for (size_t i = skip; i < skip + 44100; ++i) acc += (double)il[i] * (double)ol[(size_t)((long long)i + lag)];
            if (acc > bv) { bv = acc; best = lag; }
        }
        printf("  measured latency (xcorr peak) %s: %d samples, reported %u\n", mode ? "speakers" : "headphones", best, e1_surround_latency(s));
        CHECK(abs(best - (int)e1_surround_latency(s)) <= 1, "latency mismatch: measured %d", best);
    }

    /* timbre: magnitude response for a centre source (L=R impulse) and a side source (L=-R),
       full immersion, 1/6-octave smoothed, 40 Hz .. 16 kHz: no deep comb notches */
    for (int mode = 0; mode < 2; ++mode) {
        for (int src = 0; src < 2; ++src) {
            const size_t ni = 8192;
            float *al = fbuf(ni), *ar = fbuf(ni);
            al[0] = 1.0f; ar[0] = src ? -1.0f : 1.0f;
            e1_surround_set(s, 1.0f, mode); e1_surround_reset(s);
            run(&m, al, ar, ni, 128, NULL, 0, 0);
            double mn = 1e9, mx = -1e9;
            for (int b = 0; b < 120; ++b) {
                const double fc = 40.0 * pow(400.0, (double)b / 119.0);
                double pacc = 0.0; int cnt = 0;
                for (int q = -4; q <= 4; ++q) {
                    const double f = fc * pow(2.0, (double)q / 48.0), w = 2.0 * E1_PI * f / fs;
                    double re = 0.0, im = 0.0;
                    for (size_t t = 0; t < 2048; ++t) {
                        /* ear-mid for a centre source, ear-side for a side source */
                        const double v = src ? 0.5 * ((double)al[t] - (double)ar[t]) : 0.5 * ((double)al[t] + (double)ar[t]);
                        re += v * cos(w * (double)t); im -= v * sin(w * (double)t);
                    }
                    pacc += re * re + im * im; ++cnt;
                }
                const double db = db10(pacc / cnt);
                if (db < mn) mn = db;
                if (db > mx) mx = db;
            }
            printf("  timbre, %s source, %s, level 1: 1/6-oct response %+.1f .. %+.1f dB (40 Hz-16 kHz)\n",
                   src ? "side" : "centre", mode ? "speakers" : "headphones", mn, mx);
            CHECK(mn > -9.0 && mx < 9.0 && mx - mn < 14.0, "coloration too strong (%.1f..%.1f dB)", mn, mx);
            free(al); free(ar);
        }
    }

    /* crossfeed: hard-left pink must reach the right ear in headphone mode */
    for (int mode = 0; mode < 2; ++mode) {
        printf("  hard-left source, right/left output power (%s):", mode ? "speakers" : "headphones");
        gen_pink(il, n, 444u, -20.0);
        memset(ir, 0, n * sizeof(float));
        const float lvs[3] = {0.0f, 0.4f, 1.0f};
        for (int k = 0; k < 3; ++k) {
            memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
            e1_surround_set(s, lvs[k], mode); e1_surround_reset(s);
            run(&m, ol, or_, n, 128, NULL, 0, 0);
            const double rl = db10(pow_of(or_, skip, n) / pow_of(ol, skip, n));
            printf("  level %.1f: %+.1f dB", lvs[k], rl);
            if (mode == 0 && k == 2) CHECK(rl > -20.0 && rl < -2.0, "headphone crossfeed %.1f dB implausible", rl);
        }
        printf("\n");
    }

    /* block-size independence with parameter changes in flight */
    {
        make_stereo(il, ir, n, 2, 99u, -20.0);
        float *al = fbuf(n), *ar = fbuf(n);
        Ev ev[4] = {{0, 0.4f, 0}, {20000, 1.0f, 1}, {20001, 0.7f, 1}, {60000, 0.0f, 0}};
        memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
        memcpy(al, il, n * sizeof(float)); memcpy(ar, ir, n * sizeof(float));
        e1_surround_set(s, 0.0f, 0); e1_surround_reset(s);
        run(&m, ol, or_, n, 128, ev, 4, 0);
        e1_surround_set(s, 0.0f, 0); e1_surround_reset(s);
        run(&m, al, ar, n, 0, ev, 4, 4242u);
        CHECK(memcmp(ol, al, n * sizeof(float)) == 0 && memcmp(or_, ar, n * sizeof(float)) == 0,
              "output depends on the caller's block size");
        free(al); free(ar);
    }

    /* silence -> exact zeros within a bounded time */
    {
        make_stereo(il, ir, n, 1, 5u, -6.0);
        const size_t half = n / 4;
        for (size_t i = half; i < n; ++i) il[i] = ir[i] = 0.0f;
        e1_surround_set(s, 1.0f, 0); e1_surround_reset(s);
        run(&m, il, ir, n, 128, NULL, 0, 0);
        size_t last = 0;
        for (size_t i = 0; i < n; ++i) if (il[i] != 0.0f || ir[i] != 0.0f) last = i;
        const double decay_ms = (double)(last + 1 - half) * 1000.0 / fs;
        printf("  silence: output exactly 0 after %.1f ms of digital silence\n", decay_ms);
        CHECK(last + 1 < n - (size_t)(0.5 * fs), "output does not reach exact zero (last non-zero %zu)", last);
        CHECK(count_bad(il, n) == 0 && count_bad(ir, n) == 0, "denormals in decay");
    }

    /* clicks: level and mode switches on a sine must not produce discontinuities */
    {
        for (size_t i = 0; i < n; ++i) {
            il[i] = (float)(0.5 * sin(2.0 * E1_PI * 300.0 * (double)i / fs));
            ir[i] = (float)(0.5 * sin(2.0 * E1_PI * 300.0 * (double)i / fs + 1.0));
        }
        const size_t a = (size_t)(1.0 * fs), b = (size_t)(1.5 * fs);
        Ev ev[3] = {{0, 0.0f, 0}, {a, 1.0f, 0}, {b, 1.0f, 1}};
        e1_surround_set(s, 0.0f, 0); e1_surround_reset(s);
        run(&m, il, ir, n, 128, ev, 3, 0);
        double steady, trans;
        click_metric(il, n, 300.0, fs, a, b, 2000, (size_t)(0.5 * fs), &steady, &trans);
        printf("  click test (300 Hz tone residual): steady %.2e, during level/mode switches %.2e\n", steady, trans);
        CHECK(trans < 2e-4, "discontinuity during parameter switch (%.2e)", trans);
    }

    e1_surround_destroy(s);
    free(il); free(ir); free(ol); free(or_);
}

/* ------------------------------------------------------------------------------------------ */
/* Clarity / Crystalizer                                                                       */
/* ------------------------------------------------------------------------------------------ */
static double sine_gain_db(E1Crystal *c, double fs, double f, float level)
{
    const size_t n = (size_t)(fs * 1.0);
    float *l = fbuf(n), *r = fbuf(n), *x = fbuf(n);
    for (size_t i = 0; i < n; ++i) x[i] = l[i] = r[i] = (float)(0.25 * sin(2.0 * E1_PI * f * (double)i / fs));
    e1_crystal_set(c, level);
    e1_crystal_reset(c);
    Mod m = {MOD_CRYSTAL, 2, c};
    run(&m, l, r, n, 128, NULL, 0, 0);
    const double g = db10(pow_of(l, n / 2, n) / pow_of(x, n / 2, n));
    free(l); free(r); free(x);
    return g;
}

static void test_crystal(void)
{
    printf("[clarity]\n");
    const double fs = 44100.0;
    E1Crystal *c = e1_crystal_create(fs, 2);
    CHECK(c != NULL, "create failed");
    if (!c) return;
    Mod m = {MOD_CRYSTAL, 2, c};
    const size_t n = (size_t)(fs * 3.0);
    float *il = fbuf(n), *ir = fbuf(n), *ol = fbuf(n), *or_ = fbuf(n);

    /* level 0 -> identity */
    make_stereo(il, ir, n, 2, 31u, -12.0);
    for (size_t i = 0; i < n; i += 4410) il[i] = 0.9f;  /* transients too */
    memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
    e1_crystal_set(c, 0.0f); e1_crystal_reset(c);
    run(&m, ol, or_, n, 128, NULL, 0, 0);
    double maxdev = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = fabs((double)ol[i] - (double)il[i]);
        if (d > maxdev) maxdev = d;
    }
    const double g0 = db10(pow_of(ol, 0, n) / pow_of(il, 0, n));
    printf("  level 0: max |y-x| = %.3g, power change %+.4f dB\n", maxdev, g0);
    CHECK(fabs(g0) < 0.1 && maxdev < 1e-4, "level 0 is not (near) identity");
    double g50 = sine_gain_db(c, fs, 50.0, 0.0), g1k = sine_gain_db(c, fs, 1000.0, 0.0), g12k = sine_gain_db(c, fs, 12000.0, 0.0);
    printf("  level 0 steady sines: 50 Hz %+.3f, 1 kHz %+.3f, 12 kHz %+.3f dB\n", g50, g1k, g12k);
    CHECK(fabs(g50) < 0.1 && fabs(g1k) < 0.1 && fabs(g12k) < 0.1, "level 0 frequency response not flat");

    /* static tilt at the default 0.65 and at 1.0 (steady sines do not trigger the dynamics) */
    const float lvls[2] = {0.65f, 1.0f};
    for (int k = 0; k < 2; ++k) {
        g50 = sine_gain_db(c, fs, 50.0, lvls[k]); g1k = sine_gain_db(c, fs, 1000.0, lvls[k]); g12k = sine_gain_db(c, fs, 12000.0, lvls[k]);
        printf("  level %.2f steady sines: 50 Hz %+.2f, 1 kHz %+.2f, 12 kHz %+.2f dB (tilt vs 1 kHz: LF %+.2f, HF %+.2f)\n",
               lvls[k], g50, g1k, g12k, g50 - g1k, g12k - g1k);
        CHECK(g50 - g1k > 1.5 && g50 - g1k < 6.5, "LF tilt %.2f", g50 - g1k);
        CHECK(fabs(g1k) < 3.0, "mid %.2f", g1k);
        CHECK(g12k - g1k > 1.0 && g12k - g1k < 7.0, "HF tilt %.2f", g12k - g1k);
    }

    /* loudness neutrality: K-weighted (BS.1770 pre-filter) power of pink noise in vs out */
    {
        E1BqCoef ks, kh;
        e1_bq_highshelf(&ks, fs, 1681.974450955533, 0.7071752369554193, 3.999843853973347);
        e1_bq_highpass(&kh, fs, 38.13547087602444, 0.5003270373238773);
        const float lv3[3] = {0.4f, 0.65f, 1.0f};
        for (int k = 0; k < 3; ++k) {
            gen_pink(il, n, 5u, -20.0);
            memcpy(ol, il, n * sizeof(float)); memcpy(or_, il, n * sizeof(float));
            e1_crystal_set(c, lv3[k]); e1_crystal_reset(c);
            run(&m, ol, or_, n, 128, NULL, 0, 0);
            E1BqState s1 = {0, 0}, s2 = {0, 0}, s3 = {0, 0}, s4 = {0, 0};
            e1_bq_run(&ks, &s1, il, (int)n); e1_bq_run(&kh, &s2, il, (int)n);
            e1_bq_run(&ks, &s3, ol, (int)n); e1_bq_run(&kh, &s4, ol, (int)n);
            const double lu = db10(pow_of(ol, n / 3, n) / pow_of(il, n / 3, n));
            printf("  level %.2f: K-weighted loudness change on pink noise %+.2f LU\n", lv3[k], lu);
            CHECK(fabs(lu) < 0.5, "Clarity %.2f changes loudness by %+.2f LU", lv3[k], lu);
        }
    }

    /* transient emphasis: HF bursts are boosted more than steady HF noise */
    {
        float *hb = fbuf(n), *hs = fbuf(n);
        gen_pink(hb, n, 11u, -14.0);
        E1BqCoef hp; E1BqState st = {0, 0};
        e1_bq_highpass(&hp, fs, 4000.0, 0.7071);
        e1_bq_run(&hp, &st, hb, (int)n);
        memcpy(hs, hb, n * sizeof(float));
        const size_t per = (size_t)(0.3 * fs), on = (size_t)(0.04 * fs);
        for (size_t i = 0; i < n; ++i) if (i % per >= on) hb[i] = 0.0f;
        double gb, gst;
        {
            memcpy(ol, hb, n * sizeof(float)); memcpy(or_, hb, n * sizeof(float));
            e1_crystal_set(c, 1.0f); e1_crystal_reset(c);
            run(&m, ol, or_, n, 128, NULL, 0, 0);
            gb = db10(pow_of(ol, per, n) / pow_of(hb, per, n));
        }
        {
            memcpy(ol, hs, n * sizeof(float)); memcpy(or_, hs, n * sizeof(float));
            e1_crystal_set(c, 1.0f); e1_crystal_reset(c);
            run(&m, ol, or_, n, 128, NULL, 0, 0);
            gst = db10(pow_of(ol, per, n) / pow_of(hs, per, n));
        }
        printf("  level 1: HF gain on 40 ms bursts %+.2f dB vs steady HF noise %+.2f dB (transient emphasis %+.2f dB)\n", gb, gst, gb - gst);
        CHECK(gb - gst > 0.5, "no transient emphasis (%.2f dB)", gb - gst);
        free(hb); free(hs);
    }

    /* click test on level changes */
    {
        for (size_t i = 0; i < n; ++i) il[i] = ir[i] = (float)(0.5 * sin(2.0 * E1_PI * 300.0 * (double)i / fs));
        const size_t a = (size_t)(1.0 * fs), b = (size_t)(2.0 * fs);
        Ev ev[3] = {{0, 0.0f, 0}, {a, 1.0f, 0}, {b, 0.0f, 0}};
        e1_crystal_set(c, 0.0f); e1_crystal_reset(c);
        run(&m, il, ir, n, 128, ev, 3, 0);
        double steady, trans;
        click_metric(il, n, 300.0, fs, a, b, 2000, (size_t)(0.5 * fs), &steady, &trans);
        printf("  click test (300 Hz tone residual): steady %.2e, during level switches %.2e\n", steady, trans);
        CHECK(trans < 2e-4, "discontinuity during level change (%.2e)", trans);
    }

    /* block-size independence */
    {
        make_stereo(il, ir, n, 1, 3u, -10.0);
        float *al = fbuf(n), *ar = fbuf(n);
        Ev ev[2] = {{0, 0.65f, 0}, {30001, 1.0f, 0}};
        memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
        memcpy(al, il, n * sizeof(float)); memcpy(ar, ir, n * sizeof(float));
        e1_crystal_set(c, 0.65f); e1_crystal_reset(c); run(&m, ol, or_, n, 128, ev, 2, 0);
        e1_crystal_set(c, 0.65f); e1_crystal_reset(c); run(&m, al, ar, n, 0, ev, 2, 777u);
        CHECK(memcmp(ol, al, n * sizeof(float)) == 0 && memcmp(or_, ar, n * sizeof(float)) == 0, "block-size dependent output");
        free(al); free(ar);
    }
    e1_crystal_destroy(c);
    free(il); free(ir); free(ol); free(or_);
}

/* ------------------------------------------------------------------------------------------ */
/* Dialog+                                                                                     */
/* ------------------------------------------------------------------------------------------ */
static void test_dialog(void)
{
    printf("[dialog+]\n");
    const double fs = 44100.0;
    E1Dialog *d = e1_dialog_create(fs);
    CHECK(d != NULL, "create failed");
    if (!d) return;
    Mod m = {MOD_DIALOG, 2, d};
    const size_t n = (size_t)(fs * 4.0);
    float *il = fbuf(n), *ir = fbuf(n), *ol = fbuf(n), *or_ = fbuf(n);

    /* level 0 identity */
    make_stereo(il, ir, n, 2, 8u, -15.0);
    memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
    e1_dialog_set(d, 0.0f); e1_dialog_reset(d);
    run(&m, ol, or_, n, 128, NULL, 0, 0);
    CHECK(memcmp(ol, il, n * sizeof(float)) == 0 && memcmp(or_, ir, n * sizeof(float)) == 0, "level 0 not bit-exact");

    /* centre voice-band tone (1 kHz) vs side content (1.2 kHz anti-phase) */
    static const double levels[4] = {0.3, 0.5, 0.8, 1.0};
    static const double expect[4] = {12.0, 18.0, 24.0, 24.0}; /* a - b from the level table */
    const double fc = 1000.0, fsd = 1200.0;
    for (int k = 0; k < 4; ++k) {
        for (size_t i = 0; i < n; ++i) {
            const double cc = 0.1 * sin(2.0 * E1_PI * fc * (double)i / fs), ss = 0.1 * sin(2.0 * E1_PI * fsd * (double)i / fs);
            il[i] = (float)(cc + ss);
            ir[i] = (float)(cc - ss);
        }
        memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
        e1_dialog_set(d, (float)levels[k]); e1_dialog_reset(d);
        run(&m, ol, or_, n, 128, NULL, 0, 0);
        float *mo = fbuf(n), *so = fbuf(n);
        for (size_t i = 0; i < n; ++i) { mo[i] = 0.5f * (ol[i] + or_[i]); so[i] = 0.5f * (ol[i] - or_[i]); }
        const size_t a = n / 2;
        const double cm = tone_amp(mo, a, n, fc, fs) / 0.1, sm = tone_amp(so, a, n, fsd, fs) / 0.1;
        const double rel = 20.0 * log10(cm / sm);
        const double pin = pow_of(il, a, n) + pow_of(ir, a, n), pout = pow_of(ol, a, n) + pow_of(or_, a, n);
        printf("  level %.1f: centre %+.2f dB, side %+.2f dB, relative %+.2f dB (expected %+.1f +-2), output power %+.2f dB\n",
               levels[k], 20.0 * log10(cm), 20.0 * log10(sm), rel, expect[k], db10(pout / pin));
        CHECK(fabs(rel - expect[k]) <= 2.0, "relative dialogue boost %.2f dB", rel);
        CHECK(db10(pout / pin) <= 0.5, "dialog+ made the programme louder by %.2f dB", db10(pout / pin));
        free(mo); free(so);
    }

    /* loudness normalisation on noise: correlated-0.5 pink and centred voice-band noise */
    for (int t = 0; t < 2; ++t) {
        if (t == 0) make_stereo(il, ir, n, 2, 21u, -18.0);
        else {
            gen_pink(il, n, 22u, -18.0);
            E1BqCoef hp, lp; E1BqState s1 = {0, 0}, s2 = {0, 0};
            e1_bq_highpass(&hp, fs, 300.0, 0.7071); e1_bq_lowpass(&lp, fs, 4000.0, 0.7071);
            e1_bq_run(&hp, &s1, il, (int)n); e1_bq_run(&lp, &s2, il, (int)n);
            memcpy(ir, il, n * sizeof(float));
        }
        printf("  output power change, %s:", t ? "centred voice-band noise" : "pink corr 0.5");
        for (int k = 0; k <= 10; k += 2) {
            memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
            e1_dialog_set(d, (float)k / 10.0f); e1_dialog_reset(d);
            run(&m, ol, or_, n, 128, NULL, 0, 0);
            const size_t a = n / 4;
            const double dd = db10((pow_of(ol, a, n) + pow_of(or_, a, n)) / (pow_of(il, a, n) + pow_of(ir, a, n)));
            printf(" %+.2f", dd);
            CHECK(dd <= 1.0 && dd >= -3.0, "normalisation off: %.2f dB at level %.1f", dd, k / 10.0);
            CHECK(count_bad(ol, n) == 0 && count_bad(or_, n) == 0, "bad samples");
        }
        printf("\n");
    }

    /* click test (1 kHz centre tone, level 0 -> 1 -> 0) */
    {
        for (size_t i = 0; i < n; ++i) il[i] = ir[i] = (float)(0.2 * sin(2.0 * E1_PI * 1000.0 * (double)i / fs));
        const size_t a = (size_t)(1.0 * fs), b = (size_t)(2.5 * fs);
        Ev ev[3] = {{0, 0.0f, 0}, {a, 1.0f, 0}, {b, 0.0f, 0}};
        e1_dialog_set(d, 0.0f); e1_dialog_reset(d);
        run(&m, il, ir, n, 128, ev, 3, 0);
        double steady, trans;
        click_metric(il, n, 1000.0, fs, a, b, 4000, (size_t)(0.5 * fs), &steady, &trans);
        printf("  click test (1 kHz tone residual): steady %.2e, during level switches %.2e\n", steady, trans);
        CHECK(trans < 2e-4, "discontinuity during level change (%.2e)", trans);
    }

    /* block-size independence */
    {
        make_stereo(il, ir, n, 2, 4u, -10.0);
        float *al = fbuf(n), *ar = fbuf(n);
        Ev ev[2] = {{0, 0.5f, 0}, {50001, 1.0f, 0}};
        memcpy(ol, il, n * sizeof(float)); memcpy(or_, ir, n * sizeof(float));
        memcpy(al, il, n * sizeof(float)); memcpy(ar, ir, n * sizeof(float));
        e1_dialog_set(d, 0.5f); e1_dialog_reset(d); run(&m, ol, or_, n, 128, ev, 2, 0);
        e1_dialog_set(d, 0.5f); e1_dialog_reset(d); run(&m, al, ar, n, 0, ev, 2, 99u);
        CHECK(memcmp(ol, al, n * sizeof(float)) == 0 && memcmp(or_, ar, n * sizeof(float)) == 0, "block-size dependent output");
        free(al); free(ar);
    }
    e1_dialog_destroy(d);
    free(il); free(ir); free(ol); free(or_);
}

/* ------------------------------------------------------------------------------------------ */
/* Smart Volume                                                                                */
/* ------------------------------------------------------------------------------------------ */
typedef struct SvmRun { double out_q, out_l, g_q, g_l, gmax_abs, gmin, gmax; } SvmRun;

/* quiet segment (-50 dBFS) then loud (-20 dBFS); returns steady output levels and gains */
static SvmRun svm_step(E1Svm *s, double fs, float level, int mode, double seg_s)
{
    const size_t seg = (size_t)(fs * seg_s), n = 2 * seg;
    float *l = fbuf(n), *r = fbuf(n), *xl = fbuf(n), *xr = fbuf(n);
    make_stereo(l, r, n, 2, 300u, -20.0);
    for (size_t i = 0; i < seg; ++i) { l[i] *= 0.031622777f; r[i] *= 0.031622777f; }
    memcpy(xl, l, n * sizeof(float)); memcpy(xr, r, n * sizeof(float));
    e1_svm_set(s, level, mode);
    e1_svm_reset(s);
    SvmRun o;
    memset(&o, 0, sizeof(o));
    o.gmin = 1e9; o.gmax = -1e9;
    float *ch[2];
    double gq = 0, gl = 0; int cq = 0, cl = 0;
    for (size_t p = 0; p < n; p += 128) {
        int len = (int)((n - p) < 128 ? (n - p) : 128);
        ch[0] = l + p; ch[1] = r + p;
        e1_svm_process(s, ch, 2, len);
        const double g = (double)e1_svm_gain_db(s);
        if (fabs(g) > o.gmax_abs) o.gmax_abs = fabs(g);
        if (g < o.gmin) o.gmin = g;
        if (g > o.gmax) o.gmax = g;
        if (p > seg - (size_t)(3 * fs) && p < seg) { gq += g; ++cq; }
        if (p > n - (size_t)(3 * fs)) { gl += g; ++cl; }
    }
    const unsigned lat = e1_svm_latency(s);
    const size_t q0 = seg - (size_t)(3 * fs), l0 = n - (size_t)(3 * fs);
    /* output sample i carries input sample i - lat */
    o.out_q = db10(pow_of(l, q0 + lat, seg) + pow_of(r, q0 + lat, seg));
    o.out_l = db10(pow_of(l, l0, n) + pow_of(r, l0, n));
    o.g_q = gq / (cq ? cq : 1);
    o.g_l = gl / (cl ? cl : 1);
    free(l); free(r); free(xl); free(xr);
    return o;
}

static void test_svm(int quick)
{
    printf("[smart volume]\n");
    const double fs = 44100.0;
    E1Svm *s = e1_svm_create(fs, 2, 0);
    CHECK(s != NULL, "create failed");
    if (!s) return;
    const unsigned lat = e1_svm_latency(s);
    CHECK(lat == 96, "latency %u, expected 96 at 44.1 kHz", lat);

    /* level 0 = pure delay, bit exact; impulse response position = latency */
    {
        const size_t n = 44100;
        float *l = fbuf(n), *r = fbuf(n), *xl = fbuf(n);
        gen_pink(xl, n, 1u, -20.0);
        memcpy(l, xl, n * sizeof(float)); memcpy(r, xl, n * sizeof(float));
        e1_svm_set(s, 0.0f, 0); e1_svm_reset(s);
        float *ch[2] = {l, r};
        for (size_t p = 0; p < n; p += 128) { ch[0] = l + p; ch[1] = r + p; e1_svm_process(s, ch, 2, (int)((n - p) < 128 ? n - p : 128)); }
        CHECK(memcmp(l + lat, xl, (n - lat) * sizeof(float)) == 0, "level 0 is not an exact delay");
        /* impulse with a non-unity gain state */
        memset(l, 0, n * sizeof(float)); memset(r, 0, n * sizeof(float));
        l[1000] = 0.5f; r[1000] = 0.5f;
        e1_svm_set(s, 1.0f, 0); e1_svm_reset(s);
        for (size_t p = 0; p < n; p += 128) { ch[0] = l + p; ch[1] = r + p; e1_svm_process(s, ch, 2, (int)((n - p) < 128 ? n - p : 128)); }
        size_t pk = 0;
        for (size_t i = 0; i < n; ++i) if (fabsf(l[i]) > fabsf(l[pk])) pk = i;
        printf("  measured latency (impulse) %zu samples, reported %u\n", pk - 1000, lat);
        CHECK(pk - 1000 == lat, "measured latency %zu", pk - 1000);
        free(l); free(r); free(xl);
    }

    const double seg_s = quick ? 14.0 : 20.0;
    static const char *mname[3] = {"Normal", "Loud", "Night"};
    for (int mode = 0; mode < 3; ++mode) {
        const float lv[3] = {1.0f, 0.74f, 0.3f};
        for (int k = 0; k < 3; ++k) {
            const SvmRun o = svm_step(s, fs, lv[k], mode, seg_s);
            const double range = 50.0 * lv[k];
            printf("  %-6s level %.2f: 30 dB input step -> %.1f dB output step; gain quiet %+.1f, loud %+.1f dB; gain range seen [%+.1f, %+.1f], allowed +-%.1f\n",
                   mname[mode], lv[k], o.out_l - o.out_q, o.g_q, o.g_l, o.gmin, o.gmax, range);
            CHECK(o.gmax_abs <= range + 0.01, "gain exceeds configured range");
            CHECK(o.out_l - o.out_q < 30.0 - 0.5 * fmin(range, 20.0), "step not reduced enough");
            if (k == 0) CHECK(o.out_l - o.out_q <= 12.0, "at level 1 the 30 dB step should shrink below 12 dB (got %.1f)", o.out_l - o.out_q);
        }
    }

    /* never pumps silence: near-silence after reset, and long pause after loud content */
    {
        const size_t n = (size_t)(fs * 8.0);
        float *l = fbuf(n), *r = fbuf(n);
        make_stereo(l, r, n, 1, 61u, -85.0);
        e1_svm_set(s, 1.0f, 0); e1_svm_reset(s);
        float *ch[2];
        double gmax = -1e9;
        for (size_t p = 0; p < n; p += 128) {
            ch[0] = l + p; ch[1] = r + p;
            e1_svm_process(s, ch, 2, (int)((n - p) < 128 ? n - p : 128));
            if (e1_svm_gain_db(s) > gmax) gmax = e1_svm_gain_db(s);
        }
        printf("  -85 dBFS noise floor for 8 s at level 1: max gain %+.2f dB\n", gmax);
        CHECK(gmax <= 0.5, "silence / noise floor was pumped up (%.2f dB)", gmax);

        /* speech-like level, then pause */
        make_stereo(l, r, n, 2, 62u, -40.0);
        for (size_t i = n / 2; i < n; ++i) { l[i] *= 1e-3f; r[i] *= 1e-3f; } /* -100 dBFS */
        e1_svm_reset(s);
        double g_before = 0.0, g_end = 0.0, g_pause_max = -1e9;
        for (size_t p = 0; p < n; p += 128) {
            ch[0] = l + p; ch[1] = r + p;
            e1_svm_process(s, ch, 2, (int)((n - p) < 128 ? n - p : 128));
            const double g = e1_svm_gain_db(s);
            if (p < n / 2) g_before = g;
            else { g_end = g; if (g > g_pause_max) g_pause_max = g; }
        }
        printf("  pause after -40 dBFS programme: gain before %+.2f, max in pause %+.2f, end %+.2f dB\n", g_before, g_pause_max, g_end);
        CHECK(g_pause_max <= g_before + 0.5, "gain rose during the pause");
        free(l); free(r);
    }

    /* mic variant: speech-like bursts over a noise floor */
    {
        E1Svm *mic = e1_svm_create(fs, 1, 1);
        CHECK(mic != NULL, "mic create failed");
        if (mic) {
            const size_t n = (size_t)(fs * 12.0);
            float *x = fbuf(n), *nz = fbuf(n);
            gen_pink(nz, n, 71u, -70.0);
            rng_seed(5u);
            for (size_t i = 0; i < n; ++i) {
                const double t = (double)i / fs;
                const int talk = (t < 8.0) && (fmod(t, 0.5) < 0.3);
                const double env = talk ? 0.0178 * (0.6 + 0.4 * sin(2.0 * E1_PI * 4.0 * t)) : 0.0; /* ~ -40 dBFS */
                x[i] = (float)(env * sin(2.0 * E1_PI * 220.0 * t) + env * 0.5 * sin(2.0 * E1_PI * 1100.0 * t)) + nz[i];
            }
            e1_svm_set(mic, 0.74f, 0); e1_svm_reset(mic);
            float *ch[1];
            double g_talk_end = 0.0, g_pause_max = -1e9;
            for (size_t p = 0; p < n; p += 128) {
                ch[0] = x + p;
                e1_svm_process(mic, ch, 1, (int)((n - p) < 128 ? n - p : 128));
                const double g = e1_svm_gain_db(mic);
                if (p < (size_t)(8.0 * fs)) g_talk_end = g;
                else if (g > g_pause_max) g_pause_max = g;
            }
            printf("  mic: gain after 8 s of speech-like bursts %+.2f dB, max during 4 s noise-only pause %+.2f dB, latency %u\n",
                   g_talk_end, g_pause_max, e1_svm_latency(mic));
            CHECK(g_talk_end > 6.0, "mic AGC did not raise quiet speech (%.2f dB)", g_talk_end);
            CHECK(g_pause_max <= g_talk_end + 0.5, "mic AGC pumped the noise floor");
            CHECK(g_talk_end <= 30.01, "mic boost cap exceeded");
            CHECK(count_bad(x, n) == 0, "bad samples");
            free(x); free(nz);
            e1_svm_destroy(mic);
        }
    }

    /* mic: one short loud event (100 ms cough at -3 dBFS) must not duck the voice for seconds.
       Gain trace with the burst vs the same speech without it; 1.5 s after the burst ends the
       difference must be within 1 dB. */
    {
        const size_t n = (size_t)(fs * 14.0);
        const double tb = 6.0, tl = 0.1;
        float *x0 = fbuf(n), *x1 = fbuf(n), *nz = fbuf(n), *bn = fbuf(n);
        gen_pink(nz, n, 73u, -70.0);
        gen_pink(bn, n, 74u, -3.0);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / fs;
            const int talk = fmod(t, 0.5) < 0.32;
            const double env = talk ? 0.05 * (0.6 + 0.4 * sin(2.0 * E1_PI * 4.0 * t)) : 0.0; /* ~ -26 dBFS */
            x0[i] = (float)(env * (sin(2.0 * E1_PI * 180.0 * t) + 0.5 * sin(2.0 * E1_PI * 900.0 * t)
                                   + 0.25 * sin(2.0 * E1_PI * 2300.0 * t))) + nz[i];
            x1[i] = (t >= tb && t < tb + tl) ? bn[i] : x0[i];
        }
        float *xs[2] = {x0, x1};
        double gat[2][4];
        const double probes[4] = {tb + tl + 0.5, tb + tl + 1.0, tb + tl + 1.5, tb + tl + 3.0};
        for (int v = 0; v < 2; ++v) {
            E1Svm *mic = e1_svm_create(fs, 1, 1);
            e1_svm_set(mic, 0.74f, 0); e1_svm_reset(mic);
            int pi = 0;
            for (size_t p = 0; p < n; p += 128) {
                float *ch[1] = {xs[v] + p};
                e1_svm_process(mic, ch, 1, (int)((n - p) < 128 ? n - p : 128));
                while (pi < 4 && (double)(p + 128) / fs >= probes[pi]) gat[v][pi++] = e1_svm_gain_db(mic);
            }
            e1_svm_destroy(mic);
        }
        printf("  mic cough (100 ms, -3 dBFS) over -26 dBFS speech: gain vs no-cough run at +0.5/+1/+1.5/+3 s: "
               "%+.2f %+.2f %+.2f %+.2f dB\n", gat[1][0] - gat[0][0], gat[1][1] - gat[0][1],
               gat[1][2] - gat[0][2], gat[1][3] - gat[0][3]);
        CHECK(fabs(gat[1][2] - gat[0][2]) <= 1.0, "mic AGC still ducks %.2f dB 1.5 s after a cough",
              gat[1][2] - gat[0][2]);
        CHECK(fabs(gat[1][3] - gat[0][3]) <= 0.5, "mic AGC still off by %.2f dB 3 s after a cough",
              gat[1][3] - gat[0][3]);
        free(x0); free(x1); free(nz); free(bn);
    }
    e1_svm_destroy(s);
}

/* ------------------------------------------------------------------------------------------ */
/* all modules: sample rates, robustness, silence, denormals                                    */
/* ------------------------------------------------------------------------------------------ */
static void test_robustness(void)
{
    printf("[robustness] all modules x sample rates: finite, denormal-free, silence -> exact 0\n");
    static const double rates[7] = {8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0};
    for (int ri = 0; ri < 7; ++ri) {
        const double fs = rates[ri];
        printf("  %6.0f Hz:", fs);
        for (int k = 0; k < MOD_COUNT; ++k) {
            const double t0 = now_s();
            Mod m = mod_create((ModKind)k, fs);
            const double tc = now_s() - t0;
            CHECK(m.p != NULL, "%s create failed at %.0f", mod_name[k], fs);
            if (!m.p) continue;
            const size_t n1 = (size_t)fs, n = (size_t)(fs * 4.0);
            float *l = fbuf(n), *r = fbuf(n);
            make_stereo(l, r, n1, 2, 9u + (uint32_t)k, -3.0);
            for (size_t i = 0; i < n1; i += (size_t)(fs / 7)) { l[i] = 1.0f; r[i] = -1.0f; } /* full-scale clicks */
            mod_set(&m, 0.85f, (k == MOD_SURROUND) ? E1FX_SPK_STEREO : (k == MOD_SVM ? E1FX_SVM_LOUD : 0));
            mod_reset(&m);
            Ev ev[2] = {{n1 / 3, 1.0f, (k == MOD_SVM) ? E1FX_SVM_NIGHT : 0}, {n1 / 2, 0.4f, 0}};
            run(&m, l, r, n, 0, ev, 2, (uint32_t)(ri * 31 + k + 1));
            const size_t bad = count_bad(l, n) + (m.nch > 1 ? count_bad(r, n) : 0);
            CHECK(bad == 0, "%s at %.0f Hz: %zu non-finite/denormal samples", mod_name[k], fs, bad);
            size_t last = 0;
            for (size_t i = 0; i < n; ++i) if (l[i] != 0.0f || (m.nch > 1 && r[i] != 0.0f)) last = i;
            const double zero_after = (double)(last + 1 - n1) / fs;
            CHECK(last + 1 < n - (size_t)(0.5 * fs), "%s at %.0f Hz: never reaches exact zero", mod_name[k], fs);
            double peak = 0.0;
            for (size_t i = 0; i < n; ++i) if (fabs((double)l[i]) > peak) peak = fabs((double)l[i]);
            printf(" %s(lat %u, zero after %.2fs, peak %.2f, create %.0f ms)", mod_name[k], mod_latency(&m), zero_after, peak, tc * 1e3);
            if (k == MOD_SVM || k == MOD_SVM_MIC) {
                const unsigned want = fs > 24000.0 ? 96u : 48u;
                CHECK(mod_latency(&m) == want, "svm latency %u at %.0f", mod_latency(&m), fs);
            }
            /* silence from reset must be silence immediately */
            memset(l, 0, n1 * sizeof(float)); memset(r, 0, n1 * sizeof(float));
            mod_reset(&m);
            run(&m, l, r, n1, 128, NULL, 0, 0);
            size_t nz = 0;
            for (size_t i = 0; i < n1; ++i) nz += (l[i] != 0.0f) + (m.nch > 1 && r[i] != 0.0f);
            CHECK(nz == 0, "%s: silence in -> %zu non-zero samples", mod_name[k], nz);
            free(l); free(r);
            mod_destroy(&m);
        }
        printf("\n");
    }

    /* NaN/garbage parameters must be sanitised */
    {
        Mod m = mod_create(MOD_SURROUND, 44100.0);
        if (m.p) {
            const size_t n = 4410;
            float *l = fbuf(n), *r = fbuf(n);
            make_stereo(l, r, n, 1, 3u, -10.0);
            mod_set(&m, NAN, 7);
            run(&m, l, r, n, 128, NULL, 0, 0);
            CHECK(count_bad(l, n) == 0 && count_bad(r, n) == 0, "NaN level produced bad output");
            e1_crystal_set(NULL, 1.0f);
            free(l); free(r);
            mod_destroy(&m);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* performance                                                                                  */
/* ------------------------------------------------------------------------------------------ */
static void test_perf(int quick)
{
    const double fs = 44100.0;
    const double secs = quick ? 10.0 : 60.0;
    const size_t n = (size_t)(fs * secs);
    printf("[performance] %.0f s of stereo at 44.1 kHz, 128-frame blocks, single thread\n", secs);
    float *l = fbuf(n), *r = fbuf(n), *bl = fbuf(n), *br = fbuf(n);
    make_stereo(bl, br, n, 2, 123u, -18.0);
    static const float lv[MOD_COUNT] = {0.4f, 0.65f, 0.5f, 0.74f, 0.74f};
    double total = 0.0;
    for (int k = 0; k < MOD_COUNT; ++k) {
        Mod m = mod_create((ModKind)k, fs);
        if (!m.p) continue;
        mod_set(&m, lv[k], 0);
        mod_reset(&m);
        memcpy(l, bl, n * sizeof(float)); memcpy(r, br, n * sizeof(float));
        const double t0 = now_s();
        run(&m, l, r, n, 128, NULL, 0, 0);
        const double t = now_s() - t0;
        if (k != MOD_SVM_MIC) total += t;
        printf("  %-13s %8.1f ms  = %6.0fx real time, %.3f %% of one core, %.2f us per 128-frame block\n",
               mod_name[k], t * 1e3, secs / t, 100.0 * t / secs, t * 1e6 / ((double)n / 128.0));
        mod_destroy(&m);
    }
    printf("  render modules combined: %.3f %% of one core\n", 100.0 * total / secs);

    /* worst case for the surround: permanent crossfades (parameter automation every 64 samples) */
    {
        Mod m = mod_create(MOD_SURROUND, fs);
        if (m.p) {
            memcpy(l, bl, n * sizeof(float)); memcpy(r, br, n * sizeof(float));
            mod_reset(&m);
            const double t0 = now_s();
            for (size_t p = 0; p < n; p += 128) {
                mod_set(&m, (float)((p / 128) % 2), (int)((p / 4096) % 2));
                mod_process(&m, l + p, r + p, (int)((n - p) < 128 ? n - p : 128));
            }
            const double t = now_s() - t0;
            printf("  surround with continuous crossfades: %.3f %% of one core\n", 100.0 * t / secs);
            mod_destroy(&m);
        }
    }
    free(l); free(r); free(bl); free(br);
}

int main(int argc, char **argv)
{
    const int quick = (argc > 1 && strcmp(argv[1], "quick") == 0);
    printf("E1 Studio libe1dsp spatial-module tests%s\n", quick ? " (quick)" : "");
    const double t0 = now_s();
    test_hrtf_model();
    test_surround(quick);
    test_crystal();
    test_dialog();
    test_svm(quick);
    test_robustness();
    test_perf(quick);
    printf("\n%d checks passed, %d failed (%.1f s)\n", g_pass, g_fail, now_s() - t0);
    return g_fail;
}
