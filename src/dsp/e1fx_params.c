/*
 * e1fx_params.c - parameter defaults, sanitising and the graphic-EQ preset table.
 *
 * Defaults: both master switches OFF (bit-exact passthrough on first run), sub-effects at the
 * original control-panel defaults (spec 3.1) so that switching the master on reproduces the
 * familiar sound.
 */
#include <math.h>
#include <string.h>
#include "e1dsp_modules.h"

/* Graphic-EQ presets, dB per band 31.25 Hz .. 16 kHz (spec 3.7). */
const E1EqPreset e1_eq_presets[] = {
    { "Flat",      {  0.f,  0.f,  0.f, 0.f,  0.f,  0.f,  0.f, 0.f, 0.f, 0.f } },
    { "Acoustic",  {  0.f,  1.f,  2.f, 0.f,  0.f,  0.f,  0.f, 2.f, 2.f, 2.f } },
    { "Classical", {  0.f,  6.f,  6.f, 3.f,  0.f,  0.f,  0.f, 0.f, 3.f, 3.f } },
    { "Country",   { -1.f,  0.f,  1.f, 1.f,  1.f,  0.f,  0.f, 2.f, 3.f, 4.f } },
    { "Dance",     { -1.f,  2.f,  3.f, 4.f, -1.f, -1.f,  0.f, 0.f, 4.f, 4.f } },
    { "Jazz",      {  0.f,  0.f,  1.f, 4.f,  4.f,  4.f,  0.f, 1.f, 3.f, 3.f } },
    { "New Age",   {  0.f,  2.f,  2.f, 0.f,  0.f,  0.f,  1.f, 2.f, 2.f, 2.f } },
    { "Pop",       { -2.f,  0.f,  2.f, 2.f,  0.f, -1.f, -1.f, 0.f, 3.f, 6.f } },
    { "Rock",      { -1.f, -1.f,  1.f, 2.f, -1.f, -1.f,  0.f, 0.f, 4.f, 4.f } },
    { "Vocal",     { -2.f, -1.f, -1.f, 0.f,  3.f,  4.f,  3.f, 0.f, 0.f, 1.f } },
};
const int e1_eq_preset_count = (int)(sizeof(e1_eq_presets) / sizeof(e1_eq_presets[0]));

static const float k_vfx_def[E1FX_VFX_PARAMS] = { 400.f, 1400.f, 2000.f, 1.f, 1.f, 1.f, 0.f, 0.f };
static const float k_vfx_lo[E1FX_VFX_PARAMS]  = { 200.f,  800.f, 1600.f, 0.25f, 0.5f, -1.f, 0.f, 0.f };
static const float k_vfx_hi[E1FX_VFX_PARAMS]  = { 800.f, 1800.f, 2750.f, 4.f,   2.f,   5.f, 0.2f, 0.4f };

void e1fx_params_default(E1Params *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->version = E1FX_PARAMS_VERSION;
    p->size = (uint32_t)sizeof(E1Params);

    E1RenderParams *r = &p->render;
    r->master_on = 0;
    r->surround_on = 1;    r->surround_level = 0.40f;
    r->crystal_on = 1;     r->crystal_level = 0.65f;
    r->bass_on = 0;        r->bass_level = 50.f;  r->bass_xover_hz = 80.f;
    r->svm_on = 0;         r->svm_level = 0.74f;  r->svm_mode = E1FX_SVM_NORMAL;
    r->dialog_on = 0;      r->dialog_level = 0.5f;
    r->eq_on = 0;          r->eq_preamp_db = 0.f;
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) r->eq_gain_db[i] = 0.f;
    r->speaker_mode = E1FX_SPK_HEADPHONES;
    r->scout_on = 0;
    r->output_gain_db = 0.f;
    r->limiter_on = 1;

    E1CaptureParams *c = &p->capture;
    c->master_on = 0;
    c->nr_on = 1;          c->nr_strength = 0.5f;
    c->svm_on = 0;         c->svm_level = 0.74f;
    c->vfx_on = 0;         c->vfx_preset = 0;
    for (int i = 0; i < E1FX_VFX_PARAMS; ++i) c->vfx[i] = k_vfx_def[i];
    c->output_gain_db = 0.f;
}

static float clampf(float v, float lo, float hi, float def)
{
    if (!isfinite(v)) return def;
    return v < lo ? lo : (v > hi ? hi : v);
}
static uint32_t b01(uint32_t v) { return v ? 1u : 0u; }

void e1fx_params_sanitize(E1Params *p)
{
    if (!p) return;
    p->version = E1FX_PARAMS_VERSION;
    p->size = (uint32_t)sizeof(E1Params);

    E1RenderParams *r = &p->render;
    r->master_on = b01(r->master_on);
    r->surround_on = b01(r->surround_on);
    r->surround_level = clampf(r->surround_level, 0.f, 1.f, 0.40f);
    r->crystal_on = b01(r->crystal_on);
    r->crystal_level = clampf(r->crystal_level, 0.f, 1.f, 0.65f);
    r->bass_on = b01(r->bass_on);
    r->bass_level = clampf(r->bass_level, 0.f, 100.f, 50.f);
    r->bass_xover_hz = clampf(r->bass_xover_hz, 10.f, 1000.f, 80.f);
    r->svm_on = b01(r->svm_on);
    r->svm_level = clampf(r->svm_level, 0.f, 1.f, 0.74f);
    if (r->svm_mode > E1FX_SVM_NIGHT) r->svm_mode = E1FX_SVM_NORMAL;
    r->dialog_on = b01(r->dialog_on);
    r->dialog_level = clampf(r->dialog_level, 0.f, 1.f, 0.5f);
    r->eq_on = b01(r->eq_on);
    r->eq_preamp_db = clampf(r->eq_preamp_db, -12.f, 12.f, 0.f);
    for (int i = 0; i < E1FX_EQ_BANDS; ++i) r->eq_gain_db[i] = clampf(r->eq_gain_db[i], -24.f, 24.f, 0.f);
    if (r->speaker_mode > E1FX_SPK_STEREO) r->speaker_mode = E1FX_SPK_HEADPHONES;
    r->scout_on = b01(r->scout_on);
    r->output_gain_db = clampf(r->output_gain_db, -24.f, 12.f, 0.f);
    r->limiter_on = b01(r->limiter_on);

    E1CaptureParams *c = &p->capture;
    c->master_on = b01(c->master_on);
    c->nr_on = b01(c->nr_on);
    c->nr_strength = clampf(c->nr_strength, 0.f, 1.f, 0.5f);
    c->svm_on = b01(c->svm_on);
    c->svm_level = clampf(c->svm_level, 0.f, 1.f, 0.74f);
    c->vfx_on = b01(c->vfx_on);
    /* vfx_preset is informational (UI index, may refer to a user preset): left as is. */
    for (int i = 0; i < E1FX_VFX_PARAMS; ++i) c->vfx[i] = clampf(c->vfx[i], k_vfx_lo[i], k_vfx_hi[i], k_vfx_def[i]);
    c->output_gain_db = clampf(c->output_gain_db, -24.f, 24.f, 0.f);
}
