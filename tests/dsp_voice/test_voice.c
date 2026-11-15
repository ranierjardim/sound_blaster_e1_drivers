/*
 * test_voice.c - unit tests for the libe1dsp microphone modules (owner: dsp-voice).
 *   e1_fft.c   : real FFT vs. naive DFT, round trip; WOLA perfect reconstruction
 *   e1_nr.c    : SNR improvement (white / pink noise), clean-speech distortion, latency,
 *                silence -> exact zero, NaN/Inf robustness, stereo linking, speed
 *   e1_voicefx.c: preset table, Neutral transparency, pitch ratio accuracy, intonation
 *                (Robot), envelope scaling, all presets stable, silence, speed
 * Exit code 0 = all passed.
 */
#include "e1dsp_modules.h"
#include "e1_fft.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI 3.14159265358979323846
#define FS 44100.0
#define BLK E1DSP_BLOCK
#define LEN(sec) ((int)((sec) * FS) / BLK * BLK) /* lengths are whole blocks, as the chain delivers */

static int g_fail = 0, g_pass = 0;

static void check(int ok, const char *name, const char *fmt, double v)
{
    printf("  [%s] %-58s ", ok ? "PASS" : "FAIL", name);
    printf(fmt, v);
    printf("\n");
    if (ok)
        ++g_pass;
    else
        ++g_fail;
}

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* ---------------- deterministic signal generators ---------------- */
static unsigned long long g_rng = 0x1234567ULL;
static double urand(void) /* [0,1) */
{
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(g_rng >> 11) * (1.0 / 9007199254740992.0);
}
static double grand(void)
{
    double u1 = urand(), u2 = urand();
    if (u1 < 1e-300)
        u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2);
}

static void gen_white(float *x, int n)
{
    for (int i = 0; i < n; ++i)
        x[i] = (float)grand();
}

static void gen_pink(float *x, int n) /* Paul Kellet's refined pink filter */
{
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (int i = 0; i < n; ++i) {
        double w = grand();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        x[i] = (float)(b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
        b6 = w * 0.115926;
    }
}

static double formant_gain(double f)
{
    static const double fc[3] = {550.0, 1500.0, 2500.0}, bw[3] = {90.0, 120.0, 180.0}, am[3] = {1.0, 0.5, 0.3};
    double g = 0.05;
    for (int i = 0; i < 3; ++i) {
        double d = (f - fc[i]) / (0.5 * bw[i]);
        g += am[i] / (1.0 + d * d);
    }
    return g;
}

/* Speech-like: harmonic bursts (f0 100..220 Hz, drift + vibrato), formant-shaped harmonics,
   bursts 180..420 ms separated by 120..300 ms pauses with 10 ms ramps. Peak ~0.3. */
static void gen_speechlike(float *x, int n, double fs)
{
    double ph[64] = {0};
    int i = 0;
    double base = 140.0;
    while (i < n) {
        int on = (int)((0.18 + 0.24 * urand()) * fs), off = (int)((0.12 + 0.18 * urand()) * fs);
        base = 100.0 + 120.0 * urand();
        int ramp = (int)(0.010 * fs);
        for (int k = 0; k < on && i < n; ++k, ++i) {
            double t = (double)k / fs;
            double f0 = base * (1.0 + 0.12 * sin(2 * PI * 1.3 * t) + 0.015 * sin(2 * PI * 5.0 * t));
            double env = 1.0;
            if (k < ramp)
                env = 0.5 - 0.5 * cos(PI * k / ramp);
            else if (k > on - ramp)
                env = 0.5 - 0.5 * cos(PI * (on - k) / ramp);
            double s = 0.0;
            for (int h = 1; h <= 60; ++h) {
                double fh = f0 * h;
                if (fh > 5000.0)
                    break;
                ph[h] += 2 * PI * fh / fs;
                if (ph[h] > 2 * PI)
                    ph[h] -= 2 * PI;
                s += formant_gain(fh) / sqrt((double)h) * sin(ph[h]);
            }
            x[i] = (float)(0.08 * env * s);
        }
        for (int k = 0; k < off && i < n; ++k, ++i)
            x[i] = 0.0f;
    }
}

/* sustained vowel: harmonics of f0 shaped by the formant_gain() envelope, up to 5 kHz */
static void gen_vowel(float *x, int n, double fs, double f0)
{
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int h = 1; f0 * h < 5000.0; ++h)
            s += formant_gain(f0 * h) / sqrt((double)h) * sin(2 * PI * f0 * h * i / fs);
        x[i] = (float)(0.08 * s);
    }
}

static void gen_harmonic(float *x, int n, double fs, double f0, int nh)
{
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int h = 1; h <= nh; ++h)
            if (f0 * h < 0.45 * fs)
                s += sin(2 * PI * f0 * h * i / fs) / h;
        x[i] = (float)(0.25 * s);
    }
}

