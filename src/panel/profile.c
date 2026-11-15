/*
 * profile.c - profile XML writer/reader (small tolerant parser, no MSXML dependency), factory
 * profiles and the profile folder helpers.
 *
 * Schema (docs/RECONSTRUCTION_SPEC.md 4, re/control-panel.md 6):
 *   <profile><info><avatar/><profile_name/><profile_id/><version/><device_id/></info>
 *   <settings><surround enable level/><crystalizer enable level/><bass enable level cross_freq/>
 *   <svm enable level mode/><dlgplus enable level/><voicefx enable preset/><vip_svm enable level/>
 *   <noise_reduction enable/><graphic_eq enable preset preamp band0..band9/>
 *   <e1studio .../>   (our extension; unknown elements are ignored by every reader)
 *   </settings></profile>
 * Loader rule: a float level <= 1 is scaled by (max - min) of the live range; > 1 is used as is.
 * Only Bass (0..100) is affected, so bass is written normalised (value / 100).
 */
#include "profile.h"
#include "../dsp/e1dsp_modules.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

/* ---------------------------------------------------------------------------------------------- */
/* string buffer                                                                                    */

typedef struct SBuf { char *p; size_t n, cap; int oom; } SBuf;

static void sb_put(SBuf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + n + 1) nc *= 2;
        char *q = (char *)realloc(b->p, nc);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_str(SBuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_fmt(SBuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(b, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}

static void sb_escaped(SBuf *b, const char *s)
{
    for (; *s; ++s) {
        switch (*s) {
        case '&': sb_str(b, "&amp;"); break;
        case '<': sb_str(b, "&lt;"); break;
        case '>': sb_str(b, "&gt;"); break;
        case '"': sb_str(b, "&quot;"); break;
        case '\'': sb_str(b, "&apos;"); break;
        default:
            if ((unsigned char)*s < 0x20 && *s != '\t' && *s != '\n' && *s != '\r') break;
            sb_put(b, s, 1);
        }
    }
}

/* Locale-independent float formatting: up to `dec` decimals, trailing zeros trimmed. */
static void fmt_float(char *out, size_t cap, double v, int dec)
{
    if (!isfinite(v)) v = 0;
    snprintf(out, cap, "%.*f", dec, v);
    char *dot = strchr(out, '.');
    if (dot) {
        char *e = out + strlen(out) - 1;
        while (e > dot && *e == '0') *e-- = 0;
        if (e == dot) *e = 0;
    }
    if (strcmp(out, "-0") == 0) { out[0] = '0'; out[1] = 0; }
}

static void w2u(const wchar_t *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}

static void u2w(const char *s, wchar_t *out, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) {
        out[0] = 0;
        /* truncate safely: convert as much as fits */
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
        if (n > cap) {
            wchar_t *tmp = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
            if (tmp) {
                MultiByteToWideChar(CP_UTF8, 0, s, -1, tmp, n);
                lstrcpynW(out, tmp, cap);
                free(tmp);
            }
        }
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* writer                                                                                           */

static const char *tf(uint32_t b) { return b ? "true" : "false"; }

char *profile_to_xml(const E1Profile *pr, size_t *len_out)
{
    SBuf b = {0};
    char name[PROFILE_NAME_MAX * 4], avatar[256], f1[48], f2[48], f3[48];
    const E1RenderParams *r = &pr->params.render;
    const E1CaptureParams *c = &pr->params.capture;

    w2u(pr->name, name, sizeof name);
    w2u(pr->avatar, avatar, sizeof avatar);

    sb_str(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<profile>\r\n  <info>\r\n");
    sb_str(&b, "    <avatar>"); sb_escaped(&b, avatar); sb_str(&b, "</avatar>\r\n");
    sb_str(&b, "    <profile_name>"); sb_escaped(&b, name); sb_str(&b, "</profile_name>\r\n");
    sb_fmt(&b, "    <profile_id>%d</profile_id>\r\n", pr->profile_id);
    sb_str(&b, "    <version>2.0</version>\r\n");
    sb_str(&b, "    <device_id>" E1_DEVICE_ID_STR "</device_id>\r\n");
    sb_str(&b, "  </info>\r\n  <settings>\r\n");

    fmt_float(f1, sizeof f1, r->surround_level, 4);
    sb_fmt(&b, "    <surround enable=\"%s\" level=\"%s\"/>\r\n", tf(r->surround_on), f1);
    fmt_float(f1, sizeof f1, r->crystal_level, 4);
    sb_fmt(&b, "    <crystalizer enable=\"%s\" level=\"%s\"/>\r\n", tf(r->crystal_on), f1);
    fmt_float(f1, sizeof f1, r->bass_level / 100.0, 4);
    fmt_float(f2, sizeof f2, r->bass_xover_hz, 1);
    sb_fmt(&b, "    <bass enable=\"%s\" level=\"%s\" cross_freq=\"%s\"/>\r\n", tf(r->bass_on), f1, f2);
    fmt_float(f1, sizeof f1, r->svm_level, 4);
    sb_fmt(&b, "    <svm enable=\"%s\" level=\"%s\" mode=\"%u\"/>\r\n", tf(r->svm_on), f1, (unsigned)r->svm_mode);
    fmt_float(f1, sizeof f1, r->dialog_level, 4);
    sb_fmt(&b, "    <dlgplus enable=\"%s\" level=\"%s\"/>\r\n", tf(r->dialog_on), f1);
    sb_fmt(&b, "    <voicefx enable=\"%s\" preset=\"%u\"/>\r\n", tf(c->vfx_on), (unsigned)c->vfx_preset + 1u);
    fmt_float(f1, sizeof f1, c->svm_level, 4);
    sb_fmt(&b, "    <vip_svm enable=\"%s\" level=\"%s\"/>\r\n", tf(c->svm_on), f1);
    sb_fmt(&b, "    <noise_reduction enable=\"%s\"/>\r\n", tf(c->nr_on));
    fmt_float(f1, sizeof f1, r->eq_preamp_db, 2);
    sb_fmt(&b, "    <graphic_eq enable=\"%s\" preset=\"%d\" preamp=\"%s\"", tf(r->eq_on), pr->eq_preset_id, f1);
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) {
        fmt_float(f1, sizeof f1, r->eq_gain_db[i], 2);
        sb_fmt(&b, " band%d=\"%s\"", i, f1);
    }
    sb_str(&b, "/>\r\n");
    fmt_float(f1, sizeof f1, r->output_gain_db, 2);
    fmt_float(f2, sizeof f2, c->nr_strength, 4);
    fmt_float(f3, sizeof f3, c->output_gain_db, 2);
    /* Speaker/headphone mode is a property of what is plugged in, not of a sound profile, so it is
       deliberately not stored (the original panel did not store it either). */
    sb_fmt(&b, "    <e1studio master=\"%s\" voice_master=\"%s\" scout=\"%s\" "
               "output_gain_db=\"%s\" limiter=\"%s\" nr_strength=\"%s\" voice_output_gain_db=\"%s\"",
           tf(r->master_on), tf(c->master_on), tf(r->scout_on), f1, tf(r->limiter_on), f2, f3);
    sb_str(&b, " vfx=\"");
    for (int i = 0; i < E1FX_VFX_PARAMS; ++i) {
        fmt_float(f1, sizeof f1, c->vfx[i], 5);
        if (i) sb_str(&b, ",");
        sb_str(&b, f1);
    }
    sb_str(&b, "\"/>\r\n  </settings>\r\n</profile>\r\n");

    if (b.oom) { free(b.p); return NULL; }
    if (len_out) *len_out = b.n;
    return b.p;
}

/* ---------------------------------------------------------------------------------------------- */
/* tolerant XML event parser                                                                        */

#define XML_MAX_ATTR  40
#define XML_MAX_DEPTH 16
#define XML_NAME      48
#define XML_VAL       512

typedef struct XAttr { char name[XML_NAME]; char value[XML_VAL]; } XAttr;

typedef struct XParser {
    const char *p, *end;
    char  stack[XML_MAX_DEPTH][XML_NAME];
    int   depth;
    /* callbacks */
    void (*on_start)(void *ctx, int depth, char stack[][XML_NAME], const XAttr *a, int na);
    void (*on_text)(void *ctx, int depth, char stack[][XML_NAME], const char *text);
    void *ctx;
} XParser;

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_name(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':' || (unsigned char)c >= 0x80; }

static void put_utf8(char **o, char *oend, unsigned cp)
{
    char tmp[4];
    int n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    for (int i = 0; i < n && *o < oend; ++i) *(*o)++ = tmp[i];
}

/* Decodes [s, e) with entities into out (cap incl. NUL). */
static void xml_decode(const char *s, const char *e, char *out, size_t cap)
{
    char *o = out, *oend = out + cap - 1;
    while (s < e && o < oend) {
        if (*s == '&') {
            const char *semi = s;
            while (semi < e && *semi != ';' && semi - s < 12) ++semi;
            if (semi < e && *semi == ';') {
                size_t n = (size_t)(semi - s - 1);
                const char *nm = s + 1;
                unsigned cp = 0;
                int ok = 1;
                if (n == 3 && !strncmp(nm, "amp", 3)) cp = '&';
                else if (n == 2 && !strncmp(nm, "lt", 2)) cp = '<';
                else if (n == 2 && !strncmp(nm, "gt", 2)) cp = '>';
                else if (n == 4 && !strncmp(nm, "quot", 4)) cp = '"';
                else if (n == 4 && !strncmp(nm, "apos", 4)) cp = '\'';
                else if (n >= 2 && nm[0] == '#') {
                    char num[16] = {0};
                    memcpy(num, nm + 1, n - 1 < 15 ? n - 1 : 15);
                    cp = (num[0] == 'x' || num[0] == 'X') ? (unsigned)strtoul(num + 1, NULL, 16) : (unsigned)strtoul(num, NULL, 10);
                    if (cp == 0 || cp > 0x10FFFF) ok = 0;
                } else ok = 0;
                if (ok) {
                    put_utf8(&o, oend, cp);
                    s = semi + 1;
                    continue;
                }
            }
        }
        *o++ = *s++;
    }
    *o = 0;
}

static const char *find_str(const char *p, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    for (; p + n <= end; ++p)
        if (!memcmp(p, needle, n)) return p;
    return NULL;
}

/* Returns 1 if the document parsed without structural errors. */
static int xml_parse(XParser *x)
{
    const char *text_start = NULL;
    while (x->p < x->end) {
        if (*x->p != '<') {
            if (!text_start) text_start = x->p;
            ++x->p;
            continue;
        }
        /* deliver text accumulated before this tag */
        if (text_start && x->depth > 0 && x->on_text) {
            const char *a = text_start, *b = x->p;
            while (a < b && is_ws(*a)) ++a;
            while (b > a && is_ws(b[-1])) --b;
            if (b > a) {
                char tmp[XML_VAL];
                xml_decode(a, b, tmp, sizeof tmp);
                x->on_text(x->ctx, x->depth, x->stack, tmp);
            }
        }
        text_start = NULL;
        const char *q = x->p + 1;
        if (q < x->end && *q == '?') {
            const char *e = find_str(q, x->end, "?>");
            if (!e) return 0;
            x->p = e + 2;
        } else if (x->end - q >= 3 && !memcmp(q, "!--", 3)) {
            const char *e = find_str(q + 3, x->end, "-->");
            if (!e) return 0;
            x->p = e + 3;
        } else if (x->end - q >= 8 && !memcmp(q, "![CDATA[", 8)) {
            const char *e = find_str(q + 8, x->end, "]]>");
            if (!e) return 0;
            if (x->depth > 0 && x->on_text) {
                char tmp[XML_VAL];
                size_t n = (size_t)(e - (q + 8));
                if (n >= sizeof tmp) n = sizeof tmp - 1;
                memcpy(tmp, q + 8, n);
                tmp[n] = 0;
                x->on_text(x->ctx, x->depth, x->stack, tmp);
            }
            x->p = e + 3;
        } else if (q < x->end && *q == '!') {
            const char *e = memchr(q, '>', (size_t)(x->end - q));
            if (!e) return 0;
            x->p = e + 1;
        } else if (q < x->end && *q == '/') {
            const char *e = memchr(q, '>', (size_t)(x->end - q));
            if (!e || x->depth <= 0) return 0;
            --x->depth;
            x->p = e + 1;
        } else {
            XAttr *attrs = (XAttr *)malloc(sizeof(XAttr) * XML_MAX_ATTR);
            if (!attrs) return 0;
            int na = 0;
            char name[XML_NAME];
            size_t nl = 0;
            while (q < x->end && is_name(*q)) {
                if (nl + 1 < sizeof name) name[nl++] = *q;
                ++q;
            }
            name[nl] = 0;
            if (!nl) { free(attrs); return 0; }
            int self_close = 0;
            for (;;) {
                while (q < x->end && is_ws(*q)) ++q;
                if (q >= x->end) { free(attrs); return 0; }
                if (*q == '>') { ++q; break; }
                if (*q == '/' && q + 1 < x->end && q[1] == '>') { self_close = 1; q += 2; break; }
                char an[XML_NAME];
                size_t al = 0;
                while (q < x->end && is_name(*q)) {
                    if (al + 1 < sizeof an) an[al++] = *q;
                    ++q;
                }
                an[al] = 0;
                if (!al) { free(attrs); return 0; }
                while (q < x->end && is_ws(*q)) ++q;
                if (q >= x->end || *q != '=') { free(attrs); return 0; }
                ++q;
                while (q < x->end && is_ws(*q)) ++q;
                if (q >= x->end || (*q != '"' && *q != '\'')) { free(attrs); return 0; }
                char quote = *q++;
                const char *vs = q;
                while (q < x->end && *q != quote) ++q;
                if (q >= x->end) { free(attrs); return 0; }
                if (na < XML_MAX_ATTR) {
                    memcpy(attrs[na].name, an, al + 1);
                    xml_decode(vs, q, attrs[na].value, sizeof attrs[na].value);
                    ++na;
                }
                ++q;
            }
            if (x->depth >= XML_MAX_DEPTH) { free(attrs); return 0; }
            memcpy(x->stack[x->depth], name, nl + 1);
            ++x->depth;
            if (x->on_start) x->on_start(x->ctx, x->depth, x->stack, attrs, na);
            if (self_close) --x->depth;
            free(attrs);
            x->p = q;
        }
    }
    return x->depth == 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* reader                                                                                           */

typedef struct PCtx {
    E1Profile *out;
    int        root_ok;
    int        saw_settings;
} PCtx;

static const char *attr(const XAttr *a, int na, const char *name)
{
    for (int i = 0; i < na; ++i)
        if (!_stricmp(a[i].name, name)) return a[i].value;
    return NULL;
}

static int parse_bool(const char *v, uint32_t *out)
{
    if (!v) return 0;
    while (is_ws(*v)) ++v;
    if (!_stricmp(v, "true") || !strcmp(v, "1") || !_stricmp(v, "yes") || !_stricmp(v, "on")) { *out = 1; return 1; }
    if (!_stricmp(v, "false") || !strcmp(v, "0") || !_stricmp(v, "no") || !_stricmp(v, "off")) { *out = 0; return 1; }
    return 0;
}

static int parse_float(const char *v, float *out)
{
    if (!v) return 0;
    char *e = NULL;
    double d = strtod(v, &e);
    if (e == v || !isfinite(d)) return 0;
    *out = (float)d;
    return 1;
}

/* Creative loader rule for levels on a parameter with live range [lo, hi]. */
static int parse_level(const char *v, float lo, float hi, float *out)
{
    float f;
    if (!parse_float(v, &f)) return 0;
    if (f <= 1.0f) f = lo + f * (hi - lo);
    *out = f;
    return 1;
}

static void on_start(void *ctx_, int depth, char stack[][XML_NAME], const XAttr *a, int na)
{
    PCtx *ctx = (PCtx *)ctx_;
    E1RenderParams *r = &ctx->out->params.render;
    E1CaptureParams *c = &ctx->out->params.capture;
    const char *el = stack[depth - 1];
    if (depth == 1) {
        ctx->root_ok = !_stricmp(el, "profile");
        return;
    }
    if (!ctx->root_ok || depth != 3 || _stricmp(stack[1], "settings")) return;
    ctx->saw_settings = 1;
    uint32_t b;
    float f;
    if (!_stricmp(el, "surround")) {
        if (parse_bool(attr(a, na, "enable"), &b)) r->surround_on = b;
        if (parse_level(attr(a, na, "level"), 0, 1, &f)) r->surround_level = f;
    } else if (!_stricmp(el, "crystalizer")) {
        if (parse_bool(attr(a, na, "enable"), &b)) r->crystal_on = b;
        if (parse_level(attr(a, na, "level"), 0, 1, &f)) r->crystal_level = f;
    } else if (!_stricmp(el, "bass")) {
        if (parse_bool(attr(a, na, "enable"), &b)) r->bass_on = b;
        if (parse_level(attr(a, na, "level"), 0, 100, &f)) r->bass_level = f;
        if (parse_float(attr(a, na, "cross_freq"), &f)) r->bass_xover_hz = f;
    } else if (!_stricmp(el, "svm")) {
        if (parse_bool(attr(a, na, "enable"), &b)) r->svm_on = b;
        if (parse_level(attr(a, na, "level"), 0, 1, &f)) r->svm_level = f;
        if (parse_float(attr(a, na, "mode"), &f) && f >= 0 && f <= 2) r->svm_mode = (uint32_t)(f + 0.5f);
    } else if (!_stricmp(el, "dlgplus")) {
        if (parse_bool(attr(a, na, "enable"), &b)) r->dialog_on = b;
        if (parse_level(attr(a, na, "level"), 0, 1, &f)) r->dialog_level = f;
    } else if (!_stricmp(el, "voicefx")) {
        if (parse_bool(attr(a, na, "enable"), &b)) c->vfx_on = b;
        if (parse_float(attr(a, na, "preset"), &f) && f >= 1 && f <= (float)e1_vfx_preset_count) {
            int idx = (int)(f + 0.5f) - 1;
            c->vfx_preset = (uint32_t)idx;
            for (int i = 0; i < E1FX_VFX_PARAMS; ++i) c->vfx[i] = e1_vfx_presets[idx].p[i];
        }
    } else if (!_stricmp(el, "vip_svm")) {
        if (parse_bool(attr(a, na, "enable"), &b)) c->svm_on = b;
        if (parse_level(attr(a, na, "level"), 0, 1, &f)) c->svm_level = f;
    } else if (!_stricmp(el, "noise_reduction")) {
        if (parse_bool(attr(a, na, "enable"), &b)) c->nr_on = b;
    } else if (!_stricmp(el, "graphic_eq")) {
        if (parse_bool(attr(a, na, "enable"), &b)) r->eq_on = b;
        if (parse_float(attr(a, na, "preamp"), &f)) r->eq_preamp_db = f;
        if (parse_float(attr(a, na, "preset"), &f) && f >= 0 && f < 1000) ctx->out->eq_preset_id = (int)(f + 0.5f);
        for (int i = 0; i < E1FX_EQ_BANDS; ++i) {
            char nm[16];
            snprintf(nm, sizeof nm, "band%d", i);
            if (parse_float(attr(a, na, nm), &f)) r->eq_gain_db[i] = f;
        }
    } else if (!_stricmp(el, "e1studio")) {
        const char *v;
        ctx->out->has_ext = 1;
        if (parse_bool(attr(a, na, "master"), &b)) r->master_on = b;
        if (parse_bool(attr(a, na, "voice_master"), &b)) c->master_on = b;
        if (parse_bool(attr(a, na, "scout"), &b)) r->scout_on = b;
        if (parse_float(attr(a, na, "output_gain_db"), &f)) r->output_gain_db = f;
        if (parse_bool(attr(a, na, "limiter"), &b)) r->limiter_on = b;
        if (parse_float(attr(a, na, "nr_strength"), &f)) c->nr_strength = f;
        if (parse_float(attr(a, na, "voice_output_gain_db"), &f)) c->output_gain_db = f;
        if ((v = attr(a, na, "vfx")) != NULL) {
            float tmp[E1FX_VFX_PARAMS];
            int n = 0;
            const char *s = v;
            while (n < E1FX_VFX_PARAMS && *s) {
                char *e = NULL;
                double d = strtod(s, &e);
                if (e == s || !isfinite(d)) break;
                tmp[n++] = (float)d;
                s = e;
                while (*s == ',' || is_ws(*s)) ++s;
            }
            if (n == E1FX_VFX_PARAMS) memcpy(c->vfx, tmp, sizeof tmp);
        }
    }
}

static void on_text(void *ctx_, int depth, char stack[][XML_NAME], const char *text)
{
    PCtx *ctx = (PCtx *)ctx_;
    if (!ctx->root_ok || depth != 3 || _stricmp(stack[1], "info")) return;
    const char *el = stack[2];
    if (!_stricmp(el, "profile_name")) u2w(text, ctx->out->name, PROFILE_NAME_MAX);
    else if (!_stricmp(el, "avatar")) u2w(text, ctx->out->avatar, 64);
    else if (!_stricmp(el, "profile_id")) ctx->out->profile_id = atoi(text);
}

int profile_from_xml(const char *xml, size_t len, const E1Params *base, E1Profile *out)
{
    char *conv = NULL;
    memset(out, 0, sizeof *out);
    if (base) out->params = *base;
    else e1fx_params_default(&out->params);
    out->profile_id = 100;

    if (len >= 2 && (unsigned char)xml[0] == 0xFF && (unsigned char)xml[1] == 0xFE) {
        int wl = (int)((len - 2) / 2);
        int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(xml + 2), wl, NULL, 0, NULL, NULL);
        conv = (char *)malloc((size_t)n + 1);
        if (!conv) return 0;
        WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(xml + 2), wl, conv, n, NULL, NULL);
        conv[n] = 0;
        xml = conv;
        len = (size_t)n;
    } else if (len >= 3 && (unsigned char)xml[0] == 0xEF && (unsigned char)xml[1] == 0xBB && (unsigned char)xml[2] == 0xBF) {
        xml += 3;
        len -= 3;
    }

    PCtx ctx = {out, 0, 0};
    XParser *x = (XParser *)calloc(1, sizeof *x);
    if (!x) { free(conv); return 0; }
    x->p = xml;
    x->end = xml + len;
    x->on_start = on_start;
    x->on_text = on_text;
    x->ctx = &ctx;
    int ok = xml_parse(x) && ctx.root_ok;
    free(x);
    free(conv);
    if (!ok) return 0;
    out->params.version = E1FX_PARAMS_VERSION;
    out->params.size = sizeof(E1Params);
    e1fx_params_sanitize(&out->params);
    return 1;
}

/* ---------------------------------------------------------------------------------------------- */
/* files                                                                                            */

char *e1_read_file(const wchar_t *path, size_t *len_out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    char *buf = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart <= 4 * 1024 * 1024) {
        DWORD n = (DWORD)sz.QuadPart, rd = 0;
        buf = (char *)malloc((size_t)n + 2);
        if (buf && ReadFile(h, buf, n, &rd, NULL) && rd == n) {
            buf[n] = 0;
            buf[n + 1] = 0;
            if (len_out) *len_out = n;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    CloseHandle(h);
    return buf;
}

int e1_write_file_atomic(const wchar_t *path, const void *data, size_t len)
{
    wchar_t tmp[MAX_PATH + 16];
    if (lstrlenW(path) >= MAX_PATH) return 0;
    lstrcpyW(tmp, path);
    lstrcatW(tmp, L".tmp");
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

int profile_save(const wchar_t *path, const E1Profile *pr)
{
    size_t n = 0;
    char *x = profile_to_xml(pr, &n);
    if (!x) return 0;
    int ok = e1_write_file_atomic(path, x, n);
    free(x);
    return ok;
}

int profile_load(const wchar_t *path, const E1Params *base, E1Profile *out)
{
    size_t n = 0;
    char *x = e1_read_file(path, &n);
    if (!x) return 0;
    int ok = profile_from_xml(x, n, base, out);
    free(x);
    return ok;
}

int app_data_dir(wchar_t *out, size_t cch, int create)
{
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, NULL, &base)) || !base) return 0;
    int ok = (size_t)lstrlenW(base) + 12 < cch;
    if (ok) {
        lstrcpyW(out, base);
        lstrcatW(out, L"\\E1Studio");
        if (create && !CreateDirectoryW(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ok = 0;
    }
    CoTaskMemFree(base);
    return ok;
}

int profile_dir(wchar_t *out, size_t cch, int create)
{
    if (!app_data_dir(out, cch, create)) return 0;
    if ((size_t)lstrlenW(out) + 10 >= cch) return 0;
    lstrcatW(out, L"\\profiles");
    if (create && !CreateDirectoryW(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    return 1;
}

int e1_name_valid(const wchar_t *name)
{
    int n = 0;
    if (!name) return 0;
    for (const wchar_t *p = name; *p; ++p, ++n) {
        if (*p < 0x20 || wcschr(L"\\/:*?\"<>|[]", *p)) return 0;
    }
    if (n < 1 || n > 32) return 0;
    /* not only blanks or dots */
    for (const wchar_t *p = name; *p; ++p)
        if (*p != L' ' && *p != L'.') return 1;
    return 0;
}

void profile_filename(const wchar_t *name, wchar_t *out, size_t cch)
{
    size_t o = 0;
    for (const wchar_t *p = name; *p && o + 5 < cch; ++p)
        out[o++] = (*p < 0x20 || wcschr(L"\\/:*?\"<>|", *p)) ? L'_' : *p;
    while (o > 0 && (out[o - 1] == L' ' || out[o - 1] == L'.')) --o;
    if (o == 0 && cch > 9) { lstrcpyW(out, L"profile"); o = 7; }
    out[o] = 0;
    lstrcatW(out, L".xml");
}

static int cmp_entry(const void *a, const void *b)
{
    return StrCmpLogicalW(((const ProfileEntry *)a)->name, ((const ProfileEntry *)b)->name);
}

int profile_list(const wchar_t *dir, ProfileEntry *out, int max)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    int n = 0;
    if (lstrlenW(dir) + 8 >= MAX_PATH) return 0;
    lstrcpyW(pat, dir);
    lstrcatW(pat, L"\\*.xml");
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n >= max) break;
        wchar_t path[MAX_PATH];
        if (lstrlenW(dir) + lstrlenW(fd.cFileName) + 2 >= MAX_PATH) continue;
        lstrcpyW(path, dir);
        lstrcatW(path, L"\\");
        lstrcatW(path, fd.cFileName);
        E1Profile *pr = (E1Profile *)malloc(sizeof *pr);
        if (!pr) break;
        if (profile_load(path, NULL, pr)) {
            if (!pr->name[0]) {
                lstrcpynW(pr->name, fd.cFileName, PROFILE_NAME_MAX);
                wchar_t *dot = wcsrchr(pr->name, L'.');
                if (dot) *dot = 0;
            }
            lstrcpynW(out[n].name, pr->name, PROFILE_NAME_MAX);
            lstrcpyW(out[n].path, path);
            ++n;
        }
        free(pr);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(out, (size_t)n, sizeof *out, cmp_entry);
    return n;
}

/* ---------------------------------------------------------------------------------------------- */
/* factory profiles (docs/re/control-panel.md 6, Win10 values)                                       */

typedef struct FactoryDef {
    const wchar_t *name;
    const wchar_t *avatar;
    int   id;
    int   sur_on;  float sur;
    int   cry_on;  float cry;
    int   bass_on; float bass;    /* normalised 0..1 */
    int   svm_on;  float svm;
    int   dlg_on;  float dlg;
    int   vsvm_on; float vsvm;
    int   nr_on;
    float eq[E1FX_EQ_BANDS];
} FactoryDef;

static const FactoryDef k_factory[] = {
    {L"Adventure and Action", L"avatar_sword.jpg", 4, 1, 0.55f, 1, 0.40f, 0, 0.5f, 0, 0.74f, 0, 0.50f, 1, 0.74f, 1,
     {4, 4, 0, -2, 2, 2, 1, 2, 4, 2}},
    {L"Driving Simulation", L"avatar_wheel.jpg", 5, 1, 0.20f, 0, 0.65f, 0, 0.5f, 0, 0.74f, 1, 0.35f, 1, 0.74f, 1,
     {4, 4, 2, -1, 2, 2, 1, 2, 4, 2}},
    {L"First Person Shooter", L"avatar_sniper.jpg", 7, 1, 0.30f, 0, 0.65f, 0, 0.5f, 1, 0.60f, 0, 0.50f, 1, 0.74f, 1,
     {6, 0, -3, -3, 0, 0, 0, 3, 3, 0}},
    {L"Real Time Strategy", L"avatar_map.jpg", 8, 1, 0.32f, 1, 0.50f, 0, 0.5f, 0, 0.74f, 1, 0.35f, 1, 0.74f, 1,
     {0, 2, -3, -3, -2, -2, -2, 2, 3, 0}},
};

int profile_factory_count(void) { return (int)(sizeof k_factory / sizeof k_factory[0]); }

void profile_factory(int i, const E1Params *base, E1Profile *out)
{
    const FactoryDef *d = &k_factory[i];
    memset(out, 0, sizeof *out);
    if (base) out->params = *base;
    else e1fx_params_default(&out->params);
    E1RenderParams *r = &out->params.render;
    E1CaptureParams *c = &out->params.capture;
    lstrcpynW(out->name, d->name, PROFILE_NAME_MAX);
    lstrcpynW(out->avatar, d->avatar, 64);
    out->profile_id = d->id;
    out->factory = 1;
    out->has_ext = 1;
    out->eq_preset_id = 0;
    r->master_on = 1;
    r->surround_on = (uint32_t)d->sur_on;   r->surround_level = d->sur;
    r->crystal_on = (uint32_t)d->cry_on;    r->crystal_level = d->cry;
    r->bass_on = (uint32_t)d->bass_on;      r->bass_level = d->bass * 100.0f; r->bass_xover_hz = 80.0f;
    r->svm_on = (uint32_t)d->svm_on;        r->svm_level = d->svm; r->svm_mode = E1FX_SVM_NORMAL;
    r->dialog_on = (uint32_t)d->dlg_on;     r->dialog_level = d->dlg;
    r->eq_on = 1;                            r->eq_preamp_db = 0.0f;
    for (int k = 0; k < E1FX_EQ_BANDS; ++k) r->eq_gain_db[k] = d->eq[k];
    r->scout_on = 0;
    c->master_on = 1;
    c->vfx_on = 0;
    c->vfx_preset = 0;
    if (e1_vfx_preset_count > 0)
        for (int k = 0; k < E1FX_VFX_PARAMS; ++k) c->vfx[k] = e1_vfx_presets[0].p[k];
    c->svm_on = (uint32_t)d->vsvm_on;       c->svm_level = d->vsvm;
    c->nr_on = (uint32_t)d->nr_on;
    e1fx_params_sanitize(&out->params);
}
