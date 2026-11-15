/*
 * sync.c - see sync.h.
 */
#include "sync.h"
#include <string.h>

#define RETRY_MS       2000u
#define HEARTBEAT_MS   1500u

static int valid_block(const E1SharedBlock *b)
{
    return b && b->magic == E1FX_MAGIC && b->block_size == sizeof(E1SharedBlock);
}

void sync_init(ParamSync *s, E1Params *p)
{
    memset(s, 0, sizeof *s);
    s->last_try_ms = GetTickCount();
    s->open_err = e1ipc_open(&s->map, 1);
    if (s->open_err == 0 && s->map.block) {
        s->mapped = 1;
        if (!s->map.created && valid_block(s->map.block)) {
            int32_t last = -1;
            E1Params tmp;
            if (e1ipc_read_params(s->map.block, &last, &tmp) && tmp.version == E1FX_PARAMS_VERSION &&
                tmp.size == sizeof(E1Params)) {
                e1fx_params_sanitize(&tmp);
                *p = tmp;
                return;
            }
        }
        e1ipc_load_settings(p);
        e1fx_params_sanitize(p);
        e1ipc_write_params(s->map.block, p);
        return;
    }
    s->mapped = 0;
    e1ipc_load_settings(p);
    e1fx_params_sanitize(p);
}

int sync_poll(ParamSync *s, const E1Params *p)
{
    if (s->mapped) {
        if (!valid_block(s->map.block)) { /* should never happen; drop and retry */
            e1ipc_close(&s->map);
            s->mapped = 0;
            return 1;
        }
        return 0;
    }
    DWORD now = GetTickCount();
    if (now - s->last_try_ms < RETRY_MS) return 0;
    s->last_try_ms = now;
    s->open_err = e1ipc_open(&s->map, 1);
    if (s->open_err == 0 && s->map.block) {
        s->mapped = 1;
        e1ipc_write_params(s->map.block, p);
        return 1;
    }
    return 0;
}

void sync_publish(ParamSync *s, const E1Params *p)
{
    if (s->mapped && s->map.block) e1ipc_write_params(s->map.block, p);
}

unsigned long sync_save(ParamSync *s, const E1Params *p)
{
    s->save_err = e1ipc_save_settings(p);
    return s->save_err;
}

EngineState sync_engine(ParamSync *s, int capture, E1ApoStatus *out)
{
    EngineTrack *t = &s->trk[capture ? 1 : 0];
    memset(out, 0, sizeof *out);
    if (!s->mapped || !valid_block(s->map.block)) return ENG_NOT_LOADED;
    const volatile E1ApoStatus *st = capture ? &s->map.block->capture_status : &s->map.block->render_status;
    out->alive = st->alive;
    out->sample_rate = st->sample_rate;
    out->channels = st->channels;
    out->bypass = st->bypass;
    out->peak_in[0] = st->peak_in[0];
    out->peak_in[1] = st->peak_in[1];
    out->peak_out[0] = st->peak_out[0];
    out->peak_out[1] = st->peak_out[1];
    out->gain_reduction_db = st->gain_reduction_db;
    out->xruns = st->xruns;
    DWORD now = GetTickCount();
    if (!t->seen) {
        t->seen = 1;
        t->last_alive = out->alive;
        t->last_change_ms = now - HEARTBEAT_MS - 1;
    } else if (out->alive != t->last_alive) {
        t->last_alive = out->alive;
        t->last_change_ms = now;
    }
    if (now - t->last_change_ms < HEARTBEAT_MS) return ENG_RUNNING;
    if (out->alive != 0 || out->sample_rate != 0) return ENG_IDLE;
    return ENG_NOT_LOADED;
}

void sync_close(ParamSync *s)
{
    if (s->mapped) e1ipc_close(&s->map);
    s->mapped = 0;
}
