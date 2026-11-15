# Sound Blaster E1: middleware, plumbing and IPC (Win7 1.10.03.01 vs Win10 1.13.1.1)

Scope: everything between the control panel (SBE.exe), the APO running in audiodg (KSUSBAPO64.dll) and the
kernel driver (ksusbaud.sys). It covers KSUSBWrap64, KSUSBDGFX64, KSUSBSPI64, KSUSBPPCn64, ksusbppld64,
KSUSBAIM64 (APOIM: APOMgr64 + CmdRtr64), SBUSBMon/SBUSBMonL, KsUSBDvInst (co-installer), CtUs*As* (ASIO),
the INFs, the obfuscated .ini files, and the live Windows 11 (build 26300) state.

Tags: **[C]** = CONFIRMED (seen in code, data, registry or logs). **[I]** = INFERRED (reasoned from evidence, not proven).
All addresses are VAs in the Win10 (1.13.1.1) AMD64 build unless noted otherwise. These notes describe the behaviour
in my own words. Raw decoded material sits outside the project, in
`%TEMP%\claude\...\scratchpad\decomp\` (`ini\*.ini`, `inf*_clean.txt`, `ipc_*.txt`, `live_*.txt`).

---------------------------------------------------------------------------------------------------

## 0. Executive summary (what matters for the new driver)

1. **Windows 11 currently runs the E1 render endpoint at 24-bit / 44 100 Hz / 2 ch PCM** (PKEY_AudioEngine_DeviceFormat).
   The engine mix format is float32 / 44.1 kHz. The microphone runs at 16-bit / 44.1 kHz mono, and What U Hear at 16-bit / 44.1 kHz stereo. [C]
   This is simply the INF's `OEMFormat` default. **The Win7 and Win10 INFs carry the identical OEMFormat blob**, so the
   default format did not change between packages. [C] All 48 kHz content therefore goes through Windows' SRC down to 44.1 kHz. [I]
2. **The ASIO driver was NOT dropped in Win10.** The Win10 package ships it as a separate sub-installer
   (`extras\Audio\CtUsASIO`, "Creative ASIO (USB)" 1.02.00, CtUsAs64.DLL **1.1.1.0**). The Win7 package shipped
   **1.0.01 Beta** both inside the driver INF and in the same extras installer. **It is installed and registered on this PC right now.** [C]
   ASIO is a pure **WDM-KS client**: it opens ksusbaud.sys KS pins directly through `IOCTL_KS_PROPERTY` 0x2F0003,
   `IOCTL_KS_WRITE_STREAM` 0x2F8013 and `IOCTL_KS_READ_STREAM` 0x2F4017. That bypasses audiodg, the mixer, SRC and every APO, which makes it the
   bit-transparent path. It supports 44.1/48/96 kHz and 16/24-bit, with latency between 1 and 100 ms. [C]
3. **Control-panel → DSP settings travel through the endpoint property store (FxProperties) and the registry.** No
   shared memory or pipe reaches the APO: KSUSBAPO64 contains no named-object strings and does hold the
   IMMNotificationClient IID. [C/I] Control-panel → hardware/kernel settings travel through **KSUSBSPI64**, which uses KS properties
   on the ksusbaud filter, KS events, HID reports and named global events/mutexes. [C]
4. **KSUSBWrap64 (DaisyWrap chainer), KSUSBDGFX64 (encoder shell) and KSUSBPPCn64 are dead weight for the E1.** They get registered,
   but no E1 FX key references their CLSIDs. In both INFs the chain section is fully commented out. [C]
5. Live Win11 FxProperties keep the INF defaults, which means **SBX Surround ON, Crystalizer ON (level 0.65), Headphone
   mode (speaker mode 2)**. Whenever the APO is loaded, playback is processed rather than transparent. [C for the values; whether the APO is loaded could not
   be verified]
6. **Is KSUSBAPO64 loaded into audiodg right now? NOT VERIFIABLE.** The session runs at Medium integrity, so `tasklist /m` and `Get-Process`
   both return N/A for audiodg modules, and the WER archive is ACL-protected. There are no APO-load-failure events in
   Microsoft-Windows-Audio/Operational. One `AUDIODG.EXE` crash with heap corruption (0xC0000374, 2026-10-20) is in the Application log,
   but its faulting module cannot be attributed. [C for each fact]
7. **Equalizer APO 1.4.2 is installed on the PC, but NOT on the E1 endpoints.** Its child-APO entries exist only for
   the Realtek {2c57783c}, {4082d59d} endpoints and the BT {ad8bda8b} endpoint. [C]

---------------------------------------------------------------------------------------------------

## 1. Component inventory and roles

| File (Win10 build) | Ver W7 → W10 | Role | COM CLSIDs / exports | W7 vs W10 |
|---|---|---|---|---|
| KSUSBAPO64.dll | 1.2.32.x → 1.2.47.0 | The real Creative DSP APO, loaded in audiodg | CTAPOContainerLFX {559493DD-3D1F-40a7-8E94-95762EAD6E95}, GFX {2EF9BA5B-FA37-4195-95C5-9483B6FE2104}, SFX {E9B73398-425E-4c41-B4C9-F4D1E6922A27}, MFX {41528545-B167-495e-BB36-E11A17CF64B3}, EFX {FE078F0E-3B15-42c9-9BCA-7CB7C4CB0088}, CEffectNodeInfo {B293EF73-0E2E-468b-B1C9-F083A3E5094C}; typelib {FC11D1DD-A423-49E0-8B20-AD8394388F0C} | code differs [C] |
| KSUSBWrap64.dll | 1.0.0.180 (both) | "Audio Processing Object Chaining Module" (DaisyWrap): an APO that hosts N child APOs | DaisyWrapLFX {254E8DEF-A6D4-4e41-8114-E284F877E703}, DaisyWrapGFX {E7C0C8E7-65A8-492d-9DD1-97BF2022DAE1}; APO ifaces {CD39C891-...}, {428B1980-...}; typelib {5DF0A6C3-ED5E-4707-B974-B65BE73B88A1}; ATL AppID {B4BC7D22-B40A-42ba-BE9A-690B0AF9C48D} | **code identical** (only the Authenticode block differs) [C] |
| KSUSBDGFX64.dll | 1.0.5.0 (both) | "Creative APO Shell Module" = OPTDGFX, a digital-encoder (DDL/DTS) GFX shell | OPTDGFX {242249CC-E3C8-4571-9A0B-ED0906B7F994} (INF name KSDGFX_CLSID); typelib {36BA66E0-7CE4-4793-BFE1-309419569EE6} | identical [C] |
| KSUSBSPI64.dll | 1.0.370 → 1.0.377 | "KS Vista SPI Module": the **hardware/kernel control backend** (Service Provider Interface) | CEndpointEnumeration {CAABAA19-4206-41b1-9A99-2B8917C79631} (also {0F8665B5-110D-415d-99F8-B0A2439E92DD} in rgs); typelib {730ABAD9-1C15-4AFB-B57F-3A52914E4A75}; ifaces IEndpointEnumeration / ICTSpiEndpoint (EnumEndpoints, GetEndpointID, GetEndpointSubType, CreateEndpointInterface) | code differs (minor) [C] |
| KSUSBPPCn64.dll | 1.0.0.180 (both) | "Chaining Property Page Loader" for DaisyWrap (ppchain) | PropPageStub {C09096B1-0CB5-4a3f-8158-517CE02BFF81}, EndpointPropPageLoader {7AFF9602-ADF7-4741-ADF0-BDEA0B0741F8}, ControllerPropPageLoader {407F9734-1AD1-4ad0-9AB0-BDE1DF4C4616}; typelib {A29249B6-...} | identical; **not registered** (not in INF RegisterDlls), copied only to Program Files [C] |
| ksusbppld64.dll (KSUSBPPLD64) | 1.2.32.5 → 1.2.47.0 | "Property Page Loader": the mmsys.cpl Enhancements-tab stub | PropPageStub {361E7F16-C665-45d4-B5C3-B5FADC53650B} (INF SYSFX_UI_CLSID_KS), EndpointPropPageLoader {BF35B30D-65E6-434e-B3EA-26E0B4051F8D}, ControllerPropPageLoader {AB8ED0BB-9F89-40bc-92C4-AE7DEA94970F}; typelib {9A2D55DF-5AA1-4B9E-AFA5-CD322332783C} | differs; registered, but **the UI CLSID line is commented out in both INFs**, so no Creative tab appears in mmsys.cpl [C] |
| KSUSBAIM64.exe | 3.40 → 3.47 | **Not** an "interface manager": it is a 32-bit self-extracting cabinet ("Self installation") that installs **APOIM = "Creative Audio Processing Object Interface Module"** | payload: APOMgr64.dll/APOMngr.dll (APO Manager 1.0.342.0), CmdRtr64.dll/CmdRtr.dll (Command Router 1.0.64.0), mint32/64.exe (APOIM setup), APOIM.ini | differs [C] |
| APOMgr64.dll (System32) | 1.0.342.0 | Control-panel-side "APO Manager": reads and writes APO parameters | APOManager Enumerator {AA9E54D5-17B5-4cf7-8FD8-1F7119BB72D4} | – |
| CmdRtr64.dll (System32) | 1.0.64.0 | "Command Router": aggregates the feature providers (APO and SPI) behind one enumerator | CommandRouter Enumerator {32CD1956-569B-432f-BA27-F0BFEA458D1B} | – |
| SBUSBMon.dll / SBUSBMonL.dll | 1.0.30.0 / 1.0.5.0 (both) | "SB AVStream Monitoring Utility" + launcher: a rundll32 background monitor for endpoint arrival | exports SBAVMonitor, SBAVMonitorLauncher | identical; **its Run key line is commented out in both INFs**, and the installer script deletes old Run values, so it never runs [C] |
| KsUSBDvInst.dll | 0.5.9.64 → 0.5.9.65 | Class co-installer ("CtDvInst") plus the Creative scripted installer engine | exports CtDevCoInstProc, RCAudioDriverSetup | differs [C] |
| CtUsAs64.DLL / CtUsAsio.DLL | 1.0.01 Beta → 1.1.1.0 | ASIO driver (KS client) | CAsio {B2D4D5A2-1B17-4AB6-8A6D-667095C480B2}, ProgID CtUsAsio.CAsio.1, typelib {6FCDD800-F96D-4A7C-9BB0-F92A4835CA14} | see §6 |

Win7 vs Win10 code identity check: I hashed each PE with the security directory, checksum and cert-dir entry stripped.
Wrap, DGFX, PPCn, SBUSBMon and SBUSBMonL are byte-identical apart from the Win10 copy being Authenticode-signed (8712-byte cert). [C]

---------------------------------------------------------------------------------------------------

## 2. How the pieces chain (architecture)

```
 SBE.exe (.NET WPF, 32-bit)  ── CTAudEp.dll / SndCrUSB.DLL ("SoundCore") / CTHID.dll / HKDetect.dll
        │  COM (CADI = Creative Audio Device Interface)
        ▼
 HKLM\SOFTWARE\Creative Tech\CADI\SPIEP : {32CD1956}=CmdRtr  ──►  CmdRtr.dll (Command Router Enumerator)
        │ enumerates "routers" registered under HKLM\SOFTWARE\Creative Tech\Command Router\
        ├── \APO : {AA9E54D5-...} (APOMgr)  = {920449A1-FFE8-434C-A2BC-C0CC1582BBD9}
        │        APOMngr.dll ──► writes FX params to MMDevices\Audio\Render\{ep}\FxProperties
        │                        (+ XML presets, Software\Creative Tech\Device\VID_041E&PID_323B\...)
        │                              │  IMMNotificationClient::OnPropertyValueChanged  [I]
        │                              ▼
        │                 audiodg.exe ── KSUSBAPO64.dll (CTAPOContainerSFX/MFX on Win8.1+; LFX/GFX on Win7)
        │
        └── \SPI : {CAABAA19-...} (KSUSBSPI) = {920449A1-...}
                 KSUSBSPI32.dll ──► ksusbaud.sys filter: IOCTL_KS_PROPERTY 0x2F0003 (Creative KS prop sets),
                                    IOCTL_KS_ENABLE_EVENT 0x2F0007; HID (MI_03) Get/SetOutputReport;
                                    named events/mutexes Global\{...}

 Any ASIO host ── CtUsAs64.DLL ──► ksusbaud.sys render/capture pins directly (KS streaming IOCTLs)