static double rms(const float *x, int n)
{
    double e = 0;
    for (int i = 0; i < n; ++i)
        e += (double)x[i] * x[i];
    return sqrt(e / (n > 0 ? n : 1));
}

static int all_finite(const float *x, int n)
{
    for (int i = 0; i < n; ++i)
        if (!isfinite(x[i]))
            return 0;
    return 1;
}

/* ---------------- block drivers ---------------- */
typedef void (*proc_fn)(void *m, float *const *ch, int nch, int n);
static void nr_proc(void *m, float *const *ch, int nch, int n) { e1_nr_process((E1Nr *)m, ch, nch, n); }
static void vfx_proc(void *m, float *const *ch, int nch, int n) { e1_vfx_process((E1VoiceFx *)m, ch, nch, n); }

static void run_mono(proc_fn f, void *m, const float *in, float *out, int n)
{
    memcpy(out, in, sizeof(float) * (size_t)n);
    for (int i = 0; i < n; i += BLK) {
        int len = n - i < BLK ? n - i : BLK;
        float *ch[1] = {out + i};
        f(m, ch, 1, len);
    }
}

/* magnitude of the Hann-windowed DTFT of x at frequency f */
static double dtft_mag(const float *x, int n, double fs, double f)
{
    double re = 0, im = 0, w = 2 * PI * f / fs;
    double c = cos(w), s = sin(w), cr = 1, ci = 0;
    for (int i = 0; i < n; ++i) {
        double win = 0.5 - 0.5 * cos(2 * PI * i / (n - 1));
        re += win * x[i] * cr;
        im -= win * x[i] * ci;
        double t = cr * c - ci * s;
        ci = cr * s + ci * c;
        cr = t;
    }
    return sqrt(re * re + im * im);
}

static double peak_freq(const float *x, int n, double fs, double f0, double f1, double step)
{
    double best = f0, bm = -1;
    for (double f = f0; f <= f1; f += step) {
        double m = dtft_mag(x, n, fs, f);
        if (m > bm) {
            bm = m;
            best = f;
        }
    }
    return best;
}

/* standard deviation of v[0..n) after rejecting values more than 0.5 (octave) from the median,
   i.e. robust against octave errors of the simple test pitch estimator */
static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double robust_sd(const double *v, int n)
{
    if (n < 3)
        return 0.0;
    double *t = malloc(sizeof(double) * n);
    memcpy(t, v, sizeof(double) * n);
    qsort(t, n, sizeof(double), cmp_dbl);
    double med = t[n / 2], s = 0, s2 = 0;
    int c = 0;
    for (int i = 0; i < n; ++i)
        if (fabs(v[i] - med) <= 0.5) {
            s += v[i];
            s2 += v[i] * v[i];
            ++c;
        }
    free(t);
    return c > 1 ? sqrt(fmax(0.0, s2 / c - (s / c) * (s / c))) : 0.0;
}

/* power-weighted mean frequency of x[0..n) (Hann window, n power of two) between f0 and f1 */
static double spectral_centroid(const float *x, int n, double fs, double f0, double f1)
{
    E1Fft *f = e1_fft_create(n);
    float *t = malloc(sizeof(float) * n), *X = malloc(sizeof(float) * (n + 2));
    for (int i = 0; i < n; ++i)
        t[i] = x[i] * (float)(0.5 - 0.5 * cos(2 * PI * i / n));
    e1_fft_r2c(f, t, X);
    double num = 0, den = 0;
    for (int k = 0; k <= n / 2; ++k) {
        double fk = k * fs / n;
        if (fk < f0 || fk > f1)
            continue;
        double p = (double)X[2 * k] * X[2 * k] + (double)X[2 * k + 1] * X[2 * k + 1];
        num += fk * p;
        den += p;
    }
    free(t);
    free(X);
    e1_fft_destroy(f);
    return den > 0 ? num / den : 0.0;
}

/* simple normalised-autocorrelation f0 estimate on x[0..n) (60..600 Hz), 0 if unvoiced */
static double acf_f0(const float *x, int n, double fs)
{
    int tmin = (int)(fs / 600), tmax = (int)(fs / 60);
    double best = 0;
    int bt = -1;
    static double r[1024];
    double e0 = 0;
    for (int i = 0; i < n - tmax; ++i)
        e0 += (double)x[i] * x[i];
    if (e0 < 1e-6)
        return 0;
    for (int t = tmin; t <= tmax && t < 1024; ++t) {
        double s = 0, e1 = 0;
        for (int i = 0; i < n - tmax; ++i) {
            s += (double)x[i] * x[i + t];
            e1 += (double)x[i + t] * x[i + t];
        }
        r[t] = s / sqrt(e0 * e1 + 1e-30);
        if (r[t] > best) {
            best = r[t];
            bt = t;
        }
    }
    if (best < 0.5 || bt < 0)
        return 0;
    for (int t = tmin + 1; t < tmax; ++t) /* first peak above 0.9 * best (avoid sub-octaves) */
        if (r[t] >= 0.9 * best && r[t] >= r[t - 1] && r[t] >= r[t + 1]) {
            bt = t;
            break;
        }
    double tau = bt;
    if (bt > tmin && bt < tmax) {
        double a = r[bt - 1], b = r[bt], c = r[bt + 1], den = a - 2 * b + c;
        if (den < 0)
            tau += 0.5 * (a - c) / den;
    }
    return fs / tau;
}

