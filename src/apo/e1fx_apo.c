/*
 * e1fx_apo.c - E1 Studio MFX APO (render and capture) in plain C COM.
 *
 * One object implements, through separate vtables on one allocation:
 *   IAudioProcessingObject (identity / IUnknown), IAudioProcessingObjectRT,
 *   IAudioProcessingObjectConfiguration, IAudioSystemEffects/2/3 (one vtable: 3 derives from 2
 *   derives from 1) and IAudioProcessingObjectNotifications.
 *
 * Threading:
 *   - APOProcess runs on the engine's real-time thread. It takes no locks, allocates nothing and
 *     makes no system calls: it reads parameters with the wait-free seqlock reader, runs the DSP
 *     chain and writes meters/heartbeat into the shared block.
 *   - Every other method (and the 250 ms housekeeping timer) serialises on an SRW lock that
 *     APOProcess never touches. The engine guarantees LockForProcess/UnlockForProcess/Reset are
 *     not concurrent with APOProcess, so the chain pointer is stable while processing.
 *   - The shared-block pointer is published once (InterlockedExchangePointer) and unmapped only
 *     in the final Release.
 *
 * Gates that force a bit-exact bypass, independent of the panel's parameters:
 *   sysfx_disabled  Windows "Audio enhancements: Off" (PKEY_AudioEndpoint_Disable_SysFx)
 *   user_off        IAudioSystemEffects3::SetAudioSystemEffectState(OFF)
 *   raw_mode        the engine instantiated us for the RAW processing mode
 */
#include "e1fx_apo_internal.h"
#include <string.h>
#if defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

#include "../../include/e1fx_ipc.h"
#include "../dsp/e1dsp.h"
#include <stdarg.h>
#include <stdio.h>

/* Control-path diagnostics -> Windows Application log, source "E1Studio" (event id 1000).
   Never called from APOProcess. Budget-limited so a misbehaving engine cannot flood the log. */
static volatile LONG g_e1_log_budget = 400;
static void apo_log(const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    const wchar_t *strs[1];
    HANDLE h;
    va_list ap;
    if (InterlockedDecrement(&g_e1_log_budget) < 0) return;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, sizeof(buf) / sizeof(buf[0]), _TRUNCATE, fmt, ap);
    va_end(ap);
    h = RegisterEventSourceW(NULL, L"E1Studio");
    if (h == NULL) return;
    strs[0] = buf;
    ReportEventW(h, EVENTLOG_INFORMATION_TYPE, 0, 1000, NULL, 1, 0, strs, NULL);
    DeregisterEventSource(h);
}

#define E1APO_TIMER_MS            250
#define E1APO_IPC_RETRY_TICKS     8      /* retry opening the section every 2 s */
#define E1APO_NUM_INTERFACES      6

typedef struct E1Apo {
    /* interface pointers: &a->lpVtblXxx is the interface pointer handed out */
    const IAudioProcessingObjectVtbl              *lpVtblApo;    /* identity */
    const IAudioProcessingObjectRTVtbl            *lpVtblRt;
    const IAudioProcessingObjectConfigurationVtbl *lpVtblCfg;
    const IAudioSystemEffects3Vtbl                *lpVtblFx;
    const IAudioProcessingObjectNotificationsVtbl *lpVtblNtf;
    const IUnknownVtbl                            *lpVtblInner;  /* non-delegating IUnknown */
    IUnknown     *outer;             /* controlling unknown: the aggregator (audiodg) or our inner one */

    volatile LONG refs;
    int           kind;
    const CLSID  *clsid;
    const GUID   *effect_id;

    SRWLOCK lock;                    /* control path only */
    int     initialized;
    int     discovery_only;
    unsigned long last_ipc_err;      /* diagnostics: last e1ipc_open error logged */
    IMMDevice *device;               /* endpoint from APOInitSystemEffects3 (for notifications) */

    /* stream state (valid while locked) */
    volatile LONG locked;
    UINT32     channels;
    UINT32     rate;
    UINT32     max_frames;
    E1Render  *render;
    E1Capture *capture;

    /* parameters */
    E1Params file_params;            /* settings file at Initialize: used when IPC is unavailable */
    E1Params rt_params;              /* RT thread: last snapshot */
    int32_t  rt_last_seq;
    LONG     rt_applied_off;
    E1Params ctl_params;             /* control path: last snapshot (effect list state) */
    int32_t  ctl_last_seq;

    /* IPC */
    E1IpcMapping ipc;                /* touched under `lock` only */
    E1SharedBlock *volatile shared;  /* published pointer read by the RT thread */

    /* gates */
    volatile LONG sysfx_disabled;
    volatile LONG user_off;
    volatile LONG raw_mode;

    /* change notification */
    HANDLE   fx_event;               /* duplicated from GetEffectsList */
    HANDLE   ctl_event;              /* duplicated from GetControllableSystemEffectsList */
    LONG     reported_active;
    LONG     reported_user_off;
    PTP_TIMER timer;
    UINT     timer_ticks;
} E1Apo;

#define FROM_APO(p) ((E1Apo *)((BYTE *)(p) - offsetof(E1Apo, lpVtblApo)))
#define FROM_RT(p)  ((E1Apo *)((BYTE *)(p) - offsetof(E1Apo, lpVtblRt)))
#define FROM_CFG(p) ((E1Apo *)((BYTE *)(p) - offsetof(E1Apo, lpVtblCfg)))
#define FROM_FX(p)  ((E1Apo *)((BYTE *)(p) - offsetof(E1Apo, lpVtblFx)))
#define FROM_NTF(p) ((E1Apo *)((BYTE *)(p) - offsetof(E1Apo, lpVtblNtf)))
#define FROM_INNER(p) ((E1Apo *)((BYTE *)(p) - offsetof(E1Apo, lpVtblInner)))

/* ======================================================================================== */
/* helpers                                                                                   */

const CLSID *e1apo_clsid(int kind)
{
    return kind == E1APO_KIND_RENDER ? &CLSID_E1FxRenderMfx : &CLSID_E1FxCaptureMfx;
}

