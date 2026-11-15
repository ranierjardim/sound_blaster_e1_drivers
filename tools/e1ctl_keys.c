/*
 * e1ctl_keys.c - parameter key table, parsing and formatting for e1ctl (pure C).
 */
#include "e1ctl_keys.h"
#include "../src/dsp/e1dsp_modules.h"   /* e1_eq_presets / e1_vfx_presets */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_bool_names[] = { "off", "on" };
static const char *const k_svm_names[]  = { "normal", "loud", "night" };
static const char *const k_spk_names[]  = { "headphones", "stereo" };

#define OFF(field) offsetof(E1Params, field)

#define KB(nm, fld, hlp)                 { nm, NULL, E1K_BOOL,  OFF(fld), 0, 1, k_bool_names, 2, "", hlp }
#define KF(nm, al, fld, lo, hi, un, hlp) { nm, al,   E1K_FLOAT, OFF(fld), lo, hi, NULL, 0, un, hlp }
#define KE(nm, fld, names, cnt, hlp)     { nm, NULL, E1K_ENUM,  OFF(fld), 0, (cnt) - 1, names, cnt, "", hlp }
#define KU(nm, fld, lo, hi, hlp)         { nm, NULL, E1K_UINT,  OFF(fld), lo, hi, NULL, 0, "", hlp }

#define KEQ(i, al) KF("render.eq_gain_db[" #i "]", "render.eq." al, render.eq_gain_db[i], -24, 24, "dB", \
                      "Equalizer band " al " Hz gain")

const E1KeyDef e1k_keys[] = {
    /* ---- render (playback) ---- */
    KB("render.master_on",      render.master_on,   "Effects master switch (0 = bit-exact passthrough except EQ)"),
    KB("render.surround_on",    render.surround_on, "Surround virtualizer"),
    KF("render.surround_level", NULL, render.surround_level, 0, 1, "", "Surround amount"),
    KB("render.crystal_on",     render.crystal_on,  "Clarity (transient/detail enhancer)"),
    KF("render.crystal_level",  NULL, render.crystal_level, 0, 1, "", "Clarity amount"),
    KB("render.bass_on",        render.bass_on,     "Bass enhancer"),
    KF("render.bass_level",     NULL, render.bass_level, 0, 100, "", "Bass strength"),
    KF("render.bass_xover_hz",  NULL, render.bass_xover_hz, 10, 1000, "Hz", "Bass crossover frequency"),
    KB("render.svm_on",         render.svm_on,      "Smart Volume (loudness levelling)"),
    KF("render.svm_level",      NULL, render.svm_level, 0, 1, "", "Smart Volume strength"),
    KE("render.svm_mode",       render.svm_mode, k_svm_names, 3, "Smart Volume mode"),
    KB("render.dialog_on",      render.dialog_on,   "Dialog+ (speech enhancement)"),
    KF("render.dialog_level",   NULL, render.dialog_level, 0, 1, "", "Dialog+ strength"),
    KB("render.eq_on",          render.eq_on,       "10-band equalizer (independent of master_on)"),
    KF("render.eq_preamp_db",   "render.eq.preamp", render.eq_preamp_db, -12, 12, "dB", "Equalizer pre-amp"),
    KEQ(0, "31"), KEQ(1, "62"), KEQ(2, "125"), KEQ(3, "250"), KEQ(4, "500"),
    KEQ(5, "1k"), KEQ(6, "2k"), KEQ(7, "4k"), KEQ(8, "8k"), KEQ(9, "16k"),
    KE("render.speaker_mode",   render.speaker_mode, k_spk_names, 2, "Output: headphones (binaural) or stereo speakers"),
    KB("render.scout_on",       render.scout_on,    "Scout Mode (footstep EQ curve, forces surround)"),
    KF("render.output_gain_db", "render.gain", render.output_gain_db, -24, 12, "dB", "Output trim before the limiter"),
    KB("render.limiter_on",     render.limiter_on,  "Safety limiter (only when some stage is active)"),

    /* ---- capture (microphone) ---- */
    KB("capture.master_on",     capture.master_on,  "Voice master switch (0 = bit-exact passthrough)"),
    KB("capture.nr_on",         capture.nr_on,      "Noise reduction"),
    KF("capture.nr_strength",   NULL, capture.nr_strength, 0, 1, "", "Noise reduction strength"),
    KB("capture.svm_on",        capture.svm_on,     "Mic Smart Volume"),
    KF("capture.svm_level",     NULL, capture.svm_level, 0, 1, "", "Mic Smart Volume strength"),
    KB("capture.vfx_on",        capture.vfx_on,     "Voice effects"),
    KU("capture.vfx_preset",    capture.vfx_preset, 0, 17, "Voice effect preset index (informational for the panel)"),
    KF("capture.vfx[0]", "capture.vfx.f1",        capture.vfx[0], 200, 800,   "Hz", "Voice formant F1"),
    KF("capture.vfx[1]", "capture.vfx.f2",        capture.vfx[1], 800, 1800,  "Hz", "Voice formant F2"),
    KF("capture.vfx[2]", "capture.vfx.f3",        capture.vfx[2], 1600, 2750, "Hz", "Voice formant F3"),
    KF("capture.vfx[3]", "capture.vfx.pitch",     capture.vfx[3], 0.25, 4,    "",   "Voice pitch factor"),
    KF("capture.vfx[4]", "capture.vfx.envelope",  capture.vfx[4], 0.5, 2,     "",   "Voice formant (envelope) factor"),
    KF("capture.vfx[5]", "capture.vfx.pitch_var", capture.vfx[5], -1, 5,      "",   "Voice pitch variability"),
    KF("capture.vfx[6]", "capture.vfx.quiver",    capture.vfx[6], 0, 0.2,     "",   "Voice quiver depth"),
    KF("capture.vfx[7]", "capture.vfx.contour",   capture.vfx[7], 0, 0.4,     "",   "Voice contour depth"),
    KF("capture.output_gain_db", "capture.gain", capture.output_gain_db, -24, 24, "dB", "Mic output trim"),
};
const int e1k_key_count = (int)(sizeof(e1k_keys) / sizeof(e1k_keys[0]));

