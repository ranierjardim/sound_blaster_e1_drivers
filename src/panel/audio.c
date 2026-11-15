/*
 * audio.c - see audio.h. Plain C COM against the MMDevice API.
 */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <SpatialAudioClient.h>
#include <mmreg.h>
#include <shlwapi.h>
#include <math.h>
#include "strings.h"
#include "audio.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

/* Interface ids (the SDK declares them extern without a library definition for C). */
DEFINE_GUID(E1_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(E1_IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(E1_IID_IMMNotificationClient, 0x7991EEC9, 0x7E89, 0x4D85, 0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0);
DEFINE_GUID(E1_IID_IAudioEndpointVolume, 0x5CDF2C82, 0x841E, 0x4546, 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A);
DEFINE_GUID(E1_IID_IAudioEndpointVolumeCallback, 0x657804FA, 0xD6AD, 0x4496, 0x8A, 0x60, 0x35, 0x27, 0x52, 0xAF, 0x4F, 0x89);
DEFINE_GUID(E1_IID_ISpatialAudioClient, 0xBBF8E066, 0xAAAA, 0x49BE, 0x9A, 0x4D, 0xFD, 0x2A, 0x85, 0x8E, 0xA2, 0x7F);
DEFINE_GUID(E1_IID_IDeviceTopology, 0x2A07407E, 0x6497, 0x4A18, 0x97, 0x87, 0x32, 0xF7, 0x9B, 0xD0, 0xD9, 0x8F);
DEFINE_GUID(E1_IID_IPart, 0xAE2DE0E4, 0x5BCA, 0x4F2D, 0xAA, 0x46, 0x5D, 0x13, 0xF8, 0xFD, 0xB3, 0xA9);
DEFINE_GUID(E1_IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(E1_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
/* Event context of our own volume changes (ignored in the callback). */
DEFINE_GUID(E1_VOLUME_CONTEXT, 0x3d0c7a52, 0x1f4b, 0x4f0e, 0x9e, 0x21, 0x5a, 0x6b, 0x0c, 0x11, 0x7e, 0x01);

/* Endpoint "device instance" property: "{1}.USB\VID_041E&PID_323B&MI_00\..." */
static const PROPERTYKEY k_pkey_instance = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2};
/* Active spatial-format record on render endpoints: 8 bytes header + GUID of the format. */
static const PROPERTYKEY k_pkey_spatial_fmt = {{0x8a845654, 0xd6c3, 0x4cd7, {0xb4, 0xeb, 0x24, 0x3d, 0x4b, 0xd9, 0x90, 0x32}}, 2};

/* Known spatial sound formats (Windows.Media.Audio SpatialAudioFormatSubtype). */
static const struct { GUID g; const wchar_t *name; } k_spatial_names[] = {
    {{0x1459AC38, 0x3875, 0x49BF, {0xBB, 0x59, 0x0F, 0xE8, 0x0F, 0x4D, 0x39, 0x5D}}, L"Dolby Atmos for Headphones"},
    {{0x4C81E564, 0xC8EF, 0x4AD9, {0x9F, 0x2C, 0x9E, 0xF9, 0x95, 0x53, 0x37, 0x90}}, L"Dolby Atmos for Speakers"},
    {{0xB53D940C, 0xB846, 0x4831, {0x9F, 0x76, 0xD1, 0x02, 0xB9, 0xB7, 0x25, 0xA0}}, L"Windows Sonic for Headphones"},
};

typedef struct VolCb {
    IAudioEndpointVolumeCallback iface;
    LONG   ref;
    EpKind kind;
} VolCb;

typedef struct NotifCb {
    IMMNotificationClient iface;
    LONG ref;
} NotifCb;

typedef struct EpState {
    AudioEp               info;
    IMMDevice            *dev;
    IAudioEndpointVolume *vol;
    VolCb                 cb;
    int                   cb_registered;
} EpState;

static struct {
    HWND                 hwnd;
    UINT                 msg_dev, msg_vol;
    IMMDeviceEnumerator *en;
    NotifCb              notif;
    int                  notif_registered;
    EpState              ep[2];
} A;

/* ---------------------------------------------------------------------------------------------- */
/* callbacks                                                                                        */

static HRESULT STDMETHODCALLTYPE vcb_qi(IAudioEndpointVolumeCallback *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &E1_IID_IUnknown) || IsEqualIID(riid, &E1_IID_IAudioEndpointVolumeCallback)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE vcb_addref(IAudioEndpointVolumeCallback *This) { return (ULONG)InterlockedIncrement(&((VolCb *)This)->ref); }
static ULONG STDMETHODCALLTYPE vcb_release(IAudioEndpointVolumeCallback *This) { return (ULONG)InterlockedDecrement(&((VolCb *)This)->ref); }
static HRESULT STDMETHODCALLTYPE vcb_notify(IAudioEndpointVolumeCallback *This, PAUDIO_VOLUME_NOTIFICATION_DATA d)
{
    VolCb *cb = (VolCb *)This;
    if (d && IsEqualGUID(&d->guidEventContext, &E1_VOLUME_CONTEXT)) return S_OK;
    if (A.hwnd) PostMessageW(A.hwnd, A.msg_vol, (WPARAM)cb->kind, 0);
    return S_OK;
}
static IAudioEndpointVolumeCallbackVtbl k_vcb_vtbl = {vcb_qi, vcb_addref, vcb_release, vcb_notify};

static HRESULT STDMETHODCALLTYPE nc_qi(IMMNotificationClient *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &E1_IID_IUnknown) || IsEqualIID(riid, &E1_IID_IMMNotificationClient)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nc_addref(IMMNotificationClient *This) { return (ULONG)InterlockedIncrement(&((NotifCb *)This)->ref); }
static ULONG STDMETHODCALLTYPE nc_release(IMMNotificationClient *This) { return (ULONG)InterlockedDecrement(&((NotifCb *)This)->ref); }
static void post_dev(LPARAM what) { if (A.hwnd) PostMessageW(A.hwnd, A.msg_dev, 0, what); }
static HRESULT STDMETHODCALLTYPE nc_state(IMMNotificationClient *This, LPCWSTR id, DWORD st) { (void)This; (void)id; (void)st; post_dev(0); return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_added(IMMNotificationClient *This, LPCWSTR id) { (void)This; (void)id; post_dev(0); return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_removed(IMMNotificationClient *This, LPCWSTR id) { (void)This; (void)id; post_dev(0); return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_default(IMMNotificationClient *This, EDataFlow f, ERole r, LPCWSTR id) { (void)This; (void)f; (void)r; (void)id; post_dev(0); return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_prop(IMMNotificationClient *This, LPCWSTR id, const PROPERTYKEY key)
{
    (void)This;
    (void)key;
    /* Only property changes of our endpoints matter (format, spatial format, names). Comparing the
       id strings is safe here: A.ep[].info.id is only rewritten on the UI thread during a rescan
       and a stale comparison merely causes one extra rescan. */
    if (id && ((A.ep[0].info.present && !lstrcmpiW(id, A.ep[0].info.id)) ||
               (A.ep[1].info.present && !lstrcmpiW(id, A.ep[1].info.id))))
        post_dev(1);
    return S_OK;
}
static IMMNotificationClientVtbl k_nc_vtbl = {nc_qi, nc_addref, nc_release, nc_state, nc_added, nc_removed, nc_default, nc_prop};

/* ---------------------------------------------------------------------------------------------- */
/* helpers                                                                                          */

static void read_str(IPropertyStore *ps, const PROPERTYKEY *k, wchar_t *out, int cch)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    out[0] = 0;
    if (SUCCEEDED(IPropertyStore_GetValue(ps, k, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        lstrcpynW(out, v.pwszVal, cch);
    PropVariantClear(&v);
}

static UINT read_uint(IPropertyStore *ps, const PROPERTYKEY *k, UINT def)
{
    PROPVARIANT v;
    UINT r = def;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, k, &v)) && (v.vt == VT_UI4 || v.vt == VT_I4)) r = v.ulVal;
    PropVariantClear(&v);
    return r;
}

static void read_format(IPropertyStore *ps, AudioEp *e)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    e->fmt_valid = 0;
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB &&
        v.blob.cbSize >= sizeof(WAVEFORMATEX) && v.blob.pBlobData) {
        const WAVEFORMATEX *wf = (const WAVEFORMATEX *)v.blob.pBlobData;
        e->rate = wf->nSamplesPerSec;
        e->bits = wf->wBitsPerSample;
        e->valid_bits = wf->wBitsPerSample;
        e->channels = wf->nChannels;
        e->is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && v.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
            const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)wf;
            if (x->Samples.wValidBitsPerSample) e->valid_bits = x->Samples.wValidBitsPerSample;
            e->is_float = IsEqualGUID(&x->SubFormat, &E1_SUBTYPE_IEEE_FLOAT);
        }
        e->fmt_valid = 1;
    }
    PropVariantClear(&v);
}

static int topology_match(IMMDevice *d)
{
    IDeviceTopology *topo = NULL, *topo2 = NULL;
    IConnector *con = NULL, *con2 = NULL;
    IPart *part = NULL;
    LPWSTR id = NULL;
    int match = 0;
    if (FAILED(IMMDevice_Activate(d, &E1_IID_IDeviceTopology, CLSCTX_INPROC_SERVER, NULL, (void **)&topo))) return 0;
    if (SUCCEEDED(IDeviceTopology_GetConnector(topo, 0, &con)) &&
        SUCCEEDED(IConnector_GetConnectedTo(con, &con2)) &&
        SUCCEEDED(IConnector_QueryInterface(con2, &E1_IID_IPart, (void **)&part)) &&
        SUCCEEDED(IPart_GetTopologyObject(part, &topo2)) &&
        SUCCEEDED(IDeviceTopology_GetDeviceId(topo2, &id)) && id) {
        match = StrStrIW(id, E1_HWID_MATCH) != NULL;
    }
    if (id) CoTaskMemFree(id);
    if (topo2) IDeviceTopology_Release(topo2);
    if (part) IPart_Release(part);
    if (con2) IConnector_Release(con2);
    if (con) IConnector_Release(con);
    IDeviceTopology_Release(topo);
    return match;
}

static int is_e1(IMMDevice *d, IPropertyStore *ps)
{
    wchar_t inst[260];
    read_str(ps, &k_pkey_instance, inst, 260);
    if (inst[0]) return StrStrIW(inst, E1_HWID_MATCH) != NULL;
    return topology_match(d);
}

static void release_ep(EpState *s)
{
    if (s->vol) {
        if (s->cb_registered) IAudioEndpointVolume_UnregisterControlChangeNotify(s->vol, &s->cb.iface);
        IAudioEndpointVolume_Release(s->vol);
    }
    if (s->dev) IMMDevice_Release(s->dev);
    s->vol = NULL;
    s->dev = NULL;
    s->cb_registered = 0;
    ZeroMemory(&s->info, sizeof s->info);
}

static void read_volume(EpState *s)
{
    AudioEp *e = &s->info;
    BOOL m = FALSE;
    float v = 0;
    if (!s->vol) { e->has_volume = 0; return; }
    e->has_volume = SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(s->vol, &v));
    e->vol = v;
    if (SUCCEEDED(IAudioEndpointVolume_GetMute(s->vol, &m))) e->mute = m ? 1 : 0;
    UINT n = 0;
    e->balance = 0;
    if (SUCCEEDED(IAudioEndpointVolume_GetChannelCount(s->vol, &n))) e->vol_channels = n;
    if (n == 2) {
        float l = 0, r = 0;
        if (SUCCEEDED(IAudioEndpointVolume_GetChannelVolumeLevelScalar(s->vol, 0, &l)) &&
            SUCCEEDED(IAudioEndpointVolume_GetChannelVolumeLevelScalar(s->vol, 1, &r))) {
            float mx = l > r ? l : r;
            if (mx > 1e-6f) e->balance = (r - l) / mx;
        }
    }
}

static void bind_ep(EpState *s, EpKind k, IMMDevice *d, IPropertyStore *ps)
{
    LPWSTR id = NULL;
    s->dev = d;
    IMMDevice_AddRef(d);
    s->info.present = 1;
    if (SUCCEEDED(IMMDevice_GetId(d, &id)) && id) {
        lstrcpynW(s->info.id, id, 128);
        CoTaskMemFree(id);
    }
    read_str(ps, &PKEY_Device_FriendlyName, s->info.name, 128);
    read_str(ps, &PKEY_DeviceInterface_FriendlyName, s->info.device, 96);
    read_str(ps, &PKEY_Device_DeviceDesc, s->info.desc, 64);
    read_format(ps, &s->info);
    if (SUCCEEDED(IMMDevice_Activate(d, &E1_IID_IAudioEndpointVolume, CLSCTX_INPROC_SERVER, NULL, (void **)&s->vol))) {
        s->cb.iface.lpVtbl = &k_vcb_vtbl;
        s->cb.ref = 1;
        s->cb.kind = k;
        s->cb_registered = SUCCEEDED(IAudioEndpointVolume_RegisterControlChangeNotify(s->vol, &s->cb.iface));
    }
    read_volume(s);
}

/* Picks the E1 endpoint of one data flow. Capture: prefers the microphone (form factor 4, mono)
   over the 2-channel loopback endpoint the Creative driver also exposes. */
static void find_ep(EpKind k)
{
    EpState *s = &A.ep[k];
    IMMDeviceCollection *col = NULL;
    IMMDevice *best = NULL, *def = NULL;
    IPropertyStore *best_ps = NULL;
    int best_score = -1;
    LPWSTR def_id = NULL;
    UINT n = 0;
    EDataFlow flow = k == EP_RENDER ? eRender : eCapture;

    release_ep(s);
    if (!A.en) return;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(A.en, flow, eConsole, &def)) && def) {
        IMMDevice_GetId(def, &def_id);
        IMMDevice_Release(def);
    }
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(A.en, flow, DEVICE_STATE_ACTIVE, &col))) goto done;
    IMMDeviceCollection_GetCount(col, &n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice *d = NULL;
        IPropertyStore *ps = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && is_e1(d, ps)) {
            int score = 0;
            LPWSTR id = NULL;
            if (k == EP_CAPTURE) {
                AudioEp tmp;
                ZeroMemory(&tmp, sizeof tmp);
                read_format(ps, &tmp);
                if (read_uint(ps, &PKEY_AudioEndpoint_FormFactor, 0) == Microphone) score += 4;
                if (tmp.fmt_valid && tmp.channels == 1) score += 2;
            }
            if (def_id && SUCCEEDED(IMMDevice_GetId(d, &id)) && id) {
                if (!lstrcmpiW(id, def_id)) score += 1;
                CoTaskMemFree(id);
            }
            if (score > best_score) {
                if (best) IMMDevice_Release(best);
                if (best_ps) IPropertyStore_Release(best_ps);
                best = d;
                best_ps = ps;
                best_score = score;
                continue; /* keep references */
            }
        }
        if (ps) IPropertyStore_Release(ps);
        IMMDevice_Release(d);
    }
    if (best) bind_ep(s, k, best, best_ps);
