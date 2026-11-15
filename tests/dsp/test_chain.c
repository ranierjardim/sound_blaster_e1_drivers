/*
 * test_chain.c - render/capture chain: bit-exact bypass for any block size, click-free
 * transitions, return to bit-exact after fade-out, latency, silent tails, NaN robustness.
 */
#include "test_common.h"

static E1RenderParams render_defaults(void)
{
    E1Params p;
    e1fx_params_default(&p);
    return p.render;
}
static E1CaptureParams capture_defaults(void)
{
    E1Params p;
    e1fx_params_default(&p);
    return p.capture;
}

static E1RenderParams render_all_on(void)
{
    E1RenderParams p = render_defaults();
    p.master_on = 1;
    p.surround_on = 1; p.crystal_on = 1; p.bass_on = 1; p.svm_on = 1; p.dialog_on = 1;
    p.eq_on = 1;
    memcpy(p.eq_gain_db, e1_eq_presets[8].gain_db, sizeof p.eq_gain_db); /* Rock */
    p.eq_preamp_db = -1.f;
    p.output_gain_db = 2.f;
    p.limiter_on = 1;
    return p;
}

/* Fills a buffer with random finite floats incl. extreme magnitudes, signed zeros, subnormals. */
static void fill_random(float *x, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        uint32_t b = (uint32_t)rng_u64();
        int kind = rng_int(0, 9);
        if (kind == 0) {
            b &= 0x807FFFFFu;                     /* subnormal or zero */
        } else if (kind == 1) {
            b = (b & 0x80000000u) | 0x7F7FFFFFu;  /* +-FLT_MAX */
        } else if (kind <= 3) {
            if ((b & 0x7F800000u) == 0x7F800000u) b &= 0xBF7FFFFFu; /* any finite bit pattern */
        } else {
            float f = rng_sym();
            memcpy(&b, &f, 4);
        }
        memcpy(&x[i], &b, 4);
    }
}

typedef void (*ProcFn)(void *h, const float *in, float *out, size_t n);
static void rproc(void *h, const float *in, float *out, size_t n) { e1_render_process((E1Render *)h, in, out, n); }
static void cproc(void *h, const float *in, float *out, size_t n) { e1_capture_process((E1Capture *)h, in, out, n); }

/* Processes x (frames*nch) with a block-size pattern; in-place or out-of-place. */
static void run_pattern(void *h, ProcFn fn, const float *in, float *out, size_t frames, int nch,
                        const size_t *pat, int npat, int inplace)
{
    size_t off = 0;
    int k = 0;
    if (inplace) memcpy(out, in, frames * (size_t)nch * sizeof(float));
    while (off < frames) {
        size_t n = pat[k++ % npat];
        if (n > frames - off) n = frames - off;
        const float *ip = inplace ? out + off * nch : in + off * nch;
        fn(h, ip, out + off * nch, n);
        off += n;
    }
}