static LONG apo_gate_off(const E1Apo *a)
{
    return (a->sysfx_disabled | a->user_off | a->raw_mode) ? 1 : 0;
}

/* Does this parameter set make the chain do anything (before the Windows gates)? */
static int apo_params_active(const E1Apo *a, const E1Params *p)
{
    /* Scout Mode drives the render chain on its own (Scout EQ + surround), see e1dsp_chain.c. */
    if (a->kind == E1APO_KIND_RENDER)
        return p->render.master_on != 0 || p->render.eq_on != 0 || p->render.scout_on != 0;
    return p->capture.master_on != 0;
}

/* Pushes params (with the gates applied) into the chain. RT-safe: plain copies only. */
static void apo_apply_params(E1Apo *a, const E1Params *p, LONG off)
{
    if (a->kind == E1APO_KIND_RENDER) {
        E1RenderParams r = p->render;
        if (off) { r.master_on = 0; r.eq_on = 0; r.scout_on = 0; }
        if (a->render) e1_render_set_params(a->render, &r);
    } else {
        E1CaptureParams c = p->capture;
        if (off) c.master_on = 0;
        if (a->capture) e1_capture_set_params(a->capture, &c);
    }
    a->rt_applied_off = off;
}

static E1ApoStatus *apo_status(const E1Apo *a, E1SharedBlock *b)
{
    return a->kind == E1APO_KIND_RENDER ? &b->render_status : &b->capture_status;
}

static int apo_chain_bypassed(const E1Apo *a)
{
    if (a->kind == E1APO_KIND_RENDER) return a->render ? e1_render_is_bypassed(a->render) : 1;
    return a->capture ? e1_capture_is_bypassed(a->capture) : 1;
}

/* float32, 1..8 channels, 8 k..384 k integer rate */
static BOOL apo_format_ok(const UNCOMPRESSEDAUDIOFORMAT *u)
{
    return IsEqualGUID(&u->guidFormatType, &E1_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
           u->dwBytesPerSampleContainer == 4 && u->dwValidBitsPerSample == 32 &&
           u->dwSamplesPerFrame >= 1 && u->dwSamplesPerFrame <= E1APO_MAX_CHANNELS &&
           u->fFramesPerSecond >= (FLOAT)E1APO_MIN_RATE && u->fFramesPerSecond <= (FLOAT)E1APO_MAX_RATE &&
           u->fFramesPerSecond == (FLOAT)(UINT32)u->fFramesPerSecond;
}

static BOOL apo_get_uncompressed(IAudioMediaType *t, UNCOMPRESSEDAUDIOFORMAT *u)
{
    BOOL comp = TRUE;
    if (t == NULL) return FALSE;
    if (FAILED(IAudioMediaType_IsCompressedFormat(t, &comp)) || comp) return FALSE;
    return SUCCEEDED(IAudioMediaType_GetUncompressedAudioFormat(t, u));
}

static UINT32 apo_popcount(DWORD v)
{
    UINT32 n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}

/* Shared rule for IsInputFormatSupported / IsOutputFormatSupported. Because all *_MUST_MATCH
   flags are set, the accepted format must have the opposite side's rate and channel count. */
static HRESULT apo_check_format(IAudioMediaType *opposite, IAudioMediaType *requested, IAudioMediaType **supported)
{
    UNCOMPRESSEDAUDIOFORMAT ur, uo;
    BOOL have_req, have_opp;
    UINT32 ch, rate;
    DWORD mask = 0;
    HRESULT hr;

    if (supported == NULL) return E_POINTER;
    *supported = NULL;
    if (requested == NULL) return E_POINTER;

    have_req = apo_get_uncompressed(requested, &ur);
    have_opp = apo_get_uncompressed(opposite, &uo);

    if (have_req && apo_format_ok(&ur) &&
        (!have_opp || (uo.dwSamplesPerFrame == ur.dwSamplesPerFrame && uo.fFramesPerSecond == ur.fFramesPerSecond))) {
        IAudioMediaType_AddRef(requested);
        *supported = requested;
        return S_OK;
    }

    if (have_opp) {
        ch = uo.dwSamplesPerFrame;
        rate = (UINT32)uo.fFramesPerSecond;
        if (ch < 1 || ch > E1APO_MAX_CHANNELS || rate < E1APO_MIN_RATE || rate > E1APO_MAX_RATE ||
            uo.fFramesPerSecond != (FLOAT)rate)
            return APOERR_FORMAT_NOT_SUPPORTED;
        mask = uo.dwChannelMask;
    } else if (have_req) {
        ch = ur.dwSamplesPerFrame;
        rate = (UINT32)(ur.fFramesPerSecond + 0.5f);
        mask = ur.dwChannelMask;
        if (ch < 1) ch = 1;
        if (ch > E1APO_MAX_CHANNELS) { ch = E1APO_MAX_CHANNELS; mask = 0; }
        if (rate < E1APO_MIN_RATE) rate = E1APO_MIN_RATE;
        if (rate > E1APO_MAX_RATE) rate = E1APO_MAX_RATE;
    } else {
        return APOERR_FORMAT_NOT_SUPPORTED;      /* compressed request and nothing to match */
    }
    if (apo_popcount(mask) != ch) mask = 0;

    hr = e1mt_create_float(ch, rate, mask, supported);
    return FAILED(hr) ? hr : S_FALSE;
}

/* Reads PKEY_AudioEndpoint_Disable_SysFx; returns 1 when Windows disabled enhancements. */
static LONG apo_read_sysfx_disabled(IPropertyStore *ps)
{
    PROPVARIANT v;
    LONG off = 0;
    if (ps == NULL) return 0;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &E1_PKEY_AudioEndpoint_Disable_SysFx, &v))) {
        if (v.vt == VT_UI4) off = (v.ulVal == ENDPOINT_SYSFX_DISABLED);
        else if (v.vt == VT_I4) off = (v.lVal == ENDPOINT_SYSFX_DISABLED);
        else if (v.vt == VT_BOOL) off = (v.boolVal != VARIANT_FALSE);
    }
    PropVariantClear(&v);
    return off;
}

