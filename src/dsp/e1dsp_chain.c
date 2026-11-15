/*
 * e1dsp_chain.c - render and capture chains (public API of e1dsp.h).
 *
 * Shared engine ("Core"):
 *   - BYPASS state: the input is copied to the output bit for bit, zero latency, no FIFO.
 *     (Non-finite input samples are the only exception: they are replaced by 0.)
 *   - ACTIVE state: interleaved input -> FIFO -> planar blocks of E1DSP_BLOCK frames -> slots
 *     -> FIFO -> interleaved output. The FIFO adds exactly E1DSP_BLOCK frames.
 *   - A slot wraps one module. Every usable slot always contributes its fixed latency: when the
 *     module is off its input goes through a delay line of the same length (the "dry path"),
 *     so the total latency is constant while active and toggling a module never shifts time.
 *     Toggling crossfades (E1DSP_XFADE_MS) between the aligned dry path and the module output.
 *     A module is reset when it fades in from fully off, so it never replays stale state.
 *     Warm-up: after that reset the module runs on the real signal but its crossfade weight is held
 *     at 0 for its "warm" time (its latency plus a module-specific settle time: an STFT frame, the
 *     surround's pre-delay and all-pass tails, ...). Only then does the raised-cosine fade start,
 *     so the module side of the mix never contains the start-up zeros or onset transient of a
 *     freshly reset module (that would otherwise reach the output as a step = click / dropout).
 *   - BYPASS <-> ACTIVE: crossfade over E1DSP_XFADE_MS between the zero-latency input and the
 *     active-path output. A raised-cosine, amplitude-complementary law is used (w + (1-w) = 1):
 *     it never produces a sample larger than the larger of the two inputs (safe for correlated
 *     signals, no +3 dB bump), and its smooth ends avoid spectral splatter. Reversing direction
 *     mid-fade continues from the current position. When a fade-out reaches 0 the remainder of
 *     the buffer and everything after it is a plain bit-exact copy again.
 *     The bypass path cannot be latency-aligned (it is zero latency by contract), so this one
 *     transition mixes two time-shifted copies for 20 ms; that is click-free (bounded slope),
 *     only a brief comb colouration while the effect fades in or out.
 *     Warm-up: on BYPASS -> ACTIVE every module starts from reset, so the active path first emits
 *     `latency` frames of zeros followed by the modules' onset transients. The output therefore
 *     stays the pure (zero-latency) dry signal for latency + max(settle of the enabled slots)
 *     frames, and only then does the 20 ms crossfade start.
 *   - FTZ/DAZ is enabled for the duration of an active process call and restored afterwards.
 *
 * Threading: set_params / process / reset of one instance must not run concurrently (the APO
 * applies parameter updates on its processing thread between buffers). The meter getters only
 * read floats and may be called from any thread.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "e1dsp_modules.h"
#include "e1_biquad.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__) || defined(__x86_64__)
#include <xmmintrin.h>
#define FPU_GUARD_BEGIN const unsigned int fpu_csr_ = _mm_getcsr(); _mm_setcsr(fpu_csr_ | 0x8040u);
#define FPU_GUARD_END   _mm_setcsr(fpu_csr_);
#else
#define FPU_GUARD_BEGIN
#define FPU_GUARD_END
#endif

#define MAX_SLOTS   8
#define IN_CLAMP    256.0f   /* +48 dBFS: anything beyond is garbage, keep filters sane */
#define METER_TAU_S 0.100

typedef void (*SlotProc)(void *m, float *const *ch, int nch, int n);
typedef void (*SlotReset)(void *m);

typedef struct Slot {
    void     *mod;
    SlotProc  proc;
    SlotReset reset;
    unsigned  lat;
    unsigned  settle;      /* onset transient length beyond lat */
    unsigned  warm;        /* frames the weight stays 0 after a fresh reset (lat + settle) */
    unsigned  hold;        /* remaining warm-up frames */
    int       primed;      /* module was reset for the current fade-in (or is fully on) */
    int       stereo;      /* module processes channels 0/1 only */
    int       usable;
    int       target;      /* requested on/off */
    double    mix;         /* 0 = dry path, 1 = module output */
    float    *ring[E1DSP_MAX_CH];
    unsigned  rpos;
} Slot;

