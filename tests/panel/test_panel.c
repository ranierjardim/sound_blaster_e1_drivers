/*
 * test_panel.c - non-UI unit tests of e1panel: profile XML (round trip, Creative-format input,
 * robustness), factory profiles, EQ preset INI store, EQ response maths, string tables and the
 * APO heartbeat classification. Uses only a private temp folder; touches no system state.
 */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../src/panel/profile.h"
#include "../../src/panel/eqpreset.h"
#include "../../src/panel/eqcurve.h"
#include "../../src/panel/strings.h"
#include "../../src/panel/sync.h"
#include "../../src/panel/state.h"
#include "../../src/dsp/e1dsp_modules.h"

static int g_fail, g_pass;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) ++g_pass;                                                    \
        else {                                                                 \
            ++g_fail;                                                          \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static int feq(double a, double b, double tol) { return fabs(a - b) <= tol; }

static wchar_t g_tmp[MAX_PATH];

static void make_tmp(void)
{
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    swprintf(g_tmp, MAX_PATH, L"%se1panel_test_%lu", base, GetCurrentProcessId());
    CreateDirectoryW(g_tmp, NULL);
}

static void rm_tmp(void)
{
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    swprintf(pat, MAX_PATH, L"%s\\*", g_tmp);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            swprintf(p, MAX_PATH, L"%s\\%s", g_tmp, fd.cFileName);
            DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(g_tmp);
}

/* deterministic pseudo random */
static unsigned g_seed = 12345u;
static float frand(float lo, float hi)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(g_seed >> 8) / 16777216.0f;
}

static void random_params(E1Params *p)
{
    e1fx_params_default(p);
    E1RenderParams *r = &p->render;
    E1CaptureParams *c = &p->capture;
    r->master_on = 1; r->surround_on = 1; r->surround_level = frand(0, 1);
    r->crystal_on = 0; r->crystal_level = frand(0, 1);
    r->bass_on = 1; r->bass_level = (float)(int)frand(0, 100); r->bass_xover_hz = (float)(int)frand(10, 1000);
    r->svm_on = 1; r->svm_level = frand(0, 1); r->svm_mode = E1FX_SVM_NIGHT;
    r->dialog_on = 1; r->dialog_level = frand(0, 1);
    r->eq_on = 1; r->eq_preamp_db = (float)(int)(frand(-12, 12) * 4) / 4;
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) r->eq_gain_db[i] = (float)(int)(frand(-24, 24) * 2) / 2;
    r->scout_on = 1; r->output_gain_db = -3.5f; r->limiter_on = 0;
    c->master_on = 1; c->nr_on = 0; c->nr_strength = frand(0, 1); c->svm_on = 1; c->svm_level = frand(0, 1);
    c->vfx_on = 1; c->vfx_preset = 4;
    for (int i = 0; i < E1FX_VFX_PARAMS; ++i) c->vfx[i] = e1_vfx_presets[4].p[i];
    c->output_gain_db = 2.0f;
}

