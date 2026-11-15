/*
 * e1fx_mediatype.c - minimal IAudioMediaType implementation in C.
 *
 * The SDK's CreateAudioMediaType lives in a C++ static library built against the OS-internal
 * CRT; linking it into a plain C DLL is fragile, so the APO returns its own objects. The audio
 * engine only consumes them through the IAudioMediaType vtable.
 */
#include "e1fx_apo_internal.h"
#include <string.h>

typedef struct E1MediaType {
    const IAudioMediaTypeVtbl *lpVtbl;
    volatile LONG refs;
    UINT32 cb;                        /* bytes of the stored WAVEFORMATEX (incl. extra bytes) */
    BOOL compressed;                  /* not describable as UNCOMPRESSEDAUDIOFORMAT */
    UNCOMPRESSEDAUDIOFORMAT uf;
    union {                           /* stored format; extra bytes follow for exotic formats */
        WAVEFORMATEX wfx;
        WAVEFORMATEXTENSIBLE wfxe;
    } fmt;
} E1MediaType;

static E1MediaType *mt_from(IAudioMediaType *p) { return (E1MediaType *)p; }

DWORD e1mt_default_mask(UINT32 channels)
{
    switch (channels) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 3: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER;
    case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 5: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                   SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 7: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_BACK_CENTER;
    case 8: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                   SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    default: return 0;
    }
}

