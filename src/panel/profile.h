/*
 * profile.h - audio profiles (XML, element names compatible with the original E-Series panel,
 * docs/re/control-panel.md 6) and the 4 factory profiles. No UI code; unit-tested in tests/panel.
 */
#ifndef E1P_PROFILE_H
#define E1P_PROFILE_H

#include <windows.h>
#include <stddef.h>
#include "../../include/e1fx_params.h"

#define PROFILE_NAME_MAX 64
#define PROFILE_MAX_USER 256
#define E1_DEVICE_ID_STR "USB_VID_041E_PID_323B"

typedef struct E1Profile {
    wchar_t  name[PROFILE_NAME_MAX];
    wchar_t  avatar[64];
    int      profile_id;     /* 4/5/7/8 for factory profiles, 100 for user profiles */
    int      factory;        /* 1 = built-in (never written to disk) */
    int      eq_preset_id;   /* <graphic_eq preset="..."> 1..10 standard, 0 custom */
    int      has_ext;        /* 1 when the file carried the <e1studio> extension element */
    E1Params params;
} E1Profile;

/* XML text (UTF-8, with declaration). Returns a malloc'd NUL-terminated buffer (free()), NULL on OOM. */
char *profile_to_xml(const E1Profile *pr, size_t *len_out);
/* Parses a profile. Settings absent in the file keep the value from `base` (may be NULL = defaults).
   Accepts UTF-8 (with/without BOM) and UTF-16LE with BOM. Returns 1 on success, 0 if not a profile. */
int   profile_from_xml(const char *xml, size_t len, const E1Params *base, E1Profile *out);

int   profile_save(const wchar_t *path, const E1Profile *pr);              /* atomic; 1 = ok */
int   profile_load(const wchar_t *path, const E1Params *base, E1Profile *out); /* 1 = ok */

int   profile_factory_count(void);
/* Factory profile i applied on top of `base` (speaker mode, trims and limiter are kept). */
void  profile_factory(int i, const E1Params *base, E1Profile *out);

/* %APPDATA%\E1Studio\profiles (create != 0 creates the folders). Returns 1 on success. */
int   profile_dir(wchar_t *out, size_t cch, int create);
/* %APPDATA%\E1Studio (create != 0 creates it). */
int   app_data_dir(wchar_t *out, size_t cch, int create);

typedef struct ProfileEntry {
    wchar_t name[PROFILE_NAME_MAX];
    wchar_t path[MAX_PATH];
} ProfileEntry;
/* User profiles found in dir (*.xml that parse as profiles), sorted with StrCmpLogicalW. */
int   profile_list(const wchar_t *dir, ProfileEntry *out, int max);

/* "name" -> "name.xml" with characters that are invalid in file names replaced. */
void  profile_filename(const wchar_t *name, wchar_t *out, size_t cch);
/* Names for profiles and presets: 1..32 chars, no control chars and none of \ / : * ? " < > | [ ] */
int   e1_name_valid(const wchar_t *name);

/* Generic helpers shared with the tests. */
char *e1_read_file(const wchar_t *path, size_t *len_out);   /* malloc'd, NUL-terminated; max 4 MB */
int   e1_write_file_atomic(const wchar_t *path, const void *data, size_t len);

#endif /* E1P_PROFILE_H */
