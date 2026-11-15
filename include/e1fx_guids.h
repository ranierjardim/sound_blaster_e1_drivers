/*
 * e1fx_guids.h - COM identities of the E1 Studio APO (our own namespace; nothing Creative).
 * Include <initguid.h> before this header in exactly one translation unit to define them.
 */
#ifndef E1FX_GUIDS_H
#define E1FX_GUIDS_H

#include <guiddef.h>

/* Render-side MFX APO (SBX-style chain).  {9041657D-F768-4739-B84D-7CA73B55FE94} */
DEFINE_GUID(CLSID_E1FxRenderMfx, 0x9041657d, 0xf768, 0x4739, 0xb8, 0x4d, 0x7c, 0xa7, 0x3b, 0x55, 0xfe, 0x94);
/* Capture-side MFX APO (CrystalVoice-style chain).  {6768B677-635E-46A6-9814-8560B27B571D} */
DEFINE_GUID(CLSID_E1FxCaptureMfx, 0x6768b677, 0x635e, 0x46a6, 0x98, 0x14, 0x85, 0x60, 0xb2, 0x7b, 0x57, 0x1d);
/* Effect ids reported through IAudioSystemEffects2::GetEffectsList.  {D474D264-B705-438D-8589-556A9C286298} */
DEFINE_GUID(E1FX_EFFECT_RENDER_CHAIN, 0xd474d264, 0xb705, 0x438d, 0x85, 0x89, 0x55, 0x6a, 0x9c, 0x28, 0x62, 0x98);
/* {99A643D6-7D29-4C36-805C-93BEB976095D} */
DEFINE_GUID(E1FX_EFFECT_CAPTURE_CHAIN, 0x99a643d6, 0x7d29, 0x4c36, 0x80, 0x5c, 0x93, 0xbe, 0xb9, 0x76, 0x09, 0x5d);

#define E1FX_RENDER_CLSID_STR  L"{9041657D-F768-4739-B84D-7CA73B55FE94}"
#define E1FX_CAPTURE_CLSID_STR L"{6768B677-635E-46A6-9814-8560B27B571D}"

#endif /* E1FX_GUIDS_H */
