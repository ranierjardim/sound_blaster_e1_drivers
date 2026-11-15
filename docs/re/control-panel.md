# Sound Blaster E-Series Control Panel (SBE.exe) — UI and control-path spec

Scope: the Creative control panel for the Sound Blaster E1 (USB\VID_041E&PID_323B), Windows 7 package
(driver 1.10.03.01, panel 2.12.05 build 02) compared with the Windows 10 package (driver 1.13.1.1,
panel 2.17.28 build 01, the one installed on this PC). This is a specification written in our own words so
the panel can be rebuilt natively in C. It does not reproduce Creative source.

Tags: **[C]** = CONFIRMED (seen in decompiled code, in shipped data, or read from the live system).
**[I]** = INFERRED (deduced, not proven).

Scratch evidence (not in the project): decompiled C#/XAML in
`%TEMP%\claude\…\scratchpad\decomp\cp7` and `…\cp10`; extracted skin images in `…\scratchpad\ui_assets`;
live enumeration logs `…\scratchpad\decomp\live_render_w10.txt`, `live_capture_w10.txt`, `live_hidcaps.txt`;
the read-only enumerator I wrote is `…\scratchpad\enumtool\scenum.c` (it calls only Bind, Enum and Get methods).

Related notes: `plumbing-ipc.md` covers the Command Router, APO and SPI side. This file stops at the
ISoundCore boundary and the COM objects that SndCrUSB creates.

---

## 1. Component map

| File | Win7 version | Win10 version | Role |
|---|---|---|---|
| SBE.exe | 865,792 B, .NET 4 WPF, x86, assembly "Malcolm" | 945,152 B | The UI. One code base ("Malcolm") shared by about 20 Creative products. A product is selected at run time from `SBE.dat` [C] |
| en-US\SBE.resources.dll | 1,036,288 B | 1,347,584 B | Satellite assembly: all BAML pages plus `ResourceStrings.xaml` (English strings) [C] |
| SBTEpsln.dll | 2,734,080 B (598 resources) | 2,381,824 B (644) | Managed resource-only "Epsilon" skin: 600+ PNGs under `/SBTEpsln;Component/Skins/...` [C] |
| Interop.CtSndCr.dll | 1.0.0.42 | 1.0.0.52 | COM interop for ISoundCore (enums and structs) [C] |
| SndCrUSB.DLL | 1.1.2.2 ("Creative KS SoundCore Module", 2014-04) | 1.1.88.0 (2015-06) | COM server CLSID {495E4C24-85ED-4f19-885E-C2D01D7EA26C}, registered without the registry (reg-free) through `SBE.exe.manifest`. Exposes ISoundCore and IEventNotify [C] |
| CTAudEp.dll | 3.91.1.0 | 3.97.0.0 | "Audio Endpoint Selection": MMDevice and IAudioEndpointVolume helpers, KS-node volume and AGC, monitoring, the device-selection dialog [C] |
| MalLgcy.dll | 1.0.23.0 | 1.0.24.0 | Flat `CSCT*` wrappers over CTAudEp for P/Invoke [C] |
| CTHID.dll / CTHIDRpA.dll | 1.4.12.1 | 1.7.1.1 | COM servers. CTHID does HID I/O (SetupDi + HidD_Get/SetFeature, Get/SetOutputReport, ReadFile). CTHIDRpA builds and parses report packets; SndCrUSB loads it dynamically [C] |
| HKDetect.dll | 1.0.6.0 | 1.0.9.0 | Global keyboard hot keys through RegisterHotKey; used only for Scout Mode on/off [C] |
| CTIntrfu.dll | 3.5.0.0 | 3.5.0.0 (identical) | Creative component factory and INI/registry helpers [C] |
| CTLoadRs.dll | 2.0.7.0 | 2.0.7.0 (identical) | Loader for MUI language resources [C] |
| CTGetPdt.dll | 1.0.25.0 | 1.0.41.0 | Supported-products query [C] |
| CTMFSess.dll | 1.1.3.0 | same | Media Foundation player for the demo clip and the 7.1 speaker-test WMAs [C] |
| HookWndU.dll / HWAboutU.DLL / SBAbout.bff | | | Skinned About box [C] |

Product binding [C]: `SBE.dat` `[Settings] 6B18CF79-E415-44AF-A908-3641FB1A314E = E9172A92-D19B-4015-A821-02A9619F742F`
selects `MalcolmProductID.ESeries`. App ID "Sound Blaster E-Series Control Panel". Theme "Epsln". Tray icon
`NotifyIcon_ESeries.ico`. Title logo `ImageStyleTitleLogoSBEpsilon`. Product image `product_e1.png` (292×72) for
hardware ID `USB_VID_041E_PID_323B`. Internally Creative names PID 0x323B "U3A" and PID 0x323E "U3B". The
single-instance mutex is `"Malcolm-" + AppGUID`.

---

## 2. How the panel talks to the device

### 2.1 Finding the SoundCore object [C]
1. CTAudEp enumerates the Creative endpoints. SBE remembers its choice in
   `HKCU\Software\Creative Tech\Audio Endpoint Selection\Sound Blaster E-Series Control Panel\{Render,Capture}`.
2. For each endpoint SBE reads the property `{C949C6AA-132B-4511-BB1B-35261A2A6333},0` from the MMDevice
   property store (`MalLgcyLib.GetMalcolmCLSID`). On this PC the E1 render endpoint {7e65a67a-…} and the
   Microphone capture endpoint {389c9c75-…} both hold `{495E4C24-85ED-4f19-885E-C2D01D7EA26C}`. "What U Hear"
   holds nothing, so it gets no SoundCore page.
3. `Activator.CreateInstance(CLSID)` → ISoundCore → `BindHardware({infoType=EndpointId(0), endpointId=L"{0.0.0.00000000}.{guid}"})`.
4. SBE registers `IEventNotify.RegisterEventCallback(mask=0xFF, ICallback)`. Events: ContextChange=1,
   ParamChange=2, ConfigChange=4, External=0x10, FeatureRequery=0x20, DeviceReset=0x40. The event payload is
   `{featureId, paramId}`.
