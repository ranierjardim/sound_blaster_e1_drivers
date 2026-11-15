/*
 * e1fx_apo_internal.h - private declarations shared by the e1fx.dll translation units.
 */
#ifndef E1FX_APO_INTERNAL_H
#define E1FX_APO_INTERNAL_H

#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef CINTERFACE
#define CINTERFACE
#endif
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <audiomediatype.h>
#include <mmdeviceapi.h>

#include "../../include/e1fx_guids.h"

/* ---- module bookkeeping (e1fx_dll.c) ---- */
extern HMODULE g_e1_module;
void e1dll_object_created(void);     /* every live COM object we hand out (APOs, media types) */
void e1dll_object_destroyed(void);
LONG e1dll_object_count(void);

/* ---- APO classes (e1fx_apo.c) ---- */
enum { E1APO_KIND_RENDER = 0, E1APO_KIND_CAPTURE = 1 };

#define E1APO_FRIENDLY_NAME  L"E1 Studio Effects"
#define E1APO_COPYRIGHT      L"Copyright (c) 2026 E1 Studio contributors"
#define E1APO_MAJOR_VERSION  1u
#define E1APO_MINOR_VERSION  0u
#define E1APO_MIN_RATE       8000u
#define E1APO_MAX_RATE       384000u
#define E1APO_MAX_CHANNELS   8u

HRESULT e1apo_create(int kind, IUnknown *outer, REFIID riid, void **ppv);  /* outer: COM aggregator or NULL */
const CLSID *e1apo_clsid(int kind);
/* Registration properties (CoTaskMemAlloc'ed, caller frees with CoTaskMemFree). */
HRESULT e1apo_get_reg_properties(int kind, APO_REG_PROPERTIES **out);

/* ---- interface ids (e1fx_iids.c). Own names: the SDK ships no C definitions for these. ---- */
extern const IID E1_IID_IAudioProcessingObject;
extern const IID E1_IID_IAudioProcessingObjectRT;
extern const IID E1_IID_IAudioProcessingObjectConfiguration;
extern const IID E1_IID_IAudioSystemEffects;
extern const IID E1_IID_IAudioSystemEffects2;
extern const IID E1_IID_IAudioSystemEffects3;
extern const IID E1_IID_IAudioProcessingObjectNotifications;
extern const IID E1_IID_IAudioMediaType;
extern const IID E1_IID_IMMDevice;
extern const GUID E1_KSDATAFORMAT_SUBTYPE_PCM;
extern const GUID E1_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
extern const GUID E1_AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
extern const GUID E1_AUDIO_SIGNALPROCESSINGMODE_RAW;
extern const PROPERTYKEY E1_PKEY_AudioEndpoint_Disable_SysFx;

/* The SDK's IsEqualPropertyKey macro only compiles as C++. */
static __inline int e1_pkey_equal(const PROPERTYKEY *a, const PROPERTYKEY *b)
{
    return a->pid == b->pid && IsEqualGUID(&a->fmtid, &b->fmtid);
}

/* ---- IAudioMediaType implementation (e1fx_mediatype.c) ---- */
/* Copies an arbitrary WAVEFORMATEX (cb = full size including cbSize extra bytes). */
HRESULT e1mt_create_from_wfx(const WAVEFORMATEX *wfx, UINT32 cb, IAudioMediaType **out);
/* Interleaved float32 WAVEFORMATEXTENSIBLE; mask 0 = default mask for the channel count. */
HRESULT e1mt_create_float(UINT32 channels, UINT32 rate, DWORD mask, IAudioMediaType **out);
DWORD   e1mt_default_mask(UINT32 channels);

#endif /* E1FX_APO_INTERNAL_H */
