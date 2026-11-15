/*
 * eqpreset.c - custom EQ preset store (UTF-8 INI with BOM).
 *
 *   ; E1 Studio custom equalizer presets
 *   [Late night]
 *   preamp=-2
 *   bands=0,1,2,0,0,0,0,2,2,2
 */
#include "eqpreset.h"
#include "profile.h"
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int cmp_preset(const void *a, const void *b)
{
    return StrCmpLogicalW(((const EqPreset *)a)->name, ((const EqPreset *)b)->name);
}

void eqp_sort(EqPreset *list, int n)
{
    if (n > 1) qsort(list, (size_t)n, sizeof *list, cmp_preset);
}

static float clampf(float v, float lo, float hi)
{
    if (!isfinite(v)) return 0;
    return v < lo ? lo : (v > hi ? hi : v);
}

int eqp_parse(const char *text, size_t len, EqPreset *out, int max)
{
    const char *p = text, *end = text + len;
    int n = 0, cur = -1;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') ++p;
        const char *le = p;
        if (p < end) ++p;
        while (ls < le && (*ls == ' ' || *ls == '\t')) ++ls;
        while (le > ls && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) --le;
        if (ls >= le || *ls == ';' || *ls == '#') continue;
        if (*ls == '[') {
            const char *ne = le;
            if (ne[-1] != ']') { cur = -1; continue; }
            --ne;
            char name8[200];
            size_t nl = (size_t)(ne - ls - 1);
            if (nl == 0 || nl >= sizeof name8) { cur = -1; continue; }
            memcpy(name8, ls + 1, nl);
            name8[nl] = 0;
            wchar_t wname[EQP_NAME_MAX + 8];
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name8, -1, wname, EQP_NAME_MAX + 8) ||
                !e1_name_valid(wname)) { cur = -1; continue; }
            int dup = eqp_find(out, n, wname);
            if (dup >= 0) cur = dup;
            else if (n < max) {
                cur = n++;
                memset(&out[cur], 0, sizeof out[cur]);
                lstrcpynW(out[cur].name, wname, EQP_NAME_MAX);
            } else cur = -1;
            continue;
        }
        if (cur < 0) continue;
        const char *eq = memchr(ls, '=', (size_t)(le - ls));
        if (!eq) continue;
        const char *ke = eq;
        while (ke > ls && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
        char val[512];
        const char *vs = eq + 1;
        while (vs < le && (*vs == ' ' || *vs == '\t')) ++vs;
        size_t vl = (size_t)(le - vs);
        if (vl >= sizeof val) vl = sizeof val - 1;
        memcpy(val, vs, vl);
        val[vl] = 0;
        size_t kl = (size_t)(ke - ls);
        if (kl == 6 && !_strnicmp(ls, "preamp", 6)) {
            out[cur].preamp_db = clampf((float)strtod(val, NULL), -12, 12);
        } else if (kl == 5 && !_strnicmp(ls, "bands", 5)) {
            const char *s = val;
            for (int i = 0; i < E1FX_EQ_BANDS && *s; ++i) {
                char *e = NULL;
                double d = strtod(s, &e);
                if (e == s) break;
                out[cur].gain_db[i] = clampf((float)d, -24, 24);
                s = e;
                while (*s == ',' || *s == ' ' || *s == ';') ++s;
            }
        }
    }
    eqp_sort(out, n);
    return n;
}

static void fmtf(char *o, size_t cap, float v)
{
    snprintf(o, cap, "%.2f", (double)v);
    char *dot = strchr(o, '.');
    if (dot) {
        char *e = o + strlen(o) - 1;
        while (e > dot && *e == '0') *e-- = 0;
        if (e == dot) *e = 0;
    }
    if (!strcmp(o, "-0")) { o[0] = '0'; o[1] = 0; }
}

char *eqp_format(const EqPreset *list, int n, size_t *len_out)
{
    size_t cap = 128 + (size_t)n * 400;
    char *b = (char *)malloc(cap);
    if (!b) return NULL;
    size_t o = 0;
    o += (size_t)snprintf(b + o, cap - o, "\xEF\xBB\xBF; E1 Studio custom equalizer presets (dB; bands 31 Hz..16 kHz)\r\n");
    for (int i = 0; i < n; ++i) {
        char name8[EQP_NAME_MAX * 4], f[32];
        if (!WideCharToMultiByte(CP_UTF8, 0, list[i].name, -1, name8, sizeof name8, NULL, NULL)) continue;
        fmtf(f, sizeof f, list[i].preamp_db);
        o += (size_t)snprintf(b + o, cap - o, "\r\n[%s]\r\npreamp=%s\r\nbands=", name8, f);
        for (int k = 0; k < E1FX_EQ_BANDS; ++k) {
            fmtf(f, sizeof f, list[i].gain_db[k]);
            o += (size_t)snprintf(b + o, cap - o, "%s%s", k ? "," : "", f);
        }
        o += (size_t)snprintf(b + o, cap - o, "\r\n");
    }
    if (len_out) *len_out = o;
    return b;
}

int eqp_load(const wchar_t *path, EqPreset *out, int max)
{
    size_t len = 0;
    char *t = e1_read_file(path, &len);
    if (!t) return 0;
    int n = eqp_parse(t, len, out, max);
    free(t);
    return n;
}

int eqp_save(const wchar_t *path, const EqPreset *list, int n)
{
    size_t len = 0;
    char *t = eqp_format(list, n, &len);
    if (!t) return 0;
    int ok = e1_write_file_atomic(path, t, len);
    free(t);
    return ok;
}

int eqp_find(const EqPreset *list, int n, const wchar_t *name)
{
    for (int i = 0; i < n; ++i)
        if (CompareStringOrdinal(list[i].name, -1, name, -1, TRUE) == CSTR_EQUAL) return i;
    return -1;
}

int eqp_upsert(EqPreset *list, int *n, int max, const EqPreset *p)
{
    int i = eqp_find(list, *n, p->name);
    if (i < 0) {
        if (*n >= max) return -1;
        i = (*n)++;
    }
    list[i] = *p;
    eqp_sort(list, *n);
    return eqp_find(list, *n, p->name);
}

int eqp_remove(EqPreset *list, int *n, const wchar_t *name)
{
    int i = eqp_find(list, *n, name);
    if (i < 0) return 0;
    memmove(&list[i], &list[i + 1], sizeof(EqPreset) * (size_t)(*n - i - 1));
    --*n;
    return 1;
}

int eqp_default_path(wchar_t *out, size_t cch, int create_dir)
{
    if (!app_data_dir(out, cch, create_dir)) return 0;
    if ((size_t)lstrlenW(out) + 16 >= cch) return 0;
    lstrcatW(out, L"\\eq_presets.ini");
    return 1;
}
