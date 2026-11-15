# Sound Blaster E1 on Windows 11 — Reconstruction Spec

Lead-architect synthesis of the five reverse-engineering notes in `docs/re/` (`kernel-usb.md`, `apo-dsp.md`,
`plumbing-ipc.md`, `control-panel.md`, `system-diagnosis.md`) **including their Verification sections**. Where a
verifier corrected an analyst, this spec uses the corrected value. Conflicts between notes are listed in §8.

Target: Creative Sound Blaster E1, `USB\VID_041E&PID_323B` (MI_00 audio, MI_03 HID), on this PC
(Windows 11 Home 26H2, build 26300, x64). Goal: a new C implementation that reproduces the original Sound
Blaster control-panel features and gives the best possible audio quality.

Tags: **[C]** = confirmed in code, data, live device or registry by an analyst or verifier. **[I]** = inferred.
**[GAP]** = not known yet; work item. Addresses are given only where they help someone re-check a claim; see the
per-topic notes for full detail. Everything here is written in our own words; no Creative code is reproduced,
and Creative's HRTF/coefficient tables, skin images and trademarks must not be shipped (§3.9).

---

## 0. One-page summary

* **The hardware is a fixed 44.1 kHz full-speed UAC1 device.** Playback is 16-bit or 24-bit packed stereo on an
  asynchronous isochronous endpoint with explicit feedback. The mic is 16-bit mono. There is no hardware DSP, no
  mixer, no loopback and no sample-rate control. No driver can raise these limits. [C]
* **Creative's Windows 10 kernel driver already streams bit-transparent 24-bit/44.1 kHz with correct feedback on
  this PC** (live pipe probe). The kernel path is not the cause of the quality loss. [C]
* **The quality loss comes from software processing that Windows 7 never had or that is now at defaults**:
  Dolby Atmos for Headphones is active on the E1 endpoint, stacked on Creative's SBX Surround (headphone
  virtualizer) and Crystalizer at INF defaults, after Storage Sense wiped the user's settings. [C] (§2)
* **The DSP math in the Win7 and Win10 APO builds is identical.** The only functional render-chain difference is
  that Win7 ran the GFX profile *with* a SpeakerEQ stage, while Win8.1+ runs the MFX profile *without* it. [C chain /
  I audibility]
* **Recommended architecture (§6): Option A first** — a new C APO (all SBX/CrystalVoice DSP), a new native C control
  panel and a small helper, on top of a kernel transport that already loads under Secure Boot (Creative's WHQL
  `ksUSBaud.sys` first, then the inbox `usbaudio.sys` once its feedback handling is verified). **Option B** (new
  KMDF/ACX kernel driver in C) is Phase 2 and cannot load on this PC without disabling Secure Boot (BitLocker
  recovery key) or buying attestation signing. **Option C** (filter driver) is only justified if the jack-retask
  feature turns out to need a vendor USB request and the inbox driver is chosen.
* **Vendor USB requests are not needed for audio quality or for any core feature.** The only feature that might need
  one is the headphone/mic jack retask, and its mechanism is still unknown (§1.7, §6.5).

---

## 1. Device facts

### 1.1 Identity [C]
| Item | Value |
|---|---|
| VID/PID | 0x041E / 0x323B ("Sound Blaster E1"; Creative internal name "U3A") |
| bcdUSB / bcdDevice | 2.00 / 0x1006 (RevisionId 4102) |
| Speed (live) | **Full speed** (USB 1.1 signalling), on `USB\ROOT_HUB30` port 5 |
| Strings | 1 "Creative Technology Ltd", 2 "Sound Blaster E1", 3 serial "0000047173", 4 "10012006" (unreferenced; firmware/HW id [I]) |
| Config | 1 config, 279 bytes, 4 interfaces, bus-powered (0x80), 100 mA; device qualifier STALLs (normal at FS) |
| Composite | `usbccgp`; MI_00 → audio function (IF0..IF2), MI_03 → HID |

### 1.2 Audio topology (IF0, UAC 1.0, bcdADC 1.00) [C]
```
Playback: IT#2 USB streaming, 2 ch (L,R) ─► FU#5 (master: Mute; ch1, ch2: Volume) ─► OT#4 Speaker (0x0301)
Capture : IT#1 Microphone (0x0201), 1 ch ─► FU#6 (master: Mute + Volume)       ─► OT#3 USB streaming
```
No mixer, selector, processing or extension units. **No AC interrupt endpoint.** OT#3 bAssocTerminal = 0
(verifier correction). Entity IDs used in class requests: FU5 = 5, FU6 = 6.

### 1.3 Streaming interfaces [C]
| IF/alt | Format | Endpoint(s) |
|---|---|---|
| IF1 alt0 | zero bandwidth | — |
| IF1 alt1 | PCM, 2 ch, subframe 2, 16-bit, **44100 Hz only** | EP 0x01 OUT iso **async** (0x05), wMaxPacketSize 200, bInterval 1, bSynchAddress 0x83 |
| IF1 alt2 | PCM, 2 ch, **subframe 3, 24-bit**, 44100 Hz only | EP 0x01 OUT iso async, wMaxPacketSize **300** |
| feedback | UAC1 explicit feedback | EP **0x83 IN** iso (0x01), wMaxPacketSize 3 (**10.14**), bInterval 1, **bRefresh 5 (32 ms)** |
| IF2 alt0 | zero bandwidth | — |
| IF2 alt1 | PCM, **1 ch, 16-bit**, 44100 Hz only | EP 0x81 IN iso async (0x05), wMaxPacketSize 200 |
| IF3 | HID 1.00, report descriptor 91 bytes | EP 0x84 IN interrupt, 4 bytes, 10 ms |