done:
    if (best_ps) IPropertyStore_Release(best_ps);
    if (best) IMMDevice_Release(best);
    if (col) IMMDeviceCollection_Release(col);
    if (def_id) CoTaskMemFree(def_id);
}

/* ---------------------------------------------------------------------------------------------- */
/* public                                                                                           */

int audio_init(HWND notify, UINT msg_devchange, UINT msg_volchange)
{
    ZeroMemory(&A, sizeof A);
    A.hwnd = notify;
    A.msg_dev = msg_devchange;
    A.msg_vol = msg_volchange;
    if (FAILED(CoCreateInstance(&E1_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &E1_IID_IMMDeviceEnumerator,
                                (void **)&A.en)))
        return 0;
    A.notif.iface.lpVtbl = &k_nc_vtbl;
    A.notif.ref = 1;
    A.notif_registered = SUCCEEDED(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(A.en, &A.notif.iface));
    audio_rescan();
    return 1;
}

void audio_shutdown(void)
{
    release_ep(&A.ep[0]);
    release_ep(&A.ep[1]);
    if (A.en) {
        if (A.notif_registered) IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(A.en, &A.notif.iface);
        IMMDeviceEnumerator_Release(A.en);
    }
    A.en = NULL;
    A.hwnd = NULL;
}

