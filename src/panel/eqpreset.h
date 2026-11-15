/*
 * eqpreset.h - user ("custom") equalizer presets stored in %APPDATA%\E1Studio\eq_presets.ini
 * (UTF-8 INI: one [name] section per preset with preamp= and bands= keys). No UI code.
 */
#ifndef E1P_EQPRESET_H
#define E1P_EQPRESET_H

#include <windows.h>
#include "../../include/e1fx_params.h"

#define EQP_NAME_MAX 33   /* 32 characters + NUL, like the original panel */
#define EQP_MAX      128

typedef struct EqPreset {
    wchar_t name[EQP_NAME_MAX];
    float   preamp_db;
    float   gain_db[E1FX_EQ_BANDS];
} EqPreset;

/* Parses ini text (UTF-8). Returns the number of presets (sorted with StrCmpLogicalW). */
int  eqp_parse(const char *text, size_t len, EqPreset *out, int max);
/* Serialises to UTF-8 (malloc'd, NUL-terminated; free()). */
char *eqp_format(const EqPreset *list, int n, size_t *len_out);

int  eqp_load(const wchar_t *path, EqPreset *out, int max);   /* missing file -> 0 presets */
int  eqp_save(const wchar_t *path, const EqPreset *list, int n); /* atomic; 1 = ok */

int  eqp_find(const EqPreset *list, int n, const wchar_t *name);          /* case-insensitive, -1 if none */
int  eqp_upsert(EqPreset *list, int *n, int max, const EqPreset *p);      /* returns index after sort, -1 if full */
int  eqp_remove(EqPreset *list, int *n, const wchar_t *name);             /* 1 if removed */
void eqp_sort(EqPreset *list, int n);

/* %APPDATA%\E1Studio\eq_presets.ini */
int  eqp_default_path(wchar_t *out, size_t cch, int create_dir);

#endif /* E1P_EQPRESET_H */
