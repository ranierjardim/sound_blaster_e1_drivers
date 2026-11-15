/*
 * e1_fft.c - real FFT and streaming WOLA framework for the voice modules (owner: dsp-voice).
 *
 * FFT: an n-point real transform is computed as an n/2-point complex radix-2 DIT FFT on the
 * even/odd-packed input followed by the usual split step. Twiddles are computed in double and
 * stored per stage so the butterfly loops read them contiguously.
 */
#include "e1_fft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define E1F_PI 3.14159265358979323846

struct E1Fft {
    int    n;      /* real length */
    int    m;      /* complex length n/2 */
    int   *rev;    /* bit-reversal permutation of m */
    float *twr;    /* per-stage twiddles, stage of length L starts at L/2 - 1 */
    float *twi;
    float *rwr;    /* split-step twiddles e^{-2 pi i k / n}, k = 0 .. m/2 */
    float *rwi;
};

static int is_pow2(int x) { return x > 0 && (x & (x - 1)) == 0; }

E1Fft *e1_fft_create(int n)
{
    if (!is_pow2(n) || n < 8 || n > 65536)
        return NULL;
    E1Fft *f = (E1Fft *)calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->n = n;
    f->m = n / 2;
    const int m = f->m;
    f->rev = (int *)malloc(sizeof(int) * (size_t)m);
    f->twr = (float *)malloc(sizeof(float) * (size_t)m);
    f->twi = (float *)malloc(sizeof(float) * (size_t)m);
    f->rwr = (float *)malloc(sizeof(float) * (size_t)(m / 2 + 1));
    f->rwi = (float *)malloc(sizeof(float) * (size_t)(m / 2 + 1));
    if (!f->rev || !f->twr || !f->twi || !f->rwr || !f->rwi) {
        e1_fft_destroy(f);
        return NULL;
    }
    int bits = 0;
    while ((1 << bits) < m)
        ++bits;
    for (int i = 0; i < m; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        f->rev[i] = r;
    }
    f->twr[0] = 1.0f; /* unused slot (stage L = 1) */
    f->twi[0] = 0.0f;
    for (int len = 2; len <= m; len <<= 1) {
        const int half = len >> 1;
        for (int j = 0; j < half; ++j) {
            double a = -2.0 * E1F_PI * (double)j / (double)len;
            f->twr[half - 1 + j] = (float)cos(a);
            f->twi[half - 1 + j] = (float)sin(a);
        }
    }
    for (int k = 0; k <= m / 2; ++k) {
        double a = -2.0 * E1F_PI * (double)k / (double)n;
        f->rwr[k] = (float)cos(a);
        f->rwi[k] = (float)sin(a);
    }
    return f;
}

void e1_fft_destroy(E1Fft *f)
{
    if (!f)
        return;
    free(f->rev);
    free(f->twr);
    free(f->twi);
    free(f->rwr);
    free(f->rwi);
    free(f);
}

int e1_fft_size(const E1Fft *f) { return f ? f->n : 0; }

/* In-place complex FFT of length m on interleaved data. sgn = -1 forward, +1 inverse (unscaled). */
static void cfft(const E1Fft *f, float *d, float sgn)
{
    const int m = f->m;
    const int *rev = f->rev;
    for (int i = 0; i < m; ++i) {
        int j = rev[i];
        if (i < j) {
            float tr = d[2 * i], ti = d[2 * i + 1];
            d[2 * i] = d[2 * j];
            d[2 * i + 1] = d[2 * j + 1];
            d[2 * j] = tr;
            d[2 * j + 1] = ti;
        }
    }
    for (int i = 0; i < m; i += 2) { /* length-2 stage: twiddle 1 */
        float *a = d + 2 * i;
        float br = a[2], bi = a[3];
        a[2] = a[0] - br;
        a[3] = a[1] - bi;
        a[0] += br;
        a[1] += bi;
    }
    const float s = -sgn; /* stored twiddles are the forward ones */
    for (int len = 4; len <= m; len <<= 1) {
        const int half = len >> 1;
        const float *wr = f->twr + half - 1;
        const float *wi = f->twi + half - 1;
        for (int i = 0; i < m; i += len) {
            float *a = d + 2 * i;
            float *b = d + 2 * (i + half);
            for (int j = 0; j < half; ++j) {
                const float c = wr[j], sn = s * wi[j];
                const float br = b[2 * j], bi = b[2 * j + 1];
                const float tr = br * c - bi * sn;
                const float ti = br * sn + bi * c;
                b[2 * j] = a[2 * j] - tr;
                b[2 * j + 1] = a[2 * j + 1] - ti;
                a[2 * j] += tr;
                a[2 * j + 1] += ti;
            }
        }
    }
}

