/*
 * sync.h - panel side of the parameter IPC (include/e1fx_ipc.h): owns the shared section mapping,
 * publishes E1Params with the seqlock writer, and interprets the APO status/heartbeat.
 */
#ifndef E1P_SYNC_H
#define E1P_SYNC_H

#include <windows.h>
#include "../../include/e1fx_ipc.h"

typedef enum EngineState { ENG_NOT_LOADED = 0, ENG_IDLE = 1, ENG_RUNNING = 2 } EngineState;

typedef struct EngineTrack {
    int32_t last_alive;
    DWORD   last_change_ms;
    int     seen;
} EngineTrack;

typedef struct ParamSync {
    E1IpcMapping map;
    int          mapped;
    unsigned long open_err;     /* last e1ipc_open error (0 when mapped) */
    DWORD        last_try_ms;
    unsigned long save_err;     /* last e1ipc_save_settings result */
    EngineTrack  trk[2];        /* 0 render, 1 capture */
} ParamSync;

/* Opens (or creates) the section and fills *p with the live parameters: from the section when it
   already existed, otherwise from the settings file (defaults on any failure). */
void        sync_init(ParamSync *s, E1Params *p);
/* Retries the mapping every 2 s while it is unavailable; on success publishes *p. Returns 1 when the
   mapping state changed. */
int         sync_poll(ParamSync *s, const E1Params *p);
/* Publishes *p to the APO immediately (no-op when not mapped). */
void        sync_publish(ParamSync *s, const E1Params *p);
/* Writes the settings file (call debounced). Returns the Win32 error (0 = ok). */
unsigned long sync_save(ParamSync *s, const E1Params *p);
/* Copies the status of the render (capture=0) or capture (1) APO and classifies the heartbeat.
   Call at least every 500 ms for a stable result. */
EngineState sync_engine(ParamSync *s, int capture, E1ApoStatus *out);
void        sync_close(ParamSync *s);

#endif /* E1P_SYNC_H */