/* Control path (lock held): latest parameters known to the control side. */
static const E1Params *apo_ctl_params_locked(E1Apo *a)
{
    E1SharedBlock *b = a->shared;
    if (b) {
        E1Params tmp;
        if (e1ipc_read_params(b, &a->ctl_last_seq, &tmp)) a->ctl_params = tmp;
    }
    return &a->ctl_params;
}

static LONG apo_effective_active_locked(E1Apo *a)
{
    if (apo_gate_off(a)) return 0;
    return apo_params_active(a, apo_ctl_params_locked(a)) ? 1 : 0;
}

/* Signals the duplicated change events when the reported state moved (lock held). */
static void apo_notify_changes_locked(E1Apo *a)
{
    LONG active = apo_effective_active_locked(a);
    LONG uoff = a->user_off;
    if (active != a->reported_active || uoff != a->reported_user_off) {
        a->reported_active = active;
        a->reported_user_off = uoff;
        if (a->fx_event) SetEvent(a->fx_event);
        if (a->ctl_event) SetEvent(a->ctl_event);
    }
}

/* Discovery-only instances (effect enumeration) open an existing section but never create it. */
static void apo_try_open_ipc_locked(E1Apo *a)
{
    unsigned long err;
    if (a->shared != NULL) return;
    err = e1ipc_open(&a->ipc, a->discovery_only ? 0 : 1);
    if (err == ERROR_SUCCESS) {
        a->ctl_last_seq = -1;
        InterlockedExchangePointer((PVOID volatile *)&a->shared, a->ipc.block);
        apo_log(L"apo %p kind=%d: IPC section open (created=%d)", (void *)a, a->kind, a->ipc.created);
    } else if (err != a->last_ipc_err) {
        a->last_ipc_err = err;
        apo_log(L"apo %p kind=%d discovery=%d: IPC section open failed, Win32 error %lu", (void *)a, a->kind,
                a->discovery_only, err);
    }
}

static VOID CALLBACK apo_timer_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_TIMER timer)
{
    E1Apo *a = (E1Apo *)ctx;
    (void)inst; (void)timer;
    AcquireSRWLockExclusive(&a->lock);
    if (a->shared == NULL && (a->timer_ticks++ % E1APO_IPC_RETRY_TICKS) == 0)
        apo_try_open_ipc_locked(a);
    apo_notify_changes_locked(a);
    ReleaseSRWLockExclusive(&a->lock);
}

static HANDLE apo_dup_event(HANDLE h)
{
    HANDLE dup = NULL;
    if (h == NULL || h == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return NULL;
    return dup;
}

static void apo_destroy_chain(E1Apo *a)
{
    if (a->render) { e1_render_destroy(a->render); a->render = NULL; }
    if (a->capture) { e1_capture_destroy(a->capture); a->capture = NULL; }
}

static void apo_status_idle(E1Apo *a)
{
    E1SharedBlock *b = a->shared;
    if (b) {
        E1ApoStatus *st = apo_status(a, b);
        st->bypass = 1;
        st->peak_in[0] = st->peak_in[1] = 0.f;
        st->peak_out[0] = st->peak_out[1] = 0.f;
        st->gain_reduction_db = 0.f;
    }
}

static void apo_destroy(E1Apo *a)
{
    if (a->timer) {
        SetThreadpoolTimer(a->timer, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(a->timer, TRUE);
        CloseThreadpoolTimer(a->timer);
        a->timer = NULL;
    }
    if (a->locked) apo_status_idle(a);
    apo_destroy_chain(a);
    a->shared = NULL;
    e1ipc_close(&a->ipc);
    if (a->device) { IMMDevice_Release(a->device); a->device = NULL; }
    if (a->fx_event) CloseHandle(a->fx_event);
    if (a->ctl_event) CloseHandle(a->ctl_event);
    HeapFree(GetProcessHeap(), 0, a);
    e1dll_object_destroyed();
}

/* ======================================================================================== */
/* IUnknown (shared by all interfaces)                                                       */

/* COM aggregation: the audio engine creates APOs aggregated (pUnkOuter = its own object) and asks
   for IID_IUnknown, getting our NON-delegating unknown (lpVtblInner). Every other interface's
   IUnknown methods delegate to the controlling unknown `outer` (= lpVtblInner when not aggregated). */
static HRESULT apo_qi(E1Apo *a, REFIID riid, void **ppv)
{
    if (ppv == NULL) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown))
        *ppv = &a->lpVtblInner;
    else if (IsEqualIID(riid, &E1_IID_IAudioProcessingObject))
        *ppv = &a->lpVtblApo;
    else if (IsEqualIID(riid, &E1_IID_IAudioProcessingObjectRT))
        *ppv = &a->lpVtblRt;
    else if (IsEqualIID(riid, &E1_IID_IAudioProcessingObjectConfiguration))
        *ppv = &a->lpVtblCfg;
    else if (IsEqualIID(riid, &E1_IID_IAudioSystemEffects) || IsEqualIID(riid, &E1_IID_IAudioSystemEffects2) ||
             IsEqualIID(riid, &E1_IID_IAudioSystemEffects3))
        *ppv = &a->lpVtblFx;
    else if (IsEqualIID(riid, &E1_IID_IAudioProcessingObjectNotifications))
        *ppv = &a->lpVtblNtf;
    else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    if (*ppv == (void *)&a->lpVtblInner) InterlockedIncrement(&a->refs);
    else a->outer->lpVtbl->AddRef(a->outer);   /* COM rule: AddRef through the interface returned */
    return S_OK;
}

static ULONG apo_addref(E1Apo *a) { return (ULONG)InterlockedIncrement(&a->refs); }

static ULONG apo_release(E1Apo *a)
{
    LONG r = InterlockedDecrement(&a->refs);
    if (r == 0) apo_destroy(a);
    return (ULONG)r;
}