void e1_fft_r2c(const E1Fft *f, const float *in, float *out)
{
    const int m = f->m, n = f->n;
    memcpy(out, in, sizeof(float) * (size_t)n);
    cfft(f, out, -1.0f);
    /* split step: X[k] = Fe + W^k Fo, X[m-k] = conj(Fe - W^k Fo) */
    const float z0r = out[0], z0i = out[1];
    out[0] = z0r + z0i;
    out[1] = 0.0f;
    out[n] = z0r - z0i;
    out[n + 1] = 0.0f;
    for (int k = 1; k <= m / 2; ++k) {
        const int j = m - k;
        const float ar = out[2 * k], ai = out[2 * k + 1];
        const float br = out[2 * j], bi = out[2 * j + 1];
        const float fer = 0.5f * (ar + br), fei = 0.5f * (ai - bi);
        /* D = Z[k] - conj(Z[m-k]); Fo = -0.5 i D */
        const float dr = ar - br, di = ai + bi;
        const float for_ = 0.5f * di, foi = -0.5f * dr;
        const float wr = f->rwr[k], wi = f->rwi[k];
        const float tr = wr * for_ - wi * foi;
        const float ti = wr * foi + wi * for_;
        out[2 * k] = fer + tr;
        out[2 * k + 1] = fei + ti;
        if (j != k) {
            out[2 * j] = fer - tr;
            out[2 * j + 1] = -(fei - ti);
        }
    }
}

void e1_fft_c2r(const E1Fft *f, const float *in, float *out)
{
    const int m = f->m, n = f->n;
    /* Recover Z[k] = Fe + i Fo with Fe = (X[k] + conj(X[m-k]))/2, Fo = conj(W^k) (X[k] - conj(X[m-k]))/2 */
    {
        const float x0 = in[0], xm = in[n];
        out[0] = 0.5f * (x0 + xm);
        out[1] = 0.5f * (x0 - xm);
    }
    for (int k = 1; k <= m / 2; ++k) {
        const int j = m - k;
        const float ar = in[2 * k], ai = in[2 * k + 1];
        const float br = in[2 * j], bi = in[2 * j + 1];
        const float fer = 0.5f * (ar + br), fei = 0.5f * (ai - bi);
        const float gr = 0.5f * (ar - br), gi = 0.5f * (ai + bi); /* W^k Fo */
        const float wr = f->rwr[k], wi = -f->rwi[k];             /* conj(W^k) */
        const float for_ = gr * wr - gi * wi;
        const float foi = gr * wi + gi * wr;
        /* Z[k] = Fe + i Fo */
        out[2 * k] = fer - foi;
        out[2 * k + 1] = fei + for_;
        if (j != k) {
            /* Z[m-k] = conj(Fe) + i conj(-Fo)... derived from the mirrored pair */
            out[2 * j] = fer + foi;
            out[2 * j + 1] = -fei + for_;
        }
    }
    cfft(f, out, +1.0f);
    const float sc = 1.0f / (float)m;
    for (int i = 0; i < n; ++i)
        out[i] *= sc;
}

/* ======================================================================================== */
/* WOLA                                                                                     */
/* ======================================================================================== */

#define E1W_MAX_CH 8

struct E1Wola {
    int    n, hop, nch, cap, pre, extra;
    int    infill;
    int    rd, wr, count;
    E1Fft *fft;
    float *wa, *ws, *tmp;
    float *in[E1W_MAX_CH];
    float *ola[E1W_MAX_CH];
    float *ring[E1W_MAX_CH];
    float *spec[E1W_MAX_CH];
    float  pnorm;
    void  *mem;
};

E1Wola *e1_wola_create(int nfft, int hop, int nch, int block_hint)
{
    if (!is_pow2(nfft) || nfft < 16 || !is_pow2(hop) || hop > nfft / 2 || nch < 1 || nch > E1W_MAX_CH)
        return NULL;
    E1Wola *w = (E1Wola *)calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->n = nfft;
    w->hop = hop;
    w->nch = nch;
    w->cap = 1;
    while (w->cap < 4 * hop)
        w->cap <<= 1;
    w->pre = (block_hint > 0 && hop > block_hint) ? hop - block_hint : 0;
    w->fft = e1_fft_create(nfft);
    const size_t per_ch = (size_t)nfft * 2 + (size_t)w->cap + (size_t)nfft + 2;
    const size_t total = (size_t)nfft * 3 + per_ch * (size_t)nch;
    w->mem = calloc(total, sizeof(float));
    if (!w->fft || !w->mem) {
        e1_wola_destroy(w);
        return NULL;
    }
    float *p = (float *)w->mem;
    w->wa = p; p += nfft;
    w->ws = p; p += nfft;
    w->tmp = p; p += nfft;
    for (int c = 0; c < nch; ++c) {
        w->in[c] = p; p += nfft;
        w->ola[c] = p; p += nfft;
        w->ring[c] = p; p += w->cap;
        w->spec[c] = p; p += nfft + 2;
    }
    /* periodic sqrt-Hann; sum over hops of wa*ws must be 1: sum of Hann = nfft / (2 hop) */
    double s2 = 0.0;
    const double synth = 2.0 * (double)hop / (double)nfft;
    for (int i = 0; i < nfft; ++i) {
        double v = sin(E1F_PI * (double)i / (double)nfft);
        w->wa[i] = (float)v;
        w->ws[i] = (float)(v * synth);
        s2 += v * v;
    }
    w->pnorm = (float)(1.0 / s2);
    e1_wola_reset(w);
    return w;
}