Class-specific AS endpoint bmAttributes = 0 on both data endpoints: **no sampling-frequency control** — never send
SET_CUR SAMPLING_FREQ (Creative's driver only sends it when bit 0 is set, so it never does for the E1). [C]

Live (verifier): during playback through Creative's driver the open pipes are EP01 (mps 300 → alt2, 24-bit) and
EP83. No mic pipe opens during render-only playback. [C]

### 1.4 Packet and clock math (for any kernel transport) [C unless noted]
* Nominal: 44.1 frames per 1 ms frame. Without feedback: 9 × 44 + 1 × 45 frames per 10 ms (per-mille accumulator).
* Feedback: 3-byte little-endian 10.14 value = frames per ms. Nominal 44.1 × 2^14 ≈ 722534 = `0x0B0666`.
  Convert to Q16.16 with `<< 2` (nominal ≈ 0x2C1998). Keep a Q16.16 accumulator; each packet takes the integer
  part as its frame count and keeps the fraction. Packets are therefore 44 or 45 frames (alt2: 264 or 270 bytes).
* Creative rejects feedback outside `[(n−2)<<16, (n+2)<<16]` with `n = 44` (inclusive) and substitutes nominal
  `(rate<<16)/1000`. This is a sane sanity window at 44.1 kHz; keep it but log rejections. [C]
* Initial value before the first report = nominal. Feedback URBs are scheduled once per bRefresh (32 ms). [C]
* Creative streaming parameters (good defaults): 3 URBs in flight × 10 packets (10 ms each), render pulled in
  5 ms chunks, explicit StartFrame; resync to `current+5` if the stored frame is behind or more than 100 frames
  ahead; a pre-roll counter sends silent chunks at start. ≈30 ms of queued isochronous data. [C]
* Use `GET_ISO_URB_SIZE(n)`; Creative over-allocates by 12 bytes (harmless). [C]
* Bandwidth fallback: on STATUS_INSUFFICIENT_RESOURCES Creative silently retries 24→16-bit. A new driver must not
  do this silently. [C]
* High-speed feedback (4-byte 16.16): W7 uses as-is, W10 doubles it. Irrelevant at FS. [C]

### 1.5 Hardware controls (standard UAC1 class requests on IF0) [C]
| Control | Request | wValue | wIndex | Data |
|---|---|---|---|---|
| FU5 L/R volume | GET_CUR 0x81 / SET_CUR 0x01 / GET_MIN 0x82 / GET_MAX 0x83 / GET_RES 0x84 (bmRequestType 0xA1/0x21) | `0x0200 \| ch` (ch 1, 2) | `0x0500 \| 0` | 2 bytes, signed 1/256 dB |
| FU5 master mute | GET_CUR/SET_CUR | `0x0100 \| 0` | `0x0500` | 1 byte |
| FU6 mic mute / volume | same | `0x0100` / `0x0200` (master, ch 0) | `0x0600` | 1 / 2 bytes |
| GET_MEM (jack/accessory code) | 0x85 (0xA1) | 0 | `0x0000 \| IF0` | 4 bytes, byte 0 used |

* Live render endpoint volume range reported by Creative's driver: −74..0 dB in 1 dB steps; current −24 dB
  (`0xE800`). **Read GET_MIN/MAX/RES at runtime; do not hard-code.** [C]
* The mic endpoint (−96..+6 dB, 1/16 dB) and What U Hear (−96..0 dB) ranges are **Creative software volume nodes
  3/7/10 in the kernel**, not FU6. FU6's own range is [GAP] (read it from the device). [C]
* GET_MEM jack codes 0x0C, 0x17, 0x2D (reported as 0x1A) — but the E1 has no interrupt source that triggers the
  read; jack detection on the E1 is effectively unreachable/unused. Do not implement. [C/I]
* The `0x0300/0x0400` GET_MEM reads seen in the binary belong to the base class and are **overridden to a no-op for
  the E1** (verifier). [C]

### 1.6 HID (MI_03, owned by inbox HidUsb) [C caps; GAP protocol]
* COL01: Consumer page 0x0C usage 0x01. **Input report 9** (2 × 8-bit), **output report 8** (5 × 8-bit).
  Vendor command/response channel; battery level/status almost certainly comes from here [I]. Byte layout [GAP].
* COL02: Telephony page 0x0B, usage 0x05 Headset. Input report 2 with **Phone Mute (0x2F)** button; output report 2
  (mute LED); feature report 6 (1 byte, 0..1). Bound by Windows to `hidtelephonydriver.inf`.
* No HID volume or SBX button. The volume wheel is not HID [C caps / I analog]. (Conflict: system-diagnosis says
  "consumer control (volume wheel)" — that is only the collection name; see §8.)
* The raw 91-byte report descriptor was not read [GAP]; readable with a USB capture or via WinUSB if ever bound.
* Creative's kernel driver also issues HID GET_REPORT/SET_REPORT (feature) over EP0 and sends registry value
  `StartUpHIDCommand` at start. All of this is reachable from user mode with `HidD_*` because HidUsb owns MI_03. [C]

### 1.7 Vendor requests (URB_FUNCTION_VENDOR_OTHER, bmRequestType 0x43 OUT / 0xC3 IN) [C wire / I meaning]
| bReq | Dir | wValue | wIndex | Len | Creative use | Needed by new stack? |
|---|---|---|---|---|---|---|
| 0x01 | IN | 0 | 0 | 32 | device info block (bytes 4.. forwarded to panel) | No (About box only) |
| 0x92 | IN | 0 | 0 | 4 | two 16-bit values, firmware version [I]; reference 0x000B0005 | No |
| 0x2A | IN | 0 | 0 | 2 | status word; bit 0 drives HS/FS policy | No |
| 0x29 | OUT | 1 / 0 | 2000 | 0 | switch to high-speed / back to full-speed, device re-enumerates. Policy: `...\Enum\USB\VID_041E&PID_323B&MI_00\USBMode` (default 1 = stay FS), `...\Control\Creative Tech\USB\ForceUSB` | **No.** Experimental; HS descriptors never captured; Creative still exposes 44.1 kHz only in HS (and forces 16-bit slots) |
| 0x21 | OUT | 0 (pin 0) / (rate≠44100) (pin 8) | 1 | 0 | clock select for the auxiliary render "pin 8" path | No. Research hook: may hint at a vendor-switched 48 kHz clock [I], untested |
| 0x2E | OUT | 0/1 | 0 | 0 | enable flag, forced 1 while pin 8 active | No |
| 0x2B | OUT | caller value | 0 | 0 | followed by 0x2E wValue 0; called at start; "route/input select" [I] | **Maybe** — leading candidate for the **jack retask** (Headphones=2 / Mic-in=3) [GAP] |

Restricted-mode allow-list in Creative's helper decodes to {0x00, 0x01, 0x04, 0x05, 0x06, 0x14–0x17, 0x29, 0x2A};
filtered requests return success silently. [C]

### 1.8 What Creative's kernel driver exposes (for parity) [C]
* AVStream minidriver (no PortCls, no WaveRT) → standard-streaming KS behind `ksthunk`. Consequence: engine period
  fixed at 441 frames (10 ms), min device period 3 ms, no low-latency shared mode.
* KS data ranges (identical W7/W10): render pin 0 = 2 ch, 44100, 16..24 bit (24 = 3-byte container); mic pin 5 =
  1 ch 44100 16-bit; **What U Hear pin 6 = 2 ch 44100 16-bit software loopback**; pin 8 aux render (FS only).
* Format mapper: render 4 ch → 6 ch/0x3F, mono → stereo, bits capped at 24; mic stereo request → mono with
  downmix flag; WUH mono → stereo; capture bits capped at 16.
* WASAPI result (live): exclusive render accepts **only 44.1/16/2 and 44.1/24-packed/2**; 24-in-32, 32-int, 32-float
  rejected (0x88890008). WUH exclusive 44.1/16/2; mic 44.1/16/1. Shared: everything is 44.1 kHz float.
* In-kernel DSP exists (cubic/linear SRC, voice morph, "Silencer" NR, EAX-style reverb, software volume nodes) but
  the E1 render path at 44.1 kHz bypasses the SRC; no dither anywhere. Render bit-exactness is strongly inferred
  but not proven with a known-signal capture [I].
* Private KS sets: {E8E7B1C0-…} (48-byte device-identity / APO licensing handshake, BASICSUPPORT), {12C98F26-…}
  (VoiceFX, float32 params 0..7 + State 99), {5FA7CE37-…} (Silencer NR, float32 params 0..11 + State 99), and ~70
  control sets used by KSUSBSPI. **The main control set differs per build** ({C4BD0511} W7 vs {447845DF} W10, plus
  {761E4C5B} W10): Win7 panel/SPI and Win10 kernel are not interchangeable.
* Creative APO refuses to process (falls back to passthrough) unless the kernel answers the {E8E7B1C0} handshake,
  so **Creative's APO cannot be reused on any non-Creative kernel driver**.

### 1.9 Live endpoint state on this PC (2026-11-14) [C]
| Endpoint | ID | DeviceFormat | FX |
|---|---|---|---|
| Render "Speakers (Sound Blaster E1)", default | `{7e65a67a-04d8-403f-b379-885cdec445d5}` | EXTENSIBLE 2 ch / 44100 / 24-in-24 / mask 0x3; mix 44.1k float | LFX {559493DD}, GFX {2EF9BA5B}, **SFX {E9B73398}, MFX {41528545}**, no EFX; modes DEFAULT only |
| Capture "Microphone", default | `{389c9c75-…}` | 1 ch / 44100 / 16 / mask 0x4 | Creative chain |
| Capture "What U Hear" | `{1d53f1ec-…}` | 2 ch / 44100 / 16 | none |

Form factor of the render endpoint = Speakers (1); "headphone" behaviour comes from SpeakerMode=2 /
HeadphoneOverride. Spatial sound = **Dolby Atmos for Headphones** active. Audio enhancements on. FxProperties ACL:
BUILTIN\Users have SetValue. `DisableProtectedAudioDG = 1`.

---

## 2. Ranked root causes of the Windows 11 quality loss

| Rank | Cause | Evidence | Confidence | Fix in new stack |
|---|---|---|---|---|
| 1 | **Dolby Atmos for Headphones** is the active spatial format on the E1 render endpoint (7.1.4 beds, 128 objects). It virtualizes every stream before the device. Did not exist on Win7. | WinRT `SpatialAudioDeviceConfiguration`: Active = Default = {1459AC38-…}; `ISpatialAudioClient` active; Dolby Access 3.27 installed | [C] | Panel detects it and offers to turn it off (user setting, Settings > Sound > Spatial audio). Ship with a "no spatial" recommendation. |
| 2 | **Creative APO runs with virtualizer defaults**: SBX Surround ON (0.40, headphone HRTF path because SpeakerMode=2), Crystalizer ON (0.65). Stacked on #1 → double binauralization + transient/shelf EQ. The SFX stage (MatrixEncoder) also runs (it is **not** a memcpy). | FxProperties values = INF defaults; `IAudioEffectsManager` lists VIRTUAL_SURROUND/HEADPHONES, ROOM_CORRECTION, SPEAKER_PROTECTION (GUIDs only in KSUSBAPO64 1.2.47) → APO loaded | [C] values, [strong I] loaded | New APO defaults to **transparent**; every effect user-switchable; IAudioSystemEffects3 so Windows' Enhancements toggle works. |
| 3 | **The user's tuning was wiped.** Storage Sense removed the device nodes on 2026-08-26; endpoints re-created 2026-11-14 20:56 with INF defaults (+ Windows' default spatial format). | setupapi logs, endpoint creation timestamp | [C] events, [I] impact | Panel persists settings outside the endpoint (profile XML + per-device store) and re-applies after re-enumeration. |
| 4 | **Win8.1+ path lost SpeakerEQ.** Win7 ran GFX profile 101 (…BassMgmt → **SpeakerEQ** → BandCombine…); Win8.1/10/11 run MFX profile 102 without it, even though `{42F4281E},0` = TRUE. | Profile tables dumped in both builds; live FX list has no EFX | [C] chain, [I] audible, SpeakerEQ curve [GAP] | Provide an optional "device tuning EQ" stage; fit its curve by measurement against Win7 behaviour if a Win7 reference is ever available (§7). |
| 5 | **Shared-mode SRC**: all 48 kHz content is resampled to 44.1 kHz by the Windows engine. Same on Win7, so not a regression, but a quality lever. | Only 44100 Hz in descriptors; WASAPI shared rejects other rates | [C] | Unavoidable in hardware. Recommend 44.1 kHz sources / exclusive mode for music; APO never adds a second SRC. |
| 6 | **Narrow exclusive-mode format list** (16 and 24-packed only). Bit-perfect players asking for 24-in-32 / 32-bit fall back to 16-bit or shared. Win7 users had the ASIO DLL in the driver INF; on Win10 it moved to an extras installer (still installed here, but its default 48 kHz gives ASE_NoClock until set to 44.1). | WASAPI probe; ASIO RE | [C] | Option B only: expose 16/24p/24-in-32/32i/32f and pack to 3 bytes. In Option A: document exclusive 44.1/24-packed. |
| 7 | **Inbox `usbaudio.sys` may not handle the async feedback EP correctly** → periodic clicks/drift. Would explain "the normal Windows drivers are very bad" if the user tried them. | Not tested | [I] | Test before choosing the inbox transport (§7 T2). |
| 8 | Minor: communications ducking default (HID telephony collection present), CrystalVoice NR on by default on the mic. | Registry | [C] | Panel exposes them. |

