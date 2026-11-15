/*
 * test_params.c - defaults, sanitising, EQ preset table.
 */
#include "test_common.h"

int main(void)
{
    printf("test_params\n");
    E1Params p;
    e1fx_params_default(&p);
    CHECK(p.version == E1FX_PARAMS_VERSION && p.size == sizeof(E1Params), "header");
    CHECK(p.render.master_on == 0 && p.render.eq_on == 0 && p.capture.master_on == 0, "masters off by default");
    CHECK(p.render.surround_on == 1 && fabsf(p.render.surround_level - 0.40f) < 1e-6f, "surround default");
    CHECK(p.render.crystal_on == 1 && fabsf(p.render.crystal_level - 0.65f) < 1e-6f, "crystal default");
    CHECK(p.render.bass_level == 50.f && p.render.bass_xover_hz == 80.f, "bass default");
    CHECK(fabsf(p.render.svm_level - 0.74f) < 1e-6f && p.render.dialog_level == 0.5f, "svm/dialog default");
    CHECK(p.render.limiter_on == 1 && p.capture.nr_on == 1 && p.capture.vfx[1] == 1400.f, "misc defaults");

    E1Params q = p;
    e1fx_params_sanitize(&q);
    CHECK(memcmp(&p, &q, sizeof p) == 0, "defaults are a sanitize fixed point");

    q.render.master_on = 7;
    q.render.surround_level = NAN;
    q.render.crystal_level = 3.f;
    q.render.bass_level = -INFINITY;
    q.render.bass_xover_hz = 5.f;
    q.render.svm_mode = 9;
    q.render.eq_preamp_db = 40.f;
    q.render.eq_gain_db[3] = -100.f;
    q.render.eq_gain_db[4] = INFINITY;
    q.render.speaker_mode = 5;
    q.render.output_gain_db = 30.f;
    q.capture.vfx[3] = 10.f;
    q.capture.vfx[0] = NAN;
    q.capture.output_gain_db = -50.f;
    q.version = 0; q.size = 3;
    e1fx_params_sanitize(&q);
    CHECK(q.render.master_on == 1, "bool normalised");
    CHECK(fabsf(q.render.surround_level - 0.40f) < 1e-6f, "NaN -> default");
    CHECK(q.render.crystal_level == 1.f && q.render.bass_level == 50.f && q.render.bass_xover_hz == 10.f, "clamps 1");
    CHECK(q.render.svm_mode == E1FX_SVM_NORMAL && q.render.speaker_mode == E1FX_SPK_HEADPHONES, "enums");
    CHECK(q.render.eq_preamp_db == 12.f && q.render.eq_gain_db[3] == -24.f && q.render.eq_gain_db[4] == 0.f, "eq clamps");
    CHECK(q.render.output_gain_db == 12.f && q.capture.output_gain_db == -24.f, "gain clamps");
    CHECK(q.capture.vfx[3] == 4.f && q.capture.vfx[0] == 400.f, "vfx clamps");
    CHECK(q.version == E1FX_PARAMS_VERSION && q.size == sizeof(E1Params), "header restored");

    CHECK(e1_eq_preset_count == 10, "10 EQ presets");
    static const char *names[] = { "Flat", "Acoustic", "Classical", "Country", "Dance", "Jazz", "New Age", "Pop", "Rock", "Vocal" };
    for (int i = 0; i < 10 && i < e1_eq_preset_count; ++i) CHECK(strcmp(e1_eq_presets[i].name, names[i]) == 0, "preset %d name", i);
    static const float pop[10] = { -2, 0, 2, 2, 0, -1, -1, 0, 3, 6 }, vocal[10] = { -2, -1, -1, 0, 3, 4, 3, 0, 0, 1 };
    CHECK(memcmp(e1_eq_presets[7].gain_db, pop, sizeof pop) == 0, "Pop values");
    CHECK(memcmp(e1_eq_presets[9].gain_db, vocal, sizeof vocal) == 0, "Vocal values");
    return test_summary("test_params");
}