```

* `{920449A1-FFE8-434C-A2BC-C0CC1582BBD9}` is the value stored for every router registration (APO, SPI, CADI\SPIEP).
  It also appears in APOMgr, CmdRtr and SPI. It looks like the IID of the common enumerator interface (IEndpointEnumeration). [C for values; meaning I]
* SBE.exe is 32-bit, so the control panel loads the **32-bit** set: KSUSBSPI32, APOMngr, CmdRtr. The 64-bit copies serve 64-bit
  clients and audiodg. [C: SBE.exe runs under WOW64; live module list shows CTAudEp/HKDetect/CTLoadRs/DSOUND/MMDevApi;
  SPI and CmdRtr are not resident right now, so they load on demand [I]]
* Command Router per-feature routing comes from `cfSB1600.ini` section `[{32CD1956-...}]`:
  `{24A019C9-CC42-4D0A-B2B6-C9B936634D08}_Config=0`, `{764D8FF3-530C-4417-A288-4A1E06284711}_Config=2`,
  `{62516197-CB95-42e2-8C02-2268E5F16796}_Config=2`, `ShowUnpairedEndpoint=1`. The ini comment reads "IUpmix as secondary
  interface, SpeakerConfig and SpeakerConfig_v2 use APO only". {24A019C9} and {764D8FF3} are feature IIDs found in SPI, APOMgr
  and SndCrUSB. [C] Reading of the numbers: 0 = default/primary provider, 2 = APO-only. [I]

### 2.1 GUID cross-reference (which binary shares which GUID)
I extracted every GUID that KSUSBSPI64's code references (lea/mov rip-relative) and searched for its binary form in the other binaries [C]:
* **Shared with ksusbaud.sys, so these are kernel KS property/event sets SPI talks to (hand to the kernel RE topic):**
  `{C4BD0504/050B/050D/0510/051B/051E/051F-C3A0-407C-A08B-EB2CFB843E9F}` (a Creative sequential family),
  {447845DF-5610-460A-B1A4-BF2C1DA34F16} (81 call sites), {7BCEC54A-C7CD-46DB-BE2C-FA09DDD2AF78},
  {91D3A283-032E-40FB-9E21-9F5D2447BEA1}, {EE34D1AB-E2D2-425D-912A-D96F1A5C205A} (56 sites), {9ED25A7C-1E54-4ECC-B56D-0952B5F7BCA3},
  {7B6A44BD-8361-46F7-B765-0E51DF220647} (53 sites), {E76BCBF4-...}, {0AD1B8CC-...}, {B2DCBDE6-...}, {12C8A4F9-...},
  {5FA7CE37-243F-417B-B503-AF4633DA5799} and {12C98F26-05CD-458C-829E-219E482FAD8E} (also in KSUSBAPO64 and APOMgr, i.e.
  features that may live in either the kernel or the APO), plus KSPROPSETID_Pin/Topology/Connection.
* **Shared with SndCrUSB.DLL (+APOMgr)** = feature-interface IIDs exposed up to the UI (about 70 GUIDs, e.g.
  {24A019C9}, {764D8FF3}, {36F354C3}, {B92DE081}, {E8FE922B}, ...).
* **Shared by KSUSBAPO64, APOMgr and CTAudEp** = MMDevice API: CLSID_MMDeviceEnumerator {BCDE0395-...},
  IID_IMMDeviceEnumerator {A95664D2-...}, IID_IMMEndpoint {1BE09788-...}, and PKEY GUIDs {F19F064D} (DeviceFormat),
  {E4870E26} (OEMFormat), {1DA5D803}, {233164C8}, {294935CE}/{568B9108}/{F8679F50}/{D81A7E53} (one family).
* **KSUSBAPO64 only:** IID_IMMNotificationClient {7991EEC9-7E89-4D85-8390-6C703CEC60C0} (2×). The APO subscribes to
  endpoint property changes, which is how control-panel writes reach the DSP. [C for presence; mechanism I]

---------------------------------------------------------------------------------------------------

## 3. IPC mechanisms (complete list found)

| Channel | Type | Owner → peer | Data | Tag |
|---|---|---|---|---|
| `HKLM\...\MMDevices\Audio\Render\{ep}\FxProperties` | registry / IPropertyStore | APOMgr (string `%s\%s\FxProperties` and `SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\`) → KSUSBAPO64 | Serialized PROPVARIANTs keyed by Creative PKEYs (§5). ACL: **BUILTIN\Users = QueryValues, SetValue** on FxProperties, so no elevation needed | [C] |
| IMMNotificationClient | COM callback | MMDevAPI → KSUSBAPO64 | OnPropertyValueChanged(PKEY), then the APO re-reads its parameters | [I] |
| `Global\{6AADE933-E5A7-467f-B6C8-1967E19470B1}` | named **event** (CreateEventW) | KSUSBSPI (funcs 0x42b4f0, 0x42cfc0, 0x434698, 0x441a20) | "settings changed / notify" broadcast between SPI instances (the UI process and others) | [C type; I meaning] |
| `Global\{3D3638AD-CF85-4e29-A9BD-DEC95CD8B162}` | named **event** | KSUSBSPI (0x43f370) | device arrival / refresh | [C type; I meaning] |
| `Global\{6B174228-FBE9-4c78-AB87-96BC849F399C}`, `Global\{4FC930BA-1BAA-43e6-BD51-96E04D102398}` | named **mutexes**, taken with WaitForSingleObject(INFINITE) | KSUSBSPI (0x440564) | serialise device/registry access across processes | [C] |
| `Global\HIDInterrupt_%s` | named object (event) | KSUSBSPI 0x42e138; also present in SBUSBMon | HID interrupt-IN report arrival (E1 MI_03 HID) | [C name; I type] |
| `Global\%s` (formatted) | named event, plus 2 waitable timers | KSUSBSPI 0x42ad0c/0x42b4f0 | per-endpoint event (`{CAABAA19-...}_%s` naming) | [C] |
| `Global\MutexForHIDReport_VID0x%04X_PID0x%04X[_SRN%s]` | named mutex | CTHID.dll (control panel) | serialises HID Set/Get output reports to the E1 HID interface | [C] |
| `Global\{1B28DA20-E753-46f7-8666-6C757D9EBD14}`, `Global\{154D96E8-CC81-4efb-91E0-A9E0EAAC6436}`, `Global\{87579689-5C05-4e31-B27A-21355CFC0086}` | single-instance mutexes | SBUSBMon / SBUSBMonL | prevent duplicate monitors (dormant on this system) | [C] |
| `\\.\pipe\ctaudsvc` | named pipe, **CallNamedPipeW** | CTAudEp.dll → "ctaudsvc" service | **The server is not installed** (no service, no binary), so the path is dormant | [C] |
| KS IOCTLs on the ksusbaud filter/pins | DeviceIoControl | KSUSBSPI, CtUs*As* | see §4 and §6 | [C] |
| `\\.\HCD%d` + IOCTL 0x220424 (USB_GET_ROOT_HUB_NAME) / 0x220408 (GET_HCD_DRIVERKEY_NAME) | DeviceIoControl | KSUSBSPI 0x4385cc / 0x438920 | detects whether the E1 sits on a USB 2.0 root hub ("ROOT_HUB20", `PCI\VEN_%x&DEV_%x...`) | [C] |
| HID.DLL (HidD_SetOutputReport / GetInputReport / GetAttributes) | HID | KSUSBSPI, CTHID, SBUSBMon, APOMgr | vendor HID reports on MI_03 (button/LED/feature control) | [C APIs] |
| SendMessage / PostMessage / keybd_event | window msgs, synthetic keys | SBUSBMon (PostMessageW, keybd_event), SPI (SendMessageW, dialog) | SBUSBMon turns HID events into media keys [I] | [C imports] |
| CreateFileMapping/MapViewOfFile | shared memory | only CTIntrfu.dll / CTLoadRs.dll (control-panel resource helpers) | no shared memory exists between the UI and the APO or kernel | [C] |
| KSUSBAPO64 debug dump | files | `%c:\DumpData\%s.raw` / `.wav` | raw PCM dump capability (directory absent here) | [C string] |

---------------------------------------------------------------------------------------------------

## 4. KSUSBSPI64 (hardware control backend) details
* Imports: `ksuser!KsCreatePin`, DeviceIoControl, SetupDi*, HID.DLL, CreateWaitableTimerW. [C]
* DeviceIoControl call sites and codes [C]:
  * `0x42a124`: **IOCTL_KS_ENABLE_EVENT 0x2F0007** (KS event registration: control-change or jack notifications).
  * `0x45d564, 0x45d5cc, 0x45d6b0, 0x45d904, 0x45da44`: **IOCTL_KS_PROPERTY 0x2F0003** (pin, topology, connection and Creative sets).
  * `0x45dc8c`: KsCreatePin (it builds a KSPIN_CONNECT with KSDATAFORMAT_TYPE_AUDIO / SUBTYPE_PCM / SPECIFIER_WAVEFORMATEX, at 0x45ca88).
  * `0x4385cc` / `0x438920`: USB host-controller IOCTLs 0x220424 / 0x220408 (see §3).
* Pin-category → mixer-name mapping (func 0x43e698): KSNODETYPE_SPEAKER, HEADPHONES, MICROPHONE, LINE_CONNECTOR,
  SPDIF_INTERFACE, DIGITAL_AUDIO_INTERFACE (= What U Hear). [C]
* Registry used: `Software\Creative Tech\Device\VID_%04X&PID_%04X\Defaults` (mixer defaults, e.g. SpeakerVol=0xC0000000,
  MicInMute=1, `DefaultPlayback[00/01]`=0xFFFFE800 (= -6144, i.e. -24 dB in 1/256 dB units [I]), THX*_isEnabled flags),
  `KSAud\MixerName\%s`, `KSVSPI\MixerName`, `SYSTEM\CurrentControlSet\Control\CreativeInstaller\USB\VID_%04X&PID_%04X&MI_00`,
  `Software\Creative Tech\ASIO Device\{Selected Device|All Device}\ASIODevice%03d`. Value names
  `ASIOSampleRate`, `ASIOBitDepth`, `DirectMonitorControl_AllMics_IsEnabled`, `DirectMonitorControl_LineIn1_IsEnabled`,
  `DirectMonitorControl_Mic_Level`, `UHearRecVol`. [C strings]

---------------------------------------------------------------------------------------------------

## 5. Registry layout (live, decoded)

### 5.1 `HKLM\SOFTWARE\Creative Tech\` (64-bit view) [C]
* `KSAud\VID_041E&PID_323B`: `WaveOutName`, `WaveInName`, `MixerName` = "Sound Blaster E1" (written by INF `SB1600.AddReg` via `DevInfoSB1600`).
* `KSAud\MixerName\English`: short code → display name table (`UHear`="What U Hear", `UHearRecVol`="What U Hear Recording Volume", ...). `KSVSPI\MixerName` is the inverse table.
* `KSUSBAud\ProductInf` = "ksUSBaud.inf" (**Win10 INF only**, written by `KS_NAMES.AddReg`).
* `CADI\SPIEP\{32CD1956-...}` = {920449A1-...}; `Command Router\APO\{AA9E54D5-...}` = {920449A1-...}; `Command Router\SPI\{CAABAA19-...}` = {920449A1-...} (mirrored under WOW6432Node).
* `Device\VID_041E&PID_323{A,B,C,E}\Defaults`: hardware mixer and THX defaults (values listed in §4).
* WOW6432Node: `Software Installed\APOIM\1033` (Version 3.47.00, path System32), `Software Installed\ASIO Driver USB\1033` (1.02.00),
  `Installation\CTRedist\APOIM\SharedApps\{41AE3714-371B-40A5-B28D-5F242C19109A}_APOIM`="1" (refcount; the same GUID is the `/g:` argument passed to KSUSBAIM).
* `HKLM\SYSTEM\CurrentControlSet\Control\Creative Tech\Coinstaller`: `ksUSBdvinst`="ksUSBaud.ini", `SourcePath`=DriverStore folder.
* `HKLM\SYSTEM\CurrentControlSet\Control\CreativeInstaller\USB\VID_041E&PID_323B&MI_00\setupStatus`="0" (other PIDs show 3758096907 = 0xE000020B).

### 5.2 HKCU [C]
* `Software\Creative Tech\CtUsAsio`: `Latency`=DWORD **100** (ms), `SampleRate`=QWORD holding an IEEE double = **44100.0** (0x40E5888000000000). `BitDepth` is absent, so the default applies.
* `Software\Creative Tech\Audio Endpoint Selection\Sound Blaster E-Series Control Panel`: `Render`/`Capture` = MMDevice IDs
  `{0.0.0.00000000}.{9b883784-...}` / `{0.0.1.00000000}.{2ec6b950-...}`. **Stale**: they do not match the current E1 endpoints {7e65a67a}/{389c9c75}.
* `Software\Creative Tech\Sound Blaster E-Series Control Panel\Settings\USB_VID_041E_PID_323B\Avatar`="".

### 5.3 Device instance / interface keys [C]
* Driver key `{4d36e96c-...}\0011`: `CoInstallers32`=KsUSBDvInst.dll,CtDevCoInstProc; `Driver`=ksusbaud.sys;
  `NTMPDriver`="ksusbaud.sys,sbemul.sys"; `AssociatedFilters`="wdmaud,redbook"; `SetupPreferredAudioDevices`=1;
  `InfSection`=SB1600_Device.6.3 (+.NTamd64); `DriverVersion`=1.13.1.1.
* Interface key `DeviceClasses\{6994ad04-...}\##?#USB#VID_041E&PID_323B&MI_00#...\#{101DE7F6-62E6-4f36-BCCD-5A6F71AD5FC4}\Device Parameters`
  (KSNAME_GLOBAL = {101DE7F6...}) holds `EP\0`, `EP\1`, `FX\0`, `FX\1` exactly as the INF writes them. The engine copies them into MMDevices.