5. The hardware-ID string comes from PKEY `{B3F8FA53-0004-438E-9003-51A46E139BFC},2` and is reduced to
   `USB_VID_041E_PID_323B`.

Threading [C, live]: the object must be used from an **STA** thread. Under MTA, BindHardware and EnumFeatures
succeed but every effect parameter returns E_FAIL (0x80004005). Under STA they work.

### 2.2 ISoundCore interface [C]
IID {6111E7C4-3EA4-47ED-B074-C638875282C4}. Vtable order after IUnknown:
`BindHardware(stHardwareInfo*)`, `EnumContexts(idx, stContextInfo*)`, `GetContextInfo(ctx, stContextInfo*)`,
`GetContext(ctx*)`, `SetContext(ctx, restoreState)`, `EnumFeatures(ctx, idx, stFeatureInfo*)`,
`GetFeatureInfo(ctx, feat, stFeatureInfo*)`, `EnumParams(ctx, idx, feat, stParamInfo*)`,
`GetParamInfo(stParam, stParamInfo*)`, `GetParamValue(stParam, stParamValue*)`,
`SetParamValue(stParam, stParamValue)`, `GetParamValueEx(stParam, size*, data*)`,
`SetParamValueEx(stParam, size, data*)`, `ValidateParamValue`, `ValidateParamValueEx`,
`GetParamValueEx_unsafe(stParam, PARAM_DATA*)`, `SetParamValueEx_unsafe(stParam, PARAM_DATA*)`,
`ValidateParamValueEx_unsafe`.
IEventNotify {F6CB394A-A680-45C0-ACD2-F0595626A3FD}: `RegisterEventCallback(mask, ICallback*)`,
`UnregisterEventCallback()`. ICallback {B353C442-C49D-4532-9E3A-1B20A182FD00}: `EventCallback(stEventInfo)`.
Typelib {54C2AF11-22C4-43c5-9803-611E9484062B}.

Structs (pack 4) [C]:
```
stParam      { u32 paramId; u32 featureId; u32 contextId; }                    // 12 bytes
stParamValue { u32 type; union{float f; i32 b; u32 dw; i32 l;} }               // 8
stParamInfo  { stParam id; u32 type; u32 dataSize; stParamValue min,max,step,def;
               u32 attrib; char desc[32]; }                                     // 88
stFeatureInfo{ u32 featureId; char desc[32]; char ver[16]; }
stContextInfo{ u32 contextId; char desc[32]; }
stHardwareInfo{ u32 infoType /*0=EndpointId,1=AdapterId*/; wchar endpointId[260]; }
PARAM_DATA   { u32 size; void* data; }
EnumPreset in/out : { i32 index; u32 presetType /*0 std,1 custom*/; u32 presetId; wchar name[32]; }
Active/Remove preset : { u32 presetId; }      SavePreset : { wchar name[32]; } (max 32 chars)
EnumSpeakerConfig / EnumMultiplexOutputs item : { i32 index; u32 value; }
BatteryInfo : { i32 maxLevel; i32 minLevel; i32 stepSize; }
```
Param types: 0 Float, 1 Bool, 2 Dword, 3 LongInt, 4 Select, 5 VarSize. Contexts: 1 Standard,
2 Entertainment, 4 Karaoke, 8 THX2. E1 reports only context 1, "Standard" [C live].
`paramAttrib == 1` means read-only: the panel hides the slider and shows a label instead.

### 2.3 Below ISoundCore [C at the call site; the far side is described in plumbing-ipc.md]
In SndCrUSB (Win7 build, function near VA 0x42a380) the object calls
`CoCreateInstance({32CD1956-569B-432f-BA27-F0BFEA458D1B} /*Command Router enumerator*/, CLSCTX_INPROC|LOCAL (5), IID {920449A1-FFE8-434C-A2BC-C0CC1582BBD9})`.
If that fails it falls back to `{CAABAA19-4206-41b1-9A99-2B8917C79631}` (KSUSBSPI CEndpointEnumeration).
It then calls vtbl+0x0C, an endpoint-enumeration method taking (index, flags, 0xF, out ptr), and asks the
object it gets back for {4E953B53-09B5-4EEB-B41B-F94C590D2736} (a callback/advise interface, vtbl+0x0C).
IID {6CB33A03-F351-4C59-9089-AE181EAC6B43} is also queried. SndCrUSB makes **no direct DeviceIoControl
call** (it does not import it). Effects travel SndCrUSB → Command Router → APO manager (KSUSBAPO) or SPI
(KSUSBSPI → driver). [C for the imports and GUIDs; I for the meaning of each method]
The Win10 SndCrUSB also imports `CTGet/SetWindowsAudioEngineDeviceFormatOfAudioOutputEndpoint` (shared-mode
format, i.e. sample rate and bit depth) and `CTGet/SetSpeakersConfiguration` from CTAudEp. The Win7 build does not [C].

Other storage SndCrUSB refers to [C as strings]: `Software\Creative Tech\KsMalc\VID_%04X&PID_%04X`,
`%s\Creative\KsMalc\VID_%04X&PID_%04X_{Playback,Recording,MicReverb,Profile}.xml` (custom presets and
profiles). None of these files or keys exist on this PC. Per-endpoint persisted state lives in MMDevice
properties `{f112024a-fe30-42a8-80ab-8dd825a06f78},0..78` and `{5510c7ab-dfc2-40d0-a98b-5f77f697005e},1..35`
(VT_UI4 and blobs), and `C:\ProgramData\cfSB1600.ini` (obfuscated) [C that they exist; I that they are
SndCr/SPI state].
Install defaults for E1 [C]: `HKLM\SOFTWARE\Creative Tech\Device\VID_041E&PID_323B\Defaults`
(SpeakerVol=0xC0000000 as float −2.0, DefaultPlayback[00/01]=−6144 = −24 dB in 1/256-dB steps [I],
THXCMSS_isEnabled=1, THXCrystalizer_isEnabled=1, THXNoiseReduction_isEnabled=1, THXAEC_isEnabled=1, all others 0,
THXDolbyDigital_*=2).

