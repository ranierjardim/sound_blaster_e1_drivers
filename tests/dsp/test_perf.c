/*
 * test_perf.c - throughput of the render chain with every effect on (44.1 kHz stereo, 441-frame
 * buffers like the Windows engine's 10 ms period), plus capture and bypass for reference.
 * Requirement: >= 50x real time.
 */
#include "test_common.h"

static double run_render(const E1RenderParams *p, double seconds, int nch)
{
    const double fs = 44100;
    const size_t chunk = 441, total = (size_t)(fs * seconds);
    E1Render *r = e1_render_create(fs, nch);
    e1_render_set_params(r, p);
    float *buf = (float *)malloc(chunk * nch * sizeof(float)), *src = (float *)malloc(chunk * 64 * nch * sizeof(float));
    rng_seed(1);
    for (size_t i = 0; i < chunk * 64 * nch; ++i) src[i] = 0.5f * rng_sym();
    double best = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
        double t0 = now_s();
        for (size_t off = 0, k = 0; off < total; off += chunk, ++k) {
            memcpy(buf, src + (k % 64) * chunk * nch, chunk * nch * sizeof(float));
            e1_render_process(r, buf, buf, chunk);
        }
        double t = now_s() - t0;
        if (t < best) best = t;
    }
    free(buf); free(src);
    e1_render_destroy(r);
    return seconds / best;
}

static double run_capture(const E1CaptureParams *p, double seconds)
{
    const double fs = 44100;
    const size_t chunk = 441, total = (size_t)(fs * seconds);
    E1Capture *c = e1_capture_create(fs, 1);
    e1_capture_set_params(c, p);
    float buf[441];
    rng_seed(2);
    double t0 = now_s();
    for (size_t off = 0; off < total; off += chunk) {
        for (size_t i = 0; i < chunk; ++i) buf[i] = 0.3f * rng_sym();
        e1_capture_process(c, buf, buf, chunk);
    }
    double t = now_s() - t0;
    e1_capture_destroy(c);
    return seconds / t;
}

int main(void)
{
    printf("test_perf\n");
    E1Params d;
    e1fx_params_default(&d);
    E1RenderParams p = d.render;
    p.master_on = 1;
    p.surround_on = p.crystal_on = p.bass_on = p.svm_on = p.dialog_on = 1;
    p.eq_on = 1;
    memcpy(p.eq_gain_db, e1_eq_presets[8].gain_db, sizeof p.eq_gain_db);
    p.output_gain_db = 2.f;
    p.limiter_on = 1;
    double x_all = run_render(&p, 60.0, 2);
    printf("  render, all effects on, 44.1 kHz stereo: %.0fx real time (%.3f%% of one core)\n", x_all, 100.0 / x_all);
    CHECK(x_all >= 50.0, "render all-on only %.1fx real time", x_all);

    E1RenderParams core = d.render;
    core.master_on = 1;
    core.surround_on = core.crystal_on = core.svm_on = core.dialog_on = 0;
    core.bass_on = 1; core.eq_on = 1;
    memcpy(core.eq_gain_db, e1_eq_presets[8].gain_db, sizeof core.eq_gain_db);
    core.output_gain_db = 2.f;
    double x_core = run_render(&core, 60.0, 2);
    printf("  render, EQ + Bass + trim + limiter only (dsp-core modules): %.0fx real time\n", x_core);

    E1RenderParams idle = d.render;
    idle.master_on = 1;
    idle.surround_on = idle.crystal_on = 0;
    idle.output_gain_db = -0.5f;
    double x_idle = run_render(&idle, 60.0, 2);
    printf("  render active, no effects (FIFO + latency alignment + trim): %.0fx real time\n", x_idle);

    double x_byp = run_render(&d.render, 60.0, 2);
    printf("  render bypass (bit-exact copy): %.0fx real time\n", x_byp);

    E1CaptureParams q = d.capture;
    q.master_on = 1; q.nr_on = 1; q.svm_on = 1; q.vfx_on = 1; q.output_gain_db = 3.f;
    double x_cap = run_capture(&q, 30.0);
    printf("  capture, NR + VoiceFX + SVM + gain + limiter, 44.1 kHz mono: %.0fx real time\n", x_cap);
    CHECK(x_cap >= 50.0, "capture only %.1fx real time", x_cap);
    return test_summary("test_perf");
}
