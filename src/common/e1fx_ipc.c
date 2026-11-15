/*
 * e1fx_ipc.c - implementation of include/e1fx_ipc.h (shared section, seqlock, settings file).
 *
 * Linked into the APO (audiodg.exe, LOCAL SERVICE), the control panel and the tools.
 * Depends only on Win32 (kernel32, advapi32) and on e1fx_params_default/e1fx_params_sanitize.
 *
 * Reader convention: start with *last_seq = -1 (any odd value means "nothing seen yet"; a
 * consistent snapshot always has an even seq). e1ipc_read_params already rejects foreign
 * params versions and runs e1fx_params_sanitize on the snapshot it returns; e1ipc_load_settings
 * sanitizes too, and e1ipc_save_settings stamps version/size and sanitizes before writing.
 *
 * Test-only overrides (environment variables, read on every call):
 *   E1FX_SECTION_OVERRIDE   section name to use instead of E1FX_SECTION_NAME. Only names in the
 *                           session-local namespace ("Local\...") are honoured, so tests do not need
 *                           SeCreateGlobalPrivilege and a stray variable can never redirect the
 *                           production Global section to another global object.
 *   E1FX_SETTINGS_OVERRIDE  full path of the settings file instead of E1FX_SETTINGS_PATH.
 *
 * Settings file reads never block indefinitely: the file lives in a folder where users may modify
 * files, and the APO reads it on audiodg's control path. It is opened with NtCreateFile and
 * FILE_COMPLETE_IF_OPLOCKED, so an oplock held by another process (which might never acknowledge
 * the break) makes the open return at once with STATUS_OPLOCK_BREAK_IN_PROGRESS instead of
 * waiting; a few short retries cover benign holders (indexer, antivirus), then the caller gets
 * defaults and ERROR_OPLOCK_BREAK_IN_PROGRESS. Reparse points are not followed and UNC paths are
 * refused (no network I/O from audiodg).
 *
 * Seqlock writers (panel, e1ctl, the section initialisation) are mutually exclusive: a writer
 * claims the odd state with a compare-exchange from an even value, so seq only grows and a reader
 * can never accept a mix of two writers' data. A writer that died mid-update (seq odd for longer
 * than E1IPC_WRITER_TAKEOVER_MS) is taken over.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <sddl.h>
#include <string.h>

#include "../../include/e1fx_ipc.h"

#define E1IPC_PATH_CCH 1024
#define E1IPC_OPLOCK_RETRIES 4          /* opens that hit a pending oplock break, 5 ms apart */
#define E1IPC_WRITER_TAKEOVER_MS 100u   /* odd seq for this long = the writer died mid-update */

#ifndef FILE_COMPLETE_IF_OPLOCKED
#define FILE_COMPLETE_IF_OPLOCKED 0x00000100
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FILE_SEQUENTIAL_ONLY
#define FILE_SEQUENTIAL_ONLY 0x00000004
#endif
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef STATUS_OPLOCK_BREAK_IN_PROGRESS
#define STATUS_OPLOCK_BREAK_IN_PROGRESS ((NTSTATUS)0x00000108L)
#endif

/* ---------------------------------------------------------------------------------------- */
/* CRC-32 (IEEE 802.3 / zlib: reflected polynomial 0xEDB88320, init and final xor 0xFFFFFFFF) */

static const uint32_t k_crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