### 2.4 Feature and parameter set reported live for the E1 (Win10 SndCrUSB 1.1.88 on this PC) [C live]
Render endpoint (Speakers, Sound Blaster E1):

| featureId | Feature | pid | Name | Type | Range / step / default | Current |
|---|---|---|---|---|---|---|
| 0x01000002 | Processing Control | 0 | SpeakerConfig | Dword | (mask) | 0x80000000 (Headphones) |
| | | 1 | PhysicalSpeakerConfig | Dword | | 3 (FL+FR) |
| | | 3 | EnumSpeakerConfig | VarSize 8 | items: 0x80000000, 0x00003003 | |
| 0x10000001 | THX Surround (CMSS-3D) | 0 | Enable | Bool | def 1 | 1 |
| | | 1 | Immersion Level | Float | 0..1 step 0.01 def **0.40** | 0.40 |
| 0x10000008 | Crystalizer | 0 | Enable | Bool | def 1 | 1 |
| | | 1 | Level | Float | 0..1 step 0.01 def 0.65 | 0.65 |
| 0x10000020 | Speaker Bass (X-Bass) | 0 | Enable | Bool | def 0 | 0 |
| | | 1 | Strength | Float | **0..100 step 1 def 50** | 50 |
| | | 2 | Freq_Hz (crossover) | Float | **10..1000 step 1** def 80 | 80 |
| 0x10000004 | Smart Volume | 0 | Enable | Bool | def 0 | 0 |
| | | 1 | Strength | Float | 0..1 step 0.01 def 0.74 | 0.74 |
| | | 2 | Mode | Dword | 0..2 (0 Normal, 1 Loud, 2 Night) | 0 |
| 0x10000002 | Dialog Plus | 0 | Enable | Bool | def 0 | 0 |
| | | 1 | Strength | Float | 0..1 step 0.01 def 0.5 | 0.5 |
| 0x10000010 | Graphic EQ | 0 | Enable | Bool | def 0 | 0 |
| | | 1 | Preamp | Float dB | **−12..+12 step 0.01** def 0 | 0 |
| | | 2..11 | Band0..Band9 | Float dB | **−24..+24 step 0.01** def 0 | 0 |
| | | 256 | EnumPreset | VarSize | not listed by EnumParams, but works when called directly | 10 presets |
| | | 258 | ActivePreset | VarSize 4 | | 1 (Flat) |
| 0x60000001 | Effect master control | 0 | THX/SBX Features (supported mask) | Dword | | 47 = 0x2F |
| | | 1 | THX Features State (enabled mask) | Dword | | 9 = Surround+Crystalizer |
| | | 2 | THX Master Control (SBX on/off) | Bool | | 1 |
| | | 6 | Scout Mode Master Control | Bool | | 0 |
| 0x70000020 | Battery | 0 | Info | VarSize 12 | {max 100, min 0, step 1} | |
| | | 1 | Level | Dword | | 100 |
| | | 2 | Status | Dword | 1 = LowBatt, 2 = Charging | 2 |
| 0x01000001 | Multiplex Output | 14 | EnumMultiplexOutputs | VarSize 8 | items: 2, 3 | |
| | | 15 | MultiplexOutput | Dword | 2 = Headphone, 3 = MicIn | 2 |

Bits of the effect masks [C, `CTSoundCoreConst`]: CMSS3D 0x01, DialogPlus 0x02, SVM 0x04, Crystalizer 0x08,
GraphicEQ 0x10, XBass 0x20, BassMgmt 0x40, SpeakerEnhancement 0x80. Each is the low byte of its featureId.
The live mask 0x2F leaves out GraphicEQ, so the SBX master switch does not drive the EQ.

Capture endpoint (Microphone):

| featureId | Feature | pid | Name | Range | Current |
|---|---|---|---|---|---|
| 0x20000004 | VIP Noise Reduction | 0 | Enable (Bool, def 1) | — | 1 |
| 0x20000010 | VIP Mic Smart Volume | 0/1 | Enable / Strength | 0..1 step 0.01 def 0.74 | 0 / 0.74 |
| 0x20000001 | VIP VoiceFX | 0 | Enable | def 0 | 0 |
| | | 1 | FormantFreq1 | 200..800 step 5 def 400 | |
| | | 2 | FormantFreq2 | 800..1800 step 5 def 1400 | |
| | | 3 | FormantFreq3 | 1600..2750 step 5 def 2000 | |
| | | 4 | PitchMod | 0.25..4 step 0.05 def 1 | |
| | | 5 | EnvelopeMod | 0.5..2 step 0.01 def 1 | |
| | | 6 | PitchVariability | −1..5 step 0.05 def 1 | |
| | | 7 | QuiverDepth | 0..0.2 step 0.05 def 0 | |
| | | 8 | ContourDepth | 0..0.4 step 0.05 def 0 | |
| | | 256/258 | EnumPreset / ActivePreset | 18 presets | active 1 |
| 0x60000001 | master | 3 | VIP Features (supported mask) | | 21 = 0x15 (VoiceFX, NR, MicSVM) |
| | | 4 | VIP Features State | | 4 (NR) |
| | | 5 | VIP Master Control (CrystalVoice on/off) | | 1 |

The E1 has **no** AEC, Mic Array/Focus, Mic EQ, Bass Management, Speaker EQ, headphone-gain (LoadSelect),
EAX, Dolby, Direct Mode or EffectsProfile features [C live]. Noise-reduction strength is not exposed (only the
Enable switch) [C live].

