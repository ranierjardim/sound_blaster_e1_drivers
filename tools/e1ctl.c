/*
 * e1ctl.c - E1 Studio command-line control tool.
 *
 *   e1ctl [--dry-run] <command> [args]
 *     status [--watch]          APO heartbeat, format, bypass, meters + current parameters
 *     get [key ...]             print all parameters or the given keys
 *     set <key> <value> [...]   change parameters (live through shared memory + saved to settings.bin)
 *     defaults [all|render|capture]
 *     preset eq <name>|list     apply/list equalizer presets
 *     preset vfx <name>|list    apply/list voice presets
 *     keys                      list every key with its range
 *     find                      list Sound Blaster E1 endpoints, device format, effects and spatial sound
 *     init-settings             write default settings.bin if it does not exist (used by the installer)
 *     version
 *
 * Talks to the APO through src/common/e1fx_ipc.c (shared section + settings file). Never needs admin.
 * Exit codes: 0 ok, 1 usage error, 2 runtime error.
 */
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <propidl.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/e1fx_params.h"
#include "../include/e1fx_ipc.h"
#include "../include/e1fx_guids.h"
#include "../src/dsp/e1dsp_modules.h"
#include "e1ctl_keys.h"

#define E1CTL_VERSION "1.0.0"

/* Our own copies of the COM identities so we do not depend on what uuid.lib happens to carry. */
DEFINE_GUID(E1CTL_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(E1CTL_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(E1CTL_IID_IMMEndpoint,          0x1BE09788, 0x6894, 0x4089, 0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5);
DEFINE_GUID(E1CTL_IID_ISpatialAudioClient,  0xBBF8E066, 0xAAAA, 0x49BE, 0x9A, 0x4D, 0xFD, 0x2A, 0x85, 0x8E, 0xA2, 0x7F);
DEFINE_GUID(E1CTL_SUBTYPE_PCM,              0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(E1CTL_SUBTYPE_FLOAT,            0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
/* PKEY_AudioEngine_DeviceFormat {f19f064d-082c-4e27-bc73-6882a1bb8e4c},0 */
DEFINE_PROPERTYKEY(E1CTL_PKEY_DeviceFormat, 0xf19f064d, 0x082c, 0x4e27, 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c, 0);
/* PKEY_Device_FriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0},14 */
DEFINE_PROPERTYKEY(E1CTL_PKEY_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);

#define MMDEV_ROOT L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\"
#define E1_HW_ID   L"VID_041E&PID_323B"

/* Registry value names (PROPERTYKEY "{fmtid},pid" as stored by the endpoint builder). */
#define PK_INSTANCE_ID   L"{b3f8fa53-0004-438e-9003-51a46e139bfc},2"
#define PK_DEVICE_PATH   L"{233164c8-1b2c-4c7d-bc68-b671687a2567},1"
#define PK_JACK_SUBTYPE  L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},8"
#define PK_DISABLE_SYSFX L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5"
#define PK_SPATIAL_FMT   L"{8a845654-d6c3-4cd7-b4eb-243d4bd99032},2"
#define PK_FX_FMT        L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},%d"

typedef struct Ctx {
    E1IpcMapping map;
    int          have_block;
    int          dry_run;
} Ctx;

static volatile LONG g_stop = 0;
static char g_settings_u8[1024];

/* Effective settings file path, mirroring src/common/e1fx_ipc.c (which honours the test-only
   E1FX_SETTINGS_OVERRIDE variable). Used for messages and the init-settings existence check. */
static const wchar_t *settings_path(wchar_t *buf, DWORD cch)
{
    DWORD n = GetEnvironmentVariableW(L"E1FX_SETTINGS_OVERRIDE", buf, cch);
    if (n > 0 && n < cch) return buf;
    return E1FX_SETTINGS_PATH;
}

static const char *settings_path_u8(void)
{
    wchar_t buf[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, settings_path(buf, MAX_PATH * 2), -1, g_settings_u8,
                        (int)sizeof g_settings_u8, NULL, NULL);
    g_settings_u8[sizeof g_settings_u8 - 1] = 0;
    return g_settings_u8;
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

/* ---------------------------------------------------------------------------------------- */

static void print_win32_error(const char *what, unsigned long err)
{
    wchar_t msg[512];
    char utf8[1024];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0,
                             msg, (DWORD)(sizeof msg / sizeof msg[0]), NULL);
    while (n && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' || msg[n - 1] == L' ')) msg[--n] = 0;
    if (!n) wcscpy_s(msg, sizeof msg / sizeof msg[0], L"unknown error");
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8, (int)sizeof utf8, NULL, NULL);
    fprintf(stderr, "e1ctl: %s failed: %lu (%s)\n", what, err, utf8);
}

static void w2u(const wchar_t *w, char *out, size_t n)
{
    if (!n) return;
    out[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    out[n - 1] = 0;
}

static void ctx_open(Ctx *c)
{
    unsigned long err;
    memset(&c->map, 0, sizeof c->map);
    c->have_block = 0;
    err = e1ipc_open(&c->map, 0);
    if (err == 0 && c->map.block) {
        if (e1k_block_compatible(c->map.block)) {
            c->have_block = 1;
        } else {
            fprintf(stderr, "e1ctl: shared block has an unexpected layout (magic 0x%08X, size %u, expected %u); "
                            "ignoring it. Is the installed APO from a different version?\n",
                    (unsigned)c->map.block->magic, (unsigned)c->map.block->block_size,
                    (unsigned)sizeof(E1SharedBlock));
        }
    }
}

static void ctx_close(Ctx *c)
{
    if (c->map.block || c->map.section) e1ipc_close(&c->map);
    c->have_block = 0;
}

/* Current parameters: the live block wins (the panel and APO use it); otherwise the settings file. */
static const char *load_params(Ctx *c, E1Params *p)
{
    if (c->have_block) {
        int tries;
        for (tries = 0; tries < 200; ++tries) {
            int32_t last = -1; /* odd: never equal to a stable (even) sequence number */
            if (e1ipc_read_params(c->map.block, &last, p)) {
                e1fx_params_sanitize(p);
                return "shared memory (live)";
            }
            Sleep(1);
        }
        fprintf(stderr, "e1ctl: shared block stayed busy; falling back to the settings file\n");
    }
    if (e1ipc_load_settings(p) == 0) {
        static char src[1100];
        e1fx_params_sanitize(p);
        snprintf(src, sizeof src, "settings file %s", settings_path_u8());
        return src;
    }
    e1fx_params_default(p);
    return "built-in defaults (settings file missing or invalid)";
}

static int commit_params(Ctx *c, E1Params *p)
{
    unsigned long err;
    e1fx_params_sanitize(p);
    if (c->dry_run) {
        printf("(dry run: nothing written)\n");
        return 0;
    }
    if (c->have_block) {
        e1ipc_write_params(c->map.block, p);
        printf("applied live to the running APO\n");
    } else {
        printf("APO shared block not available: change takes effect when the APO next starts\n");
    }
    err = e1ipc_save_settings(p);
    if (err) {
        print_win32_error("saving settings file", err);
        return c->have_block ? 0 : 2;
    }
    printf("saved to %s\n", settings_path_u8());
    return 0;
}

/* ------------------------------------------------------------------------------- status */

static void print_params_header(const char *src)
{
    printf("Parameters (source: %s)\n", src);
}

static int cmd_status(Ctx *c, int argc, char **argv)
{
    E1Params p;
    const char *src;
    int watch = 0, i;
    for (i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--watch") || !strcmp(argv[i], "-w")) watch = 1;
        else { fprintf(stderr, "e1ctl status: unknown option '%s'\n", argv[i]); return 1; }
    }

    printf("E1 Studio status\n");
    if (!c->have_block) {
        printf("APO shared block: not available.\n"
               "  The E1 Studio APO creates it when audiodg loads it (first stream on the E1 after the audio\n"
               "  service starts). Check 'e1ctl find' to confirm the APO is registered on the endpoints.\n");
    } else {
        E1ApoStatus r0, c0, r1, c1;
        const double dt = 0.4;
        r0 = *(const E1ApoStatus *)&c->map.block->render_status;
        c0 = *(const E1ApoStatus *)&c->map.block->capture_status;
        Sleep((DWORD)(dt * 1000));
        r1 = *(const E1ApoStatus *)&c->map.block->render_status;
        c1 = *(const E1ApoStatus *)&c->map.block->capture_status;
        printf("APO shared block: open (%s)\n", c->map.created ? "created by e1ctl" : "created by the APO");
        e1k_print_status(stdout, "Render (playback)", &r0, &r1, dt);
        e1k_print_status(stdout, "Capture (microphone)", &c0, &c1, dt);

        if (watch) {
            SetConsoleCtrlHandler(ctrl_handler, TRUE);
            printf("\nwatching meters (Ctrl+C to stop)\n");
            while (!g_stop) {
                char a[16], b[16], d[16], e[16], m[16];
                E1ApoStatus r = *(const E1ApoStatus *)&c->map.block->render_status;
                E1ApoStatus q = *(const E1ApoStatus *)&c->map.block->capture_status;
                e1k_format_dbfs(r.peak_out[0], a, sizeof a);
                e1k_format_dbfs(r.peak_out[1], b, sizeof b);
                e1k_format_dbfs(r.peak_in[0], d, sizeof d);
                e1k_format_dbfs(r.peak_in[1], e, sizeof e);
                e1k_format_dbfs(q.peak_out[0], m, sizeof m);
                printf("\rplay in %s/%s out %s/%s dBFS  GR %5.1f dB  %s | mic out %s dBFS  %s   ",
                       d, e, a, b, (double)r.gain_reduction_db, r.bypass ? "bypass" : "active",
                       m, q.bypass ? "bypass" : "active");
                fflush(stdout);
                Sleep(250);
            }
            printf("\n");
        }
    }
    src = load_params(c, &p);
    printf("\n");
    print_params_header(src);
    e1k_dump_params(stdout, &p, "");
    return 0;
}

/* ---------------------------------------------------------------------------------- get */

static int cmd_get(Ctx *c, int argc, char **argv)
{
    E1Params p;
    const char *src = load_params(c, &p);
    int i, rc = 0;
    if (argc == 0) {
        print_params_header(src);
        e1k_dump_params(stdout, &p, "");
        return 0;
    }
    for (i = 0; i < argc; ++i) {
        const E1KeyDef *k = e1k_find(argv[i]);
        char val[64];
        if (!k) {
            /* "render" / "capture" prefixes print a whole group */
            if (!_stricmp(argv[i], "render") || !_stricmp(argv[i], "capture")) {
                char pre[16];
                snprintf(pre, sizeof pre, "%s.", argv[i]);
                _strlwr_s(pre, sizeof pre);
                e1k_dump_params(stdout, &p, pre);
                continue;
            }
            fprintf(stderr, "e1ctl: unknown key '%s' (see 'e1ctl keys')\n", argv[i]);
            rc = 1;
            continue;
        }
        e1k_format(k, &p, val, sizeof val);
        printf("%s = %s\n", k->name, val);
    }
    return rc;
}

/* ---------------------------------------------------------------------------------- set */

static int cmd_set(Ctx *c, int argc, char **argv)
{
    E1Params p, before;
    const char *src;
    int i;
    if (argc < 2 || (argc % 2) != 0) {
        fprintf(stderr, "usage: e1ctl set <key> <value> [<key> <value> ...]\n");
        return 1;
    }
    src = load_params(c, &p);
    before = p;
    /* validate everything first: either all changes apply or none */
    for (i = 0; i < argc; i += 2) {
        const E1KeyDef *k = e1k_find(argv[i]);
        double v;
        char err[256];
        if (!k) {
            fprintf(stderr, "e1ctl: unknown key '%s' (see 'e1ctl keys')\n", argv[i]);
            return 1;
        }
        if (!e1k_parse_value(k, argv[i + 1], &v, err, sizeof err)) {
            fprintf(stderr, "e1ctl: %s\n", err);
            return 1;
        }
        e1k_set(k, &p, v);
    }
    printf("base: %s\n", src);
    for (i = 0; i < argc; i += 2) {
        const E1KeyDef *k = e1k_find(argv[i]);
        char a[64], b[64];
        e1k_format(k, &before, a, sizeof a);
        e1k_format(k, &p, b, sizeof b);
        printf("  %-28s %s -> %s\n", k->name, a, b);
    }
    return commit_params(c, &p);
}

/* ----------------------------------------------------------------------------- defaults */

static int cmd_defaults(Ctx *c, int argc, char **argv)
{
    E1Params p, d;
    const char *which = argc > 0 ? argv[0] : "all";
    load_params(c, &p);
    e1fx_params_default(&d);
    if (!_stricmp(which, "all")) p = d;
    else if (!_stricmp(which, "render")) p.render = d.render;
    else if (!_stricmp(which, "capture")) p.capture = d.capture;
    else { fprintf(stderr, "usage: e1ctl defaults [all|render|capture]\n"); return 1; }
    printf("restoring defaults (%s): effects off, bit-exact passthrough\n", which);
    return commit_params(c, &p);
}

/* ------------------------------------------------------------------------------- preset */

static int cmd_preset(Ctx *c, int argc, char **argv)
{
    E1Params p;
    int i, idx;
    if (argc < 2) {
        fprintf(stderr, "usage: e1ctl preset eq <name>|list\n       e1ctl preset vfx <name>|list\n");
        return 1;
    }
    if (!_stricmp(argv[0], "eq")) {
        if (!_stricmp(argv[1], "list")) {
            for (i = 0; i < e1_eq_preset_count; ++i) {
                int b;
                printf("  %-12s", e1_eq_presets[i].name);
                for (b = 0; b < E1FX_EQ_BANDS; ++b) printf(" %+5.1f", (double)e1_eq_presets[i].gain_db[b]);
                printf("\n");
            }
            return 0;
        }
        load_params(c, &p);
        idx = e1k_apply_eq_preset(&p, argv[1]);
        if (idx < 0) {
            fprintf(stderr, "e1ctl: unknown EQ preset '%s' (see 'e1ctl preset eq list')\n", argv[1]);
            return 1;
        }
        printf("equalizer preset '%s' applied (equalizer on)\n", e1_eq_presets[idx].name);
        return commit_params(c, &p);
    }
    if (!_stricmp(argv[0], "vfx") || !_stricmp(argv[0], "voice")) {
        if (!_stricmp(argv[1], "list")) {
            for (i = 0; i < e1_vfx_preset_count; ++i) {
                int b;
                printf("  %2d %-20s", i, e1_vfx_presets[i].name);
                for (b = 0; b < E1FX_VFX_PARAMS; ++b) printf(" %g", (double)e1_vfx_presets[i].p[b]);
                printf("\n");
            }
            return 0;
        }
        load_params(c, &p);
        idx = e1k_apply_vfx_preset(&p, argv[1]);
        if (idx < 0) {
            fprintf(stderr, "e1ctl: unknown voice preset '%s' (see 'e1ctl preset vfx list')\n", argv[1]);
            return 1;
        }
        printf("voice preset '%s' applied (voice effects on; capture.master_on must be 1 to hear it)\n",
               e1_vfx_presets[idx].name);
        return commit_params(c, &p);
    }
    fprintf(stderr, "e1ctl: unknown preset family '%s' (eq|vfx)\n", argv[0]);
    return 1;
}

/* --------------------------------------------------------------------------------- keys */

static int cmd_keys(void)
{
    int i;
    for (i = 0; i < e1k_key_count; ++i) {
        char range[160], name[96];
        const E1KeyDef *k = &e1k_keys[i];
        e1k_describe_range(k, range, sizeof range);
        if (k->alias) snprintf(name, sizeof name, "%s (%s)", k->name, k->alias);
        else snprintf(name, sizeof name, "%s", k->name);
        printf("  %-46s %-24s %s\n", name, range, k->help);
    }
    return 0;
}

/* ---------------------------------------------------------------------------------- find */

static LSTATUS reg_read(const wchar_t *subkey, const wchar_t *value, DWORD *type, void *buf, DWORD *cb)
{
    return RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value, RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY | RRF_NOEXPAND,
                        type, buf, cb);
}

static const char *state_name(DWORD s)
{
    switch (s) {
    case DEVICE_STATE_ACTIVE: return "active";
    case DEVICE_STATE_DISABLED: return "disabled";
    case DEVICE_STATE_NOTPRESENT: return "not present";
    case DEVICE_STATE_UNPLUGGED: return "unplugged";
    default: return "?";
    }
}

static const char *clsid_owner(const wchar_t *s)
{
    static const wchar_t *const creative[] = {
        L"{559493DD-3D1F-40a7-8E94-95762EAD6E95}", L"{2EF9BA5B-FA37-4195-95C5-9483B6FE2104}",
        L"{E9B73398-425E-4c41-B4C9-F4D1E6922A27}", L"{41528545-B167-495e-BB36-E11A17CF64B3}",
    };
    size_t i;
    if (!_wcsicmp(s, E1FX_RENDER_CLSID_STR)) return "E1 Studio render";
    if (!_wcsicmp(s, E1FX_CAPTURE_CLSID_STR)) return "E1 Studio capture";
    for (i = 0; i < sizeof creative / sizeof creative[0]; ++i)
        if (!_wcsicmp(s, creative[i])) return "Creative";
    return "other";
}

static const char *spatial_name(const GUID *g)
{
    static const struct { GUID g; const char *n; } known[] = {
        { { 0x1459AC38, 0x3875, 0x49BF, { 0xBB, 0x59, 0x0F, 0xE8, 0x0F, 0x4D, 0x39, 0x5D } }, "Dolby Atmos for Headphones" },
        { { 0x4C81E564, 0xC8EF, 0x4AD9, { 0x9F, 0x2C, 0x9E, 0xF9, 0x95, 0x53, 0x37, 0x90 } }, "Dolby Atmos for Speakers" },
        { { 0xB53D940C, 0xB846, 0x4831, { 0x9F, 0x76, 0xD1, 0x02, 0xB9, 0xB7, 0x25, 0xA0 } }, "Windows Sonic for Headphones" },
    };
    size_t i;
    static const GUID zero = { 0 };
    if (!memcmp(g, &zero, sizeof zero)) return "off";
    for (i = 0; i < sizeof known / sizeof known[0]; ++i)
        if (IsEqualGUID(g, &known[i].g)) return known[i].n;
    return "third-party format";
}

static void print_format(const WAVEFORMATEX *wf, size_t cb)
{
    const char *kind = "?";
    unsigned valid = wf->wBitsPerSample;
    unsigned long mask = 0;
    if (wf->wFormatTag == WAVE_FORMAT_PCM) kind = "PCM";
    else if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) kind = "float";
    else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && cb >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)wf;
        if (IsEqualGUID(&x->SubFormat, &E1CTL_SUBTYPE_PCM)) kind = "PCM";
        else if (IsEqualGUID(&x->SubFormat, &E1CTL_SUBTYPE_FLOAT)) kind = "float";
        valid = x->Samples.wValidBitsPerSample;
        mask = x->dwChannelMask;
    }
    printf("    device format : %lu Hz, %u-bit %s (%u valid, %u-byte container), %u ch",
           (unsigned long)wf->nSamplesPerSec, (unsigned)wf->wBitsPerSample, kind, valid,
           wf->nChannels ? (unsigned)(wf->nBlockAlign / wf->nChannels) : 0u, (unsigned)wf->nChannels);
    if (mask) printf(", mask 0x%lX", mask);
    printf("\n");
}

static void print_fx(const wchar_t *flow, const wchar_t *guid)
{
    static const struct { int pid; const char *label; } slots[] = {
        { 1, "LFX (pre-mix)" }, { 2, "GFX (post-mix)" }, { 5, "SFX (stream)" }, { 6, "MFX (mode)" }, { 7, "EFX (endpoint)" },
    };
    wchar_t key[300], name[96], val[128];
    size_t i;
    int any = 0;
    swprintf(key, 300, MMDEV_ROOT L"%s\\%s\\FxProperties", flow, guid);
    for (i = 0; i < sizeof slots / sizeof slots[0]; ++i) {
        DWORD type = 0, cb = sizeof val;
        swprintf(name, 96, PK_FX_FMT, slots[i].pid);
        if (reg_read(key, name, &type, val, &cb) == ERROR_SUCCESS && type == REG_SZ) {
            char u[128];
            w2u(val, u, sizeof u);
            printf("    %-14s: %s (%s)\n", slots[i].label, u, clsid_owner(val));
            any = 1;
        }
    }
    if (!any) printf("    effects       : none registered\n");
    {
        wchar_t pkey[300];
        DWORD type = 0, v = 0, cb = sizeof v;
        swprintf(pkey, 300, MMDEV_ROOT L"%s\\%s\\Properties", flow, guid);
        if (reg_read(pkey, PK_DISABLE_SYSFX, &type, &v, &cb) == ERROR_SUCCESS && type == REG_DWORD && v)
            printf("    enhancements  : DISABLED by the user (Windows will not load any APO here)\n");
    }
}

static void print_spatial(IMMDevice *dev, const wchar_t *guid)
{
    wchar_t pkey[300];
    BYTE blob[64];
    DWORD type = 0, cb = sizeof blob;
    ISpatialAudioClient *sac = NULL;
    HRESULT hr;

    swprintf(pkey, 300, MMDEV_ROOT L"Render\\%s\\Properties", guid);
    /* Undocumented endpoint value: 8-byte PROPVARIANT header, 8 bytes, then the selected format GUID. */
    if (reg_read(pkey, PK_SPATIAL_FMT, &type, blob, &cb) == ERROR_SUCCESS && type == REG_BINARY && cb >= 32) {
        GUID g;
        memcpy(&g, blob + 16, sizeof g);
        printf("    spatial sound : %s (setting)\n", spatial_name(&g));
    } else {
        printf("    spatial sound : off / never configured (setting)\n");
    }
    hr = IMMDevice_Activate(dev, &E1CTL_IID_ISpatialAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&sac);
    if (SUCCEEDED(hr) && sac) {
        UINT32 maxobj = 0;
        AudioObjectType mask = AudioObjectType_None;
        HRESULT h1 = ISpatialAudioClient_GetMaxDynamicObjectCount(sac, &maxobj);
        HRESULT h2 = ISpatialAudioClient_GetNativeStaticObjectTypeMask(sac, &mask);
        if (SUCCEEDED(h1) && SUCCEEDED(h2))
            printf("    spatial engine: %s (max dynamic objects %u, static mask 0x%X)\n",
                   maxobj > 0 ? "ACTIVE - turn it off for bit-exact playback" : "inactive",
                   (unsigned)maxobj, (unsigned)mask);
        else
            printf("    spatial engine: inactive (0x%08lX)\n", (unsigned long)(FAILED(h1) ? h1 : h2));
        ISpatialAudioClient_Release(sac);
    } else {
        printf("    spatial engine: not available (0x%08lX)\n", (unsigned long)hr);
    }
}

static int cmd_find(void)
{
    HRESULT hr;
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *coll = NULL;
    UINT count = 0, i;
    int found = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { fprintf(stderr, "e1ctl: CoInitializeEx failed 0x%08lX\n", (unsigned long)hr); return 2; }
    hr = CoCreateInstance(&E1CTL_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &E1CTL_IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { fprintf(stderr, "e1ctl: MMDeviceEnumerator unavailable 0x%08lX\n", (unsigned long)hr); CoUninitialize(); return 2; }
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(en, eAll, DEVICE_STATEMASK_ALL, &coll);
    if (SUCCEEDED(hr)) hr = IMMDeviceCollection_GetCount(coll, &count);
    if (FAILED(hr)) {
        fprintf(stderr, "e1ctl: EnumAudioEndpoints failed 0x%08lX\n", (unsigned long)hr);
        if (coll) IMMDeviceCollection_Release(coll);
        IMMDeviceEnumerator_Release(en);
        CoUninitialize();
        return 2;
    }

    printf("Sound Blaster E1 endpoints (USB\\VID_041E&PID_323B)\n");
    for (i = 0; i < count; ++i) {
        IMMDevice *dev = NULL;
        IMMEndpoint *ep = NULL;
        IPropertyStore *ps = NULL;
        LPWSTR id = NULL;
        DWORD state = 0;
        EDataFlow flow = eRender;
        const wchar_t *guid, *flow_name;
        wchar_t pkey[300], inst[512], jack[64];
        DWORD type = 0, cb;
        char u_id[160], u_name[256], u_inst[512];
        PROPVARIANT pv;

        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev))) continue;
        if (FAILED(IMMDevice_GetId(dev, &id)) || !id) { IMMDevice_Release(dev); continue; }
        IMMDevice_GetState(dev, &state);
        if (SUCCEEDED(IMMDevice_QueryInterface(dev, &E1CTL_IID_IMMEndpoint, (void **)&ep))) {
            IMMEndpoint_GetDataFlow(ep, &flow);
            IMMEndpoint_Release(ep);
        }
        guid = wcsstr(id, L"}.{");
        guid = guid ? guid + 2 : id;
        flow_name = flow == eCapture ? L"Capture" : L"Render";

        swprintf(pkey, 300, MMDEV_ROOT L"%s\\%s\\Properties", flow_name, guid);
        cb = sizeof inst;
        inst[0] = 0;
        if (reg_read(pkey, PK_INSTANCE_ID, &type, inst, &cb) != ERROR_SUCCESS || type != REG_SZ) {
            cb = sizeof inst;
            if (reg_read(pkey, PK_DEVICE_PATH, &type, inst, &cb) != ERROR_SUCCESS || type != REG_SZ) inst[0] = 0;
        }
        _wcsupr_s(inst, sizeof inst / sizeof inst[0]);
        if (!wcsstr(inst, E1_HW_ID)) {
            CoTaskMemFree(id);
            IMMDevice_Release(dev);
            continue;
        }
        found++;
        jack[0] = 0;
        cb = sizeof jack;
        reg_read(pkey, PK_JACK_SUBTYPE, &type, jack, &cb);

        w2u(id, u_id, sizeof u_id);
        w2u(inst, u_inst, sizeof u_inst);
        strcpy_s(u_name, sizeof u_name, "?");
        PropVariantInit(&pv);
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &E1CTL_PKEY_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                w2u(pv.pwszVal, u_name, sizeof u_name);
            PropVariantClear(&pv);
        }
        printf("\n  [%s] %s  (%s)\n", flow == eCapture ? "capture" : "render", u_name, state_name(state));
        printf("    endpoint id   : %s\n", u_id);
        printf("    instance      : %s\n", u_inst);
        if (!_wcsicmp(jack, L"{DFF21FE2-F70F-11D0-B917-00A0C9223196}"))
            printf("    role          : software loopback (\"What U Hear\"; never processed)\n");
        else if (!_wcsicmp(jack, L"{DFF21BE1-F70F-11D0-B917-00A0C9223196}"))
            printf("    role          : microphone\n");
        else if (!_wcsicmp(jack, L"{DFF21CE1-F70F-11D0-B917-00A0C9223196}"))
            printf("    role          : speaker / headphone output\n");
        if (ps) {
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &E1CTL_PKEY_DeviceFormat, &pv)) && pv.vt == VT_BLOB &&
                pv.blob.cbSize >= sizeof(WAVEFORMATEX))
                print_format((const WAVEFORMATEX *)pv.blob.pBlobData, pv.blob.cbSize);
            else
                printf("    device format : (not set)\n");
            PropVariantClear(&pv);
            IPropertyStore_Release(ps);
        }
        print_fx(flow_name, guid);
        if (flow == eRender && state == DEVICE_STATE_ACTIVE) print_spatial(dev, guid);
        CoTaskMemFree(id);
        IMMDevice_Release(dev);
    }
    if (!found) printf("  none found (is the E1 plugged in?)\n");
    IMMDeviceCollection_Release(coll);
    IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return found ? 0 : 2;
}

