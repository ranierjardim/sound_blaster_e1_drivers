# Sound Blaster E1 on Windows 11: live-system diagnosis

Scope: why the E1 sounds worse on this Windows 11 machine, and which facts about the live system the new driver stack has to handle.
Everything below was collected **read-only**, at Medium IL (not elevated), on 2026-11-14.
Labels: **CONFIRMED** means I saw it in the registry, an API result, a descriptor, a log or a binary. **INFERRED** means it is reasoning or general platform knowledge that was not verified on this machine.

Probe sources and raw outputs (scratchpad, not part of the project):
- `C:\Users\Ranier\AppData\Local\Temp\claude\97227dcc83a82625\f26f8da6-b847-4bbd-8fed-cface9700cc4\scratchpad\diag\wasprobe.cpp`: WASAPI format, period, spatial-audio and effects probe. Output is in `wasprobe.txt`.
- `...\scratchpad\diag\usbdesc.c`: reads the device and configuration descriptors through the parent hub with `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION`. Raw bytes are in `e1_config.bin`.
- `...\scratchpad\diag\volprobe.cpp`: reads `IAudioEndpointVolume` capabilities.
- `...\scratchpad\diag\dumpprops.py` and `props.txt`: decode the MMDevices property store.

---

## 1. Ranked causes of the quality loss

| # | Cause | Evidence | New on Win10/11? |
|---|-------|----------|------------------|
| 1 | **Dolby Atmos for Headphones is the active spatial-sound format on the E1 "Speakers" endpoint.** Its HRTF virtualizer runs before the Creative APO chain. | `SpatialAudioDeviceConfiguration.GetForDeviceId(...)` returned `ActiveSpatialAudioFormat = DefaultSpatialAudioFormat = {1459AC38-3875-49BF-BB59-0FE80F4D395D}`, which equals `SpatialAudioFormatSubtype.DolbyAtmosForHeadphones` (CONFIRMED). `ISpatialAudioClient` activates with `maxDynamicObjects=128` and static mask `0xFFFFE`, which is 7.1.4 static beds (CONFIRMED). Dolby Access 3.27.12250.0 is installed (CONFIRMED). | **Yes.** The Windows spatial-sound platform did not exist on Win7 (INFERRED from platform history). |
| 2 | **Creative DSP runs on every stream with "virtualizer" defaults.** These are SBX Surround ON at immersion 0.40, SBX Crystalizer ON at level 0.65, and speaker mode "headphone". Stacked with #1, the stereo signal is virtualized twice. | Render `FxProperties` hold `{5b4777a4-...},0`=TRUE (SBX surround enable), `{A5A78EA4-...},0`=0.4f, `{3CD83C04-...},0`=TRUE (Crystalizer), `{0F03F0BB-...},0`=0.65f, `{3C663165-...},0`=1.0f (pre-attenuation), `{59FBB5C6-...},0`=2 (speaker mode), `{e8d2d16c-...},0`=TRUE (headphone override) (CONFIRMED). These values are exactly the INF defaults. `IAudioEffectsManager::GetAudioEffects` on a shared render stream reports 4 effects, all ON and none switchable (`canSetState=0`): VIRTUAL_SURROUND `{6F64ADC6-...}`, VIRTUAL_HEADPHONES `{6F64ADC7-...}`, ROOM_CORRECTION `{6F64ADC9-...}` and SPEAKER_PROTECTION `{6F64ADCC-...}` (CONFIRMED). | **Partly.** The defaults are identical in both INFs. However, the endpoint was **re-created today (2026-11-14 20:56 local)**, so any tuning the user had applied was lost (CONFIRMED, see §6). |
| 3 | **The hardware supports only 44.1 kHz.** All 48 kHz content (games, browsers, video, most of Windows) is resampled to 44.1 kHz by the Windows audio-engine SRC in shared mode. | USB descriptors list a single rate of 44100 Hz in every alternate setting (CONFIRMED, §3). WASAPI shared mode returns `S_FALSE` for 48 k, 88.2 k, 96 k, 176.4 k and 192 k, with the closest format 44.1 k/float (CONFIRMED). | No. The hardware limit is the same on Win7. Engine-side SRC for apps that rely on `AUTOCONVERTPCM` is more common on 10/11 (INFERRED). |
| 4 | **The exclusive-mode format list is narrow.** Render accepts only 44.1 kHz stereo 16-bit and 24-bit *packed* (3-byte). 24-in-32, 32-int and 32-float are rejected. A bit-perfect player that asks for a 32-bit container fails and falls back to 16-bit or to shared mode. | `IsFormatSupported(EXCLUSIVE)` returned S_OK for only 2 of 60 render combinations. The rest returned `0x88890008` AUDCLNT_E_UNSUPPORTED_FORMAT (CONFIRMED). | Unknown. The Win7 `ksusbaud.sys` 6.0.01.0586 format table is not checked yet (the driver-RE topic covers it). |
| 5 | **The Win7 package shipped a Creative ASIO driver (`CtUsAs64.DLL`), and the Win10 package does not.** ASIO gave the user a bit-perfect path that bypassed APOs and spatial audio. | `CtUsAs64.DLL` exists in the Win7 `Bin\AMD64` folder and is absent from the Win10 folder (CONFIRMED). The DLL contains ASIO strings and the CLSID `{B2D4D5A2-1B17-4AB6-8A6D-667095C480B2}` (CONFIRMED). On this machine it is still registered as "Creative Sound Blaster ASIO" (separately installed "Creative ASIO (USB) 1.02", DLL v1.1.1.0). ASIO4ALL 2.16 is also registered (CONFIRMED). Whether it still works with ksUSBaud 6.1.13 is not tested. | **Yes** (a package difference). |
| 6 | **System enhancements are enabled** (Win11 "Audio enhancements = Device default effects"). | `PKEY_AudioEndpoint_Disable_SysFx` `{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5` is absent from both `Properties` and `FxProperties` of every E1 endpoint (CONFIRMED). | No (this is the default). |
| 7 | **The Win10 APO is a different build from the Win7 APO.** On Win8.1+ the APO also runs in different slots: SFX/MFX instead of LFX/GFX. Only the DEFAULT processing mode is declared, so RAW-mode streams are still processed. | Installed `KSUSBAPO64.dll` 1.2.47.0 (sha256 `54c23e81...`) is the Win10 build. The Win7 build is 1.2.32.5 (`02b1ea87...`) (CONFIRMED). The Win10 binary contains extra effect-type GUIDs (ROOM_CORRECTION, LOUDNESS_EQUALIZER) that the Win7 binary lacks (CONFIRMED). The `.6.3` INF section adds `PKEY_FX_StreamEffectClsid/ModeEffectClsid` and `PKEY_SFX/MFX_ProcessingModes_Supported_For_Streaming = {C18E2F7E-...}` (DEFAULT only) (CONFIRMED). Audible DSP differences are not verified. | **Yes.** |
| 8 | Smaller factors: communications ducking at the default 80% (the E1 also enumerates a HID telephony headset collection), and the mic path runs CrystalVoice noise reduction (enabled by default). | `HKCU\Software\Microsoft\Multimedia\Audio` has no `UserDuckingPreference` (CONFIRMED, so the default applies). MI_03 COL02 is bound to `hidtelephonydriver.inf` (CONFIRMED). Mic `FxProperties {40D0D021-...},1`=TRUE (CONFIRMED). | Partly (HID telephony is a Win10+ feature, INFERRED). |

