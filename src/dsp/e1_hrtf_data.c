/*
 * e1_hrtf_data.c - analytic HRTF model + minimum-phase virtualizer filter-bank designer.
 * See e1_hrtf_data.h for the signal model. Everything here runs at create() time only.
 */
#include "e1_hrtf_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------- */
/* small complex helpers + radix-2 FFT (double)                                              */
/* ---------------------------------------------------------------------------------------- */
typedef struct Cx { double re, im; } Cx;

static Cx cx_make(double re, double im) { Cx z; z.re = re; z.im = im; return z; }
static Cx cx_mul(Cx a, Cx b) { return cx_make(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re); }
static Cx cx_add(Cx a, Cx b) { return cx_make(a.re + b.re, a.im + b.im); }
static Cx cx_sub(Cx a, Cx b) { return cx_make(a.re - b.re, a.im - b.im); }
static Cx cx_scale(Cx a, double s) { return cx_make(a.re * s, a.im * s); }
static Cx cx_expj(double ph) { return cx_make(cos(ph), sin(ph)); }
static double cx_abs2(Cx a) { return a.re * a.re + a.im * a.im; }

typedef struct Fft { int n; Cx *tw; } Fft;

static int fft_init(Fft *f, int n)
{
    f->n = n;
    f->tw = (Cx *)malloc(sizeof(Cx) * (size_t)(n / 2));
    if (!f->tw) return -1;
    for (int k = 0; k < n / 2; ++k) f->tw[k] = cx_expj(-2.0 * E1_PI * (double)k / (double)n);
    return 0;
}

static void fft_run(const Fft *f, Cx *a, int inverse)
{
    const int n = f->n;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { Cx t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; ++j) {
                Cx w = f->tw[j * step];
                if (inverse) w.im = -w.im;
                Cx u = a[i + j], v = cx_mul(a[i + j + half], w);
                a[i + j] = cx_add(u, v);
                a[i + j + half] = cx_sub(u, v);
            }
        }
    }
    if (inverse) {
        const double s = 1.0 / (double)n;
        for (int i = 0; i < n; ++i) a[i] = cx_scale(a[i], s);
    }
}

/* ---------------------------------------------------------------------------------------- */
/* analytic head model                                                                       */
/* ---------------------------------------------------------------------------------------- */
#define HEAD_RADIUS_M 0.0875
#define SOUND_SPEED   343.0
#define DEG2RAD       (E1_PI / 180.0)

/* Angle between the source direction (horizontal plane) and the ear axis, radians. */
static double ear_incidence(double az_deg, int ear)
{
    double c = sin(az_deg * DEG2RAD) * (double)ear;
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return acos(c);
}

double e1_hrtf_model_delay(double az_deg, int ear)
{
    const double th = ear_incidence(az_deg, ear), k = HEAD_RADIUS_M / SOUND_SPEED;
    return (th < 0.5 * E1_PI) ? -k * cos(th) : k * (th - 0.5 * E1_PI);
}

double e1_hrtf_model_mag(double f, double az_deg, int ear)
{
    if (f < 0.0) f = -f;
    const double th = ear_incidence(az_deg, ear);
    const double th_deg = th / DEG2RAD;

    /* Head shadow: one-pole/one-zero spherical-head filter, zero position depends on incidence. */
    const double amin = 0.1, thmin = 150.0;
    const double alpha = (1.0 + 0.5 * amin) + (1.0 - 0.5 * amin) * cos(th_deg / thmin * E1_PI);
    const double x = (2.0 * E1_PI * f) / (2.0 * SOUND_SPEED / HEAD_RADIUS_M);
    const double hs = sqrt((1.0 + alpha * alpha * x * x) / (1.0 + x * x));

    /* Pinna: five short echoes whose delays depend on azimuth (elevation 0).  The reflection
       coefficients sum to zero, so the pinna is transparent at low frequency; they are scaled
       down (0.3) so notches stay moderate. */
    static const double rho[5] = {0.5, -1.0, 0.5, -0.25, 0.25};
    static const double pa[5] = {1.0, 5.0, 5.0, 5.0, 5.0};
    static const double pb[5] = {2.0, 4.0, 7.0, 11.0, 13.0};
    static const double pd[5] = {1.0, 0.5, 0.5, 0.5, 0.5};
    const double ca = cos(0.5 * az_deg * DEG2RAD);
    double pr = 1.0, pi = 0.0;
    for (int k = 0; k < 5; ++k) {
        const double tau = (pa[k] * ca * sin(pd[k] * 0.5 * E1_PI) + pb[k]) / 44100.0;
        const double ph = -2.0 * E1_PI * f * tau;
        pr += 0.3 * rho[k] * cos(ph);
        pi += 0.3 * rho[k] * sin(ph);
    }
    const double pinna = sqrt(pr * pr + pi * pi);

    /* Torso/shoulder: one low-passed reflection, about 1.0..1.3 ms. */
    const double tt = 1.0e-3 + 0.3e-3 * th_deg / 180.0;
    const double rt = 0.12 / sqrt(1.0 + (f / 2500.0) * (f / 2500.0));
    const double tph = -2.0 * E1_PI * f * tt;
    const double torso = sqrt((1.0 + rt * cos(tph)) * (1.0 + rt * cos(tph)) + (rt * sin(tph)) * (rt * sin(tph)));

    /* Concha resonance (stronger on the ipsilateral side) and rear pinna shadow. */
    const double lf = log2((f > 1.0 ? f : 1.0) / 4300.0) / 0.4;
    const double concha_db = (1.5 + 1.5 * (1.0 + cos(th))) / (1.0 + lf * lf);
    double rear = -cos(az_deg * DEG2RAD);
    if (rear < 0.0) rear = 0.0;
    const double rear_db = -6.0 * rear * (f * f) / (f * f + 3500.0 * 3500.0);

    return hs * pinna * torso * pow(10.0, (concha_db + rear_db) / 20.0);
}