/* ------------------------------------------------------------------------- init-settings */

static int cmd_init_settings(Ctx *c)
{
    E1Params p;
    wchar_t path[MAX_PATH * 2];
    DWORD attr = GetFileAttributesW(settings_path(path, MAX_PATH * 2));
    unsigned long err;
    if (attr != INVALID_FILE_ATTRIBUTES) {
        printf("settings file already exists; left untouched\n");
        return 0;
    }
    e1fx_params_default(&p);
    if (c->dry_run) {
        printf("(dry run) would write default settings to %s\n", settings_path_u8());
        return 0;
    }
    err = e1ipc_save_settings(&p);
    if (err) { print_win32_error("writing default settings", err); return 2; }
    printf("default settings written to %s\n", settings_path_u8());
    return 0;
}

/* ---------------------------------------------------------------------------------- main */

static void usage(void)
{
    printf("e1ctl %s - E1 Studio control tool\n\n"
           "usage: e1ctl [--dry-run] <command> [args]\n\n"
           "  status [--watch]            APO heartbeat, format, bypass state, meters and parameters\n"
           "  get [key|render|capture ...] print parameters\n"
           "  set <key> <value> [...]     change parameters (e.g. set render.master_on 1 render.surround_on 1)\n"
           "  defaults [all|render|capture] reset to defaults (effects off = bit-exact)\n"
           "  preset eq <name>|list       equalizer presets\n"
           "  preset vfx <name>|list      voice presets\n"
           "  keys                        list every key, its range and meaning\n"
           "  find                        list E1 endpoints, device format, effects and spatial sound\n"
           "  init-settings               write default settings.bin if missing (installer)\n"
           "  version\n\n"
           "Booleans accept 1/0/on/off; enums accept names (svm_mode normal|loud|night,\n"
           "speaker_mode headphones|stereo). Changes apply live when the APO is running and are\n"
           "always saved to C:\\ProgramData\\E1Studio\\settings.bin.\n", E1CTL_VERSION);
}