Not a cause: **volume is hardware/driver volume, not engine attenuation.** Render reports `QueryHardwareSupport = VOLUME|MUTE`, range -74..0 dB in 1 dB steps, currently -24 dB on both channels (CONFIRMED). On a 24-bit path that costs about 4 bits of headroom, which is not audible (INFERRED).

**Bottom line.** The biggest Win11-only degradation is **software processing that Win7 never had**: Dolby Atmos for Headphones, stacked with Creative SBX and Crystalizer at their defaults. It is **not** a lower sample rate or bit depth. The E1 itself is a full-speed UAC1 device fixed at 44.1 kHz with up to 24-bit playback and 16-bit mono capture. No driver can raise those limits. "Maximum quality" therefore means a **bit-exact 44.1 kHz/24-bit path with no unwanted DSP and with correct async feedback**, plus optional, user-controlled effects.

Quick user-side mitigations (no new driver needed, INFERRED to help; not applied, because this task is read-only):
1. Settings > Sound > Speakers (Sound Blaster E1) > Spatial audio: **Off**.
2. In SBE.exe, turn off SBX Surround and Crystalizer, or set "Audio enhancements: Off" for the endpoint.
3. Use WASAPI exclusive mode at 44.1 kHz/24-bit packed, or the registered Creative ASIO, for bit-perfect playback.

---

## 2. Endpoint inventory (MMDevices)

