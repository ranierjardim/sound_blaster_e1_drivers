/*
 * e1wavfx.c - offline harness: runs a WAV file through the E1 Studio render or capture chain.
 *
 *   e1wavfx render|capture <in.wav> <out.wav> [params.txt] [options]
 *
 * Options:
 *   --noalign     keep the chain latency in the output (default: latency removed, tail flushed)
 *   --pcm16 / --pcm24   write PCM (TPDF dithered) instead of float32
 *   --block N     buffer size passed to the chain per call (default 441 = Windows 10 ms period)
 *   --set k=v     extra parameter assignment (repeatable, applied after the file)
 *
 * Parameter file: one "key = value" per line, '#' or ';' start a comment. Keys are the field names
 * of E1RenderParams / E1CaptureParams (e.g. master_on, surround_level, eq_gain_db, vfx).
 * Arrays: "eq_gain_db = 0,1,2,0,0,0,0,2,2,2" or "eq_gain_db[3] = 4". Extra keys:
 *   eq_preset = <name or index>   loads a graphic-EQ preset into eq_gain_db
 *   vfx_preset = <index>          loads VoiceFX preset values into vfx[] (and sets vfx_preset)
 * Booleans accept 0/1/on/off/true/false. speaker_mode accepts headphones/stereo, svm_mode
 * normal/loud/night. Defaults: e1fx_params_default (both masters off).
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "../../src/dsp/e1dsp_modules.h"
#include "wav.h"

typedef enum { T_BOOL, T_U32, T_F32, T_F32ARR } FieldType;
typedef struct { const char *name; int capture; size_t offset; FieldType type; int count; } Field;

#define RF(n, t) { #n, 0, offsetof(E1RenderParams, n), t, 1 }
#define CF(n, t) { #n, 1, offsetof(E1CaptureParams, n), t, 1 }
static const Field k_fields[] = {
    RF(master_on, T_BOOL), RF(surround_on, T_BOOL), RF(surround_level, T_F32), RF(crystal_on, T_BOOL),
    RF(crystal_level, T_F32), RF(bass_on, T_BOOL), RF(bass_level, T_F32), RF(bass_xover_hz, T_F32),
    RF(svm_on, T_BOOL), RF(svm_level, T_F32), RF(svm_mode, T_U32), RF(dialog_on, T_BOOL), RF(dialog_level, T_F32),
    RF(eq_on, T_BOOL), RF(eq_preamp_db, T_F32),
    { "eq_gain_db", 0, offsetof(E1RenderParams, eq_gain_db), T_F32ARR, E1FX_EQ_BANDS },
    RF(speaker_mode, T_U32), RF(scout_on, T_BOOL), RF(output_gain_db, T_F32), RF(limiter_on, T_BOOL),
    CF(master_on, T_BOOL), CF(nr_on, T_BOOL), CF(nr_strength, T_F32), CF(svm_on, T_BOOL), CF(svm_level, T_F32),
    CF(vfx_on, T_BOOL), CF(vfx_preset, T_U32),
    { "vfx", 1, offsetof(E1CaptureParams, vfx), T_F32ARR, E1FX_VFX_PARAMS },
    CF(output_gain_db, T_F32),
};

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int parse_num(const char *v, double *out)
{
    if (!_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes")) { *out = 1; return 1; }
    if (!_stricmp(v, "off") || !_stricmp(v, "false") || !_stricmp(v, "no")) { *out = 0; return 1; }
    if (!_stricmp(v, "headphones")) { *out = E1FX_SPK_HEADPHONES; return 1; }
    if (!_stricmp(v, "stereo") || !_stricmp(v, "speakers")) { *out = E1FX_SPK_STEREO; return 1; }
    if (!_stricmp(v, "normal")) { *out = E1FX_SVM_NORMAL; return 1; }
    if (!_stricmp(v, "loud")) { *out = E1FX_SVM_LOUD; return 1; }
    if (!_stricmp(v, "night")) { *out = E1FX_SVM_NIGHT; return 1; }
    char *end;
    *out = strtod(v, &end);
    return end != v && *trim(end) == 0;
}

static void store(void *base, const Field *f, int idx, double v)
{
    char *p = (char *)base + f->offset;
    if (f->type == T_BOOL || f->type == T_U32) *(uint32_t *)p = (uint32_t)(v < 0 ? 0 : v);
    else ((float *)p)[idx] = (float)v;
}

/* Applies one "key=value" assignment; capture_ctx selects which struct shared names refer to. */
static int apply_kv(E1Params *P, int capture_ctx, char *key, char *val)
{
    key = trim(key);
    val = trim(val);
    if (!_stricmp(key, "eq_preset")) {
        double d;
        int idx = -1;
        if (parse_num(val, &d)) idx = (int)d;
        else
            for (int i = 0; i < e1_eq_preset_count; ++i)
                if (!_stricmp(val, e1_eq_presets[i].name)) idx = i;
        if (idx < 0 || idx >= e1_eq_preset_count) { fprintf(stderr, "unknown EQ preset '%s'\n", val); return 0; }
        memcpy(P->render.eq_gain_db, e1_eq_presets[idx].gain_db, sizeof P->render.eq_gain_db);
        return 1;
    }
    if (!_stricmp(key, "vfx_preset")) {
        double d;
        if (!parse_num(val, &d) || d < 0 || (int)d >= e1_vfx_preset_count) { fprintf(stderr, "bad vfx_preset '%s'\n", val); return 0; }
        memcpy(P->capture.vfx, e1_vfx_presets[(int)d].p, sizeof P->capture.vfx);
        P->capture.vfx_preset = (uint32_t)d;
        return 1;
    }
    int idx = -1;
    char *br = strchr(key, '[');
    if (br) {
        idx = atoi(br + 1);
        *br = 0;
        key = trim(key);
    }
    const Field *f = NULL;
    for (size_t i = 0; i < sizeof k_fields / sizeof k_fields[0]; ++i)
        if (!_stricmp(k_fields[i].name, key) && k_fields[i].capture == capture_ctx) f = &k_fields[i];
    if (!f)
        for (size_t i = 0; i < sizeof k_fields / sizeof k_fields[0]; ++i)
            if (!_stricmp(k_fields[i].name, key)) f = &k_fields[i];
    if (!f) { fprintf(stderr, "unknown key '%s'\n", key); return 0; }
    void *base = f->capture ? (void *)&P->capture : (void *)&P->render;
    if (f->type == T_F32ARR && idx < 0) {
        int n = 0;
        for (char *tok = strtok(val, ", \t"); tok && n < f->count; tok = strtok(NULL, ", \t"), ++n) {
            double d;
            if (!parse_num(tok, &d)) { fprintf(stderr, "bad number '%s' for %s\n", tok, key); return 0; }
            store(base, f, n, d);
        }
        if (n != f->count) { fprintf(stderr, "%s needs %d values\n", key, f->count); return 0; }
        return 1;
    }
    if (idx >= f->count || (idx >= 0 && f->type != T_F32ARR)) { fprintf(stderr, "bad index for %s\n", key); return 0; }
    double d;
    if (!parse_num(val, &d)) { fprintf(stderr, "bad value '%s' for %s\n", val, key); return 0; }
    store(base, f, idx < 0 ? 0 : idx, d);
    return 1;
}