/* ---------------------------------------------------------------------------------------- */
/* designer                                                                                  */
/* ---------------------------------------------------------------------------------------- */
int e1_virt_taps(double fs)
{
    int t = (int)ceil(0.00145 * fs);
    t = (t + 3) & ~3;
    if (t < E1_VIRT_MIN_TAPS) t = E1_VIRT_MIN_TAPS;
    if (t > E1_VIRT_MAX_TAPS) t = E1_VIRT_MAX_TAPS;
    return t;
}

typedef struct Dz {
    double fs;
    int n, nh;           /* FFT size, n/2+1 */
    int taps;
    Fft fft;
    Cx *w1, *w2;         /* work spectra, n each */
    double *freq;        /* bin frequency */
    double *pinkw;       /* pink weight per bin (0 outside 20 Hz .. ftop) */
    double pinkw_sum;
    double *prefix;      /* scratch for smoothing (nh+1) */
    double *tmp;         /* scratch (nh) */
    double ftop;
} Dz;

static double *dz_vec(Dz *d) { return (double *)calloc((size_t)d->nh, sizeof(double)); }

/* Power-domain fractional-octave smoothing (in place allowed). */
static void smooth_pow(Dz *d, const double *p, double *out, double frac_oct)
{
    const int nh = d->nh;
    d->prefix[0] = 0.0;
    for (int k = 0; k < nh; ++k) d->prefix[k + 1] = d->prefix[k] + p[k];
    const double r = pow(2.0, 0.5 * frac_oct);
    for (int k = 0; k < nh; ++k) {
        int lo = (int)floor((double)k / r), hi = (int)ceil((double)k * r);
        if (lo < 0) lo = 0;
        if (hi > nh - 1) hi = nh - 1;
        if (hi < lo) hi = lo;
        d->tmp[k] = (d->prefix[hi + 1] - d->prefix[lo]) / (double)(hi - lo + 1);
    }
    memcpy(out, d->tmp, sizeof(double) * (size_t)nh);
}

static double pink_avg(const Dz *d, const double *p)
{
    double s = 0.0;
    for (int k = 0; k < d->nh; ++k) s += d->pinkw[k] * p[k];
    return s / d->pinkw_sum;
}

/* Minimum-phase spectrum (full n bins) from a magnitude given on nh bins (homomorphic method). */
static void minphase_spec(Dz *d, const double *mag, Cx *out)
{
    const int n = d->n, nh = d->nh;
    Cx *w = d->w2;
    for (int k = 0; k < n; ++k) {
        const int kk = (k < nh) ? k : n - k;
        double m = mag[kk];
        if (!(m > 1e-6)) m = 1e-6;
        w[k] = cx_make(log(m), 0.0);
    }
    fft_run(&d->fft, w, 1);                       /* real cepstrum */
    for (int k = 1; k < n / 2; ++k) w[k] = cx_make(2.0 * w[k].re, 0.0);
    w[0].im = 0.0;
    w[n / 2].im = 0.0;
    for (int k = n / 2 + 1; k < n; ++k) w[k] = cx_make(0.0, 0.0);
    fft_run(&d->fft, w, 0);
    for (int k = 0; k < n; ++k) out[k] = cx_scale(cx_expj(w[k].im), exp(w[k].re));
}