static void test_bypass_exact(void)
{
    static const size_t sizes[] = { 1, 7, 128, 441, 480, 4096 };
    static const int chans[] = { 1, 2, 6, 8 };
    const size_t frames = 44100;
    size_t ragged[64];
    rng_seed(2024);
    for (int i = 0; i < 64; ++i) ragged[i] = (size_t)rng_int(0, 1500);
    int nconf = 0, fails = 0;
    for (size_t ci = 0; ci < sizeof chans / sizeof chans[0]; ++ci) {
        int nch = chans[ci];
        float *in = (float *)malloc(frames * nch * sizeof(float)), *out = (float *)malloc(frames * nch * sizeof(float));
        fill_random(in, frames * nch);
        for (int variant = 0; variant < 3; ++variant) {
            E1Render *r = e1_render_create(44100, nch);
            E1Capture *c = e1_capture_create(44100, nch);
            if (variant == 1) {
                /* every sub-effect on but master and EQ off: still bit-exact */
                E1RenderParams p = render_all_on();
                p.master_on = 0; p.eq_on = 0; p.scout_on = 0;
                e1_render_set_params(r, &p);
                E1CaptureParams q = capture_defaults();
                q.nr_on = q.svm_on = q.vfx_on = 1; q.output_gain_db = 6.f; q.master_on = 0;
                e1_capture_set_params(c, &q);
            } else if (variant == 2) {
                /* master on with nothing enabled: nothing to do, stays bypassed */
                E1RenderParams p = render_defaults();
                p.master_on = 1; p.surround_on = p.crystal_on = 0;
                e1_render_set_params(r, &p);
            }
            for (int pass = 0; pass < (int)(sizeof sizes / sizeof sizes[0]) + 1; ++pass)
                for (int inplace = 0; inplace < 2; ++inplace) {
                    const size_t *pat = pass < 6 ? &sizes[pass] : ragged;
                    int npat = pass < 6 ? 1 : 64;
                    memset(out, 0xA5, frames * nch * sizeof(float));
                    run_pattern(r, rproc, in, out, frames, nch, pat, npat, inplace);
                    ++nconf;
                    if (memcmp(in, out, frames * nch * sizeof(float)) != 0 || !e1_render_is_bypassed(r)) ++fails;
                    memset(out, 0xA5, frames * nch * sizeof(float));
                    run_pattern(c, cproc, in, out, frames, nch, pat, npat, inplace);
                    ++nconf;
                    if (memcmp(in, out, frames * nch * sizeof(float)) != 0 || !e1_capture_is_bypassed(c)) ++fails;
                }
            e1_render_destroy(r);
            e1_capture_destroy(c);
        }
        free(in); free(out);
    }
    CHECK(fails == 0, "bypass not bit-exact in %d of %d configurations", fails, nconf);
    printf("  bypass bit-exact: %d configurations (1/7/128/441/480/4096/ragged, in/out-of-place, 1/2/6/8 ch)\n", nconf);

    /* non-finite input in bypass: replaced by 0, everything else untouched */
    E1Render *r = e1_render_create(48000, 2);
    float in[8] = { 0.5f, -0.25f, NAN, 1.0f, INFINITY, -INFINITY, 1e-40f, -0.0f }, out[8];
    e1_render_process(r, in, out, 4);
    CHECK(out[0] == 0.5f && out[1] == -0.25f && out[2] == 0.f && out[3] == 1.0f && out[4] == 0.f && out[5] == 0.f &&
          memcmp(&out[6], &in[6], 8) == 0, "bypass non-finite handling");
    e1_render_destroy(r);
}

static size_t settle_until_bypassed(E1Render *r, float *buf, size_t chunk, int nch, size_t maxframes)
{
    size_t done = 0;
    while (!e1_render_is_bypassed(r) && done < maxframes) {
        for (size_t i = 0; i < chunk * nch; ++i) buf[i] = rng_sym() * 0.5f;
        e1_render_process(r, buf, buf, chunk);
        done += chunk;
    }
    return done;
}