**Ruled out** [C]: lower sample rate or bit depth (Win11 streams 24/44.1 via alt2 with feedback), kernel DSP on the
render path (SRC bypassed at 1:1), different INF default format (identical blob), different APO math (all W7
constants present in W10, identical chains/gates/ranges), the >48 kHz effect bypass (endpoint runs at 44.1 kHz),
hardware volume (FU5 at −24 dB on a 24-bit path costs nothing audible).

---

## 3. Original feature list and DSP chain

### 3.1 Feature inventory exposed to the panel for the E1 (live, Win10 stack) [C]
Render endpoint:
| SoundCore featureId | Feature | Params (pid: range, step, default) |
|---|---|---|
| 0x60000001 | Effect master | 0 supported mask = 0x2F; 1 enabled mask; **2 SBX master on/off**; 6 Scout Mode on/off |
| 0x10000001 | SBX Surround (CMSS-3D) | 0 enable (def 1); 1 Immersion 0..1, 0.01, **0.40** |
| 0x10000008 | Crystalizer | 0 enable (def 1); 1 Level 0..1, 0.01, **0.65** |
| 0x10000020 | Bass (X-Bass) | 0 enable (def 0); 1 Strength **0..100**, 1, 50; 2 Crossover **10..1000 Hz**, 1, 80 |
| 0x10000004 | Smart Volume | 0 enable (0); 1 Strength 0..1, 0.74; 2 Mode 0 Normal / 1 Loud / 2 Night |
| 0x10000002 | Dialog Plus | 0 enable (0); 1 Strength 0..1, 0.5 |
| 0x10000010 | Graphic EQ | 0 enable (0); 1 Preamp −12..+12 dB, 0.01; 2..11 bands −24..+24 dB, 0.01; 256 EnumPreset; 258 ActivePreset; 259 Save; 260 Remove |
| 0x01000002 | Speaker config | 0 SpeakerConfig: 0x80000000 Headphones / 0x00003003 Stereo 2.0/2.1; 1 physical (3 = FL+FR); 3 enum |
| 0x01000001 | Multiplex output (jack retask) | 14 enum {2, 3}; **15: 2 = Headphones, 3 = Mic-in** |
| 0x70000020 | Battery | 0 info {max 100, min 0, step 1}; 1 level; 2 status (1 low, 2 charging) |