/* FIR (taps) from a power response raised to expo/2: minimum phase, truncated with a half-Hann tail. */
static void fir_from_pow(Dz *d, const double *pw, double expo, float *h)
{
    const int nh = d->nh, L = d->taps;
    double *mag = d->tmp + 0; /* tmp is reused by smooth_pow only, safe here */
    for (int k = 0; k < nh; ++k) mag[k] = pow(pw[k] > 1e-12 ? pw[k] : 1e-12, 0.5 * expo);
    minphase_spec(d, mag, d->w1);
    fft_run(&d->fft, d->w1, 1);
    const int fade = L / 4;
    for (int t = 0; t < L; ++t) {
        double v = d->w1[t].re;
        if (t >= L - fade) v *= 0.5 * (1.0 + cos(E1_PI * (double)(t - (L - fade) + 1) / (double)(fade + 1)));
        h[t] = (float)v;
    }
}

static void fir_spectrum(Dz *d, const float *h, Cx *out)
{
    for (int k = 0; k < d->n; ++k) out[k] = cx_make(k < d->taps ? (double)h[k] : 0.0, 0.0);
    fft_run(&d->fft, out, 0);
}

static double fir_pinkpow(Dz *d, const float *h)
{
    fir_spectrum(d, h, d->w1);
    double s = 0.0;
    for (int k = 0; k < d->nh; ++k) s += d->pinkw[k] * cx_abs2(d->w1[k]);
    return s / d->pinkw_sum;
}

static void fir_scale(float *h, int L, double s)
{
    for (int t = 0; t < L; ++t) h[t] = (float)((double)h[t] * s);
}

static void fir_delta(float *h, int L)
{
    memset(h, 0, sizeof(float) * (size_t)L);
    h[0] = 1.0f;
}

static Cx bq_resp(const E1BqCoef *c, double w)
{
    const Cx z1 = cx_expj(-w), z2 = cx_expj(-2.0 * w);
    const Cx num = cx_add(cx_make(c->b0, 0.0), cx_add(cx_scale(z1, c->b1), cx_scale(z2, c->b2)));
    const Cx den = cx_add(cx_make(1.0, 0.0), cx_add(cx_scale(z1, c->a1), cx_scale(z2, c->a2)));
    const double dd = cx_abs2(den);
    return cx_make((num.re * den.re + num.im * den.im) / dd, (num.im * den.re - num.re * den.im) / dd);
}

static Cx allpass_resp(double w, int delay)
{
    const Cx zd = cx_expj(-w * (double)delay);
    const Cx num = cx_add(cx_make(E1_AMB_AP_G, 0.0), zd);
    const Cx den = cx_add(cx_make(1.0, 0.0), cx_scale(zd, E1_AMB_AP_G));
    const double dd = cx_abs2(den);
    return cx_make((num.re * den.re + num.im * den.im) / dd, (num.im * den.re - num.re * den.im) / dd);
}

/* Ear power (pink weighted, averaged over both ears) produced by the ambience path for unit-power
   side input, given the four ambience FIRs.  Exactly mirrors the runtime signal flow. */