/* ------------------------------------------------------------------------------------------ */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int e1k_loose_equal(const char *a, const char *b)
{
    for (;;) {
        while (*a == ' ' || *a == '-' || *a == '_') ++a;
        while (*b == ' ' || *b == '-' || *b == '_') ++b;
        if (!*a || !*b) return *a == *b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
}

/* "render.eq_gain_db.3" -> "render.eq_gain_db[3]" (only when the suffix is all digits). */
static void normalize_index(const char *in, char *out, size_t n)
{
    size_t len = strlen(in);
    const char *dot = strrchr(in, '.');
    if (dot && dot[1] && len + 2 < n) {
        const char *q = dot + 1;
        while (*q && isdigit((unsigned char)*q)) ++q;
        if (*q == '\0') {
            size_t head = (size_t)(dot - in);
            memcpy(out, in, head);
            out[head] = '[';
            memcpy(out + head + 1, dot + 1, len - head - 1);
            out[len] = ']';
            out[len + 1] = '\0';
            return;
        }
    }
    if (n) {
        strncpy(out, in, n - 1);
        out[n - 1] = '\0';
    }
}

const E1KeyDef *e1k_find(const char *name)
{
    char norm[128];
    int i;
    if (!name || !*name) return NULL;
    for (i = 0; i < e1k_key_count; ++i) {
        if (ieq(name, e1k_keys[i].name)) return &e1k_keys[i];
        if (e1k_keys[i].alias && ieq(name, e1k_keys[i].alias)) return &e1k_keys[i];
    }
    normalize_index(name, norm, sizeof norm);
    for (i = 0; i < e1k_key_count; ++i)
        if (ieq(norm, e1k_keys[i].name)) return &e1k_keys[i];
    return NULL;
}

static int parse_double(const char *s, double *out)
{
    char *end = NULL;
    double v;
    while (isspace((unsigned char)*s)) ++s;
    if (!*s) return 0;
    errno = 0;
    v = strtod(s, &end);
    if (end == s || errno == ERANGE) return 0;
    while (isspace((unsigned char)*end)) ++end;
    if (*end != '\0') return 0;
    if (!isfinite(v)) return 0;
    *out = v;
    return 1;
}

int e1k_parse_value(const E1KeyDef *k, const char *text, double *out, char *err, size_t errsz)
{
    double v = 0.0;
    char range[160];
    if (!k || !text || !out) {
        if (err && errsz) snprintf(err, errsz, "internal error");
        return 0;
    }
    e1k_describe_range(k, range, sizeof range);

    if (k->type == E1K_BOOL) {
        if (ieq(text, "1") || ieq(text, "on") || ieq(text, "true") || ieq(text, "yes")) { *out = 1; return 1; }
        if (ieq(text, "0") || ieq(text, "off") || ieq(text, "false") || ieq(text, "no")) { *out = 0; return 1; }
        if (err && errsz) snprintf(err, errsz, "%s: '%s' is not a boolean (use 1/0, on/off)", k->name, text);
        return 0;
    }
    if (k->type == E1K_ENUM) {
        int i;
        for (i = 0; i < k->enum_count; ++i)
            if (ieq(text, k->enum_names[i])) { *out = (double)i; return 1; }
        /* "speakers" is accepted as a synonym of "stereo" */
        if (k->enum_names == k_spk_names && (ieq(text, "speakers") || ieq(text, "speaker"))) { *out = 1; return 1; }
    }
    if (!parse_double(text, &v)) {
        if (err && errsz) snprintf(err, errsz, "%s: '%s' is not a valid value (range %s)", k->name, text, range);
        return 0;
    }
    if (k->type == E1K_ENUM || k->type == E1K_UINT) {
        if (v != floor(v)) {
            if (err && errsz) snprintf(err, errsz, "%s: '%s' must be an integer (range %s)", k->name, text, range);
            return 0;
        }
    }
    if (v < k->minv || v > k->maxv) {
        if (err && errsz) snprintf(err, errsz, "%s: %s is out of range (%s)", k->name, text, range);
        return 0;
    }
    *out = v;
    return 1;
}

double e1k_get(const E1KeyDef *k, const E1Params *p)
{
    const unsigned char *base = (const unsigned char *)p + k->offset;
    if (k->type == E1K_FLOAT) {
        float f;
        memcpy(&f, base, sizeof f);
        return (double)f;
    } else {
        uint32_t u;
        memcpy(&u, base, sizeof u);
        return (double)u;
    }
}

void e1k_set(const E1KeyDef *k, E1Params *p, double v)
{
    unsigned char *base = (unsigned char *)p + k->offset;
    if (k->type == E1K_FLOAT) {
        float f = (float)v;
        memcpy(base, &f, sizeof f);
    } else {
        uint32_t u = (v <= 0.0) ? 0u : (uint32_t)(v + 0.5);
        memcpy(base, &u, sizeof u);
    }
}

void e1k_format(const E1KeyDef *k, const E1Params *p, char *buf, size_t n)
{
    double v;
    if (!buf || !n) return;
    v = e1k_get(k, p);
    switch (k->type) {
    case E1K_BOOL:
        snprintf(buf, n, "%u (%s)", (unsigned)v, v != 0.0 ? "on" : "off");
        break;
    case E1K_ENUM: {
        unsigned u = (unsigned)v;
        snprintf(buf, n, "%u (%s)", u, (int)u < k->enum_count ? k->enum_names[u] : "?");
        break;
    }
    case E1K_UINT:
        snprintf(buf, n, "%u", (unsigned)v);
        break;
    default:
        if (k->unit && k->unit[0]) snprintf(buf, n, "%.2f %s", v, k->unit);
        else snprintf(buf, n, "%.2f", v);
        break;
    }
}

void e1k_describe_range(const E1KeyDef *k, char *buf, size_t n)
{
    if (!buf || !n) return;
    buf[0] = '\0';
    if (k->type == E1K_BOOL) {
        snprintf(buf, n, "0|1");
    } else if (k->type == E1K_ENUM) {
        size_t used = 0;
        int i;
        for (i = 0; i < k->enum_count && used + 1 < n; ++i) {
            int w = snprintf(buf + used, n - used, "%s%s", i ? "|" : "", k->enum_names[i]);
            if (w < 0) break;
            used += (size_t)w;
        }
    } else if (k->type == E1K_UINT) {
        snprintf(buf, n, "%.0f..%.0f", k->minv, k->maxv);
    } else {
        snprintf(buf, n, "%g..%g%s%s", k->minv, k->maxv, (k->unit && k->unit[0]) ? " " : "", k->unit ? k->unit : "");
    }
}

int e1k_find_eq_preset(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < e1_eq_preset_count; ++i)
        if (e1_eq_presets[i].name && e1k_loose_equal(name, e1_eq_presets[i].name)) return i;
    return -1;
}