void audio_rescan(void)
{
    find_ep(EP_RENDER);
    find_ep(EP_CAPTURE);
}

const AudioEp *audio_ep(EpKind k) { return &A.ep[k].info; }

void audio_refresh_volume(EpKind k) { read_volume(&A.ep[k]); }

int audio_set_volume(EpKind k, float scalar)
{
    EpState *s = &A.ep[k];
    if (!s->vol) return 0;
    if (scalar < 0) scalar = 0;
    if (scalar > 1) scalar = 1;
    if (FAILED(IAudioEndpointVolume_SetMasterVolumeLevelScalar(s->vol, scalar, &E1_VOLUME_CONTEXT))) return 0;
    read_volume(s);
    return 1;
}

int audio_set_mute(EpKind k, int mute)
{
    EpState *s = &A.ep[k];
    if (!s->vol) return 0;
    if (FAILED(IAudioEndpointVolume_SetMute(s->vol, mute ? TRUE : FALSE, &E1_VOLUME_CONTEXT))) return 0;
    s->info.mute = mute ? 1 : 0;
    return 1;
}

int audio_set_balance(EpKind k, float b)
{
    EpState *s = &A.ep[k];
    float l = 0, r = 0;
    if (!s->vol || s->info.vol_channels != 2) return 0;
    if (b < -1) b = -1;
    if (b > 1) b = 1;
    if (FAILED(IAudioEndpointVolume_GetChannelVolumeLevelScalar(s->vol, 0, &l)) ||
        FAILED(IAudioEndpointVolume_GetChannelVolumeLevelScalar(s->vol, 1, &r)))
        return 0;
    float m = l > r ? l : r;
    if (m <= 1e-6f) {
        if (FAILED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(s->vol, &m))) return 0;
    }
    float nl = m * (b > 0 ? 1 - b : 1), nr = m * (b < 0 ? 1 + b : 1);
    HRESULT h1 = IAudioEndpointVolume_SetChannelVolumeLevelScalar(s->vol, 0, nl, &E1_VOLUME_CONTEXT);
    HRESULT h2 = IAudioEndpointVolume_SetChannelVolumeLevelScalar(s->vol, 1, nr, &E1_VOLUME_CONTEXT);
    read_volume(s);
    return SUCCEEDED(h1) && SUCCEEDED(h2);
}