static double ambience_pow(Dz *d, const float *fss, const float *fsd, const float *fbs, const float *fbd)
{
    const int nh = d->nh;
    E1BqCoef hp, sh;
    e1_amb_filters(d->fs, &hp, &sh);
    const int pre = e1_amb_delay_samples(E1_AMB_PREDELAY_MS, d->fs);
    const int ap0 = e1_amb_delay_samples(E1_AMB_AP0_MS, d->fs), ap1 = e1_amb_delay_samples(E1_AMB_AP1_MS, d->fs);
    const int ap2 = e1_amb_delay_samples(E1_AMB_AP2_MS, d->fs), ap3 = e1_amb_delay_samples(E1_AMB_AP3_MS, d->fs);

    Cx *H = (Cx *)malloc(sizeof(Cx) * (size_t)nh * 4);
    if (!H) return 1.0;
    const float *fl[4] = {fss, fsd, fbs, fbd};
    for (int i = 0; i < 4; ++i) {
        fir_spectrum(d, fl[i], d->w1);
        memcpy(H + (size_t)i * nh, d->w1, sizeof(Cx) * (size_t)nh);
    }
    double acc = 0.0;
    for (int k = 0; k < nh; ++k) {
        if (d->pinkw[k] == 0.0) continue;
        const double w = 2.0 * E1_PI * d->freq[k] / d->fs;
        const Cx a = cx_mul(cx_mul(bq_resp(&hp, w), bq_resp(&sh, w)), cx_expj(-w * (double)pre));
        const Cx sl = cx_mul(allpass_resp(w, ap0), a);
        const Cx sr = cx_scale(cx_mul(allpass_resp(w, ap1), a), -1.0);
        const Cx bl = cx_scale(cx_mul(allpass_resp(w, ap2), a), E1_AMB_BACK_GAIN);
        const Cx br = cx_scale(cx_mul(allpass_resp(w, ap3), a), -E1_AMB_BACK_GAIN);
        const Cx x2 = cx_scale(cx_add(sl, sr), 0.5), x3 = cx_scale(cx_sub(sl, sr), 0.5);
        const Cx x4 = cx_scale(cx_add(bl, br), 0.5), x5 = cx_scale(cx_sub(bl, br), 0.5);
        const Cx es = cx_add(cx_mul(H[0 * (size_t)nh + k], x2), cx_mul(H[2 * (size_t)nh + k], x4));
        const Cx ed = cx_add(cx_mul(H[1 * (size_t)nh + k], x3), cx_mul(H[3 * (size_t)nh + k], x5));
        acc += d->pinkw[k] * 0.5 * (cx_abs2(cx_add(es, ed)) + cx_abs2(cx_sub(es, ed)));
    }
    free(H);
    return acc / d->pinkw_sum;
}

/* Sum/difference power responses of a symmetric virtual pair at +-az (diffuse-field equalised). */
static int pair_response(Dz *d, const double *dfeq, double az, double *sum2, double *dif2)
{
    const int n = d->n, nh = d->nh;
    double *mi = dz_vec(d), *mc = dz_vec(d);
    Cx *hi = (Cx *)malloc(sizeof(Cx) * (size_t)n);
    if (!mi || !mc || !hi) { free(mi); free(mc); free(hi); return -1; }
    for (int k = 0; k < nh; ++k) {
        const double f = d->freq[k] < d->ftop ? d->freq[k] : d->ftop;
        mi[k] = e1_hrtf_model_mag(f, az, +1) * dfeq[k];
        mc[k] = e1_hrtf_model_mag(f, az, -1) * dfeq[k];
    }
    minphase_spec(d, mi, hi);
    minphase_spec(d, mc, d->w1);
    const double itd = e1_hrtf_model_delay(az, -1) - e1_hrtf_model_delay(az, +1);
    for (int k = 0; k < nh; ++k) {
        const Cx c = cx_mul(d->w1[k], cx_expj(-2.0 * E1_PI * d->freq[k] * itd));
        sum2[k] = cx_abs2(cx_add(hi[k], c));
        dif2[k] = cx_abs2(cx_sub(hi[k], c));
    }
    free(mi); free(mc); free(hi);
    return 0;
}

/* Regularised power ratio x/y with cap, used for the loudspeaker (crosstalk) designs. */
static void pow_ratio(Dz *d, const double *x, const double *y, double *out, double lo_db, double hi_db)
{
    double ymax = 0.0;
    for (int k = 0; k < d->nh; ++k) if (y[k] > ymax) ymax = y[k];
    const double beta = 1e-4 * ymax + 1e-12;
    const double lo = pow(10.0, lo_db / 10.0), hi = pow(10.0, hi_db / 10.0);
    for (int k = 0; k < d->nh; ++k) {
        double r = x[k] * y[k] / (y[k] * y[k] + beta * beta);
        if (r < lo) r = lo;
        if (r > hi) r = hi;
        out[k] = r;
    }
}

static void normalise_pow(Dz *d, double *p)
{
    const double s = pink_avg(d, p);
    if (s > 0.0) for (int k = 0; k < d->nh; ++k) p[k] /= s;
}

