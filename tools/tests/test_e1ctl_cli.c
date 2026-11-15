/*
 * test_e1ctl_cli.c - end-to-end test of e1ctl.exe against a private IPC section.
 *
 * Plays the APO: creates the shared block under a session-local name and a settings file in
 * %TEMP% (via the test-only overrides honoured by src/common/e1fx_ipc.c, inherited by the child),
 * runs a heartbeat thread, then drives e1ctl.exe (expected next to this executable) and checks
 * both its output and the resulting block / settings file. Touches nothing outside %TEMP%.
 */
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/e1fx_ipc.h"
#include "../../src/dsp/e1dsp_modules.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_fail;                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                             \
    } while (0)

static wchar_t g_exe[MAX_PATH];
static char g_out[1 << 16];
static volatile LONG g_stop = 0;
static E1IpcMapping g_map;

/* Runs e1ctl with a narrow argument string; returns the exit code, output in g_out. */
static int run(const char *args)
{
    wchar_t cmd[2048];
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0xFFFFFFFF, got = 0, total = 0;
    int n = swprintf(cmd, 2048, L"\"%s\" %hs", g_exe, args);
    g_out[0] = 0;
    if (n < 0) return -1;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    while (total + 1 < sizeof g_out && ReadFile(rd, g_out + total, (DWORD)(sizeof g_out - 1 - total), &got, NULL) && got)
        total += got;
    g_out[total] = 0;
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    printf("  $ e1ctl %s -> %lu\n", args, (unsigned long)code);
    return (int)code;
}

static int has(const char *needle)
{
    if (strstr(g_out, needle)) return 1;
    fprintf(stderr, "---- output did not contain \"%s\":\n%s\n----\n", needle, g_out);
    return 0;
}

static DWORD WINAPI heartbeat(LPVOID arg)
{
    E1SharedBlock *b = (E1SharedBlock *)arg;
    b->render_status.sample_rate = 44100;
    b->render_status.channels = 2;
    b->render_status.bypass = 1;
    b->render_status.peak_in[0] = 0.5f;
    b->render_status.peak_in[1] = 0.25f;
    b->render_status.peak_out[0] = 0.5f;
    b->render_status.peak_out[1] = 0.25f;
    b->capture_status.sample_rate = 44100;
    b->capture_status.channels = 1;
    b->capture_status.bypass = 0;
    while (!g_stop) {
        InterlockedIncrement((volatile LONG *)&b->render_status.alive);
        Sleep(10);
    }
    return 0;
}

static int read_block(E1Params *p)
{
    int i;
    for (i = 0; i < 100; ++i) {
        int32_t last = -1;
        if (e1ipc_read_params(g_map.block, &last, p)) return 1;
        Sleep(1);
    }
    return 0;
}

static int settings_equal_block(void)
{
    E1Params a, b;
    if (!read_block(&a)) return 0;
    if (e1ipc_load_settings(&b) != 0) return 0;
    return memcmp(&a, &b, sizeof a) == 0;
}