### 5.4 Creative FX PKEYs (FxProperties of the render endpoint {7e65a67a}), decoded from INF [Strings] and live values [C]
Blob format in MMDevices: `DWORD vt, DWORD 1, payload`. Blob format in INF/interface key: `WORD vt, WORD pad, DWORD cb, payload`.

| PKEY | INF name | Live value |
|---|---|---|
| {D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},0/1/2/5/6 | FX_Association / PreMix(LFX) / PostMix(GFX) / SFX / MFX | SPEAKER / {559493DD} / {2EF9BA5B} / {E9B73398} / {41528545} |
| {D3993A3F-99C2-4402-B5EC-A92A0367664B},5/6 | SFX/MFX ProcessingModes_Supported_For_Streaming | {C18E2F7E-...} (DEFAULT mode only) |
| {5B4777A4-8AD4-4d34-893A-DF34DA0E56CA},0 | SBX Surround enable | VT_BOOL TRUE |
| {A5A78EA4-C156-4db7-85AA-81CFF1C3F192},0 | Surround immersion | float 0.4 |
| {3CD83C04-868F-4f08-8D75-B4625FFE3B31},0 | Crystalizer enable | TRUE |
| {0F03F0BB-72C7-4ec1-8422-7B8D7410694A},0 | Crystalizer level | float 0.65 |
| {3C663165-48FC-41d8-8120-81CB0B12B230},0 | Crystalizer pre-attenuation | float 1.0 |
| {59FBB5C6-4C98-4741-ADC0-40ABB474A963},0 | Speaker mode (0 ext, 1 integrated, 2 headphone) | 2 |
| {E8D2D16C-A666-44a0-BB77-3051F7384D22},0 | Headphone override | TRUE |
| {156AC5B7-384A-4b15-A3A4-79350563FC88},0..5,10..20 | "Scout mode" block | ,0 F; ,1 T; ,2 0.9; ,3 0; ,4 T; ,5 0; ,10 2.0; ,11 3.0; ,12 3.0; ,13 -2.0; ,14 -2.0; ,15 0; ,16 3.0; ,17 4.0; ,18 3.0; ,19 2.0; ,20 F |
| {40D0D021-20BD-4d15-A93C-1DBE8922C642},1 (FX\1 mic) | CrystalVoice noise reduction | TRUE |
| {40D0D021-...},11 (mic) | Mic-array inter-mic distance | 2 floats 0.042 (VT 0x0004 with odd flags 0x63) |
| {B725F130-47EF-101A-A5F1-02608C9EEBAC},10 | display name | "Creative System Effect" |