int main(int argc, char **argv)
{
    Ctx ctx;
    UINT old_cp = GetConsoleOutputCP();
    int rc, ai = 1;
    const char *cmd;

    memset(&ctx, 0, sizeof ctx);
    SetConsoleOutputCP(CP_UTF8);
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] == '-') {
        if (!strcmp(argv[ai], "--dry-run")) ctx.dry_run = 1;
        else if (!strcmp(argv[ai], "--help")) { usage(); SetConsoleOutputCP(old_cp); return 0; }
        else { fprintf(stderr, "e1ctl: unknown option '%s'\n", argv[ai]); SetConsoleOutputCP(old_cp); return 1; }
        ++ai;
    }
    if (ai >= argc) { usage(); SetConsoleOutputCP(old_cp); return 1; }
    cmd = argv[ai++];

    if (!_stricmp(cmd, "help") || !_stricmp(cmd, "-h") || !_stricmp(cmd, "/?")) { usage(); rc = 0; }
    else if (!_stricmp(cmd, "version")) {
        printf("e1ctl %s, params v%u (%u bytes), shared block %u bytes\n", E1CTL_VERSION,
               (unsigned)E1FX_PARAMS_VERSION, (unsigned)sizeof(E1Params), (unsigned)sizeof(E1SharedBlock));
        rc = 0;
    }
    else if (!_stricmp(cmd, "keys")) rc = cmd_keys();
    else if (!_stricmp(cmd, "find")) rc = cmd_find();
    else {
        ctx_open(&ctx);
        if (!_stricmp(cmd, "status")) rc = cmd_status(&ctx, argc - ai, argv + ai);
        else if (!_stricmp(cmd, "get")) rc = cmd_get(&ctx, argc - ai, argv + ai);
        else if (!_stricmp(cmd, "set")) rc = cmd_set(&ctx, argc - ai, argv + ai);
        else if (!_stricmp(cmd, "defaults")) rc = cmd_defaults(&ctx, argc - ai, argv + ai);
        else if (!_stricmp(cmd, "preset")) rc = cmd_preset(&ctx, argc - ai, argv + ai);
        else if (!_stricmp(cmd, "init-settings")) rc = cmd_init_settings(&ctx);
        else { fprintf(stderr, "e1ctl: unknown command '%s'\n\n", cmd); usage(); rc = 1; }
        ctx_close(&ctx);
    }
    fflush(stdout);
    SetConsoleOutputCP(old_cp);
    return rc;
}