static void compare_params(const E1Params *a, const E1Params *b, const char *what)
{
    const E1RenderParams *x = &a->render, *y = &b->render;
    const E1CaptureParams *u = &a->capture, *v = &b->capture;
    CHECK(x->master_on == y->master_on && x->surround_on == y->surround_on && x->crystal_on == y->crystal_on &&
              x->bass_on == y->bass_on && x->svm_on == y->svm_on && x->dialog_on == y->dialog_on && x->eq_on == y->eq_on &&
              x->scout_on == y->scout_on && x->limiter_on == y->limiter_on && x->svm_mode == y->svm_mode,
          "%s: render switches differ", what);
    CHECK(feq(x->surround_level, y->surround_level, 1e-4) && feq(x->crystal_level, y->crystal_level, 1e-4) &&
              feq(x->bass_level, y->bass_level, 1e-2) && feq(x->bass_xover_hz, y->bass_xover_hz, 0.06) &&
              feq(x->svm_level, y->svm_level, 1e-4) && feq(x->dialog_level, y->dialog_level, 1e-4) &&
              feq(x->output_gain_db, y->output_gain_db, 1e-3),
          "%s: render levels differ", what);
    CHECK(feq(x->eq_preamp_db, y->eq_preamp_db, 1e-3), "%s: preamp %f vs %f", what, x->eq_preamp_db, y->eq_preamp_db);
    for (int i = 0; i < E1FX_EQ_BANDS; ++i)
        CHECK(feq(x->eq_gain_db[i], y->eq_gain_db[i], 1e-3), "%s: band %d %f vs %f", what, i, x->eq_gain_db[i], y->eq_gain_db[i]);
    CHECK(u->master_on == v->master_on && u->nr_on == v->nr_on && u->svm_on == v->svm_on && u->vfx_on == v->vfx_on &&
              u->vfx_preset == v->vfx_preset,
          "%s: capture switches differ", what);
    CHECK(feq(u->nr_strength, v->nr_strength, 1e-4) && feq(u->svm_level, v->svm_level, 1e-4) &&
              feq(u->output_gain_db, v->output_gain_db, 1e-3),
          "%s: capture levels differ", what);
    for (int i = 0; i < E1FX_VFX_PARAMS; ++i)
        CHECK(feq(u->vfx[i], v->vfx[i], 1e-3 * (fabs(u->vfx[i]) + 1)), "%s: vfx[%d] %f vs %f", what, i, u->vfx[i], v->vfx[i]);
}

static void test_profile_roundtrip(void)
{
    printf("profile XML round trip\n");
    for (int iter = 0; iter < 50; ++iter) {
        E1Profile a, b;
        memset(&a, 0, sizeof a);
        random_params(&a.params);
        e1fx_params_sanitize(&a.params);
        lstrcpyW(a.name, L"Perfil Ação & <Noite> \"ñ\" 'x'");
        lstrcpyW(a.avatar, L"avatar_map.jpg");
        a.profile_id = 100;
        a.eq_preset_id = 3;
        size_t n = 0;
        char *xml = profile_to_xml(&a, &n);
        CHECK(xml && n > 100, "serialise");
        if (!xml) return;
        E1Params base;
        e1fx_params_default(&base);
        CHECK(profile_from_xml(xml, n, &base, &b), "parse back");
        CHECK(lstrcmpW(a.name, b.name) == 0, "name round trip");
        CHECK(lstrcmpW(a.avatar, b.avatar) == 0, "avatar round trip");
        CHECK(b.profile_id == 100 && b.eq_preset_id == 3 && b.has_ext, "info fields");
        compare_params(&a.params, &b.params, "roundtrip");
        if (iter == 0) {
            CHECK(strstr(xml, "<surround enable=\"true\"") != NULL, "Creative element names present");
            CHECK(strstr(xml, "<graphic_eq enable=\"true\" preset=\"3\"") != NULL, "graphic_eq attributes");
            CHECK(strstr(xml, "&amp;") && strstr(xml, "&lt;Noite&gt;"), "escaping");
            CHECK(strstr(xml, "speaker_mode") == NULL, "speaker mode is not part of a profile");
        }
        free(xml);
    }
}