static void test_return_to_bypass(void)
{
    static const size_t chunks[] = { 1, 7, 441, 480, 4096 };
    const int nch = 2;
    float *in = (float *)malloc(8192 * nch * sizeof(float)), *out = (float *)malloc(8192 * nch * sizeof(float));
    int fails = 0;
    for (size_t k = 0; k < 5; ++k) {
        E1Render *r = e1_render_create(44100, nch);
        E1RenderParams p = render_all_on();
        e1_render_set_params(r, &p);
        float *tmp = (float *)malloc(chunks[k] * nch * sizeof(float));
        size_t done = 0;
        while (done < 22050) {
            for (size_t i = 0; i < chunks[k] * nch; ++i) tmp[i] = rng_sym() * 0.5f;
            e1_render_process(r, tmp, tmp, chunks[k]);
            done += chunks[k];
        }
        CHECK(!e1_render_is_bypassed(r), "chain should be active");
        p.master_on = 0; p.eq_on = 0;
        e1_render_set_params(r, &p);
        size_t t = settle_until_bypassed(r, tmp, chunks[k], nch, 44100);
        size_t lim = (size_t)(44100 * E1DSP_XFADE_MS / 1000.0) + chunks[k] + 1;
        CHECK(t <= lim, "chunk %zu: fade-out took %zu frames (> %zu)", chunks[k], t, lim);
        fill_random(in, 8192 * nch);
        e1_render_process(r, in, out, 8192);
        if (memcmp(in, out, 8192 * nch * sizeof(float)) != 0) ++fails;
        free(tmp);
        e1_render_destroy(r);
    }
    CHECK(fails == 0, "output not bit-identical after crossfade-out (%d cases)", fails);

    /* fade-out finishing in the middle of a buffer: the remainder of that buffer is exact */
    E1Render *r = e1_render_create(44100, 2);
    E1RenderParams p = render_defaults();
    p.eq_on = 1; p.eq_gain_db[5] = 6.f;
    e1_render_set_params(r, &p);
    for (int i = 0; i < 100; ++i) { fill_random(in, 441 * 2); for (int j = 0; j < 882; ++j) in[j] = rng_sym(); e1_render_process(r, in, out, 441); }
    p.eq_on = 0;
    e1_render_set_params(r, &p);
    for (int j = 0; j < 4096 * 2; ++j) in[j] = rng_sym();
    e1_render_process(r, in, out, 4096);
    size_t xf = (size_t)ceil(44100 * E1DSP_XFADE_MS / 1000.0);
    CHECK(e1_render_is_bypassed(r), "bypassed after one 4096-frame buffer");
    CHECK(memcmp(in + (xf + 1) * 2, out + (xf + 1) * 2, (4096 - xf - 1) * 2 * sizeof(float)) == 0,
          "remainder of the buffer after the fade-out is not bit-exact");
    e1_render_destroy(r);
    free(in); free(out);
}

/* Toggles effects at random buffer boundaries on a sine; checks the max sample step. */
static void test_clicks(double fs)
{
    const double f = 1000.0, A = 0.4;
    const size_t frames = (size_t)(fs * 6);
    const int nch = 2;
    float *x = (float *)malloc(frames * nch * sizeof(float));
    for (size_t i = 0; i < frames; ++i) x[2 * i] = x[2 * i + 1] = (float)(A * sin(2 * E1_PI * f * (double)i / fs));
    E1Render *r = e1_render_create(fs, nch);
    E1RenderParams p = render_defaults();
    p.eq_gain_db[5] = 6.f;          /* 1 kHz band +6 dB => wet amplitude 0.8 */
    p.bass_level = 80.f;
    p.output_gain_db = -3.f;
    size_t off = 0;
    int toggles = 0;
    rng_seed(77);
    while (off < frames) {
        size_t n = (size_t)rng_int(1, 900);
        if (n > frames - off) n = frames - off;
        if (rng_unit() < 0.15) {
            switch (rng_int(0, 4)) {
            case 0: p.eq_on ^= 1u; break;
            case 1: p.master_on ^= 1u; break;
            case 2: p.bass_on ^= 1u; break;
            case 3: p.limiter_on ^= 1u; break;
            default: p.output_gain_db = p.output_gain_db < -1.f ? 0.f : -3.f; break;
            }
            p.surround_on = p.crystal_on = 0;
            e1_render_set_params(r, &p);
            ++toggles;
        }
        e1_render_process(r, x + off * nch, x + off * nch, n);
        off += n;
    }
    double maxd = 0;
    size_t at = 0;
    for (size_t i = 1; i < frames; ++i) {
        double d = fabs((double)x[2 * i] - x[2 * i - 2]);
        if (d > maxd) { maxd = d; at = i; }
    }
    /* steepest legitimate slope: amplitude <= 0.8 (EQ +6 dB), +15 % for the EQ's start-up
       transient when its slot fades in from reset */
    double bound = 2 * sin(E1_PI * f / fs) * 0.8 * 1.15 + 0.005;
    CHECK(maxd <= bound, "fs %g: max sample step %.4f at %zu > %.4f (click)", fs, maxd, at, bound);
    printf("  fs %.0f: %d random toggles, max sample step %.4f (bound %.4f, a latency jump would be ~%.2f)\n",
           fs, toggles, maxd, bound, 2 * A);
    e1_render_destroy(r);
    free(x);
}

