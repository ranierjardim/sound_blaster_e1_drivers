/*
 * apohost.c - test host for e1fx.dll and the IPC helper (src/common/e1fx_ipc.c).
 *
 * Runs entirely in-process, without registration or the audio engine:
 *   - IPC unit tests (CRC-32, settings file round trip / corruption, seqlock incl. a 2-thread stress)
 *   - LoadLibrary(e1fx.dll) -> DllGetClassObject -> both APO classes
 *   - Initialize (APOInitSystemEffects / 2 / 3 with fake property store and device collection),
 *     format negotiation, LockForProcess, APOProcess incl. BUFFER_SILENT, bit-exact passthrough,
 *     parameter changes through a private section, Windows gates, effect lists + change events,
 *     notifications, IPC-unavailable fallback and reconnection, registration (HKLM redirected
 *     to a temporary HKCU key with RegOverridePredefKey), refcounts and DllCanUnloadNow.
 *
 * Test-only environment overrides used (honoured by e1fx_ipc.c):
 *   E1FX_SECTION_OVERRIDE  = Local\E1Test_<pid>[_x]
 *   E1FX_SETTINGS_OVERRIDE = %TEMP%\e1fx_apotest_<pid>\settings.bin
 *
 * Usage: apohost.exe [path\to\e1fx.dll]
 */
#include "../../src/apo/e1fx_apo_internal.h"
#include "../../include/e1fx_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* ---- objects created by the host's own copy of e1fx_mediatype.c ---- */
static volatile LONG g_host_objects;
HMODULE g_e1_module;
void e1dll_object_created(void) { InterlockedIncrement(&g_host_objects); }
void e1dll_object_destroyed(void) { InterlockedDecrement(&g_host_objects); }
LONG e1dll_object_count(void) { return g_host_objects; }