uint32_t e1ipc_crc32(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    if (p == NULL) len = 0;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ k_crc_nibble[crc & 15u];
        crc = (crc >> 4) ^ k_crc_nibble[crc & 15u];
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------------------- */
/* Names and paths                                                                          */

static const wchar_t *e1ipc_section_name(wchar_t *buf, DWORD cch)
{
    DWORD n = GetEnvironmentVariableW(L"E1FX_SECTION_OVERRIDE", buf, cch);
    if (n > 6 && n < cch && _wcsnicmp(buf, L"Local\\", 6) == 0)
        return buf;
    return E1FX_SECTION_NAME;
}

static const wchar_t *e1ipc_settings_path(wchar_t *buf, DWORD cch)
{
    DWORD n = GetEnvironmentVariableW(L"E1FX_SETTINGS_OVERRIDE", buf, cch);
    if (n > 0 && n < cch)
        return buf;
    return E1FX_SETTINGS_PATH;
}

/* Creates the directory that contains `path` (one level; the parent of that must exist). */
static DWORD e1ipc_ensure_parent_dir(const wchar_t *path)
{
    wchar_t dir[E1IPC_PATH_CCH];
    size_t len = wcslen(path);
    if (len >= E1IPC_PATH_CCH) return ERROR_FILENAME_EXCED_RANGE;
    memcpy(dir, path, (len + 1) * sizeof(wchar_t));
    while (len > 0 && dir[len - 1] != L'\\' && dir[len - 1] != L'/') --len;
    if (len == 0) return ERROR_SUCCESS;              /* relative file name: current directory */
    dir[len - 1] = L'\0';
    if (dir[0] == L'\0' || (len == 3 && dir[1] == L':')) return ERROR_SUCCESS; /* root */
    if (CreateDirectoryW(dir, NULL)) return ERROR_SUCCESS;
    {
        DWORD err = GetLastError();
        return err == ERROR_ALREADY_EXISTS ? ERROR_SUCCESS : err;
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Settings file                                                                            */

typedef NTSTATUS (NTAPI *PFN_NtCreateFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                           PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef ULONG (NTAPI *PFN_RtlNtStatusToDosError)(NTSTATUS);

/* Opens `path` for reading without ever waiting on an oplock break (see the file header).
   Returns a synchronous handle, or INVALID_HANDLE_VALUE with *err set. */
static HANDLE e1ipc_open_read_nonblocking(const wchar_t *path, DWORD *err)
{
    PFN_NtCreateFile nt_create = NULL;
    PFN_RtlNtStatusToDosError nt_err = NULL;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    wchar_t full[E1IPC_PATH_CCH], ntpath[E1IPC_PATH_CCH + 8];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    DWORD n;
    int attempt;

    if (ntdll != NULL) {
        nt_create = (PFN_NtCreateFile)(void *)GetProcAddress(ntdll, "NtCreateFile");
        nt_err = (PFN_RtlNtStatusToDosError)(void *)GetProcAddress(ntdll, "RtlNtStatusToDosError");
    }
    if (nt_create == NULL) { *err = ERROR_PROC_NOT_FOUND; return INVALID_HANDLE_VALUE; }

    n = GetFullPathNameW(path, E1IPC_PATH_CCH, full, NULL);
    if (n == 0) { *err = GetLastError(); return INVALID_HANDLE_VALUE; }
    if (n >= E1IPC_PATH_CCH) { *err = ERROR_FILENAME_EXCED_RANGE; return INVALID_HANDLE_VALUE; }
    if (full[0] == L'\\' && full[1] == L'\\') {
        /* UNC or device path: never touched from audiodg (could block on the network). */
        *err = ERROR_BAD_PATHNAME;
        return INVALID_HANDLE_VALUE;
    }
    memcpy(ntpath, L"\\??\\", 4 * sizeof(wchar_t));
    memcpy(ntpath + 4, full, ((size_t)n + 1) * sizeof(wchar_t));
    us.Buffer = ntpath;
    us.Length = (USHORT)((n + 4) * sizeof(wchar_t));
    us.MaximumLength = (USHORT)(us.Length + sizeof(wchar_t));
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);

    for (attempt = 0;; ++attempt) {
        HANDLE h = INVALID_HANDLE_VALUE;
        NTSTATUS st;
        memset(&iosb, 0, sizeof(iosb));
        /* FILE_SHARE_DELETE so that a concurrent atomic replace by the panel is never blocked. */
        st = nt_create(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                       FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY |
                       FILE_COMPLETE_IF_OPLOCKED | FILE_OPEN_REPARSE_POINT, NULL, 0);
        if (st == STATUS_OPLOCK_BREAK_IN_PROGRESS) {
            /* Somebody holds an oplock: do not use the file while the break is pending. */
            CloseHandle(h);
            if (attempt + 1 >= E1IPC_OPLOCK_RETRIES) { *err = ERROR_OPLOCK_BREAK_IN_PROGRESS; return INVALID_HANDLE_VALUE; }
            Sleep(5);
            continue;
        }
        if (st < 0) {
            *err = nt_err ? (DWORD)nt_err(st) : ERROR_OPEN_FAILED;
            return INVALID_HANDLE_VALUE;
        }
        *err = ERROR_SUCCESS;
        return h;
    }
}

unsigned long e1ipc_load_settings(E1Params *p)
{
    wchar_t pathbuf[E1IPC_PATH_CCH];
    const wchar_t *path;
    E1SettingsFile f;
    LARGE_INTEGER size;
    DWORD got = 0, err = ERROR_SUCCESS;
    HANDLE h;

    if (p == NULL) return ERROR_INVALID_PARAMETER;
    e1fx_params_default(p);

    memset(&f, 0, sizeof(f));
    path = e1ipc_settings_path(pathbuf, E1IPC_PATH_CCH);
    h = e1ipc_open_read_nonblocking(path, &err);
    if (h == INVALID_HANDLE_VALUE) return err;

    if (!GetFileSizeEx(h, &size)) err = GetLastError();
    else if (size.QuadPart != (LONGLONG)sizeof(f)) err = ERROR_INVALID_DATA;
    else if (!ReadFile(h, &f, (DWORD)sizeof(f), &got, NULL)) err = GetLastError();
    else if (got != sizeof(f)) err = ERROR_HANDLE_EOF;
    CloseHandle(h);
    if (err != ERROR_SUCCESS) return err;

    if (f.magic != E1FX_MAGIC) return ERROR_INVALID_DATA;
    if (f.crc32 != e1ipc_crc32(&f.params, sizeof(f.params))) return ERROR_CRC;
    if (f.params.version != E1FX_PARAMS_VERSION || f.params.size != (uint32_t)sizeof(E1Params))
        return ERROR_REVISION_MISMATCH;

    e1fx_params_sanitize(&f.params);
    *p = f.params;
    return ERROR_SUCCESS;
}

unsigned long e1ipc_save_settings(const E1Params *p)
{
    wchar_t pathbuf[E1IPC_PATH_CCH], tmp[E1IPC_PATH_CCH + 32];
    const wchar_t *path;
    E1SettingsFile f;
    DWORD put = 0, err = ERROR_SUCCESS;
    HANDLE h;
    size_t len;

    if (p == NULL) return ERROR_INVALID_PARAMETER;

    memset(&f, 0, sizeof(f));
    f.magic = E1FX_MAGIC;
    f.params = *p;
    f.params.version = E1FX_PARAMS_VERSION;
    f.params.size = (uint32_t)sizeof(E1Params);
    e1fx_params_sanitize(&f.params);
    f.crc32 = e1ipc_crc32(&f.params, sizeof(f.params));

    path = e1ipc_settings_path(pathbuf, E1IPC_PATH_CCH);
    err = e1ipc_ensure_parent_dir(path);
    if (err != ERROR_SUCCESS) return err;

    /* Temp file in the same directory so the final MoveFileEx is a same-volume rename. */
    len = wcslen(path);
    if (len + 24 >= sizeof(tmp) / sizeof(tmp[0])) return ERROR_FILENAME_EXCED_RANGE;
    memcpy(tmp, path, len * sizeof(wchar_t));
    {
        static const wchar_t hex[] = L"0123456789abcdef";
        DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
        wchar_t *q = tmp + len;
        int i;
        *q++ = L'.';
        for (i = 7; i >= 0; --i) *q++ = hex[(pid >> (i * 4)) & 15u];
        for (i = 7; i >= 0; --i) *q++ = hex[(tid >> (i * 4)) & 15u];
        memcpy(q, L".tmp", 5 * sizeof(wchar_t));
    }

    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    if (!WriteFile(h, &f, (DWORD)sizeof(f), &put, NULL)) err = GetLastError();
    else if (put != sizeof(f)) err = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(h)) err = GetLastError();
    CloseHandle(h);

    if (err == ERROR_SUCCESS &&
        !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        err = GetLastError();
    if (err != ERROR_SUCCESS) DeleteFileW(tmp);
    return err;
}

/* ---------------------------------------------------------------------------------------- */
/* Seqlock                                                                                  */

void e1ipc_write_params(E1SharedBlock *b, const E1Params *p)
{
    volatile LONG *seq;
    LONG cur;
    DWORD t0;
    unsigned spin;

    if (b == NULL || p == NULL) return;
    seq = (volatile LONG *)&b->seq;

    /* Claim the update: even -> odd by compare-exchange, so only one writer copies at a time and
       seq never moves backwards. */
    t0 = GetTickCount();
    for (spin = 0;; ++spin) {
        LONG s = *seq;
        if ((s & 1) == 0) {
            if (InterlockedCompareExchange(seq, s + 1, s) == s) { cur = s + 1; break; }
            continue;
        }
        if (GetTickCount() - t0 > E1IPC_WRITER_TAKEOVER_MS) {
            /* the owner died mid-update: take over, staying odd (s + 2) */
            if (InterlockedCompareExchange(seq, s + 2, s) == s) { cur = s + 2; break; }
            continue;
        }
        if (spin < 64) YieldProcessor();
        else Sleep(spin < 256 ? 0 : 1);
    }

    MemoryBarrier();
    memcpy((void *)&b->params, p, sizeof(*p));
    MemoryBarrier();
    /* Publish (even). Fails only when another writer took over after this one stalled for longer
       than the take-over time; that writer's newer data then wins. */
    InterlockedCompareExchange(seq, cur + 1, cur);
}

int e1ipc_read_params(const E1SharedBlock *b, int32_t *last_seq, E1Params *out)
{
    E1Params tmp;
    int32_t s1, s2;

    if (b == NULL || last_seq == NULL || out == NULL) return 0;
    if (*(const volatile uint32_t *)&b->magic != E1FX_MAGIC) return 0;

    s1 = *(const volatile int32_t *)&b->seq;
    if ((s1 & 1) || s1 == *last_seq) return 0;
    MemoryBarrier();
    memcpy(&tmp, (const void *)&b->params, sizeof(tmp));
    MemoryBarrier();
    s2 = *(const volatile int32_t *)&b->seq;
    if (s2 != s1) return 0;                             /* torn: try again on the next buffer */

    *last_seq = s1;
    if (tmp.version != E1FX_PARAMS_VERSION || tmp.size != (uint32_t)sizeof(E1Params))
        return 0;                                       /* incompatible writer: ignore this version */
    e1fx_params_sanitize(&tmp);
    *out = tmp;
    return 1;
}

/* ---------------------------------------------------------------------------------------- */
/* Section                                                                                  */

static void e1ipc_init_block(E1SharedBlock *b)
{
    E1Params p;
    volatile LONG *seq = (volatile LONG *)&b->seq;

    e1ipc_load_settings(&p);                 /* defaults on any failure */

    /* A fresh section is zero-filled (seq 0). Compare-exchange so that a panel which opened the
       section in the few microseconds before the magic was published, and already wrote, is
       never moved backwards. */
    InterlockedCompareExchange(seq, 1, 0);   /* odd while initialising */
    MemoryBarrier();
    b->block_size = (uint32_t)sizeof(E1SharedBlock);
    b->reserved0 = 0;
    memcpy((void *)&b->params, &p, sizeof(p));
    memset((void *)&b->render_status, 0, sizeof(b->render_status));
    memset((void *)&b->capture_status, 0, sizeof(b->capture_status));
    b->render_status.bypass = 1;
    b->capture_status.bypass = 1;
    MemoryBarrier();
    *(volatile uint32_t *)&b->magic = E1FX_MAGIC;
    MemoryBarrier();
    InterlockedCompareExchange(seq, 2, 1);
}

unsigned long e1ipc_open(E1IpcMapping *m, int allow_create)
{
    wchar_t namebuf[256];
    const wchar_t *name;
    HANDLE h;
    DWORD err;
    int created = 0;
    E1SharedBlock *b;
    MEMORY_BASIC_INFORMATION mbi;

    if (m == NULL) return ERROR_INVALID_PARAMETER;
    m->section = NULL;
    m->block = NULL;
    m->created = 0;

    name = e1ipc_section_name(namebuf, (DWORD)(sizeof(namebuf) / sizeof(namebuf[0])));

    h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (h == NULL) {
        PSECURITY_DESCRIPTOR sd = NULL;
        SECURITY_ATTRIBUTES sa;

        err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND || !allow_create) return err;

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(E1FX_SECTION_SDDL, SDDL_REVISION_1, &sd, NULL))
            return GetLastError();
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
        h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)sizeof(E1SharedBlock), name);
        err = GetLastError();
        LocalFree(sd);
        if (h == NULL) return err;
        created = (err != ERROR_ALREADY_EXISTS);    /* lost a creation race: just open it */
    }

    b = (E1SharedBlock *)MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(E1SharedBlock));
    if (b == NULL) {
        err = GetLastError();
        CloseHandle(h);
        return err;
    }
    if (VirtualQuery(b, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < sizeof(E1SharedBlock)) {
        UnmapViewOfFile(b);
        CloseHandle(h);
        return ERROR_INVALID_DATA;
    }

    if (created) {
        e1ipc_init_block(b);
    } else {
        /* magic == 0 means the creator is still initialising; readers check magic themselves. */
        uint32_t magic = *(volatile uint32_t *)&b->magic;
        if (magic != 0 && (magic != E1FX_MAGIC || b->block_size != (uint32_t)sizeof(E1SharedBlock))) {
            UnmapViewOfFile(b);
            CloseHandle(h);
            return ERROR_REVISION_MISMATCH;
        }
    }

    m->section = h;
    m->block = b;
    m->created = created;
    return ERROR_SUCCESS;
}

void e1ipc_close(E1IpcMapping *m)
{
    if (m == NULL) return;
    if (m->block != NULL) UnmapViewOfFile(m->block);
    if (m->section != NULL) CloseHandle((HANDLE)m->section);
    m->block = NULL;
    m->section = NULL;
    m->created = 0;
}
