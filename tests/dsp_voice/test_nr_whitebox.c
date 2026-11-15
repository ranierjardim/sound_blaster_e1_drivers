/*
 * test_nr_whitebox.c - white-box noise-reduction metrics (owner: dsp-voice).
 *
 * Includes e1_nr.c directly so the per-frame gains can be applied to the clean speech and to
 * the noise separately ("shadow filtering", as in ITU-T G.160 / P.1100 style evaluations).
 * This separates the two effects that a plain output-SNR measure mixes together:
 *   SNRI  = output speech-component SNR - input SNR  (the usual "SNR improvement")
 *   NA    = noise attenuation (whole signal, and in speech pauses)
 *   SDR   = speech-component distortion (speech out vs. speech in)
 * Build: compiled without e1_nr.c on the command line (it is included here).
 */
#include "../../src/dsp/e1_nr.c"

#include <stdio.h>

#define PI 3.14159265358979323846
#define FS 44100.0
#define BLK E1DSP_BLOCK

static unsigned long long g_rng = 0x2468ACEULL;
static double urand(void)
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
static void gen_speechlike(float *x, int n, double fs)
{
    double ph[64] = {0};
    int i = 0;
    while (i < n) {
        int on = (int)((0.18 + 0.24 * urand()) * fs), off = (int)((0.12 + 0.18 * urand()) * fs);
        double base = 100.0 + 120.0 * urand();
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
static void gen_pink(float *x, int n)
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

static E1Nr *g_nr;
static void shadow_gain(void *user, float *const *spec, const float *const *frame, int nch)
{
    (void)user;
    (void)frame;
    for (int c = 0; c < nch; ++c)
        for (int k = 0; k < g_nr->K; ++k) {
            spec[c][2 * k] *= g_nr->Gout[k];
            spec[c][2 * k + 1] *= g_nr->Gout[k];
        }
}

static int g_fail = 0, g_pass = 0;

static void run_case(const char *name, int pink, double snr_db, float strength, double need_snri)
{
    const int n = (int)(12 * FS) / BLK * BLK;
    float *s = malloc(sizeof(float) * n), *v = malloc(sizeof(float) * n), *x = malloc(sizeof(float) * n);
    float *so = malloc(sizeof(float) * n), *vo = malloc(sizeof(float) * n);
    gen_speechlike(s, n, FS);
    if (pink)
        gen_pink(v, n);
    else
        for (int i = 0; i < n; ++i)
            v[i] = (float)grand();
    double es = 0, en = 0;
    for (int i = 0; i < n; ++i) {
        es += (double)s[i] * s[i];
        en += (double)v[i] * v[i];
    }
    const double g = sqrt(es / en / pow(10.0, snr_db / 10.0));
    for (int i = 0; i < n; ++i) {
        v[i] = (float)(g * v[i]);
        x[i] = s[i] + v[i];
    }
    memcpy(so, s, sizeof(float) * n);
    memcpy(vo, v, sizeof(float) * n);

    g_nr = e1_nr_create(FS, 1);
    e1_nr_set(g_nr, strength);
    E1Wola *ws = e1_wola_create(g_nr->nfft, g_nr->hop, 1, BLK), *wv = e1_wola_create(g_nr->nfft, g_nr->hop, 1, BLK);
    for (int i = 0; i < n; i += BLK) {
        float *cx[1] = {x + i}, *cs[1] = {so + i}, *cv[1] = {vo + i};
        e1_nr_process(g_nr, cx, 1, BLK);   /* computes this hop's gains (hop == block) */
        e1_wola_process(ws, cs, 1, BLK, shadow_gain, NULL);
        e1_wola_process(wv, cv, 1, BLK, shadow_gain, NULL);
    }
    const int L = (int)e1_nr_latency(g_nr), s0 = (int)(2.0 * FS);
    double S = 0, Si = 0, D = 0, N = 0, No = 0, Np = 0, Nop = 0;
    for (int i = s0; i + L < n; ++i) {
        const double d = so[i + L] - s[i];
        S += (double)so[i + L] * so[i + L];
        Si += (double)s[i] * s[i];
        D += d * d;
        N += (double)v[i] * v[i];
        No += (double)vo[i + L] * vo[i + L];
        if (s[i] == 0.0f) {
            Np += (double)v[i] * v[i];
            Nop += (double)vo[i + L] * vo[i + L];
        }
    }
    const double snr_in = 10 * log10(Si / N), snr_out = 10 * log10(S / No);
    const double snri = snr_out - snr_in;
    const int ok = snri >= need_snri && isfinite(snri);
    printf("  [%s] %-34s SNR %5.1f -> %5.1f dB  SNRI %+5.1f dB | noise att. %4.1f dB (pauses %4.1f) | speech SDR %4.1f dB\n",
           ok ? "PASS" : "FAIL", name, snr_in, snr_out, snri, 10 * log10(N / No), 10 * log10(Np / Nop),
           10 * log10(Si / D));
    if (ok)
        ++g_pass;
    else
        ++g_fail;
    e1_wola_destroy(ws);
    e1_wola_destroy(wv);
    e1_nr_destroy(g_nr);
    free(s);
    free(v);
    free(x);
    free(so);
    free(vo);
}

int main(void)
{
    printf("Noise reduction, white-box (shadow-filtered speech / noise components)\n");
    run_case("white 5 dB, strength 0.5", 0, 5.0, 0.5f, 6.0);
    run_case("pink  5 dB, strength 0.5", 1, 5.0, 0.5f, 6.0);
    run_case("white 15 dB, strength 0.5", 0, 15.0, 0.5f, 6.0);
    run_case("pink  15 dB, strength 0.5", 1, 15.0, 0.5f, 5.0);
    run_case("white 5 dB, strength 0.0", 0, 5.0, 0.0f, 3.0);
    run_case("white 5 dB, strength 1.0", 0, 5.0, 1.0f, 10.0);
    run_case("pink  0 dB, strength 1.0", 1, 0.0, 1.0f, 8.0);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