Endpoint (EP) PKEYs from the INF: {BB282A80-2A05-4c17-AA45-9910F8F7D30D},0 = CommandRouter settings override; {872572B3-...},0 /
{872572B4-...},0 = Endpoint / THX settings override → all = `C:\ProgramData\cfSB1600.ini`; {F1056047-B091-4d85-A5C0-B13D4D8BAC57},0/1 =
EffectNodeInfo render/capture → {B293EF73}; {C949C6AA-132B-4511-BB1B-35261A2A6333},0 = SoundCore control CLSID {495E4C24-85ED-4f19-885E-C2D01D7EA26C}
(KSMALCCTL); {F5376650-918F-4CF4-91FB-D123CF4E1350},1 = supported-SC endpoint {ac6d33b1-0629-4d06-a0d6-73d896269347};
{F3E80BEF-1723-4FF2-BCC4-7F83DC5E46D4},4 (EnableEndpointByDefault) = 0x101 render / 0x201 capture.
Unused chain keys (both INFs, all commented out): PKEY_APO_NUM {d46c4dfd-28b8-4ab8-a45d-70399958ef26},0;
PKEY_GFX_APO1/2 {e1032fc5-ddab-4c4b-9940-8a6be78a4db6},0/1; PKEY_Chain_Encoder {03C89A4E-CCF3-4f3e-98E1-22CB05513326},0;
PKEY_DGFX_Encoder_Selection {D04E05A6-...},131072. **KSUSBWrap64 contains these exact GUIDs in binary form** at 0x401ec8 ({e1032fc5}),
0x401ef8 ({d46c4dfd}), 0x401f28 ({03C89A4E}) and 0x401c48 ({D04E05A6}). So DaisyWrap reads its child CLSIDs and count from the endpoint FX store. [C]

---------------------------------------------------------------------------------------------------

## 6. ASIO driver (CtUsAs64.DLL): how it talks to the kernel  (quality-critical)

* **Packaging** [C]:
  * Win7 pkg: `CtUsAs64.dll`/`CtUsAsio.dll` listed in the driver INF (`SfxFx.CopyList.*`, `SfxFx.RegisterDlls.*` → regsvr) **and**
    in `extras\Audio\CtUsASIO` ("Creative ASIO (USB)" 1.00.00, Bin.cab dated 2014-04-17, FileVersion "1.0.01.0000-1.00.0000 (Beta-Release)").
  * Win10 pkg: removed from the driver INF and **kept** in `extras\Audio\CtUsASIO` 1.02.00 (CtUsAs64 **1.1.1.0**, 2014-09-01, ProductGUID
    145367F9-2550-4835-8313-396C31320F0D, `ASIOUSB.ini` RegSvrList installs `<WINSYS64>\CtUsAs64.DLL` + `<WINSYSDIR>\CtUsAsio.DLL`, OSMinimumReq=WINVISTA).
  * Live: `C:\Windows\System32\CtUsAs64.DLL` 1.1.1.0 (**unsigned**), `SysWOW64\CtUsAsio.DLL`; `HKLM\SOFTWARE\ASIO\Creative Sound Blaster ASIO Device`
    → CLSID {B2D4D5A2-...}, Description "Creative Sound Blaster ASIO" (both registry views). ASIO4ALL v2 is also present.
* **Transport** [C]: the code is built on Microsoft's KS helper classes (CKsFilter, CKsPin, CKsIrpTarget; strings "KSPROPERTY_PIN_DATARANGES",
  "Could not find a Render pin that supports the given wave format", ...).
  * Enumerates filters with SetupDiGetClassDevsA(interface class, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE=0x12) (func 0x18000ab20). Categories are
    built inline (0x6994AD04 / 0x93EF / 0x11D0 = KSCATEGORY_AUDIO; 0x65E8773E/D / 0x8F56 = RENDER/CAPTURE) together with SetupDiGetDeviceInterfaceAlias (func 0x180003ad0).
  * Opens the filter with CreateFileA. Pins are created through `ksuser!KsCreatePin` (loaded dynamically), with a KSPIN_CONNECT carrying a WAVEFORMATEX(TENSIBLE) (KSDATAFORMAT_SPECIFIER_WAVEFORMATEX).
  * `CKsIrpTarget::SyncIoctl` (0x18000e37c): overlapped DeviceIoControl + event wait. **IOCTL_KS_PROPERTY 0x2F0003** is used for state
    (KSSTATE_STOP/ACQUIRE/PAUSE/RUN), pin properties and topology.
  * `CKsPin::WriteData` (0x18000cdf0): **IOCTL_KS_WRITE_STREAM 0x2F8013**. `ReadData` (0x18000ce70): **IOCTL_KS_READ_STREAM 0x2F4017**.
    This is classic standard-streaming KS (KSSTREAM_HEADER packets), **not** WaveRT and not audiodg.
  * Streaming thread: AVRT `AvSetMmThreadCharacteristicsA("Pro Audio")` + AvSetMmThreadPriority. [C imports + string]
  * Before using a filter it queries a private KS property `{8AD4EC32-C30C-40B8-B0B9-5623075249B3}`, id 1, ULONG (func 0x1800079f0). **Neither ksusbaud.sys build contains this GUID**,
    so it is a probe for another Creative driver family. The E1 driver doesn't answer it. [C absence; I purpose]
* **Channel naming by pin category** (funcs 0x180007f20..0x180008280): SPEAKER→"Line-Out L/R", MICROPHONE→"Mic L/R",
  LINE_CONNECTOR→"Line-In L/R", DIGITAL_AUDIO_INTERFACE→**"What U Hear L/R"** (string present **only in 1.1.1.0**; the Win7 beta has only
  "Line In L/R", "Mic L/R", "Line Out L/R"). [C]
* **Settings table** (Win10 file offset 0x27d18, Win7 0x258a0; identical contents) [C]:
  * Latency (ms): `{1, 2, 4, 5, 6, 8, 10, 20, 40, 50, 60, 80, 100}` (13 entries).
  * Bit depth: `{16, 24}`.
  * Sample rate: `{44100, 48000, 96000}`. **No 88.2k, 176.4k or 192k.**
  * Stored at `HKCU\Software\Creative Tech\CtUsAsio\{Latency (DWORD), BitDepth (DWORD), SampleRate (QWORD = double)}`. The control-panel dialog is
    `IDD_ASIOCP_MALCOLM` ("Preferences", "Latency :").
  * The latency-change handler (0x180002c30) computes **buffer frames = latency_ms × sampleRate / 1000** (multiply by 0x10624DD3, then >>6), then calls the
    host callback with kAsioLatenciesChanged(6), kAsioBufferSizeChange(4, frames), kAsioResetRequest(3). [C]
  * Clock source: one entry, "Internal Clock". [C]
* **Why this matters**: for the E1 the ASIO path is the only Creative path that is bit-transparent at 24/96. The shared-mode WASAPI path runs at the
  44.1 kHz OEM default with SBX/Crystalizer processing. A new driver should expose WaveRT pins usable in WASAPI exclusive mode at
  44.1/48/96 kHz, 16/24-bit, which gives ASIO-equivalent transparency without a custom ASIO DLL. [I]