/* ======================================================================================== */
static void test_fft(void)
{
    printf("FFT\n");
    double worst = 0, worst_rt = 0;
    for (int n = 16; n <= 4096; n *= 2) {
        E1Fft *f = e1_fft_create(n);
        float *x = malloc(sizeof(float) * n), *X = malloc(sizeof(float) * (n + 2)), *y = malloc(sizeof(float) * n);
        gen_white(x, n);
        e1_fft_r2c(f, x, X);
        double emax = 0, xmax = 0;
        for (int k = 0; k <= n / 2; ++k) {
            double re = 0, im = 0;
            for (int t = 0; t < n; ++t) {
                double a = -2 * PI * (double)k * t / n;
                re += x[t] * cos(a);
                im += x[t] * sin(a);
            }
            double e = hypot(re - X[2 * k], im - X[2 * k + 1]);
            if (e > emax)
                emax = e;
            double mg = hypot(re, im);
            if (mg > xmax)
                xmax = mg;
        }
        e1_fft_c2r(f, X, y);
        double ert = 0;
        for (int i = 0; i < n; ++i)
            if (fabs(y[i] - x[i]) > ert)
                ert = fabs(y[i] - x[i]);
        if (emax / xmax > worst)
            worst = emax / xmax;
        if (ert > worst_rt)
            worst_rt = ert;
        free(x);
        free(X);
        free(y);
        e1_fft_destroy(f);
    }
    check(20 * log10(worst) < -100, "r2c vs naive DFT, n=16..4096 (max rel. error)", "%.1f dB", 20 * log10(worst));
    check(20 * log10(worst_rt) < -110, "c2r(r2c(x)) round trip (max abs. error, |x|~1)", "%.1f dB", 20 * log10(worst_rt));
}

static void test_wola_pr(void)
{
    printf("STFT/WOLA framework (identity callback)\n");
    static const int cfg[][2] = {{512, 128}, {2048, 256}, {1024, 128}, {256, 64}, {2048, 512}};
    const int n = LEN(3);
    float *x = malloc(sizeof(float) * n), *y = malloc(sizeof(float) * n), *x2 = malloc(sizeof(float) * n),
          *y2 = malloc(sizeof(float) * n);
    gen_white(x, n);
    gen_speechlike(x2, n, FS);
    for (size_t c = 0; c < sizeof(cfg) / sizeof(cfg[0]); ++c) {
        E1Wola *w = e1_wola_create(cfg[c][0], cfg[c][1], 2, BLK);
        unsigned L = e1_wola_latency(w);
        memcpy(y, x, sizeof(float) * n);
        memcpy(y2, x2, sizeof(float) * n);
        for (int i = 0; i < n; i += BLK) {
            int len = n - i < BLK ? n - i : BLK;
            float *ch[2] = {y + i, y2 + i};
            e1_wola_process(w, ch, 2, len, NULL, NULL);
        }
        double es = 0, ee = 0;
        for (int i = (int)L; i < n; ++i) {
            double d1 = y[i] - x[i - L], d2 = y2[i] - x2[i - L];
            es += (double)x[i - L] * x[i - L] + (double)x2[i - L] * x2[i - L];
            ee += d1 * d1 + d2 * d2;
        }
        double err = 10 * log10(ee / es + 1e-300);
        char name[96];
        snprintf(name, sizeof name, "PR nfft=%d hop=%d stereo, latency %u (= nfft-%d)", cfg[c][0], cfg[c][1], L,
                 cfg[c][0] - (int)L);
        int lat_ok = (int)L == cfg[c][0] - (cfg[c][1] > BLK ? BLK : cfg[c][1]);
        check(err < -90 && lat_ok && e1_wola_latency(w) == L, name, "err %.1f dB", err);
        e1_wola_destroy(w);
    }
    /* irregular call sizes: latency may grow once, stream stays exact */
    {
        E1Wola *w = e1_wola_create(512, 128, 1, BLK);
        memcpy(y, x, sizeof(float) * n);
        int i = 0, k = 0;
        static const int sizes[] = {100, 37, 128, 1, 513, 64, 300};
        while (i < n) {
            int len = sizes[k++ % 7];
            if (len > n - i)
                len = n - i;
            float *ch[1] = {y + i};
            e1_wola_process(w, ch, 1, len, NULL, NULL);
            i += len;
        }
        unsigned L = e1_wola_latency(w);
        double es = 0, ee = 0;
        for (int j = n / 2; j < n; ++j) {
            double d = y[j] - x[j - L];
            es += (double)x[j - L] * x[j - L];
            ee += d * d;
        }
        double err = 10 * log10(ee / es + 1e-300);
        check(err < -90 && L <= 511, "PR with irregular call sizes (latency grew to <= nfft-1)", "err %.1f dB", err);
        e1_wola_destroy(w);
    }
    free(x);
    free(y);
    free(x2);
    free(y2);
}

