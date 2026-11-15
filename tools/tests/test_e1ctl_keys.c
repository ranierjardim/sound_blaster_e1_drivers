/*
 * test_e1ctl_keys.c - unit tests for the e1ctl key table (tools/e1ctl_keys.c).
 *
 * The most important check is structural: every field of E1RenderParams / E1CaptureParams must be
 * reachable through exactly one key, so the CLI cannot silently fall out of sync with the
 * contract in include/e1fx_params.h.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../e1ctl_keys.h"
#include "../../src/dsp/e1dsp_modules.h"

static int g_fail = 0, g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_fail;                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

static void test_table_covers_contract(void)
{
    unsigned char cover[sizeof(E1Params)];
    size_t first = offsetof(E1Params, render), i;
    int k;
    memset(cover, 0, sizeof cover);
    for (k = 0; k < e1k_key_count; ++k) {
        const E1KeyDef *d = &e1k_keys[k];
        size_t b;
        CHECK(d->offset + 4 <= sizeof(E1Params));
        CHECK(d->offset >= first);
        CHECK(d->minv <= d->maxv);
        CHECK(d->help && d->help[0]);
        for (b = 0; b < 4 && d->offset + b < sizeof cover; ++b) cover[d->offset + b]++;
    }
    for (i = first; i < sizeof(E1Params); ++i) {
        if (cover[i] != 1) {
            fprintf(stderr, "byte %u of E1Params covered %u times\n", (unsigned)i, (unsigned)cover[i]);
            CHECK(cover[i] == 1);
            break;
        }
    }
    /* version/size are not user keys */
    CHECK(cover[offsetof(E1Params, version)] == 0);
    CHECK(cover[offsetof(E1Params, size)] == 0);
}

static void test_names_unique_and_lookup(void)
{
    int a, b;
    for (a = 0; a < e1k_key_count; ++a) {
        CHECK(e1k_find(e1k_keys[a].name) == &e1k_keys[a]);
        if (e1k_keys[a].alias) CHECK(e1k_find(e1k_keys[a].alias) == &e1k_keys[a]);
        for (b = a + 1; b < e1k_key_count; ++b) {
            CHECK(strcmp(e1k_keys[a].name, e1k_keys[b].name) != 0);
            if (e1k_keys[a].alias && e1k_keys[b].alias) CHECK(strcmp(e1k_keys[a].alias, e1k_keys[b].alias) != 0);
        }
    }
    CHECK(e1k_find("RENDER.SURROUND_ON") == e1k_find("render.surround_on"));
    CHECK(e1k_find("render.eq_gain_db.3") == e1k_find("render.eq_gain_db[3]"));
    CHECK(e1k_find("render.eq.250") == e1k_find("render.eq_gain_db[3]"));
    CHECK(e1k_find("render.eq.16k") == e1k_find("render.eq_gain_db[9]"));
    CHECK(e1k_find("capture.vfx.2") == e1k_find("capture.vfx.f3"));
    CHECK(e1k_find("render.eq_gain_db[10]") == NULL);
    CHECK(e1k_find("render.nope") == NULL);
    CHECK(e1k_find("") == NULL);
    CHECK(e1k_find(NULL) == NULL);
    CHECK(e1k_find("render.eq_gain_db.") == NULL);
}

static void test_parse(void)
{
    const E1KeyDef *on = e1k_find("render.surround_on");
    const E1KeyDef *lvl = e1k_find("render.surround_level");
    const E1KeyDef *mode = e1k_find("render.svm_mode");
    const E1KeyDef *spk = e1k_find("render.speaker_mode");
    const E1KeyDef *eq = e1k_find("render.eq.1k");
    const E1KeyDef *pre = e1k_find("capture.vfx_preset");
    char err[256];
    double v = -1;

    CHECK(on && lvl && mode && spk && eq && pre);
    if (!(on && lvl && mode && spk && eq && pre)) return;
    CHECK(e1k_parse_value(on, "1", &v, err, sizeof err) && v == 1);
    CHECK(e1k_parse_value(on, "OFF", &v, err, sizeof err) && v == 0);
    CHECK(e1k_parse_value(on, "yes", &v, err, sizeof err) && v == 1);
    CHECK(!e1k_parse_value(on, "2", &v, err, sizeof err));
    CHECK(strstr(err, "render.surround_on") != NULL);

    CHECK(e1k_parse_value(lvl, "0.4", &v, err, sizeof err) && fabs(v - 0.4) < 1e-12);
    CHECK(e1k_parse_value(lvl, " 1 ", &v, err, sizeof err) && v == 1.0);
    CHECK(!e1k_parse_value(lvl, "1.01", &v, err, sizeof err));
    CHECK(strstr(err, "out of range") != NULL);
    CHECK(!e1k_parse_value(lvl, "-0.1", &v, err, sizeof err));
    CHECK(!e1k_parse_value(lvl, "nan", &v, err, sizeof err));
    CHECK(!e1k_parse_value(lvl, "inf", &v, err, sizeof err));
    CHECK(!e1k_parse_value(lvl, "0.5x", &v, err, sizeof err));
    CHECK(!e1k_parse_value(lvl, "", &v, err, sizeof err));
    CHECK(!e1k_parse_value(lvl, "1e999", &v, err, sizeof err));

    CHECK(e1k_parse_value(mode, "night", &v, err, sizeof err) && v == 2);
    CHECK(e1k_parse_value(mode, "Loud", &v, err, sizeof err) && v == 1);
    CHECK(e1k_parse_value(mode, "0", &v, err, sizeof err) && v == 0);
    CHECK(!e1k_parse_value(mode, "3", &v, err, sizeof err));
    CHECK(!e1k_parse_value(mode, "1.5", &v, err, sizeof err));
    CHECK(!e1k_parse_value(mode, "quiet", &v, err, sizeof err));
    CHECK(e1k_parse_value(spk, "speakers", &v, err, sizeof err) && v == 1);
    CHECK(e1k_parse_value(spk, "headphones", &v, err, sizeof err) && v == 0);

    CHECK(e1k_parse_value(eq, "-24", &v, err, sizeof err) && v == -24);
    CHECK(e1k_parse_value(eq, "+6.5", &v, err, sizeof err) && v == 6.5);
    CHECK(!e1k_parse_value(eq, "24.5", &v, err, sizeof err));

    CHECK(e1k_parse_value(pre, "17", &v, err, sizeof err) && v == 17);
    CHECK(!e1k_parse_value(pre, "18", &v, err, sizeof err));
    CHECK(!e1k_parse_value(pre, "2.5", &v, err, sizeof err));
}