static long impulse_delay(void *h, ProcFn fn, int nch, size_t chunk, double *peak)
{
    const size_t frames = 8192, pos = 1000;
    float *x = (float *)calloc(frames * nch, sizeof(float));
    for (int c = 0; c < nch; ++c) x[pos * nch + c] = 0.5f;
    for (size_t off = 0; off < frames; off += chunk) {
        size_t n = chunk < frames - off ? chunk : frames - off;
        fn(h, x + off * nch, x + off * nch, n);
    }
    long best = -1;
    double pk = 0;
    for (size_t i = 0; i < frames; ++i)
        if (fabs(x[i * nch]) > pk) { pk = fabs(x[i * nch]); best = (long)i; }
    *peak = pk;
    free(x);
    return best < 0 ? -1 : best - (long)pos;
}

static void test_latency(void)
{
    for (int nch = 1; nch <= 6; nch += 5) {
        static const size_t chunks[] = { 1, 100, 441, 1024 };
        for (int k = 0; k < 4; ++k) {
            E1Render *r = e1_render_create(44100, nch);
            unsigned L = e1_render_latency_frames(r);
            E1RenderParams p = render_defaults();
            p.master_on = 1; p.surround_on = p.crystal_on = 0;
            p.output_gain_db = -0.5f;   /* active path, pure delay */
            e1_render_set_params(r, &p);
            /* run the fade-in out of the way first */
            float tmp[2048 * 6] = { 0 };
            e1_render_process(r, tmp, tmp, 2048);
            double pk;
            long d = impulse_delay(r, rproc, nch, chunks[k], &pk);
            CHECK(d == (long)L, "render %dch chunk %zu: measured delay %ld, reported %u", nch, chunks[k], d, L);
            CHECK(fabs(pk - 0.5 * pow(10, -0.5 / 20)) < 1e-6, "render impulse amplitude %g", pk);
            /* limiter slot on (gain-adding trim) must not change the delay */
            p.output_gain_db = 0.5f;
            e1_render_set_params(r, &p);
            e1_render_process(r, tmp, tmp, 2048);
            d = impulse_delay(r, rproc, nch, chunks[k], &pk);
            CHECK(d == (long)L, "render %dch with limiter: measured delay %ld, reported %u", nch, d, L);
            if (k == 0) printf("  render %d ch: latency %u frames (%.2f ms) = measured impulse delay\n", nch, L, L / 44.1);
            e1_render_destroy(r);

            E1Capture *c = e1_capture_create(44100, nch);
            unsigned Lc = e1_capture_latency_frames(c);
            E1CaptureParams q = capture_defaults();
            q.master_on = 1; q.nr_on = 0; q.output_gain_db = -1.f;
            e1_capture_set_params(c, &q);
            e1_capture_process(c, tmp, tmp, 2048);
            d = impulse_delay(c, cproc, nch, chunks[k], &pk);
            CHECK(d == (long)Lc, "capture %dch chunk %zu: measured delay %ld, reported %u", nch, chunks[k], d, Lc);
            if (k == 0) printf("  capture %d ch: latency %u frames (%.2f ms) = measured impulse delay\n", nch, Lc, Lc / 44.1);
            e1_capture_destroy(c);
        }
    }
}