void e1_wola_destroy(E1Wola *w)
{
    if (!w)
        return;
    e1_fft_destroy(w->fft);
    free(w->mem);
    free(w);
}

void e1_wola_reset(E1Wola *w)
{
    if (!w)
        return;
    for (int c = 0; c < w->nch; ++c) {
        memset(w->in[c], 0, sizeof(float) * (size_t)w->n);
        memset(w->ola[c], 0, sizeof(float) * (size_t)w->n);
        memset(w->ring[c], 0, sizeof(float) * (size_t)w->cap);
        memset(w->spec[c], 0, sizeof(float) * (size_t)(w->n + 2));
    }
    w->infill = 0;
    w->extra = 0;
    w->rd = 0;
    w->count = w->pre; /* pre-filled zeros */
    w->wr = w->pre & (w->cap - 1);
}

unsigned e1_wola_latency(const E1Wola *w)
{
    return w ? (unsigned)(w->n - w->hop + w->pre + w->extra) : 0u;
}
int   e1_wola_nfft(const E1Wola *w) { return w ? w->n : 0; }
int   e1_wola_hop(const E1Wola *w) { return w ? w->hop : 0; }
float e1_wola_power_norm(const E1Wola *w) { return w ? w->pnorm : 0.0f; }

static void wola_frame(E1Wola *w, int nch, E1WolaFrameFn fn, void *user)
{
    const int n = w->n, hop = w->hop, mask = w->cap - 1;
    for (int c = 0; c < nch; ++c) {
        const float *x = w->in[c];
        for (int i = 0; i < n; ++i)
            w->tmp[i] = x[i] * w->wa[i];
        e1_fft_r2c(w->fft, w->tmp, w->spec[c]);
    }
    if (fn)
        fn(user, w->spec, (const float *const *)w->in, nch);
    for (int c = 0; c < nch; ++c) {
        e1_fft_c2r(w->fft, w->spec[c], w->tmp);
        float *o = w->ola[c];
        for (int i = 0; i < n; ++i)
            o[i] += w->tmp[i] * w->ws[i];
        float *r = w->ring[c];
        int wp = w->wr;
        for (int i = 0; i < hop; ++i) {
            r[wp] = o[i];
            wp = (wp + 1) & mask;
        }
        memmove(o, o + hop, sizeof(float) * (size_t)(n - hop));
        memset(o + n - hop, 0, sizeof(float) * (size_t)hop);
        memmove(w->in[c], w->in[c] + hop, sizeof(float) * (size_t)(n - hop));
    }
    w->wr = (w->wr + hop) & mask;
    w->count += hop;
}

void e1_wola_process(E1Wola *w, float *const *ch, int nch, int n, E1WolaFrameFn fn, void *user)
{
    if (!w || !ch || n <= 0)
        return;
    if (nch > w->nch)
        nch = w->nch;
    if (nch <= 0)
        return;
    const int hop = w->hop, mask = w->cap - 1;
    int i = 0;
    while (i < n) {
        int seg = hop - w->infill;
        if (seg > n - i)
            seg = n - i;
        /* 1) append input (sanitised: NaN -> 0, |x| <= 1e4 keeps every spectral power finite) */
        for (int c = 0; c < nch; ++c) {
            const float *src = ch[c] + i;
            float *dst = w->in[c] + (w->n - hop + w->infill);
            for (int k = 0; k < seg; ++k) {
                float x = src[k];
                if (!(x == x))
                    x = 0.0f;
                else if (x > 1e4f)
                    x = 1e4f;
                else if (x < -1e4f)
                    x = -1e4f;
                dst[k] = x;
            }
        }
        w->infill += seg;
        if (w->infill == hop) {
            wola_frame(w, nch, fn, user);
            w->infill = 0;
        }
        /* 2) emit; an underflow (call sizes not multiple of block_hint) delays the stream once */
        int take = seg < w->count ? seg : w->count;
        int miss = seg - take;
        for (int c = 0; c < nch; ++c) {
            float *dst = ch[c] + i;
            const float *r = w->ring[c];
            int rp = w->rd;
            for (int k = 0; k < miss; ++k)
                dst[k] = 0.0f;
            for (int k = 0; k < take; ++k) {
                dst[miss + k] = r[rp];
                rp = (rp + 1) & mask;
            }
        }
        w->rd = (w->rd + take) & mask;
        w->count -= take;
        w->extra += miss;
        i += seg;
    }
}