Mask bits: CMSS3D 0x01, DialogPlus 0x02, SVM 0x04, Crystalizer 0x08, GraphicEQ 0x10, XBass 0x20, BassMgmt 0x40,
SpeakerEnhancement 0x80 (supported mask 0x2F excludes the EQ, so the SBX master does not switch the EQ).

Capture endpoint:
| featureId | Feature | Params |
|---|---|---|
| 0x60000001 | CrystalVoice master | 3 supported mask 0x15; 4 enabled mask; **5 master on/off** |
| 0x20000004 | Noise Reduction | 0 enable (def 1); strength not exposed |
| 0x20000010 | Mic Smart Volume | 0 enable; 1 Strength 0..1, 0.74 |
| 0x20000001 | VoiceFX | 0 enable; 1 F1 200..800 Hz (400); 2 F2 800..1800 (1400); 3 F3 1600..2750 (2000); 4 PitchMod 0.25..4 (1); 5 EnvelopeMod 0.5..2 (1); 6 PitchVariability −1..5 (1); 7 Quiver 0..0.2 (0); 8 Contour 0..0.4 (0); 256/258 presets (18) |

Not present on the E1: AEC, mic array/focus, mic EQ, bass management, speaker EQ UI, headphone gain, EAX, Dolby,
Direct Mode, SPDIF, EffectsProfile hardware profiles. Win7's static SndCrUSB table has different fallback ranges
(X-Bass 0..1, EQ step 1, CMSS default 0.67); profiles are stored normalised so both work. Win7 live ranges [GAP].

### 3.2 Creative's processing graph (both builds) [C]
Render, Windows 7 (LFX + GFX): LFX = profile 37 (MatrixEncoder), GFX = profile 101.
Render, Windows 8.1–11 (SFX + MFX): SFX = profile 37 (MatrixEncoder), MFX = profile **102**.
```
profile 101: deinterleave → BandSplit → CMSS3D(mode 1){Surround, SmartVolume, DialogPlus}
             → Reverb(mode 1) → THX Crystalizer → GraphicEQ → BassMgmt(X-Bass)
             → SpeakerEQ → BandCombine → Calibrator → THX Limiter → interleave
profile 102: same without SpeakerEQ
mic (profile 103): BandSplit → STFT → NoiseReduction → ISTFT → VoiceFX → THX SVM → BandCombine
```
Device id gating: chain chosen by (container kind, device id, form factor); the E1 maps to id 67 only after the
{E8E7B1C0} handshake.

### 3.3 Engine conventions [C]
* Float32 only (interleaved from the engine; planar inside, up to 8 channels). Only AC-3-over-S/PDIF passes through.
* Container chunk 448 frames @44.1 k / 480 @48 k / fs/100, but **modules run in blocks of LCM(module granularity)
  = 128 frames at 44.1/48 kHz** (256 at 96 kHz); the start module FIFO-rebuffers, adding 128/fs ≈ 2.9 ms latency.
  All per-block time constants assume 128-frame blocks.
* Start-up: dry→wet linear crossfade over a configurable prime time (GFX/MFX only) and optional zero-fill.
* Rate gate on CMSS3D, Crystalizer, BassMgmt, Calibrator, Limiter, SpeakerEQ, MatrixEncoder: process only if
  44.1 kHz ≤ fs ≤ 192 kHz and (fs ≤ 48 kHz or hidden key {DBD7BD1B…},0). GraphicEQ and Reverb are not gated.
  Hi-res mode decimates to a 48 kHz base band via BandSplit/BandCombine. Not relevant for the E1 (44.1 kHz).
* Biquad: Direct Form I, coefficient order {a1, a2, b0, b1, b2}, `y = b0·x + b1·x[n−1] + b2·x[n−2] + a1·y[n−1] +
  a2·y[n−2]` (feedback **added**, so a1/a2 are negated textbook values); state stored {x[n−2], x[n−1], y[n−2],
  y[n−1]}; 1e−10 anti-denormal offset. First-order: {a1, b0, b1}. Click-free changes by per-sample linear
  coefficient ramps.
* dB→linear 10^(x/20); one-pole smoothing coefficient from a time constant.
* Channel layout index from the channel mask; SpeakerMode=2 or HeadphoneOverride forces the headphone layout.

### 3.4 SBX Surround (CMSS-3D engine A) — [C structure, GAP in parts]
* Params: enable, second enable flag, Immersion 0..1 (scaled ×0.6 internally, ramped), output/input config.
* Stage 1, **upmix** stereo → centre/surround feeds [GAP: matrix of the 6.8 KB upmix routine not decoded].
* Centre shaping: two biquads designed from 8 kHz and 2 kHz corner constants (presence/low-pass pair) [C form / I use].
* **Rear decorrelator**: one Schroeder all-pass **per surround feed, in parallel** (feeds 5..8), delays **4.7, 3.7,
  6.5, 7.0 ms**, g = 0.465, section `v = g·x + buf; buf = x − g·v; y = v`; two **13 ms** delays on the first pair;
  dry/wet = √f / √(1−f) with f per layout (1.0 for configs 0–4).
* Output mix: 7-out × 6-in weight matrix [GAP: weights not tabulated in the notes].
* **Virtualizer**: headphones (config 0) → binaural with 3 HRTF FIR pairs (ipsi/contra), **59 taps @44.1 kHz /
  64 @48 kHz**, minimum-phase (peak at tap 0), plus integer delay lines of **31 samples (~0.70 ms) headphone /
  113 samples (~2.56 ms) speaker** (ITD by inference); speakers (1..4) → stereo crosstalk virtualizer with shaping
  biquads; others → plain downmix with 1.0 / 0.7071 folding.
* Re-implementation: public HRTF set (e.g. MIT KEMAR or SADIE II) truncated to 64 taps, minimum-phase, ITD as
  ~0.7 ms delay; shuffler/crosstalk-cancel virtualizer for speakers. Immersion = wet/dry 0..0.6.

### 3.5 Smart Volume and Dialog Plus (engine B) [C unless noted]
* Detector: 64-sample analysis frames, **96-sample look-ahead** on all channels (32/48 at ≤ 24 kHz). Weighting =
  BS.1770 RLB high-pass (48 kHz: {1.99004745, −0.99007225, 1, −2, 1}) **followed by a custom low/high tilt** (+6.6 dB
  @20 Hz, 0 dB @1–2 kHz, +2.2 dB @16 kHz) — not K-weighting. Per-rate scale (44.1 k: 1.3047).
* Level → curve table `{level: threshold dB, range dB}`: 0 → (−52, 0), 0.5 → (−52, 25), 0.74 → (−52, 37), 1.0 →
  (−52, 50), linear interpolation. Floor −100 dB. Hold 15 ms.
* Two init presets: mode 0 {ratio 8, attack 40 ms, release 100 ms, slow 4000 ms, …}; mode 1 {ratio 24, 40/200/1000 ms,
  …}. Engine param p4 ∈ [0,2] from PKEY {E6EC3743} = the panel's Normal/Loud/Night [I]. **[GAP]: exact mapping of
  the three UI modes onto the two presets, and the meaning of engine params p10/p11 ({402F0D82}, {D0A3B741}).**
