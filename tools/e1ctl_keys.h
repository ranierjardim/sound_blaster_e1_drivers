/*
 * e1ctl_keys.h - parameter key table for e1ctl (pure C, no OS headers).
 *
 * Maps dotted, human-readable names ("render.surround_on", "render.eq_gain_db[3]",
 * alias "render.eq.250") onto fields of E1Params, with type, legal range and parsing/formatting.
 * Kept separate from e1ctl.c so it can be unit-tested without Windows.
 */
#ifndef E1CTL_KEYS_H
#define E1CTL_KEYS_H

#include <stddef.h>
#include <stdio.h>
#include "../include/e1fx_params.h"
#include "../include/e1fx_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum E1KeyType {
    E1K_BOOL = 0,   /* uint32_t 0/1 */
    E1K_ENUM,       /* uint32_t with symbolic names */
    E1K_UINT,       /* uint32_t integer range */
    E1K_FLOAT       /* float range */
} E1KeyType;

typedef struct E1KeyDef {
    const char        *name;        /* canonical name, e.g. "render.eq_gain_db[3]" */
    const char        *alias;       /* optional alias, e.g. "render.eq.250"; NULL if none */
    E1KeyType          type;
    size_t             offset;      /* byte offset inside E1Params */
    double             minv, maxv;  /* legal range (inclusive) */
    const char *const *enum_names;  /* E1K_ENUM: names indexed by value */
    int                enum_count;
    const char        *unit;        /* "", "dB", "Hz", ... */
    const char        *help;
} E1KeyDef;

extern const E1KeyDef e1k_keys[];
extern const int      e1k_key_count;

/* Case-insensitive lookup by canonical name or alias. Accepts "x[3]" and "x.3" for arrays. */
const E1KeyDef *e1k_find(const char *name);

/* Parse text for key k. Booleans accept 1/0/on/off/true/false/yes/no; enums accept the index or
   the symbolic name; numbers must be finite and inside [minv, maxv].
   Returns 1 on success (fills *out), 0 on error (fills err with a message). */
int    e1k_parse_value(const E1KeyDef *k, const char *text, double *out, char *err, size_t errsz);

double e1k_get(const E1KeyDef *k, const E1Params *p);
void   e1k_set(const E1KeyDef *k, E1Params *p, double v);

/* "1 (on)", "0.40", "-3.50 dB", "2 (night)". Always NUL-terminated. */
void   e1k_format(const E1KeyDef *k, const E1Params *p, char *buf, size_t n);

/* Describe the legal range: "0|1", "0..1", "headphones|stereo", ... */
void   e1k_describe_range(const E1KeyDef *k, char *buf, size_t n);

/* Preset helpers. Name match is case-insensitive and ignores spaces, '-' and '_'.
   Return the preset index or -1. */
int    e1k_find_eq_preset(const char *name);
int    e1k_find_vfx_preset(const char *name);
/* Apply an EQ preset: gains from the table, eq_on = 1, scout_on = 0 (Scout overrides the EQ). */
int    e1k_apply_eq_preset(E1Params *p, const char *name);
/* Apply a VoiceFX preset: vfx[] from the table, vfx_preset = index, vfx_on = 1. */
int    e1k_apply_vfx_preset(E1Params *p, const char *name);

/* Case-insensitive compare ignoring ' ', '-', '_' (used for preset names). */
int    e1k_loose_equal(const char *a, const char *b);

/* ---- report helpers (pure; used by "e1ctl status/get") ---------------------------------- */

/* Print every key whose name starts with prefix ("" = all, "render.", "capture.") as
   "  name = value". */
void   e1k_dump_params(FILE *f, const E1Params *p, const char *prefix);

/* Linear peak -> dBFS text ("-inf" for silence). */
void   e1k_format_dbfs(float peak, char *buf, size_t n);

/* Print one APO status block. `before` and `after` are two samples taken `dt_s` seconds apart
   (heartbeat delta gives buffers per second). Returns 1 if the heartbeat moved. */
int    e1k_print_status(FILE *f, const char *label, const E1ApoStatus *before,
                        const E1ApoStatus *after, double dt_s);

/* Validate a mapped shared block header. Returns 1 if magic and size match this build. */
int    e1k_block_compatible(const E1SharedBlock *b);

#ifdef __cplusplus
}
#endif
#endif /* E1CTL_KEYS_H */
