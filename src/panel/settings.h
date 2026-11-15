/*
 * settings.h - per-user panel settings in HKCU\Software\E1Studio\Panel and the
 * "start with Windows" entry in HKCU\...\Run. (Effect parameters are NOT stored here: they live in
 * the shared settings file written through e1ipc_save_settings.)
 */
#ifndef E1P_SETTINGS_H
#define E1P_SETTINGS_H

#include <windows.h>

#define E1_SETTINGS_KEY L"Software\\E1Studio\\Panel"
#define E1_RUN_VALUE    L"E1Studio"

DWORD settings_get_dword(const wchar_t *name, DWORD def);
void  settings_set_dword(const wchar_t *name, DWORD v);
int   settings_get_str(const wchar_t *name, wchar_t *buf, DWORD cch);   /* 1 if present */
void  settings_set_str(const wchar_t *name, const wchar_t *s);

int   autostart_get(void);       /* 1 if the Run entry points at this exe */
int   autostart_set(int on);     /* 1 on success */

#endif /* E1P_SETTINGS_H */