* Dialog Plus level → {a, b, c} dB: 0 → (0,0,0), 0.3 → (+6,−6,0), 0.5 → (+9,−9,0), 0.8 → (+12,−12,+3), 1.0 →
  (+12,−12,+6). Proposed: a = dialogue (centre/mid) gain, b = ambience (side) gain, c = presence EQ [I].
  **[GAP]: signal topology (mid/side vs centre extraction).**
* Engine C (final up/down-mix and output gain) [GAP].

### 3.6 Crystalizer [C]
* Params: enable, Level 0..1 (0.65), pre-attenuation 0.01..1 linear (1.0). Works in **32-sample frames**, parameter
  smoothing over 16 frames.
* Analysis: mono sum → LF band biquad (T1, 48 k: {1.88129497, −0.888609052, 0.192152217, −0.369676381,
  0.192152217}) and HF band biquad (T2, 48 k: {−0.328228623, −0.728210032, 0.110678248, −0.221356496,
  0.110678248}); per-rate tables for 22.05/24/32/44.1/48 kHz.
* Detector: peak envelope (attack 0.01, release 0.001 per frame, floor 0.03), piecewise-linear dB-like map (knots
  0.1, 1.9, 20, 131), 5-frame peak memory decaying 0.975; excess over threshold × sensitivity (cap 0.5) → LF and HF
  boost amounts with separate smoothing.
* Level map: `x = L ≤ 0.5 ? 0.8·L : 0.4 + 1.2·(L − 0.5)`, interpolated across a 4-row table
  (gain, two per-rate tilt coefs, LF/HF amounts, LF/HF smoothing, sensitivity, threshold, static LF dB, static HF dB):
  row 0 {1.0417, …, 0/0 dB}, row 1 {1.10, …, 3.333/2.800 dB}, row 2 {1.10, …, 4.133/3.600 dB}, row 3 {1.00, …,
  5.200/5.466 dB} (full table in `re/apo-dsp.md` §5.2).
* Path per channel: `y = staticShelfPair( dynamicFirstOrderTilt(x) ) · gain · preAtten`; LFE gain-only.
  In short: a transient-driven dynamic LF/HF shelving expander plus a level-dependent static loudness tilt.

### 3.7 Graphic EQ [C]
* 10 bands at **31.25, 62.5, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz**, bandwidth 0.7071·fc (one octave).
* Band 0 low-shelf (A = 10^(g/40), BW ≤ 4.172·fc), bands 1–8 peaking (Regalia–Mitra constant-Q:
  `t = tan(BW/2)/√G`, `k = (t−1)/(t+1)`, a2 = k, a1 = (1−k)·cos ω0, b0 = (1+G)/2 − (1−G)/2·k, b1 = −(1−k)·cos ω0,
  b2 = (1−G)/2 − (1+G)/2·k; 0 dB → unity), band 9 high-shelf.
* Ranges: preamp ±12 dB (folded into section 0's b-coefficients, not a separate gain), gains ±24 dB, centre 20 Hz..
  min(24 kHz, fs/2), bandwidth 1..12000 Hz. Optional auto-normalisation from the product of Nyquist gains.
* Changes ramp **one section at a time, ≈10 ms each** (`round(fs·0.01/block)` blocks).
* Presets (dB, 31→16k): Flat 0×10; Acoustic 0,1,2,0,0,0,0,2,2,2; Classical 0,6,6,3,0,0,0,0,3,3; Country
  −1,0,1,1,1,0,0,2,3,4; Dance −1,2,3,4,−1,−1,0,0,4,4; Jazz 0,0,1,4,4,4,0,1,3,3; New Age 0,2,2,0,0,0,1,2,2,2; Pop
  −2,0,2,2,0,−1,−1,0,3,6; Rock −1,−1,1,2,−1,−1,0,0,4,4; Vocal −2,−1,−1,0,3,4,3,0,0,1. Scout Mode EQ:
  +2,+3,+3,−2,−2,0,+3,+4,+3,+2 (+ surround tweak Scout2 = 0.9).

### 3.8 Bass (X-Bass), Limiter, others
* **Bass** [C constants / I exact flow]: RBJ Butterworth (Q = 0.70710678) LPF/HPF crossover at fc (default 80 Hz,
  10..1000); enhancer centre fb = 0.875·fc (≥ 40 Hz, ≤ fc − 25 Hz), gain factor 0.67 + 0.0005·fb clamped to
  [0.77, 0.87], band-limiting filters around fb ± 20 Hz with caps 0.35·fc and 420 Hz, polynomial-in-log makeup;
  smoothing 50 ms / 1000 ms. Strength 0..100 (UI) ↔ engine 0..1 (default 0.5) [I /100 mapping]. **[GAP]: harmonic
  generation vs pure peaking boost.**
* **THX Limiter** [C]: look-ahead peak limiter, peak sub-block 4/8/16/32 samples for fs ≤ 24k/≤ 48k/≤ 96k/above,
  look-ahead 9 sub-blocks (72 samples ≈ 1.6 ms @44.1k), gain computed in dB per sub-block, interpolated per sample,
  release 250 ms, constant −4.5 dB (threshold/knee [I]), minimum gain 0.5 [I].
* **Reverb(mode 1)**: EAX-like 24-parameter set, no preset enabled for E1 playback → bypass [I]. Skip.
* **Calibrator**: per-speaker trims/delays, neutral for stereo [I]. Skip (or unity).
* **SpeakerEQ**: Win7-only tuning EQ [GAP: tables undecoded; ~6 KB process code].
* **MatrixEncoder (SFX)**: runs on Win11 for stereo; effect on stereo [GAP]; treat as identity, verify by measurement.
* **BandSplit/BandCombine**: only for fs > 48 kHz; not needed.

### 3.9 Mic chain (CrystalVoice) [C names / I algorithms]
* **Noise reduction** ("Silencer", STFT domain): params AdaptationStep, Selectivity, FastSmoothFactor,
  SlowSmoothFactor, MaxAttenuation, VoiceEQ Low/High/Full gain, Low/Mid/High over-estimation factor, FilterLimit
  (float32) → an adaptive spectral-subtraction/Wiener suppressor with per-band over-subtraction and a gain floor [I].
  Values/defaults [GAP]. Panel exposes on/off only.
* **VoiceFX**: three formant frequencies, pitch factor, formant (envelope) factor, pitch variability, quiver depth,
  contour depth → pitch shifting + spectral-envelope warping [I]; 18 presets with exact values in
  `re/control-panel.md` §4.3 (Neutral, Female→Male, Male→Female, Scrappy Kid, Elderly, Orc, Elf, Dwarf, Alien Brute,
  Alien Infiltrator, Robot, Marine, Emo, Unstable, From Up North, Deep Voice, Munchkin, Demon).
* **Mic Smart Volume**: same engine family as §3.5, default 0.74.

### 3.10 Parameter keys (Creative FxProperties PKEYs; keep for optional compatibility) [C unless noted]
| PKEY | Type | Meaning |
|---|---|---|
| {5B4777A4-8AD4-4D34-893A-DF34DA0E56CA},0 / {A5A78EA4-C156-4DB7-85AA-81CFF1C3F192},0 | BOOL / R4 | Surround enable / immersion |
| {3CD83C04-868F-4F08-8D75-B4625FFE3B31},0 / {0F03F0BB-72C7-4EC1-8422-7B8D7410694A},0 / {3C663165-48FC-41D8-8120-81CB0B12B230},0 | BOOL / R4 / R4 | Crystalizer enable / level / pre-attenuation |
| {9AD782D7-…},0 / {80B0C7BB-…},0 / {E6EC3743-…},0 | BOOL / R4 / R4 | Smart Volume enable / level / mode [I] |
| {EA3137F9-…},0 / {A79717E9-…},0 | BOOL / R4 | Dialog Plus enable / level [I] |
| **{9A9D0CB2-4DC9-494C-8210-9848AE1AA629},0** / **{DDCF8D90-DE27-4DE4-AF57-088B8AD78FDF},0** | **BOOL / R4** | **EQ enable / preamp dB** (verifier swap) |
| {2B88C76D-…},0..9 / {D24674DF-…},0..9 / {02939409-…},0..9 | R4 | EQ band gain / centre / bandwidth |
| {F67CF426-…},0 / {0519A199-…},0 / {3F23DBC5-…},2 / {DD527E35-…},0 | BOOL / BOOL / R4 / R4 | X-Bass enable / for 2.0 / crossover 80 / strength 50 |
| {59FBB5C6-4C98-4741-ADC0-40ABB474A963},0 / {E8D2D16C-A666-44A0-BB77-3051F7384D22},0 | UI4 / BOOL | Speaker mode (0 ext, 1 integrated, 2 headphone) / headphone override |
| {156AC5B7-384A-4B15-A3A4-79350563FC88},0..5,10..20 | mixed | Scout Mode block |
| {42F4281E-…},0 | BOOL | SpeakerEQ select (unused on Win11) |
| {DBD7BD1B-AF04-4960-A346-14E5B58222C2},0 | BOOL | allow processing above 48 kHz |
| {40D0D021-20BD-4D15-A93C-1DBE8922C642},1 | BOOL | mic NR enable |

Our own APO should use **its own PKEY GUID namespace** by default; mirroring these keys is optional and only makes
sense if we ever want SBE.exe to drive our APO (untested [I]).

---

## 4. Control-panel UI spec (summary of `re/control-panel.md`)

* **Window**: borderless, 882 × 637, black background with #191919/#282828 panels; title bar 36 px (logo, endpoint
  name, battery icon 26×12, menu/minimize/close 44×31); left navigation 292 px; content frame 564 × 575 with 0.5 s
  cross-fade + 0.85→1 scale page transitions. Tahoma; text #AFAFAF, hover/selected white, disabled #464646; accent red;
  slider fill #880709 on #323232, thumb 26×30; power toggle 42×42 ON #9D0003 / OFF #464545; buttons 110×40 #262626,
  hover #CB0202; custom presets shown in orange.