static void test_profile_creative_input(void)
{
    printf("Creative-format profile input\n");
    const char *xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<!-- exported by the original panel -->\r\n"
        "<profile>\n <info><avatar>avatar_sword.jpg</avatar><profile_name>Caf&#233; &amp; Jogo</profile_name>"
        "<profile_id>4</profile_id><version>2.0</version></info>\n"
        " <settings>\n"
        "  <surround enable=\"true\" level=\"0.55\"/>\n"
        "  <stereo_surround enable=\"false\" level=\"0.2\"/>\n"
        "  <crystalizer enable='true' level='0.40'/>\n"
        "  <bass enable=\"false\" level=\"0.5\" cross_freq=\"80\"/>\n"
        "  <svm enable=\"true\" level=\"0.6\" mode=\"2\"/>\n"
        "  <dlgplus enable=\"false\" level=\"0.35\"/>\n"
        "  <voicefx enable=\"true\" preset=\"5\"/>\n"
        "  <vip_svm enable=\"true\" level=\"0.74\"/>\n"
        "  <noise_reduction enable=\"true\"/>\n"
        "  <aec enable=\"false\"/>\n"
        "  <graphic_eq enable=\"true\" preset=\"0\" preamp=\"-1.5\" band0=\"4\" band1=\"4\" band2=\"0\" band3=\"-2\" band4=\"2\""
        " band5=\"2\" band6=\"1\" band7=\"2\" band8=\"4\" band9=\"2\"/>\n"
        " </settings>\n</profile>\n";
    E1Params base;
    E1Profile p;
    e1fx_params_default(&base);
    base.render.master_on = 0;
    base.render.speaker_mode = E1FX_SPK_STEREO;
    base.render.output_gain_db = -6;
    CHECK(profile_from_xml(xml, strlen(xml), &base, &p), "parse Creative profile");
    CHECK(lstrcmpW(p.name, L"Café & Jogo") == 0, "entity decoding in name");
    CHECK(p.profile_id == 4 && !p.has_ext, "info id, no extension");
    CHECK(p.params.render.surround_on && feq(p.params.render.surround_level, 0.55, 1e-6), "surround");
    CHECK(feq(p.params.render.bass_level, 50, 1e-4), "bass level 0.5 normalised -> 50 (got %f)", p.params.render.bass_level);
    CHECK(feq(p.params.render.bass_xover_hz, 80, 1e-4), "crossover");
    CHECK(p.params.render.svm_on && p.params.render.svm_mode == E1FX_SVM_NIGHT && feq(p.params.render.svm_level, 0.6, 1e-6), "svm");
    CHECK(p.params.capture.vfx_on && p.params.capture.vfx_preset == 4, "voicefx preset id 5 -> index 4");
    CHECK(feq(p.params.capture.vfx[0], e1_vfx_presets[4].p[0], 1e-4) && feq(p.params.capture.vfx[6], e1_vfx_presets[4].p[6], 1e-4),
          "voicefx raw params loaded from the preset table");
    CHECK(feq(p.params.render.eq_preamp_db, -1.5, 1e-6) && feq(p.params.render.eq_gain_db[3], -2, 1e-6), "eq");
    CHECK(p.params.render.master_on == 0 && p.params.render.speaker_mode == E1FX_SPK_STEREO &&
              feq(p.params.render.output_gain_db, -6, 1e-6),
          "fields absent in the file keep the base values");

    /* bass above 1 is taken as is (loader rule) */
    const char *x2 = "<profile><settings><bass enable=\"true\" level=\"72\" cross_freq=\"120\"/></settings></profile>";
    CHECK(profile_from_xml(x2, strlen(x2), &base, &p) && feq(p.params.render.bass_level, 72, 1e-4), "bass > 1 as is");
    /* out-of-range values are clamped by sanitize */
    const char *x3 = "<profile><settings><surround enable=\"1\" level=\"7\"/><graphic_eq band0=\"99\" preamp=\"-40\"/></settings></profile>";
    CHECK(profile_from_xml(x3, strlen(x3), &base, &p), "parse out-of-range");
    CHECK(p.params.render.surround_level <= 1.0f && p.params.render.eq_gain_db[0] <= 24.0f && p.params.render.eq_preamp_db >= -12.0f,
          "clamped (%f %f %f)", p.params.render.surround_level, p.params.render.eq_gain_db[0], p.params.render.eq_preamp_db);

    /* UTF-16LE with BOM */
    int wl = MultiByteToWideChar(CP_UTF8, 0, xml, -1, NULL, 0);
    char *u16 = (char *)malloc((size_t)wl * 2 + 2);
    ((unsigned char *)u16)[0] = 0xFFu;
    ((unsigned char *)u16)[1] = 0xFEu;
    MultiByteToWideChar(CP_UTF8, 0, xml, -1, (wchar_t *)(u16 + 2), wl);
    CHECK(profile_from_xml(u16, (size_t)(wl - 1) * 2 + 2, &base, &p) && lstrcmpW(p.name, L"Café & Jogo") == 0, "UTF-16 file");
    free(u16);

    /* rejects */
    const char *bad[] = {"", "not xml", "<settings><surround enable=\"true\"/></settings>", "<profile><settings>",
                         "<profile><settings><surround enable=\"true\" level=0.4/></settings></profile>",
                         "<profile></settings></profile></profile>"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        CHECK(!profile_from_xml(bad[i], strlen(bad[i]), &base, &p), "reject malformed #%u", (unsigned)i);
    /* deep nesting must not overflow */
    char deep[4096];
    size_t o = 0;
    o += (size_t)snprintf(deep, sizeof deep, "<profile>");
    for (int i = 0; i < 100; ++i) o += (size_t)snprintf(deep + o, sizeof deep - o, "<a>");
    CHECK(!profile_from_xml(deep, o, &base, &p), "reject over-deep document");
}

static void test_factory(void)
{
    printf("factory profiles\n");
    CHECK(profile_factory_count() == 4, "4 factory profiles");
    E1Params base;
    e1fx_params_default(&base);
    base.render.speaker_mode = E1FX_SPK_STEREO;
    base.render.output_gain_db = -2;
    E1Profile p;
    profile_factory(2, &base, &p); /* First Person Shooter */
    CHECK(p.factory && p.profile_id == 7, "FPS id");
    CHECK(p.params.render.master_on && p.params.render.surround_on && feq(p.params.render.surround_level, 0.30, 1e-6), "FPS surround");
    CHECK(!p.params.render.crystal_on && p.params.render.svm_on && feq(p.params.render.svm_level, 0.6, 1e-6), "FPS crystal/svm");
    static const float fps_eq[10] = {6, 0, -3, -3, 0, 0, 0, 3, 3, 0};
    int eq_ok = p.params.render.eq_on;
    for (int i = 0; i < 10; ++i) eq_ok &= feq(p.params.render.eq_gain_db[i], fps_eq[i], 1e-6);
    CHECK(eq_ok, "FPS EQ curve");
    CHECK(p.params.capture.master_on && p.params.capture.nr_on && p.params.capture.svm_on, "FPS voice");
    CHECK(p.params.render.speaker_mode == E1FX_SPK_STEREO && feq(p.params.render.output_gain_db, -2, 1e-6), "keeps device settings");
    profile_factory(0, &base, &p);
    CHECK(feq(p.params.render.surround_level, 0.55, 1e-6) && p.params.render.crystal_on && feq(p.params.render.crystal_level, 0.40, 1e-6),
          "Adventure and Action");
    profile_factory(3, &base, &p);
    CHECK(feq(p.params.render.surround_level, 0.32, 1e-6) && p.params.render.dialog_on && feq(p.params.render.dialog_level, 0.35, 1e-6),
          "Real Time Strategy");
    /* factory -> XML -> back */
    size_t n;
    char *x = profile_to_xml(&p, &n);
    E1Profile q;
    CHECK(x && profile_from_xml(x, n, &base, &q), "factory serialises");
    if (x) compare_params(&p.params, &q.params, "factory roundtrip");
    free(x);
}

static void test_files(void)
{
    printf("profile files\n");
    wchar_t path[MAX_PATH];
    E1Profile a, b;
    memset(&a, 0, sizeof a);
    random_params(&a.params);
    e1fx_params_sanitize(&a.params);
    lstrcpyW(a.name, L"Noite");
    swprintf(path, MAX_PATH, L"%s\\Noite.xml", g_tmp);
    CHECK(profile_save(path, &a), "save");
    CHECK(profile_load(path, NULL, &b) && lstrcmpW(b.name, L"Noite") == 0, "load");
    compare_params(&a.params, &b.params, "file");
    lstrcpyW(a.name, L"Profile 10");
    swprintf(path, MAX_PATH, L"%s\\p10.xml", g_tmp);
    profile_save(path, &a);
    lstrcpyW(a.name, L"Profile 2");
    swprintf(path, MAX_PATH, L"%s\\p2.xml", g_tmp);
    profile_save(path, &a);
    swprintf(path, MAX_PATH, L"%s\\garbage.xml", g_tmp);
    e1_write_file_atomic(path, "<html></html>", 13);
    ProfileEntry e[8];
    int n = profile_list(g_tmp, e, 8);
    CHECK(n == 3, "list skips invalid files (n=%d)", n);
    if (n == 3)
        CHECK(!lstrcmpW(e[0].name, L"Noite") && !lstrcmpW(e[1].name, L"Profile 2") && !lstrcmpW(e[2].name, L"Profile 10"),
              "logical sort order");
    wchar_t fn[MAX_PATH];
    profile_filename(L"a/b:c*?.", fn, MAX_PATH);
    CHECK(!lstrcmpW(fn, L"a_b_c__.xml"), "sanitised file name (%ls)", fn);
    CHECK(e1_name_valid(L"Jogo noturno") && !e1_name_valid(L"") && !e1_name_valid(L"   ") && !e1_name_valid(L"a[b]") &&
              !e1_name_valid(L"123456789012345678901234567890123") && e1_name_valid(L"12345678901234567890123456789012"),
          "name validation");
}

static void test_eqpresets(void)
{
    printf("EQ preset store\n");
    EqPreset list[EQP_MAX], back[EQP_MAX];
    int n = 0;
    EqPreset p;
    memset(&p, 0, sizeof p);
    const wchar_t *names[] = {L"Preset 10", L"Preset 2", L"Graves fortes", L"Ação"};
    for (int k = 0; k < 4; ++k) {
        lstrcpyW(p.name, names[k]);
        p.preamp_db = (float)k - 1.5f;
        for (int i = 0; i < E1FX_EQ_BANDS; ++i) p.gain_db[i] = (float)(i - k) * 1.5f;
        CHECK(eqp_upsert(list, &n, EQP_MAX, &p) >= 0, "upsert");
    }
    CHECK(n == 4, "4 presets");
    CHECK(!lstrcmpW(list[0].name, L"Ação") && !lstrcmpW(list[2].name, L"Preset 2") && !lstrcmpW(list[3].name, L"Preset 10"),
          "sorted logically");
    lstrcpyW(p.name, L"preset 2"); /* case-insensitive replace */
    p.preamp_db = 7;
    eqp_upsert(list, &n, EQP_MAX, &p);
    CHECK(n == 4 && feq(list[eqp_find(list, n, L"PRESET 2")].preamp_db, 7, 1e-6), "case-insensitive replace");
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\eq_presets.ini", g_tmp);
    CHECK(eqp_save(path, list, n), "save ini");
    int m = eqp_load(path, back, EQP_MAX);
    CHECK(m == n, "load count %d", m);
    for (int k = 0; k < m && k < n; ++k) {
        CHECK(!lstrcmpW(list[k].name, back[k].name), "name %d", k);
        CHECK(feq(list[k].preamp_db, back[k].preamp_db, 1e-3), "preamp %d", k);
        for (int i = 0; i < E1FX_EQ_BANDS; ++i) CHECK(feq(list[k].gain_db[i], back[k].gain_db[i], 1e-3), "gain %d/%d", k, i);
    }
    CHECK(eqp_remove(list, &n, L"graves FORTES") && n == 3 && eqp_find(list, n, L"Graves fortes") < 0, "remove");
    const char *messy = "junk\r\n[ok]\r\npreamp = 99\r\nbands=1, 2,3\r\n[bad/name]\r\npreamp=1\r\n[]\r\n[ok]\r\nbands=4\r\n";
    m = eqp_parse(messy, strlen(messy), back, EQP_MAX);
    CHECK(m == 1 && feq(back[0].preamp_db, 12, 1e-6) && feq(back[0].gain_db[0], 4, 1e-6) && feq(back[0].gain_db[1], 2, 1e-6),
          "tolerant parse, clamp, duplicate section merges (m=%d)", m);
    swprintf(path, MAX_PATH, L"%s\\missing.ini", g_tmp);
    CHECK(eqp_load(path, back, EQP_MAX) == 0, "missing file = no presets");
}

static void test_eqcurve(void)
{
    printf("EQ response\n");
    E1BqCoef c[E1FX_EQ_BANDS];
    float gains[E1FX_EQ_BANDS] = {0};
    double maxdev = 0;
    eqc_design(EQC_FS, 0, gains, c);
    for (double f = 10; f < 22000; f *= 1.1) maxdev = fmax(maxdev, fabs(eqc_response_db(c, EQC_FS, f)));
    CHECK(maxdev < 1e-9, "flat = 0 dB (%g)", maxdev);
    eqc_design(EQC_FS, 3.0f, gains, c);
    CHECK(feq(eqc_response_db(c, EQC_FS, 1000), 3.0, 1e-6) && feq(eqc_response_db(c, EQC_FS, 50), 3.0, 1e-6), "preamp folds in");
    gains[5] = 6;
    eqc_design(EQC_FS, 0, gains, c);
    double at = eqc_response_db(c, EQC_FS, 1000);
    CHECK(feq(at, 6.0, 0.01), "+6 dB peak at 1 kHz (%f)", at);
    CHECK(fabs(eqc_response_db(c, EQC_FS, 31.25)) < 0.3 && fabs(eqc_response_db(c, EQC_FS, 16000)) < 0.3, "peak is local");
    /* octave bandwidth: the band edges sit near half gain (in dB) */
    double edge = eqc_response_db(c, EQC_FS, 1000 * pow(2, 0.5));
    CHECK(edge > 1.5 && edge < 4.5, "one-octave bandwidth (edge %f dB)", edge);
    /* boost/cut symmetry of the constant-Q section */
    float neg[E1FX_EQ_BANDS] = {0};
    neg[5] = -6;
    E1BqCoef d[E1FX_EQ_BANDS];
    eqc_design(EQC_FS, 0, neg, d);
    maxdev = 0;
    for (double f = 20; f < 20000; f *= 1.07) maxdev = fmax(maxdev, fabs(eqc_response_db(c, EQC_FS, f) + eqc_response_db(d, EQC_FS, f)));
    CHECK(maxdev < 1e-6, "boost/cut mirror (%g)", maxdev);
    /* shelves */
    memset(gains, 0, sizeof gains);
    gains[0] = 6;
    gains[9] = -6;
    eqc_design(EQC_FS, 0, gains, c);
    CHECK(feq(eqc_response_db(c, EQC_FS, 5), 6, 0.2) && feq(eqc_response_db(c, EQC_FS, 20000), -6, 0.6) &&
              fabs(eqc_response_db(c, EQC_FS, 1000)) < 0.3,
          "low/high shelf (%f %f %f)", eqc_response_db(c, EQC_FS, 5), eqc_response_db(c, EQC_FS, 20000), eqc_response_db(c, EQC_FS, 1000));
    /* extreme settings stay finite and stable (poles inside the unit circle) */
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) gains[i] = (i & 1) ? 24.0f : -24.0f;
    eqc_design(EQC_FS, 12, gains, c);
    int stable = 1;
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) stable &= fabs(c[i].a2) < 1.0 && fabs(c[i].a1) < 1.0 + c[i].a2;
    CHECK(stable, "all sections stable at +-24 dB");
    CHECK(isfinite(eqc_response_db(c, EQC_FS, 1234)), "finite");
    /* the panel curve must agree with a direct biquad run (impulse response -> DFT at 1 kHz) */
    memset(gains, 0, sizeof gains);
    gains[3] = 9;
    gains[6] = -4;
    eqc_design(EQC_FS, -2, gains, c);
    enum { N = 1 << 15 };
    static float imp[N];
    memset(imp, 0, sizeof imp);
    imp[0] = 1;
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) {
        E1BqState s = {0, 0};
        e1_bq_run(&c[i], &s, imp, N);
    }
    double re = 0, im = 0, w = 2 * E1_PI * 250.0 / EQC_FS;
    for (int i = 0; i < N; ++i) { re += imp[i] * cos(w * i); im -= imp[i] * sin(w * i); }
    double sim = 20 * log10(sqrt(re * re + im * im));
    CHECK(feq(sim, eqc_response_db(c, EQC_FS, 250), 0.02), "curve matches processed impulse (%f vs %f)", sim, eqc_response_db(c, EQC_FS, 250));
}