---------------------------------------------------------------------------------------------------

## 7. What U Hear (WUH) implementation
* Live capture endpoint {1d53f1ec-fad3-4484-a8c8-b0562ba0c1ec} "What U Hear": FormFactor 7 (UnknownDigitalPassthrough), JackSubType/Association
  KSNODETYPE_DIGITAL_AUDIO_INTERFACE {DFF21FE2-...}, DeviceFormat 16-bit/44.1k/2ch PCM, **no FxProperties** (raw). The pin path list shows
  `...{101DE7F6...}/Software_IO/6` and `/Physical_External/5`, which suggests a software (driver-internal) source on pin 6. [C data; I meaning]
* ksusbaud.sys (both builds) contains the WUH pin-name GUID KSAUDGUID.HWMixIn {A186641E-3A64-47c6-BBB4-04BFF34A8AA2} ("UHear"),
  record-volume node {BD7BBBF4-D6E6-4edc-B3D4-4ADAE2CE0454} ("UHearRecVol"), record-mute {ED323662-958A-45c7-95FA-DF87284AB6FF} ("UHearRecMute"),
  plus the other pin/node GUIDs from the Win7 INF [Strings]. The Win10 kernel also embeds the English names ("What U Hear", "What U Hear Recording Volume").
  The Win7 kernel holds only the short codes. [C]
* So WUH is a **kernel-side capture pin with its own volume/mute nodes**, exposed by ksusbaud.sys. That it is a loopback of the render mix in software
  (the E1 likely has no HW loopback) is [I] and needs confirmation in the kernel RE.
* ASIO 1.1.1.0 exposes WUH as an ASIO input pair; SPI maps the DIGITAL_AUDIO_INTERFACE category; SPI writes `UHearRecVol`. [C]
* For a new driver: Windows' native WASAPI loopback (AUDCLNT_STREAMFLAGS_LOOPBACK) replaces WUH for apps, and a driver-level loopback pin is only needed for parity. [I]

---------------------------------------------------------------------------------------------------

## 8. Co-installer and installer scripts
* `KsUSBDvInst.dll!CtDevCoInstProc` is registered via `COINST.AddReg` (`CoInstallers32`). On DIF_INSTALLDEVICE it spawns
  `rundll32 <dll>,RCAudioDriverSetup /task:Install /nproc:bmar /dev:%s %s`, the Creative script engine that executes `ksUSBaud.ini`. [C string + setupapi log
  "CoInstaller 1 == KsUSBDvInst.dll,CtDevCoInstProc" Enter/Exit at 20:56:17 today]
* **ksUSBaud.ini / ksusbchimp.ini / cfSB*.ini / APOIM.ini are obfuscated with a chained XOR**: `plain[i] = enc[i] XOR enc[i-1]`, with `enc[-1] = 0xFF`
  (plain[0] is '['). [C: all files decode to clean text] Every enc byte has bit 7 set. To encode: `enc[i] = plain[i] XOR enc[i-1]`.
* Installed copies [C, md5-identical to the Win10 package]: `C:\Windows\System32\ksUSBaud.ini`, `C:\Windows\System32\ksusbchimp.ini`,
  DriverStore `ksusbaud.inf_amd64_cef47356c80e391b\ksUSBaud.ini`, `C:\ProgramData\cfSB15{80,90}.ini, cfSB16{00,10}.ini`,
  `C:\Program Files\Creative\PreInst\ksusbchimp.ini\...` (pre-install staging copy).
* ksUSBaud.ini (installer script, sections like `[Mapper]`, `[DeviceList]`, `[AudioDriver.Install.Pre/Main/Post]`, `[ExeFiles.*]`) [C]:
  * `[ExeFiles.Install.WDM.B.amd64.6]`: `regedit /s UsbMixerDefault.reg`, `regedit /s UsbDefaultVista.reg`,
    `KSUSBAIM64.exe /hidemsg /1: /s /g:{41AE3714-371B-40a5-B28D-5F242C19109A}` (installs APOIM).
  * `[ExeFiles.Install.WDM.A.AMD64.6]`: regsvr32 /S of the 32-bit KSUSBAPO32, KSUSBSPI32, **KsUSBMalc32.dll (not shipped)**, KSUSBDGFX32.
  * Win10 only: `[ExeFiles.Extra.Install.WDM.B.amd64.6]` runs `AddCat.exe` on `KSUSB{APO,Wrap,DGFX}{64,32}.hda`. The .hda files are the
    APO **catalog files** (same size as the Win7 `Support\AMD64\*.cat`), copied to %WINDIR% and added to the system catalog DB so audiodg's signature
    check passes. Win7 ran AddCat on `%01%\*.cat` from the Support folder instead. [C]
  * `[DelReg.Extra.WDM.B]` deletes Run values "Creative SB U1/U3/USB Monitoring Utility Launcher", so the SBUSBMon autostart is removed. [C]
  * Win10: `[Mapper]`/`[DeviceList]` add PID_323C; uninstall `DelFiles` sections added.
* ksusbchimp.ini: the same engine for the multi-INF package (thisInf1/2 = ksUsbaud.inf / SB1100.inf, order swapped in Win10), with a USB2-specific install flow
  (`driverPackage.USB2.Install`). This is where SPI's USB2 root-hub detection is consumed [I].
* cfSB1600.ini (identical W7/W10): per-device defaults read by APOMgr/CmdRtr (GetPrivateProfileString):
  SpeakerConfig {E8D2D16C},1000_Default/Count/GUIDs, `1002_Default="Software\\Creative Tech\\KSVSPI\\SpkDef_VID_041E&PID_323B"`; CMSS
  {6FBE372C},0..2_Default=1; Crystalizer {7C060DBC},0_Default=1; speaker mode {59FBB5C6},0_Default=2 and available modes {3F783545} = {0,2};
  THX Surround enable 1 / immersion 0.4; THX Crystalizer enable 1 / level 0.65 / pre-att 1; XBass {F67CF426} and {0519A199} = 0; NR {40D0D021},1 = 1;
  master features {3C14ECCC},0_Features=47 (0x2F), 0_FeaturesVIP=149 (0x95). [C]

---------------------------------------------------------------------------------------------------

## 9. INF diff (Win7 1.10.03.01 inf7 vs Win10 1.13.1.1 inf10)
Note: the shared `scratchpad\inf10.txt` is **corrupt** (ASCII mis-decoded as UTF-16, then saved as UTF-8). Clean copies are at
`scratchpad\decomp\inf7_clean.txt` / `inf10_clean.txt`. The installed `DriverStore\...\ksUsbaud.inf` is byte-identical to the Win10 package INF. [C]

| Area | Win7 INF | Win10 INF | Tag |
|---|---|---|---|
| DriverVer | 04/17/2014, 1.10.03.01 | 07/21/2015, 1.13.1.1 | [C] |
| Models | `[CREATIVE]`/`.ntamd64`/`.ntx86.6.3`/`.ntamd64.6.3`: **only PID_323B (E1)** | adds PID_323A (X7, was "E7"), 323C (E5, was "R5"), 323E (E3); 323A/323C use the non-6.3 section even on 6.3 | [C] |
| `[SignatureAttributes]` | commented out | `KSUSBAPO32/64.dll = PETrust=true` | [C] |
| ASIO | CtUsAsio.dll / CtUsAs64.dll in SourceDisksFiles, SfxFx.CopyList, SfxFx(32).CopyList, SfxFx.RegisterDlls | **removed** (moved to the separate extras installer) | [C] |
| APOIM CopyList | ResDefE.exe, KSUSBAIM*.exe | + AddCat.exe + `KSUSB{APO,DGFX,Wrap}{32,64}.hda` | [C] |
| `[SB1600.Interface.Wave(.6.3)]` AddReg | `...SysFx1600(.6.3).AddReg, SysFx1600.APOChain.AddReg, SB1600.SysEP.AddReg` | APOChain reference dropped (it was **all comments** in Win7 anyway, so no functional change) | [C] |
| `[SysFx.DelReg]` | deletes FX\0..FX\7 keys | deletes only FX\0 keys | [C] |
| `[KS_NAMES.AddReg]` | empty | `HKLM,"Software\Creative Tech\KSUSBAud",ProductInf,,"ksUSBaud.inf"` | [C] |
| [Strings] | contains ~270 KSAUDGUID/KSPin/KSNode/KSPinShort/KSNodeShort pin and node name GUIDs and strings (unused by any AddReg), `MediaCategories`, `MyMixerName`, `KSVSPIInfo` | removed (names now embedded in ksusbaud.sys); adds KSNODETYPE_PROCESSING_MICROPHONE_ARRAY, PERSONAL_MICROPHONE, DOWN_LINE_PHONE | [C] |
| **E1 FX registration** | `SysFx1600.AddReg` (OS < 6.3): LFX {559493DD} + GFX {2EF9BA5B} only. `SysFx1600.6.3.AddReg` (Win8.1+): LFX + GFX + **SFX {E9B73398} + MFX {41528545}** + ProcessingModes = DEFAULT | **identical** | [C] |
| **E1 endpoint formats (SB1600.SysEP.AddReg)** | EP\0 render `{E4870E26},3` (PKEY_AudioEngine_OEMFormat) = WFX-EXT 2 ch / **44100** / **24-bit** (valid 24) / mask 0x3 / PCM; `{E4870E26},0` (Creative name "OEMFormat_FLOAT") = 2 ch / 44100 / 32-bit float. EP\1 mic: OEMFormat = 1 ch / 44100 / 16-bit / mask 0x4 (FC) PCM; FLOAT = 1 ch / 44100 / 32f. | **identical** | [C] |
| PKEY_AudioEngine_DeviceFormat | defined in Strings, **never written** | same | [C] |
| Disable enhancements (`PKEY_AudioEndpoint_Disable_SysFx` {1DA5D803},5) / format restrictions | none | none | [C] |
| UI CLSID (`PKEY_SYSFX_UiClsid` → {361E7F16}) | commented out | commented out | [C] |
| SBUSBMon Run key (`SBAVMon.AddReg`) | commented out | commented out | [C] |
| Co-installer, service (`KsUSBaud`, demand start, kernel), wdmaud/KS includes, `mssysfx.CopyFilesAndRegister` | same | same | [C] |