/* ======================================================================================== */
static double nr_snr_test(const float *clean, const float *noise, int n, double snr_in_db, float strength,
                          double *snr_in_out, int *finite)
{
    float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);
    double es = 0, en = 0;
    for (int i = 0; i < n; ++i) {
        es += (double)clean[i] * clean[i];
        en += (double)noise[i] * noise[i];
    }
    double g = sqrt(es / en / pow(10.0, snr_in_db / 10.0));
    for (int i = 0; i < n; ++i)
        in[i] = clean[i] + (float)(g * noise[i]);
    E1Nr *m = e1_nr_create(FS, 1);
    e1_nr_set(m, strength);
    run_mono(nr_proc, m, in, out, n);
    unsigned L = e1_nr_latency(m);
    int s0 = (int)(2.0 * FS);
    double S = 0, Ni = 0, No = 0;
    for (int i = s0; i + (int)L < n; ++i) {
        double s = clean[i], d = out[i + L] - s, ni = in[i] - s;
        S += s * s;
        Ni += ni * ni;
        No += d * d;
    }
    *snr_in_out = 10 * log10(S / Ni);
    *finite = all_finite(out, n);
    e1_nr_destroy(m);
    free(in);
    free(out);
    return 10 * log10(S / No);
}

static void test_nr(void)
{
    printf("Noise reduction\n");
    const int n = LEN(12);
    float *clean = malloc(sizeof(float) * n), *noise = malloc(sizeof(float) * n);
    gen_speechlike(clean, n, FS);

    struct { const char *name; int pink; double snr; float st; double need; } cases[] = {
        {"white 5 dB, strength 0.5", 0, 5.0, 0.5f, 6.0},
        {"pink  5 dB, strength 0.5", 1, 5.0, 0.5f, 5.0},
        {"white 15 dB, strength 0.5", 0, 15.0, 0.5f, 5.0},
        {"white 5 dB, strength 0.0", 0, 5.0, 0.0f, 3.0},
        {"white 5 dB, strength 1.0", 0, 5.0, 1.0f, 6.0},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        if (cases[c].pink)
            gen_pink(noise, n);
        else
            gen_white(noise, n);
        double sin_, sout;
        int fin;
        sout = nr_snr_test(clean, noise, n, cases[c].snr, cases[c].st, &sin_, &fin);
        char nm[128];
        snprintf(nm, sizeof nm, "strict SNR (distortion counts) %s: %.1f -> %.1f", cases[c].name, sin_, sout);
        check(fin && sout - sin_ >= cases[c].need, nm, "improvement %+.1f dB", sout - sin_);
    }

    /* clean speech: must stay essentially undistorted (noise floor -90 dBFS) */
    {
        float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);
        gen_white(noise, n);
        for (int i = 0; i < n; ++i)
            in[i] = clean[i] + 3e-5f * noise[i];
        E1Nr *m = e1_nr_create(FS, 1);
        e1_nr_set(m, 0.5f);
        run_mono(nr_proc, m, in, out, n);
        unsigned L = e1_nr_latency(m);
        double S = 0, E = 0;
        for (int i = (int)(2 * FS); i + (int)L < n; ++i) {
            double d = out[i + L] - in[i];
            S += (double)in[i] * in[i];
            E += d * d;
        }
        double sdr = 10 * log10(S / E);
        check(sdr > 25.0, "clean speech-like (+-90 dBFS floor): signal-to-distortion", "%.1f dB", sdr);

        /* latency: cross-correlation peak of output vs input */
        int best = -1;
        double bv = -1e300;
        for (int lag = 0; lag < 1200; ++lag) {
            double s = 0;
            for (int i = (int)(2 * FS); i < (int)(4 * FS); ++i)
                s += (double)in[i] * out[i + lag];
            if (s > bv) {
                bv = s;
                best = lag;
            }
        }
        char nm[96];
        snprintf(nm, sizeof nm, "measured latency %d == reported %u frames", best, L);
        check(best == (int)L && L == 384, nm, "(%.2f ms)", 1000.0 * L / FS);
        e1_nr_destroy(m);
        free(in);
        free(out);
    }

    /* silence -> exact zero; NaN/Inf/huge robustness */
    {
        E1Nr *m = e1_nr_create(FS, 2);
        e1_nr_set(m, 1.0f);
        int nn = 690 * BLK; /* ~2 s */
        float *a = malloc(sizeof(float) * nn), *b = malloc(sizeof(float) * nn);
        gen_white(a, nn);
        for (int i = 0; i < nn; ++i) {
            a[i] *= 0.1f;
            b[i] = a[i];
        }
        a[1000] = NAN;
        a[2000] = INFINITY;
        b[3000] = -INFINITY;
        a[4000] = 1e30f;
        for (int i = 0; i < nn; i += BLK) {
            float *ch[2] = {a + i, b + i};
            e1_nr_process(m, ch, 2, nn - i < BLK ? nn - i : BLK);
        }
        int fin = all_finite(a, nn) && all_finite(b, nn);
        check(fin, "NaN / +-Inf / 1e30 input samples -> finite output", "", 0.0);
        int zl = 345 * BLK; /* ~1 s */
        float *z = calloc(zl, sizeof(float)), *z2 = calloc(zl, sizeof(float));
        for (int i = 0; i < zl; i += BLK) {
            float *ch[2] = {z + i, z2 + i};
            e1_nr_process(m, ch, 2, zl - i < BLK ? zl - i : BLK);
        }
        int nz = 0, first = zl;
        for (int i = zl - 1; i >= 0; --i)
            if (z[i] != 0.0f || z2[i] != 0.0f) {
                first = i + 1;
                break;
            }
        for (int i = first; i < zl; ++i)
            nz += (z[i] != 0.0f);
        check(first <= (int)e1_nr_latency(m) + 512 && nz == 0,
              "digital silence -> exact 0.0f (frames until exact zero <= latency+nfft)", "%.0f", (double)first);
        free(z);
        free(z2);
        free(a);
        free(b);
        e1_nr_destroy(m);
    }

    /* stereo linked: identical channels give identical output equal to the mono result */
    {
        int nn = 1034 * BLK; /* ~3 s */
        float *in = malloc(sizeof(float) * nn), *mo = malloc(sizeof(float) * nn), *l = malloc(sizeof(float) * nn),
              *r = malloc(sizeof(float) * nn);
        gen_white(noise, nn);
        for (int i = 0; i < nn; ++i)
            in[i] = clean[i] + 0.05f * noise[i];
        E1Nr *m1 = e1_nr_create(FS, 1), *m2 = e1_nr_create(FS, 2);
        run_mono(nr_proc, m1, in, mo, nn);
        memcpy(l, in, sizeof(float) * nn);
        memcpy(r, in, sizeof(float) * nn);
        for (int i = 0; i < nn; i += BLK) {
            float *ch[2] = {l + i, r + i};
            e1_nr_process(m2, ch, 2, nn - i < BLK ? nn - i : BLK);
        }
        double d = 0;
        for (int i = 0; i < nn; ++i)
            d = fmax(d, fmax(fabs(l[i] - r[i]), fabs(l[i] - mo[i])));
        check(d < 1e-6, "stereo (identical L/R) == mono output, L == R", "max diff %.2g", d);
        e1_nr_destroy(m1);
        e1_nr_destroy(m2);
        free(in);
        free(mo);
        free(l);
        free(r);
    }

    /* speed */
    {
        int nn = LEN(30);
        float *in = malloc(sizeof(float) * nn), *out = malloc(sizeof(float) * nn);
        gen_white(in, nn);
        for (int i = 0; i < nn; ++i)
            in[i] = 0.05f * in[i] + clean[i % n];
        E1Nr *m = e1_nr_create(FS, 1);
        double t0 = now_s();
        run_mono(nr_proc, m, in, out, nn);
        double dt = now_s() - t0;
        check(30.0 / dt >= 20.0, "speed, mono 44.1 kHz (x real time)", "%.0fx", 30.0 / dt);
        E1Nr *m2 = e1_nr_create(48000.0, 2);
        float *r = malloc(sizeof(float) * nn);
        memcpy(r, in, sizeof(float) * nn);
        t0 = now_s();
        for (int i = 0; i + BLK <= nn; i += BLK) {
            float *ch[2] = {out + i, r + i};
            e1_nr_process(m2, ch, 2, BLK);
        }
        dt = now_s() - t0;
        check(nn / 48000.0 / dt >= 20.0, "speed, stereo 48 kHz (x real time)", "%.0fx", nn / 48000.0 / dt);
        e1_nr_destroy(m2);
        e1_nr_destroy(m);
        free(r);
        free(in);
        free(out);
    }
    free(clean);
    free(noise);
}