**Win7 vs Win10 for ranges** [C static plus C live]: SndCrUSB contains a static descriptor table, built by
1,570 (Win7) / 1,817 (Win10) CRT initialisers. I materialised it by emulation; 41 identical copies per binary.
It holds *different* defaults from the live values above: CMSS default 0.67, X-Bass Strength 0..1 step 0.01
def 0.5, X-Bass Freq 1..1000 step 10, EQ step 1.0, VoiceFX Formant1 250..750 step 1, Formant2 850..1750,
Formant3 1850..2750, PitchMod step 0.01, EnvelopeMod step 0.1, PitchVariability step 0.1, Quiver/Contour step
0.01. The live values come from the provider (APO) through the router [I]. **The Win7 APO may therefore have
reported X-Bass as 0..1. The panel copes with both because profiles are stored normalised (§6).**
Static-table changes Win7 → Win10 [C]: Win10 adds MalcolmDeviceControl pid16 HeadphoneLoadSelect and pid19
BlueToothDeviceInfo; ProcessingControl pid2 SizeSpeakerConfig, 7/8 DirectMode, 9 SPDIFInPassThrough,
10 SPDIFOutPassThrough, 11 HighPowerAmplifier, 14–16 SpeakersModel/Presets; MicControl pid1 MicBoost
0..30 dB step 10; the whole SpeakerCalibrator feature 0x10000100 (levels, invert, delay, master, mute,
MaxSpeakerDistance); Miscellaneous 0x70000100; WaveTest 0x70000200. NoiseReduction Strength moves from pid1 to
pid2 ("StrengthV2"). None of these additions apply to the E1 live.

### 2.5 Panel ControlID → SoundCore mapping (subset used by the E1) [C, `SoundCoreObject.AssignParamID*`]
| Panel ControlID | featureId / paramId |
|---|---|
| THXMasterOnOff 100 (the SBX power button) | 0x60000001 / 2 |
| THXSurroundEnable 101 / Value 102 | 0x10000001 / 0, 1 |
| THXSimpleSurroundEnable 103 / Value 104, SurroundActiveType 105 | 0x10000200 / 0, 1 ; 0x80000001 / 0 (see note*) |
| THXCrystalEnable 106 / Value 107 | 0x10000008 / 0, 1 |
| THXBassEnable 108 / Value 109 / Freq 110 | 0x10000020 / 0, 1, 2 |
| THXDlgPlusEnable 111 / Value 112 | 0x10000002 / 0, 1 |
| THXSmrtVolEnable 113 / Value 114 / Mode 115 | 0x10000004 / 0, 1, 2 |
| SpkConfig 204, PhysicalSpkConfig 205, EnumSpkConfig 207 | 0x01000002 / 0, 1, 3 |
| SpkEnumMultiplexOutputs 208, SpkMultiplexOutput 209 | 0x01000001 / 14, 15 |
| EqualizerEnable 501, Preamp 506, Band1..10 507–516 | 0x10000010 / 0, 1, 2..11 |
| EqualizerEnumPreset 503 / ActivePreset 502 / Save 504 / Remove 505 | 0x10000010 / 256 / 258 / 259 / 260 |
| ScoutModeOnOff 1201 | 0x60000001 / 6 |
| SBXMasterOnOff 1310 | 0x60000001 / 7 (not present on the E1; the E1 uses pid 2) |
| BatteryInfo 1601 / Level 1602 / Status 1603 | 0x70000020 / 0, 1, 2 |
| MicMasterOnOff 10101 (the CrystalVoice power button) | 0x60000001 / 5 |
| MicVoiceFXEnable 10102 / Active 10103 / Enum 10104 / Save 10105 / Remove 10106 | 0x20000001 / 0, 258, 256, 259, 260 |
| MicSmrtVolEnable 10107 / Value 10108 | 0x20000010 / 0, 1 |
| MicNoiseEnable 10123 | 0x20000004 / 0 |

*Simple ("Stereo") surround is eFeature_Effects_SimpleSurround = 268435968 = **0x10000200** (pid 0 Enable,
1 Level). SurroundType is 0x80000001 (pid 0 Active, 1 = CMSS3D). 0x10000100 is SpeakerCalibrator and 0x10000080
is SpeakerEnhancement. Neither SimpleSurround nor SurroundType exists on the E1, so the "3D / Stereo" radio
pair under Surround stays hidden [C].

---

## 3. Window, layout and visual style (Epsilon theme) [C unless marked]

