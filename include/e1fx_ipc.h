/*
 * e1fx_ipc.h - E1 Studio IPC contract between the control panel (user session)
 * and the APO (audiodg.exe, LOCAL SERVICE, session 0).
 *
 * Transport:
 *   1. Named shared section E1FX_SECTION_NAME (Global namespace) holding E1SharedBlock.
 *      Created by whichever side comes first with an explicit security descriptor
 *      E1FX_SECTION_SDDL (SYSTEM/Admins/LocalService full, Interactive users read/write).
 *      The panel may lack SeCreateGlobalPrivilege: if CreateFileMapping on the Global name
 *      fails with ERROR_ACCESS_DENIED it must retry OpenFileMapping periodically and in the
 *      meantime only write the settings file.
 *   2. Persistent settings file E1FX_SETTINGS_PATH (raw E1SettingsFile). Written by the panel
 *      (atomic: write temp + MoveFileEx REPLACE), read by the APO when it creates the section.
 *      The installer grants Users modify + LOCAL SERVICE read on C:\ProgramData\E1Studio.
 *
 * Seqlock protocol (single writer = panel; readers = APO instances):
 *   writer: seq++ (becomes odd) ; MemoryBarrier ; copy params ; MemoryBarrier ; seq++ (even)
 *   reader (real-time thread, wait-free): s1 = seq ; if (s1 odd || s1 == last_seen) skip ;
 *           copy params to local ; MemoryBarrier ; if (seq != s1) skip (retry next buffer) ;
 *           last_seen = s1 ; e1fx_params_sanitize(local) ; apply.
 *
 * Status fields are written by the APO (per instance kind) without locking (torn reads are
 * acceptable: they are meters/diagnostics only).
 */
#ifndef E1FX_IPC_H
#define E1FX_IPC_H

#include <stddef.h>
#include <stdint.h>
#include "e1fx_params.h"

#define E1FX_SECTION_NAME   L"Global\\E1StudioFxParams_v1"
#define E1FX_SECTION_SDDL   L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGW;;;IU)(A;;GRGW;;;AU)"
#define E1FX_SETTINGS_DIR   L"C:\\ProgramData\\E1Studio"
#define E1FX_SETTINGS_PATH  L"C:\\ProgramData\\E1Studio\\settings.bin"
#define E1FX_MAGIC          0x58463145u /* 'E1FX' */

typedef struct E1ApoStatus {
    volatile int32_t  alive;          /* incremented by the APO every processed buffer (heartbeat) */
    volatile uint32_t sample_rate;    /* Hz, from LockForProcess */
    volatile uint32_t channels;
    volatile uint32_t bypass;         /* 1 when the chain is a bit-exact copy right now */
    volatile float    peak_in[2];     /* linear peak of the last ~100 ms, input */
    volatile float    peak_out[2];    /* output */
    volatile float    gain_reduction_db; /* limiter / smart volume reduction for the meter */
    volatile uint32_t xruns;          /* count of buffers where processing was skipped/failed */
} E1ApoStatus;

typedef struct E1SharedBlock {
    uint32_t          magic;          /* E1FX_MAGIC */
    uint32_t          block_size;     /* sizeof(E1SharedBlock) */
    volatile int32_t  seq;            /* seqlock counter, see above */
    uint32_t          reserved0;
    E1Params          params;         /* guarded by seq */
    E1ApoStatus       render_status;  /* written by the render APO */
    E1ApoStatus       capture_status; /* written by the capture APO */
} E1SharedBlock;

typedef struct E1SettingsFile {
    uint32_t magic;                   /* E1FX_MAGIC */
    uint32_t crc32;                   /* CRC-32 (IEEE, zlib polynomial) of `params` */
    E1Params params;
} E1SettingsFile;

/* ---- helper library: src/common/e1fx_ipc.c (owner: "apo" implementer; used by APO, panel, tools) ----
 * Needs <windows.h>; declared with opaque types so this header stays OS-neutral. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct E1IpcMapping {
    void          *section;   /* HANDLE */
    E1SharedBlock *block;     /* mapped view, NULL if not available */
    int            created;   /* 1 if this process created the section */
} E1IpcMapping;

/* Open the section; if it does not exist and allow_create != 0, create it with E1FX_SECTION_SDDL and
   initialise params from the settings file (or defaults). Returns 0 on success, Win32 error otherwise. */
unsigned long e1ipc_open(E1IpcMapping *m, int allow_create);
void          e1ipc_close(E1IpcMapping *m);

/* Seqlock writer (panel). */
void e1ipc_write_params(E1SharedBlock *b, const E1Params *p);
/* Seqlock reader, wait-free (APO real-time thread). Returns 1 and fills *out when a NEW consistent
   snapshot is available (compared with *last_seq, which it updates); 0 otherwise. */
int  e1ipc_read_params(const E1SharedBlock *b, int32_t *last_seq, E1Params *out);

/* Settings file (CRC-checked). load returns 0 on success; on any failure fills defaults and returns error. */
unsigned long e1ipc_load_settings(E1Params *p);
unsigned long e1ipc_save_settings(const E1Params *p);   /* atomic replace; creates E1FX_SETTINGS_DIR */
uint32_t      e1ipc_crc32(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* E1FX_IPC_H */