static void test_tail_and_robustness(double fs)
{
    const int nch = 2;
    E1Render *r = e1_render_create(fs, nch);
    E1RenderParams p = render_all_on();
    e1_render_set_params(r, &p);
    const size_t chunk = 480;
    float buf[480 * 2];
    int finite = 1, sub = 0, nz = 0, alive = 0;
    rng_seed(8);
    for (size_t off = 0; off < (size_t)(fs * 2); off += chunk) {
        for (size_t i = 0; i < chunk * nch; ++i) buf[i] = 0.7f * rng_sym();
        if (off > (size_t)fs && off < (size_t)fs + 4 * chunk) { buf[3] = NAN; buf[10] = INFINITY; buf[77] = -INFINITY; buf[100] = 1e30f; }
        e1_render_process(r, buf, buf, chunk);
        for (size_t i = 0; i < chunk * nch; ++i) {
            finite &= isfinite(buf[i]);
            if (off > (size_t)(fs * 1.5)) alive |= fabsf(buf[i]) > 0.01f;
        }
    }
    CHECK(finite, "fs %g: non-finite output with NaN/Inf input", fs);
    CHECK(alive, "fs %g: chain dead after NaN/Inf input", fs);
    const size_t tail = (size_t)(fs * 3);
    for (size_t off = 0; off < tail; off += chunk) {
        memset(buf, 0, sizeof buf);
        e1_render_process(r, buf, buf, chunk);
        for (size_t i = 0; i < chunk * nch; ++i) {
            sub += is_subnormal_f(buf[i]);
            if (off >= (size_t)(fs * 2)) nz += buf[i] != 0.f;
        }
    }
    CHECK(sub == 0, "fs %g: %d subnormal output samples", fs, sub);
    CHECK(nz == 0, "fs %g: render output not exactly zero 2 s after silence (%d samples)", fs, nz);
    float pi[2], po[2], gr;
    e1_render_meter(r, pi, po, &gr);
    CHECK(gr <= 0.f && isfinite(pi[0]) && isfinite(po[0]), "meter values: in %g out %g gr %g", pi[0], po[0], gr);
    e1_render_destroy(r);

    E1Capture *c = e1_capture_create(fs, 1);
    E1CaptureParams q = capture_defaults();
    q.master_on = 1; q.svm_on = 1; q.vfx_on = 1; q.output_gain_db = 6.f;
    e1_capture_set_params(c, &q);
    nz = 0; finite = 1;
    for (size_t off = 0; off < (size_t)(fs * 1); off += chunk) {
        for (size_t i = 0; i < chunk; ++i) buf[i] = 0.5f * rng_sym();
        e1_capture_process(c, buf, buf, chunk);
        for (size_t i = 0; i < chunk; ++i) finite &= isfinite(buf[i]) && fabsf(buf[i]) <= 1.0f;
    }
    for (size_t off = 0; off < tail; off += chunk) {
        memset(buf, 0, sizeof buf);
        e1_capture_process(c, buf, buf, chunk);
        for (size_t i = 0; i < chunk; ++i) if (off >= (size_t)(fs * 2)) nz += buf[i] != 0.f;
    }
    CHECK(finite, "fs %g: capture output non-finite or above full scale with +6 dB gain (limiter)", fs);
    CHECK(nz == 0, "fs %g: capture output not exactly zero 2 s after silence", fs);
    e1_capture_destroy(c);
}

