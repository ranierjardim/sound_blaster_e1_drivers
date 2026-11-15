/*
 * e1fx_dll.c - DLL entry, class factories, object/lock counting and self-registration.
 *
 * DllRegisterServer writes, for each class (needs administrator rights; run by the installer):
 *   HKLM\SOFTWARE\Classes\CLSID\{clsid}                       (default) = description
 *   HKLM\SOFTWARE\Classes\CLSID\{clsid}\InprocServer32        (default) = <dll path>, ThreadingModel = Both
 *   HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\{clsid}
 *       FriendlyName, Copyright (REG_SZ), MajorVersion, MinorVersion, Flags, MinInputConnections,
 *       MaxInputConnections, MinOutputConnections, MaxOutputConnections, MaxInstances,
 *       NumAPOInterfaces (REG_DWORD), APOInterface0..N-1 (REG_SZ IIDs)
 * which is the layout the SDK's RegisterAPO() helper produces. Endpoint FX keys are NOT touched
 * here; binding the APO to the E1 endpoints is the installer's job.
 */
#include "e1fx_apo_internal.h"
#include <stdio.h>
#include <string.h>

HMODULE g_e1_module;
static volatile LONG g_objects;   /* live APOs and media types */
static volatile LONG g_locks;     /* factory references + IClassFactory::LockServer */

void e1dll_object_created(void) { InterlockedIncrement(&g_objects); }
void e1dll_object_destroyed(void) { InterlockedDecrement(&g_objects); }
LONG e1dll_object_count(void) { return g_objects; }

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_e1_module = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

/* ======================================================================================== */
/* class factory                                                                             */

typedef struct E1Factory {
    const IClassFactoryVtbl *lpVtbl;
    int kind;
} E1Factory;

static HRESULT STDMETHODCALLTYPE cf_QueryInterface(IClassFactory *This, REFIID riid, void **ppv)
{
    if (ppv == NULL) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *ppv = This;
        IClassFactory_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

/* The factories are static; their references only keep the module loaded. */
static ULONG STDMETHODCALLTYPE cf_AddRef(IClassFactory *This)
{
    (void)This;
    return (ULONG)InterlockedIncrement(&g_locks) + 1u;
}

static ULONG STDMETHODCALLTYPE cf_Release(IClassFactory *This)
{
    (void)This;
    return (ULONG)InterlockedDecrement(&g_locks) + 1u;
}

static HRESULT STDMETHODCALLTYPE cf_CreateInstance(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv)
{
    if (ppv == NULL) return E_POINTER;
    *ppv = NULL;
    /* The audio engine aggregates APOs (outer != NULL, riid = IID_IUnknown). */
    return e1apo_create(((E1Factory *)This)->kind, outer, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE cf_LockServer(IClassFactory *This, BOOL lock)
{
    (void)This;
    if (lock) InterlockedIncrement(&g_locks);
    else InterlockedDecrement(&g_locks);
    return S_OK;
}

static const IClassFactoryVtbl g_cf_vtbl = {
    cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance, cf_LockServer
};

static E1Factory g_factory_render = { &g_cf_vtbl, E1APO_KIND_RENDER };
static E1Factory g_factory_capture = { &g_cf_vtbl, E1APO_KIND_CAPTURE };

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    E1Factory *f;
    if (ppv == NULL) return E_POINTER;
    *ppv = NULL;
    if (IsEqualCLSID(rclsid, &CLSID_E1FxRenderMfx)) f = &g_factory_render;
    else if (IsEqualCLSID(rclsid, &CLSID_E1FxCaptureMfx)) f = &g_factory_capture;
    else return CLASS_E_CLASSNOTAVAILABLE;
    return cf_QueryInterface((IClassFactory *)f, riid, ppv);
}

STDAPI DllCanUnloadNow(void)
{
    return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE;
}

/* ======================================================================================== */
/* registration                                                                              */

#define E1_CLASSES_ROOT L"SOFTWARE\\Classes"
#define E1_APO_ROOT     L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects"

static LONG reg_set_sz(HKEY k, const wchar_t *name, const wchar_t *val)
{
    return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
}

static LONG reg_set_dw(HKEY k, const wchar_t *name, DWORD val)
{
    return RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
}

static LONG reg_create(const wchar_t *path, HKEY *out)
{
    return RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, REG_OPTION_NON_VOLATILE,
                           KEY_WRITE | KEY_WOW64_64KEY, NULL, out, NULL);
}

static LONG reg_delete_tree(const wchar_t *path)
{
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, path);
    if (r == ERROR_SUCCESS) r = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, path, KEY_WOW64_64KEY, 0);
    return (r == ERROR_FILE_NOT_FOUND) ? ERROR_SUCCESS : r;
}