static void test_roundtrip_and_sanitize(void)
{
    int k;
    for (k = 0; k < e1k_key_count; ++k) {
        const E1KeyDef *d = &e1k_keys[k];
        double probes[3];
        int i;
        probes[0] = d->minv;
        probes[1] = d->maxv;
        probes[2] = (d->type == E1K_FLOAT) ? 0.5 * (d->minv + d->maxv) : floor(0.5 * (d->minv + d->maxv));
        for (i = 0; i < 3; ++i) {
            E1Params p;
            double got;
            e1fx_params_default(&p);
            e1k_set(d, &p, probes[i]);
            got = e1k_get(d, &p);
            CHECK(fabs(got - probes[i]) <= fabs(probes[i]) * 1e-6 + 1e-6);
            /* a legal value must survive the contract's sanitizer unchanged */
            e1fx_params_sanitize(&p);
            got = e1k_get(d, &p);
            if (!(fabs(got - probes[i]) <= fabs(probes[i]) * 1e-6 + 1e-6))
                fprintf(stderr, "sanitize changed %s: %g -> %g\n", d->name, probes[i], got);
            CHECK(fabs(got - probes[i]) <= fabs(probes[i]) * 1e-6 + 1e-6);
        }
    }
}

static void test_defaults_in_range(void)
{
    E1Params p;
    int k;
    e1fx_params_default(&p);
    CHECK(p.version == E1FX_PARAMS_VERSION);
    CHECK(p.size == sizeof(E1Params));
    CHECK(p.render.master_on == 0);   /* bit-exact by default */
    CHECK(p.capture.master_on == 0);
    CHECK(p.render.eq_on == 0);
    for (k = 0; k < e1k_key_count; ++k) {
        double v = e1k_get(&e1k_keys[k], &p);
        if (!(v >= e1k_keys[k].minv && v <= e1k_keys[k].maxv))
            fprintf(stderr, "default of %s = %g outside %g..%g\n", e1k_keys[k].name, v, e1k_keys[k].minv, e1k_keys[k].maxv);
        CHECK(v >= e1k_keys[k].minv && v <= e1k_keys[k].maxv);
    }
}

static void test_format(void)
{
    E1Params p;
    char buf[64];
    e1fx_params_default(&p);
    p.render.surround_on = 1;
    p.render.svm_mode = 2;
    p.render.eq_gain_db[3] = -3.5f;
    e1k_format(e1k_find("render.surround_on"), &p, buf, sizeof buf);
    CHECK(strcmp(buf, "1 (on)") == 0);
    e1k_format(e1k_find("render.svm_mode"), &p, buf, sizeof buf);
    CHECK(strcmp(buf, "2 (night)") == 0);
    e1k_format(e1k_find("render.eq.250"), &p, buf, sizeof buf);
    CHECK(strcmp(buf, "-3.50 dB") == 0);
    e1k_describe_range(e1k_find("render.svm_mode"), buf, sizeof buf);
    CHECK(strcmp(buf, "normal|loud|night") == 0);
    e1k_describe_range(e1k_find("render.bass_xover_hz"), buf, sizeof buf);
    CHECK(strcmp(buf, "10..1000 Hz") == 0);
    e1k_format(e1k_find("render.svm_mode"), &p, buf, 4);   /* truncation stays terminated */
    CHECK(strlen(buf) == 3);
    e1k_format_dbfs(1.0f, buf, sizeof buf);
    CHECK(strcmp(buf, "   0.0") == 0);
    e1k_format_dbfs(0.5f, buf, sizeof buf);
    CHECK(strcmp(buf, "  -6.0") == 0);
    e1k_format_dbfs(0.0f, buf, sizeof buf);
    CHECK(strcmp(buf, "  -inf") == 0);
    e1k_format_dbfs(NAN, buf, sizeof buf);
    CHECK(strcmp(buf, "  -inf") == 0);
}

