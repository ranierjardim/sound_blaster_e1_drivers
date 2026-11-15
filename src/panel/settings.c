/*
 * settings.c - see settings.h.
 */
#include "settings.h"
#include <shlwapi.h>

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

DWORD settings_get_dword(const wchar_t *name, DWORD def)
{
    DWORD v = 0, sz = sizeof v, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, E1_SETTINGS_KEY, name, RRF_RT_REG_DWORD, &type, &v, &sz) != ERROR_SUCCESS)
        return def;
    return v;
}

void settings_set_dword(const wchar_t *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, E1_SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
}

int settings_get_str(const wchar_t *name, wchar_t *buf, DWORD cch)
{
    DWORD sz = cch * sizeof(wchar_t);
    if (cch == 0) return 0;
    buf[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, E1_SETTINGS_KEY, name, RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS) {
        buf[0] = 0;
        return 0;
    }
    buf[cch - 1] = 0;
    return 1;
}

void settings_set_str(const wchar_t *name, const wchar_t *s)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, E1_SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)s, (DWORD)((lstrlenW(s) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

static void run_command(wchar_t *out, DWORD cch)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { out[0] = 0; return; }
    wnsprintfW(out, (int)cch, L"\"%s\" /tray", exe);
}

int autostart_get(void)
{
    wchar_t cur[MAX_PATH * 2], want[MAX_PATH * 2];
    DWORD sz = sizeof cur;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, E1_RUN_VALUE, RRF_RT_REG_SZ, NULL, cur, &sz) != ERROR_SUCCESS)
        return 0;
    run_command(want, MAX_PATH * 2);
    return lstrcmpiW(cur, want) == 0;
}

int autostart_set(int on)
{
    HKEY k;
    LSTATUS st;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return 0;
    if (on) {
        wchar_t cmd[MAX_PATH * 2];
        run_command(cmd, MAX_PATH * 2);
        st = cmd[0] ? RegSetValueExW(k, E1_RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((lstrlenW(cmd) + 1) * sizeof(wchar_t)))
                    : ERROR_INVALID_DATA;
    } else {
        st = RegDeleteValueW(k, E1_RUN_VALUE);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}