/* Derives UNCOMPRESSEDAUDIOFORMAT from PCM / IEEE float / EXTENSIBLE(PCM|float). */
static BOOL mt_describe(const WAVEFORMATEX *w, UINT32 cb, UNCOMPRESSEDAUDIOFORMAT *uf)
{
    UINT32 container;
    memset(uf, 0, sizeof(*uf));
    if (w->nChannels == 0 || w->nSamplesPerSec == 0 || w->wBitsPerSample == 0 ||
        w->nBlockAlign == 0 || (w->nBlockAlign % w->nChannels) != 0)
        return FALSE;
    container = w->nBlockAlign / w->nChannels;

    if (w->wFormatTag == WAVE_FORMAT_PCM || w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        uf->guidFormatType = (w->wFormatTag == WAVE_FORMAT_PCM) ? E1_KSDATAFORMAT_SUBTYPE_PCM
                                                                : E1_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        uf->dwValidBitsPerSample = w->wBitsPerSample;
        uf->dwChannelMask = e1mt_default_mask(w->nChannels);
    } else if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && cb >= sizeof(WAVEFORMATEXTENSIBLE) &&
               w->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)w;
        if (!IsEqualGUID(&x->SubFormat, &E1_KSDATAFORMAT_SUBTYPE_PCM) &&
            !IsEqualGUID(&x->SubFormat, &E1_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            return FALSE;
        uf->guidFormatType = x->SubFormat;
        uf->dwValidBitsPerSample = x->Samples.wValidBitsPerSample ? x->Samples.wValidBitsPerSample
                                                                  : w->wBitsPerSample;
        uf->dwChannelMask = x->dwChannelMask;
    } else {
        return FALSE;
    }
    if (container * 8u != w->wBitsPerSample) return FALSE;
    uf->dwSamplesPerFrame = w->nChannels;
    uf->dwBytesPerSampleContainer = container;
    uf->fFramesPerSecond = (FLOAT)w->nSamplesPerSec;
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE mt_QueryInterface(IAudioMediaType *This, REFIID riid, void **ppv)
{
    if (ppv == NULL) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &E1_IID_IAudioMediaType)) {
        *ppv = This;
        InterlockedIncrement(&mt_from(This)->refs);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE mt_AddRef(IAudioMediaType *This)
{
    return (ULONG)InterlockedIncrement(&mt_from(This)->refs);
}

static ULONG STDMETHODCALLTYPE mt_Release(IAudioMediaType *This)
{
    LONG r = InterlockedDecrement(&mt_from(This)->refs);
    if (r == 0) {
        HeapFree(GetProcessHeap(), 0, This);
        e1dll_object_destroyed();
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE mt_IsCompressedFormat(IAudioMediaType *This, BOOL *pfCompressed)
{
    if (pfCompressed == NULL) return E_POINTER;
    *pfCompressed = mt_from(This)->compressed;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mt_IsEqual(IAudioMediaType *This, IAudioMediaType *other, DWORD *pdwFlags)
{
    E1MediaType *m = mt_from(This);
    UNCOMPRESSEDAUDIOFORMAT ou;
    BOOL ocomp = TRUE;
    DWORD flags = 0;

    if (other == NULL || pdwFlags == NULL) return E_POINTER;
    *pdwFlags = 0;
    if (FAILED(IAudioMediaType_IsCompressedFormat(other, &ocomp))) return E_FAIL;

    if (!m->compressed && !ocomp) {
        if (FAILED(IAudioMediaType_GetUncompressedAudioFormat(other, &ou))) return E_FAIL;
        if (IsEqualGUID(&m->uf.guidFormatType, &ou.guidFormatType)) {
            flags |= AUDIOMEDIATYPE_EQUAL_FORMAT_TYPES;
            if (m->uf.dwSamplesPerFrame == ou.dwSamplesPerFrame &&
                m->uf.dwBytesPerSampleContainer == ou.dwBytesPerSampleContainer &&
                m->uf.dwValidBitsPerSample == ou.dwValidBitsPerSample &&
                m->uf.fFramesPerSecond == ou.fFramesPerSecond &&
                m->uf.dwChannelMask == ou.dwChannelMask)
                flags |= AUDIOMEDIATYPE_EQUAL_FORMAT_DATA | AUDIOMEDIATYPE_EQUAL_FORMAT_USER_DATA;
        }
    } else if (m->compressed && ocomp) {
        const WAVEFORMATEX *ow = IAudioMediaType_GetAudioFormat(other);
        if (ow != NULL && ow->wFormatTag == m->fmt.wfx.wFormatTag) {
            flags |= AUDIOMEDIATYPE_EQUAL_FORMAT_TYPES;
            if (ow->cbSize == m->fmt.wfx.cbSize &&
                memcmp(ow, &m->fmt.wfx, sizeof(WAVEFORMATEX) + ow->cbSize) == 0)
                flags |= AUDIOMEDIATYPE_EQUAL_FORMAT_DATA | AUDIOMEDIATYPE_EQUAL_FORMAT_USER_DATA;
        }
    }
    *pdwFlags = flags;
    return (flags & (AUDIOMEDIATYPE_EQUAL_FORMAT_TYPES | AUDIOMEDIATYPE_EQUAL_FORMAT_DATA)) ==
                   (AUDIOMEDIATYPE_EQUAL_FORMAT_TYPES | AUDIOMEDIATYPE_EQUAL_FORMAT_DATA)
               ? S_OK : S_FALSE;
}

static const WAVEFORMATEX *STDMETHODCALLTYPE mt_GetAudioFormat(IAudioMediaType *This)
{
    return &mt_from(This)->fmt.wfx;
}

static HRESULT STDMETHODCALLTYPE mt_GetUncompressedAudioFormat(IAudioMediaType *This,
                                                               UNCOMPRESSEDAUDIOFORMAT *out)
{
    E1MediaType *m = mt_from(This);
    if (out == NULL) return E_POINTER;
    if (m->compressed) return APOERR_FORMAT_NOT_SUPPORTED;
    *out = m->uf;
    return S_OK;
}

static const IAudioMediaTypeVtbl g_mt_vtbl = {
    mt_QueryInterface, mt_AddRef, mt_Release,
    mt_IsCompressedFormat, mt_IsEqual, mt_GetAudioFormat, mt_GetUncompressedAudioFormat
};

HRESULT e1mt_create_from_wfx(const WAVEFORMATEX *wfx, UINT32 cb, IAudioMediaType **out)
{
    E1MediaType *m;
    SIZE_T alloc;
    UINT32 need;

    if (out == NULL) return E_POINTER;
    *out = NULL;
    if (wfx == NULL || cb < sizeof(WAVEFORMATEX)) return E_INVALIDARG;
    need = (wfx->wFormatTag == WAVE_FORMAT_PCM) ? (UINT32)sizeof(WAVEFORMATEX)
                                                : (UINT32)sizeof(WAVEFORMATEX) + wfx->cbSize;
    if (cb < need) return E_INVALIDARG;

    alloc = sizeof(E1MediaType);
    if (need > sizeof(WAVEFORMATEXTENSIBLE)) alloc += need - sizeof(WAVEFORMATEXTENSIBLE);
    m = (E1MediaType *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, alloc);
    if (m == NULL) return E_OUTOFMEMORY;

    m->lpVtbl = &g_mt_vtbl;
    m->refs = 1;
    m->cb = need;
    memcpy(&m->fmt, wfx, need);
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) m->fmt.wfx.cbSize = 0;
    m->compressed = !mt_describe(&m->fmt.wfx, need, &m->uf);

    e1dll_object_created();
    *out = (IAudioMediaType *)m;
    return S_OK;
}

HRESULT e1mt_create_float(UINT32 channels, UINT32 rate, DWORD mask, IAudioMediaType **out)
{
    WAVEFORMATEXTENSIBLE x;
    if (channels == 0 || channels > 0xFFFFu / 4u || rate == 0) return E_INVALIDARG;
    memset(&x, 0, sizeof(x));
    x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels = (WORD)channels;
    x.Format.nSamplesPerSec = rate;
    x.Format.wBitsPerSample = 32;
    x.Format.nBlockAlign = (WORD)(channels * 4u);
    x.Format.nAvgBytesPerSec = rate * channels * 4u;
    x.Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    x.Samples.wValidBitsPerSample = 32;
    x.dwChannelMask = mask ? mask : e1mt_default_mask(channels);
    x.SubFormat = E1_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return e1mt_create_from_wfx(&x.Format, (UINT32)sizeof(x), out);
}