* Borderless WPF window (`WindowStyle=None`, `AllowsTransparency`, `ResizeMode=CanMinimize`, centred).
  **Main grid is 882 × 637 px**, filled by `Skins/Bkgnd_Main.png` (882×637: black #000000 with #191919 and
  #282828 panels).
  * Rows: 6 / 36 (title bar) / * / 10. Columns: 6 / **292 (left navigation)** / * / 10.
  * Content frame: **564 × 575 px** (`MainPageWidth/Height`), centred in the right column. Page changes use a
    0.5 s cross-fade plus a scale from 0.85 to 1.
* Title bar (row 1): column 0 is an 85 px logo (`TitleLogo_SoundBlaster_Epsilon.png`). Column 1 is the
  title label (Tahoma 13.33, shows the endpoint name such as "Speakers / Microphone"). Then a battery icon
  (26×12, shown when Battery is supported). Then four 44×31 buttons: Menu (combo with the items below),
  Switch View (hidden for E-Series), Minimize, Close.
  * Menu items: "Options...", "Start {app} when Windows starts" (checkable), separator,
    "Restore Default...", separator, "Audio Device Selection...", "Software Language Selection...",
    "Software Update...", separator, "About {app}...". Win10 adds "Setup Assistant..." but it is hidden for
    the E-Series.
* Left navigation, 292 px wide (UI "Generation 3", `gridFeatureV1`):
  1. Mode button with the product image `product_e1.png` 292×72 and the text "PRO STUDIO" (font size 20).
  2. Feature list. Each item is 292×41 with background `feature_bkgnd_{normal,over,down}.png`
     (#191919 / red gradient #800000–#980000 / #880709), a 40×40 icon (`featureicon/icon_*_{normal,over}.png`)
     and a Tahoma 16 label. Text #979797 normally, white when hovered or selected. Items for the E1, in order:
     **SBX Pro Studio, CrystalVoice, Scout Mode, Speakers/Headphones, Mixer, Equalizer, Advanced Features**.
     The list is computed from features (`MalcolmUtil.HasFeaturesPage`).
  3. Bottom: a 1 px divider #323232; a "Profile" radio button (118×27, margin 20,16); the current profile
     name (13.33 px, #979797); Facebook (23 px) and Sound Blaster (50 px) link buttons.
  4. Master volume strip: `vol_background.png` 292×52 with a CTVolumeSlider (mute button plus a horizontal
     slider) bound to the Windows endpoint master volume (scalar 0..1) and mute.
* Palette (theme `ThemeEpsln.xaml` plus sampled skins): text #AFAFAF (`MainFontBrush`); hover and selected
  text white; disabled #464646; feature text #979797; dividers and dialog border #323232; standard presets
  white; **custom presets orange**; theme accent `MainThemeFontBrush` = Red (#FF0000); combo selected
  background Red; slider filled track **#880709**, empty track #323232, thumb 26×30 grey (#807878); master
  power toggle 42×42: ON #9D0003, OFF #464545; text buttons 110×40 with fill #262626 and hover #CB0202; combo
  boxes #262626, height 30; check boxes 30×30 with a red tick.
* Fonts: Tahoma throughout (`PageFontFamily`; sizes Big 12, Medium 10.667, Small 10). An embedded "Tactica"
  font (`/Font/#Tactica`, tactica_f.ttf) is the feature font in the other themes; Epsilon uses Tahoma.
* Info "i" buttons are collapsed in Epsilon (`InfoButtonVisibility=Collapsed`).
* Mini view (259 px wide) exists in the XAML but is **disabled for the E-Series** in both versions
  (`CanToggleToMiniView` is true only for Apollo8, Apollo18 and Malcolm).
* Reference images: `scratchpad\ui_assets\w7_SBTEpsln\SBTEpsln.g.resources\skins\…` (598 PNGs) and the
  matching `w10_SBTEpsln` folder. These are for reference only and must not be redistributed.

---

## 4. Pages (E1-relevant controls). All sliders take their min, max and default from `GetParamInfo` (no fixed ranges).

Slider label formats (`SliderLabelValueConverter`) [C]: Percentage shows `value/max` formatted as `0%`;
Hz shows `int+"Hz"`; dB shows `int+"dB"`; PlusdB adds a leading "+"; Degree adds "°". A slider is hidden when
its parameter is missing. It becomes a read-only label when `attrib==1`.

### 4.1 SBX Pro Studio (`THXTruStudioProPage`; THX labels are swapped for SBX through `ReplaceResourceString`)
* A demo screen of 400×227 with an effect animation and Play/Stop buttons (50×20 each, background 146×34).
  The animation plays `Demo.wmv` through CTMFSess.
* Master: a 42×42 power toggle plus the label "SBX Pro Studio". Tooltip "SBX Pro Studio On/Off".
  → 0x60000001/2 (Bool).
* Rows (check box, then a CTSlider with a percentage label):
  | Label (en) | Tooltip | Enable / value param |
  |---|---|---|
  | Surround | "Control the level of audio immersion in music, movies and games" | 0x10000001/0 ; /1 (0..1). A hidden "3D / Stereo" radio pair appears only if SimpleSurround exists |
  | Crystalizer | "Enhance music and movies to make them sound livelier" | 0x10000008/0 ; /1 |
  | Bass | "Enhance the sound quality and bass of any speaker system…" | 0x10000020/0 ; /1 (0..100 live). Second row "Crossover Frequency" slider in Hz → /2 (10..1000) |
  | Smart Volume | "Adjust the loudness of your audio playback automatically…" | 0x10000004/0 ; /1. Radio buttons "Normal" / "Loud" ("Maintain loudness at the maximum level at all times") / "Night" ("Preserve audio details at quieter volume levels") → /2 = 0/1/2 |
  | Dialog Plus | "Enhance the voices in movies for clearer dialog" | 0x10000002/0 ; /1 |
* The `level` values in profiles for these sliders are the raw 0..1 values, except Bass, which is normalised
  (§6).

### 4.2 Equalizer (`EqualizerPage`)
* Check box "EQ" → 0x10000010/0. A preset combo (height 30) plus "Save" and "Delete" buttons (110×40).
  Delete is enabled only for custom presets, which are shown in orange. Standard presets come first; custom
  presets follow, sorted with `StrCmpLogicalW`.
* Ten vertical sliders (dB format), in a UniformGrid 245 px high, labelled **31, 62, 125, 250, 500, 1k, 2k,
  4k, 8k, 16k** (10.667 px). A "LEVEL" master slider (40 px wide) = Preamp. Axis labels:
  left `max dB` / "0dB" / `min dB` (±24); right ±12 for the master.
* Moving any band after choosing a preset turns the selection into a "custom" state. "Save" opens a name
  dialog (300×170) with at most 32 characters and invalid-character checks; an existing name asks
  "Confirm overwrite". Delete asks "Are you sure…". Save calls SavePreset (pid 259, name), Delete calls
  RemovePreset (pid 260, presetId), selecting a preset calls SetActivePreset (pid 258).
* **Standard EQ presets** (ids 1–10, identical in Win7 and Win10; values in dB; preamp 0 in all) [C, the XML
  embedded in SndCrUSB, confirmed live]:

| id | Name | 31 | 62 | 125 | 250 | 500 | 1k | 2k | 4k | 8k | 16k |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | Flat | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 | Acoustic | 0 | 1 | 2 | 0 | 0 | 0 | 0 | 2 | 2 | 2 |
| 3 | Classical | 0 | 6 | 6 | 3 | 0 | 0 | 0 | 0 | 3 | 3 |
| 4 | Country | −1 | 0 | 1 | 1 | 1 | 0 | 0 | 2 | 3 | 4 |
| 5 | Dance | −1 | 2 | 3 | 4 | −1 | −1 | 0 | 0 | 4 | 4 |
| 6 | Jazz | 0 | 0 | 1 | 4 | 4 | 4 | 0 | 1 | 3 | 3 |
| 7 | New Age | 0 | 2 | 2 | 0 | 0 | 0 | 1 | 2 | 2 | 2 |
| 8 | Pop | −2 | 0 | 2 | 2 | 0 | −1 | −1 | 0 | 3 | 6 |
| 9 | Rock | −1 | −1 | 1 | 2 | −1 | −1 | 0 | 0 | 4 | 4 |
| 10 | Vocal | −2 | −1 | −1 | 0 | 3 | 4 | 3 | 0 | 0 | 1 |

(The XML maps ParamId 1 to the preamp and ParamIds 2..11 to bands 0..9. Presets have Type 'Standard'. Names
are localised with `resStrPreset*`. If enumeration fails, the panel falls back to ids 1..10 plus a dummy
"Custom 1" with id 101.)

### 4.3 CrystalVoice (`MicLineInPage`; recording endpoint)
* Header: "Recording Device:" plus the device name, and a "Set as Windows default" button.
* "Mic Recording Volume": a mute toggle (24×20) plus a percentage slider (0..1). It uses the capture endpoint's
  master volume and mute through CTAudEp (`CSCTGet/SetMasterVolume`, scalar).
* "Mic Boost": a dB slider from KS volume node index 1 of the capture endpoint
  (`CSCTOpenKsNodeTypeVolumeOfAudioEndpoint(idx=1)`, range from `GetLevelRange…` in dB). It can show as an
  AGC check box instead (`OpenKsNodeTypeAutoGainControl idx 0`). Each appears only if its node exists [C code;
  I whether the E1 has the node].
* "Mic Monitoring Volume": a mute toggle plus a slider. It drives the hardware "output volume of audio input
  endpoint" (`CSCTOpen/SetMasterVolumeForTheOutputVolumeOfAudioInputEndpoint`), i.e. the USB mixer path from
  mic to output. If that is missing it falls back to software monitoring (`CTStartMonitorAudioInputEndpoint`,
  output gain 1.0).
* Master: 42×42 power toggle plus "CrystalVoice" → 0x60000001/5.
* E1 rows:
  * **FX**: check box ("Morph your voice into different characters and accents") → 0x20000001/0. Preset combo
    → ActivePreset 258. "Add" and "Delete" buttons manage custom VoiceFX presets.
  * **Smart Volume**: check box ("Be heard clearly without having to shout or whisper") → 0x20000010/0,
    slider → /1.
  * **Noise Reduction** (graphic check box, "Eliminate unwanted background noise in your conversation") →
    0x20000004/0.
  * EQ, Acoustic Echo Cancellation and Focus/Wedge Angle exist in the page but are hidden for the E1.
* "Test" button (110×40): a mic loopback test.
* **VoiceFX presets** (ids 1–18; parameters F1/F2/F3 Hz, PitchMod, EnvelopeMod, PitchVariability, Quiver,
  Contour) [C embedded XML, confirmed live]:
  1 Neutral 400/1400/2000, 1, 1, 1, 0, 0 · 2 Female to Male 400/1400/2000, 0.60, 1.05, 1, 0, 0 ·
  3 Male to Female 400/1400/2220, 2.37, 0.92, 1, 0, 0 · 4 Scrappy Kid 400/1400/2000, 3.00, 0.66, 1, 0, 0 ·
  5 Elderly 713/1500/1800, 1.40, 1.45, 1, 0.1818, 0 · 6 Orc 468/1321/2200, 0.65, 1.50, 1, 0, 0 ·
  7 Elf 398/1395/2451, 1.11, 0.96, 1, 0, 0 · 8 Dwarf 294/1527/2055, 0.77, 0.97, 1, 0, 0 ·
  9 Alien Brute 383.54/1428.50/2550, 0.65, 1.3113, 1, 0.03, 0 · 10 Alien Infiltrator 200/1370.50/2000, 1.24, 1.06, 1, 0.13, 0 ·
  11 Robot 400/1400/2000, 1.39409, 1, −0.00824, 0, 0 · 12 Marine 388/1155/1848, 0.80, 1.08, 0.54, 0, 0 ·
  13 Emo 400/1400/2000, 1, 1, 0.2, 0, 0 · 14 Unstable 400/1400/2000, 2.37, 0.92, 2.5, 0, 0 ·
  15 From Up North 400/1400/2000, 1, 1, −0.20661, 0, 0.26446 · 16 Deep Voice 339.54/1362.50/2046, 0.42912, 1.20132, 1, 0, 0 ·
  17 Munchkin 400/1400/2000, 1, 0.60163, 1, 0, 0 · 18 Demon 453/1544/2600, 0.69, 2.00, 1, 0, 0.
  The internal name of id 6 is "Org"; the display string is "Orc".
* Mic EQ presets (not used by the E1; included for completeness) [C]: Flat, then "Preset 1..10"
  (8 bands, dB): P1 −3,−4,0,2,3,−3,4,5 · P2 −3,−4,0,2,4,−2,2,4 · P3 −2,−3,3,4,4,−4,3,2 ·
  P4 −3,−5,0,4,4,−3,0,0 · P5 −2,−3,2,4,4,0,−3,2 · P6 −5,−4,−2,0,3,4,6,7 · P7 0,3,−2,−4,−4,−2,5,7 ·
  P8 0,0,2,2,3,−4,2,4 · P9 0,0,2,2,−2,0,−4,4 · P10 0,2,−2,0,3,5,6,5. Win10 adds 12 "DM1"
  0,8,0,12,12,4,8,10 (`resStrPresetDM1` "DM-1").

### 4.4 Scout Mode (`ScoutModePage`)
* Check box "Scout Mode" → 0x60000001/6.
* "Hot Key Configuration" table with Task and Hot Key columns: "Turn on Scout Mode" (task 1) and
  "Turn off Scout Mode" (task 2). The button "Configure Hot Keys..." opens HotKeyDialog.
* HKDetect: `CTInitHotkey(appFlag, callback, data)`, `CTGetHotkey(epId, &collection)`,
  `CTSetHotkey(&collection)`, `CTUnbindHotkey(epId, taskId)`.
  `CTHOTKEY {u32 taskId, runtimeId, modifiers, vk, enabled, registerResult; LPWSTR endpointId}`. The
  callback event 1 means "hot key pressed", and the panel then sets ScoutModeOnOff on that endpoint's
  SoundCore. Registration is done with Win32 `RegisterHotKey`. The hot keys only work while the panel runs;
  the note says to enable "Start … when Windows starts".

### 4.5 Speakers/Headphones (`SpeakerHeadphonePage`)
* The label "Speakers / Headphones Configuration" (or "Headphones Configuration") with a combo filled from
  EnumSpeakerConfig: **0x80000000 → "Headphones"**, **0x00003003 → "Stereo (2.0/2.1)"**
  (`SPEAKERCONFIG_EXTERNAL_STEREO`). Selecting an entry sets SpeakerConfig (0x01000002/0). Mask meanings:
  0x80000000 headphones, 0x40000000 built-in speakers, 0x02000000 "Speakers"; low 12 bits are the
  KSAUDIO_SPEAKER mask.
* A CTSpeakers picture (248×224) with a "Test" button that plays `WmaFile\{FL,FR,…}_7_1.wma` per channel.
* Bass Management, headphone gain (Normal/High) and the "Fine-tuned for" speaker-EQ combo are hidden for the
  E1, because those features are absent.
* Win10 adds a speaker/headphone toggle and separate Speaker and Headphone pages, but only for the U1 (X7) and
  ZxRi. They are not shown for the E1 [C].

### 4.6 Mixer (`MixerPage`)
* "Playback": one CTMixerControl per render-side control. Types: `MasterVolumeCtrl` (endpoint master volume
  and mute through IAudioEndpointVolume) and `OutVolOfAudioInputCtrl` (monitor or listen level of each input,
  through `CSCTOpenMonitoringControl`, `…SetVolumeLevelUniformOfMonitoringControl`, `…SetMuteOfMonitoringControl`,
  name from `GetNameOfConnectorOfMonitoringControl`).
* "Recording" (label colour #D00808): "Input Device:" with a retask toggle (36×36), the capture master
  volumes, "Microphone Boost" slider and AGC, "Balance" per channel (L/R/C/Sub/SL/SR/RL/RR, only those that
  exist), and a "listen" check box with a slider (0..1).
* Volumes are scalar 0..1 in the panel and are shown 0–100.

### 4.7 Advanced Features (`AdvanceFeaturesPage`)
* For the E1 only this group shows: **"Configure Headphones/Microphone Jack as:"** with the radios
  **"Headphones"** (MultiplexOutput = 2) and **"Microphone"** (= 3). It maps to 0x01000001/15. The page is
  shown because EnumMultiplexOutputs contains MicIn (3).
* Bit-matched recording, SPDIF stereo mix, THX/SBX hardware profiles, USB power overdrive, Bluetooth, LED
  and Siren appear only on other products.

### 4.8 Profile (`ProfilePage`, opened from the "Profile" button on the left)
* An avatar button (100×100) that opens AvatarDialog. Win7 avatars (Game, Map, Movie, Music, Sniper, Sword,
  Wheel) are embedded in SBE.exe; Win10 moves them into SBTEpsln `skins/avatar/` and adds a custom avatar.
* "User Profiles" (Win10: "Audio Profiles") combo, plus "Save", "Delete", "Import", "Export" (110×40).
  Import and Export use the filter `XML File (.xml)|*.xml`. Win10 also has "Create Desktop Shortcut", for
  the U1 only.
* Factory profiles: `<app>\Profile\USB_VID_041E_PID_323B\*.xml` (the E-Series loads factory profiles by
  hardware ID). User profiles: `%LOCALAPPDATA%\Creative\Sound Blaster E-Series Control Panel\profile\USB_VID_041E_PID_323B\<guid>.xml`.
  The last applied profile is recorded in `HKCU\Software\Creative Tech\Sound Blaster E-Series Control Panel\Settings\USB_VID_041E_PID_323B`
  (value `Avatar`, and the profile path).

### 4.9 Options and About
* Options dialog: an auto-update check box plus OK and Cancel (110×40). Restore Default calls
  `SetContext(current, eRestoreState_AllDefault=3)`.
* About: HWAboutU skinned box (SBAbout.bff). Copyright "2011-2014" (Win7) or "2011-2015" (Win10).

---

## 5. HID: buttons and battery on the E1 [C live caps; I for protocol]
`hidcaps` (read-only) shows two top-level collections on MI_03:
* **COL01**: Consumer page 0x0C, usage 0x01. Input report **0x09** (2 bytes, vendor data, usage 0) and output
  report **0x08** (5 bytes). This is Creative's vendor channel. SndCrUSB loads CTHIDRpA and CTHID (COM, through
  CTIntrfu) and most likely reads battery level and status, and handles other device queries, through output
  report 8 and input report 9 [I]. The battery appears in SoundCore as feature 0x70000020; the panel shows it
  in the title bar (CTBattery: max 100, min 0, step 1; status 1 low, 2 charging).
* **COL02**: Telephony page 0x0B, usage 0x05 (Headset). Input report **0x02** with button usage **0x2F
  (Phone Mute)**. Output report 0x02 with usage 0x09 (mute LED). Feature report 0x06 is 1 byte (0..1). This is
  the **mic-mute** control. Windows' HID telephony stack and the driver use it; the panel does not process
  it [I].
* No HID volume or "SBX" button collection exists. The E1's volume wheel is not exposed as HID [C caps; I hardware].
* HKDetect handles only keyboard hot keys for Scout Mode (§4.4) and is unrelated to the device's HID.

---

## 6. Profile file format (exported/imported `.xml`) [C]
```
<?xml version="1.0" encoding="UTF-8"?>
<profile>
  <info><avatar>avatar_sword.jpg</avatar><profile_name>…</profile_name>
        <profile_id>4</profile_id><version>2.0</version></info>      (device_id is written on export)
  <settings>
    <surround enable="true|false" level="0..1"/>          raw value of 0x10000001/1
    <stereo_surround …/>                                    (simple surround, not on the E1)
    <crystalizer enable level/>                             raw 0x10000008/1
    <bass enable level cross_freq/>                         level NORMALISED = value/(max-min); cross_freq raw Hz
    <svm enable level mode/>                                raw 0x10000004/1 and /2
    <dlgplus enable level/>                                 raw 0x10000002/1
    <voicefx enable preset/>   <vip_svm enable level/>   <noise_reduction enable/>
    <aec/> <voice_fcs level/> <mic_eq preset/> (Win10)     (not on the E1)
    <graphic_eq enable preset preamp band0..band9/>         dB values; band0 = 31 Hz
  </settings>
</profile>
```
Loader rule for float levels: a value above 1 is applied as is; otherwise it is multiplied by (max−min) of
the live range. This is why Bass 0.5 becomes 50 on the 0..100 X-Bass.
Factory E1 profiles (Win10 values; Win7 differences in brackets):

| Profile (id) | Surround | Crystalizer | Bass | SVM (mode 0) | Dialog+ | VIP SVM | NR | EQ bands 31…16k |
|---|---|---|---|---|---|---|---|---|
| Adventure and Action (4) | on 0.55 | on 0.40 | off 0.5 / 80 Hz | off 0.74 [0.8] | off 0.5 | on 0.74 | on | 4,4,0,−2,2,2,1,2,4,2 |
| Driving Simulation (5) | on 0.20 | off 0.65 [0.4] | off 0.5 | off 0.74 [0.8] | on 0.35 | on 0.74 | on | 4,4,2,−1,2,2,1,2,4,2 |
| First Person Shooter (7) | on 0.30 | off 0.65 [0.42] | off 0.5 | on 0.6 | off 0.5 [0.35] | on 0.74 | on | 6,0,−3,−3,0,0,0,3,3,0 |
| Real Time Strategy (8) | on 0.32 | on 0.50 | off 0.5 | off 0.74 [0.6] | on 0.35 | on 0.74 | on | 0,2,−3,−3,−2,−2,−2,2,3,0 |

All four have EQ enabled, preamp 0, and VoiceFX off with preset 1. Avatars: Win7 used GUID-named .jpg files;
Win10 uses avatar_sword/wheel/sniper/map.
SndCrUSB also embeds "HostEffectProfiles" XML sets (SBX Default, Warm Sound, Smart Volume, Dynamic Boost,
Night Mode, Clear Dialog, Stadium Surround, Clear Comms, Cinematic Action; Music/Movie/Gaming in Win10). These
are for products with the EffectsProfile feature (hardware profiles). They are **not** used by the E1
(EffectsProfile is absent live) [C absent / I purpose].

---

## 7. Win7 vs Win10 summary (control panel)
1. Panel 2.12.05 → 2.17.28. SndCrUSB 1.1.2.2 → 1.1.88.0, CTAudEp 3.91.1 → 3.97.0, CTHID/CTHIDRpA
   1.4.12 → 1.7.1, HKDetect 1.0.6 → 1.0.9, MalLgcy 1.0.23 → 1.0.24. CTIntrfu and CTLoadRs are identical [C].
2. The Win10 SndCrUSB links CTAudEp to set the Windows audio-engine device format and the speaker
   configuration. It adds Calibrator, DirectMode, Speaker models, MicBoost, NR StrengthV2, Miscellaneous and
   WaveTest params. None of these is used by the E1 [C].
3. Live parameter ranges come from the provider; SndCrUSB's static fallback table differs (X-Bass 0..1, EQ
   step 1, CMSS default 0.67). Profiles are stored normalised, so they work either way [C/I].
4. EQ and VoiceFX presets are identical. Win10 Mic EQ adds DM1 [C].
5. UI: Win10 adds U1/ZxRi-only pages (SpeakerModel, Headphone, HybridProfile), Bluetooth states, a Setup
   Assistant menu item and desktop shortcuts. None of these is visible for the E1. String changes: "User
   Profiles" → "Audio Profiles", "EQ" → "Mic EQ". Avatars move into the skin. The SBTEpsln skin is
   re-encoded (2.73 MB → 2.38 MB) and gains expand-button, Bluetooth, jack-gold and spkhp toggle images [C].
6. The Win10 manifest adds Windows 7/8/8.1/10 supportedOS entries. Reg-free COM for SndCrUSB and CTMFSess is
   unchanged [C].
7. Factory profiles: Win7 ships only PID_323B; Win10 adds 323C (with Music, Movie and Default plus Profile.ini)
   and 323E. Some E1 defaults changed (table in §6) [C].

## 8. Implications for the native rebuild
* For the E1, the UI only needs these features: SBX (5 effects plus master), 10-band EQ with preamp and
  presets, CrystalVoice (NR, Mic SVM, VoiceFX with 18 presets, master), Scout Mode (on/off plus global hot
  keys), headphone/stereo configuration, jack retask (Headphones/Microphone), master and mic volumes, mic
  monitoring, battery level and charging indicator, and profiles in the XML format above.
* Use the live ranges in §2.4 as the native defaults: CMSS 0.40, Crystalizer 0.65, Bass 50 % at 80 Hz,
  SVM 0.74 Normal, Dialog+ 0.5, EQ ±24 dB with a ±12 dB preamp, and these on at first run: SBX master,
  Surround, Crystalizer, CrystalVoice master and NR.