/* Delegating IUnknown of every exposed interface -> controlling unknown. */
#define E1_IUNKNOWN_THUNKS(prefix, Iface, FROM)                                                   \
    static HRESULT STDMETHODCALLTYPE prefix##_QueryInterface(Iface *This, REFIID riid, void **ppv) \
    { IUnknown *o = FROM(This)->outer; return o->lpVtbl->QueryInterface(o, riid, ppv); }          \
    static ULONG STDMETHODCALLTYPE prefix##_AddRef(Iface *This)                                     \
    { IUnknown *o = FROM(This)->outer; return o->lpVtbl->AddRef(o); }                              \
    static ULONG STDMETHODCALLTYPE prefix##_Release(Iface *This)                                    \
    { IUnknown *o = FROM(This)->outer; return o->lpVtbl->Release(o); }

/* Non-delegating IUnknown (object identity when aggregated; owns the reference count). */
static HRESULT STDMETHODCALLTYPE inner_QueryInterface(IUnknown *This, REFIID riid, void **ppv)
{ return apo_qi(FROM_INNER(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE inner_AddRef(IUnknown *This) { return apo_addref(FROM_INNER(This)); }
static ULONG STDMETHODCALLTYPE inner_Release(IUnknown *This) { return apo_release(FROM_INNER(This)); }
static const IUnknownVtbl g_inner_vtbl = { inner_QueryInterface, inner_AddRef, inner_Release };

E1_IUNKNOWN_THUNKS(apo, IAudioProcessingObject, FROM_APO)
E1_IUNKNOWN_THUNKS(rt, IAudioProcessingObjectRT, FROM_RT)
E1_IUNKNOWN_THUNKS(cfg, IAudioProcessingObjectConfiguration, FROM_CFG)
E1_IUNKNOWN_THUNKS(fx, IAudioSystemEffects3, FROM_FX)
E1_IUNKNOWN_THUNKS(ntf, IAudioProcessingObjectNotifications, FROM_NTF)

/* ======================================================================================== */
/* IAudioProcessingObject                                                                    */

static HRESULT STDMETHODCALLTYPE apo_Reset(IAudioProcessingObject *This)
{
    E1Apo *a = FROM_APO(This);
    AcquireSRWLockExclusive(&a->lock);
    if (a->render) e1_render_reset(a->render);
    if (a->capture) e1_capture_reset(a->capture);
    ReleaseSRWLockExclusive(&a->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE apo_GetLatency(IAudioProcessingObject *This, HNSTIME *pTime)
{
    E1Apo *a = FROM_APO(This);
    UINT64 frames = 0;
    if (pTime == NULL) return E_POINTER;
    AcquireSRWLockShared(&a->lock);
    if (a->locked && a->rate != 0 && !apo_chain_bypassed(a)) {
        frames = a->kind == E1APO_KIND_RENDER ? e1_render_latency_frames(a->render)
                                              : e1_capture_latency_frames(a->capture);
        *pTime = (HNSTIME)((frames * 10000000ull + a->rate / 2) / a->rate);
    } else {
        *pTime = 0;
    }
    ReleaseSRWLockShared(&a->lock);
    return S_OK;
}

HRESULT e1apo_get_reg_properties(int kind, APO_REG_PROPERTIES **out)
{
    static const IID *const ifaces[E1APO_NUM_INTERFACES] = {
        &E1_IID_IAudioProcessingObject, &E1_IID_IAudioProcessingObjectRT,
        &E1_IID_IAudioProcessingObjectConfiguration, &E1_IID_IAudioSystemEffects,
        &E1_IID_IAudioSystemEffects2, &E1_IID_IAudioSystemEffects3
    };
    APO_REG_PROPERTIES *p;
    UINT32 i;
    SIZE_T size = sizeof(APO_REG_PROPERTIES) + (E1APO_NUM_INTERFACES - 1) * sizeof(IID);

    if (out == NULL) return E_POINTER;
    *out = NULL;
    p = (APO_REG_PROPERTIES *)CoTaskMemAlloc(size);
    if (p == NULL) return E_OUTOFMEMORY;
    memset(p, 0, size);
    p->clsid = *e1apo_clsid(kind);
    p->Flags = (APO_FLAG)(APO_FLAG_INPLACE | APO_FLAG_SAMPLESPERFRAME_MUST_MATCH |
                          APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH);
    wcscpy_s(p->szFriendlyName, 256, E1APO_FRIENDLY_NAME);
    wcscpy_s(p->szCopyrightInfo, 256, E1APO_COPYRIGHT);
    p->u32MajorVersion = E1APO_MAJOR_VERSION;
    p->u32MinorVersion = E1APO_MINOR_VERSION;
    p->u32MinInputConnections = 1;
    p->u32MaxInputConnections = 1;
    p->u32MinOutputConnections = 1;
    p->u32MaxOutputConnections = 1;
    p->u32MaxInstances = 0xFFFFFFFFu;
    p->u32NumAPOInterfaces = E1APO_NUM_INTERFACES;
    for (i = 0; i < E1APO_NUM_INTERFACES; ++i) p->iidAPOInterfaceList[i] = *ifaces[i];
    *out = p;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE apo_GetRegistrationProperties(IAudioProcessingObject *This,
                                                              APO_REG_PROPERTIES **ppRegProps)
{
    return e1apo_get_reg_properties(FROM_APO(This)->kind, ppRegProps);
}

static HRESULT STDMETHODCALLTYPE apo_Initialize(IAudioProcessingObject *This, UINT32 cbDataSize, BYTE *pbyData)
{
    E1Apo *a = FROM_APO(This);
    IPropertyStore *endpoint_props = NULL;
    IMMDeviceCollection *devices = NULL;
    GUID mode = E1_AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    BOOL discovery = FALSE;
    HRESULT hr = S_OK;

    apo_log(L"apo %p kind=%d: Initialize cbDataSize=%u", (void *)a, a->kind, cbDataSize);
    if (cbDataSize != 0 && pbyData == NULL) return E_POINTER;

    if (cbDataSize != 0) {
        const APOInitBaseStruct *base = (const APOInitBaseStruct *)pbyData;
        if (cbDataSize < sizeof(APOInitBaseStruct) || base->cbSize != cbDataSize) {
            apo_log(L"apo %p: Initialize rejected, cbSize=%u cbDataSize=%u", (void *)a,
                    cbDataSize >= sizeof(UINT32) ? base->cbSize : 0, cbDataSize);
            return E_INVALIDARG;
        }
        if (!IsEqualCLSID(&base->clsid, a->clsid)) {
            apo_log(L"apo %p: Initialize rejected, CLSID mismatch", (void *)a);
            return APOERR_INVALID_APO_CLSID;
        }

        /* The three layouts have distinct sizes on every architecture (x64: 56 / 88 / 80). */
        if (cbDataSize == sizeof(APOInitSystemEffects3)) {
            const APOInitSystemEffects3 *i3 = (const APOInitSystemEffects3 *)pbyData;
            endpoint_props = i3->pAPOEndpointProperties;
            devices = i3->pDeviceCollection;
            mode = i3->AudioProcessingMode;
            discovery = i3->InitializeForDiscoveryOnly;
        } else if (cbDataSize == sizeof(APOInitSystemEffects2)) {
            const APOInitSystemEffects2 *i2 = (const APOInitSystemEffects2 *)pbyData;
            endpoint_props = i2->pAPOEndpointProperties;
            mode = i2->AudioProcessingMode;
            discovery = i2->InitializeForDiscoveryOnly;
        } else if (cbDataSize == sizeof(APOInitSystemEffects)) {
            endpoint_props = ((const APOInitSystemEffects *)pbyData)->pAPOEndpointProperties;
        } else if (cbDataSize != sizeof(APOInitBaseStruct)) {
            apo_log(L"apo %p: Initialize rejected, unknown init struct size %u (v1=%u v2=%u v3=%u)", (void *)a,
                    cbDataSize, (unsigned)sizeof(APOInitSystemEffects), (unsigned)sizeof(APOInitSystemEffects2),
                    (unsigned)sizeof(APOInitSystemEffects3));
            return E_INVALIDARG;
        }
    }

    AcquireSRWLockExclusive(&a->lock);
    if (a->initialized) {
        hr = APOERR_ALREADY_INITIALIZED;
        goto done;
    }

    a->discovery_only = discovery ? 1 : 0;
    a->raw_mode = IsEqualGUID(&mode, &E1_AUDIO_SIGNALPROCESSINGMODE_RAW) ? 1 : 0;
    a->sysfx_disabled = apo_read_sysfx_disabled(endpoint_props);

    if (devices != NULL) {
        /* As in Microsoft's samples, the endpoint is the last device of the collection. */
        UINT n = 0;
        if (SUCCEEDED(IMMDeviceCollection_GetCount(devices, &n)) && n > 0) {
            IMMDevice *dev = NULL;
            if (SUCCEEDED(IMMDeviceCollection_Item(devices, n - 1, &dev))) a->device = dev;
        }
    }

    e1ipc_load_settings(&a->file_params);          /* defaults (= bypass) on any failure */
    a->ctl_params = a->file_params;
    a->ctl_last_seq = -1;
    a->rt_last_seq = -1;

    apo_try_open_ipc_locked(a);
    if (!a->discovery_only) {
        /* Housekeeping only (IPC retry + effect-list change events); failure is not fatal. */
        a->timer = CreateThreadpoolTimer(apo_timer_cb, a, NULL);
        if (a->timer) {
            FILETIME due;
            ULARGE_INTEGER u;
            u.QuadPart = (ULONGLONG)(-(LONGLONG)E1APO_TIMER_MS * 10000LL);
            due.dwLowDateTime = u.LowPart;
            due.dwHighDateTime = u.HighPart;
            SetThreadpoolTimer(a->timer, &due, E1APO_TIMER_MS, E1APO_TIMER_MS / 5);
        }
    }
    a->reported_active = apo_effective_active_locked(a);
    a->reported_user_off = a->user_off;
    a->initialized = 1;
done:
    apo_log(L"apo %p kind=%d: Initialize hr=0x%08lX discovery=%d raw=%d sysfx_off=%d ipc=%d", (void *)a, a->kind,
            (unsigned long)hr, a->discovery_only, a->raw_mode, a->sysfx_disabled, a->shared != NULL);
    ReleaseSRWLockExclusive(&a->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE apo_IsInputFormatSupported(IAudioProcessingObject *This, IAudioMediaType *pOpposite,
                                                           IAudioMediaType *pRequested, IAudioMediaType **ppSupported)
{
    HRESULT hr = apo_check_format(pOpposite, pRequested, ppSupported);
    if (hr != S_OK) apo_log(L"apo %p: IsInputFormatSupported hr=0x%08lX", (void *)This, (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE apo_IsOutputFormatSupported(IAudioProcessingObject *This, IAudioMediaType *pOpposite,
                                                            IAudioMediaType *pRequested, IAudioMediaType **ppSupported)
{
    HRESULT hr = apo_check_format(pOpposite, pRequested, ppSupported);
    if (hr != S_OK) apo_log(L"apo %p: IsOutputFormatSupported hr=0x%08lX", (void *)This, (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE apo_GetInputChannelCount(IAudioProcessingObject *This, UINT32 *pu32ChannelCount)
{
    E1Apo *a = FROM_APO(This);
    if (pu32ChannelCount == NULL) return E_POINTER;
    AcquireSRWLockShared(&a->lock);
    *pu32ChannelCount = a->locked ? a->channels : 0;
    ReleaseSRWLockShared(&a->lock);
    return S_OK;
}

static const IAudioProcessingObjectVtbl g_apo_vtbl = {
    apo_QueryInterface, apo_AddRef, apo_Release,
    apo_Reset, apo_GetLatency, apo_GetRegistrationProperties, apo_Initialize,
    apo_IsInputFormatSupported, apo_IsOutputFormatSupported, apo_GetInputChannelCount
};

/* ======================================================================================== */
/* IAudioProcessingObjectConfiguration                                                       */

static HRESULT STDMETHODCALLTYPE cfg_LockForProcess(IAudioProcessingObjectConfiguration *This,
                                                    UINT32 nIn, APO_CONNECTION_DESCRIPTOR **ppIn,
                                                    UINT32 nOut, APO_CONNECTION_DESCRIPTOR **ppOut)
{
    E1Apo *a = FROM_CFG(This);
    UNCOMPRESSEDAUDIOFORMAT ui, uo;
    E1Params initial;
    HRESULT hr = S_OK;

    AcquireSRWLockExclusive(&a->lock);
    if (!a->initialized || a->discovery_only) { hr = APOERR_NOT_INITIALIZED; goto done; }
    if (a->locked) { hr = APOERR_APO_LOCKED; goto done; }
    if (nIn != 1 || nOut != 1 || ppIn == NULL || ppOut == NULL || ppIn[0] == NULL || ppOut[0] == NULL) {
        hr = APOERR_NUM_CONNECTIONS_INVALID;
        goto done;
    }
    if (!apo_get_uncompressed(ppIn[0]->pFormat, &ui) || !apo_get_uncompressed(ppOut[0]->pFormat, &uo) ||
        !apo_format_ok(&ui) || !apo_format_ok(&uo) ||
        ui.dwSamplesPerFrame != uo.dwSamplesPerFrame || ui.fFramesPerSecond != uo.fFramesPerSecond) {
        hr = APOERR_INVALID_CONNECTION_FORMAT;
        goto done;
    }
    if (ppOut[0]->u32MaxFrameCount < ppIn[0]->u32MaxFrameCount) {
        hr = APOERR_INVALID_OUTPUT_MAXFRAMECOUNT;
        goto done;
    }

    a->channels = ui.dwSamplesPerFrame;
    a->rate = (UINT32)ui.fFramesPerSecond;
    a->max_frames = ppIn[0]->u32MaxFrameCount;

    if (a->kind == E1APO_KIND_RENDER) a->render = e1_render_create((double)a->rate, (int)a->channels);
    else a->capture = e1_capture_create((double)a->rate, (int)a->channels);
    if (a->render == NULL && a->capture == NULL) { hr = E_OUTOFMEMORY; goto done; }

    /* Initial parameters: newest shared snapshot, else the settings file read at Initialize. */
    initial = a->file_params;
    a->rt_last_seq = -1;
    if (a->shared) {
        E1Params tmp;
        int32_t seq = -1;
        if (e1ipc_read_params(a->shared, &seq, &tmp)) {
            initial = tmp;
            a->rt_last_seq = seq;
        }
    }
    a->rt_params = initial;
    apo_apply_params(a, &a->rt_params, apo_gate_off(a));
    /* Nothing has been output yet: start directly in the target state (no fade-in), so that
       GetLatency, which the engine reads once while building the graph, is already the latency
       of the path that will actually run. */
    if (a->render) e1_render_reset(a->render);
    if (a->capture) e1_capture_reset(a->capture);

    InterlockedExchange(&a->locked, 1);
done:
    if (nIn >= 1 && ppIn != NULL && ppIn[0] != NULL && apo_get_uncompressed(ppIn[0]->pFormat, &ui))
        apo_log(L"apo %p kind=%d: LockForProcess hr=0x%08lX in: %u ch %.0f Hz %u-bit max=%u", (void *)a, a->kind,
                (unsigned long)hr, ui.dwSamplesPerFrame, ui.fFramesPerSecond, ui.dwBytesPerSampleContainer * 8,
                ppIn[0]->u32MaxFrameCount);
    else
        apo_log(L"apo %p kind=%d: LockForProcess hr=0x%08lX (format unreadable)", (void *)a, a->kind,
                (unsigned long)hr);
    ReleaseSRWLockExclusive(&a->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cfg_UnlockForProcess(IAudioProcessingObjectConfiguration *This)
{
    E1Apo *a = FROM_CFG(This);
    HRESULT hr = S_OK;
    AcquireSRWLockExclusive(&a->lock);
    if (!a->locked) {
        hr = APOERR_ALREADY_UNLOCKED;
    } else {
        InterlockedExchange(&a->locked, 0);
        apo_status_idle(a);
        apo_destroy_chain(a);
        a->channels = a->rate = a->max_frames = 0;
    }
    ReleaseSRWLockExclusive(&a->lock);
    return hr;
}

static const IAudioProcessingObjectConfigurationVtbl g_cfg_vtbl = {
    cfg_QueryInterface, cfg_AddRef, cfg_Release, cfg_LockForProcess, cfg_UnlockForProcess
};

/* ======================================================================================== */
/* IAudioProcessingObjectRT                                                                  */

static void STDMETHODCALLTYPE rt_APOProcess(IAudioProcessingObjectRT *This,
                                            UINT32 nIn, APO_CONNECTION_PROPERTY **ppIn,
                                            UINT32 nOut, APO_CONNECTION_PROPERTY **ppOut)
{
    E1Apo *a = FROM_RT(This);
    APO_CONNECTION_PROPERTY *in, *out;
    E1SharedBlock *b;
    E1ApoStatus *st = NULL;
    UINT32 frames;
    float *src, *dst;
    LONG off;
    int bypassed;
#if defined(_M_X64) || defined(_M_IX86)
    unsigned int saved_csr;
#endif

    if (nIn != 1 || nOut != 1 || ppIn == NULL || ppOut == NULL || ppIn[0] == NULL || ppOut[0] == NULL) return;
    in = ppIn[0];
    out = ppOut[0];
    frames = in->u32ValidFrameCount;
    src = (float *)in->pBuffer;
    dst = (float *)out->pBuffer;
    b = a->shared;
    if (b) st = apo_status(a, b);

    /* Not ready: the format (channel count) is unknown, so nothing may be written. */
    if (!a->locked || a->channels == 0 || (a->render == NULL && a->capture == NULL)) {
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_SILENT;
        if (st) InterlockedIncrement((volatile LONG *)&st->xruns);
        return;
    }
    /* Out of contract (more frames than LockForProcess allowed): the output connection is only
       guaranteed to hold max_frames, so pass through at most that many, untouched. */
    if (frames > a->max_frames) {
        const UINT32 n = a->max_frames;
        if (in->u32BufferFlags != BUFFER_SILENT && src != dst && n)
            memmove(dst, src, (size_t)n * a->channels * sizeof(float));
        out->u32ValidFrameCount = n;
        out->u32BufferFlags = in->u32BufferFlags;
        if (st) InterlockedIncrement((volatile LONG *)&st->xruns);
        return;
    }

    /* Parameters: at most one wait-free snapshot per buffer, plus the Windows gates. */
    {
        int changed = 0;
        if (b && e1ipc_read_params(b, &a->rt_last_seq, &a->rt_params)) changed = 1;
        off = apo_gate_off(a);
        if (off != a->rt_applied_off) changed = 1;
        if (changed) apo_apply_params(a, &a->rt_params, off);
    }

    if (in->u32BufferFlags == BUFFER_SILENT && apo_chain_bypassed(a)) {
        /* Bit-exact bypass of silence is silence: no tails to flush. Keep the data defined for
           consumers that ignore the flag. */
        if (frames) memset(dst, 0, (size_t)frames * a->channels * sizeof(float));
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_SILENT;
        if (st) {
            st->peak_in[0] = st->peak_in[1] = 0.f;
            st->peak_out[0] = st->peak_out[1] = 0.f;
            st->gain_reduction_db = 0.f;
        }
    } else {
        if (in->u32BufferFlags == BUFFER_SILENT) {
            /* Contents of a silent buffer are undefined: run the chain in place on zeros in the
               output buffer (never write into the upstream connection) to flush its tails. */
            if (frames) memset(dst, 0, (size_t)frames * a->channels * sizeof(float));
            src = dst;
        }
#if defined(_M_X64) || defined(_M_IX86)
        saved_csr = _mm_getcsr();
        _mm_setcsr(saved_csr | 0x8040u);              /* FTZ | DAZ for the duration of the chain */
#endif
        if (a->kind == E1APO_KIND_RENDER) e1_render_process(a->render, src, dst, frames);
        else e1_capture_process(a->capture, src, dst, frames);
#if defined(_M_X64) || defined(_M_IX86)
        _mm_setcsr(saved_csr);
#endif
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_VALID;
        if (st) {
            float pin[2], pout[2], gr = 0.f;
            if (a->kind == E1APO_KIND_RENDER) e1_render_meter(a->render, pin, pout, &gr);
            else e1_capture_meter(a->capture, pin, pout, &gr);
            st->peak_in[0] = pin[0];
            st->peak_in[1] = pin[1];
            st->peak_out[0] = pout[0];
            st->peak_out[1] = pout[1];
            st->gain_reduction_db = gr;
        }
    }

    if (st) {
        bypassed = apo_chain_bypassed(a);
        st->sample_rate = a->rate;
        st->channels = a->channels;
        st->bypass = (uint32_t)bypassed;
        InterlockedIncrement((volatile LONG *)&st->alive);
    }
}

static UINT32 STDMETHODCALLTYPE rt_CalcInputFrames(IAudioProcessingObjectRT *This, UINT32 u32OutputFrameCount)
{
    (void)This;
    return u32OutputFrameCount;
}

static UINT32 STDMETHODCALLTYPE rt_CalcOutputFrames(IAudioProcessingObjectRT *This, UINT32 u32InputFrameCount)
{
    (void)This;
    return u32InputFrameCount;
}

static const IAudioProcessingObjectRTVtbl g_rt_vtbl = {
    rt_QueryInterface, rt_AddRef, rt_Release, rt_APOProcess, rt_CalcInputFrames, rt_CalcOutputFrames
};

/* ======================================================================================== */
/* IAudioSystemEffects / 2 / 3                                                               */

static HRESULT STDMETHODCALLTYPE fx_GetEffectsList(IAudioSystemEffects3 *This, LPGUID *ppEffectsIds,
                                                  UINT *pcEffects, HANDLE Event)
{
    E1Apo *a = FROM_FX(This);
    HRESULT hr = S_OK;

    if (ppEffectsIds == NULL || pcEffects == NULL) return E_POINTER;
    *ppEffectsIds = NULL;
    *pcEffects = 0;

    AcquireSRWLockExclusive(&a->lock);
    if (Event != NULL) {
        HANDLE dup = apo_dup_event(Event);
        if (dup) {
            if (a->fx_event) CloseHandle(a->fx_event);
            a->fx_event = dup;
        }
    }
    a->reported_active = apo_effective_active_locked(a);
    if (a->reported_active) {
        GUID *ids = (GUID *)CoTaskMemAlloc(sizeof(GUID));
        if (ids == NULL) {
            hr = E_OUTOFMEMORY;
        } else {
            ids[0] = *a->effect_id;
            *ppEffectsIds = ids;
            *pcEffects = 1;
        }
    }
    ReleaseSRWLockExclusive(&a->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE fx_GetControllableSystemEffectsList(IAudioSystemEffects3 *This,
                                                                    AUDIO_SYSTEMEFFECT **effects,
                                                                    UINT *numEffects, HANDLE event)
{
    E1Apo *a = FROM_FX(This);
    AUDIO_SYSTEMEFFECT *e;

    if (effects == NULL || numEffects == NULL) return E_POINTER;
    *effects = NULL;
    *numEffects = 0;

    e = (AUDIO_SYSTEMEFFECT *)CoTaskMemAlloc(sizeof(AUDIO_SYSTEMEFFECT));
    if (e == NULL) return E_OUTOFMEMORY;

    AcquireSRWLockExclusive(&a->lock);
    if (event != NULL) {
        HANDLE dup = apo_dup_event(event);
        if (dup) {
            if (a->ctl_event) CloseHandle(a->ctl_event);
            a->ctl_event = dup;
        }
    }
    e->id = *a->effect_id;
    e->canSetState = TRUE;
    /* The controllable state is the Windows-side gate; the panel's master switch is separate. */
    e->state = a->user_off ? AUDIO_SYSTEMEFFECT_STATE_OFF : AUDIO_SYSTEMEFFECT_STATE_ON;
    a->reported_user_off = a->user_off;
    ReleaseSRWLockExclusive(&a->lock);

    *effects = e;
    *numEffects = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fx_SetAudioSystemEffectState(IAudioSystemEffects3 *This, GUID effectId,
                                                             AUDIO_SYSTEMEFFECT_STATE state)
{
    E1Apo *a = FROM_FX(This);
    if (!IsEqualGUID(&effectId, a->effect_id)) return E_INVALIDARG;
    if (state != AUDIO_SYSTEMEFFECT_STATE_OFF && state != AUDIO_SYSTEMEFFECT_STATE_ON) return E_INVALIDARG;
    AcquireSRWLockExclusive(&a->lock);
    InterlockedExchange(&a->user_off, state == AUDIO_SYSTEMEFFECT_STATE_OFF ? 1 : 0);
    apo_notify_changes_locked(a);
    ReleaseSRWLockExclusive(&a->lock);
    return S_OK;
}

static const IAudioSystemEffects3Vtbl g_fx_vtbl = {
    fx_QueryInterface, fx_AddRef, fx_Release,
    fx_GetEffectsList, fx_GetControllableSystemEffectsList, fx_SetAudioSystemEffectState
};

/* ======================================================================================== */
/* IAudioProcessingObjectNotifications                                                       */

static HRESULT STDMETHODCALLTYPE ntf_GetApoNotificationRegistrationInfo(IAudioProcessingObjectNotifications *This,
                                                                       APO_NOTIFICATION_DESCRIPTOR **apoNotifications,
                                                                       DWORD *count)
{
    E1Apo *a = FROM_NTF(This);
    APO_NOTIFICATION_DESCRIPTOR *d;
    HRESULT hr = S_OK;

    if (apoNotifications == NULL || count == NULL) return E_POINTER;
    *apoNotifications = NULL;
    *count = 0;

    AcquireSRWLockShared(&a->lock);
    if (a->device != NULL) {
        d = (APO_NOTIFICATION_DESCRIPTOR *)CoTaskMemAlloc(sizeof(*d));
        if (d == NULL) {
            hr = E_OUTOFMEMORY;
        } else {
            memset(d, 0, sizeof(*d));
            d->type = APO_NOTIFICATION_TYPE_ENDPOINT_PROPERTY_CHANGE;
            IMMDevice_AddRef(a->device);          /* the caller releases the device it receives */
            d->audioEndpointPropertyChange.device = a->device;
            *apoNotifications = d;
            *count = 1;
        }
    }
    ReleaseSRWLockShared(&a->lock);
    return hr;
}

static void STDMETHODCALLTYPE ntf_HandleNotification(IAudioProcessingObjectNotifications *This,
                                                    APO_NOTIFICATION *n)
{
    E1Apo *a = FROM_NTF(This);
    if (n == NULL || n->type != APO_NOTIFICATION_TYPE_ENDPOINT_PROPERTY_CHANGE) return;
    if (!e1_pkey_equal(&n->audioEndpointPropertyChange.propertyKey, &E1_PKEY_AudioEndpoint_Disable_SysFx)) return;
    AcquireSRWLockExclusive(&a->lock);
    InterlockedExchange(&a->sysfx_disabled, apo_read_sysfx_disabled(n->audioEndpointPropertyChange.propertyStore));
    apo_notify_changes_locked(a);
    ReleaseSRWLockExclusive(&a->lock);
}

static const IAudioProcessingObjectNotificationsVtbl g_ntf_vtbl = {
    ntf_QueryInterface, ntf_AddRef, ntf_Release,
    ntf_GetApoNotificationRegistrationInfo, ntf_HandleNotification
};

/* ======================================================================================== */
/* construction                                                                              */

HRESULT e1apo_create(int kind, IUnknown *outer, REFIID riid, void **ppv)
{
    E1Apo *a;
    HRESULT hr;

    if (ppv == NULL) return E_POINTER;
    *ppv = NULL;
    if (kind != E1APO_KIND_RENDER && kind != E1APO_KIND_CAPTURE) return E_INVALIDARG;
    /* COM aggregation rule: an aggregated object can only be created for IID_IUnknown. */
    if (outer != NULL && !IsEqualIID(riid, &IID_IUnknown)) return CLASS_E_NOAGGREGATION;

    a = (E1Apo *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(E1Apo));
    if (a == NULL) return E_OUTOFMEMORY;
    a->lpVtblApo = &g_apo_vtbl;
    a->lpVtblRt = &g_rt_vtbl;
    a->lpVtblCfg = &g_cfg_vtbl;
    a->lpVtblFx = &g_fx_vtbl;
    a->lpVtblNtf = &g_ntf_vtbl;
    a->lpVtblInner = &g_inner_vtbl;
    a->outer = outer ? outer : (IUnknown *)&a->lpVtblInner;   /* no AddRef on the aggregator */
    a->refs = 1;
    a->kind = kind;
    a->clsid = e1apo_clsid(kind);
    a->effect_id = kind == E1APO_KIND_RENDER ? &E1FX_EFFECT_RENDER_CHAIN : &E1FX_EFFECT_CAPTURE_CHAIN;
    InitializeSRWLock(&a->lock);
    a->rt_last_seq = -1;
    a->ctl_last_seq = -1;
    e1fx_params_default(&a->file_params);
    a->ctl_params = a->file_params;
    a->rt_params = a->file_params;
    e1dll_object_created();

    apo_log(L"apo %p kind=%d: created (aggregated=%d)", (void *)a, kind, outer != NULL);
    hr = apo_qi(a, riid, ppv);
    apo_release(a);                  /* drops the construction reference (destroys on QI failure) */
    return hr;
}