/* ======================================================================================== */
static const E1VfxPreset *find_preset(const char *name)
{
    for (int i = 0; i < e1_vfx_preset_count; ++i)
        if (strcmp(e1_vfx_presets[i].name, name) == 0)
            return &e1_vfx_presets[i];
    return NULL;
}

static void test_vfx(void)
{
    printf("VoiceFX\n");
    /* preset table */
    {
        static const float lo[8] = {200, 800, 1600, 0.25f, 0.5f, -1, 0, 0}, hi[8] = {800, 1800, 2750, 4, 2, 5, 0.2f, 0.4f};
        int ok = e1_vfx_preset_count == 18;
        for (int i = 0; i < e1_vfx_preset_count; ++i) {
            ok &= e1_vfx_presets[i].name && e1_vfx_presets[i].name[0];
            for (int j = 0; j < 8; ++j)
                ok &= e1_vfx_presets[i].p[j] >= lo[j] && e1_vfx_presets[i].p[j] <= hi[j];
        }
        const float neutral[8] = {400, 1400, 2000, 1, 1, 1, 0, 0};
        ok &= memcmp(e1_vfx_presets[0].p, neutral, sizeof neutral) == 0;
        const E1VfxPreset *d = find_preset("Deep Voice");
        ok &= d && d->p[3] == 0.42912f && d->p[4] == 1.20132f && d->p[0] == 339.54f;
        check(ok, "preset table: 18 entries, Neutral first, values in range", "count %.0f", (double)e1_vfx_preset_count);
    }

    const int n = LEN(4);
    float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);

    /* Neutral transparency + latency */
    {
        gen_speechlike(in, n, FS);
        E1VoiceFx *m = e1_vfx_create(FS, 1);
        e1_vfx_set(m, e1_vfx_presets[0].p);
        run_mono(vfx_proc, m, in, out, n);
        unsigned L = e1_vfx_latency(m);
        double S = 0, E = 0;
        for (int i = (int)L; i < n; ++i) {
            double d = out[i] - in[i - L];
            S += (double)in[i - L] * in[i - L];
            E += d * d;
        }
        double err = 10 * log10(E / S + 1e-300);
        check(err < -90, "Neutral preset: output vs delayed input", "err %.1f dB", err);
        char nm[96];
        snprintf(nm, sizeof nm, "latency reported %u frames (nfft 2048 - block 128)", L);
        check(L == 1920, nm, "(%.1f ms)", 1000.0 * L / FS);
        e1_vfx_destroy(m);
    }

    /* pitch ratio accuracy on a 200 Hz harmonic tone */
    {
        struct { float pm; double expect; } pc[] = {{2.0f, 400.0}, {0.5f, 100.0}, {1.5f, 300.0}, {1.25f, 250.0}};
        for (size_t c = 0; c < sizeof(pc) / sizeof(pc[0]); ++c) {
            gen_harmonic(in, n, FS, 200.0, 20);
            E1VoiceFx *m = e1_vfx_create(FS, 1);
            float p[8] = {400, 1400, 2000, pc[c].pm, 1, 1, 0, 0};
            e1_vfx_set(m, p);
            e1_vfx_reset(m); /* start with the target already applied */
            run_mono(vfx_proc, m, in, out, n);
            const float *seg = out + (int)(1.0 * FS);
            int sl = (int)(2.0 * FS);
            double e = pc[c].expect;
            double f = peak_freq(seg, sl, FS, 0.8 * e, 1.2 * e, 0.25);
            double fa = acf_f0(seg, 8192, FS);
            double m_peak = dtft_mag(seg, sl, FS, f), m200 = dtft_mag(seg, sl, FS, 200.0);
            char nm[128];
            snprintf(nm, sizeof nm, "PitchMod %.2f: 200 Hz -> %.2f Hz (acf %.1f), expect %.0f +-3%%", pc[c].pm, f, fa, e);
            int ok = fabs(f - e) / e < 0.03 && fabs(fa - e) / e < 0.03 && all_finite(out, n);
            if (pc[c].pm == 2.0f) /* nothing left at the original fundamental */
                ok &= 20 * log10(m200 / m_peak) < -30;
            check(ok, nm, "dev %+.2f%%", 100.0 * (f - e) / e);
            e1_vfx_destroy(m);
        }
    }

    /* Robot (variability ~0): intonation flattened, pitch x1.394 */
    {
        /* gliding harmonic tone: f0 = 150 * 2^(0.3 sin(2 pi 0.7 t)) */
        double ph = 0;
        for (int i = 0; i < n; ++i) {
            double f0 = 150.0 * pow(2.0, 0.3 * sin(2 * PI * 0.7 * i / FS));
            ph += 2 * PI * f0 / FS;
            double s = 0;
            for (int h = 1; h <= 25; ++h)
                s += sin(h * ph) / h;
            in[i] = (float)(0.2 * s);
        }
        E1VoiceFx *m = e1_vfx_create(FS, 1);
        e1_vfx_set(m, find_preset("Robot")->p);
        e1_vfx_reset(m);
        run_mono(vfx_proc, m, in, out, n);
        unsigned L = e1_vfx_latency(m);
        static double li[512], lo[512];
        double mo = 0;
        int cnt = 0;
        for (int i = (int)(1.5 * FS); i + 2048 + (int)L < n && cnt < 512; i += 1024) {
            double fi = acf_f0(in + i, 2048, FS), fo = acf_f0(out + i + L, 2048, FS);
            if (fi <= 0 || fo <= 0)
                continue;
            li[cnt] = log2(fi);
            lo[cnt] = log2(fo);
            mo += fo;
            ++cnt;
        }
        double sdi = robust_sd(li, cnt), sdo = robust_sd(lo, cnt);
        char nm[128];
        snprintf(nm, sizeof nm, "Robot: f0 spread %.3f -> %.3f oct, mean f0 %.0f Hz", sdi, sdo, mo / cnt);
        check(cnt > 20 && sdo < 0.25 * sdi && all_finite(out, n), nm, "ratio %.2f", sdo / sdi);
        e1_vfx_destroy(m);
    }

    /* intonation controls on a steady 150 Hz vowel: quiver (Elderly) and contour (From Up North)
       must create pitch movement; variability 2.5 (Unstable) must widen a gliding contour */
    {
        struct { const char *preset; int glide; double need; } ic[] = {
            {"Elderly", 0, 0.04}, {"From Up North", 0, 0.04}, {"Unstable", 1, 1.8}};
        for (int c = 0; c < 3; ++c) {
            if (ic[c].glide) {
                double ph = 0;
                for (int i = 0; i < n; ++i) {
                    double f0 = 150.0 * pow(2.0, 0.15 * sin(2 * PI * 0.7 * i / FS));
                    ph += 2 * PI * f0 / FS;
                    double sv = 0;
                    for (int h = 1; h <= 25; ++h)
                        sv += formant_gain(150.0 * h) / sqrt((double)h) * sin(h * ph);
                    in[i] = (float)(0.08 * sv);
                }
            } else {
                gen_vowel(in, n, FS, 150.0);
            }
            E1VoiceFx *m = e1_vfx_create(FS, 1);
            e1_vfx_set(m, find_preset(ic[c].preset)->p);
            e1_vfx_reset(m);
            run_mono(vfx_proc, m, in, out, n);
            unsigned L = e1_vfx_latency(m);
            static double li[1024], lo[1024];
            int cnt = 0;
            for (int i = (int)(1.5 * FS); i + 2048 + (int)L < n && cnt < 1024; i += 512) {
                double fi = acf_f0(in + i, 2048, FS), fo = acf_f0(out + i + L, 2048, FS);
                if (fi <= 0 || fo <= 0)
                    continue;
                li[cnt] = log2(fi);
                lo[cnt] = log2(fo);
                ++cnt;
            }
            double sdi = robust_sd(li, cnt), sdo = robust_sd(lo, cnt);
            char nm[128];
            int ok;
            if (ic[c].glide) {
                snprintf(nm, sizeof nm, "%s (variability 2.5): f0 spread %.3f -> %.3f oct", ic[c].preset, sdi, sdo);
                ok = cnt > 20 && sdo > ic[c].need * sdi;
            } else {
                snprintf(nm, sizeof nm, "%s: steady 150 Hz vowel, output f0 spread %.3f oct", ic[c].preset, sdo);
                ok = cnt > 20 && sdo > ic[c].need;
            }
            check(ok && all_finite(out, n), nm, "(%.0f windows)", (double)cnt);
            e1_vfx_destroy(m);
        }
    }

    /* Munchkin (EnvelopeMod 0.6, pitch 1): formants up, f0 unchanged */
    {
        gen_speechlike(in, n, FS);
        E1VoiceFx *m = e1_vfx_create(FS, 1);
        e1_vfx_set(m, find_preset("Munchkin")->p);
        e1_vfx_reset(m);
        run_mono(vfx_proc, m, in, out, n);
        unsigned L = e1_vfx_latency(m);
        /* spectral centroid (100 Hz .. 6 kHz) of input vs output from full FFT power spectra */
        const int sl = 16384, seg0 = (int)(1.0 * FS);
        double ci = spectral_centroid(in + seg0, sl, FS, 100.0, 6000.0);
        double co = spectral_centroid(out + seg0 + L, sl, FS, 100.0, 6000.0);
        char nm[128];
        snprintf(nm, sizeof nm, "Munchkin (EnvelopeMod 0.6): spectral centroid %.0f -> %.0f Hz", ci, co);
        check(co > 1.3 * ci && all_finite(out, n), nm, "x%.2f", co / ci);
        e1_vfx_destroy(m);
    }

    /* Demon (EnvelopeMod 2, F3 2600, pitch 0.69): formants down, pitch down */
    {
        gen_vowel(in, n, FS, 180.0);
        E1VoiceFx *m = e1_vfx_create(FS, 1);
        e1_vfx_set(m, find_preset("Demon")->p);
        e1_vfx_reset(m);
        run_mono(vfx_proc, m, in, out, n);
        unsigned L = e1_vfx_latency(m);
        const int sl = 16384, seg0 = (int)(1.0 * FS);
        double ci = spectral_centroid(in + seg0, sl, FS, 60.0, 6000.0);
        double co = spectral_centroid(out + seg0 + L, sl, FS, 60.0, 6000.0);
        double f = acf_f0(out + seg0 + L, 8192, FS);
        char nm[128];
        snprintf(nm, sizeof nm, "Demon: centroid %.0f -> %.0f Hz, f0 180 -> %.1f Hz (expect %.1f)", ci, co, f, 180.0 * 0.69);
        check(co < 0.8 * ci && fabs(f - 124.2) / 124.2 < 0.03 && all_finite(out, n), nm, "x%.2f", co / ci);
        e1_vfx_destroy(m);
    }

    /* every preset: finite, sane level, parameter switches mid-stream */
    {
        gen_speechlike(in, n, FS);
        double rin = rms(in + (int)FS, n - (int)FS);
        int ok = 1;
        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < e1_vfx_preset_count; ++i) {
            E1VoiceFx *m = e1_vfx_create(FS, 1);
            e1_vfx_set(m, e1_vfx_presets[i].p);
            run_mono(vfx_proc, m, in, out, n);
            double ro = rms(out + (int)FS, n - (int)FS);
            double db = 20 * log10(ro / rin + 1e-30);
            if (db < lo)
                lo = db;
            if (db > hi)
                hi = db;
            if (!all_finite(out, n) || db < -20 || db > 12) {
                ok = 0;
                printf("    preset %-18s level %+.1f dB%s\n", e1_vfx_presets[i].name, db, all_finite(out, n) ? "" : " NON-FINITE");
            }
            e1_vfx_destroy(m);
        }
        char nm[128];
        snprintf(nm, sizeof nm, "all 18 presets finite, level within [-20,+12] dB (%+.1f..%+.1f)", lo, hi);
        check(ok, nm, "", 0.0);
        /* rapid preset switching on one instance, stereo */
        E1VoiceFx *m = e1_vfx_create(FS, 2);
        float *r = malloc(sizeof(float) * n);
        memcpy(out, in, sizeof(float) * n);
        memcpy(r, in, sizeof(float) * n);
        int pi = 0;
        for (int i = 0; i + BLK <= n; i += BLK) {
            if ((i / BLK) % 40 == 0)
                e1_vfx_set(m, e1_vfx_presets[(pi++) % e1_vfx_preset_count].p);
            float *ch[2] = {out + i, r + i};
            e1_vfx_process(m, ch, 2, BLK);
        }
        double d = 0;
        for (int i = 0; i < n; ++i)
            d = fmax(d, fabs(out[i] - r[i]));
        check(all_finite(out, n) && d < 1e-6, "stereo, preset switched every 40 blocks: finite, L == R", "max diff %.2g", d);
        free(r);
        e1_vfx_destroy(m);
    }

    /* silence -> exact zero, NaN robustness */
    {
        E1VoiceFx *m = e1_vfx_create(FS, 1);
        e1_vfx_set(m, find_preset("Demon")->p);
        gen_white(in, n);
        in[500] = NAN;
        in[900] = INFINITY;
        in[1300] = -1e30f;
        run_mono(vfx_proc, m, in, out, 344 * BLK);
        int fin = all_finite(out, 344 * BLK);
        check(fin, "NaN / Inf / -1e30 input samples -> finite output", "", 0.0);
        memset(in, 0, sizeof(float) * n);
        run_mono(vfx_proc, m, in, out, n);
        int first = n;
        for (int i = n - 1; i >= 0; --i)
            if (out[i] != 0.0f) {
                first = i + 1;
                break;
            }
        check(first <= (int)e1_vfx_latency(m) + 2048,
              "digital silence -> exact 0.0f (frames until exact zero <= latency+nfft)", "%.0f", (double)first);
        e1_vfx_destroy(m);
    }

    /* speed */
    {
        const char *pn[] = {"Male to Female", "Unstable", "Deep Voice"};
        int nn = LEN(30);
        float *a = malloc(sizeof(float) * nn), *b = malloc(sizeof(float) * nn);
        gen_speechlike(a, nn, FS);
        for (int k = 0; k < 3; ++k) {
            E1VoiceFx *m = e1_vfx_create(FS, 1);
            e1_vfx_set(m, find_preset(pn[k])->p);
            double t0 = now_s();
            run_mono(vfx_proc, m, a, b, nn);
            double dt = now_s() - t0;
            char nm[96];
            snprintf(nm, sizeof nm, "speed, mono 44.1 kHz, preset \"%s\" (x real time)", pn[k]);
            check(30.0 / dt >= 20.0, nm, "%.0fx", 30.0 / dt);
            e1_vfx_destroy(m);
        }
        free(a);
        free(b);
    }
    free(in);
    free(out);
}

int main(void)
{
    printf("libe1dsp voice module tests (fs %.0f, block %d)\n", FS, BLK);
    test_fft();
    test_wola_pr();
    test_nr();
    test_vfx();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