typedef struct Core {
    double    fs;
    int       nch;
    Slot      slot[MAX_SLOTS];
    int       nslots;
    int       trim_at;     /* the output trim runs before this slot index */
    int       lim_slot;    /* slot index of the limiter (for the meter), -1 if none */
    E1Limiter *lim;
    double    trim, trim_t, trim_a;
    float    *fin[E1DSP_MAX_CH], *fout[E1DSP_MAX_CH], *dry[E1DSP_MAX_CH];
    unsigned  pos;
    float    *xsave;       /* interleaved copy of the current input chunk (crossfade dry) */
    int       want;        /* parameters ask for the active path */
    int       active;      /* 0 = bit-exact bypass */
    double    xk;          /* top-level crossfade position, 0 = bypass, 1 = active */
    int       xdir;
    unsigned  xhold;       /* top-level warm-up frames left (output = dry, fade not started) */
    double    xstep;      /* per frame */
    double    sstep;       /* per-slot crossfade step per frame */
    unsigned  latency;
    volatile float pk_in[2], pk_out[2], gr_db;
} Core;

/* ------------------------------------------------------------------------------------------ */
/* helpers                                                                                    */

static float xf_w(double k) { return (float)(0.5 - 0.5 * cos(E1_PI * k)); }

static void copy_finite(const float *in, float *out, size_t n)
{
    if (in != out) memcpy(out, in, n * sizeof(float));
    for (size_t i = 0; i < n; ++i) {
        uint32_t b;
        memcpy(&b, &out[i], sizeof b);
        if ((b & 0x7f800000u) == 0x7f800000u) out[i] = 0.0f;
    }
}

static float peak_ch(const float *buf, size_t frames, int nch, int c)
{
    float pk = 0.f;
    for (size_t i = 0; i < frames; ++i) {
        float v = fabsf(buf[i * (size_t)nch + (size_t)c]);
        if (v > pk && v <= 3.4e38f) pk = v; /* NaN and Inf are ignored */
    }
    return pk;
}