int e1_virt_design(double fs, int taps, float *coef)
{
    if (!coef || !(fs >= 4000.0) || taps < E1_VIRT_MIN_TAPS || taps > E1_VIRT_MAX_TAPS) return -1;

    Dz dz;
    memset(&dz, 0, sizeof(dz));
    Dz *d = &dz;
    d->fs = fs;
    d->taps = taps;
    int n = 4096;
    while (n < 16 * taps) n <<= 1;
    d->n = n;
    d->nh = n / 2 + 1;
    d->ftop = (0.45 * fs < 20000.0) ? 0.45 * fs : 20000.0;

    int rc = -1;
    enum { V_DFEQ, V_S30, V_D30, V_S45, V_D45, V_S110, V_D110, V_S145, V_D145,
           V_FM, V_FS, V_A0, V_A1, V_A2, V_A3, V_T0, V_T1, V_COUNT };
    double *v[V_COUNT];
    memset(v, 0, sizeof(v));
    float *amb = NULL;

    if (fft_init(&d->fft, n) != 0) goto done;
    d->w1 = (Cx *)malloc(sizeof(Cx) * (size_t)n);
    d->w2 = (Cx *)malloc(sizeof(Cx) * (size_t)n);
    d->freq = dz_vec(d);
    d->pinkw = dz_vec(d);
    d->tmp = dz_vec(d);
    d->prefix = (double *)calloc((size_t)d->nh + 1, sizeof(double));
    amb = (float *)calloc((size_t)taps * 4, sizeof(float));
    if (!d->w1 || !d->w2 || !d->freq || !d->pinkw || !d->tmp || !d->prefix || !amb) goto done;
    for (int i = 0; i < V_COUNT; ++i) if (!(v[i] = dz_vec(d))) goto done;

    for (int k = 0; k < d->nh; ++k) {
        d->freq[k] = (double)k * fs / (double)n;
        d->pinkw[k] = (d->freq[k] >= 20.0 && d->freq[k] <= d->ftop) ? 1.0 / d->freq[k] : 0.0;
        d->pinkw_sum += d->pinkw[k];
    }
    if (!(d->pinkw_sum > 0.0)) goto done;

    /* Diffuse-field (horizontal-plane) equalisation: the virtualizer must not add the average
       head/pinna/concha colouration that real headphones are already tuned for. */
    for (int az = 0; az < 360; az += 5)
        for (int k = 0; k < d->nh; ++k) {
            const double f = d->freq[k] < d->ftop ? d->freq[k] : d->ftop;
            const double m = e1_hrtf_model_mag(f, (double)az, +1);
            v[V_DFEQ][k] += m * m / 72.0;
        }
    smooth_pow(d, v[V_DFEQ], v[V_DFEQ], 1.0 / 3.0);
    for (int k = 0; k < d->nh; ++k) v[V_DFEQ][k] = 1.0 / sqrt(v[V_DFEQ][k]);

    if (pair_response(d, v[V_DFEQ], E1_VIRT_FRONT_DEG, v[V_S30], v[V_D30]) ||
        pair_response(d, v[V_DFEQ], E1_VIRT_SPK_WIDE_DEG, v[V_S45], v[V_D45]) ||
        pair_response(d, v[V_DFEQ], E1_VIRT_SIDE_DEG, v[V_S110], v[V_D110]) ||
        pair_response(d, v[V_DFEQ], E1_VIRT_BACK_DEG, v[V_S145], v[V_D145]))
        goto done;

    for (int mode = 0; mode < 2; ++mode) {
        double *fm = v[V_FM], *fsd = v[V_FS];
        if (mode == 0) {
            /* Headphones.  Front mid path: one-octave power smoothing of the exact pair sum removes
               the phantom-centre comb (first notch ~1.9 kHz) while keeping the timbre. */
            smooth_pow(d, v[V_S30], fm, 1.0);
            smooth_pow(d, v[V_D30], fsd, 1.0 / 3.0);
            /* The side path vanishes at LF for a real head (ITD only); keep it within 6 dB of the
               mid path so low-frequency stereo separation survives. */
            for (int k = 0; k < d->nh; ++k) if (fsd[k] < 0.25 * fm[k]) fsd[k] = 0.25 * fm[k];
            smooth_pow(d, v[V_S110], v[V_A0], 1.0 / 3.0);
            smooth_pow(d, v[V_D110], v[V_A1], 1.0 / 3.0);
            smooth_pow(d, v[V_S145], v[V_A2], 1.0 / 3.0);
            smooth_pow(d, v[V_D145], v[V_A3], 1.0 / 3.0);
            for (int k = 0; k < d->nh; ++k) {
                if (v[V_A1][k] < 0.1 * v[V_A0][k]) v[V_A1][k] = 0.1 * v[V_A0][k];
                if (v[V_A3][k] < 0.1 * v[V_A2][k]) v[V_A3][k] = 0.1 * v[V_A2][k];
            }
        } else {
            /* Loudspeakers at +-30 deg: shuffler crosstalk designs = target pair / real pair in the
               sum and difference domains, regularised, capped and smoothed (spectral compensation).
               The mid path stays an exact identity (perfect mono compatibility). */
            double *ss = v[V_T0], *sd = v[V_T1];
            smooth_pow(d, v[V_S30], ss, 1.0 / 6.0);
            smooth_pow(d, v[V_D30], sd, 1.0 / 6.0);
            smooth_pow(d, v[V_D45], fsd, 1.0 / 6.0);
            pow_ratio(d, fsd, sd, fsd, -10.0, 10.0);
            for (int k = 0; k < d->nh; ++k) {
                const double q = d->freq[k] / 150.0, lfw = 1.0 / (1.0 + q * q * q * q);
                fsd[k] = fsd[k] * (1.0 - lfw) + lfw;
            }
            smooth_pow(d, fsd, fsd, 1.0 / 3.0);
            smooth_pow(d, v[V_S110], v[V_A0], 1.0 / 6.0);
            smooth_pow(d, v[V_D110], v[V_A1], 1.0 / 6.0);
            smooth_pow(d, v[V_S145], v[V_A2], 1.0 / 6.0);
            smooth_pow(d, v[V_D145], v[V_A3], 1.0 / 6.0);
            pow_ratio(d, v[V_A0], ss, v[V_A0], -12.0, 10.0);
            pow_ratio(d, v[V_A1], sd, v[V_A1], -12.0, 10.0);
            pow_ratio(d, v[V_A2], ss, v[V_A2], -12.0, 10.0);
            pow_ratio(d, v[V_A3], sd, v[V_A3], -12.0, 10.0);
            for (int i = V_A0; i <= V_A3; ++i) smooth_pow(d, v[i], v[i], 1.0 / 3.0);
            for (int k = 0; k < d->nh; ++k) fm[k] = 1.0;
        }
        normalise_pow(d, fm);
        normalise_pow(d, fsd);

        /* Unit-energy ambience FIRs (full rendering; the immersion level scales their gain). */
        for (int i = 0; i < 4; ++i) fir_from_pow(d, v[V_A0 + i], 1.0, amb + (size_t)i * taps);
        const double pa = ambience_pow(d, amb, amb + taps, amb + 2 * (size_t)taps, amb + 3 * (size_t)taps);
        if (pa > 0.0) fir_scale(amb, 4 * taps, 1.0 / sqrt(pa));

        for (int j = 0; j < E1_VIRT_LEVELS; ++j) {
            float *fl[E1_VIRT_NFILT];
            for (int f = 0; f < E1_VIRT_NFILT; ++f) fl[f] = coef + e1_virt_index(mode, j, f, taps);
            const double lev = (double)j / (double)(E1_VIRT_LEVELS - 1);
            if (j == 0) {
                fir_delta(fl[E1_VF_FRONT_SUM], taps);
                fir_delta(fl[E1_VF_FRONT_DIF], taps);
                for (int f = E1_VF_SIDE_SUM; f < E1_VIRT_NFILT; ++f) memset(fl[f], 0, sizeof(float) * (size_t)taps);
                continue;
            }
            const double expo = E1_VIRT_WET_SCALE * lev;
            const double amb_frac = E1_VIRT_AMB_MAX_FRACTION * lev;
            if (mode == 0) {
                fir_from_pow(d, fm, expo, fl[E1_VF_FRONT_SUM]);
                fir_scale(fl[E1_VF_FRONT_SUM], taps, 1.0 / sqrt(fir_pinkpow(d, fl[E1_VF_FRONT_SUM])));
            } else {
                fir_delta(fl[E1_VF_FRONT_SUM], taps);
            }
            fir_from_pow(d, fsd, expo, fl[E1_VF_FRONT_DIF]);
            fir_scale(fl[E1_VF_FRONT_DIF], taps, sqrt((1.0 - amb_frac) / fir_pinkpow(d, fl[E1_VF_FRONT_DIF])));
            for (int i = 0; i < 4; ++i) {
                memcpy(fl[E1_VF_SIDE_SUM + i], amb + (size_t)i * taps, sizeof(float) * (size_t)taps);
                fir_scale(fl[E1_VF_SIDE_SUM + i], taps, sqrt(amb_frac));
            }
        }
    }
    rc = 0;

done:
    for (int i = 0; i < V_COUNT; ++i) free(v[i]);
    free(amb);
    free(d->fft.tw);
    free(d->w1); free(d->w2); free(d->freq); free(d->pinkw); free(d->tmp); free(d->prefix);
    return rc;
}