Practical consequences [I]:
* On Windows 8.1/10/11 the engine uses the **SFX + MFX** containers (the LFX/GFX keys are ignored once SFX/MFX keys exist). On real Windows 7 the same
  DLL ran as **LFX + GFX**. Installing either package on Win11 selects the `.6.3` section (confirmed by setupapi: `[SB1600_Device.6.3.NTamd64]`),
  so the Win7 package would behave the same on Win11 at the INF level. Any audible Win7 vs Win10 difference must come from the
  kernel (1.10.03.01 vs 1.13.1.1), the APO build (1.2.32 vs 1.2.47) or the Windows engine itself, not from the INF.
* The default format was 24/44.1 in both, so the Win7 "good sound" did not come from a different default format.

---------------------------------------------------------------------------------------------------

## 10. Live Windows 11 state (read-only inspection, 2026-11-14)
* E1 endpoints [C]:
  * Render **{7e65a67a-04d8-403f-b379-885cdec445d5}** "Speakers (Sound Blaster E1)", state ACTIVE.
    `{F19F064D-...},0` **DeviceFormat = WAVE_FORMAT_EXTENSIBLE, 2 ch, 44100 Hz, 24-bit (valid 24), mask 0x3, KSDATAFORMAT_SUBTYPE_PCM**
    (blob `41000000 01000000 FEFF0200 44AC0000 98090400 0600 1800 1600 1800 03000000 <PCM GUID>`). OEMFormat {E4870E26},3 is the same.
    {E4870E26},0 = float32/44.1k. {E4870E26},1 = 100000 hns (10 ms, OEM period). Engine mix formats {3D6E1656},2/3 and {624F56DE},3 = float32/44.1k/2ch.
  * Capture {389c9c75-...} "Microphone": ACTIVE, 16-bit/44.1k/mono (mask 0x4), FX = Creative SFX/MFX/LFX/GFX.
  * Capture {3ea0f972-...} "Microphone": NOT PRESENT (stale duplicate).
  * Capture **{1d53f1ec-...} "What U Hear"**: ACTIVE, 16-bit/44.1k/stereo, no FX.
* COM registration (both views) points at System32/SysWOW64 as listed in §1. APO registry `HKCR\AudioEngine\AudioProcessingObjects` contains Creative
  entries (APO flags: LFX/SFX/MFX = 13, GFX/EFX = 5, DaisyWrap = 15). [C]
* Signatures: KSUSBAPO64, KSUSBDGFX64, KSUSBWrap64, KSUSBSPI64, KSUSBPPLD64, KsUSBDvInst and ksUSBaud.sys are **Valid / catalog-signed by Microsoft Windows Hardware
  Compatibility Publisher**. **CtUsAs64.DLL is NotSigned** (it isn't loaded by audiodg, so that's fine). [C]
* Processes: SBE.exe (32-bit .NET) running, started from HKLM Run `"...\SBE.exe" /r`. Loaded Creative modules: CTLoadRs, HKDetect, CTAudEp. No SBUSBMon
  rundll32. No ctaudsvc service. [C]
* audiodg (PID 7880) modules: **unreadable** at Medium integrity (`tasklist /m` → N/A). **Whether KSUSBAPO64 is loaded is unknown.** Run an elevated
  `tasklist /m KSUSBAPO64.dll` while audio plays on the E1 to settle it. [C]
* Event logs: Microsoft-Windows-Audio/Operational has 2181 events, all of type 65/177–180 (state changes, standby). Nothing mentions the E1, the
  Creative CLSIDs or APO failures. The Application log has an AUDIODG.EXE crash 2026-10-20 03:28 (ntdll, 0xC0000374 heap corruption) with an unknown module. [C]
* setupapi.dev.log (today 20:56:17): device reinstalled from oem57.inf, section `SB1600_Device.6.3.NTamd64`, rank 0x00FF0001 WHQL. It beat
  the inbox `wdma_usb.inf` (USBAudio, rank 0x00FF2005) and the new **`usbmidi2.inf` "USBMidi2-ACX"** (rank 0x00FF3105). RegisterDlls ran for KSUSBAPO32/64,
  KSUSBPPLD64, KSUSBSPI32/64, KSUSBWrap64, KSUSBDGFX64; co-installer ran; one warning "Value 'FriendlyName' should not be set directly". No errors. [C]

---------------------------------------------------------------------------------------------------

## 11. Implications for the new C driver stack [I]
1. Make the kernel driver expose WaveRT pins with **44.1/48/96 kHz × 16/24-bit**, the set ASIO proves the hardware path supports. Set
   PKEY_AudioEngine_OEMFormat to 24-bit/48 kHz (or let the user choose) instead of Creative's 24/44.1. Support WASAPI exclusive/event mode. That
   reproduces the ASIO-grade bit-transparent path without CtUsAs64.
2. Replace the Command Router / APOMgr / SPI stack with a single user-mode control service or library that:
   * talks to the kernel through one documented private KS property set (instead of the ~15 Creative sets),
   * uses HID (MI_03) with a `Global\MutexForHIDReport_VID0x041E_PID0x323B` style mutex for serialisation,
   * writes DSP parameters to FxProperties (Users already have SetValue) and lets a modern APO pick them up via IMMNotificationClient,
     or better, via the APO's documented IAudioSystemEffects3 / IAudioProcessingObjectNotifications (Win11).
3. Ship the DSP APO as an SFX/MFX pair with `AUDIO_SIGNALPROCESSINGMODE_DEFAULT` and `RAW`, so pro apps can bypass it. The Creative INF
   advertises only DEFAULT, so RAW-mode streams get no bypass declaration.
4. Drop the DaisyWrap chainer, DGFX encoder shell, PPCn, SBUSBMon, co-installer and .ini scripting. None of them has any function for the E1.
5. Implement What U Hear either as a kernel loopback pin (parity) or rely on WASAPI loopback.

## 12. Open items
* Confirm whether audiodg actually loads KSUSBAPO64 on Win11 26300 (needs elevation).
* Exact semantics of SPI's Creative KS property sets (kernel RE topic): IDs and payloads for the {C4BD05xx-...} family, {447845DF}, {7BCEC54A}, {EE34D1AB}, {7B6A44BD}, ...
* Whether WUH in ksusbaud.sys is a software loopback of the render stream or a hardware path.
* APOMgr XML preset location (`DefaultPreset.xml`, `OEMDefaultPreset.xml`, `%SU%lX-%lX.xml`) and exact write path (IPropertyStore vs RegSetValue). Its
  imports include RegSetValueExA, and a `%s\%s\FxProperties` format string is present.


---------------------------------------------------------------------------------------------------

## Verification