static void test_presets(void)
{
    E1Params p;
    int idx, b;
    CHECK(e1_eq_preset_count > 0);
    CHECK(e1k_loose_equal("new-age", "New Age"));
    CHECK(e1k_loose_equal("NEWAGE", "New_Age"));
    CHECK(!e1k_loose_equal("New", "New Age"));
    e1fx_params_default(&p);
    p.render.scout_on = 1;
    idx = e1k_apply_eq_preset(&p, e1_eq_presets[e1_eq_preset_count - 1].name);
    CHECK(idx == e1_eq_preset_count - 1);
    CHECK(p.render.eq_on == 1 && p.render.scout_on == 0);
    for (b = 0; b < E1FX_EQ_BANDS; ++b) CHECK(p.render.eq_gain_db[b] == e1_eq_presets[idx].gain_db[b]);
    CHECK(e1k_apply_eq_preset(&p, "no such preset") == -1);
    CHECK(e1k_find_eq_preset("flat") >= 0);

    CHECK(e1_vfx_preset_count > 0);
    idx = e1k_apply_vfx_preset(&p, e1_vfx_presets[0].name);
    CHECK(idx == 0 && p.capture.vfx_on == 1 && p.capture.vfx_preset == 0);
    for (b = 0; b < E1FX_VFX_PARAMS; ++b) CHECK(p.capture.vfx[b] == e1_vfx_presets[0].p[b]);
    /* every shipped VoiceFX preset must be inside the ranges the CLI enforces */
    for (idx = 0; idx < e1_vfx_preset_count; ++idx) {
        for (b = 0; b < E1FX_VFX_PARAMS; ++b) {
            char name[32];
            const E1KeyDef *d;
            snprintf(name, sizeof name, "capture.vfx[%d]", b);
            d = e1k_find(name);
            CHECK(d != NULL);
            if (d) CHECK(e1_vfx_presets[idx].p[b] >= d->minv && e1_vfx_presets[idx].p[b] <= d->maxv);
        }
    }
    for (idx = 0; idx < e1_eq_preset_count; ++idx)
        for (b = 0; b < E1FX_EQ_BANDS; ++b)
            CHECK(e1_eq_presets[idx].gain_db[b] >= -24.f && e1_eq_presets[idx].gain_db[b] <= 24.f);
}

static void test_status_report(void)
{
    E1ApoStatus a, b;
    E1SharedBlock blk;
    FILE *f = tmpfile();
    char text[2048];
    size_t n;
    int moving;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    CHECK(f != NULL);
    if (!f) return;
    a.alive = 0x7FFFFFF0; /* wraps across INT32_MAX */
    b.alive = (int32_t)0x80000010u;
    b.sample_rate = 44100;
    b.channels = 2;
    b.peak_in[0] = 0.5f;
    b.peak_out[0] = 1.0f;
    b.gain_reduction_db = -1.5f;
    moving = e1k_print_status(f, "Render", &a, &b, 0.4);
    CHECK(moving == 1);
    moving = e1k_print_status(f, "Capture", &b, &b, 0.4);
    CHECK(moving == 0);
    rewind(f);
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    CHECK(strstr(text, "running, 80 buffers/s") != NULL);
    CHECK(strstr(text, "44100 Hz, 2 ch") != NULL);
    CHECK(strstr(text, "-6.0") != NULL);
    CHECK(strstr(text, "idle") != NULL);
    CHECK(strstr(text, "-1.5 dB") != NULL);

    memset(&blk, 0, sizeof blk);
    CHECK(!e1k_block_compatible(&blk));
    blk.magic = E1FX_MAGIC;
    blk.block_size = sizeof blk;
    CHECK(e1k_block_compatible(&blk));
    blk.block_size = sizeof blk - 4;
    CHECK(!e1k_block_compatible(&blk));
    CHECK(!e1k_block_compatible(NULL));
}

static void test_dump(void)
{
    E1Params p;
    FILE *f = tmpfile();
    char text[8192];
    size_t n;
    int lines = 0;
    const char *c;
    CHECK(f != NULL);
    if (!f) return;
    e1fx_params_default(&p);
    e1k_dump_params(f, &p, "capture.");
    rewind(f);
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    for (c = text; *c; ++c) lines += (*c == '\n');
    CHECK(strstr(text, "render.") == NULL);
    CHECK(strstr(text, "capture.master_on") != NULL);
    CHECK(lines == 16); /* 7 scalars + 8 vfx + output gain */
}

int main(void)
{
    test_table_covers_contract();
    test_names_unique_and_lookup();
    test_parse();
    test_roundtrip_and_sanitize();
    test_defaults_in_range();
    test_format();
    test_presets();
    test_status_report();
    test_dump();
    printf("test_e1ctl_keys: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