* **Navigation (E1, in order)**: product tile "PRO STUDIO" (292×72), **SBX Pro Studio, CrystalVoice, Scout Mode,
  Speakers/Headphones, Mixer, Equalizer, Advanced Features**; bottom: Profile button + current profile name; master
  volume strip (mute + slider bound to endpoint volume).
* **Menu**: Options…, Start with Windows (check), Restore Default…, Audio Device Selection…, Language…, Software
  Update…, About.
* **SBX Pro Studio**: master power (feature 0x60000001/2) + demo player; rows with check box + slider: Surround,
  Crystalizer, Bass (+ Crossover Hz slider), Smart Volume (+ Normal/Loud/Night radios), Dialog Plus. Sliders take
  min/max/default from the provider; percentage labels.
* **Equalizer**: EQ check box, preset combo, Save (name ≤ 32 chars, overwrite confirm) / Delete (custom only); 10
  vertical dB sliders labelled 31…16k (±24 axis) + LEVEL preamp (±12); moving a band turns the preset "custom".
* **CrystalVoice** (capture): device header + "Set as default"; Mic Recording Volume (mute + slider); Mic Boost /
  AGC only if the node exists; Mic Monitoring Volume (hardware path or software monitor fallback); master power;
  VoiceFX (check + preset combo + Add/Delete), Smart Volume (check + slider), Noise Reduction (check); Test button.
* **Scout Mode**: on/off + global hot keys (turn on / turn off) via RegisterHotKey; only active while the panel or
  helper runs.
* **Speakers/Headphones**: combo Headphones (0x80000000) / Stereo 2.0/2.1 (0x00003003); speaker test per channel.
* **Mixer**: playback master + monitor levels; recording master, mic boost/AGC, per-channel balance, listen.
* **Advanced Features**: "Configure Headphones/Microphone jack as": Headphones (2) / Microphone (3).
* **Profiles**: avatar, combo, Save/Delete/Import/Export (`.xml`); factory profiles Adventure & Action, Driving
  Simulation, First Person Shooter, Real Time Strategy (values in `re/control-panel.md` §6); XML schema
  `<profile><info/><settings><surround/><crystalizer/><bass/><svm/><dlgplus/><voicefx/><vip_svm/><noise_reduction/>
  <graphic_eq/></settings></profile>`; float levels ≤ 1 are scaled by the live range (Bass is normalised).
* **Battery**: title-bar icon, level 0..100, low/charging status (from HID COL01, layout [GAP]).
* New-stack additions (not in original, needed for quality): spatial-audio warning/toggle, "Direct (bit-exact)"
  indicator, device format display (44.1 kHz/24-bit), effects-on-RAW information.
* **Assets**: Creative's skin PNGs, product images, demo clip and "SBX/THX/Sound Blaster/CrystalVoice" marks must not
  be redistributed; recreate the layout and behaviour with our own artwork and neutral names (e.g. "Surround",
  "Clarity", "Bass", "Smart Volume", "Dialog", "Voice").

---

## 5. Kernel driver vs user-mode APO responsibilities

| Function | Kernel transport (Creative ksUSBaud / inbox usbaudio / our ACX driver) | User-mode APO (audiodg) | Panel / helper (user) |
|---|---|---|---|
| USB config, alt-setting (IF1 alt2 24-bit, IF2 alt1) | ✔ | | |
| Isochronous OUT with feedback-driven 44/45-frame packets; feedback EP 0x83 polling every 32 ms | ✔ (mandatory) | | |
| Isochronous IN mic, mono↔stereo mapping | ✔ | | |
| Exposed PCM formats & container conversion (16 / 24p / 24-in-32 / 32i / 32f → 3-byte) | ✔ (only our own driver can widen the list) | | |
| FU5/FU6 volume & mute (class requests), endpoint volume ranges from GET_MIN/MAX/RES | ✔ | | |
| Position/clock reporting, WaveRT/ACX buffers, low latency | ✔ | | |
| What U Hear as a capture endpoint (post-mix loopback copy) | ✔ (only kernel can create an endpoint) | | WASAPI loopback is the user-mode substitute |
| Vendor requests (jack retask [GAP], info) | ✔ or via Creative's KS properties | | requests through the transport |
| Signal-processing mode declaration (RAW + DEFAULT) | ✔ (driver/INF) | | |
| SBX Surround, Crystalizer, X-Bass, Smart Volume, Dialog Plus, 10-band EQ, Limiter, tuning EQ | ✘ (no DSP in kernel) | ✔ MFX on render | parameters |
| CrystalVoice NR, VoiceFX, Mic SVM | ✘ | ✔ MFX/SFX on capture | parameters |
| Bit-exact passthrough when effects are off; IAudioSystemEffects3 list for Windows' Enhancements toggle | | ✔ | |
| Parameter storage/notification | | reads FX property store; `APO_NOTIFICATION_TYPE_SYSTEM_EFFECTS_PROPERTY_CHANGE` | writes via `IAudioSystemEffectsPropertyStore` (Win11 SDK: `mmdeviceapi.h`) [C SDK] |
| Battery/status (HID COL01 reports 8/9), mic-mute (COL02) | inbox HidUsb | | ✔ `HidD_*` / ReadFile |
| Scout hot keys, profiles, presets, spatial-audio state, re-registration after re-enumeration | | | ✔ |
| Device authentication / licensing / disk fingerprinting (Creative) | ✘ drop | ✘ drop | ✘ drop |

