/*
 * e1dsp.h - public API of libe1dsp (pure C99, no OS headers, no allocation after *_create).
 *
 * Real-time rules for every *_process / *_set_params function:
 *   - no allocation, no locks, no I/O, no syscalls; bounded time;
 *   - accepts ANY frame count (0..1e6) and re-blocks internally to E1DSP_BLOCK frames;
 *   - in-place (in == out) must work; buffers are interleaved float32;
 *   - output is always finite (NaN/Inf-safe), denormal-safe.
 *
 * Bit-exact guarantee: when params->master_on == 0 AND (render) eq_on == 0 the chain copies
 * input to output bit-for-bit with ZERO latency (no FIFO in the path). When the chain switches
 * between bypass and active it crossfades over E1DSP_XFADE_MS so there are no clicks; the
 * reported latency stays constant while active.
 */
#ifndef E1DSP_H
#define E1DSP_H

#include <stddef.h>
#include "../../include/e1fx_params.h"

#ifdef __cplusplus
extern "C" {
#endif

#define E1DSP_BLOCK      128   /* internal block size in frames (matches Creative's module granularity) */
#define E1DSP_MAX_CH     8
#define E1DSP_XFADE_MS   20.0

typedef struct E1Render  E1Render;
typedef struct E1Capture E1Capture;

/* ---- render (playback) chain ------------------------------------------------------------ */
/* fs: 8000..384000. channels: 1..8 (processing is defined for stereo; other counts: effects that
   need stereo are applied to channels 0/1 only, the rest pass through delayed to stay aligned). */
E1Render *e1_render_create(double fs, int channels);
void      e1_render_destroy(E1Render *r);
void      e1_render_reset(E1Render *r);                         /* clear all state (stream restart) */
void      e1_render_set_params(E1Render *r, const E1RenderParams *p); /* RT-safe, takes effect smoothly */
void      e1_render_process(E1Render *r, const float *in, float *out, size_t frames);
int       e1_render_is_bypassed(const E1Render *r);             /* 1 = currently bit-exact copy */
unsigned  e1_render_latency_frames(const E1Render *r);          /* while active */
void      e1_render_meter(const E1Render *r, float peak_in[2], float peak_out[2], float *gr_db);

/* ---- capture (microphone) chain ---------------------------------------------------------- */
E1Capture *e1_capture_create(double fs, int channels);
void       e1_capture_destroy(E1Capture *c);
void       e1_capture_reset(E1Capture *c);
void       e1_capture_set_params(E1Capture *c, const E1CaptureParams *p);
void       e1_capture_process(E1Capture *c, const float *in, float *out, size_t frames);
int        e1_capture_is_bypassed(const E1Capture *c);
unsigned   e1_capture_latency_frames(const E1Capture *c);
void       e1_capture_meter(const E1Capture *c, float peak_in[2], float peak_out[2], float *gr_db);

/* ---- parameter helpers (implemented in e1fx_params.c) ----------------------------------- */
/* see include/e1fx_params.h: e1fx_params_default / e1fx_params_sanitize */

#ifdef __cplusplus
}
#endif
#endif /* E1DSP_H */