/* ---- check helpers ---- */
static int g_pass, g_fail;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) ++g_pass;                                                     \
        else {                                                                  \
            ++g_fail;                                                           \
            printf("  FAIL line %d: %s -- ", __LINE__, #cond);                  \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)
#define CHECK_HR(expr, expect)                                                  \
    do {                                                                        \
        HRESULT hr_ = (expr);                                                   \
        CHECK(hr_ == (HRESULT)(expect), "%s returned 0x%08lX, expected 0x%08lX", \
              #expr, (unsigned long)hr_, (unsigned long)(expect));              \
    } while (0)

/* ---- COM aggregation, as done by audiodg: CoCreateInstance(clsid, pUnkOuter, IID_IUnknown) ---- */
typedef struct FakeOuter { const IUnknownVtbl *lpVtbl; LONG refs; IUnknown *inner; } FakeOuter;
static HRESULT STDMETHODCALLTYPE fo_qi(IUnknown *This, REFIID riid, void **ppv)
{
    FakeOuter *o = (FakeOuter *)This;
    if (IsEqualIID(riid, &IID_IUnknown)) { *ppv = This; InterlockedIncrement(&o->refs); return S_OK; }
    return o->inner ? o->inner->lpVtbl->QueryInterface(o->inner, riid, ppv) : E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fo_addref(IUnknown *This) { return (ULONG)InterlockedIncrement(&((FakeOuter *)This)->refs); }
static ULONG STDMETHODCALLTYPE fo_release(IUnknown *This) { return (ULONG)InterlockedDecrement(&((FakeOuter *)This)->refs); }
static const IUnknownVtbl g_fo_vtbl = { fo_qi, fo_addref, fo_release };

static void test_aggregation(IClassFactory *cf)
{
    FakeOuter fo = { &g_fo_vtbl, 0, NULL };
    IUnknown *inner = NULL, *id = NULL;
    IAudioProcessingObject *apo = NULL;
    IAudioProcessingObjectConfiguration *cfg = NULL;
    HRESULT hr = IClassFactory_CreateInstance(cf, (IUnknown *)&fo, &IID_IUnknown, (void **)&inner);
    CHECK(hr == S_OK && inner != NULL, "aggregated CreateInstance hr=0x%08lX", (unsigned long)hr);
    if (!inner) return;
    CHECK(fo.refs == 0, "aggregated create does not AddRef the outer (refs=%ld)", fo.refs);
    fo.inner = inner;
    hr = inner->lpVtbl->QueryInterface(inner, &E1_IID_IAudioProcessingObject, (void **)&apo);
    CHECK(hr == S_OK && apo, "inner QI(IAudioProcessingObject)");
    CHECK(fo.refs == 1, "interface reference counted on the outer (refs=%ld)", fo.refs);
    if (apo) {
        hr = apo->lpVtbl->QueryInterface(apo, &IID_IUnknown, (void **)&id);
        CHECK(hr == S_OK && id == (IUnknown *)&fo, "QI(IUnknown) through an interface returns the outer identity");
        if (id) id->lpVtbl->Release(id);
        hr = apo->lpVtbl->QueryInterface(apo, &E1_IID_IAudioProcessingObjectConfiguration, (void **)&cfg);
        CHECK(hr == S_OK && cfg, "delegated QI(IAudioProcessingObjectConfiguration)");
        if (cfg) cfg->lpVtbl->Release(cfg);
        hr = apo->lpVtbl->Initialize(apo, 0, NULL);
        CHECK(hr == S_OK, "Initialize on the aggregated object hr=0x%08lX", (unsigned long)hr);
        apo->lpVtbl->Release(apo);
    }
    CHECK(fo.refs == 0, "outer references balanced (refs=%ld)", fo.refs);
    CHECK(inner->lpVtbl->Release(inner) == 0, "inner release destroys the object");
}

static void section(const char *name) { printf("[%s]\n", name); }

/* ---- deterministic noise ---- */
static uint32_t g_rng = 0x12345678u;
static float rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (float)((double)g_rng / 4294967295.0 * 2.0 - 1.0);
}

static void fill_noise(float *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) b[i] = rnd() * 0.9f;
    /* awkward values a careless chain could alter */
    if (n > 8) { b[0] = -0.0f; b[1] = 1e-40f; b[2] = -1e-42f; b[3] = 1.0f; b[4] = -1.0f; b[5] = 3.0e38f; }
}

/* ======================================================================================== */
/* fake IPropertyStore / IMMDevice / IMMDeviceCollection                                     */

typedef struct FakeStore { const IPropertyStoreVtbl *lpVtbl; LONG refs; DWORD disable_sysfx; } FakeStore;

static HRESULT STDMETHODCALLTYPE fs_QI(IPropertyStore *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyStore)) {
        *ppv = This; ((FakeStore *)This)->refs++; return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fs_AddRef(IPropertyStore *This) { return (ULONG)++((FakeStore *)This)->refs; }
static ULONG STDMETHODCALLTYPE fs_Release(IPropertyStore *This) { return (ULONG)--((FakeStore *)This)->refs; }
static HRESULT STDMETHODCALLTYPE fs_GetCount(IPropertyStore *This, DWORD *c) { (void)This; *c = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE fs_GetAt(IPropertyStore *This, DWORD i, PROPERTYKEY *k)
{
    (void)This;
    if (i != 0) return E_INVALIDARG;
    *k = E1_PKEY_AudioEndpoint_Disable_SysFx; return S_OK;
}
static HRESULT STDMETHODCALLTYPE fs_GetValue(IPropertyStore *This, REFPROPERTYKEY key, PROPVARIANT *v)
{
    PropVariantInit(v);
    if (e1_pkey_equal(key, &E1_PKEY_AudioEndpoint_Disable_SysFx)) {
        v->vt = VT_UI4; v->ulVal = ((FakeStore *)This)->disable_sysfx;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fs_SetValue(IPropertyStore *This, REFPROPERTYKEY k, REFPROPVARIANT v)
{ (void)This; (void)k; (void)v; return STG_E_ACCESSDENIED; }
static HRESULT STDMETHODCALLTYPE fs_Commit(IPropertyStore *This) { (void)This; return S_OK; }
static const IPropertyStoreVtbl g_fs_vtbl = { fs_QI, fs_AddRef, fs_Release, fs_GetCount, fs_GetAt, fs_GetValue, fs_SetValue, fs_Commit };

typedef struct FakeDevice { const IMMDeviceVtbl *lpVtbl; volatile LONG refs; } FakeDevice;
static HRESULT STDMETHODCALLTYPE fd_QI(IMMDevice *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &E1_IID_IMMDevice)) {
        *ppv = This; InterlockedIncrement(&((FakeDevice *)This)->refs); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fd_AddRef(IMMDevice *This) { return (ULONG)InterlockedIncrement(&((FakeDevice *)This)->refs); }
static ULONG STDMETHODCALLTYPE fd_Release(IMMDevice *This) { return (ULONG)InterlockedDecrement(&((FakeDevice *)This)->refs); }
static HRESULT STDMETHODCALLTYPE fd_Activate(IMMDevice *This, REFIID iid, DWORD ctx, PROPVARIANT *p, void **ppv)
{ (void)This; (void)iid; (void)ctx; (void)p; *ppv = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fd_OpenPropertyStore(IMMDevice *This, DWORD a, IPropertyStore **pp)
{ (void)This; (void)a; *pp = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fd_GetId(IMMDevice *This, LPWSTR *id) { (void)This; *id = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fd_GetState(IMMDevice *This, DWORD *s) { (void)This; *s = DEVICE_STATE_ACTIVE; return S_OK; }
static const IMMDeviceVtbl g_fd_vtbl = { fd_QI, fd_AddRef, fd_Release, fd_Activate, fd_OpenPropertyStore, fd_GetId, fd_GetState };

typedef struct FakeColl { const IMMDeviceCollectionVtbl *lpVtbl; LONG refs; IMMDevice *dev; } FakeColl;
static HRESULT STDMETHODCALLTYPE fc_QI(IMMDeviceCollection *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown)) { *ppv = This; ((FakeColl *)This)->refs++; return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fc_AddRef(IMMDeviceCollection *This) { return (ULONG)++((FakeColl *)This)->refs; }
static ULONG STDMETHODCALLTYPE fc_Release(IMMDeviceCollection *This) { return (ULONG)--((FakeColl *)This)->refs; }
static HRESULT STDMETHODCALLTYPE fc_GetCount(IMMDeviceCollection *This, UINT *n) { (void)This; *n = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE fc_Item(IMMDeviceCollection *This, UINT i, IMMDevice **pp)
{
    if (i != 0) { *pp = NULL; return E_INVALIDARG; }
    *pp = ((FakeColl *)This)->dev; IMMDevice_AddRef(*pp); return S_OK;
}
static const IMMDeviceCollectionVtbl g_fc_vtbl = { fc_QI, fc_AddRef, fc_Release, fc_GetCount, fc_Item };

/* ======================================================================================== */
/* APO plumbing helpers                                                                      */

typedef HRESULT (STDAPICALLTYPE *PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (STDAPICALLTYPE *PFN_DllVoid)(void);

typedef struct Apo {
    IAudioProcessingObject *apo;
    IAudioProcessingObjectRT *rt;
    IAudioProcessingObjectConfiguration *cfg;
    IAudioSystemEffects3 *fx;
    IAudioProcessingObjectNotifications *ntf;
} Apo;

static int apo_new(IClassFactory *cf, Apo *x)
{
    IUnknown *unk = NULL;
    HRESULT hr;
    memset(x, 0, sizeof(*x));
    hr = IClassFactory_CreateInstance(cf, NULL, &IID_IUnknown, (void **)&unk);
    CHECK(SUCCEEDED(hr) && unk, "CreateInstance hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 0;
    hr = IUnknown_QueryInterface(unk, &E1_IID_IAudioProcessingObject, (void **)&x->apo);
    CHECK(hr == S_OK, "QI IAudioProcessingObject");
    hr = IUnknown_QueryInterface(unk, &E1_IID_IAudioProcessingObjectRT, (void **)&x->rt);
    CHECK(hr == S_OK, "QI IAudioProcessingObjectRT");
    hr = IUnknown_QueryInterface(unk, &E1_IID_IAudioProcessingObjectConfiguration, (void **)&x->cfg);
    CHECK(hr == S_OK, "QI IAudioProcessingObjectConfiguration");
    hr = IUnknown_QueryInterface(unk, &E1_IID_IAudioSystemEffects3, (void **)&x->fx);
    CHECK(hr == S_OK, "QI IAudioSystemEffects3");
    hr = IUnknown_QueryInterface(unk, &E1_IID_IAudioProcessingObjectNotifications, (void **)&x->ntf);
    CHECK(hr == S_OK, "QI IAudioProcessingObjectNotifications");
    IUnknown_Release(unk);
    return x->apo && x->rt && x->cfg && x->fx && x->ntf;
}

/* Releases every interface; returns the count left after the last Release (0 = destroyed). */
static ULONG apo_free(Apo *x)
{
    ULONG r = 0;
    if (x->rt) IAudioProcessingObjectRT_Release(x->rt);
    if (x->cfg) IAudioProcessingObjectConfiguration_Release(x->cfg);
    if (x->fx) IAudioSystemEffects3_Release(x->fx);
    if (x->ntf) IAudioProcessingObjectNotifications_Release(x->ntf);
    if (x->apo) r = IAudioProcessingObject_Release(x->apo);
    memset(x, 0, sizeof(*x));
    return r;
}

static HRESULT apo_init2(Apo *x, const CLSID *clsid, IPropertyStore *ep, const GUID *mode, BOOL discovery)
{
    APOInitSystemEffects2 i2;
    memset(&i2, 0, sizeof(i2));
    i2.APOInit.cbSize = sizeof(i2);
    i2.APOInit.clsid = *clsid;
    i2.pAPOEndpointProperties = ep;            /* NULL stores must be tolerated */
    i2.AudioProcessingMode = mode ? *mode : E1_AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    i2.InitializeForDiscoveryOnly = discovery;
    return IAudioProcessingObject_Initialize(x->apo, sizeof(i2), (BYTE *)&i2);
}

static IAudioMediaType *mk_float(UINT32 ch, UINT32 rate)
{
    IAudioMediaType *t = NULL;
    e1mt_create_float(ch, rate, 0, &t);
    return t;
}

static IAudioMediaType *mk_pcm16(UINT32 ch, UINT32 rate)
{
    WAVEFORMATEX w;
    IAudioMediaType *t = NULL;
    memset(&w, 0, sizeof(w));
    w.wFormatTag = WAVE_FORMAT_PCM; w.nChannels = (WORD)ch; w.nSamplesPerSec = rate; w.wBitsPerSample = 16;
    w.nBlockAlign = (WORD)(2 * ch); w.nAvgBytesPerSec = rate * w.nBlockAlign;
    e1mt_create_from_wfx(&w, sizeof(w), &t);
    return t;
}

static int fmt_is(IAudioMediaType *t, UINT32 ch, UINT32 rate)
{
    UNCOMPRESSEDAUDIOFORMAT u;
    if (t == NULL || FAILED(IAudioMediaType_GetUncompressedAudioFormat(t, &u))) return 0;
    return IsEqualGUID(&u.guidFormatType, &E1_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && u.dwBytesPerSampleContainer == 4 &&
           u.dwValidBitsPerSample == 32 && u.dwSamplesPerFrame == ch && u.fFramesPerSecond == (FLOAT)rate;
}

static HRESULT apo_lock(Apo *x, IAudioMediaType *fin, IAudioMediaType *fout, UINT32 maxf, float *bin, float *bout)
{
    APO_CONNECTION_DESCRIPTOR din, dout, *pi = &din, *po = &dout;
    din.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL; din.pBuffer = (UINT_PTR)bin; din.u32MaxFrameCount = maxf;
    din.pFormat = fin; din.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    dout = din; dout.pBuffer = (UINT_PTR)bout; dout.pFormat = fout;
    return IAudioProcessingObjectConfiguration_LockForProcess(x->cfg, 1, &pi, 1, &po);
}

static HRESULT apo_lock_simple(Apo *x, UINT32 ch, UINT32 rate, UINT32 maxf, float *bin, float *bout)
{
    IAudioMediaType *f = mk_float(ch, rate);
    HRESULT hr = apo_lock(x, f, f, maxf, bin, bout);
    IAudioMediaType_Release(f);
    return hr;
}

static APO_BUFFER_FLAGS apo_run(Apo *x, float *bin, float *bout, UINT32 frames, APO_BUFFER_FLAGS flags)
{
    APO_CONNECTION_PROPERTY cin, cout, *pi = &cin, *po = &cout;
    cin.pBuffer = (UINT_PTR)bin; cin.u32ValidFrameCount = frames; cin.u32BufferFlags = flags;
    cin.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    cout.pBuffer = (UINT_PTR)bout; cout.u32ValidFrameCount = 0; cout.u32BufferFlags = BUFFER_INVALID;
    cout.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    IAudioProcessingObjectRT_APOProcess(x->rt, 1, &pi, 1, &po);
    if (cout.u32ValidFrameCount != frames) {
        ++g_fail;
        printf("  FAIL: output valid frame count %u != %u\n", cout.u32ValidFrameCount, frames);
    }
    return cout.u32BufferFlags;
}

/* APOProcess without the frame-count check; returns the output's valid frame count. */
static UINT32 apo_run_raw(Apo *x, float *bin, float *bout, UINT32 frames, APO_BUFFER_FLAGS flags,
                          APO_BUFFER_FLAGS *out_flags)
{
    APO_CONNECTION_PROPERTY cin, cout, *pi = &cin, *po = &cout;
    cin.pBuffer = (UINT_PTR)bin; cin.u32ValidFrameCount = frames; cin.u32BufferFlags = flags;
    cin.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    cout.pBuffer = (UINT_PTR)bout; cout.u32ValidFrameCount = 0; cout.u32BufferFlags = BUFFER_INVALID;
    cout.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    IAudioProcessingObjectRT_APOProcess(x->rt, 1, &pi, 1, &po);
    if (out_flags) *out_flags = cout.u32BufferFlags;
    return cout.u32ValidFrameCount;
}

/* Oversized buffer into an output connection that ends at a PAGE_NOACCESS guard page. Returns
   1 when APOProcess stayed inside the output buffer, 0 on an access violation. */
static int apo_run_guarded(Apo *x, UINT32 ch, UINT32 maxf, UINT32 frames, UINT32 *valid)
{
    SYSTEM_INFO si;
    size_t out_bytes = (size_t)maxf * ch * sizeof(float), pages, total;
    BYTE *mem;
    float *out, *in;
    DWORD old;
    int ok = 1;
    GetSystemInfo(&si);
    pages = (out_bytes + si.dwPageSize - 1) / si.dwPageSize;
    total = (pages + 1) * si.dwPageSize;
    mem = (BYTE *)VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    in = (float *)malloc((size_t)frames * ch * sizeof(float));
    if (!mem || !in) { free(in); if (mem) VirtualFree(mem, 0, MEM_RELEASE); return 0; }
    VirtualProtect(mem + pages * si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &old);
    out = (float *)(mem + pages * si.dwPageSize - out_bytes);
    fill_noise(in, (size_t)frames * ch);
    *valid = 0;
    __try {
        *valid = apo_run_raw(x, in, out, frames, BUFFER_VALID, NULL);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        ok = 0;
    }
    if (ok && *valid && memcmp(out, in, (size_t)*valid * ch * sizeof(float)) != 0) ok = 0;
    VirtualFree(mem, 0, MEM_RELEASE);
    free(in);
    return ok;
}

#define BUF_FRAMES 441u             /* 10 ms @ 44.1 kHz, what the engine uses */
#define MAX_CH     8u
static float g_in[BUF_FRAMES * MAX_CH], g_out[BUF_FRAMES * MAX_CH], g_ref[BUF_FRAMES * MAX_CH];

/* n buffers of noise, separate in/out buffers or in place; returns number of non-bit-exact buffers. */
static int run_bitexact(Apo *x, UINT32 ch, int nbuf, int in_place)
{
    int k, bad = 0;
    size_t n = (size_t)BUF_FRAMES * ch;
    for (k = 0; k < nbuf; ++k) {
        APO_BUFFER_FLAGS f;
        fill_noise(g_in, n);
        memcpy(g_ref, g_in, n * sizeof(float));
        if (in_place) {
            f = apo_run(x, g_in, g_in, BUF_FRAMES, BUFFER_VALID);
            if (memcmp(g_in, g_ref, n * sizeof(float)) != 0 || f != BUFFER_VALID) ++bad;
        } else {
            memset(g_out, 0xAB, n * sizeof(float));
            f = apo_run(x, g_in, g_out, BUF_FRAMES, BUFFER_VALID);
            if (memcmp(g_out, g_ref, n * sizeof(float)) != 0 || f != BUFFER_VALID) ++bad;
        }
    }
    return bad;
}

/* Feeds `nbuf` buffers of a 1 kHz sine (amplitude 0.25) and returns the output/input RMS
   ratio in dB over the second half (after crossfades and latency have settled). */
static double run_gain_db(Apo *x, UINT32 ch, UINT32 rate, int nbuf)
{
    static double phase;
    double ein = 0, eout = 0;
    int k;
    size_t i, n = (size_t)BUF_FRAMES * ch;
    for (k = 0; k < nbuf; ++k) {
        for (i = 0; i < BUF_FRAMES; ++i) {
            float s = (float)(0.25 * sin(phase));
            UINT32 c;
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / rate;
            for (c = 0; c < ch; ++c) g_in[i * ch + c] = s;
        }
        memcpy(g_ref, g_in, n * sizeof(float));
        apo_run(x, g_in, g_in, BUF_FRAMES, BUFFER_VALID);           /* in place, as the engine does */
        if (k >= nbuf / 2)
            for (i = 0; i < n; ++i) { ein += (double)g_ref[i] * g_ref[i]; eout += (double)g_in[i] * g_in[i]; }
    }
    if (ein <= 0 || eout <= 0) return -999.0;
    return 10.0 * log10(eout / ein);
}

static void params_gain_only(E1Params *p, int render_on, int capture_on, float gain_db)
{
    e1fx_params_default(p);
    p->render.master_on = (uint32_t)render_on;
    p->render.surround_on = p->render.crystal_on = p->render.bass_on = 0;
    p->render.svm_on = p->render.dialog_on = p->render.eq_on = p->render.scout_on = 0;
    p->render.output_gain_db = gain_db;
    p->capture.master_on = (uint32_t)capture_on;
    p->capture.nr_on = p->capture.svm_on = p->capture.vfx_on = 0;
    p->capture.output_gain_db = gain_db;
}

static int effects_list_count(Apo *x, HANDLE ev, GUID *first)
{
    LPGUID ids = NULL;
    UINT n = 99;
    HRESULT hr = IAudioSystemEffects3_GetEffectsList(x->fx, &ids, &n, ev);
    CHECK(hr == S_OK, "GetEffectsList hr=0x%08lX", (unsigned long)hr);
    if (n > 0 && ids && first) *first = ids[0];
    CoTaskMemFree(ids);
    return (int)n;
}

static float max_abs(const float *b, size_t n)
{
    float m = 0.f;
    size_t i;
    for (i = 0; i < n; ++i) if (fabsf(b[i]) > m) m = fabsf(b[i]);
    return m;
}

/* ======================================================================================== */
/* IPC unit tests                                                                            */

static wchar_t g_tmpdir[MAX_PATH], g_settings[MAX_PATH];

typedef struct StressCtx { E1SharedBlock *b; volatile LONG stop; LONG writes; int upper; } StressCtx;

static DWORD WINAPI stress_writer(LPVOID p)
{
    StressCtx *s = (StressCtx *)p;
    E1Params prm;
    LONG k = 0;
    int i;
    e1fx_params_default(&prm);
    while (!s->stop) {
        /* in every range used below; a second writer uses the other half so mixes are visible */
        float v = s->upper ? (float)(k % 12) : (float)(k % 12) - 12.f;
        prm.render.output_gain_db = v;
        prm.render.eq_preamp_db = v;
        for (i = 0; i < E1FX_EQ_BANDS; ++i) prm.render.eq_gain_db[i] = v;
        prm.capture.output_gain_db = v;
        e1ipc_write_params(s->b, &prm);
        ++k;
    }
    s->writes = k;
    return 0;
}

static void test_ipc(void)
{
    E1Params p, q;
    E1IpcMapping m, m2;
    int32_t seq = -1;
    unsigned long err;
    HANDLE h;

    section("ipc: crc32");
    CHECK(e1ipc_crc32("123456789", 9) == 0xCBF43926u, "crc=0x%08X", e1ipc_crc32("123456789", 9));
    CHECK(e1ipc_crc32("", 0) == 0u, "empty crc");

    section("ipc: settings file");
    err = e1ipc_load_settings(&p);
    e1fx_params_default(&q);
    CHECK(err != 0 && memcmp(&p, &q, sizeof(p)) == 0, "missing file -> error %lu + defaults", err);
    params_gain_only(&p, 1, 0, -3.f);
    p.render.eq_gain_db[3] = 99.f;                                  /* out of range: sanitized on save */
    err = e1ipc_save_settings(&p);
    CHECK(err == 0, "save err=%lu (creates the directory)", err);
    err = e1ipc_load_settings(&q);
    CHECK(err == 0 && q.render.master_on == 1 && q.render.output_gain_db == -3.f, "load err=%lu", err);
    CHECK(q.render.eq_gain_db[3] == 24.f, "sanitized eq gain = %f", q.render.eq_gain_db[3]);
    params_gain_only(&p, 1, 0, -5.f);
    CHECK(e1ipc_save_settings(&p) == 0, "overwrite (MoveFileEx replace)");
    CHECK(e1ipc_load_settings(&q) == 0 && q.render.output_gain_db == -5.f, "reload after replace");
    {   /* flip one byte of params -> CRC error, defaults returned */
        HANDLE f = CreateFileW(g_settings, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        BYTE byte = 0; DWORD io = 0;
        SetFilePointer(f, 40, NULL, FILE_BEGIN); ReadFile(f, &byte, 1, &io, NULL);
        byte ^= 0x5A;
        SetFilePointer(f, 40, NULL, FILE_BEGIN); WriteFile(f, &byte, 1, &io, NULL);
        CloseHandle(f);
        err = e1ipc_load_settings(&q);
        e1fx_params_default(&p);
        CHECK(err == ERROR_CRC && memcmp(&p, &q, sizeof(p)) == 0, "corrupted file -> err=%lu", err);
    }
    DeleteFileW(g_settings);

    section("ipc: section + seqlock");
    SetEnvironmentVariableW(L"E1FX_SECTION_OVERRIDE", L"Local\\E1Test_ipc_unit");
    err = e1ipc_open(&m, 0);
    CHECK(err == ERROR_FILE_NOT_FOUND, "open without create on missing section err=%lu", err);
    err = e1ipc_open(&m, 1);
    CHECK(err == 0 && m.block && m.created == 1, "create err=%lu created=%d", err, m.created);
    if (err) return;
    CHECK(m.block->magic == E1FX_MAGIC && m.block->block_size == sizeof(E1SharedBlock), "header initialised");
    CHECK(m.block->seq == 2, "seq after init = %ld", (long)m.block->seq);
    err = e1ipc_open(&m2, 1);
    CHECK(err == 0 && m2.created == 0 && m2.block, "second open shares the section, created=%d", m2.created);
    CHECK(e1ipc_read_params(m2.block, &seq, &p) == 1, "first read returns the initial snapshot");
    e1fx_params_default(&q);
    CHECK(memcmp(&p, &q, sizeof(p)) == 0, "initial snapshot = defaults (no settings file)");
    CHECK(e1ipc_read_params(m2.block, &seq, &p) == 0, "no new snapshot -> 0");
    params_gain_only(&q, 1, 1, -7.f);
    e1ipc_write_params(m.block, &q);
    CHECK(e1ipc_read_params(m2.block, &seq, &p) == 1 && p.render.output_gain_db == -7.f, "write -> read");
    CHECK(seq == 4, "seq=%ld", (long)seq);
    m.block->seq = 5;                                                /* writer died mid-update */
    CHECK(e1ipc_read_params(m2.block, &seq, &p) == 0, "odd seq -> no read");
    q.render.output_gain_db = -8.f;
    e1ipc_write_params(m.block, &q);
    CHECK(m.block->seq == 8 && e1ipc_read_params(m2.block, &seq, &p) == 1 && p.render.output_gain_db == -8.f,
          "writer recovers from odd seq (seq=%ld)", (long)m.block->seq);
    q.version = 99;
    e1ipc_write_params(m.block, &q);
    CHECK(e1ipc_read_params(m2.block, &seq, &p) == 0 && p.render.output_gain_db == -8.f, "foreign version ignored");
    q.version = E1FX_PARAMS_VERSION;

    section("ipc: seqlock stress (writer thread vs reader)");
    {
        StressCtx s;
        DWORD t0 = GetTickCount();
        long reads = 0, torn = 0;
        int i;
        memset(&s, 0, sizeof(s));
        s.b = m.block;
        h = CreateThread(NULL, 0, stress_writer, &s, 0, NULL);
        while (GetTickCount() - t0 < 400) {
            if (e1ipc_read_params(m2.block, &seq, &p)) {
                float v = p.render.output_gain_db;
                ++reads;
                if (p.render.eq_preamp_db != v || p.capture.output_gain_db != v) ++torn;
                for (i = 0; i < E1FX_EQ_BANDS; ++i) if (p.render.eq_gain_db[i] != v) { ++torn; break; }
            }
        }
        s.stop = 1;
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        printf("  writes=%ld consistent reads=%ld torn=%ld\n", (long)s.writes, reads, torn);
        CHECK(torn == 0 && reads > 100, "torn=%ld reads=%ld", torn, reads);
    }

    section("ipc: seqlock stress (two concurrent writers vs reader)");
    {
        StressCtx s1, s2;
        HANDLE h2;
        DWORD t0 = GetTickCount();
        long reads = 0, torn = 0, back = 0;
        LONG last = m.block->seq;
        int i;
        memset(&s1, 0, sizeof(s1));
        memset(&s2, 0, sizeof(s2));
        s1.b = m.block;
        s2.b = m.block;
        s2.upper = 1;
        h = CreateThread(NULL, 0, stress_writer, &s1, 0, NULL);
        h2 = CreateThread(NULL, 0, stress_writer, &s2, 0, NULL);
        while (GetTickCount() - t0 < 600) {
            LONG cur = *(volatile LONG *)&m.block->seq;
            if (cur < last) ++back;
            last = cur;
            if (e1ipc_read_params(m2.block, &seq, &p)) {
                float v = p.render.output_gain_db;
                ++reads;
                if (p.render.eq_preamp_db != v || p.capture.output_gain_db != v) ++torn;
                for (i = 0; i < E1FX_EQ_BANDS; ++i) if (p.render.eq_gain_db[i] != v) { ++torn; break; }
            }
        }
        s1.stop = 1;
        s2.stop = 1;
        WaitForSingleObject(h, INFINITE);
        WaitForSingleObject(h2, INFINITE);
        CloseHandle(h);
        CloseHandle(h2);
        printf("  writes=%ld+%ld consistent reads=%ld torn=%ld seq went backwards %ld times\n",
               (long)s1.writes, (long)s2.writes, reads, torn, back);
        CHECK(torn == 0 && back == 0 && reads > 100 && s1.writes > 100 && s2.writes > 100,
              "two writers: torn=%ld backwards=%ld reads=%ld", torn, back, reads);
    }
    e1ipc_close(&m2);
    e1ipc_close(&m);
    CHECK(m.block == NULL && m.section == NULL, "close clears the mapping");

    section("ipc: creator loads the settings file");
    params_gain_only(&p, 1, 0, -4.f);
    e1ipc_save_settings(&p);
    SetEnvironmentVariableW(L"E1FX_SECTION_OVERRIDE", L"Local\\E1Test_ipc_unit2");
    err = e1ipc_open(&m, 1);
    seq = -1;
    CHECK(err == 0 && m.created && e1ipc_read_params(m.block, &seq, &q) == 1 && q.render.output_gain_db == -4.f &&
          q.render.master_on == 1, "section initialised from file");
    e1ipc_close(&m);
    DeleteFileW(g_settings);
}

/* ======================================================================================== */
/* APO tests                                                                                 */

static void test_formats(Apo *x)
{
    IAudioMediaType *f44 = mk_float(2, 44100), *f48 = mk_float(2, 48000), *p16 = mk_pcm16(2, 44100);
    IAudioMediaType *f12 = mk_float(12, 44100), *f2k = mk_float(2, 2000), *out = NULL;
    HRESULT hr;

    hr = IAudioProcessingObject_IsInputFormatSupported(x->apo, NULL, f44, &out);
    CHECK(hr == S_OK && out == f44, "float32 44.1k stereo accepted as-is (hr=0x%08lX)", (unsigned long)hr);
    if (out) IAudioMediaType_Release(out), out = NULL;
    hr = IAudioProcessingObject_IsOutputFormatSupported(x->apo, f44, f44, &out);
    CHECK(hr == S_OK && out == f44, "output float32 matching opposite accepted");
    if (out) IAudioMediaType_Release(out), out = NULL;

    hr = IAudioProcessingObject_IsInputFormatSupported(x->apo, NULL, p16, &out);
    CHECK(hr == S_FALSE && fmt_is(out, 2, 44100), "int16 rejected with float32 44.1k/2 suggestion (hr=0x%08lX)",
          (unsigned long)hr);
    if (out) IAudioMediaType_Release(out), out = NULL;

    hr = IAudioProcessingObject_IsInputFormatSupported(x->apo, f48, f44, &out);
    CHECK(hr == S_FALSE && fmt_is(out, 2, 48000), "rate must match opposite -> suggest 48k");
    if (out) IAudioMediaType_Release(out), out = NULL;

    hr = IAudioProcessingObject_IsOutputFormatSupported(x->apo, NULL, f12, &out);
    CHECK(hr == S_FALSE && fmt_is(out, 8, 44100), "12 ch -> suggest 8 ch");
    if (out) IAudioMediaType_Release(out), out = NULL;

    hr = IAudioProcessingObject_IsInputFormatSupported(x->apo, f12, f44, &out);
    CHECK(hr == APOERR_FORMAT_NOT_SUPPORTED && out == NULL, "opposite 12 ch cannot be matched");

    hr = IAudioProcessingObject_IsInputFormatSupported(x->apo, NULL, f2k, &out);
    CHECK(hr == S_FALSE && fmt_is(out, 2, 8000), "2 kHz -> suggest 8 kHz");
    if (out) IAudioMediaType_Release(out), out = NULL;

    hr = IAudioProcessingObject_IsInputFormatSupported(x->apo, NULL, f44, NULL);
    CHECK(hr == E_POINTER, "NULL out pointer");

    IAudioMediaType_Release(f44); IAudioMediaType_Release(f48); IAudioMediaType_Release(p16);
    IAudioMediaType_Release(f12); IAudioMediaType_Release(f2k);
}

static void test_render(IClassFactory *cf, E1SharedBlock *panel)
{
    Apo x;
    APO_REG_PROPERTIES *rp = NULL;
    E1Params prm;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL), cev = CreateEventW(NULL, FALSE, FALSE, NULL);
    HNSTIME lat = -1;
    UINT32 ch = 0;
    GUID id;
    LONG alive0;
    double g;
    int k, bad;

    section("render: identity, registration properties, Initialize");
    if (!apo_new(cf, &x)) return;
    {
        IUnknown *u1 = NULL, *u2 = NULL;
        IAudioProcessingObjectRT_QueryInterface(x.rt, &IID_IUnknown, (void **)&u1);
        IAudioSystemEffects3_QueryInterface(x.fx, &IID_IUnknown, (void **)&u2);
        CHECK(u1 && u1 == u2 && (void *)u1 != (void *)x.apo, "COM identity (separate non-delegating IUnknown)");
        if (u1) IUnknown_Release(u1);
        if (u2) IUnknown_Release(u2);
        u1 = (IUnknown *)(void *)1;
        CHECK(IAudioProcessingObject_QueryInterface(x.apo, &IID_IClassFactory, (void **)&u1) == E_NOINTERFACE && !u1,
              "unknown interface");
        CHECK(IAudioProcessingObject_QueryInterface(x.apo, &E1_IID_IAudioSystemEffects2, (void **)&u1) == S_OK,
              "QI IAudioSystemEffects2");
        if (u1) IUnknown_Release(u1);
        CHECK(IAudioProcessingObject_AddRef(x.apo) == 6, "refcount: 5 held interfaces + AddRef");
        CHECK(IAudioProcessingObject_Release(x.apo) == 5, "refcount after Release");
    }
    CHECK_HR(IAudioProcessingObject_GetRegistrationProperties(x.apo, &rp), S_OK);
    if (rp) {
        CHECK(IsEqualCLSID(&rp->clsid, &CLSID_E1FxRenderMfx), "reg clsid");
        CHECK(rp->Flags == 0xF, "flags=0x%X", (unsigned)rp->Flags);
        CHECK(wcscmp(rp->szFriendlyName, L"E1 Studio Effects") == 0, "friendly name");
        CHECK(rp->u32MinInputConnections == 1 && rp->u32MaxInputConnections == 1 &&
              rp->u32MinOutputConnections == 1 && rp->u32MaxOutputConnections == 1, "connections");
        CHECK(rp->u32NumAPOInterfaces == 6 && IsEqualIID(&rp->iidAPOInterfaceList[0], &E1_IID_IAudioProcessingObject),
              "interface list");
        CoTaskMemFree(rp);
    }
    CHECK_HR(apo_lock_simple(&x, 2, 44100, BUF_FRAMES, g_in, g_out), APOERR_NOT_INITIALIZED);
    CHECK_HR(apo_init2(&x, &CLSID_E1FxCaptureMfx, NULL, NULL, FALSE), APOERR_INVALID_APO_CLSID);
    {
        BYTE junk[60];
        memset(junk, 0, sizeof(junk));
        ((APOInitBaseStruct *)junk)->cbSize = sizeof(junk);
        ((APOInitBaseStruct *)junk)->clsid = CLSID_E1FxRenderMfx;
        CHECK_HR(IAudioProcessingObject_Initialize(x.apo, sizeof(junk), junk), E_INVALIDARG);
    }
    CHECK_HR(apo_init2(&x, &CLSID_E1FxRenderMfx, NULL, NULL, FALSE), S_OK);
    CHECK_HR(apo_init2(&x, &CLSID_E1FxRenderMfx, NULL, NULL, FALSE), APOERR_ALREADY_INITIALIZED);

    section("render: format negotiation");
    test_formats(&x);

    section("render: LockForProcess");
    {
        IAudioMediaType *f = mk_float(2, 44100), *p16 = mk_pcm16(2, 44100), *m1 = mk_float(1, 44100);
        APO_CONNECTION_DESCRIPTOR d, *pp[2] = { &d, &d };
        d.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL; d.pBuffer = (UINT_PTR)g_in; d.u32MaxFrameCount = BUF_FRAMES;
        d.pFormat = f; d.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
        CHECK_HR(IAudioProcessingObjectConfiguration_LockForProcess(x.cfg, 2, pp, 1, pp), APOERR_NUM_CONNECTIONS_INVALID);
        CHECK_HR(apo_lock(&x, p16, p16, BUF_FRAMES, g_in, g_out), APOERR_INVALID_CONNECTION_FORMAT);
        CHECK_HR(apo_lock(&x, f, m1, BUF_FRAMES, g_in, g_out), APOERR_INVALID_CONNECTION_FORMAT);
        IAudioMediaType_Release(f); IAudioMediaType_Release(p16); IAudioMediaType_Release(m1);
    }
    CHECK_HR(apo_lock_simple(&x, 2, 44100, BUF_FRAMES, g_in, g_out), S_OK);
    CHECK_HR(apo_lock_simple(&x, 2, 44100, BUF_FRAMES, g_in, g_out), APOERR_APO_LOCKED);
    CHECK(IAudioProcessingObject_GetInputChannelCount(x.apo, &ch) == S_OK && ch == 2, "channel count %u", ch);
    CHECK(IAudioProcessingObject_GetLatency(x.apo, &lat) == S_OK && lat == 0, "latency while bypassed = %lld", lat);
    CHECK(IAudioProcessingObjectRT_CalcInputFrames(x.rt, 441) == 441 && IAudioProcessingObjectRT_CalcOutputFrames(x.rt, 480) == 480,
          "frame calc");
    CHECK(effects_list_count(&x, ev, NULL) == 0, "effects list empty with defaults (master off)");

    section("render: bit-exact passthrough with default params");
    alive0 = panel->render_status.alive;
    bad = run_bitexact(&x, 2, 100, 0);
    CHECK(bad == 0, "%d of 100 separate-buffer blocks not bit-exact", bad);
    bad = run_bitexact(&x, 2, 100, 1);
    CHECK(bad == 0, "%d of 100 in-place blocks not bit-exact", bad);
    for (k = 0; k < 10; ++k) {
        size_t n = BUF_FRAMES * 2;
        APO_BUFFER_FLAGS f;
        fill_noise(g_in, n);                                       /* garbage in a SILENT buffer */
        f = apo_run(&x, g_in, g_out, BUF_FRAMES, BUFFER_SILENT);
        if (f != BUFFER_SILENT || max_abs(g_out, n) != 0.f) { bad++; }
    }
    CHECK(bad == 0, "BUFFER_SILENT in bypass -> silent flag + zero data");
    bad = run_bitexact(&x, 2, 5, 1);
    CHECK(bad == 0, "bit-exact again after silence");
    {   /* odd sizes, zero frames */
        APO_BUFFER_FLAGS f = apo_run(&x, g_in, g_out, 0, BUFFER_VALID);
        CHECK(f == BUFFER_VALID, "0 frames");
    }
    CHECK(panel->render_status.alive - alive0 == 216, "heartbeat counted %ld buffers", (long)(panel->render_status.alive - alive0));
    CHECK(panel->render_status.sample_rate == 44100 && panel->render_status.channels == 2 &&
          panel->render_status.bypass == 1, "status rate/ch/bypass");

    section("render: parameter change through the shared section");
    params_gain_only(&prm, 1, 0, -6.f);
    e1ipc_write_params(panel, &prm);
    g = run_gain_db(&x, 2, 44100, 100);
    printf("  measured gain %.3f dB (expected -6)\n", g);
    CHECK(fabs(g + 6.0) < 0.5, "gain %.3f dB", g);
    CHECK(panel->render_status.bypass == 0, "status shows active chain");
    CHECK(panel->render_status.peak_in[0] > 0.2f && panel->render_status.peak_out[0] > 0.05f, "meters %.3f %.3f",
          panel->render_status.peak_in[0], panel->render_status.peak_out[0]);
    CHECK(WaitForSingleObject(ev, 3000) == WAIT_OBJECT_0, "effects-changed event signalled by the housekeeping timer");
    id = GUID_NULL;
    CHECK(effects_list_count(&x, ev, &id) == 1 && IsEqualGUID(&id, &E1FX_EFFECT_RENDER_CHAIN), "effects list = render chain");
    IAudioProcessingObject_GetLatency(x.apo, &lat);
    printf("  GetLatency while active = %lld hns\n", lat);
    CHECK(lat > 0 && lat < 2000000, "latency %lld", lat);

    section("render: BUFFER_SILENT while active (tails flushed, garbage ignored)");
    {
        size_t i, n = BUF_FRAMES * 2;
        APO_BUFFER_FLAGS f = BUFFER_INVALID;
        for (k = 0; k < 60; ++k) {
            for (i = 0; i < n; ++i) g_in[i] = 0.9f;
            f = apo_run(&x, g_in, g_out, BUF_FRAMES, BUFFER_SILENT);
        }
        CHECK(f == BUFFER_VALID, "flag VALID while the chain runs");
        CHECK(max_abs(g_out, n) < 1e-3f, "tail decayed, garbage not processed (max %g)", max_abs(g_out, n));
    }

    section("render: IAudioSystemEffects3 gate");
    {
        AUDIO_SYSTEMEFFECT *e = NULL;
        UINT n = 0;
        CHECK_HR(IAudioSystemEffects3_GetControllableSystemEffectsList(x.fx, &e, &n, cev), S_OK);
        CHECK(n == 1 && e && IsEqualGUID(&e->id, &E1FX_EFFECT_RENDER_CHAIN) && e->canSetState &&
              e->state == AUDIO_SYSTEMEFFECT_STATE_ON, "controllable list");
        CoTaskMemFree(e); e = NULL;
        CHECK_HR(IAudioSystemEffects3_SetAudioSystemEffectState(x.fx, GUID_NULL, AUDIO_SYSTEMEFFECT_STATE_OFF), E_INVALIDARG);
        CHECK_HR(IAudioSystemEffects3_SetAudioSystemEffectState(x.fx, E1FX_EFFECT_RENDER_CHAIN, AUDIO_SYSTEMEFFECT_STATE_OFF), S_OK);
        CHECK(WaitForSingleObject(cev, 0) == WAIT_OBJECT_0, "controllable-list event signalled");
        run_bitexact(&x, 2, 10, 1);                                 /* > crossfade */
        bad = run_bitexact(&x, 2, 50, 1);
        CHECK(bad == 0, "gate OFF -> bit-exact (%d bad)", bad);
        CHECK(effects_list_count(&x, ev, NULL) == 0, "effects list empty while gated off");
        CHECK_HR(IAudioSystemEffects3_GetControllableSystemEffectsList(x.fx, &e, &n, NULL), S_OK);
        CHECK(n == 1 && e && e->state == AUDIO_SYSTEMEFFECT_STATE_OFF, "controllable state OFF");
        CoTaskMemFree(e);
        CHECK_HR(IAudioSystemEffects3_SetAudioSystemEffectState(x.fx, E1FX_EFFECT_RENDER_CHAIN, AUDIO_SYSTEMEFFECT_STATE_ON), S_OK);
        g = run_gain_db(&x, 2, 44100, 60);
        CHECK(fabs(g + 6.0) < 0.5, "gate ON again -> gain %.3f dB", g);
    }

    section("render: every effect on (smoke: finite output, CPU per 10 ms buffer)");
    {
        LARGE_INTEGER f0, t0, t1;
        size_t i, n = BUF_FRAMES * 2;
        int finite = 1, nb = 300;
        float peak = 0.f;
        e1fx_params_default(&prm);
        prm.render.master_on = prm.render.surround_on = prm.render.crystal_on = prm.render.bass_on = 1;
        prm.render.svm_on = prm.render.dialog_on = prm.render.eq_on = prm.render.scout_on = 1;
        for (i = 0; i < E1FX_EQ_BANDS; ++i) prm.render.eq_gain_db[i] = (i & 1) ? 6.f : -6.f;
        e1ipc_write_params(panel, &prm);
        QueryPerformanceFrequency(&f0);
        QueryPerformanceCounter(&t0);
        for (k = 0; k < nb; ++k) {
            fill_noise(g_in, n);
            g_in[5] = 0.5f;
            apo_run(&x, g_in, g_in, BUF_FRAMES, BUFFER_VALID);
            for (i = 0; i < n; ++i) {
                if (!isfinite(g_in[i])) finite = 0;
                if (fabsf(g_in[i]) > peak) peak = fabsf(g_in[i]);
            }
        }
        QueryPerformanceCounter(&t1);
        printf("  %d buffers, peak %.3f, %.1f us per 10 ms stereo buffer (%.2f%% of one core)\n", nb, peak,
               (double)(t1.QuadPart - t0.QuadPart) * 1e6 / f0.QuadPart / nb,
               (double)(t1.QuadPart - t0.QuadPart) * 100.0 / f0.QuadPart / (nb * 0.01));
        CHECK(finite, "non-finite output with every effect on");
        /* -0.3 dBFS ceiling (+0.05 dB limiter tolerance), including the module fade-ins */
        CHECK(peak <= 0.9716f, "safety limiter ceiling exceeded: peak %.4f", peak);
        CHECK(panel->render_status.bypass == 0, "active");
    }

    section("render: Scout Mode alone keeps the chain active; Windows gate still forces bit-exact");
    {
        e1fx_params_default(&prm);
        prm.render.scout_on = 1;                                   /* master_on = eq_on = 0 */
        e1ipc_write_params(panel, &prm);
        g = run_gain_db(&x, 2, 44100, 40);
        CHECK(panel->render_status.bypass == 0, "Scout Mode with master off: chain active");
        CHECK(effects_list_count(&x, ev, NULL) == 1, "effects list non-empty with Scout Mode only");
        CHECK_HR(IAudioSystemEffects3_SetAudioSystemEffectState(x.fx, E1FX_EFFECT_RENDER_CHAIN, AUDIO_SYSTEMEFFECT_STATE_OFF), S_OK);
        run_bitexact(&x, 2, 10, 1);                                 /* > crossfade */
        bad = run_bitexact(&x, 2, 50, 1);
        CHECK(bad == 0, "gate OFF with Scout Mode on -> bit-exact (%d bad)", bad);
        CHECK(effects_list_count(&x, ev, NULL) == 0, "effects list empty while gated off (Scout Mode on)");
        CHECK_HR(IAudioSystemEffects3_SetAudioSystemEffectState(x.fx, E1FX_EFFECT_RENDER_CHAIN, AUDIO_SYSTEMEFFECT_STATE_ON), S_OK);
    }

    section("render: master off via IPC -> back to bit-exact");
    params_gain_only(&prm, 0, 0, -6.f);
    e1ipc_write_params(panel, &prm);
    run_bitexact(&x, 2, 10, 1);
    bad = run_bitexact(&x, 2, 50, 0);
    CHECK(bad == 0, "bit-exact after master off (%d bad)", bad);
    CHECK(panel->render_status.bypass == 1, "status bypass");
    CHECK(IAudioProcessingObject_GetLatency(x.apo, &lat) == S_OK && lat == 0, "latency 0 in bypass");

    section("render: Reset / Unlock / relock 48 kHz mono");
    CHECK_HR(IAudioProcessingObject_Reset(x.apo), S_OK);
    CHECK_HR(IAudioProcessingObjectConfiguration_UnlockForProcess(x.cfg), S_OK);
    CHECK_HR(IAudioProcessingObjectConfiguration_UnlockForProcess(x.cfg), APOERR_ALREADY_UNLOCKED);
    CHECK_HR(apo_lock_simple(&x, 1, 48000, 480, g_in, g_out), S_OK);
    {
        int b2 = 0;
        for (k = 0; k < 20; ++k) {
            fill_noise(g_in, 480);
            memcpy(g_ref, g_in, 480 * sizeof(float));
            apo_run(&x, g_in, g_out, 480, BUFFER_VALID);
            if (memcmp(g_out, g_ref, 480 * sizeof(float)) != 0) ++b2;
        }
        CHECK(b2 == 0, "48k mono bit-exact");
        CHECK(apo_run_raw(&x, g_in, g_out, 481, BUFFER_VALID, NULL) == 480,
              "over max: passthrough clamped to max_frames");
        CHECK(panel->render_status.xruns >= 1, "oversized buffer counted as xrun");
    }
    CHECK_HR(IAudioProcessingObjectConfiguration_UnlockForProcess(x.cfg), S_OK);
    CHECK(panel->render_status.bypass == 1, "status idle after unlock");

    section("render: GetLatency valid right after LockForProcess (active params)");
    {
        UINT32 valid = 0;
        params_gain_only(&prm, 1, 0, -6.f);
        e1ipc_write_params(panel, &prm);
        CHECK_HR(apo_lock_simple(&x, 2, 44100, 512, g_in, g_out), S_OK);
        lat = -1;
        CHECK(IAudioProcessingObject_GetLatency(x.apo, &lat) == S_OK && lat > 0,
              "latency after Lock with effects on = %lld hns (must not be 0)", lat);
        printf("  GetLatency directly after LockForProcess = %lld hns\n", lat);
        CHECK(apo_run_guarded(&x, 2, 512, 768, &valid) && valid == 512,
              "frames 768 > max 512 with separate buffers: no write past the output (valid %u)", valid);
        CHECK_HR(IAudioProcessingObjectConfiguration_UnlockForProcess(x.cfg), S_OK);
        CHECK_HR(apo_lock_simple(&x, 2, 44100, 512, g_in, g_out), S_OK);
        lat = -1;
        CHECK(IAudioProcessingObject_GetLatency(x.apo, &lat) == S_OK && lat > 0, "latency after re-Lock = %lld", lat);
        CHECK_HR(IAudioProcessingObjectConfiguration_UnlockForProcess(x.cfg), S_OK);
        params_gain_only(&prm, 0, 0, -6.f);
        e1ipc_write_params(panel, &prm);
    }

    CHECK(apo_free(&x) == 0, "final Release returns 0");
    CloseHandle(ev);
    CloseHandle(cev);
}

static void test_render_v3_gates(IClassFactory *cf, E1SharedBlock *panel)
{
    Apo x;
    FakeStore store = { &g_fs_vtbl, 1, ENDPOINT_SYSFX_DISABLED };
    FakeDevice dev = { &g_fd_vtbl, 1 };
    FakeColl coll = { &g_fc_vtbl, 1, NULL };
    APOInitSystemEffects3 i3;
    APO_NOTIFICATION_DESCRIPTOR *d = NULL;
    APO_NOTIFICATION n;
    DWORD cnt = 0;
    E1Params prm;
    double g;
    int bad;

    section("render: APOInitSystemEffects3, Windows enhancements off, notifications");
    coll.dev = (IMMDevice *)&dev;
    if (!apo_new(cf, &x)) return;
    memset(&i3, 0, sizeof(i3));
    i3.APOInit.cbSize = sizeof(i3);
    i3.APOInit.clsid = CLSID_E1FxRenderMfx;
    i3.pAPOEndpointProperties = (IPropertyStore *)&store;
    i3.pDeviceCollection = (IMMDeviceCollection *)&coll;
    i3.AudioProcessingMode = E1_AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    CHECK_HR(IAudioProcessingObject_Initialize(x.apo, sizeof(i3), (BYTE *)&i3), S_OK);
    CHECK(dev.refs == 2, "APO holds the endpoint device (refs=%ld)", dev.refs);

    params_gain_only(&prm, 1, 0, -6.f);
    e1ipc_write_params(panel, &prm);
    CHECK_HR(apo_lock_simple(&x, 2, 44100, BUF_FRAMES, g_in, g_out), S_OK);
    bad = run_bitexact(&x, 2, 50, 1);
    CHECK(bad == 0, "Disable_SysFx=1 forces bit-exact although master is on (%d bad)", bad);
    CHECK(effects_list_count(&x, NULL, NULL) == 0, "effects list empty while Windows disabled enhancements");

    CHECK_HR(IAudioProcessingObjectNotifications_GetApoNotificationRegistrationInfo(x.ntf, &d, &cnt), S_OK);
    CHECK(cnt == 1 && d && d->type == APO_NOTIFICATION_TYPE_ENDPOINT_PROPERTY_CHANGE &&
          d->audioEndpointPropertyChange.device == (IMMDevice *)&dev, "registers for endpoint property changes");
    if (d) { if (d->audioEndpointPropertyChange.device) IMMDevice_Release(d->audioEndpointPropertyChange.device); CoTaskMemFree(d); }

    store.disable_sysfx = ENDPOINT_SYSFX_ENABLED;
    memset(&n, 0, sizeof(n));
    n.type = APO_NOTIFICATION_TYPE_ENDPOINT_PROPERTY_CHANGE;
    n.audioEndpointPropertyChange.endpoint = (IMMDevice *)&dev;
    n.audioEndpointPropertyChange.propertyStore = (IPropertyStore *)&store;
    n.audioEndpointPropertyChange.propertyKey = E1_PKEY_AudioEndpoint_Disable_SysFx;
    IAudioProcessingObjectNotifications_HandleNotification(x.ntf, &n);
    g = run_gain_db(&x, 2, 44100, 60);
    CHECK(fabs(g + 6.0) < 0.5, "enhancements re-enabled by notification -> gain %.3f dB", g);

    store.disable_sysfx = ENDPOINT_SYSFX_DISABLED;
    IAudioProcessingObjectNotifications_HandleNotification(x.ntf, &n);
    run_bitexact(&x, 2, 10, 1);
    bad = run_bitexact(&x, 2, 30, 1);
    CHECK(bad == 0, "disabled again by notification -> bit-exact (%d bad)", bad);

    CHECK(apo_free(&x) == 0, "final Release (locked APO released without Unlock)");
    CHECK(dev.refs == 1, "device released by the APO (refs=%ld)", dev.refs);
    CHECK(store.refs == 1 && coll.refs == 1, "no leaked store/collection refs");

    section("render: RAW mode and discovery-only Initialize");
    if (!apo_new(cf, &x)) return;
    CHECK_HR(apo_init2(&x, &CLSID_E1FxRenderMfx, NULL, &E1_AUDIO_SIGNALPROCESSINGMODE_RAW, FALSE), S_OK);
    CHECK_HR(apo_lock_simple(&x, 2, 44100, BUF_FRAMES, g_in, g_out), S_OK);
    bad = run_bitexact(&x, 2, 30, 1);
    CHECK(bad == 0, "RAW mode is always bit-exact (%d bad)", bad);
    CHECK(apo_free(&x) == 0, "release");
    if (!apo_new(cf, &x)) return;
    CHECK_HR(apo_init2(&x, &CLSID_E1FxRenderMfx, NULL, NULL, TRUE), S_OK);
    CHECK_HR(apo_lock_simple(&x, 2, 44100, BUF_FRAMES, g_in, g_out), APOERR_NOT_INITIALIZED);
    CHECK(effects_list_count(&x, NULL, NULL) == 1, "discovery instance still reports the effect state");
    CHECK(apo_free(&x) == 0, "release");
    {   /* APOInitSystemEffects (v1) and the bare base struct */
        APOInitSystemEffects i1;
        APOInitBaseStruct ib;
        if (!apo_new(cf, &x)) return;
        memset(&i1, 0, sizeof(i1));
        i1.APOInit.cbSize = sizeof(i1);
        i1.APOInit.clsid = CLSID_E1FxRenderMfx;
        CHECK_HR(IAudioProcessingObject_Initialize(x.apo, sizeof(i1), (BYTE *)&i1), S_OK);
        apo_free(&x);
        if (!apo_new(cf, &x)) return;
        ib.cbSize = sizeof(ib);
        ib.clsid = CLSID_E1FxRenderMfx;
        CHECK_HR(IAudioProcessingObject_Initialize(x.apo, sizeof(ib), (BYTE *)&ib), S_OK);
        apo_free(&x);
    }
    params_gain_only(&prm, 0, 0, 0.f);
    e1ipc_write_params(panel, &prm);
}

static void test_capture(IClassFactory *cf, E1SharedBlock *panel)
{
    Apo x;
    E1Params prm;
    APO_REG_PROPERTIES *rp = NULL;
    GUID id = GUID_NULL;
    double g;
    int bad;

    section("capture: Initialize, mono 44.1 kHz bit-exact, params, effect list");
    if (!apo_new(cf, &x)) return;
    CHECK(IAudioProcessingObject_GetRegistrationProperties(x.apo, &rp) == S_OK && rp &&
          IsEqualCLSID(&rp->clsid, &CLSID_E1FxCaptureMfx), "capture reg clsid");
    CoTaskMemFree(rp);
    CHECK_HR(apo_init2(&x, &CLSID_E1FxRenderMfx, NULL, NULL, FALSE), APOERR_INVALID_APO_CLSID);
    CHECK_HR(apo_init2(&x, &CLSID_E1FxCaptureMfx, NULL, NULL, FALSE), S_OK);
    CHECK_HR(apo_lock_simple(&x, 1, 44100, BUF_FRAMES, g_in, g_out), S_OK);
    bad = run_bitexact(&x, 1, 100, 0) + run_bitexact(&x, 1, 100, 1);
    CHECK(bad == 0, "capture bit-exact (%d bad)", bad);
    CHECK(panel->capture_status.channels == 1 && panel->capture_status.alive >= 200, "capture status written");

    params_gain_only(&prm, 1, 1, -6.f);
    prm.render.master_on = 0;
    e1ipc_write_params(panel, &prm);
    g = run_gain_db(&x, 1, 44100, 100);
    printf("  capture measured gain %.3f dB (expected -6)\n", g);
    CHECK(fabs(g + 6.0) < 0.5, "capture gain %.3f dB", g);
    CHECK(effects_list_count(&x, NULL, &id) == 1 && IsEqualGUID(&id, &E1FX_EFFECT_CAPTURE_CHAIN), "capture effect id");
    CHECK(panel->render_status.bypass == 1, "render status untouched by capture APO");

    params_gain_only(&prm, 0, 0, 0.f);
    e1ipc_write_params(panel, &prm);
    run_bitexact(&x, 1, 10, 1);
    bad = run_bitexact(&x, 1, 50, 1);
    CHECK(bad == 0, "capture bit-exact after master off (%d bad)", bad);
    CHECK_HR(IAudioProcessingObjectConfiguration_UnlockForProcess(x.cfg), S_OK);
    CHECK(apo_free(&x) == 0, "capture final Release");
}

/* IPC unavailable: settings-file params are used, and the timer reconnects later. */
static void test_ipc_fallback(IClassFactory *cf)
{
    Apo x;
    E1Params prm;
    E1IpcMapping m;
    HANDLE squat;
    wchar_t name[64];
    double g;
    LONG a0;
    int k;

    section("render: IPC unavailable -> settings file params, then reconnect");
    swprintf_s(name, 64, L"Local\\E1Test_%lu_fallback", GetCurrentProcessId());
    squat = CreateMutexW(NULL, FALSE, name);               /* same name, wrong object type */
    SetEnvironmentVariableW(L"E1FX_SECTION_OVERRIDE", name);
    params_gain_only(&prm, 1, 0, -6.f);
    CHECK(e1ipc_save_settings(&prm) == 0, "save settings");

    if (!apo_new(cf, &x)) return;
    CHECK_HR(apo_init2(&x, &CLSID_E1FxRenderMfx, NULL, NULL, FALSE), S_OK);
    CHECK(e1ipc_open(&m, 0) != 0, "section really unavailable");
    CHECK_HR(apo_lock_simple(&x, 2, 44100, BUF_FRAMES, g_in, g_out), S_OK);
    g = run_gain_db(&x, 2, 44100, 60);
    CHECK(fabs(g + 6.0) < 0.5, "settings-file params applied without IPC (gain %.3f dB)", g);

    CloseHandle(squat);                                       /* name becomes free */
    for (k = 0; k < 40 && e1ipc_open(&m, 0) != 0; ++k) Sleep(100);
    CHECK(m.block != NULL, "APO timer created the section after %d00 ms", k);
    Sleep(50);                                 /* let the timer callback publish the mapping */
    if (m.block) {
        E1Params cur;
        int32_t seq = -1;
        CHECK(e1ipc_read_params(m.block, &seq, &cur) == 1 && cur.render.output_gain_db == -6.f,
              "section initialised from the settings file");
        a0 = m.block->render_status.alive;
        params_gain_only(&prm, 0, 0, 0.f);
        e1ipc_write_params(m.block, &prm);
        run_bitexact(&x, 2, 10, 1);
        CHECK(run_bitexact(&x, 2, 20, 1) == 0, "params from the late section are applied");
        CHECK(m.block->render_status.alive - a0 == 30, "heartbeat after reconnect");
        e1ipc_close(&m);
    }
    apo_free(&x);
    DeleteFileW(g_settings);
}

static LONG reg_dw(HKEY root, const wchar_t *path, const wchar_t *name, DWORD *v)
{
    DWORD sz = sizeof(*v);
    return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, NULL, v, &sz);
}

static void test_registration(PFN_DllVoid reg, PFN_DllVoid unreg, const wchar_t *dllpath)
{
    wchar_t sub[128], buf[MAX_PATH * 2];
    HKEY fake = NULL;
    DWORD v = 0, sz;
    LONG r;

    section("registration (HKLM redirected to a temporary HKCU key)");
    swprintf_s(sub, 128, L"Software\\E1StudioApoTest_%lu", GetCurrentProcessId());
    r = RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &fake, NULL);
    CHECK(r == ERROR_SUCCESS, "create test key %ld", r);
    if (r) return;
    r = RegOverridePredefKey(HKEY_LOCAL_MACHINE, fake);
    CHECK(r == ERROR_SUCCESS, "RegOverridePredefKey %ld", r);
    if (r == ERROR_SUCCESS) {
        CHECK_HR(reg(), S_OK);
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL);

        sz = sizeof(buf);
        r = RegGetValueW(fake, L"SOFTWARE\\Classes\\CLSID\\" E1FX_RENDER_CLSID_STR L"\\InprocServer32", NULL,
                         RRF_RT_REG_SZ, NULL, buf, &sz);
        CHECK(r == 0 && _wcsicmp(buf, dllpath) == 0, "InprocServer32 = %ls", r == 0 ? buf : L"?");
        sz = sizeof(buf);
        r = RegGetValueW(fake, L"SOFTWARE\\Classes\\CLSID\\" E1FX_CAPTURE_CLSID_STR L"\\InprocServer32",
                         L"ThreadingModel", RRF_RT_REG_SZ, NULL, buf, &sz);
        CHECK(r == 0 && wcscmp(buf, L"Both") == 0, "ThreadingModel Both");
        {
            const wchar_t *apo = L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\" E1FX_RENDER_CLSID_STR;
            CHECK(reg_dw(fake, apo, L"Flags", &v) == 0 && v == 0xF, "Flags=%lu", v);
            CHECK(reg_dw(fake, apo, L"NumAPOInterfaces", &v) == 0 && v == 6, "NumAPOInterfaces=%lu", v);
            CHECK(reg_dw(fake, apo, L"MaxInstances", &v) == 0 && v == 0xFFFFFFFFu, "MaxInstances");
            CHECK(reg_dw(fake, apo, L"MinInputConnections", &v) == 0 && v == 1, "MinInputConnections");
            sz = sizeof(buf);
            CHECK(RegGetValueW(fake, apo, L"FriendlyName", RRF_RT_REG_SZ, NULL, buf, &sz) == 0 &&
                  wcscmp(buf, L"E1 Studio Effects") == 0, "FriendlyName");
            sz = sizeof(buf);
            CHECK(RegGetValueW(fake, apo, L"APOInterface0", RRF_RT_REG_SZ, NULL, buf, &sz) == 0 &&
                  _wcsicmp(buf, L"{FD7F2B29-24D0-4B5C-B177-592C39F9CA10}") == 0, "APOInterface0 = %ls", buf);
            sz = sizeof(buf);
            CHECK(RegGetValueW(fake, apo, L"APOInterface5", RRF_RT_REG_SZ, NULL, buf, &sz) == 0 &&
                  _wcsicmp(buf, L"{C58B31CD-FC6A-4255-BC1F-AD29BB0A4A17}") == 0, "APOInterface5 = %ls", buf);
        }
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, fake);
        CHECK_HR(unreg(), S_OK);
        CHECK_HR(unreg(), S_OK);                                   /* idempotent */
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL);
        CHECK(reg_dw(fake, L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\" E1FX_RENDER_CLSID_STR,
                     L"Flags", &v) == ERROR_FILE_NOT_FOUND, "APO key removed");
        sz = sizeof(buf);
        CHECK(RegGetValueW(fake, L"SOFTWARE\\Classes\\CLSID\\" E1FX_CAPTURE_CLSID_STR L"\\InprocServer32", NULL,
                           RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_FILE_NOT_FOUND, "CLSID key removed");
    }
    RegCloseKey(fake);
    RegDeleteTreeW(HKEY_CURRENT_USER, sub);
    RegDeleteKeyW(HKEY_CURRENT_USER, sub);
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t dllpath[MAX_PATH * 2], secname[64];
    HMODULE dll;
    PFN_DllGetClassObject gco;
    PFN_DllVoid can_unload, reg, unreg;
    IClassFactory *cf_r = NULL, *cf_c = NULL;
    E1IpcMapping panel;
    unsigned long err;

    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (argc > 1) GetFullPathNameW(argv[1], MAX_PATH * 2, dllpath, NULL);
    else {
        wchar_t *slash;
        GetModuleFileNameW(NULL, dllpath, MAX_PATH * 2);
        slash = wcsrchr(dllpath, L'\\');
        wcscpy_s(slash + 1, MAX_PATH * 2 - (slash + 1 - dllpath), L"e1fx.dll");
    }

    GetTempPathW(MAX_PATH, g_tmpdir);
    swprintf_s(g_tmpdir + wcslen(g_tmpdir), MAX_PATH - wcslen(g_tmpdir), L"e1fx_apotest_%lu", GetCurrentProcessId());
    swprintf_s(g_settings, MAX_PATH, L"%s\\settings.bin", g_tmpdir);
    SetEnvironmentVariableW(L"E1FX_SETTINGS_OVERRIDE", g_settings);
    printf("E1 Studio APO test host\n  dll: %ls\n  settings override: %ls\n", dllpath, g_settings);

    test_ipc();

    section("dll: load and class objects");
    dll = LoadLibraryW(dllpath);
    CHECK(dll != NULL, "LoadLibrary error %lu", GetLastError());
    if (!dll) goto out;
    gco = (PFN_DllGetClassObject)(void *)GetProcAddress(dll, "DllGetClassObject");
    can_unload = (PFN_DllVoid)(void *)GetProcAddress(dll, "DllCanUnloadNow");
    reg = (PFN_DllVoid)(void *)GetProcAddress(dll, "DllRegisterServer");
    unreg = (PFN_DllVoid)(void *)GetProcAddress(dll, "DllUnregisterServer");
    CHECK(gco && can_unload && reg && unreg, "all four exports present");
    if (!(gco && can_unload && reg && unreg)) goto out;
    CHECK_HR(can_unload(), S_OK);
    {
        void *p = (void *)1;
        CHECK(gco(&GUID_NULL, &IID_IClassFactory, &p) == CLASS_E_CLASSNOTAVAILABLE && p == NULL, "unknown CLSID");
    }
    CHECK_HR(gco(&CLSID_E1FxRenderMfx, &IID_IClassFactory, (void **)&cf_r), S_OK);
    CHECK_HR(gco(&CLSID_E1FxCaptureMfx, &IID_IClassFactory, (void **)&cf_c), S_OK);
    if (!cf_r || !cf_c) goto out;
    CHECK_HR(can_unload(), S_FALSE);
    {
        IUnknown *u = NULL;
        CHECK(IClassFactory_CreateInstance(cf_r, (IUnknown *)cf_c, &E1_IID_IAudioProcessingObject, (void **)&u) ==
                  CLASS_E_NOAGGREGATION && !u, "aggregation only for IID_IUnknown");
        test_aggregation(cf_r);
        CHECK(IClassFactory_CreateInstance(cf_r, NULL, &IID_IClassFactory, (void **)&u) == E_NOINTERFACE && !u,
              "CreateInstance with unsupported IID");
    }

    /* "Panel" side of a private section. The APO creates it (first opener); we open it after. */
    swprintf_s(secname, 64, L"Local\\E1Test_%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"E1FX_SECTION_OVERRIDE", secname);
    {
        Apo first;
        if (apo_new(cf_r, &first)) {
            CHECK_HR(apo_init2(&first, &CLSID_E1FxRenderMfx, NULL, NULL, FALSE), S_OK);
            err = e1ipc_open(&panel, 0);
            CHECK(err == 0 && panel.block && panel.created == 0, "APO created the section (open err=%lu)", err);
            if (err == 0) {
                CHECK_HR(IClassFactory_LockServer(cf_r, TRUE), S_OK);
                apo_free(&first);
                test_render(cf_r, panel.block);
                test_render_v3_gates(cf_r, panel.block);
                test_capture(cf_c, panel.block);
                CHECK_HR(IClassFactory_LockServer(cf_r, FALSE), S_OK);
                e1ipc_close(&panel);
            } else {
                apo_free(&first);
            }
        }
    }
    test_ipc_fallback(cf_r);
    test_registration(reg, unreg, dllpath);

    section("dll: unload");
    CHECK_HR(can_unload(), S_FALSE);
    CHECK(IClassFactory_Release(cf_r) >= 1 && IClassFactory_Release(cf_c) >= 1, "factory release");
    CHECK_HR(can_unload(), S_OK);
    CHECK(g_host_objects == 0, "host media types leaked: %ld", g_host_objects);
    FreeLibrary(dll);

out:
    RemoveDirectoryW(g_tmpdir);
    CoUninitialize();
    printf("\nRESULT: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