static void test_strings(void)
{
    printf("string tables\n");
    for (int lang = 0; lang < 2; ++lang) {
        str_set_lang(lang);
        int empty = 0;
        for (int i = 0; i < S__COUNT; ++i)
            if (!tr((StrId)i) || !tr((StrId)i)[0]) ++empty;
        CHECK(empty == 0, "lang %d: %d empty strings", lang, empty);
    }
    str_set_lang(LANG_PT_);
    CHECK(!lstrcmpW(tr(S_NAV_EQ), L"Equalizador"), "pt-BR table");
    CHECK(!lstrcmpW(tr_preset_name("Classical"), L"Clássico") && !lstrcmpW(tr_preset_name("Deep Voice"), L"Voz grave"), "preset names translated");
    int untranslated = 0;
    for (int i = 0; i < e1_eq_preset_count; ++i) {
        wchar_t en[64];
        MultiByteToWideChar(CP_UTF8, 0, e1_eq_presets[i].name, -1, en, 64);
        if (!lstrcmpW(tr_preset_name(e1_eq_presets[i].name), en) && lstrcmpW(en, L"Country") && lstrcmpW(en, L"Dance") &&
            lstrcmpW(en, L"Jazz") && lstrcmpW(en, L"New Age") && lstrcmpW(en, L"Pop") && lstrcmpW(en, L"Rock") && lstrcmpW(en, L"Vocal"))
            ++untranslated;
    }
    CHECK(untranslated == 0, "%d EQ preset names of libe1dsp have no pt-BR entry", untranslated);
    untranslated = 0;
    for (int i = 0; i < e1_vfx_preset_count; ++i) {
        wchar_t en[64];
        MultiByteToWideChar(CP_UTF8, 0, e1_vfx_presets[i].name, -1, en, 64);
        if (!lstrcmpW(tr_preset_name(e1_vfx_presets[i].name), en) && lstrcmpW(en, L"Orc") && lstrcmpW(en, L"Emo")) ++untranslated;
    }
    CHECK(untranslated == 0, "%d VoiceFX preset names of libe1dsp have no pt-BR entry", untranslated);
    CHECK(e1_eq_preset_count == 10 && e1_vfx_preset_count == 18, "preset table sizes (%d, %d)", e1_eq_preset_count, e1_vfx_preset_count);
    str_set_lang(LANG_EN_);
    for (int i = 0; i < 4; ++i) {
        E1Profile p;
        profile_factory(i, NULL, &p);
        CHECK(!lstrcmpW(tr_factory(i), p.name), "factory name %d matches the English table", i);
    }
}