static void test_scout_and_reset(void)
{
    E1Render *r = e1_render_create(44100, 2);
    E1RenderParams p = render_defaults();
    p.scout_on = 1;  /* Scout alone (master and EQ off) activates EQ curve + surround */
    e1_render_set_params(r, &p);
    float buf[441 * 2];
    for (int i = 0; i < 441 * 2; ++i) buf[i] = 0.1f * rng_sym();
    e1_render_process(r, buf, buf, 441);
    CHECK(!e1_render_is_bypassed(r), "scout mode should activate the chain");
    e1_render_reset(r);
    CHECK(!e1_render_is_bypassed(r), "reset keeps the active state");
    p.scout_on = 0;
    e1_render_set_params(r, &p);
    e1_render_reset(r);
    CHECK(e1_render_is_bypassed(r), "reset with inactive params is bypassed immediately");
    e1_render_destroy(r);
    CHECK(e1_render_create(1000, 2) == NULL && e1_render_create(44100, 0) == NULL && e1_render_create(44100, 9) == NULL,
          "invalid formats rejected");
}

/* The safety limiter must hold its ceiling while modules fade in. Start from an active chain
   that adds no gain (trim -6 dB), then switch every gain-adding effect on while the input is hot
   white noise with +-1.0 spikes: every output sample must stay at or below -0.3 dBFS. */
static void test_ceiling_during_fades(double fs)
{
    const size_t n = 441;
    const float ceil_lin = (float)pow(10.0, (-0.3 + 0.05) / 20.0);
    float pk = 0.f;
    float buf[441 * 2];
    E1Render *r = e1_render_create(fs, 2);
    E1RenderParams p = render_defaults();
    p.master_on = 1;
    p.output_gain_db = -6.f;
    e1_render_set_params(r, &p);
    for (int k = 0; k < 50; ++k) {
        for (size_t i = 0; i < n * 2; ++i) buf[i] = 0.25f * rng_sym();
        e1_render_process(r, buf, buf, n);
    }
    for (int round = 0; round < 32; ++round) {
        p = render_all_on();
        p.scout_on = (uint32_t)(round & 1);
        if (round & 2) { for (int b = 0; b < E1FX_EQ_BANDS; ++b) p.eq_gain_db[b] = (b & 1) ? 6.f : -6.f; p.output_gain_db = 0.f; }
        e1_render_set_params(r, &p);
        for (int k = 0; k < 30; ++k) {
            for (size_t i = 0; i < n * 2; ++i) buf[i] = 0.9f * rng_sym();
            buf[3] = 1.f; buf[4] = -1.f;
            e1_render_process(r, buf, buf, n);
            for (size_t i = 0; i < n * 2; ++i) if (fabsf(buf[i]) > pk) pk = fabsf(buf[i]);
        }
        p = render_defaults();
        p.master_on = 1;
        p.output_gain_db = -6.f;
        e1_render_set_params(r, &p);
        for (int k = 0; k < 10; ++k) {
            for (size_t i = 0; i < n * 2; ++i) buf[i] = 0.9f * rng_sym();
            e1_render_process(r, buf, buf, n);
            for (size_t i = 0; i < n * 2; ++i) if (fabsf(buf[i]) > pk) pk = fabsf(buf[i]);
        }
    }
    printf("  fs %g: peak through module fades %.4f (ceiling %.4f)\n", fs, pk, ceil_lin);
    CHECK(pk <= ceil_lin, "fs %g: limiter ceiling exceeded during module fades: %.4f", fs, pk);
    e1_render_destroy(r);
}

/* ---- toggle smoothness: second difference at random toggle times vs steady state ------------- */

typedef struct ToggleCase {
    const char *name;
    int capture;
    int nch;
    E1RenderParams r0, r1;
    E1CaptureParams c0, c1;
} ToggleCase;

static void tc_set(void *h, const ToggleCase *t, int on)
{
    if (t->capture) e1_capture_set_params((E1Capture *)h, on ? &t->c1 : &t->c0);
    else e1_render_set_params((E1Render *)h, on ? &t->r1 : &t->r0);
}