int e1k_find_vfx_preset(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < e1_vfx_preset_count; ++i)
        if (e1_vfx_presets[i].name && e1k_loose_equal(name, e1_vfx_presets[i].name)) return i;
    return -1;
}

int e1k_apply_eq_preset(E1Params *p, const char *name)
{
    int idx = e1k_find_eq_preset(name), b;
    if (idx < 0) return -1;
    for (b = 0; b < E1FX_EQ_BANDS; ++b) p->render.eq_gain_db[b] = e1_eq_presets[idx].gain_db[b];
    p->render.eq_on = 1;
    p->render.scout_on = 0;
    return idx;
}

int e1k_apply_vfx_preset(E1Params *p, const char *name)
{
    int idx = e1k_find_vfx_preset(name), i;
    if (idx < 0) return -1;
    for (i = 0; i < E1FX_VFX_PARAMS; ++i) p->capture.vfx[i] = e1_vfx_presets[idx].p[i];
    p->capture.vfx_preset = (uint32_t)idx;
    p->capture.vfx_on = 1;
    return idx;
}

/* ---- report helpers ------------------------------------------------------------------- */

void e1k_dump_params(FILE *f, const E1Params *p, const char *prefix)
{
    size_t plen = prefix ? strlen(prefix) : 0;
    int i;
    for (i = 0; i < e1k_key_count; ++i) {
        char val[64];
        if (plen && strncmp(e1k_keys[i].name, prefix, plen) != 0) continue;
        e1k_format(&e1k_keys[i], p, val, sizeof val);
        fprintf(f, "  %-28s = %s\n", e1k_keys[i].name, val);
    }
}