An adversarial verifier re-checked this note on 2026-11-14. Every check below was redone directly against the package binaries
(`w7`/`w10` `...\Bin\AMD64`, `asio_w7`/`asio_w10`, `aim_w10`), the INFs as shipped and the live registry and USB device. The verifier's helper
scripts are in `%TEMP%\claude\...\scratchpad\verify\` (dz.py, rawcalls.py, back.py, gs.py, xd.py and others). No Creative code was copied here.
Verdicts: **CONFIRMED**, **CORRECTED** (partly wrong; the right value is given), **REFUTED**, or **UNVERIFIABLE**.

### V.1 Verdicts on the key claims

| # | Claim (short) | Verdict | Verifier's evidence / correction |
|---|---|---|---|
| 1 | Render DeviceFormat = WFX-EXT 2 ch / 44100 / 24 (valid 24) / mask 0x3 PCM; mix float32/44.1k; mic 16/44.1 mono; WUH 16/44.1 stereo | **CONFIRMED** | Live `{f19f064d},0` = `41000000 01000000 FEFF 0200 44AC0000 98090400 0600 1800 1600 1800 03000000 +PCM GUID`. This is byte-identical to `{E4870E26},3`. `{3d6e1656},2/3` and `{624f56de},3` are float32/44.1k/2ch. Capture: {389c9c75} `FEFF 0100 44AC0000 ... 1000 ... 04000000` (16-bit mono FC); {1d53f1ec} WUH `FEFF 0200 44AC0000 ... 1000 ... 03000000` (16-bit stereo). **Minor correction:** `{E4870E26},1` = 100000 hns is present on **every** render endpoint on this PC (Realtek, LG, MacroSilicon, ...), so it is a Windows-engine value, not a Creative "OEM period". The WDK documents PKEY_AudioEngine_OEMPeriod as pid 6 [I]. |
| 2 | Win7 and Win10 INFs: identical E1 OEMFormat and FX CLSIDs; no Disable_SysFx; DeviceFormat never written | **CONFIRMED** | `decomp\inf{7,10}_clean.txt` are byte-identical (CR-stripped) to the shipped `ksusbaud.inf` files (102930 B / 74755 B). `[SB1600.SysEP.AddReg]`, `[SysFx1600.AddReg]` and `[SysFx1600.6.3.AddReg]` diff as **identical**. `[SysFx1600.APOChain.AddReg]` differs only in comment lines. In both, `PKEY_AudioEngine_DeviceFormat` appears only in [Strings]. `{1da5d803},5` occurs nowhere (live render Properties hold `{1da5d803}` pids 0/2/8 only). |
| 3 | ASIO in both packages and installed (CtUsAs64 1.1.1.0, unsigned, CLSID {B2D4D5A2}) | **CONFIRMED** | Live `System32\CtUsAs64.DLL` is byte-identical to `w10\extras\Audio\CtUsASIO` Bin.cab. FileVersion is 1.1.1.0 and Authenticode reports NotSigned (both DLLs). `HKLM\SOFTWARE\ASIO\Creative Sound Blaster ASIO Device` and its WOW6432Node twin hold CLSID {B2D4D5A2-1B17-4AB6-8A6D-667095C480B2}. `Software Installed\ASIO Driver USB\1033` Version = 1.02.00. Win7 has `CtUsAs64.dll` in the INF (lines 55/575/880) **and** in `w7\Audio\extras\Audio\CtUsASIO`. The `asio_w7` DLL is byte-identical to the INF copy. **Conflict in another doc:** `system-diagnosis.md` row 5 ("the Win10 package does not ship ASIO") is wrong. It only left the driver INF. |
| 4 | CtUsAs64 = pure KS client: 0x2F0003 / 0x2F8013 / 0x2F4017, KsCreatePin, MMCSS "Pro Audio" | **CONFIRMED** | `0x18000ce18 mov edx,0x2F8013` → `0x18000ce25 call DeviceIoControl` (WriteData). `0x18000ce98 mov edx,0x2F4017` → `0x18000cea5` (ReadData). In SyncIoctl, `0x18000e3bb` DeviceIoControl, then GetLastError/WaitForSingleObject, with a special-case `cmp ebp,0x2F0003` at `0x18000e40b`. Imports: AVRT `AvSetMmThreadCharacteristicsA`/`AvSetMmThreadPriority`. Strings: "Pro Audio" @0x1fd00, "KsCreatePin"/"ksuser.dll" (resolved dynamically). The Win7 beta has the same constants (0x2F8013 ×1, 0x2F4017 ×1, 0x2F0003 ×12). |
| 5 | ASIO options: rates {44100,48000,96000}, depths {16,24}, latency table; frames = lat·rate/1000; HKCU values | **CORRECTED** | The table contents and offsets are right (Win10 file 0x27d18 = VA 0x180029118: 13 latencies, pad 0, {16,24} @0x180029150, {44100,48000,96000} @0x180029158; Win7 0x258a0 has the same values). **The rate list does not define what ASIO accepts, though.** `canSampleRate` (0x180001440) never reads the table. It builds a 2-ch WAVEFORMATEX at the requested rate and current bit depth (helper 0x18000a8d0) and asks each enumerated KS filter whether a pin accepts it (0x180006d40). If none does it returns ASE_NoClock (-995 = 0xFFFFFC1D). The only code that reads the rate table is the constructor default (0x18000356d → **48000.0**). Because the E1 offers only 44100 Hz (see V.2-3), **only 44.1 kHz works through ASIO on the E1. ASIO is no evidence for 48/96 kHz.** Defaults the analyst missed (ctor 0x180003500): Latency = table[9] = **50 ms**, BitDepth = **24**, SampleRate = **48000.0**. Buffer sizing (`getBufferSize` 0x180001300): min = rate·1/1000, max = rate·100/1000, preferred = rate·lat/1000 **rounded up to a multiple of 8 frames**, granularity = preferred/lat. The reported latency is preferred·1000.0/rate. The panel handler 0x180002c30 formula (0x180002d6b..0x180002d7e, ×0x10624DD3 >>6) is confirmed. It passes the **unrounded** value with kAsioBufferSizeChange(4), after kAsioLatenciesChanged(6), then kAsioResetRequest(3). HKCU live: Latency = 0x64, SampleRate = QWORD 0x40E5888000000000 (44100.0), BitDepth absent. **Channel names:** the claim that "What U Hear" exists only in 1.1.1.0 is **wrong**. The Win7 beta already has "WUH L/R", "WUH Rear/Front L/R", "Spdif In/Out L/R" and "Aux In L/R" (file 0x2c60..0x2d10). Only the spelling changed ("WUH" → "What U Hear", "Spdif" → "SPDIF-"). |
| 6 | Control panel → APO via FxProperties (not pipes or shared memory) | **CORRECTED** (mechanism refined; still [I]) | The ACL is confirmed: BUILTIN\Users = QueryValues, SetValue on the E1 FxProperties key. KSUSBAPO64 holds no named-object or pipe strings (only the CRT "broken pipe") and has IID_IMMNotificationClient ×2. **But APOMgr's `%s\%s\FxProperties` path is a READ:** its only xref (0x43b189, function at about 0x43b160) builds `HKLM\SOFTWARE\...\MMDevices\Audio\Render\<id>\FxProperties` and reads `{3BA0CD54-830F-4551-A6EB-F3EAB68E3700},4` and `,6` through RegOpenKeyExA/RegQueryValueExA (helper 0x411be0). On this PC those values exist only on the Realtek endpoints. The write path is most likely the **undocumented IPolicyConfig FX store**: APOMgr64 **and** KSUSBAPO64 both embed CLSID_CPolicyConfigVistaClient {294935CE-F637-4E7C-A41B-AB255460B862}, IPolicyConfigVista {568B9108-44BF-40B4-9006-86AFE5B5A620} and IPolicyConfig {F8679F50-850A-41CF-9C72-430F290290C8}. §2.1 lists these as "one family" without naming them. Evidence that writes do happen: the live E1 FxProperties holds about 30 keys that **no INF writes**, e.g. GraphicEQ `{2B88C76D},0..9` = 0.0, `{3F23DBC5},2` = 80.0, `{DD527E35},0` = 50.0, `{80B0C7BB},0` = 0.74, `{A79717E9},0` = 0.5, `{1A2C8ED7},0` = 9 (VT_UI4), `{3C14ECCC},0` = TRUE, `{42F4281E},0` = TRUE, `{0519A199}`/`{F67CF426}`/`{9AD782D7}`/`{EA3137F9}`/`{9A9D0CB2}` = FALSE. These GUIDs exist in APOMgr64/KSUSBAPO64 and not in SndCrUSB. §5.4 lists only the INF keys. |
| 7 | SPI: shared KS sets with ksusbaud.sys, IOCTL_KS_ENABLE_EVENT, named events {6AADE933}/{3D3638AD}, mutexes {6B174228}/{4FC930BA} | **CORRECTED** | (a) The listed "DeviceIoControl sites" are **function entry points**, not call sites (0x42a124 begins `mov r11,rsp`). The real call sites are `0x42a290` (0x2F0007, in 0x18-byte KSEVENT), `0x45d9b3` (0x2F0003, the single central KS-property helper), `0x4386ab`/`0x438701` (0x220424) and `0x438970`/`0x4389c2` (0x220408, loaded into esi at 0x43894c). (b) **The USB IOCTL names are swapped** (FILE_DEVICE_USB = 0x22): **0x220408 = IOCTL_USB_GET_ROOT_HUB_NAME** (HCD fn 258) and **0x220424 = IOCTL_GET_HCD_DRIVERKEY_NAME** (fn 265). (c) **Object types:** `Global\%s` (xrefs 0x42aede, 0x42b612) and `Global\HIDInterrupt_%s` (0x42e23f) are passed to **CreateMutexW** (0x42af38, 0x42b669, 0x42e297). They are mutexes, not events. `{6AADE933}` (xrefs 0x42b764/0x42d27a/0x434839/0x441c48) → CreateEventW and `{3D3638AD}` (0x43f4a7) → CreateEventW are confirmed. `{6B174228}`/`{4FC930BA}` (0x440575/0x4405b7) → CreateMutexW at 0x440580/0x4405c2 are confirmed. Win7 SPI has the same names and IOCTLs. (d) **A Win7-vs-Win10 difference was missed.** The main Creative control set differs per build: Win7 SPI and Win7 ksusbaud.sys use **{C4BD0511-C3A0-407C-A08B-EB2CFB843E9F}** (73 refs in W7 SPI, 3 in W7 .sys, 0 in either W10 binary), while Win10 SPI and Win10 .sys use **{447845DF-5610-460A-B1A4-BF2C1DA34F16}** (74 raw occurrences in W10 SPI, not "81"; 3 in W10 .sys; 0 in either W7 binary). Win10 adds **{761E4C5B-82CF-4898-A7AE-4B653D949159}** (1 ref in W10 SPI, 5 in W10 .sys). **So a Win7 control panel/SPI cannot drive the Win10 kernel, or the reverse.** The other listed GUIDs ({C4BD0504/050B/050D/0510/051B/051E/051F}, {7BCEC54A}, {EE34D1AB}, {7B6A44BD}, {91D3A283}, {9ED25A7C}, {5FA7CE37}, {12C98F26}) were checked present in **both** kernels. |
| 8 | .ini = chained XOR, seed 0xFF; roles of ksUSBaud/ksusbchimp/cfSB1600 | **CONFIRMED** | Re-decoded with an independent script. Every encoded byte has bit 7 set (29831/29831, 2595/2595, 42854/42854). `[{32CD1956-...}]` Command Router routing text verified verbatim. md5 of the installed `System32\ksUSBaud.ini` = package = 47163d84...; `C:\ProgramData\cfSB1600.ini` = package = 0ceb6127.... Win7 and Win10 `CfSB1600.ini` are byte-identical. Nit: plain[0] is `;` for CfSB1600.ini, not always `[`. |
| 9 | KSUSBAIM64.exe = SFX cabinet installing APOIM (APOMgr/CmdRtr) | **CONFIRMED** | It is an i386 PE (0x14c). 7z lists 13 entries: APOIM.ini, apoim32/64.ini, APOMgr64.dll, APOMngr.dll, CmdRtr.dll, CmdRtr64.dll, mint.ini, mint32/64.exe, ct_cabinfo.*. Live `HKLM\SOFTWARE\Creative Tech\Command Router\{APO,SPI}` = {920449A1-...}. |
| 10 | Wrap/DGFX/PPCn/SBUSBMon unused for the E1 | **CONFIRMED** | A recursive scan of every value under `MMDevices\Audio` found **0** hits for {254E8DEF}, {E7C0C8E7}, {242249CC}, {361E7F16} or {C09096B1}. `[SBAVMon.AddReg]` is commented in both INFs, and no Run value exists in HKLM/HKLM-WOW/HKCU. The UiClsid lines are commented in `SysFx1600(.6.3).AddReg` in both INFs. Note: the Win7 INF **does** set UiClsid uncommented for SB1580/SB1590 (other models), but not for the E1. |
| 11 | WUH = ksusbaud.sys capture pin with UHearRecVol/Mute; likely software loopback | **CONFIRMED (loopback by elimination)** | The GUIDs {A186641E}, {BD7BBBF4} and {ED323662} are each present once in both kernels and absent from SPI. The live configuration descriptor (279 B, re-read through the hub IOCTL with `diag\usbdesc.exe`, identical to `diag\e1_config.bin`) has **only one** capture streaming interface: IF2 alt1, 1 ch, 16-bit, 44100 Hz, EP 0x81. A 2-ch WUH stream therefore cannot come from hardware and must be a driver-side loopback. Where in the kernel the copy happens is still unknown. |
| 12 | Whether KSUSBAPO64 is loaded in audiodg could not be determined | **CORRECTED** (superseded by indirect proof) | Direct module listing is still impossible at Medium IL (`Get-Process audiodg` .Modules returns empty; `tasklist /m KSUSBAPO64.dll` reports "no tasks", which proves nothing). However, `IAudioEffectsManager::GetAudioEffects` on the E1 render stream (`diag\wasprobe.txt` l.101-105) returns 4 effects: VIRTUAL_SURROUND {6F64ADC6}, VIRTUAL_HEADPHONES {6F64ADC7}, ROOM_CORRECTION {6F64ADC9} and SPEAKER_PROTECTION {6F64ADCC} (ksmedia.h). All 4 GUIDs are in the installed KSUSBAPO64.dll, which is byte-identical to the W10 package. The list can only come from a loaded APO's effects interface, so **the APO is loaded [strong I]**. The Win7 APO lacks {6F64ADC9}. `DisableProtectedAudioDG` = 1 (confirmed). |

### V.2 Important things this note missed

1. **The APO talks to the kernel directly.** This channel is missing from the §3 table. KSUSBAPO64 imports DeviceIoControl and sends IOCTL_KS_PROPERTY 0x2F0003 from `0x18001e33c`, `0x18001e494` and `0x1800700a5`, plus the generic helper at 0x18002a970 (call 0x18002aa00). The sequence is:
   * It reads the endpoint PKEY `{233164C8-1B2C-4C7D-BC68-B671687A2567}` (it expects VT_LPWSTR), then calls CreateFileW on that device path plus 8 characters.
   * It sends the property set **{E8E7B1C0-EB43-4AA5-98EE-7F5DB42D902F}**, Id 0, Flags 0x200, with a 0x30-byte output buffer.

   The same GUID is present in both APO builds and both ksusbaud.sys builds. `apo-dsp.md` and `kernel-usb.md` describe it as a nonce-and-encrypted device-identity handshake. **Consequence:** with a replacement kernel driver that does not answer this property, the original Creative APO falls back to its default/passthrough profile. A new stack must either implement the handshake or ship its own APO. [C: IOCTL, GUID, flags; purpose per the other notes]
2. **The APO also fingerprints disks.** Both APO builds contain `\\.\PhysicalDrive%d`, `\\.\Scsi%d:` and `SCSIDISK`, and issue IOCTL_STORAGE_QUERY_PROPERTY 0x2D1400 (@0x1800bff2b), IOCTL_STORAGE_GET_MEDIA_SERIAL_NUMBER 0x2D0C10 (@0x1800c0059), SMART_GET_VERSION 0x74080 (@0x1800c0205) and IOCTL_SCSI_MINIPORT 0x4D008 (@0x1800c04bb). This is licensing or hardware-ID code inside audiodg [C IOCTLs; I purpose]. None of it is needed for a re-implementation.
3. **The E1 hardware runs at 44.1 kHz only.** The descriptors were verified live. Render is IF1 alt1 at 16-bit and alt2 at 24-bit (subframe 3), stereo, 44100 Hz only, on isochronous-async EP 0x01 (maxpkt 200/300) with explicit feedback EP 0x83 (3 bytes). The mic is IF2, mono, 16-bit, 44100. HID is IF3 with EP 0x84. **§0.2, §6 "Why this matters" and §11.1 (expose 44.1/48/96 kHz, "the set ASIO proves the hardware path supports") are therefore wrong.** The best possible path is bit-exact 24-bit/44.1 kHz. Making OEMFormat 48 kHz would *add* an SRC stage.
4. The Win7 ↔ Win10 property-set rename ({C4BD0511} → {447845DF}) plus the new {761E4C5B} set: see V.1 #7(d).
5. **APOMgr64 and KSUSBAPO64 embed the IPolicyConfig/CPolicyConfigVistaClient GUIDs.** This is the probable FX-store write path for the control panel (V.1 #6). A new control panel can use the same undocumented path, or `IPropertyStore` from `IMMDevice::OpenPropertyStore`, or plain registry writes, which work because Users have SetValue on FxProperties.
6. **About 30 runtime-written FxProperties keys**, beyond the INF defaults, are live on the E1 endpoint (V.1 #6). Any parity implementation must seed them; the APO reads them.
7. **ASIO defaults (50 ms / 24-bit / 48000) and 8-frame buffer rounding** (V.1 #5). There is also a latent mismatch: an ASIO host that keeps the default 48000 will get ASE_NoClock on the E1 until it switches to 44100.

### V.3 Summary of corrections to apply in the body
* §3 table: `Global\HIDInterrupt_%s` and `Global\%s` are **mutexes**. Add the APO → ksusbaud KS property channel ({E8E7B1C0}).
* §3 / §4: 0x220408 = USB_GET_ROOT_HUB_NAME and 0x220424 = GET_HCD_DRIVERKEY_NAME. The quoted SPI addresses are function starts; the call sites are listed in V.1 #7.
* §0.2 / §6 / §11.1: ASIO's rate list is not a hardware capability. The E1 is 44.1 kHz only.
* §6: the Win7 beta ASIO already had the WUH channel names.
* §10: `{E4870E26},1` is a generic engine value. "APO loaded?" is answered indirectly: yes.
* §2.1: {294935CE}/{568B9108}/{F8679F50} = CPolicyConfigVistaClient / IPolicyConfigVista / IPolicyConfig.