static void tc_signal(float *x, size_t frames, int nch, double fs)
{
    for (size_t i = 0; i < frames; ++i) {
        const double t = (double)i / fs;
        if (nch == 1) {
            x[i] = (float)(0.2 * sin(2 * E1_PI * 220.0 * t) + 0.2 * sin(2 * E1_PI * 660.0 * t));
        } else {
            x[2 * i] = (float)(0.25 * sin(2 * E1_PI * 440.0 * t));
            x[2 * i + 1] = (float)(0.25 * sin(2 * E1_PI * 440.0 * t + 2.0));   /* mid + side content */
        }
    }
}

/* max |x[i+1] - 2x[i] + x[i-1]| over frames [a, b) of every channel */
static double max_d2(const float *y, size_t a, size_t b, int nch)
{
    double m = 0;
    for (size_t i = a + 1; i + 1 < b; ++i)
        for (int c = 0; c < nch; ++c) {
            const double d = fabs((double)y[(i + 1) * nch + c] - 2.0 * y[i * nch + c] + (double)y[(i - 1) * nch + c]);
            if (d > m) m = d;
        }
    return m;
}

/* Runs `frames` of the test signal in 441-frame buffers; toggles at the given buffer indices
   (on at even list entries, off at odd). Returns the output (caller frees). */
static float *tc_run(const ToggleCase *t, double fs, size_t frames, const int *tog, int ntog, int start_on)
{
    const size_t B = 441;
    float *x = (float *)malloc(frames * t->nch * sizeof(float));
    tc_signal(x, frames, t->nch, fs);
    void *h = t->capture ? (void *)e1_capture_create(fs, t->nch) : (void *)e1_render_create(fs, t->nch);
    int on = start_on, ti = 0;
    tc_set(h, t, on);
    if (t->capture) e1_capture_reset((E1Capture *)h); else e1_render_reset((E1Render *)h);
    for (size_t b = 0; b * B < frames; ++b) {
        if (ti < ntog && tog[ti] == (int)b) { on = !on; tc_set(h, t, on); ++ti; }
        const size_t n = frames - b * B < B ? frames - b * B : B;
        float *p = x + b * B * t->nch;
        if (t->capture) e1_capture_process((E1Capture *)h, p, p, n); else e1_render_process((E1Render *)h, p, p, n);
    }
    if (t->capture) e1_capture_destroy((E1Capture *)h); else e1_render_destroy((E1Render *)h);
    return x;
}