| Endpoint | GUID | State | FormFactor / JackSubType | FX |
|---|---|---|---|---|
| Render "Speakers (Sound Blaster E1)", the **default console render** | `{7e65a67a-04d8-403f-b379-885cdec445d5}` | 1 ACTIVE | 1 Speakers / `{DFF21CE1-...}` KSNODETYPE_SPEAKER | Creative chain |
| Capture "What U Hear (Sound Blaster E1)" | `{1d53f1ec-fad3-4484-a8c8-b0562ba0c1ec}` | 1 ACTIVE | 7 UnknownDigitalPassthrough(?) / `{DFF21FE2-...}` | none (empty FxProperties) |
| Capture "Microphone (Sound Blaster E1)", the **default console capture** | `{389c9c75-9856-4906-96ba-1871e1407d51}` | 1 ACTIVE | 4 Microphone / `{DFF21BE1-...}` | Creative chain |
| Capture "Microphone" (stale, from the old instance) | `{3ea0f972-48f3-4eae-8849-c6086f93661e}` | 4 NOTPRESENT | 4 | Creative chain |

All are bound to `{1}.USB\VID_041E&PID_323B&MI_00\6&2E7F50D9&1&0000` through the KS filter interface `\\?\usb#vid_041e&pid_323b&mi_00#6&2e7f50d9&1&0000#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\{101de7f6-62e6-4f36-bccd-5a6f71ad5fc4}`, with Software_IO pins 0 (render), 6 (What U Hear) and 3 (mic) (CONFIRMED).
The device exposes no hardware loopback interface (§3), so **"What U Hear" is a software loopback synthesized by `ksUSBaud.sys`** (CONFIRMED by the descriptors plus the endpoint list). A replacement driver must reimplement it, for example as a KS loopback pin, or drop it and document the WASAPI loopback alternative (INFERRED).

### 2.1 Format properties (decoded; blob = 8-byte header `41 00 00 00 01 00 00 00`/`41 00 7C 74 28 00 00 00`, then WAVEFORMATEXTENSIBLE)