void e1k_format_dbfs(float peak, char *buf, size_t n)
{
    if (!buf || !n) return;
    if (!(peak > 1e-10f) || !isfinite(peak)) snprintf(buf, n, "  -inf");
    else snprintf(buf, n, "%6.1f", 20.0 * log10((double)peak));
}

int e1k_print_status(FILE *f, const char *label, const E1ApoStatus *before,
                     const E1ApoStatus *after, double dt_s)
{
    char i0[16], i1[16], o0[16], o1[16];
    int32_t delta = (int32_t)((uint32_t)after->alive - (uint32_t)before->alive);
    int moving = delta != 0;
    unsigned ch = after->channels;

    fprintf(f, "%s APO\n", label);
    if (!moving) {
        fprintf(f, "  heartbeat     : %ld (idle: no buffers processed in %.1f s; no stream open, or APO not loaded)\n",
                (long)after->alive, dt_s);
    } else {
        fprintf(f, "  heartbeat     : %ld (running, %.0f buffers/s)\n", (long)after->alive,
                dt_s > 0 ? (double)delta / dt_s : 0.0);
    }
    if (after->sample_rate == 0 && after->channels == 0) {
        fprintf(f, "  format        : (not locked yet)\n");
    } else {
        fprintf(f, "  format        : %u Hz, %u ch, float32\n", (unsigned)after->sample_rate, ch);
    }
    fprintf(f, "  processing    : %s\n", after->bypass ? "bit-exact bypass" : "effects active");
    e1k_format_dbfs(after->peak_in[0], i0, sizeof i0);
    e1k_format_dbfs(after->peak_in[1], i1, sizeof i1);
    e1k_format_dbfs(after->peak_out[0], o0, sizeof o0);
    e1k_format_dbfs(after->peak_out[1], o1, sizeof o1);
    if (ch == 1) {
        fprintf(f, "  peak in/out   : %s / %s dBFS\n", i0, o0);
    } else {
        fprintf(f, "  peak in  L/R  : %s / %s dBFS\n", i0, i1);
        fprintf(f, "  peak out L/R  : %s / %s dBFS\n", o0, o1);
    }
    fprintf(f, "  gain reduction: %.1f dB\n", isfinite(after->gain_reduction_db) ? (double)after->gain_reduction_db : 0.0);
    fprintf(f, "  xruns         : %u\n", (unsigned)after->xruns);
    return moving;
}

int e1k_block_compatible(const E1SharedBlock *b)
{
    return b && b->magic == E1FX_MAGIC && b->block_size == (uint32_t)sizeof(E1SharedBlock);
}