static void test_toggle_smoothness(double fs)
{
    ToggleCase tc[12];
    int nc = 0;
    E1RenderParams rb = render_defaults();
    E1CaptureParams cb = capture_defaults();
    rb.surround_on = rb.crystal_on = rb.bass_on = rb.svm_on = rb.dialog_on = 0;
    cb.nr_on = cb.svm_on = cb.vfx_on = 0;

    /* top level: bypass <-> active */
    tc[nc] = (ToggleCase){"render master (trim -0.1 dB)", 0, 2}; tc[nc].r0 = rb; tc[nc].r1 = rb;
    tc[nc].r1.master_on = 1; tc[nc].r1.output_gain_db = -0.1f; ++nc;
    tc[nc] = (ToggleCase){"render master (factory surround+clarity)", 0, 2}; tc[nc].r0 = render_defaults();
    tc[nc].r1 = render_defaults(); tc[nc].r1.master_on = 1; tc[nc].r0.master_on = 0; ++nc;
    tc[nc] = (ToggleCase){"capture master (NR)", 1, 1}; tc[nc].c0 = cb; tc[nc].c1 = cb;
    tc[nc].c1.master_on = 1; tc[nc].c1.nr_on = 1; ++nc;
    tc[nc] = (ToggleCase){"capture master (Voice FX)", 1, 1}; tc[nc].c0 = cb; tc[nc].c1 = cb;
    tc[nc].c1.master_on = 1; tc[nc].c1.vfx_on = 1; ++nc;

    /* per slot, chain held active */
    E1RenderParams ra = rb; ra.master_on = 1; ra.output_gain_db = -0.5f;
    E1CaptureParams ca = cb; ca.master_on = 1; ca.output_gain_db = -0.5f;
    tc[nc] = (ToggleCase){"render Surround slot", 0, 2}; tc[nc].r0 = ra; tc[nc].r1 = ra; tc[nc].r1.surround_on = 1; ++nc;
    tc[nc] = (ToggleCase){"render Smart Volume slot", 0, 2}; tc[nc].r0 = ra; tc[nc].r1 = ra; tc[nc].r1.svm_on = 1; ++nc;
    tc[nc] = (ToggleCase){"render Clarity slot", 0, 2}; tc[nc].r0 = ra; tc[nc].r1 = ra; tc[nc].r1.crystal_on = 1; ++nc;
    tc[nc] = (ToggleCase){"render Dialog+ slot", 0, 2}; tc[nc].r0 = ra; tc[nc].r1 = ra; tc[nc].r1.dialog_on = 1; ++nc;
    tc[nc] = (ToggleCase){"render limiter slot", 0, 2}; tc[nc].r0 = ra; tc[nc].r1 = ra;
    tc[nc].r0.limiter_on = 0; tc[nc].r1.limiter_on = 1; ++nc;
    tc[nc] = (ToggleCase){"capture NR slot", 1, 1}; tc[nc].c0 = ca; tc[nc].c1 = ca; tc[nc].c1.nr_on = 1; ++nc;
    tc[nc] = (ToggleCase){"capture Voice FX slot", 1, 1}; tc[nc].c0 = ca; tc[nc].c1 = ca; tc[nc].c1.vfx_on = 1; ++nc;
    tc[nc] = (ToggleCase){"capture Smart Volume slot", 1, 1}; tc[nc].c0 = ca; tc[nc].c1 = ca; tc[nc].c1.svm_on = 1; ++nc;

    const size_t frames = (size_t)(fs * 8.0), settle = (size_t)(fs * 1.0);
    rng_seed(4242);
    for (int k = 0; k < nc; ++k) {
        const ToggleCase *t = &tc[k];
        /* steady references */
        float *y0 = tc_run(t, fs, (size_t)(fs * 3.0), NULL, 0, 0);
        float *y1 = tc_run(t, fs, (size_t)(fs * 3.0), NULL, 0, 1);
        const double ref0 = max_d2(y0, settle, (size_t)(fs * 3.0), t->nch);
        const double ref1 = max_d2(y1, settle, (size_t)(fs * 3.0), t->nch);
        const double ref = ref0 > ref1 ? ref0 : ref1;
        free(y0); free(y1);
        /* random toggles, at least 0.35 s apart (fade + warm-up complete) */
        int tog[16], nt = 0, b = (int)(settle / 441) + rng_int(0, 20);
        while (nt < 16) {
            if ((size_t)(b + 40) * 441 >= frames) break;
            tog[nt++] = b;
            b += 40 + rng_int(0, 30);
        }
        float *y = tc_run(t, fs, frames, tog, nt, 0);
        const double worst = max_d2(y, settle - 441, frames, t->nch);
        free(y);
        const double bound = ref * 1.25 + 2e-4;
        printf("  fs %.0f %-40s: %2d toggles, max |2nd diff| %.5f (steady %.5f, bound %.5f)\n",
               fs, t->name, nt, worst, ref, bound);
        CHECK(worst <= bound, "fs %.0f %s: toggle produces a step (2nd diff %.5f > %.5f)", fs, t->name, worst, bound);
    }
}

int main(void)
{
    printf("test_chain\n");
    test_toggle_smoothness(44100);
    test_toggle_smoothness(48000);
    test_bypass_exact();
    test_return_to_bypass();
    test_clicks(44100);
    test_clicks(48000);
    test_latency();
    test_tail_and_robustness(44100);
    test_tail_and_robustness(48000);
    test_scout_and_reset();
    test_ceiling_during_fades(44100);
    test_ceiling_during_fades(48000);
    return test_summary("test_chain");
}