Render `{7e65a67a-...}` (all CONFIRMED):
- `PKEY_AudioEngine_DeviceFormat {f19f064d-082c-4e27-bc73-6882a1bb8e4c},0`: EXTENSIBLE, 2 ch, **44100 Hz, 24/24-bit PCM**, mask 0x3, block align 6.
- `PKEY_AudioEngine_OEMFormat {e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},3` (Microsoft's real OEMFormat PID): 44100 Hz, 24/24 PCM stereo. This is the INF default.
- `{e4870e26-...},0`: Creative's INF names this `PKEY_AudioEngine_OEMFormat_FLOAT`. Value: 44100 Hz, 32-bit IEEE float stereo. Note: the task brief called this "OEM format", but the actual OEMFormat PID is 3.
- `{e4870e26-...},1` = `a0 86 01 00 00 00 00 00`, which is 100000 hns = **10 ms**. Probably the OEM period (INFERRED name).
- `{3d6e1656-2e50-4c4c-8d85-d0acae3c6c68},2/3` and `{624f56de-fd24-473e-814a-de40aacaed16},3`: 44.1 kHz float stereo. These look like audio-engine/APO internal formats (INFERRED meaning).
- Shared mix format (`GetMixFormat`): **44100 Hz, 32-bit float, stereo**.

What U Hear `{1d53f1ec-...}`: DeviceFormat 44100 Hz **16-bit** stereo PCM. Mix format 44.1 kHz float stereo (CONFIRMED).
Microphone `{389c9c75-...}`: DeviceFormat 44100 Hz **16-bit mono, mask 0x4 (FRONT_CENTER)**. OEMFormat is the same. Mix format 44.1 kHz float mono (CONFIRMED).

### 2.2 WASAPI capability matrix (CONFIRMED, from `wasprobe.exe`)

Tested rates: 44.1, 48, 88.2, 96, 176.4 and 192 kHz. Tested formats: 16-int, 24-int packed, 24-in-32 int, 32-int and 32-float, each in mono and stereo.

| Endpoint | SHARED (`IsFormatSupported`) | EXCLUSIVE (S_OK) |
|---|---|---|
| Render | Every 44.1 kHz variant returns S_OK (the engine converts). Every other rate returns S_FALSE with closest = 44.1 k/32f/2ch. | **44100/16/2ch** and **44100/24 packed/2ch** only. |
| What U Hear | Same pattern as render. | **44100/16/2ch** only. |
| Microphone | Same pattern (mono). | **44100/16/1ch** only. |

Other results:
- `GetDevicePeriod`: default 10.000 ms, minimum 3.000 ms (all three endpoints).
- `IAudioClient3::GetSharedModeEnginePeriod`: default = fundamental = min = max = **441 frames (10 ms)**. There is no low-latency shared mode, which is consistent with a non-WaveRT (WaveCyclic or standard-streaming KS) driver behind `ksthunk` (INFERRED).
- A shared stream initialized with a 20 ms request gets bufferFrames = 970.
- Unsupported exclusive formats return `0x88890008` (AUDCLNT_E_UNSUPPORTED_FORMAT).

### 2.3 Effects and the APO

`FxProperties` of the render endpoint and the mic (CONFIRMED). The INF's own names are in brackets.
- `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},0` association = KSNODETYPE_SPEAKER `{DFF21CE1-...}` (mic: `{DFF21BE1-...}`).
- `,1` PreMix/LFX = `{559493DD-3D1F-40a7-8E94-95762EAD6E95}` "CTAPOContainerLFX", APO flags 13, interface `{5A705A48-DDA9-4316-A4A9-211AE9BDE51F}`.
- `,2` PostMix/GFX = `{2EF9BA5B-FA37-4195-95C5-9483B6FE2104}` "CTAPOContainerGFX", flags 5, interface `{700B86E1-BC5F-47CD-A055-47F1639FA8BF}`.
- `,5` [PKEY_SYSFX_SFXClsid], which is Microsoft's PKEY_FX_StreamEffectClsid = `{E9B73398-425E-4c41-B4C9-F4D1E6922A27}` "CTAPOContainerSFX", flags 13, interface `{AEE02DC4-C477-4AED-8BC4-B57F9925E3AD}`. Note: this is **not** a UI CLSID as the task brief assumed.
- `,6` [PKEY_SYSFX_MFXClsid], which is PKEY_FX_ModeEffectClsid = `{41528545-B167-495e-BB36-E11A17CF64B3}` "CTAPOContainerMFX", flags 13, interface `{E253C036-89B2-46CC-BFB0-FF5989132351}`.
- All four live in `C:\WINDOWS\system32\KSUSBAPO64.dll`, ThreadingModel Both. The 32-bit copy is `SysWow64\KSUSBAPO32.dll`.
- `{d3993a3f-99c2-4402-b5ec-a92a0367664b},5/6` SFX/MFX processing modes = `{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}` (AUDIO_SIGNALPROCESSINGMODE_DEFAULT) only.
- `{b725f130-...},10` display name "Creative System Effect".
- Speaker scout-mode properties `{156AC5B7-384A-4b15-A3A4-79350563FC88},0..20` match the INF defaults (Scout off).
- Mic: `{40D0D021-20BD-4d15-A93C-1DBE8922C642},1`=TRUE (CrystalVoice NR) and `,11` = 0.042 f (inter-mic distance, `0x3D2C0831`).

Other Creative endpoint properties: `{BB282A80-...},0` / `{872572B3-...},0` / `{872572B4-...},0` = `C:\ProgramData\cfSB1600.ini`. This file is **binary/obfuscated, not plain INI** (CONFIRMED by reading it); decoding it belongs to the control-panel RE topic. `{C949C6AA-...},0` = `{495E4C24-85ED-4f19-885E-C2D01D7EA26C}` (KSMALCCTL sound-core control CLSID). `{F1056047-...},0/1` = `{B293EF73-0E2E-468b-B1C9-F083A3E5094C}` (effect-node info).

**Is the Creative APO actually loaded in audiodg.exe?**
- Module enumeration of audiodg (PID 7880) was **denied**: `tasklist /m` shows N/A, and `Get-Process` returns no modules at Medium IL (CONFIRMED). The archived WER report folder is also access-denied.
- Indirect proof (CONFIRMED): `IAudioEffectsManager` on a shared render stream reports the 4 effect GUIDs. Those exact `AUDIO_EFFECT_TYPE_*` GUIDs are present as binary data in the installed `KSUSBAPO64.dll` and in none of the other KSUSB DLLs. The endpoint's FX keys reference only Creative CLSIDs. Conclusion: **the Creative APO is loaded and processing the render stream** (strong inference).
- Capture endpoints report 0 effects (the mic APO does not publish an effects list).
- `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\DisableProtectedAudioDG = 1` (CONFIRMED). audiodg runs unprotected, which (INFERRED) also means an unsigned or test-signed APO **can** be loaded. That matters for the user-mode part of the new stack.
- One audiodg crash was logged: 2026-10-20 03:28, exception `0xc0000374` (heap corruption) in ntdll, bucket `PCH_CC_FROM_ntdll+0x1628A4` (CONFIRMED). The faulting APO cannot be identified without admin access to the WER archive.

### 2.4 Windows 11 audio settings

- Spatial sound: **Dolby Atmos for Headphones, ON** for the E1 render endpoint (CONFIRMED, §1). Supported formats on this endpoint: Windows Sonic `{B53D940C-...}`, Dolby Atmos for Headphones and Dolby Atmos for Speakers `{4C81E564-...}` (CONFIRMED via `IsSpatialAudioFormatSupported`). `HKLM\...\CurrentVersion\Audio\DisableSpatialOnComboEndpoints = 0`.
- Audio enhancements: ON ("Device default effects"), because `Disable_SysFx` is absent (CONFIRMED).
- Endpoint volume: render -24 dB (scalar 0.20), hardware VOLUME+MUTE, -74..0 dB, 1 dB/step. What U Hear: 0 dB, VOLUME only, -96..0 dB, 0.0625 dB/step. Mic: 0 dB (scalar 0.754), VOLUME+MUTE, -96..+6 dB, 0.0625 dB/step (CONFIRMED).
- Defaults: the E1 is both the default render (eConsole) and the default capture (eConsole) device (CONFIRMED).
- Communications ducking: no user override, so the default applies (CONFIRMED absence).

---

## 3. What the hardware really offers (USB descriptors, CONFIRMED)

Read through root hub `USB\ROOT_HUB30\4&21947844&0&0`, port 5, at **full speed (USB 1.1 timing)**. Strings: "Creative Technology Ltd" / "Sound Blaster E1" / serial "0000047173" / iString4 "10012006".

Device descriptor: bcdUSB 0x0200, class 0 (per interface), bMaxPacketSize0 64, VID 0x041E, PID 0x323B, **bcdDevice 0x1006**, 1 configuration.
Configuration: wTotalLength 0x0117 (279 bytes), 4 interfaces, bmAttributes 0x80 (bus-powered), MaxPower 100 mA.

| IF/Alt | Content |
|---|---|
| IF0 AudioControl | **UAC 1.0** (bcdADC 0x0100). Topology: IT#2 USB-streaming 2 ch (L,R) → FU#5 (master: mute; ch1/ch2: volume) → OT#4 **Speaker (0x0301)**. IT#1 **Microphone (0x0201)** 1 ch → FU#6 (master: mute+volume) → OT#3 USB-streaming. There are no other units: no mixer, no selector, no processing or extension units. |
| IF1 alt0 | Playback, zero bandwidth. |
| IF1 alt1 | PCM, 2 ch, subframe 2 bytes, **16-bit**, one discrete rate **44100 Hz**. EP **0x01 OUT isochronous asynchronous** (bmAttributes 0x05), wMaxPacketSize 200, bInterval 1, bSynchAddress **0x83**. Class EP: bmAttributes 0x00 (**no sampling-frequency control**, no pitch). |
| IF1 alt2 | PCM, 2 ch, subframe **3 bytes, 24-bit**, 44100 Hz only. EP 0x01 OUT async, wMaxPacketSize **300**, sync EP 0x83. |
| Feedback | EP **0x83 IN isochronous, usage feedback** (bmAttributes 0x01), wMaxPacketSize **3** (full-speed 10.14 format), bInterval 1, **bRefresh 5 (every 32 ms)**. |
| IF2 alt0/alt1 | Capture: PCM, **1 ch, 16-bit**, 44100 Hz only. EP **0x81 IN isochronous async**, wMaxPacketSize 200, bInterval 1. |
| IF3 | HID 1.11, report descriptor 91 bytes, EP 0x84 interrupt IN, 4 bytes, interval 10 ms. Enumerated as "HID-compliant consumer control" (volume wheel) plus "HID-compliant headset" (telephony). |

Implications for the new driver (INFERRED unless noted):
- The only rate is 44.1 kHz. Sample-rate-control requests (SET_CUR SAMPLING_FREQ) are not advertised, so do not send them, or send them and tolerate a STALL.
- Max quality is **44.1 kHz / 24-bit stereo out (alt2)** and **44.1 kHz / 16-bit mono in**.
- Playback is **asynchronous**: the driver **must** consume the 0x83 feedback value (10.14 fixed point, frames per 1 ms frame; nominal 44.1 = `0x0B0666` in 10.14) to size each packet as 44 or 45 frames. Otherwise samples slip and periodic clicks follow. Packet budget: alt1 200 B = up to 50 frames × 4 B; alt2 300 B = up to 50 frames × 6 B.
- Expose more container formats on the KS/WaveRT side (24-in-32, 32-int, 32-float) and convert them to 3-byte on the USB side. That fixes cause #4.
- Volume: FU#5 accepts per-channel volume (1 dB/step, -74..0 dB as currently reported by the driver) and master mute. Mic FU#6 has master mute and volume. Read GET_MIN/MAX/RES from the device rather than hard-coding these values.

---

## 4. Driver binding and inbox alternative

MI_00 is currently bound to **Creative KsUSBaud** (CONFIRMED):
- Service `KsUSBaud`, `\SystemRoot\system32\drivers\ksUSBaud.sys` v6.1.13.0101, Start 3 (demand), Type 1 (kernel). Device stack: `ksthunk → KsUSBaud → usbccgp`.
- INF `oem57.inf` (= `ksusbaud.inf_amd64_cef47356c80e391b`), section `SB1600_Device.6.3`, DriverVer 07/21/2015 1.13.1.1, provider "Creative Technology Ltd.", class MEDIA.
- Matching ID `usb\vid_041e&pid_323b&mi_00`, DriverRank `0x00FF0001`, signer score **WHQL (0D000005)**.
- Signature: Valid, "Microsoft Windows Hardware Compatibility Publisher" (WHQL), cert valid 2014-12-19..2016-03-19 and timestamped. That is why it still loads under Secure Boot and the vulnerable-driver blocklist.
- Compatible IDs: `USB\Class_01&SubClass_01&Prot_00`, i.e. **UAC1** (not Prot_20).

Inbox candidates that PnP evaluated at today's install (setupapi, CONFIRMED):
- `wdma_usb.inf` section `USBAudio` → **usbaudio.sys** (UAC1 class driver), rank `0x00FF2005`, signer Inbox (0D000003). The INF matches `USB\CLASS_01` and excludes `...&Prot_20`. It has **no** E1-specific (`041E`/`323B`) entry and uses `mssysfx.CopyFilesAndRegisterCapX`.
- `usbmidi2.inf` section `USBMidi2Device`, rank `0x00FF3105` (matches the audio-class compatible ID but is irrelevant).
- `usbaudio2.inf` (usbaudio2.sys) matches only `USB\Class_01&SubClass_0x&Prot_20`, so it **cannot** bind the E1. The E1 is UAC1, and the UAC2 driver is not an option without a filter that rewrites descriptors (INFERRED).

What the inbox usbaudio.sys would give (INFERRED from the descriptors and the INF):
- The same 44.1 kHz 16/24-bit formats.
- FU volume and mute.
- No "What U Hear" endpoint and no Creative effects (only the Microsoft default sysfx).
- Unverified: whether the legacy UAC1 driver honours the 0x83 async feedback endpoint correctly. If it does not, the result would be periodic clicks or drift, which could explain the user's report that "the normal Windows drivers are very bad".

Win7 vs Win10 INF binding: both INFs have the same `[Manufacturer] CREATIVE, ntamd64, ntx86.6.3, ntamd64.6.3`. On Win7 (NT 6.1) the device used `SB1600_Device` with LFX/GFX only. On Win8.1+ it uses `SB1600_Device.6.3`, which adds SFX/MFX and processing modes (CONFIRMED). The Win7 INF additionally has `SysFx1600.APOChain.AddReg`, but every line in it is commented out, so there is no effective difference (CONFIRMED).

---

## 5. Signing and Code Integrity state (decides how a new kernel driver can load)

| Item | Value | Source |
|---|---|---|
| Secure Boot | **ON** (`HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State\UEFISecureBootEnabled = 1`). `Confirm-SecureBootUEFI` returned access denied (not elevated). | CONFIRMED |
| Test signing | **OFF**. `SystemStartOptions = " NOEXECUTE=OPTIN  FVEBOOT=2125824  NOVGA"` contains no TESTSIGNING. `bcdedit /enum {current}` failed without admin. | CONFIRMED |
| HVCI / Memory Integrity | **OFF**: `...\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity Enabled = 0, WasEnabledBy = 0`. | CONFIRMED |
| VBS | OFF: `EnableVirtualizationBasedSecurity = 0`. Win32_DeviceGuard `VirtualizationBasedSecurityStatus = 0`, `SecurityServicesRunning = {0}`, `CodeIntegrityPolicyEnforcementStatus = 2`. `RequireMicrosoftSignedBootChain = 1`. | CONFIRMED |
| Vulnerable-driver blocklist | ON (`CI\Config\VulnerableDriverBlocklistEnable = 1`). | CONFIRMED |
| BitLocker | FVEBOOT present in the start options. Changing Secure Boot or BCD will ask for the recovery key. | CONFIRMED (option present); consequence INFERRED |
| OS | Windows 11 Home Single Language, build **26300.9974**, DisplayVersion 26H2. | CONFIRMED |
| audiodg | `DisableProtectedAudioDG = 1`. | CONFIRMED |

Consequences for the project (INFERRED):
- A self-built, unsigned kernel driver (KMDF/PortCls/ACX) **cannot load** as things stand. Secure Boot blocks `bcdedit /set testsigning on`. The development options are (a) temporarily disable Secure Boot in firmware and enable test signing (BitLocker recovery key needed), or (b) get Microsoft attestation signing via Partner Center, which requires an EV certificate.
- HVCI is off, so there is no extra HVCI-compatibility gate today. The driver should still be HVCI-clean so it keeps working if the user turns Memory Integrity on.
- **User-mode-only fallback that loads today:** keep the inbox `usbaudio.sys`, or Creative's WHQL `ksUSBaud.sys`, as the kernel transport, and put the new "Sound Blaster" DSP in a **custom APO**. audiodg is unprotected (`DisableProtectedAudioDG=1`). Associating the APO with the endpoint means writing `FxProperties` with admin rights (Equalizer-APO style) or using a signed extension INF.
- Building the user-mode parts needs only the installed toolchain: MSVC 2022 and SDK 10.0.26100, with no WDK.

---

## 6. Install history (setupapi.dev*.log, CONFIRMED)

- **2026-02-01 13:58**: `setup.exe /S /4 /preinstall` (Creative Win10 package) ran `UpdateDriverForPlugAndPlayDevices` for PIDs 323A, 323B, 323C and 323E. For 323B, PnP picked `SB1600_Device.6.3.NTamd64` (rank 0x00ff0001, WHQL, 1.13.1.1). The package was already imported as `oem57.inf`. Result: "Device required reboot: Query remove failed CR_REMOVE_VETOED". Uninstall entry: "Sound Blaster E-Series 1.04.05", InstallDate 20260201.
- **2026-08-26 22:48**: `cleanmgr.exe /autocleanstoragesense` deleted the non-present E1 device nodes: composite, MI_00 `6&2E7F50D9&0&0000`, MI_03 and its HID collections. **All per-endpoint settings were lost** at that point.
- **2026-11-14 20:56:17**: hardware-initiated install of the new instance `USB\VID_041E&PID_323B&MI_00\6&2E7F50D9&**1**&0000` (re-enumerated, now on root-hub port 5). Candidates were ksusbaud (selected), wdma_usb and usbmidi2. During the file copy, `VerifyTrustFailed 0x800b0109` (chain to an untrusted root) was logged for the SET*.tmp backups next to `KSUSBAPO64.dll`, `KSUSBAPO32.dll`, `KSUSBWrap64.dll`, `KSUSBDGFX64.dll` and `KSUSBDGFX32.dll`. The copy still succeeded and `Get-AuthenticodeSignature` on the installed DLLs says Valid, so this is probably the driver-policy check of their embedded SHA-1-era signatures (INFERRED, benign). `KsUSBDvInst.dll` and `KSUSBAIM64.exe` were "pruned from copy" because they were already present.
- The endpoint property `{b3f8fa53-0004-438e-9003-51a46e139bfc},15` (SYSTEMTIME) on all three active endpoints = 2026-11-14 23:56:47 UTC (20:56 local). So the **endpoints are brand new**: INF defaults plus Windows' default spatial format (Dolby) apply.
- Event log: System event 20003 (UserPnp) at 2026-11-14 20:56:20 reports "add Service KsUSBaud ... status 0". There are no KsUSBaud errors. Unrelated WUDFRd 0xC0000365 boot warnings are present.

---

## 7. Win7 vs Win10 package differences relevant to this topic

| Item | Win7 pkg (1.10.03.01, 2014-04-17) | Win10 pkg (1.13.1.1, 2015-07-21) | Status |
|---|---|---|---|
| Kernel driver | `ksusbaud.sys` 6.0.01.0586, 1 538 560 B | `KsUSBaud.sys` 6.1.13.0101, 1 580 552 B (installed) | CONFIRMED |
| APO | `KSUSBAPO64.dll` 1.2.32.5, 1 959 192 B, sha256 `02b1ea87…` | 1.2.47.0, 2 451 216 B, sha256 `54c23e81…` (installed) | CONFIRMED |
| Effect-type GUIDs inside the APO | 12 (no ROOM_CORRECTION, no LOUDNESS_EQ) | 14 (adds ROOM_CORRECTION `{6F64ADC9}` and LOUDNESS_EQUALIZER `{6F64ADC4}`) | CONFIRMED |
| ASIO driver | **`CtUsAs64.DLL` / `CtUsAsio.dll`** (1.0.01.0000 beta, CLSID `{B2D4D5A2-1B17-4AB6-8A6D-667095C480B2}`) | **absent** | CONFIRMED |
| `.hda` sidecar files | none | `KSUSBAPO64.hda`, `KSUSBDGFX64.hda`, `KSUSBWrap64.hda` | CONFIRMED (purpose not analysed) |
| Other bins | ppld 1.2.32.5, SPI 1.0.370.0, AIM 3.40 | ppld 1.2.47.0, SPI 1.0.377.0, AIM 3.47 | CONFIRMED |
| E1 OEM default format | 44.1 k/24-bit stereo (render); 44.1 k/16-bit mono (mic) | identical | CONFIRMED |
| E1 FX defaults (SBX, Crystalizer, speaker mode, Scout, CrystalVoice NR) | identical | identical | CONFIRMED |
| FX slots used on the target OS | Win7 → LFX/GFX (`SysFx1600.AddReg`) | Win11 → `.6.3` section: LFX/GFX **plus** SFX/MFX and processing modes = DEFAULT | CONFIRMED (INF); runtime slot choice INFERRED |
| Spatial sound (Atmos or Sonic) in the path | not possible on Win7 | **active** on Win11 | CONFIRMED (live) |

---

## 8. Requirements for the new stack (from this diagnosis)

1. Stream 44.1 kHz only, 24-bit packed to the device (alt2), with async feedback from EP 0x83 (10.14 format, bRefresh 32 ms).
2. Advertise 16, 24-packed, 24-in-32, 32-int and 32-float at 44.1 kHz in exclusive mode and convert internally. Set the shared default to 44.1 kHz/24-bit so the engine mix format stays 44.1 kHz float.
3. Declare the RAW processing mode (`AUDIO_SIGNALPROCESSINGMODE_RAW`) alongside DEFAULT so a bit-perfect app can bypass the effects. The effects must be user-switchable, and IAudioSystemEffects3 should be implemented so that Windows 11 "Audio enhancements" toggles them.
4. Optionally declare the endpoint as **Headphones** (form factor 3), or keep it as Speakers, and leave spatial sound to the user. Do not ship with SBX and Crystalizer ON by default if "maximum fidelity" is the goal.
5. Recreate "What U Hear" as a loopback capture (2 ch, 44.1 kHz, ideally 24-bit or float instead of 16-bit).
6. Mic: 44.1 kHz/16-bit mono, FU#6 mute and volume. CrystalVoice NR is optional.
7. HID IF3 (volume wheel and telephony) stays on inbox HidUsb. Nothing is needed from the new driver.

## 9. Open questions

- Does the inbox `usbaudio.sys` (UAC1) honour the asynchronous feedback EP 0x83 on build 26300? This needs an A/B test or USB capture. It is the likely root of "inbox driver sounds bad".
- What do ROOM_CORRECTION and SPEAKER_PROTECTION map to inside the Creative APO (Crystalizer? Smart Volume? the headphone limiter)? This is the APO RE topic.
- Is the exclusive 24-in-32 restriction also present in Win7 `ksusbaud.sys` 6.0.01.0586? This is the driver RE topic.
- Does the registered Creative ASIO (`CtUsAs64.DLL` 1.1.1.0) still work against KsUSBaud 6.1.13? It needs an ASIO host test.
- `cfSB1600.ini` is obfuscated, and where SBE.exe persists the user's settings is unknown (it may write the FxProperties directly).
- Which module caused the 2026-10-20 audiodg heap corruption? This needs admin access to the WER archive.
- Is Dolby Atmos for Headphones the system-wide *default* because Dolby Access sets it for new endpoints? (`DefaultSpatialAudioFormat` = Dolby on this endpoint.)