---

## 6. Recommended Windows 11 architecture

### 6.1 Constraints on this machine [C]
Secure Boot **ON**; test signing **OFF** (cannot be enabled while Secure Boot is on); HVCI/Memory Integrity OFF; VBS
OFF; vulnerable-driver blocklist ON; BitLocker (FVEBOOT) present → firmware/BCD changes trigger recovery-key prompts;
`DisableProtectedAudioDG = 1` (an unsigned/self-signed APO can load into audiodg); toolchain = MSVC 2022 + SDK
10.0.26100 **without WDK** (the SDK does ship `audioengineextensionapo.h` with IAudioSystemEffects3 /
IAudioProcessingObjectNotifications, `mmdeviceapi.h` with IAudioSystemEffectsPropertyStore, and
`baseaudioprocessingobject.h` + `audiobaseprocessingobject.lib`). The Creative WHQL kernel driver loads today; an
unsigned kernel driver will not.

### 6.2 Options

| | **A. User-mode stack on a signed kernel transport** | **B. New KMDF/ACX kernel driver in C** | **C. Hybrid: inbox driver + small filter** |
|---|---|---|---|
| What we write | C APO DLL (render + capture), C control panel, C helper/tray, installer | everything in A **plus** an ACX (KMDF) USB audio driver for MI_00 | everything in A plus a KMDF upper/lower filter on MI_00 that only forwards vendor requests from a private IOCTL |
| Kernel transport | A1: Creative `ksUSBaud.sys` 6.1.13 (WHQL, installed, known-good 24-bit async + WUH); A2: inbox `usbaudio.sys` (UAC1) | ours | inbox `usbaudio.sys` |
| Loads on this PC today | **Yes** | **No** — needs Secure Boot off + `bcdedit /set testsigning on` (BitLocker key), or attestation signing (Partner Center + EV certificate) | **No** (same signing problem, smaller code) |
| Fixes root causes #1–#3 | Yes | Yes | Yes |
| #4 (tuning EQ) | Yes (APO stage) | Yes | Yes |
| #6 exclusive formats (24-in-32, 32f), low-latency shared mode, RAW mode | No (limited by transport) | **Yes** | No |
| What U Hear endpoint | A1 yes (Creative's, 16-bit); A2 no (use WASAPI loopback) | Yes (can be 24-bit/float) | No |
| Jack retask (Headphones/Mic) | A1 via Creative KS property [GAP]; A2 no | Yes once the request is known | Yes once known |
| Battery, mic-mute | Yes (HID, user mode) | Yes | Yes |
| Risk | Low: user-mode crash only kills audiodg stream; easy rollback | High: BSOD risk, isoch timing, power management, months of work, must stay HVCI-clean | Medium |
| Dependence on Creative binaries | A1 yes (closed 2015 driver; HVCI compatibility unknown); A2 none | none | none |
| Tooling | have it | needs WDK (free download) | needs WDK |

### 6.3 Recommendation
1. **Phase 1 — Option A (now).** Build, in C:
   * `e1fx.dll` — APO implemented directly in C COM (or a thin shim over `CBaseAudioProcessingObject`):
     IAudioProcessingObject/RT/Configuration, IAudioSystemEffects2/**3**, IAudioProcessingObjectNotifications.
     Render: register as **MFX** (post-mix, DEFAULT mode) only — no SFX (avoids per-stream cost and Creative's
     MatrixEncoder), no EFX (so RAW streams, if the transport declares RAW, stay untouched). Capture: MFX with
     NR/VoiceFX/Mic SVM. Float32, internal block 128 frames (match original time constants) with FIFO re-blocking,
     double-precision biquad state (or TPT/SVF) for low-frequency bands, FTZ/DAZ, parameter ramps, **exact memcpy
     when every effect is off**, no 48 kHz gate, no licensing.
   * `libe1dsp` — pure C DSP library (no Windows headers) with an offline WAV test harness.
   * `e1panel.exe` — native Win32 C UI reproducing §4 (Direct2D/GDI), writing parameters through
     `IAudioSystemEffectsPropertyStore` (user store), profiles XML compatible with Creative's schema.
   * `e1helper.exe` — per-user tray: HID battery/status, Scout hot keys, spatial-audio check, re-applies FX
     registration (via an elevated scheduled task) when the endpoint is re-created.
   * Installer (admin): register COM/APO (`HKCR\AudioEngine\AudioProcessingObjects`), write the endpoint FX keys
     (`PKEY_FX_ModeEffectClsid` {D04E05A6…},6 on render and capture; clear LFX/GFX/SFX Creative CLSIDs), keep a backup
     for rollback.
   * Start on **A1** (Creative kernel driver kept, Creative APO and SBE.exe disabled): zero kernel change, keeps WUH
     and the known-good async path. Then run test T2 (§7) on **A2**; if the inbox driver handles feedback correctly the
     user may drop all Creative binaries (losing WUH and, until resolved, jack retask).
2. **Phase 2 — Option B (only if the user accepts the signing cost).** KMDF + ACX driver for MI_00 in C:
   isochronous OUT/IN via KMDF isoch URBs, explicit-feedback accumulator (§1.4), FU5/FU6 elements, WaveRT-style
   buffers, formats 16/24p/24-in-32/32i/32f converted to 3-byte, RAW + DEFAULT modes, 24-bit What U Hear loopback,
   vendor requests behind a private KS property, no in-kernel DSP, no silent 24→16 fallback, HVCI-compliant
   (no executable pool, no RWX). Development requires disabling Secure Boot and enabling test signing (with the
   BitLocker recovery key at hand) or attestation signing for real deployment. Keep Creative's INF installed as a
   fallback; switch with `pnputil`/Device Manager.
3. **Option C** only if: the inbox driver is chosen **and** jack retask is required **and** it proves to need a
   vendor request. Before writing a filter, test the no-kernel route: bind WinUSB to MI_03 with an INF-only package
   (inbox `winusb.sys`, self-signed catalog, libwdi-style) and send the request with `WinUsb_ControlTransfer`
   (bmRequestType 0x43). That route works under Secure Boot because only Microsoft-signed binaries load [I], but it
   replaces HidUsb on MI_03, so the helper must then read EP 0x84 and implement battery/mute itself, and Windows'
   telephony mute integration is lost.

### 6.4 Default configuration for "maximum quality"
Endpoint format 44.1 kHz / 24-bit (alt2); FU5 hardware volume; spatial sound **off**; all effects present but **off**
on first run (bit-exact passthrough) with one-click "SBX" presets; Windows Enhancements toggle maps to the APO's
master; exclusive-mode players at 44.1/24-packed (or ASIO at 44.1 if the user keeps Creative A1); mic 44.1/16 mono with
NR off by default (user choice).

### 6.5 Which vendor-specific USB requests are needed, and can they be sent without our own kernel driver?
* **Audio streaming, quality, volume/mute, mic: none.** Everything is standard UAC1 (§1.3–1.5).
* **Battery, status, mic-mute LED/button: none** — they are HID reports on MI_03, reachable from user mode with
  `HidD_SetOutputReport`/`ReadFile`/`HidD_GetFeature` while HidUsb owns the interface.
* **Jack retask (Headphones ↔ Mic-in): unknown.** Candidate vendor 0x2B (+0x2E) [I], or a HID output report 8
  command [I]. Must be resolved (T4). If it is a vendor request:
  - with Creative's kernel driver (A1): send it **indirectly** through Creative's KS property that KSUSBSPI uses — no
    new kernel code, needs the property id/payload (open item);
  - with the inbox driver (A2): **not possible** from user mode while usbaudio.sys owns MI_00 and HidUsb owns MI_03;
    possible via WinUSB-on-MI_03 [I] or an Option C filter / Option B driver.
  - If it is a HID report: user mode, no kernel at all.
* **Firmware info (0x01, 0x92), status (0x2A): optional**, same reachability as above.
* **HS switch (0x29) and aux clock (0x21/0x2E/0x2B): not needed; research only**, never in production without the HS
  descriptor set captured (device re-enumerates).

---

## 7. Verification and test plan
| # | Test | Purpose | Notes |
|---|---|---|---|
| T1 | Capture Creative's processed output via the **What U Hear** endpoint (kernel loopback, therefore post-APO) while playing sweeps/noise/transients with each effect on alone | Reference curves for Surround, Crystalizer, EQ, Bass, SVM, Dialog Plus and MatrixEncoder | 16-bit, adequate for curve matching; read-only on the system |
| T2 | Bind inbox `usbaudio.sys` (user decision), play audio, list open pipes with the hub IOCTL (`scratchpad\verify_k\pipes.py` pattern); expect EP01 + **EP83**; 30-min 1 kHz sine for clicks | Decide A1 vs A2 | needs a driver switch; reversible |
| T3 | Bit-exactness: play a known 24-bit pattern in exclusive mode on A1 and compare a USB capture (or WUH at 16-bit for the top bits) | Prove the kernel render path is transparent | USB capture needs USBPcap (system change, user decision) |
| T4 | Find the jack-retask mechanism: static trace of the KSUSBSPI MultiplexOutput property → ksusbaud handler → URB, or a USB capture while toggling it in SBE.exe | Decide whether any vendor request is needed | determines Option C relevance |
| T5 | Decode HID COL01 reports 8/9 (static RE of CTHIDRpA/CTHID, or capture) | Battery/status in the helper | |
| T6 | APO A/B: our chain vs T1 references, null tests per module, CPU < 2 % at 44.1 kHz | Parity | |
| T7 | After Phase 2: WASAPI matrix re-run (expect 16/24p/24-in-32/32i/32f exclusive), latency, 24-hour soak, Driver Verifier | B acceptance | test-signed machine |

## 8. Open items and documentation errata

Open [GAP]: CMSS upmix matrix and 7×6 output weights; Smart Volume mode mapping and params p10/p11; Dialog Plus
topology; engine C; Bass enhancer flow; SpeakerEQ curve; MatrixEncoder effect on stereo; NR/VoiceFX algorithm
details and defaults; HID report layouts and raw report descriptor; jack-retask mechanism; meaning of vendor
0x01/0x21/0x2B/0x2E/0x92 and 0x2A bits; HS-mode descriptors; whether inbox usbaudio.sys honours EP 0x83; which
module crashed audiodg on 2026-10-20 (WER needs admin); whether Dolby Access sets Atmos as default for new endpoints.

Errata between notes (this spec follows the corrected value):
1. `system-diagnosis.md` §1 row 5 / §7: "Win10 package has no ASIO" — wrong; it ships in `extras\Audio\CtUsASIO`
   (1.02.00, DLL 1.1.1.0) and is installed here.
2. `plumbing-ipc.md` §0.2, §6, §11.1: "expose 44.1/48/96 kHz because ASIO supports it" — wrong; ASIO's rate table is
   not a capability check and the E1 offers 44.1 kHz only.
3. `apo-dsp.md` §2.2: EQ keys swapped ({9A9D0CB2} = enable BOOL, {DDCF8D90} = preamp R4); LFX/SFX is not a memcpy;
   modules run in 128-frame blocks; all-passes are parallel (4.7/3.7/6.5/7.0 ms); the SVM second filter is not
   K-weighting; EQ ramps are per section ≈10 ms with the preamp folded into section 0; GetLatency = re-buffer + modules.
4. `kernel-usb.md` §1.4: OT3 bAssocTerminal = 0; §4.2 the 0x0300/0x0400 GET_MEM reads never run on the E1; §6
   "streaming constants unchanged" except the W10 HS-feedback doubling.
5. `system-diagnosis.md` §3 says HID 1.11; the descriptor bytes (`09 21 00 01`) give HID **1.00**. "Consumer control
   (volume wheel)" is only the collection name; no HID volume usage exists.
6. `system-diagnosis.md` §2.1 calls `{E4870E26},1` the OEM period; it is a generic engine value present on every
   render endpoint.
7. `plumbing-ipc.md` §3: `Global\%s` and `Global\HIDInterrupt_%s` are mutexes; IOCTL 0x220408 = GET_ROOT_HUB_NAME,
   0x220424 = GET_HCD_DRIVERKEY_NAME.

## 9. Win7 vs Win10 package differences that matter
| Area | Win7 1.10.03.01 | Win10 1.13.1.1 | Impact |
|---|---|---|---|
| Kernel E1 formats/streaming | 44.1k, 16–24 bit, 3×10 ms URBs, feedback ±2 clamp | identical (HS feedback ×2 differs, irrelevant at FS) | none |
| Kernel control set | {C4BD0511} | {447845DF} + {761E4C5B} | panels not interchangeable |
| Kernel signature | catalog only | embedded Authenticode | none |
| APO DSP | 1.2.32.5 | 1.2.47.0, same constants/chains; adds capture-only modules; 88.2k block 896→882 | none for E1 render |
| Effects slots used | LFX+GFX (profile 101, **with SpeakerEQ**) | on Win8.1+: SFX+MFX (profile 102, **no SpeakerEQ**) | possible audible difference [I] |
| INF E1 format & FX defaults | 24/44.1 render, 16/44.1 mic, SBX+Crystalizer on | identical | none |
| ASIO | in driver INF (1.0.01 beta) + extras | extras only (1.1.1.0) | Win7 users had ASIO by default |
| Control panel | 2.12.05; SndCrUSB 1.1.2.2 | 2.17.28; SndCrUSB 1.1.88 (can set engine device format) | UI same for E1; factory profile values tweaked |
| Windows platform | no spatial sound, no IAudioEffectsManager | Dolby Atmos/Sonic spatial sound possible (active here) | **largest audible difference** |