int main(void)
{
    wchar_t tmp[MAX_PATH], dir[MAX_PATH], settings[MAX_PATH], section[64];
    wchar_t *slash;
    HANDLE th;
    E1Params p, d;
    int32_t seq0;
    char args[256];
    unsigned long err;

    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    slash = wcsrchr(g_exe, L'\\');
    if (!slash) return 2;
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - g_exe), L"e1ctl.exe");
    if (GetFileAttributesW(g_exe) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "e1ctl.exe not found next to the test (%ls)\n", g_exe);
        return 1;
    }

    GetTempPathW(MAX_PATH, tmp);
    swprintf(dir, MAX_PATH, L"%se1ctl_test_%lu", tmp, GetCurrentProcessId());
    swprintf(settings, MAX_PATH, L"%s\\settings.bin", dir);
    swprintf(section, 64, L"Local\\E1CtlTest_%lu", GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);
    SetEnvironmentVariableW(L"E1FX_SETTINGS_OVERRIDE", settings);
    SetEnvironmentVariableW(L"E1FX_SECTION_OVERRIDE", section);

    /* ---- no APO yet: settings-file only ---- */
    CHECK(run("status") == 0);
    CHECK(has("APO shared block: not available"));
    CHECK(has("built-in defaults"));
    CHECK(run("init-settings") == 0);
    CHECK(has("default settings written"));
    CHECK(GetFileAttributesW(settings) != INVALID_FILE_ATTRIBUTES);
    CHECK(run("init-settings") == 0);
    CHECK(has("left untouched"));
    CHECK(run("set render.eq_on 1 render.eq.1k -6") == 0);
    CHECK(has("change takes effect when the APO next starts"));
    CHECK(e1ipc_load_settings(&p) == 0 && p.render.eq_on == 1 && p.render.eq_gain_db[5] == -6.0f);

    /* ---- "APO" starts: creates the section from the settings file ---- */
    err = e1ipc_open(&g_map, 1);
    CHECK(err == 0 && g_map.block != NULL);
    if (err || !g_map.block) return 1;
    th = CreateThread(NULL, 0, heartbeat, g_map.block, 0, NULL);
    Sleep(50);

    CHECK(run("status") == 0);
    CHECK(has("APO shared block: open"));
    CHECK(has("running"));
    CHECK(has("44100 Hz, 2 ch"));
    CHECK(has("bit-exact bypass"));
    CHECK(has("-6.0 /  -12.0 dBFS"));
    CHECK(has("shared memory (live)"));
    CHECK(has("render.eq_gain_db[5]         = -6.00 dB"));

    CHECK(run("set render.master_on 1 render.surround_level 0.25 render.svm_mode night") == 0);
    CHECK(has("applied live"));
    CHECK(read_block(&p));
    CHECK(p.render.master_on == 1 && fabsf(p.render.surround_level - 0.25f) < 1e-6f && p.render.svm_mode == 2);
    CHECK(settings_equal_block());

    CHECK(run("get render.surround_level render.svm_mode") == 0);
    CHECK(has("render.surround_level = 0.25"));
    CHECK(has("render.svm_mode = 2 (night)"));

    /* invalid input changes nothing */
    seq0 = g_map.block->seq;
    CHECK(run("set render.surround_level 5") == 1);
    CHECK(has("out of range"));
    CHECK(run("set render.master_on 0 render.nope 1") == 1);
    CHECK(run("set render.master_on") == 1);
    CHECK(run("bogus") == 1);
    CHECK(run("--dry-run set render.bass_on 1") == 0);
    CHECK(has("dry run"));
    CHECK(g_map.block->seq == seq0);
    CHECK(read_block(&p) && p.render.bass_on == 0 && p.render.master_on == 1);

    /* presets */
    snprintf(args, sizeof args, "preset eq \"%s\"", e1_eq_presets[e1_eq_preset_count - 1].name);
    CHECK(run(args) == 0);
    CHECK(read_block(&p) && p.render.eq_on == 1);
    CHECK(memcmp(p.render.eq_gain_db, e1_eq_presets[e1_eq_preset_count - 1].gain_db, sizeof p.render.eq_gain_db) == 0);
    CHECK(run("preset eq nosuchpreset") == 1);
    CHECK(run("preset eq list") == 0);
    CHECK(has(e1_eq_presets[0].name));
    snprintf(args, sizeof args, "preset vfx \"%s\"", e1_vfx_presets[e1_vfx_preset_count - 1].name);
    CHECK(run(args) == 0);
    CHECK(read_block(&p) && p.capture.vfx_on == 1 && p.capture.vfx_preset == (uint32_t)(e1_vfx_preset_count - 1));

    /* defaults render keeps capture */
    CHECK(run("set capture.master_on 1") == 0);
    CHECK(run("defaults render") == 0);
    e1fx_params_default(&d);
    CHECK(read_block(&p));
    CHECK(memcmp(&p.render, &d.render, sizeof p.render) == 0);
    CHECK(p.capture.master_on == 1 && p.capture.vfx_on == 1);
    CHECK(run("defaults") == 0);
    CHECK(read_block(&p) && memcmp(&p, &d, sizeof p) == 0);
    CHECK(settings_equal_block());

    CHECK(run("keys") == 0);
    CHECK(has("render.eq_gain_db[9] (render.eq.16k)"));
    CHECK(run("version") == 0);
    CHECK(has("params v1"));

    /* the heartbeat stops -> idle */
    g_stop = 1;
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    CHECK(run("status") == 0);
    CHECK(has("idle"));

    /* APO gone, settings corrupted -> defaults reported, still exit 0 */
    e1ipc_close(&g_map);
    {
        HANDLE h = CreateFileW(settings, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD wr = 0;
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, "garbage", 7, &wr, NULL);
            CloseHandle(h);
        }
    }
    CHECK(run("get render.master_on") == 0);
    CHECK(has("render.master_on = 0 (off)"));
    CHECK(run("status") == 0);
    CHECK(has("built-in defaults"));

    DeleteFileW(settings);
    {
        wchar_t t2[MAX_PATH];
        swprintf(t2, MAX_PATH, L"%s.tmp", settings);
        DeleteFileW(t2);
    }
    RemoveDirectoryW(dir);
    printf("test_e1ctl_cli: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