static void test_state(void)
{
    printf("Studio status and hot-key rule\n");
    E1Params d;
    e1fx_params_default(&d);
    E1RenderParams r = d.render;
    int sfx = -1;
    r.master_on = 0; r.eq_on = 0; r.scout_on = 0;
    CHECK(panel_studio_status(&r, &sfx) == S_STUDIO_OFF && sfx == 0 && !panel_render_processing(&r), "all off = bit-exact");
    r.eq_on = 1;                                           /* flat EQ: still bypassed */
    for (int b = 0; b < E1FX_EQ_BANDS; ++b) r.eq_gain_db[b] = 0.f;
    r.eq_preamp_db = 0.f;
    CHECK(panel_studio_status(&r, &sfx) == S_STUDIO_OFF, "flat EQ with master off = bit-exact");
    r.eq_gain_db[3] = 4.f;
    CHECK(panel_studio_status(&r, &sfx) == S_STUDIO_OFF_EQ && panel_render_processing(&r), "EQ on, master off: not bit-exact");
    r.eq_on = 0; r.scout_on = 1;
    CHECK(panel_studio_status(&r, &sfx) == S_STUDIO_OFF_SCOUT && panel_render_processing(&r), "Scout on, master off: not bit-exact");
    r.master_on = 1;
    CHECK(panel_studio_status(&r, &sfx) == S_STUDIO_ON && sfx == 1, "master on + Scout");
    str_set_lang(LANG_EN_);
    CHECK(wcsstr(tr(S_STUDIO_OFF_EQ), L"bit-exact") == NULL && wcsstr(tr(S_STUDIO_OFF_SCOUT), L"bit-exact") == NULL,
          "processing states never claim bit-exact");
    CHECK(wcsstr(tr(S_SPK_NOTE), L"Bass") != NULL && wcsstr(tr(S_SPK_NOTE), L"Only Surround") == NULL,
          "speaker note mentions that Bass depends on the mode");

    CHECK(!panel_hotkey_allowed(MOD_SHIFT, 'A'), "Shift+A rejected");
    CHECK(!panel_hotkey_allowed(MOD_SHIFT, VK_TAB) && !panel_hotkey_allowed(MOD_SHIFT, VK_SPACE), "Shift+Tab / Shift+Space rejected");
    CHECK(!panel_hotkey_allowed(0, 'A'), "plain A rejected");
    CHECK(panel_hotkey_allowed(MOD_CONTROL | MOD_SHIFT, 'S') && panel_hotkey_allowed(MOD_ALT, '1') && panel_hotkey_allowed(MOD_WIN, 'K'),
          "Ctrl/Alt/Win combinations accepted");
    CHECK(panel_hotkey_allowed(0, VK_F9) && panel_hotkey_allowed(MOD_SHIFT, VK_F12), "function keys accepted");

    wchar_t s[64];
    str_set_lang(LANG_PT_);
    lstrcpyW(s, L"+3.5 dB / 44.1 kHz / v1.2. x");
    str_decimal(s);
    CHECK(!lstrcmpW(s, L"+3,5 dB / 44,1 kHz / v1,2. x"), "pt-BR decimal comma (%ls)", s);
    CHECK(!lstrcmpW(tr(S_METER_L), L"E") && !lstrcmpW(tr(S_IMPORTED), L"Importado") && lstrcmpW(tr(S_ENDPOINT), L"Endpoint"),
          "pt-BR unit/label strings");
    str_set_lang(LANG_EN_);
    lstrcpyW(s, L"+3.5 dB");
    str_decimal(s);
    CHECK(!lstrcmpW(s, L"+3.5 dB"), "English keeps the decimal point");
}