static void clsid_string(int kind, wchar_t *buf, int cch)
{
    StringFromGUID2(e1apo_clsid(kind), buf, cch);
}

static LONG register_class(int kind, const wchar_t *module_path)
{
    wchar_t clsid[40], path[256], iid[40], name[32];
    const wchar_t *desc = kind == E1APO_KIND_RENDER ? L"E1 Studio Effects (render)" : L"E1 Studio Effects (capture)";
    APO_REG_PROPERTIES *p = NULL;
    HKEY k = NULL;
    LONG r;
    UINT32 i;

    clsid_string(kind, clsid, 40);

    /* COM server */
    swprintf_s(path, 256, E1_CLASSES_ROOT L"\\CLSID\\%s", clsid);
    r = reg_create(path, &k);
    if (r != ERROR_SUCCESS) return r;
    r = reg_set_sz(k, NULL, desc);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return r;

    swprintf_s(path, 256, E1_CLASSES_ROOT L"\\CLSID\\%s\\InprocServer32", clsid);
    r = reg_create(path, &k);
    if (r != ERROR_SUCCESS) return r;
    r = reg_set_sz(k, NULL, module_path);
    if (r == ERROR_SUCCESS) r = reg_set_sz(k, L"ThreadingModel", L"Both");
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return r;

    /* APO registration (same values as the SDK RegisterAPO helper) */
    if (FAILED(e1apo_get_reg_properties(kind, &p))) return ERROR_OUTOFMEMORY;
    swprintf_s(path, 256, E1_APO_ROOT L"\\%s", clsid);
    r = reg_create(path, &k);
    if (r == ERROR_SUCCESS) {
        r = reg_set_sz(k, L"FriendlyName", p->szFriendlyName);
        if (r == ERROR_SUCCESS) r = reg_set_sz(k, L"Copyright", p->szCopyrightInfo);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"MajorVersion", p->u32MajorVersion);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"MinorVersion", p->u32MinorVersion);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"Flags", (DWORD)p->Flags);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"MinInputConnections", p->u32MinInputConnections);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"MaxInputConnections", p->u32MaxInputConnections);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"MinOutputConnections", p->u32MinOutputConnections);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"MaxOutputConnections", p->u32MaxOutputConnections);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"MaxInstances", p->u32MaxInstances);
        if (r == ERROR_SUCCESS) r = reg_set_dw(k, L"NumAPOInterfaces", p->u32NumAPOInterfaces);
        for (i = 0; r == ERROR_SUCCESS && i < p->u32NumAPOInterfaces; ++i) {
            swprintf_s(name, 32, L"APOInterface%u", i);
            StringFromGUID2(&p->iidAPOInterfaceList[i], iid, 40);
            r = reg_set_sz(k, name, iid);
        }
        RegCloseKey(k);
    }
    CoTaskMemFree(p);
    return r;
}

static LONG unregister_class(int kind)
{
    wchar_t clsid[40], path[256];
    LONG r1, r2;
    clsid_string(kind, clsid, 40);
    swprintf_s(path, 256, E1_APO_ROOT L"\\%s", clsid);
    r1 = reg_delete_tree(path);
    swprintf_s(path, 256, E1_CLASSES_ROOT L"\\CLSID\\%s", clsid);
    r2 = reg_delete_tree(path);
    return r1 != ERROR_SUCCESS ? r1 : r2;
}

STDAPI DllRegisterServer(void)
{
    wchar_t module_path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(g_e1_module, module_path, (DWORD)(sizeof(module_path) / sizeof(module_path[0])));
    LONG r;
    if (n == 0 || n >= sizeof(module_path) / sizeof(module_path[0])) return HRESULT_FROM_WIN32(GetLastError());

    r = register_class(E1APO_KIND_RENDER, module_path);
    if (r == ERROR_SUCCESS) r = register_class(E1APO_KIND_CAPTURE, module_path);
    if (r != ERROR_SUCCESS) {
        unregister_class(E1APO_KIND_RENDER);
        unregister_class(E1APO_KIND_CAPTURE);
        return HRESULT_FROM_WIN32(r);
    }
    return S_OK;
}

STDAPI DllUnregisterServer(void)
{
    LONG r1 = unregister_class(E1APO_KIND_RENDER);
    LONG r2 = unregister_class(E1APO_KIND_CAPTURE);
    if (r1 != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r1);
    if (r2 != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r2);
    return S_OK;
}