static int load_params(E1Params *P, int capture_ctx, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open parameter file %s\n", path); return 0; }
    char line[1024];
    int ln = 0, ok = 1;
    while (fgets(line, sizeof line, f)) {
        ++ln;
        char *c = strpbrk(line, "#;");
        if (c) *c = 0;
        char *s = trim(line);
        if (!*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) { fprintf(stderr, "%s:%d: expected key=value\n", path, ln); ok = 0; continue; }
        *eq = 0;
        if (!apply_kv(P, capture_ctx, s, eq + 1)) { fprintf(stderr, "%s:%d: rejected\n", path, ln); ok = 0; }
    }
    fclose(f);
    return ok;
}

static void usage(void)
{
    fprintf(stderr, "usage: e1wavfx render|capture <in.wav> <out.wav> [params.txt] [--noalign] [--pcm16|--pcm24] [--block N] [--set key=value]...\n");
}

int main(int argc, char **argv)
{
    if (argc < 4) { usage(); return 2; }
    int capture = !_stricmp(argv[1], "capture");
    if (!capture && _stricmp(argv[1], "render")) { usage(); return 2; }
    const char *inp = argv[2], *outp = argv[3];
    int align = 1, fmt = WAV_F32;
    size_t block = 441;
    E1Params P;
    e1fx_params_default(&P);
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--noalign")) align = 0;
        else if (!strcmp(argv[i], "--pcm16")) fmt = WAV_PCM16;
        else if (!strcmp(argv[i], "--pcm24")) fmt = WAV_PCM24;
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) { block = (size_t)atol(argv[++i]); if (!block) block = 1; }
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s", argv[++i]);
            char *eq = strchr(buf, '=');
            if (!eq) { usage(); return 2; }
            *eq = 0;
            if (!apply_kv(&P, capture, buf, eq + 1)) return 2;
        } else if (argv[i][0] != '-') {
            if (!load_params(&P, capture, argv[i])) return 2;
        } else { usage(); return 2; }
    }
    e1fx_params_sanitize(&P);

    WavData w;
    if (wav_read(inp, &w) != 0) return 1;
    if (w.channels > E1DSP_MAX_CH) { fprintf(stderr, "too many channels (%d)\n", w.channels); wav_free(&w); return 1; }
    const int nch = w.channels;
    E1Render *r = NULL;
    E1Capture *c = NULL;
    unsigned lat;
    if (capture) {
        c = e1_capture_create(w.rate, nch);
        if (!c) { fprintf(stderr, "unsupported format %d Hz / %d ch\n", w.rate, nch); wav_free(&w); return 1; }
        e1_capture_set_params(c, &P.capture);
        lat = e1_capture_latency_frames(c);
    } else {
        r = e1_render_create(w.rate, nch);
        if (!r) { fprintf(stderr, "unsupported format %d Hz / %d ch\n", w.rate, nch); wav_free(&w); return 1; }
        e1_render_set_params(r, &P.render);
        lat = e1_render_latency_frames(r);
    }
    /* Pad with `lat` frames of silence to flush the tail. An active chain delays by exactly
       `lat`; a chain that stayed in bypass is zero latency. */
    size_t total = w.frames + lat;
    float *buf = (float *)calloc(total * (size_t)nch + 1, sizeof(float));
    if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }
    memcpy(buf, w.data, w.frames * (size_t)nch * sizeof(float));
    int ever_active = 0;
    for (size_t off = 0; off < total; off += block) {
        size_t n = block < total - off ? block : total - off;
        if (capture) e1_capture_process(c, buf + off * nch, buf + off * nch, n);
        else e1_render_process(r, buf + off * nch, buf + off * nch, n);
        ever_active |= capture ? !e1_capture_is_bypassed(c) : !e1_render_is_bypassed(r);
    }
    const size_t extra = (align && ever_active) ? lat : 0;
    float pi[2], po[2], gr;
    if (capture) e1_capture_meter(c, pi, po, &gr);
    else e1_render_meter(r, pi, po, &gr);
    double pk = 0;
    for (size_t i = extra * nch; i < total * (size_t)nch; ++i) pk = fmax(pk, fabs(buf[i]));
    int ret = wav_write(outp, buf + extra * nch, w.frames, nch, w.rate, fmt) == 0 ? 0 : 1;
    printf("%s chain: %zu frames, %d Hz, %d ch (source %d-bit%s), latency %u frames (%.2f ms)%s%s\n",
           capture ? "capture" : "render", w.frames, w.rate, nch, w.bits, w.is_float ? " float" : "", lat,
           1000.0 * lat / w.rate, ever_active ? "" : ", bit-exact bypass", extra ? ", latency removed" : "");
    printf("output peak %.2f dBFS, last limiter gain reduction %.2f dB -> %s\n",
           20 * log10(pk > 1e-12 ? pk : 1e-12), gr, outp);
    free(buf);
    wav_free(&w);
    if (r) e1_render_destroy(r);
    if (c) e1_capture_destroy(c);
    return ret;
}