void audio_spatial(SpatialInfo *out)
{
    EpState *s = &A.ep[EP_RENDER];
    ISpatialAudioClient *sac = NULL;
    IPropertyStore *ps = NULL;
    ZeroMemory(out, sizeof *out);
    if (!s->dev) return;
    out->checked = 1;
    if (SUCCEEDED(IMMDevice_Activate(s->dev, &E1_IID_ISpatialAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&sac)) && sac) {
        UINT32 n = 0;
        if (SUCCEEDED(ISpatialAudioClient_GetMaxDynamicObjectCount(sac, &n))) out->max_objects = n;
        ISpatialAudioClient_Release(sac);
    }
    /* Windows reports dynamic objects only while a spatial format is active on the endpoint. */
    out->active = out->max_objects > 0;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(s->dev, STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &k_pkey_spatial_fmt, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= 24 &&
            v.blob.pBlobData) {
            memcpy(&out->format, v.blob.pBlobData + 8, sizeof(GUID));
            for (size_t i = 0; i < sizeof k_spatial_names / sizeof k_spatial_names[0]; ++i)
                if (IsEqualGUID(&out->format, &k_spatial_names[i].g)) lstrcpynW(out->name, k_spatial_names[i].name, 64);
        }
        PropVariantClear(&v);
        IPropertyStore_Release(ps);
    }
    if (!out->active) out->name[0] = 0;
}

void audio_format_short(const AudioEp *e, wchar_t *out, int cch)
{
    if (!e->fmt_valid) { out[0] = 0; return; }
    DWORD khz10 = (e->rate + 50) / 100;
    if (khz10 % 10) wnsprintfW(out, cch, L"%lu.%lu kHz / %u-bit%s", khz10 / 10, khz10 % 10, (unsigned)e->valid_bits, e->is_float ? L" float" : L"");
    else wnsprintfW(out, cch, L"%lu kHz / %u-bit%s", khz10 / 10, (unsigned)e->valid_bits, e->is_float ? L" float" : L"");
    str_decimal(out);
}

void audio_format_long(const AudioEp *e, wchar_t *out, int cch)
{
    wchar_t s[64];
    if (!e->fmt_valid) { out[0] = 0; return; }
    audio_format_short(e, s, 64);
    wnsprintfW(out, cch, L"%s / %u %s", s, (unsigned)e->channels, tr(S_CH_UNIT));
}