static void meter_update(volatile float pk[2], const float *buf, size_t frames, int nch, double fs)
{
    float dec = (float)exp(-(double)frames / (METER_TAU_S * fs));
    for (int c = 0; c < 2; ++c) {
        int src = c < nch ? c : 0;
        float p = peak_ch(buf, frames, nch, src), o = pk[c] * dec;
        pk[c] = p > o ? p : o;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* core                                                                                       */

static int core_init(Core *k, double fs, int nch)
{
    memset(k, 0, sizeof(*k));
    k->fs = fs;
    k->nch = nch;
    k->lim_slot = -1;
    k->trim = k->trim_t = 1.0;
    k->trim_a = e1_onepole_coef(0.010, fs);
    k->xstep = 1.0 / (E1DSP_XFADE_MS * 0.001 * fs);
    k->sstep = k->xstep;
    for (int c = 0; c < nch; ++c) {
        k->fin[c] = (float *)calloc(E1DSP_BLOCK, sizeof(float));
        k->fout[c] = (float *)calloc(E1DSP_BLOCK, sizeof(float));
        k->dry[c] = (float *)calloc(E1DSP_BLOCK, sizeof(float));
        if (!k->fin[c] || !k->fout[c] || !k->dry[c]) return 0;
    }
    k->xsave = (float *)calloc((size_t)E1DSP_BLOCK * (size_t)nch, sizeof(float));
    return k->xsave != NULL;
}

static void core_free(Core *k)
{
    for (int c = 0; c < E1DSP_MAX_CH; ++c) {
        free(k->fin[c]); free(k->fout[c]); free(k->dry[c]);
        for (int s = 0; s < k->nslots; ++s) free(k->slot[s].ring[c]);
    }
    free(k->xsave);
}

/* settle: frames after lat until a freshly reset module's output is free of its onset transient */
static int core_add_slot(Core *k, void *mod, SlotProc proc, SlotReset reset, unsigned lat, unsigned settle,
                         int stereo)
{
    Slot *s = &k->slot[k->nslots++];
    s->mod = mod;
    s->proc = proc;
    s->reset = reset;
    s->stereo = stereo;
    s->usable = (mod != NULL) && (!stereo || k->nch >= 2);
    s->lat = s->usable ? lat : 0;
    s->settle = s->usable ? settle : 0;
    s->warm = s->lat + s->settle;
    if (s->lat) {
        for (int c = 0; c < k->nch; ++c) {
            s->ring[c] = (float *)calloc(s->lat, sizeof(float));
            if (!s->ring[c]) return 0;
        }
    }
    k->latency += s->lat;
    return 1;
}

static void core_clear(Core *k)
{
    for (int c = 0; c < k->nch; ++c) {
        memset(k->fin[c], 0, E1DSP_BLOCK * sizeof(float));
        memset(k->fout[c], 0, E1DSP_BLOCK * sizeof(float));
    }
    k->pos = 0;
    for (int i = 0; i < k->nslots; ++i) {
        Slot *s = &k->slot[i];
        s->mix = s->target ? 1.0 : 0.0;
        s->primed = s->target;
        s->hold = 0;
        s->rpos = 0;
        for (int c = 0; c < k->nch; ++c)
            if (s->ring[c]) memset(s->ring[c], 0, s->lat * sizeof(float));
        if (s->usable) s->reset(s->mod);
    }
    k->trim = k->trim_t;
    k->gr_db = 0.f;
    k->xhold = 0;
}

/* Warm-up of the whole active path after core_clear: the input reaches the output after
   `latency` frames, and each enabled module needs its settle time on top of that. */
static unsigned core_warmup(const Core *k)
{
    unsigned extra = 0;
    for (int i = 0; i < k->nslots; ++i) {
        const Slot *s = &k->slot[i];
        if (s->usable && s->target && s->settle > extra) extra = s->settle;
    }
    return k->latency + extra;
}

static void core_set_want(Core *k, int want)
{
    k->want = want;
    if (k->active) k->xdir = want ? (k->xk < 1.0 ? 1 : 0) : -1;
}

static void trim_run(Core *k, float *const *ch)
{
    const int n = E1DSP_BLOCK;
    if (k->trim == k->trim_t) {
        if (k->trim == 1.0) return;
        const float g = (float)k->trim;
        for (int c = 0; c < k->nch; ++c)
            for (int i = 0; i < n; ++i) ch[c][i] *= g;
        return;
    }
    double g = k->trim;
    const double t = k->trim_t, a = k->trim_a;
    float gs[E1DSP_BLOCK];
    for (int i = 0; i < n; ++i) {
        g = t + (g - t) * a;
        gs[i] = (float)g;
    }
    if (fabs(g - t) < 1e-6 * t) g = t;
    k->trim = g;
    for (int c = 0; c < k->nch; ++c)
        for (int i = 0; i < n; ++i) ch[c][i] *= gs[i];
}

static void slot_run(Core *k, Slot *s, float *const *ch)
{
    const int n = E1DSP_BLOCK, nch = k->nch;
    if (!s->usable) return;
    const int off = !s->target && s->mix <= 0.0;
    const int full = s->target && s->mix >= 1.0;
    const int mc = s->stereo ? 2 : nch;

    if (s->lat) {
        /* aligned dry path; kept running so a later fade-out has valid history */
        const unsigned L = s->lat;
        unsigned p = s->rpos;
        for (int c = 0; c < nch; ++c) {
            float *rg = s->ring[c], *d = k->dry[c];
            const float *x = ch[c];
            p = s->rpos;
            for (int i = 0; i < n; ++i) {
                d[i] = rg[p];
                rg[p] = x[i];
                if (++p == L) p = 0;
            }
        }
        s->rpos = p;
    } else if (!full && !off) {
        for (int c = 0; c < mc; ++c) memcpy(k->dry[c], ch[c], (size_t)n * sizeof(float));
    }

    if (off) {
        s->primed = 0;
        s->hold = 0;
        if (s->lat)
            for (int c = 0; c < nch; ++c) memcpy(ch[c], k->dry[c], (size_t)n * sizeof(float));
        return;
    }
    if (!s->primed) { /* fresh fade-in from fully off: start clean, warm up before fading */
        s->reset(s->mod);
        s->primed = 1;
        s->hold = s->warm;
    }
    s->proc(s->mod, ch, mc, n);
    if (s->lat)
        for (int c = mc; c < nch; ++c) memcpy(ch[c], k->dry[c], (size_t)n * sizeof(float));
    if (!full) {
        double m = s->mix;
        const double st = s->target ? k->sstep : -k->sstep;
        unsigned hold = s->target ? s->hold : 0;
        for (int i = 0; i < n; ++i) {
            if (hold) { /* warm-up: module output discarded, the aligned dry path is the output */
                --hold;
                for (int c = 0; c < mc; ++c) ch[c][i] = k->dry[c][i];
                continue;
            }
            m += st;
            if (m > 1.0) m = 1.0;
            if (m < 0.0) m = 0.0;
            const float w = xf_w(m), v = 1.0f - w;
            for (int c = 0; c < mc; ++c) ch[c][i] = w * ch[c][i] + v * k->dry[c][i];
        }
        s->hold = hold;
        s->mix = m;
    }
}

static void core_block(Core *k)
{
    float *const *ch = k->fout;
    for (int c = 0; c < k->nch; ++c) memcpy(k->fout[c], k->fin[c], E1DSP_BLOCK * sizeof(float));
    for (int i = 0; i < k->nslots; ++i) {
        if (i == k->trim_at) trim_run(k, ch);
        slot_run(k, &k->slot[i], ch);
    }
    if (k->trim_at >= k->nslots) trim_run(k, ch);
    for (int c = 0; c < k->nch; ++c) {
        float *o = k->fout[c];
        for (int i = 0; i < E1DSP_BLOCK; ++i)
            if (!isfinite(o[i])) o[i] = 0.0f;
    }
    if (k->lim_slot >= 0 && k->slot[k->lim_slot].mix > 0.0) k->gr_db = e1_limiter_gr_db(k->lim);
    else k->gr_db = 0.f;
}

static void core_process(Core *k, const float *in, float *out, size_t frames)
{
    const int nch = k->nch;
    if (!in || !out || frames == 0) return;
    meter_update(k->pk_in, in, frames, nch, k->fs);
    if (!k->active) {
        if (!k->want) {
            copy_finite(in, out, frames * (size_t)nch);
            k->pk_out[0] = k->pk_in[0];
            k->pk_out[1] = k->pk_in[1];
            k->gr_db = 0.f;
            return;
        }
        core_clear(k);
        k->active = 1;
        k->xk = 0.0;
        k->xdir = 1;
        k->xhold = core_warmup(k);
    }

    FPU_GUARD_BEGIN
    size_t done = 0;
    while (done < frames) {
        const float *ip = in + done * (size_t)nch;
        float *op = out + done * (size_t)nch;
        if (!k->active) {
            copy_finite(ip, op, (frames - done) * (size_t)nch);
            break;
        }
        size_t chunk = E1DSP_BLOCK - k->pos;
        if (chunk > frames - done) chunk = frames - done;
        const int fading = (k->xdir != 0);
        const size_t ns = chunk * (size_t)nch;
        if (fading) memcpy(k->xsave, ip, ns * sizeof(float));

        for (size_t i = 0; i < chunk; ++i)
            for (int c = 0; c < nch; ++c) {
                float v = ip[i * (size_t)nch + (size_t)c];
                if (!(v >= -IN_CLAMP && v <= IN_CLAMP)) v = isfinite(v) ? (v > 0.f ? IN_CLAMP : -IN_CLAMP) : 0.0f;
                k->fin[c][k->pos + i] = v;
            }
        for (size_t i = 0; i < chunk; ++i)
            for (int c = 0; c < nch; ++c) op[i * (size_t)nch + (size_t)c] = k->fout[c][k->pos + i];

        if (fading) {
            for (size_t i = 0; i < chunk; ++i) {
                if (k->xhold && k->xdir > 0) { /* warm-up: the active path is not valid yet */
                    --k->xhold;
                    float *o = op + i * (size_t)nch;
                    const float *d = k->xsave + i * (size_t)nch;
                    for (int c = 0; c < nch; ++c) o[c] = isfinite(d[c]) ? d[c] : 0.0f;
                    continue;
                }
                k->xk += k->xdir * k->xstep;
                if (k->xk >= 1.0) {
                    k->xk = 1.0;
                    k->xdir = 0;
                    break; /* rest of the chunk is fully wet */
                }
                if (k->xk <= 0.0) {
                    k->xk = 0.0;
                    k->xdir = 0;
                    k->xhold = 0;
                    k->active = 0;
                    copy_finite(k->xsave + i * (size_t)nch, op + i * (size_t)nch, (chunk - i) * (size_t)nch);
                    break;
                }
                const float w = xf_w(k->xk), v = 1.0f - w;
                float *o = op + i * (size_t)nch;
                const float *d = k->xsave + i * (size_t)nch;
                for (int c = 0; c < nch; ++c) {
                    float dv = d[c];
                    if (!isfinite(dv)) dv = 0.0f;
                    o[c] = w * o[c] + v * dv;
                }
            }
        }
        k->pos += (unsigned)chunk;
        done += chunk;
        if (k->pos == E1DSP_BLOCK) {
            if (k->active) core_block(k);
            k->pos = 0;
        }
    }
    FPU_GUARD_END
    meter_update(k->pk_out, out, frames, nch, k->fs);
}

static void core_reset(Core *k)
{
    core_clear(k);
    k->active = k->want;
    k->xk = k->want ? 1.0 : 0.0;
    k->xdir = 0;
    k->pk_in[0] = k->pk_in[1] = k->pk_out[0] = k->pk_out[1] = 0.f;
}

static void core_meter(const Core *k, float peak_in[2], float peak_out[2], float *gr_db)
{
    if (peak_in) { peak_in[0] = k->pk_in[0]; peak_in[1] = k->pk_in[1]; }
    if (peak_out) { peak_out[0] = k->pk_out[0]; peak_out[1] = k->pk_out[1]; }
    if (gr_db) *gr_db = k->active ? k->gr_db : 0.f;
}

/* ------------------------------------------------------------------------------------------ */
/* module adapters                                                                            */

static void p_dialog(void *m, float *const *ch, int nch, int n) { (void)nch; e1_dialog_process((E1Dialog *)m, ch[0], ch[1], n); }
static void r_dialog(void *m) { e1_dialog_reset((E1Dialog *)m); }
static void p_sur(void *m, float *const *ch, int nch, int n) { (void)nch; e1_surround_process((E1Surround *)m, ch[0], ch[1], n); }
static void r_sur(void *m) { e1_surround_reset((E1Surround *)m); }
static void p_svm(void *m, float *const *ch, int nch, int n) { e1_svm_process((E1Svm *)m, ch, nch, n); }
static void r_svm(void *m) { e1_svm_reset((E1Svm *)m); }
static void p_cry(void *m, float *const *ch, int nch, int n) { e1_crystal_process((E1Crystal *)m, ch, nch, n); }
static void r_cry(void *m) { e1_crystal_reset((E1Crystal *)m); }
static void p_eq(void *m, float *const *ch, int nch, int n) { e1_eq_process((E1Eq *)m, ch, nch, n); }
static void r_eq(void *m) { e1_eq_reset((E1Eq *)m); }
static void p_bass(void *m, float *const *ch, int nch, int n) { e1_bass_process((E1Bass *)m, ch, nch, n); }
static void r_bass(void *m) { e1_bass_reset((E1Bass *)m); }
static void p_lim(void *m, float *const *ch, int nch, int n) { e1_limiter_process((E1Limiter *)m, ch, nch, n); }
static void r_lim(void *m) { e1_limiter_reset((E1Limiter *)m); }
static void p_nr(void *m, float *const *ch, int nch, int n) { e1_nr_process((E1Nr *)m, ch, nch, n); }
static void r_nr(void *m) { e1_nr_reset((E1Nr *)m); }
static void p_vfx(void *m, float *const *ch, int nch, int n) { e1_vfx_process((E1VoiceFx *)m, ch, nch, n); }
static void r_vfx(void *m) { e1_vfx_reset((E1VoiceFx *)m); }

/* Onset settle time for modules without a dedicated figure: a block-processing (STFT/WOLA or
   look-ahead) module smears the onset over about one more latency; a zero-latency IIR module
   needs a few milliseconds for its start-up transient. */
static unsigned settle_generic(unsigned lat) { return lat ? lat : 256u; }

static int valid_fmt(double fs, int channels)
{
    return fs >= 8000.0 && fs <= 384000.0 && channels >= 1 && channels <= E1DSP_MAX_CH;
}

/* ------------------------------------------------------------------------------------------ */
/* render                                                                                     */

enum { RS_DIALOG = 0, RS_SURROUND, RS_SVM, RS_CRYSTAL, RS_EQ, RS_BASS, RS_LIMITER, RS_COUNT };

struct E1Render {
    Core        core;
    E1Dialog   *dlg;
    E1Surround *sur;
    E1Svm      *svm;
    E1Crystal  *cry;
    E1Eq       *eq;
    E1Bass     *bass;
    E1Limiter  *lim;
};

E1Render *e1_render_create(double fs, int channels)
{
    if (!valid_fmt(fs, channels)) return NULL;
    E1Render *r = (E1Render *)calloc(1, sizeof(E1Render));
    if (!r) return NULL;
    Core *k = &r->core;
    int ok = core_init(k, fs, channels);
    if (ok && channels >= 2) {
        r->dlg = e1_dialog_create(fs);
        r->sur = e1_surround_create(fs);
        ok = r->dlg && r->sur;
    }
    if (ok) {
        r->svm = e1_svm_create(fs, channels, 0);
        r->cry = e1_crystal_create(fs, channels);
        r->eq = e1_eq_create(fs, channels);
        r->bass = e1_bass_create(fs, channels);
        r->lim = e1_limiter_create(fs, channels);
        ok = r->svm && r->cry && r->eq && r->bass && r->lim;
    }
    if (ok) {
        const unsigned ldlg = r->dlg ? e1_dialog_latency(r->dlg) : 0;
        const unsigned lsur = r->sur ? e1_surround_latency(r->sur) : 0;
        const unsigned lsvm = e1_svm_latency(r->svm), lcry = e1_crystal_latency(r->cry);
        const unsigned leq = e1_eq_latency(r->eq), lbass = e1_bass_latency(r->bass);
        const unsigned llim = e1_limiter_latency(r->lim);
        ok = core_add_slot(k, r->dlg, p_dialog, r_dialog, ldlg, settle_generic(ldlg), 1)
          && core_add_slot(k, r->sur, p_sur, r_sur, lsur, r->sur ? e1_surround_settle_frames(r->sur) : 0, 1)
          && core_add_slot(k, r->svm, p_svm, r_svm, lsvm, settle_generic(lsvm), 0)
          && core_add_slot(k, r->cry, p_cry, r_cry, lcry, settle_generic(lcry), 0)
          && core_add_slot(k, r->eq, p_eq, r_eq, leq, settle_generic(leq), 0)
          && core_add_slot(k, r->bass, p_bass, r_bass, lbass, settle_generic(lbass), 0)
          && core_add_slot(k, r->lim, p_lim, r_lim, llim, settle_generic(llim), 0);
        k->trim_at = RS_LIMITER;
        k->lim_slot = RS_LIMITER;
        k->lim = r->lim;
        k->latency += E1DSP_BLOCK;
    }
    if (!ok) { e1_render_destroy(r); return NULL; }
    e1_limiter_set(r->lim, -0.3f);
    E1Params d;
    e1fx_params_default(&d);
    e1_render_set_params(r, &d.render);
    core_reset(k);
    return r;
}

void e1_render_destroy(E1Render *r)
{
    if (!r) return;
    if (r->dlg) e1_dialog_destroy(r->dlg);
    if (r->sur) e1_surround_destroy(r->sur);
    if (r->svm) e1_svm_destroy(r->svm);
    if (r->cry) e1_crystal_destroy(r->cry);
    if (r->eq) e1_eq_destroy(r->eq);
    if (r->bass) e1_bass_destroy(r->bass);
    if (r->lim) e1_limiter_destroy(r->lim);
    core_free(&r->core);
    free(r);
}

void e1_render_reset(E1Render *r) { if (r) core_reset(&r->core); }

void e1_render_set_params(E1Render *r, const E1RenderParams *pin)
{
    if (!r || !pin) return;
    Core *k = &r->core;
    E1Params tmp;
    e1fx_params_default(&tmp);
    tmp.render = *pin;
    e1fx_params_sanitize(&tmp);
    const E1RenderParams *p = &tmp.render;

    static const float scout_eq[E1FX_EQ_BANDS] = E1FX_SCOUT_EQ;
    const int master = (int)p->master_on, scout = (int)p->scout_on;
    const int enabled = master || p->eq_on || scout;

    const int sur = (master && p->surround_on) || scout;
    const int dlg = master && p->dialog_on;
    const int svm = master && p->svm_on;
    const int cry = master && p->crystal_on;
    const int bass = master && p->bass_on;
    const int eq_eff = p->eq_on || scout;
    const float *gains = scout ? scout_eq : p->eq_gain_db;
    const float pre = scout ? 0.f : p->eq_preamp_db;
    int eq_nonflat = (pre != 0.f);
    for (int b = 0; b < E1FX_EQ_BANDS; ++b)
        if (gains[b] != 0.f) eq_nonflat = 1;
    const int eq = eq_eff && eq_nonflat;
    const double trim_t = enabled ? e1_db2lin(p->output_gain_db) : 1.0;
    /* The safety limiter runs whenever the chain is active (and limiter_on). Below the ceiling it
       is an exact delay, so it costs nothing in transparency; keeping it in the path at all times
       means module crossfades (each fading between an aligned dry path and a boosted output) can
       never overshoot the ceiling, which a limiter that itself fades in alongside them could. */
    const int lim = (int)p->limiter_on;

    const int any = sur || dlg || svm || cry || eq || bass || trim_t != 1.0;
    const int want = enabled && any;
    if (!want) {
        /* keep the active path exactly as it was while it fades out */
        core_set_want(k, 0);
        return;
    }

    if (r->sur) e1_surround_set(r->sur, scout ? 0.9f : p->surround_level, (int)p->speaker_mode);
    if (r->dlg) e1_dialog_set(r->dlg, p->dialog_level);
    e1_svm_set(r->svm, p->svm_level, (int)p->svm_mode);
    e1_crystal_set(r->cry, p->crystal_level);
    e1_eq_set(r->eq, pre, gains);
    e1_bass_set(r->bass, p->bass_level, p->bass_xover_hz, p->speaker_mode == E1FX_SPK_HEADPHONES);
    e1_limiter_set(r->lim, -0.3f);

    k->slot[RS_DIALOG].target = dlg;
    k->slot[RS_SURROUND].target = sur;
    k->slot[RS_SVM].target = svm;
    k->slot[RS_CRYSTAL].target = cry;
    k->slot[RS_EQ].target = eq;
    k->slot[RS_BASS].target = bass;
    k->slot[RS_LIMITER].target = lim;
    k->trim_t = trim_t;
    core_set_want(k, 1);
}

void e1_render_process(E1Render *r, const float *in, float *out, size_t frames)
{
    if (r) core_process(&r->core, in, out, frames);
}

int e1_render_is_bypassed(const E1Render *r) { return r ? !r->core.active : 1; }
unsigned e1_render_latency_frames(const E1Render *r) { return r ? r->core.latency : 0; }
void e1_render_meter(const E1Render *r, float peak_in[2], float peak_out[2], float *gr_db)
{
    if (r) core_meter(&r->core, peak_in, peak_out, gr_db);
}

/* ------------------------------------------------------------------------------------------ */
/* capture                                                                                    */

enum { CS_NR = 0, CS_VFX, CS_SVM, CS_LIMITER, CS_COUNT };

struct E1Capture {
    Core       core;
    E1Nr      *nr;
    E1VoiceFx *vfx;
    E1Svm     *svm;
    E1Limiter *lim;
};

E1Capture *e1_capture_create(double fs, int channels)
{
    if (!valid_fmt(fs, channels)) return NULL;
    E1Capture *cp = (E1Capture *)calloc(1, sizeof(E1Capture));
    if (!cp) return NULL;
    Core *k = &cp->core;
    int ok = core_init(k, fs, channels);
    if (ok) {
        cp->nr = e1_nr_create(fs, channels);
        cp->vfx = e1_vfx_create(fs, channels);
        cp->svm = e1_svm_create(fs, channels, 1);
        cp->lim = e1_limiter_create(fs, channels);
        ok = cp->nr && cp->vfx && cp->svm && cp->lim;
    }
    if (ok) {
        const unsigned lnr = e1_nr_latency(cp->nr), lvfx = e1_vfx_latency(cp->vfx);
        const unsigned lsvm = e1_svm_latency(cp->svm), llim = e1_limiter_latency(cp->lim);
        ok = core_add_slot(k, cp->nr, p_nr, r_nr, lnr, settle_generic(lnr), 0)
          && core_add_slot(k, cp->vfx, p_vfx, r_vfx, lvfx, settle_generic(lvfx), 0)
          && core_add_slot(k, cp->svm, p_svm, r_svm, lsvm, settle_generic(lsvm), 0)
          && core_add_slot(k, cp->lim, p_lim, r_lim, llim, settle_generic(llim), 0);
        k->trim_at = CS_LIMITER;
        k->lim_slot = CS_LIMITER;
        k->lim = cp->lim;
        k->latency += E1DSP_BLOCK;
    }
    if (!ok) { e1_capture_destroy(cp); return NULL; }
    e1_limiter_set(cp->lim, -0.3f);
    E1Params d;
    e1fx_params_default(&d);
    e1_capture_set_params(cp, &d.capture);
    core_reset(k);
    return cp;
}

void e1_capture_destroy(E1Capture *c)
{
    if (!c) return;
    if (c->nr) e1_nr_destroy(c->nr);
    if (c->vfx) e1_vfx_destroy(c->vfx);
    if (c->svm) e1_svm_destroy(c->svm);
    if (c->lim) e1_limiter_destroy(c->lim);
    core_free(&c->core);
    free(c);
}

void e1_capture_reset(E1Capture *c) { if (c) core_reset(&c->core); }

void e1_capture_set_params(E1Capture *cp, const E1CaptureParams *pin)
{
    if (!cp || !pin) return;
    Core *k = &cp->core;
    E1Params tmp;
    e1fx_params_default(&tmp);
    tmp.capture = *pin;
    e1fx_params_sanitize(&tmp);
    const E1CaptureParams *p = &tmp.capture;

    const int master = (int)p->master_on;
    const int nr = master && p->nr_on;
    const int vfx = master && p->vfx_on;
    const int svm = master && p->svm_on;
    const double trim_t = master ? e1_db2lin(p->output_gain_db) : 1.0;
    const int lim = master; /* always in the active path: see e1_render_set_params */
    const int want = master && (nr || vfx || svm || trim_t != 1.0);
    if (!want) {
        core_set_want(k, 0);
        return;
    }
    e1_nr_set(cp->nr, p->nr_strength);
    e1_vfx_set(cp->vfx, p->vfx);
    e1_svm_set(cp->svm, p->svm_level, 0);
    e1_limiter_set(cp->lim, -0.3f);
    k->slot[CS_NR].target = nr;
    k->slot[CS_VFX].target = vfx;
    k->slot[CS_SVM].target = svm;
    k->slot[CS_LIMITER].target = lim;
    k->trim_t = trim_t;
    core_set_want(k, 1);
}

void e1_capture_process(E1Capture *c, const float *in, float *out, size_t frames)
{
    if (c) core_process(&c->core, in, out, frames);
}

int e1_capture_is_bypassed(const E1Capture *c) { return c ? !c->core.active : 1; }
unsigned e1_capture_latency_frames(const E1Capture *c) { return c ? c->core.latency : 0; }
void e1_capture_meter(const E1Capture *c, float peak_in[2], float peak_out[2], float *gr_db)
{
    if (c) core_meter(&c->core, peak_in, peak_out, gr_db);
}