static void test_heartbeat(void)
{
    printf("APO heartbeat classification\n");
    static E1SharedBlock blk;
    ParamSync s;
    E1ApoStatus st;
    memset(&s, 0, sizeof s);
    memset(&blk, 0, sizeof blk);
    blk.magic = E1FX_MAGIC;
    blk.block_size = sizeof blk;
    s.mapped = 1;
    s.map.block = &blk;
    CHECK(sync_engine(&s, 0, &st) == ENG_NOT_LOADED, "fresh block = not loaded");
    blk.render_status.sample_rate = 44100;
    blk.render_status.alive = 5;
    CHECK(sync_engine(&s, 0, &st) == ENG_RUNNING, "counter moved = running");
    blk.render_status.alive = 9;
    blk.render_status.peak_out[1] = 0.5f;
    CHECK(sync_engine(&s, 0, &st) == ENG_RUNNING && feq(st.peak_out[1], 0.5, 1e-6) && st.sample_rate == 44100, "status copied");
    Sleep(1700);
    CHECK(sync_engine(&s, 0, &st) == ENG_IDLE, "stalled counter = idle");
    CHECK(sync_engine(&s, 1, &st) == ENG_NOT_LOADED, "capture side independent");
    s.mapped = 0;
    CHECK(sync_engine(&s, 0, &st) == ENG_NOT_LOADED, "no mapping = not loaded");
}

int main(void)
{
    SetConsoleOutputCP(CP_UTF8);
    make_tmp();
    test_profile_roundtrip();
    test_profile_creative_input();
    test_factory();
    test_files();
    test_eqpresets();
    test_eqcurve();
    test_strings();
    test_state();
    test_heartbeat();
    rm_tmp();
    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
