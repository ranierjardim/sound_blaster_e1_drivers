# Sound Blaster E1: kernel driver KsUSBaud.sys and live USB descriptors

Scope: the real USB descriptors of the plugged-in E1, and how Creative's kernel driver `KsUSBaud.sys` (AMD64) drives it. Two builds are compared:

| Build | Package | File | Size | PE timestamp | VERSIONINFO | PDB GUID |
|---|---|---|---|---|---|---|
| "W7" | Win7 pkg 1.10.03.01 | `...\w7\...\Bin\AMD64\ksusbaud.sys` | 1,538,560 | 0x5344C56A (2014-04) | 6.0.01.0586 | 0c3a6131-ca2d-4438-ae02-71bcec6b21b4 |
| "W10" | Win10 pkg 1.13.1.1 (the one installed now; byte-identical to `C:\Windows\System32\drivers\ksUSBaud.sys`) | `...\w10\...\Bin\AMD64\KsUSBaud.sys` | 1,580,552 | 0x55ADED5E (2015-07) | 6.1.13.0101 | c0598098-8d89-47c5-9da4-9d46f3fbc2d5 |

Both builds come from the same source tree. The PDB path is `...\KS_AVStream_Audio\x64\Win7Release\KsUSBaud.pdb` in both, so the "Win10" driver is still a Win7-configuration build.

Tags used below: **[C]** = CONFIRMED (seen in code, data or live device). **[I]** = INFERRED. Addresses are virtual addresses at image base 0x140000000. "W7@x" and "W10@x" give the address in each build. A function given only as "@x" is from W7.

Tools and artifacts (all in the scratchpad, none in the project):
- `scratchpad\usbdump\usbdump.c` / `usbdump.exe`: read-only descriptor dumper. It uses SetupAPI, then `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX` and `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` on the parent hub. Output is in `usbdump\out.txt`.
- `scratchpad\usbdump\hidcaps.c` / `hidcaps.exe`: dumps HID capabilities through `HidP_*`. Output is in `usbdump\hid.txt`.
- `scratchpad\kern_tools\*.py`: capstone disassembly, vtable and pointer finder, KS property-set scanner, control-request call-site summarizer, function-level diff.
- `scratchpad\decomp\`: listings (`ks7b.asm`, `ks10b.asm`), Ghidra pseudocode snippets (`c7\`, `c10\`), and property-set dumps (`kssets7.txt`, `kssets10.txt`). The decompiled dumps stay there and are not copied into the project.

---

## 1. Live device: actual USB descriptors (read 2026-11-14 from the plugged-in unit)

### 1.1 Connection [C]
- Device instance `USB\VID_041E&PID_323B\0000047173`, composite (`usbccgp`). The parent is `USB\ROOT_HUB30\4&21947844&0&0`, port 5.
- **Speed = Full-Speed (USB 1.1 signalling)**, even though bcdUSB = 2.00 (V2 info: protocols USB110|USB200). Device address 4, configuration 1.
- Pipes currently open: only EP 0x84 (HID interrupt, held by HidUsb). No audio was streaming during the dump.
- Function drivers now: MI_00 uses service `KsUSBaud` (oem57.inf, section `SB1600_Device.6.3`, DriverVersion 1.13.1.1). MI_03 uses `HidUsb` (input.inf). Device Parameters: `RevisionId`=4102 (=0x1006=bcdDevice).

### 1.2 Device descriptor (18 bytes) [C]
```
12 01 00 02 00 00 00 40 1E 04 3B 32 06 10 01 02 03 01
```
bcdUSB 2.00, class/sub/proto 0/0/0 (composite by interface), bMaxPacketSize0 64, VID 0x041E, PID 0x323B, bcdDevice 0x1006, iManufacturer 1 "Creative Technology Ltd", iProduct 2 "Sound Blaster E1", iSerial 3 "0000047173", 1 configuration.
String index 4 = "10012006". No descriptor field references it. It is probably a hardware or firmware ID string [I]. Strings 5 to 15 do not exist. The device qualifier request STALLs, which is normal for a device that is running at full speed.

### 1.3 Configuration descriptor (279 bytes, raw) [C]
```
09 02 17 01 04 01 00 80 32 09 04 00 00 00 01 01
00 00 0A 24 01 00 01 47 00 02 01 02 0C 24 02 02
01 01 00 02 03 00 00 00 0C 24 02 01 01 02 00 01
00 00 00 00 09 24 03 03 01 01 00 06 00 09 24 03
04 01 03 00 05 00 0A 24 06 05 02 01 01 02 02 00
09 24 06 06 01 01 03 00 00 09 04 01 00 00 01 02
00 00 09 04 01 01 02 01 02 00 00 07 24 01 02 01
01 00 0B 24 02 01 02 02 10 01 44 AC 00 09 05 01
05 C8 00 01 00 83 07 25 01 00 00 00 00 09 05 83
01 03 00 01 05 00 09 04 01 02 02 01 02 00 00 07
24 01 02 01 01 00 0B 24 02 01 02 03 18 01 44 AC
00 09 05 01 05 2C 01 01 00 83 07 25 01 00 00 00
00 09 05 83 01 03 00 01 05 00 09 04 02 00 00 01
02 00 00 09 04 02 01 01 01 02 00 00 07 24 01 03
01 01 00 0B 24 02 01 01 02 10 01 44 AC 00 09 05
81 05 C8 00 01 00 00 07 25 01 00 00 00 00 09 04
03 00 01 03 00 00 00 09 21 00 01 00 01 22 5B 00
07 05 84 03 04 00 0A
```

### 1.4 Decoded tree [C]
```
Config 1: 4 interfaces, bus-powered (bmAttributes 0x80), 100 mA
IF0 alt0  AudioControl (class 1/sub 1/proto 0 = **UAC1 / ADC 1.00**), 0 endpoints
  AC Header   bcdADC=1.00 wTotalLength=0x47 bInCollection=2 -> IF1, IF2
  IT  ID=2  wTerminalType=0x0101 (USB streaming)  2 ch, wChannelConfig=0x0003 (L,R)
  IT  ID=1  wTerminalType=0x0201 (Microphone)      1 ch, wChannelConfig=0x0000
  OT  ID=3  wTerminalType=0x0101 (USB streaming)  bAssocTerminal=1, bSourceID=6
  OT  ID=4  wTerminalType=0x0301 (Speaker)          bSourceID=5
  FU  ID=5  bSourceID=2  bControlSize=1  bmaControls: master=0x01 (Mute), ch1=0x02 (Volume), ch2=0x02 (Volume)
  FU  ID=6  bSourceID=1  bControlSize=1  bmaControls: master=0x03 (Mute+Volume), ch1=0x00
  -> Playback: IT2 -> FU5 -> OT4(headphone/speaker).  Capture: IT1(mic) -> FU6 -> OT3.
  -> NO mixer, selector, processing or extension units. NO AC interrupt endpoint.
IF1 AudioStreaming (playback, linked to IT2)
  alt0: zero bandwidth
  alt1: 2 EPs. AS_GENERAL link=2 bDelay=1 wFormatTag=0x0001 PCM
        FORMAT_TYPE_I: 2 ch, bSubframeSize=2, bBitResolution=16, 1 discrete rate = 44100 Hz
        EP 0x01 OUT, bmAttributes=0x05 (isochronous, ASYNCHRONOUS), wMaxPacketSize=200 (=50 frames), bInterval=1, bRefresh=0, bSynchAddress=0x83
        CS EP: bmAttributes=0x00 (NO sampling-frequency control, no pitch, no max-packets-only), lock delay 0
        EP 0x83 IN, bmAttributes=0x01 (isochronous feedback, UAC1 style), wMaxPacketSize=3 (10.14 format), bInterval=1, bRefresh=5 (2^5 = 32 ms), bSynchAddress=0
  alt2: same as alt1 but bSubframeSize=3, bBitResolution=24, EP 0x01 wMaxPacketSize=300 (=50 frames)
IF2 AudioStreaming (capture, linked to OT3)
  alt0: zero bandwidth
  alt1: 1 EP. AS_GENERAL link=3 bDelay=1 PCM; TYPE_I 1 ch, 2 bytes, 16 bit, 44100 Hz only
        EP 0x81 IN, 0x05 (iso, async), wMaxPacketSize=200, bInterval=1; CS EP bmAttributes=0
IF3 HID (3/0/0), HID 1.00, country 0, 1 report descriptor of 91 (0x5B) bytes
  EP 0x84 IN interrupt, wMaxPacketSize=4, bInterval=10 ms
```
Consequences [C, inferred from the descriptor]:
- In the mode it is in now, the E1 is a **plain UAC1 full-speed device at a fixed 44.1 kHz**. Playback is 16 or 24-bit stereo, the mic is 16-bit mono. 48 kHz, 96 kHz and higher are not on offer. Any Windows driver (usbaudio.sys or Creative's) must resample 48 kHz system audio down to 44.1 kHz.
- The playback endpoint is asynchronous with an explicit feedback endpoint (0x83). A correct driver must follow the feedback to avoid drops and repeats.
- The only hardware controls are FU5 (playback: master mute, L/R volume) and FU6 (mic: mute and volume). Bass, treble and EQ do not exist in hardware.

### 1.5 HID interface (MI_03) [C for caps; the raw report descriptor could not be read]
The hub IOCTL refuses interface-recipient `GET_DESCRIPTOR(0x22)` with ERROR_GEN_FAILURE. The report descriptor is cached by hidclass and is not exposed to user mode. The HidP capabilities are:
- **COL01**: UsagePage 0x000C (Consumer), Usage 0x0001. Input report **ID 9**, 2 x 8-bit values (usage 0, 0..255). Output report **ID 8**, 5 x 8-bit values (usage 0). This is a vendor command/response channel tunnelled through a consumer collection [I].
- **COL02**: UsagePage 0x000B (Telephony), Usage 0x0005 (Headset). Input report ID 2 has buttons: usage 0x0000, and 0x002F = Phone Mute. Output report ID 2 has 2 one-bit outputs (usage 0x0009 on page 0, probably the LED page "Mute" [I]). Feature report **ID 6**, 1 byte, logical 0..1.
- The kernel driver also talks HID directly over EP0 with class-interface requests: GET_REPORT (bRequest 0x01, wValue=0x0300|ReportID, wIndex=HID interface; helper W7@1400bd0b0) and SET_REPORT (0x09, wValue=0x0300|ReportID; helper W7@1400bdff4). Both use Feature reports, and only when a HID interface was found (`this+0x7C`, interface number at `this+0x80`) [C]. At start it sends the bytes of the registry value `StartUpHIDCommand` (W7@1400b0ec8) [C].

---

## 2. KsUSBaud.sys architecture

### 2.1 Driver model [C]
- **AVStream minidriver** (imports `KsInitializeDriver`, `KsCreateFilterFactory`, `KsPinGetLeadingEdgeStreamPointer`, `KsStreamPointer*`). It is not PortCls, so it has no WaveCyclic or WaveRT miniport. The proxy CLSID in the INF is `{17CCA71B-ECD7-11D0-B908-00A0C9223196}` (KS proxy).
- It is a full replacement for usbaudio.sys. It sends URBs itself (`IoAllocateIrp` + `IofCallDriver`, `IOCTL_INTERNAL_USB_SUBMIT_URB` 0x220003, submit helper W7@1400bc774) and uses `USBD_CreateConfigurationRequestEx` and `USBD_ParseConfigurationDescriptorEx`. It queries `USB_BUS_INTERFACE_USBDI_V1` {B1A96A13-3DE0-4574-9B01-C08FEAB318D6} and stores `IsDeviceHighSpeed()` at `usbdev+0x74` (W7@1400bc8cc).
- It is C++ with heavy multiple inheritance and no RTTI. Device-family classes are chosen by PID (factory W7@140089090 / W10@14008a17c):

| PID | W7 ctor (obj size) | W10 ctor (obj size) | Product [I] |
|---|---|---|---|
| 0x323A | @1400ac89c (0x3780) | @1400ad040 (0x2B50) | E3 or E5 sibling |
| **0x323B** | **@1400af214 (0x2480)** | **@1400affd8 (0x2B40)** | **E1** |
| 0x323C | @1400b3060 (0x3778) | @1400b3c94 (0x3EB8) | sibling |
| 0x323E | @1400b18f0 (0x2478) | @1400b1b34 (0x2B40) | sibling |

  The entry gate W7@14016e124 checks VID 0x041E, accepts PID 0x323A..0x323C and 0x323E, and sleeps 3 s (`-0x1C9C380`*100 ns) for PIDs 0x3010 and 0x3015.
- E1 primary vtable: W7 0x140115D78, W10 0x14011AD08. The USB-device object vtable is W7 0x14011AF98. Its slots are:

| Offset | Method |
|---|---|
| +0x18 | SubmitUrb |
| +0x68 | ControlVendorOrClass(urbFunction, bRequest, wValue, wIndex, buf, *len, dirIn) |
| +0x70 | ClassEndpointRequest |
| +0x78 | ClassInterfaceRequest, wIndex = (entityId<<8) or (AC interface) |
| +0x98 | AllocIrp |
| +0xA0 | ResetPipe [I] |
| +0xB0 | IsHighSpeed |
| +0xB8 | GetCurrentFrameNumber |
| +0xC8 | IoInvalidateDeviceRelations (forces re-enumeration) |

### 2.2 Generic control-transfer helper (W7@1400be34c) [C]
It builds `_URB_CONTROL_VENDOR_OR_CLASS_REQUEST` (0x88 bytes) with `Function`=arg, `TransferFlags`=dirIn (USBD_TRANSFER_DIRECTION_IN=1), `Request`, `Value`, `Index`, and a pool buffer. It fails with STATUS_UNSUCCESSFUL if IRQL > APC_LEVEL. When `usbdev+0x78` (a restricted mode) is set, it only forwards `Function=0x20` with bRequest in {0x00,0x01,0x04,0x05,0x06,0x14..0x17,0x29,0x2A}, using bitmask 0x60000F00073 [C]. It optionally logs every transfer to a logger object when `HKLM\SOFTWARE\Creative Tech\USBLogging\Logger On`=1.
URB function codes used: **0x20 = URB_FUNCTION_VENDOR_OTHER** (bmRequestType 0x43 OUT / 0xC3 IN), 0x1B = CLASS_INTERFACE (0x21/0xA1), 0x1C = CLASS_ENDPOINT (0x22/0xA2), and 0x17 = VENDOR_DEVICE (other families only).

---

## 3. Streaming

### 3.1 Formats exposed to KS for the E1 (data-range builder W7@1400b0aa0 = W10@1400b0c9c, identical) [C]
| KS pin # | Direction (W7@1400b0a40) | Channels | Rate min..max | Bits min..max |
|---|---|---|---|---|
| 0 | render | 2 | 44100..44100 | 16..**24** |
| 5 | capture (mic) | 1 | 44100..44100 | 16..16 |
| 6 | capture (stereo, "What U Hear") | 2 | 44100..44100 | 16..16 |
| 8 | render, only when NOT high-speed | per-pin default 44.1k/16/2 | n/a | n/a |

The ctor defaults for 9 "format slots" (W7@1400af214 loop; same in W10@1400affd8) are 44100 Hz, 16-bit, 2 ch, channel-mask code 3, except slot 5 = 1 ch, mask 4. If the device enumerates high-speed, 8 slots are forced to 16-bit.
The base-class data range (for other models, W10@140087648) offers 96000 Hz/24-bit/6 ch plus 48000/44100 alternatives. The E1 class overrides it to 44.1 kHz only. **The Creative driver never offered more than 44.1 kHz for the E1 in either build** [C].
Format samples are SubFormat PCM. 24-bit means 24 bits in 3-byte containers, because the USB alt2 subframe is 3 bytes [C, from the descriptor]. The exact KS container options (for example 24-in-32) were not traced [I].

### 3.2 Buffering and pin model [C]
- Render pin "fill" W7@140059f70: memcpy from KS stream-pointer frames into a per-pin ring buffer (`pin+0xA0`, size `pin+0xB4`). On underrun it writes zeros and sets `pin+0x7C`. **No sample processing at this stage.**
- Capture pin W7@14005abb0: memcpy from the ring into KS frames. Pin state changes (W7@1400598c0) map KS STOP/ACQUIRE/PAUSE/RUN onto stream object states 3/0/2/1. Position is derived from `KeQueryPerformanceCounter`, multiplied by bytes per ms.
- A third render-type pin class (W7@14005b518) holds stream-pointer clones until bytes are actually played. It is probably used for the looped or "pin 8" path [I].
- USB stream object ctor W7@1400b9f2c (W10@1400bd6ed, the same): defaults 48000 Hz / 16-bit / 2 ch; **3 URBs in flight, 10 frames (10 ms) per URB** (`qword +0x40 = 0x000A0003`). The render source is pulled in **5-frame (5 ms) chunks** (W7@1400b8a50). Hardware latency is about 30 ms of queued isochronous data plus the ring.
- Pipe allocator W7@1400b63d0(urbCount, framesPerUrb, maxPacket, packetsPerFrame). It allocates `packets = frames*ppf` and a transfer buffer of `urbCount*packets*maxPacket`. At full speed ppf=1; at high speed ppf=8 microframes.
- `ExSetTimerResolution(10000 = 1 ms)` is used while streaming (W7@1400b7c14).

### 3.3 Isochronous URB construction [C]
- `_URB_ISOCH_TRANSFER` with `Function = 0x000A`, `Length = 0x98 + 12*nPackets`.
- `TransferFlags` = `pipe+0x64` OR'd with direction. Capture/IN pipes use `USBD_START_ISO_TRANSFER_ASAP|SHORT_OK|IN` (pipe ctor W7@1400b760c sets +0x64=4). Render and feedback pipes use explicit `StartFrame`: if the stored next frame is behind the bus frame (GetCurrentFrameNumber) or more than 100 frames ahead, they resync to `current + 5` and then advance by `packets/ppf` per URB (W7@1400b7780, @1400b8a50).
- Render packet sizing (W7@1400b8a50 + W7@1400b5c9c): per packet `bytes = (bitsPerSample * channels * samples) / 8`.
  - **No feedback yet**: samples = `floor(rate/(1000*ppf))`. A per-mille accumulator adds `(rate - base*1000*ppf)/ppf` each packet, plus the remainder once per frame. When the accumulator reaches 1000 or more, the packet gets one extra sample. At 44100 Hz full speed this gives 44,44,...,45 (9×44 + 1×45 per 10 ms).
  - **With feedback** (`pipe+0x7C`≠0): a Q16.16 accumulator at `pipe+0x78` gains `fb/ppf` per packet (+remainder per frame). The integer part (high word at +0x7A) becomes the packet's sample count and is then cleared, so the fraction carries over.
- The capture completion (W7@1400b7958) copies each packet's actual length, rounded down to a whole sample frame, into a staging buffer. Every 5 frames, at the end of the URB, or on a zero-length packet, it hands the data to the sink (`pipe+0x288`→vtbl+0x18).

### 3.4 Feedback / clock handling [C]
- Feedback pipe class: ctor W7@1400b989c, vtable W7 0x14011AA58. It runs a dedicated system thread (W7@1400b9ce8). Defaults are `+0x32A`=1 (vendor-poll period in ms) and `+0x32C`=5 (frames between feedback URBs, later overridden from the descriptor).
- Iso feedback path (W7@1400b9ab4): 1 packet per URB of `wMaxPacketSize` (3), 3 URBs (`stream+0xD0`→vtbl+0x20(3,1,maxpkt,ppf)). StartFrame advances by the refresh period.
- Parse (W7@1400b9a6c): **3-byte value (FS, 10.14) → `value << 2` = Q16.16 samples per frame. A 4-byte value (HS) is used as-is (16.16).**
- Apply (W7@1400b6e98, render vtbl+0x30): nominal = `rate/1000` integer samples. If the feedback lies outside [nominal−2, nominal+2] samples, it is **rejected and replaced by the nominal value** `rate*65536/1000` (computed as `(rate<<16)/1000` for rates up to 48 kHz, else `((rate<<10)*0x10624DD3)>>32 & ~0x3F`). The result is split per microframe: `+0x8C = fb/ppf`, `+0x90 = fb%ppf`.
- Initial feedback before the first report = `rate<<16 / 1000` (W7@1400ba670).
- Alternative "vendor feedback" mode (for devices without a feedback EP): the thread polls `VENDOR_OTHER IN bRequest = pipe+0x2B8, wIndex = pipe+0x328, 4 bytes` every `+0x32A` ms (W7@1400b9e74). The E1 has EP 0x83, so it uses the isochronous path [I].
- Sampling-rate control: when starting a stream (W7@1400ba670), and only if the class-specific AS endpoint has bmAttributes bit0 set, it sends `SET_CUR (0x01)`, class endpoint, `wValue=0x0100` (SAMPLING_FREQ_CONTROL), `wIndex=EP address`, 3-byte rate. **The E1's CS endpoint has bmAttributes=0, so no rate request is ever sent** [C, combined with the descriptor].

### 3.5 In-kernel DSP and resampling [C: present / I: whether used for E1 render]
- About 500 KB of `.text` (0x140001000–0x140085000) is a software DSP engine. Its contents:
  - **Sample-rate converters**: a 16-bit 4-point cubic (Hermite/Catmull-Rom) interpolator (@140035850) and a 32-bit linear interpolator (@140041500). Both use a 16.16 phase step and **bypass with a plain memcpy when step == 0x10000** (no rate change). Rounding uses the 1.5·2^52 trick (round-to-nearest) and clamps to ±32767. **There is no dither anywhere** (no RNG or noise-shaping found near the converters; `rand`/`srand` are imported but not traced).
  - A format-converter matrix (function tables at 0x140125230.. and 0x1401256B0..).
  - dB→linear gain (`lin = 2^(dB*0.1660964)` with a polynomial exp2, @140015e30).
  - Large voice-processing blocks. Strings show "FD Analysis/FD Synthesis/FDVoiceMod" (voice morph), "Silencer / Simple adaptive plugin / LMSAnalysis" (noise reduction), PitchShift, and EAX-style reverb parameters.
- Software volume nodes: E1 topology nodes 3 and 7 report a range of **min −96 dB, max +6 dB, step 1/16 dB** (W7@1400b068c: 0xFFA00000/0x60000/0x1000). These are not USB requests, so they are kernel digital gain applied to PCM. Node 1 is the hardware FU5 volume (see §4.2). Node 10 also gets a software range (W7@1400b0cc8 queries nodes 3 and 10 and programs DSP objects at E1+0x1280/+0x1288) [C]. At 0 dB the gain is presumably unity or bypassed [I].
- **For the E1 render path at 44.1 kHz the SRC is bypassed (step = 1.0)**, because the KS pin only accepts 44.1 kHz [I, strong]. The DSP engine matters for the mic effects (voice morph and noise reduction, controlled by property sets 12C98F26 and 5FA7CE37, see §5), for software volume nodes, and for the What-U-Hear mix [I].
- **What U Hear**: pin 6 is a stereo 16-bit capture pin, but the USB device has only a mono mic endpoint (IF2). WUH therefore has to be a kernel software loopback of the render stream (plus optional mic mix; strings `UHearRecVol/UHearRecMute`, `HostMixRecVol/HostMixRecMute`) [I, strong]. The exact copy point was not isolated.
- Win7 and Win10 contain the same DSP blocks. The format converters and SRCs are matched in the function diff.

**Quality takeaway [I]:** the kernel render path for the E1 is effectively bit-transparent: 44.1 kHz, 16 or 24-bit memcpy into isochronous packets, with feedback-driven packet sizing. The "much better sound on Win7" cannot come from kernel DSP on the render path. It most likely comes from (a) the Win7 package running the stream at 44.1 kHz, or 24-bit, end to end, or (b) the user-mode SBX APO (KSUSBAPO64) chain. A new driver should do the following:
- Expose 44.1 kHz 24-bit as the default format, so that the Windows mixer resamples 48k→44.1k once in float, or the app picks 44.1 kHz.
- Implement the asynchronous feedback correctly. Do not adopt Creative's ±2 sample clamp blindly; it is fine at 44 samples per frame.
- Use FU5 hardware volume (1/256 dB steps) rather than software attenuation.

---

## 4. All USB control requests

### 4.1 Vendor requests used by the E1 class (URB_FUNCTION_VENDOR_OTHER, bmRequestType 0x43/0xC3) [C for wire format; semantics I]

| bRequest | Dir | wValue | wIndex | Len | Where (W7) | Meaning |
|---|---|---|---|---|---|---|
| 0x01 | IN | 0 | 0 | 32 | @1400b029c (E1 vt+0xC0) | Device info block. Bytes 4.. are copied into the SPI "device info" reply together with VID, PID and marker 0x0600034A. Also returns HS flag+1 [C]. Firmware/version info [I]. |
| 0x92 | IN | 0 | 0 | 4 | @140089334 (start) | Two 16-bit values stored at E1+0x1DF4/0x1DF6. A flag is set if both are non-zero. Firmware version [I]. E1 ctor sets a reference 0x000B0005 (11.5?) [I]. |
| 0x2A | IN | 0 | 0 | 2 | @1400b25b8, @1400afffc, @14008e690 | Status word. Bit0 decides the USB-mode logic below [C]. Its meaning, "device configured for HS mode", is [I]. |
| **0x29** | OUT | **1** | **2000 (0x7D0)** | 0 | @1400afffc (vt+0x4E0) | **Switch the device to high-speed mode, then call IoInvalidateDeviceRelations and return STATUS 0xC0000182.** The device re-enumerates. 2000 = delay in ms [I]. |
| **0x29** | OUT | **0** | **2000** | 0 | @140093ac8 (vt+0x4E8) | **Switch back to full-speed mode** (only when currently HS) and re-enumerate. |
| 0x21 | OUT | (rate≠44100) for pin 8; 0 for pin 0 | 1 | 0 | @1400874cc (vt+0x2F8, stream start), @1400b11f8 (stop) | Selects the 44.1/48 kHz clock for the auxiliary render path (pin 8) [I]. |
| 0x2E | OUT | 0/1 (or caller value) | 0 | 0 | @1400874cc, @1400ac270 (vt+0x360), @1400ac2bc | Enable flag. It is forced to 1 while pin 8 is active (E1+0x2448). Output routing or mute of the digital path [I]. |
| 0x2B | OUT | caller value | 0 | 0 | @1400ac2bc (vt+0x358) | Followed by 0x2E wValue=0. Called at start with (E1+0x5C4 ≥ 0). Input/route select [I]. |

**USB mode logic** (W7@1400b25b8, called at device start) [C]:
- `USBMode` = DWORD at `HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_041E&PID_323B&MI_00\USBMode`, default **1** (reader @1400b24b8).
- `ForceUSB` = `HKLM\SYSTEM\CurrentControlSet\Control\Creative Tech\USB\ForceUSB`, default 0 (@1400875c8).
- If status bit0==1 and USBMode==1, the driver goes to FS (0x29 wValue 0) when the device is HS.
- If status bit0==0, USBMode==2 and ForceUSB==0, the driver switches the device to HS (0x29 wValue 1) when it is FS.

Neither value exists on this machine, so the device stays in FS mode, which is what was observed. **Whether HS mode offers more than 44.1 kHz is unknown.** The E1 KS data ranges are 44.1 kHz-only in both modes, and HS mode even forces 16-bit slots [C]. The HS descriptor set has not been captured [I]. Do NOT send 0x29 casually: it re-enumerates the device.

### 4.2 Standard UAC1 class requests used by the E1 [C]
- Volume ranges for node 1 (FU5 L/R; W7@1400b068c): `GET_MAX 0x83`, `GET_MIN 0x82`, `GET_RES 0x84` to the AC interface, with `wValue = 0x0200 | channel` (VOLUME_CONTROL) and `wIndex = (FU id<<8) | IF0`, 2 bytes. The results are scaled `<<8` (1/256 dB → KS 1/65536 dB).
- Get and set of current values go through the generic class-interface helper (vtbl+0x78), using GET_CUR 0x81 / SET_CUR 0x01 with MUTE 0x0100 or VOLUME 0x0200. The registry cache `KSAud_Topology\Filter[0]Node[1]Ch[1..2] = -1572864` (= −24.0 dB in 16.16) is the current FU5 volume. `Node[0]Ch[1]=0` is mute off [C, from the live registry].
- **GET_MEM (0x85)** to the AC interface (UAC1 memory request) [C]:
  - `wValue=0, wIndex=IF0, 4 bytes` (E1 vt+0x338 W7@1400b0f74, and @1400b26c4): returns an accessory/jack code. Values 0x0C, 0x17, 0x2D (→ reported as 0x1A) are forwarded as a KS event with GUID {37E60DF2-02A3-499B-B78A-E28DBEDC3E3E} or {4AB30A74-A81A-402A-9917-BCA5BE358697}. It is triggered by interrupt status with bit7 set.
  - `wValue=0, wIndex=0x0400|IF0, 3 bytes` (@1400ab600): byte2 bit0 → flag 0x20, bit1 → flag 0x08.
  - `wIndex=0x0300|IF0, 3 bytes`: byte2 low nibble ≠ 0 → flag 0x10. These are jack-sense or switch states [I].
- Sampling frequency SET_CUR (class endpoint): implemented, but not sent for the E1 (§3.4).
- HID GET_REPORT/SET_REPORT (Feature) to IF3: see §1.5.

### 4.3 Requests seen only in other families' classes [C]
These appear in the same binary (VENDOR_OTHER 0x04,0x05,0x06,0x14,0x15,0x16,0x17,0x24,0x25,0x91,0x93; VENDOR_DEVICE 0x00..0x08) and are not reachable from the E1 vtable. Full list: `scratchpad\decomp\ctlsum7b.txt` / `ctlsum10.txt`.

---

## 5. Private KS property sets (user-mode interface used by KSUSBSPI / KSUSBAPO / SBE)

The scanner found 77 sets in W7 and 79 in W10. GUIDs were cross-referenced against all Creative binaries in both packages. `G`/`S` = get/set handler, `dN` = MinData bytes; d0 means a variable size, ULONG in practice. MinProperty is 24 (KSPROPERTY+node) unless noted.

Sets referenced by **KSUSBAPO** (the APO talks to the kernel directly):
- **{12C98F26-05CD-458C-829E-219E482FAD8E}** (W10 handlers 0x140162xxx–0x140164xxx). Ids 0..7 G/S, 98 G, 99 G/S. Id0's set handler persists the ULONG as registry value "FormantFreq1" and broadcasts a change event carrying this set GUID [C]. The id order matches the registry names FormantFreq1, FormantFreq2, FormantFreq3, PitchFactor, FormantFactor, PitchVariability, QuiverDepth, ContourDepth, so this is the **VoiceFX / voice-morph** mic effect running in the kernel DSP [I, strong]. 98/99 are probably state/enable [I].
- **{5FA7CE37-243F-417B-B503-AF4633DA5799}**: ids 0..11 G/S, 97 G (MinProperty 28), 98 G, 99 G/S. There are 12 params, matching AdaptationStep, Selectivity, FastSmoothFactor, SlowSmoothFactor, MaxAttenuation, VoiceEQLowFGain, VoiceEQHighFGain, VoiceEQFullGain, Low/Mid/HighFOverestimationFactor, FilterLimit. This is the **"Silencer" noise reduction** [I].
- **{E8E7B1C0-EB43-4AA5-98EE-7F5DB42D902F}**: id0, support handler only, MinData 48 (W10@1401521f4). It returns a 48-byte device-identity block (+0x18=1, +0x20/+0x24 = two 16-bit IDs, PID/VID [I]) [C].

Sets referenced only by **KSUSBSPI** (control-panel back end):
- Sets with 25 ULONG params each: {56715B5A-FE63-453B-BB15-8E308022CFF7} (ids 0..24), {7B6A44BD-8361-46F7-B765-0E51DF220647} (ids 1..25), {EE34D1AB-E2D2-425D-912A-D96F1A5C205A} (ids 1..8,10..12,15..45 by 3s,60,67). These are probably the EAX reverb and THX/SBX parameter blocks [I].
- {9ED25A7C-…} (8), {475ED6E7-…} (9), {49591BFC-…} (7), {E76BCBF4-…} (9), {91D3A283-…} (6), {300C1011-…} (8), {A1910A68-06DC-4097-B3CD-FB5AC49C3639} (13), {C4BD05xx-C3A0-407C-A08B-EB2CFB843E9F} family (0504, 050B, 050D, 0510, 051B, 051E, 051F), and others. The full table with ids is in `scratchpad\decomp\sets10.md` / `sets7.md`.
- Not referenced by any user-mode file: {28D44852-…}, {2F2C8DDD-…} (pin), {4B27CFED-…}, {4F67B528-…}, {6D3B6798-…} (get d104 / set d32), {710C9A2B-…}, {93A2E47E-…}, {06261394-…} (W10 only).

Standard sets [C]:
- KSPROPSETID_Audio node items: 4 VOLUMELEVEL, 13 MUTE, 12 MUX_SOURCE, 14 BASS, 16 TREBLE, 21 AGC, 28 DEV_SPECIFIC, 29 DEMUX_DEST.
- KSPROPSETID_Audio filter items: 3 CHANNEL_CONFIG, 34 STEREO_SPEAKER_GEOMETRY.
- KSPROPSETID_Audio pin items: 5 POSITION, 2 COPY_PROTECTION.
- KSPROPSETID_Pin: 8 GLOBALCINSTANCES, 10 PHYSICALCONNECTION.
- KSPROPSETID_General: 0 COMPONENTID (72 bytes).
- Events: `KsAddEvent` (W7@140168024) and `KsGenerateEvents` (W7@140059490) are used for jack and parameter-change notifications [C].

Driver registry state (driver key `...\Enum\USB\VID_041E&PID_323B&MI_00\<inst>\Device Parameters`) [C, from the live registry]:
- `KSAud_Device\SPDIFOutCurRate`=48000, `PCMBitDepth`=16.
- `KSAud_Topology\Filter[f]Node[n]Ch[c]` holds per-node values (Node1 = −24 dB, nodes 3,4,7,10 = 0, Node8 = 1).
- `HeadphoneVolume` / `SpeakerVolume` are 8×DWORD arrays (−24 dB and −6.02 dB values).
- Profile and effect values use the names in W7 0x1400C4C30–0x1400C68D0, for example `PlaybackUserProfile%d_ParamValue%d` and `THX*`.

---

## 6. Win7 (1.10.03.01) vs Win10 (1.13.1.1) differences

| Area | W7 | W10 | Tag |
|---|---|---|---|
| Version / PDB | 6.0.01.0586, built 2014-04 | 6.1.13.0101, built 2015-07; both "Win7Release" builds | C |
| Imports | same set | adds `RtlCompareMemory` only | C |
| Functions (.pdata primary) | 3207 entries | 3274. 2086 identical; ~208 W7-only / 271 W10-only after constant-insensitive matching (mostly the base device class and property handlers that moved) | C |
| E1 object size | 0x2480 | 0x2B40. The base class grew; E1 ctor logic and defaults (44.1k/16/2, −24 dB, 0xFFFCA000, 0xB0005) are identical | C |
| E1 KS formats | 2ch 44.1k 16–24 render; 1ch 16 mic; 2ch 16 WUH | **identical** | C |
| Format→alt-setting selection | fixed per-pin format slots | new path (W10@14008c688, enabled by `+0x126C`) matches the requested format against parsed AS descriptors (channels, bit range, rate range, subframe) to pick the alternate setting. Special case: PID 0x323B pin 5 is forced to mono | C |
| Streaming engine (URBs 3×10 ms, 5 ms chunks, feedback parse/clamp, 0x88118811 marker, 48k defaults) | — | **identical constants** | C |
| USB mode switch (0x29 / 0x2A, USBMode / ForceUSB) | present | present (W10@1400b057c/@1400b05f0/@140097a70) | C |
| Vendor-request call sites | 86 | 80. Several duplicated 0x21/0x2E/0x2A sites in sibling-PID classes were merged into the base class. The request set is the same | C |
| Property sets | 77 | 79. **New:** {06261394-BC84-4BB1-A11A-6B7D89F51D17} (2 single-item tables, one pin-level MinProperty 8) and {761E4C5B-82CF-4898-A7AE-4B653D949159} (1 G/S item, used by KSUSBSPI). **Renamed:** {C4BD0511-…} (W7) → {447845DF-5610-460A-B1A4-BF2C1DA34F16} (W10), same items (0 G d38, 3 S d28). **Grown:** {63A37602-…} 1→2 items (both S, d2) | C |
| In-kernel DSP (SRC, converters, voice FX, NR, EAX) | present | present, same algorithms | C (match) / I (fine details) |
| INF binding | `SysFx1600` (legacy Vista-style SysFx) for the E1 | `SysFx1600.6.3` section on 8.1+ (see the other agents' INF notes) | C (INF) |

**Bottom line [I]:** the kernel data path for the E1 did not change in a way that would degrade quality between the two packages. Both drivers stream the E1 at 44.1 kHz, bit-transparently, with the same feedback logic. Look for the quality difference in the user-mode APO chain and in endpoint format and SRC settings, not in KsUSBaud.sys.

---

## 7. Implications for the new driver
1. The target hardware is a UAC1 full-speed device: 44.1 kHz only, 16 or 24-bit playback (IF1 alt2 = 24-bit), 16-bit mono mic, and an async OUT endpoint with 10.14 feedback on EP 0x83 (bRefresh 5).
2. Windows 11 `usbaudio.sys` (UAC1) already supports all of this. A KMDF/ACX replacement would gain little on the render path. The realistic quality levers are:
   - Force 24-bit alt2 at 44.1 kHz.
   - Set the shared-mode default format to 44100 Hz/24-bit so the audio engine does one high-quality SRC, or none.
   - Use hardware FU5 volume.
   - Re-create the SBX effects as an APO.
3. If a custom kernel driver is still wanted: copy the feedback math (§3.4) and per-mille packet sizing (§3.3), use ≥3 URBs × 10 frames, keep no in-kernel DSP on the render path, and implement What-U-Hear as a loopback in software (the device has none).
4. Optional and risky: vendor 0x29 HS-mode switching. First capture the HS descriptors by manually setting `USBMode`=2 with Creative's driver installed, on a machine where re-enumeration is acceptable.

## 8. Open items
- The raw 91-byte HID report descriptor (could be read with a filter or from a USB capture).
- The HS-mode descriptor set and rates after vendor request 0x29 wValue=1.
- The exact semantics of vendor requests 0x01, 0x21, 0x2B, 0x2E, 0x92, of the status bits from 0x2A, and of the GET_MEM codes.
- The exact location of the What-U-Hear loopback copy and of software-volume application (the engine's pull path, `pipe+0x280`→vtbl+0x18, was not resolved statically).
- Item-by-item meaning of the 25-parameter KSUSBSPI sets (to be matched against the KSUSBSPI and APO notes).

---

## Verification

This is an independent adversarial re-check, done on 2026-11-14. I did not reuse the analyst's listings for any verdict. Method:
- Fresh capstone disassembly of `ks7.sys` and `ks10.sys` (SHA-1 F5B62C02… = W7 package AMD64; E89D8C1F… = W10 package AMD64 = the installed `C:\Windows\System32\drivers\ksUSBaud.sys`). Script: `scratchpad\verify_k\kd.py`.
- My own string extraction (`verify_k\s7.txt`, `s10.txt`) and import listing.
- A fresh, read-only hub-IOCTL descriptor read (`verify_k\hubq.py`).
- A live open-pipe probe while Windows played 6 s of digital silence through the default render endpoint (`verify_k\pipes.py` + `silence.wav`). No configuration was changed.

Verdicts: **CONFIRMED** (re-derived independently), **CORRECTED** (partly wrong; the right value is given), **REFUTED**, **UNVERIFIABLE** (could not be decided statically or live).

### V.1 Claim-by-claim

| # | Claim (short) | Verdict | Evidence / correction |
|---|---|---|---|
| 1 | Full speed, UAC1, 44.1 kHz only. Playback 16-bit (alt1) or 24-bit (alt2), mic 16-bit mono | **CONFIRMED** | Re-read live: `Speed=1`, address 4, the same 279-byte configuration byte for byte. FORMAT_TYPE_I `0B 24 02 01 02 02 10 01 44 AC 00` / `…02 03 18…` / mic `…01 02 10…`. |
| 1a | §1.4 decode: "OT ID=3 … bAssocTerminal=1" | **CORRECTED** | Bytes `09 24 03 03 01 01 00 06 00` give bAssocTerminal = **0x00**, bSourceID = 6. No terminal in the E1 is associated with another. |
| 2 | EP 0x01 async with feedback EP 0x83 (3 bytes, bRefresh 5). CS EP has no sampling-frequency control | **CONFIRMED** | Descriptor bytes as quoted. **Live:** during playback the hub lists open pipes `EP01 attr05 mps300` and `EP83 attr01 mps3`, so the iso-feedback path is used, not vendor polling. |
| 3 | AVStream minidriver with its own URBs: IOCTL 0x220003, iso 0x0A, VENDOR_OTHER 0x20 | **CONFIRMED (minor corrections)** | W7@1400bc7ae writes 0x220003. W7@1400b77c8 and @1400b8c0f set Function 0x000A. Imports: 26 ks.sys functions, 0 Pc*, plus `drmk!DrmForwardContentToDeviceObject`. Corrections to §2.2: (a) when IRQL ≥ DISPATCH the helper returns **0xC0000022** (STATUS_ACCESS_DENIED), not STATUS_UNSUCCESSFUL. (b) In restricted mode (`usbdev+0x78`) a request that is filtered out is dropped **silently with STATUS_SUCCESS**. (c) The iso URB `Length` is `0x98 + 12*n`, which is 12 bytes more than `GET_ISO_URB_SIZE(n) = 0x8C + 12*n`. The extra 12 bytes are harmless. Use the macro in a new driver. |
| 4 | E1 data ranges are 44.1 kHz only (render 2ch 16–24, mic 1ch 16, WUH 2ch 16), identical in W7 and W10 | **CONFIRMED** | W7@1400b0aa0: 20-byte record {ch, rateA, rateB, maxBits, minBits}. Pin 0 = {2, 44100, 44100, 24, 16}, pin 5 = {1, …, 16, 16}, pin 6 = {2, …, 16, 16}. W10@1400b0c9c has the same immediates. Live endpoint formats agree: render DeviceFormat = EXTENSIBLE 2ch/44100/**24-in-24 (block align 6)**/mask 3. "Microphone" = 1ch/44100/16/mask 4. "What U Hear" = 2ch/44100/16/mask 3. |
| 5 | Feedback: 3-byte value `<<2`, ±2-sample clamp replaced by `rate*65536/1000`. Per-mille accumulator when there is no feedback | **CONFIRMED** (W7) / **CORRECTED** (W10 HS branch) | W7@1400b9a6c: len ≥ 4 → used as-is; len == 3 → `(b0 | b1<<8 | b2<<16) << 2`. W7@1400b6e98: `n = rate/1000` (via 0x10624DD3 >> 6). The value is rejected if `fb > (n+2)<<16` or `fb < (n-2)<<16` (bounds are inclusive), then replaced by `(rate<<16)/1000` for rate ≤ 48000, else `((rate<<10)*0x10624DD3 >> 32) & ~0x3F`. The result is stored at +0x70 and split into +0x8C (fb/ppf) and +0x90 (fb%ppf). **W10 difference, missed in §6:** the W10 parse (W10@1400bd1e8) **doubles a 4-byte (high-speed) feedback value** (`add eax,eax`), while W7 uses it unchanged. The 3-byte FS path is identical. The E1 at FS is unaffected. |
| 6 | 3 URBs × 10 frames, render pulled in 5 ms chunks, same in both builds | **CONFIRMED** | W7@1400b9f71 and W10@1400bd6ed both write `qword [+0x40] = 0x000A0003`. Stream start W7@1400ba670 reads it as **word +0x40 = 3 URBs** and **word +0x42 = 10 frames**; dword +0x44 is a separate field (0). Render builder W7@1400b8a50 pulls from `pipe+0x280`→vtbl+0x18 every 5 frames (`inc ebp; cmp ebp,5`). Missed detail: while `pipe+0x290 > 0`, the builder writes **zeros instead of pulling** and decrements the counter. This is a pre-roll silence count. Resync rule confirmed (W7@1400b784c): if the stored frame < current or > current+100, it becomes current+5. |
| 7 | SRCs: 16-bit cubic and 32-bit linear, 16.16 step, memcpy bypass at 0x10000, round-to-nearest, no dither. dB→linear | **CONFIRMED (with exact coefficients)** | W7@14003586d/@140041515 `cmp [ctx+8],0x10000` → memcpy (W7@140035d74). Constants: frac scale 1/65536 (1.52587890625e-05), 0.5 and 1.0 (the cubic uses 0.5 weights, Hermite/Catmull-Rom), a **+1.5e-8 bias** before rounding, and round via `+6755399441055744.0` (1.5·2^52). Clamp is **[-32768, +32767]** (0xFFFF8000 / 0x7FFF), not ±32767. dB→linear W7@140015e30 on a float array: `y = dB*0.16609640`; `i = floor(y)`; `f = y-i`; `g = ldexp(((0.039711263*f + 0.23278488)*f + 0.69307363)*f + 1.0 - 1e-6, i)`. The value 0 dB gives exactly 1.0. The same code exists in W10 (byte-identical prologues at W10@140035840, @140041670, @140015e60). |
| 8 | E1 render at 44.1 kHz passes through without kernel DSP | **UNVERIFIABLE (strengthened)** | Static: the E1 format mapper W7@1400b01e8 leaves pin-0 rate and bits (≤24) unchanged, so the SRC step for 44.1→44.1 is 0x10000. The engine pull (`pipe+0x280`) was not traced end to end, and software nodes 3/7 sit in the path. **Live:** the device really runs **alt2 / 24-bit** (EP01 wMaxPacketSize 300) during playback, even though the registry has `KSAud_Device\PCMBitDepth = 16`. That rules out a forced 24→16 truncation on the USB side in the current setup. Bit-exactness itself is still unproven; it would need a loopback or USB capture of a known signal. |
| 9 | WUH is a kernel software loopback | **UNVERIFIABLE (plausible)** | The descriptor has no stereo IN interface. Live: playback opens only EP01 and EP83, and no capture pipe. The WUH endpoint is 2ch/16-bit. I did not open a WUH capture to watch the pipes. |
| 10 | Vendor 0x29 (wValue 1/0, wIndex 2000) switches HS/FS, then IoInvalidateDeviceRelations. USBMode default 1, ForceUSB | **CONFIRMED** | W7@1400afffc: GET 0x2A (IN, 2 B, VENDOR_OTHER). If `!IsHighSpeed && !(st&1)`: SET 0x29 wValue=1 wIndex=0x07D0, then usbdev vtbl+0xC8 (= `IoInvalidateDeviceRelations(PDO, BusRelations)`, W7@1400bd5da), and return **0xC0000182** (STATUS_DEVICE_CONFIGURATION_ERROR). W7@140093ac8 is the reverse (wValue=0, only if HS). Policy W7@1400b25b8: `(st&1) && USBMode==1` → vt+0x4E8 (to FS); `!(st&1) && USBMode==2 && ForceUSB==0` → vt+0x4E0 (to HS). Registry paths verified: `\REGISTRY\MACHINE\SYSTEM\CurrentControlSet\Enum\USB\VID_041E&PID_323B&MI_00\` value `USBMode` (default 1), and `…\Control\Creative Tech\USB\` value `ForceUSB`. |
| 11 | HS mode does not unlock higher rates | **UNVERIFIABLE** | No HS descriptor set is available. The E1 data ranges (claim 4) are mode-independent, so Creative's driver would still expose only 44.1 kHz. What the hardware offers in HS is unknown. |
| 12 | GET_MEM 0x85 for jack/accessory codes (0x0C/0x17/0x2D) and switch states (wIndex 0x0300/0x0400 \| IF0) | **CORRECTED** | (a) The jack-code path W7@1400b0f74 (E1 vt+0x338) is right: GET_MEM wValue=0, wIndex=(0<<8)\|IF0, 4 bytes, and only **byte 0** is used. Mapping: 0x0C→0x0C, 0x17→0x17, 0x2D→**0x1A**. **Any other value is also forwarded unchanged**, because the `cmp eax,0x41` range check is overwritten by a following `test` and is dead code. The event GUID is {37E60DF2-…} or {4AB30A74-…}, chosen by W7@1400bee34. It is only reached through notification type 5 in dispatcher W7@14008d8d8. (b) **The wIndex 0x0300/0x0400 reads (W7@1400ab600) never run on the E1.** That function is slot +0x3F0 of the base class vtable 0x140113FA0 (ctor W7@1400aa910). The E1 vtable overrides +0x3F0 with `xor eax,eax; ret` (W7@140001a60). Also, `0x03xx/0x04xx` is **entity ID 3/4 (OT3/OT4)**, not a flag OR'd into the interface. (c) W7@1400b26c4 is slot +0x338 of the **PID 0x323E** class (vtable 0x140116AB8), not the E1. |
| 13 | FU5 ranges: GET_MIN/MAX/RES with wValue 0x0200\|ch, `<<8`. Nodes 3 and 7 = −96..+6 dB / 1/16 dB | **CONFIRMED** | W7@1400b074d–@1400b0802: bRequest 0x83/0x82/0x84 through ClassInterface (vtbl+0x78 = W7@1400bca1c, which builds `wIndex = (r9 & 0xFF00) \| AC_IF` and uses URB function 0x1B). Values are sign-extended and `<<8`. Nodes 3 and 7: 0x60000 / 0xFFA00000 / 0x1000. Node 10 goes through helper W7@1400ab8a0 (min −96 dB, step 1/16 dB). |
| 14 | {12C98F26} = VoiceFX formant params, {5FA7CE37} = Silencer NR params | **CONFIRMED (upgraded from INFERRED)** | Each W10 set handler passes its own registry name in `lea r8`. The mapping is direct, not inferred from order. **12C98F26:** 0 FormantFreq1, 1 FormantFreq2, 2 FormantFreq3, 3 PitchFactor, 4 FormantFactor, 5 PitchVariability, 6 QuiverDepth, 7 ContourDepth, 99 "State" (ULONG; set W10@140163f04), 98 get-only. **5FA7CE37:** 0 AdaptationStep, 1 Selectivity, 2 FastSmoothFactor, 3 SlowSmoothFactor, 4 MaxAttenuation, 5 VoiceEQLowFGain, 6 VoiceEQHighFGain, 7 VoiceEQFullGain, 8 LowFOverestimationFactor, 9 MidFOverestimationFactor, 10 HighFOverestimationFactor, 11 FilterLimit, 99 "State" (ULONG; W10@14016629c), 97 and 98 get-only. **Correction:** parameters 0..11 are **IEEE float32** (`movss xmm1,[buf]`; MinData check ≥ 4), not ULONG. In W10@140163960 the registry write (REG type passed as 0xD) and the change broadcast with the set GUID (W10@1400d5450) happen **only on the branch where the DSP object is newly created through vt+0x180**; that reading is [I]. GUID byte counts: KSUSBAPO64 13+13, KSUSBSPI64 2+2, KsUSBaud 7+5. |
| 15 | W10 vs W7: new sets 06261394/761E4C5B, C4BD0511→447845DF, 63A37602 grows 1→2, descriptor-driven alt matching, streaming constants unchanged | **CORRECTED** | The GUID byte presence is confirmed: 06261394 W7 0/W10 2, 761E4C5B 0/5, 447845DF 0/3, C4BD0511 3/0. W10@14008c688 checks `+0x126C` and PID 0x323B pin 5. "Renamed" is an interpretation [I], because the item shapes match. Not quite "unchanged": (i) the HS feedback ×2 change (row 5). (ii) W10 has an embedded Authenticode signature (security directory 0x17FC00, 8712 B, "Windows Hardware Compatibility Publisher" chain). The W7 `.sys` has **no** embedded signature (security directory empty), so it relies on its catalog only. (iii) W10 adds an `RtlCompareMemory` import, ANSI topology node-name tables ("Line-In Volume", "Bluetooth Volume", …), and the `ProcessIntData bResetStream` debug strings. (iv) The string `CAudioFilterFactory::SupportAudioSignalProcessingModes` exists **only in W7**. |

### V.2 Things the analyst missed (kernel/USB subsystem)

1. **E1 format mapper, KS format → device format** (W7@1400b01e8 = E1 vt+0x3E0; W10@1400b0864, same constants) [C]:
   - Pin 0: a 4-channel request becomes **6 ch / mask 0x3F**, a mono request becomes **2 ch / mask 3**, and bits are capped at 24. Rate is not touched.
   - Pins 5 and 6: rate is forced to 44100 and bits are capped at 16.
   - Pin 5: a stereo request becomes **1 ch / mask 1** and sets a downmix flag at `+0x28C=1`. Otherwise mask 4.
   - Pin 6: mono becomes stereo / mask 3.

   A new driver must do this mono/stereo up- and down-mixing itself.
2. **E1 stream start** (W7@1400af620 = E1 vt+0x2D8 → FS path W7@1400af660, HS path W7@140088b04) [C]:
   - Starting the render pin also re-programs the device format of slot 5 (mic).
   - On **STATUS_INSUFFICIENT_RESOURCES (0xC000009A)**, i.e. isochronous bandwidth refused, it retries with **24→16-bit**, then 96k→48k (and 48k→44.1k in the base W7@1400aad18).

   So on a crowded bus Creative silently drops to 16-bit. Live, during this test the render ran at 24-bit (mps 300) and **opened no mic pipe**.
3. **Registry device format** (W7@14008caa8) [C]:
   - At start, `KSAud_Device{,2..10}\SPDIFOutCurRate` (DWORD), `PCMSamplingRate` (DWORD) and `PCMBitDepth` (REG_BINARY 2 bytes) are applied to device-format slots 0..4.
   - The E1 accepts only slot 0 (E1 vt+0x68 = W7@1400b2850 → slots 0, 5, 6).
   - The bit-depth setter W7@140090b58 validates the value against the supported list, rewrites the slot (vt+0x98 = W7@1400b2ab4), persists it, and fires an event.
   - Live `PCMBitDepth=16` did **not** force alt1 in the probe, so the KS pin format takes precedence when a stream opens [I from the live probe].
4. **Pin 8 / vendor 0x21 / 0x2E** (W7@1400874cc = E1 vt+0x2F8) [C]:
   - Pin 0 start sends `0x21 wValue=0 wIndex=1` unless the pin-8 flag (`+0x2448`) is set.
   - Pin 8 sends `0x21 wValue=(rate≠44100) wIndex=1`, then `0x2E wValue=1` (FS only), and sets `+0x2448=1`.
   - This suggests the E1 firmware has a vendor-switched 48 kHz clock for an auxiliary path [I]. Testing it would be a way to check for 48 kHz support without HS mode. Untested.
5. **Restricted-mode allow list** (`0x60000F00073`) decodes exactly to bRequest {0x00, 0x01, 0x04, 0x05, 0x06, 0x14–0x17, 0x29, 0x2A} [C]. Everything else is silently answered with success.
6. **The jack-code dispatcher input** (W7@14008d8d8, notification type 5 → vt+0x338) has no source on the E1's own interfaces: there is no AC interrupt endpoint, and EP 0x84 is owned by HidUsb. The bit7-tagged status word must come from a side channel (HID or user mode) [I]. This is an open item for jack-detect reimplementation.
7. **Feedback/URB flags:**
   - The feedback/IN URB builder W7@1400b7780 uses `TransferFlags = pipe+0x64 | 3` (IN | SHORT_TRANSFER_OK).
   - It uses an explicit StartFrame only when `pipe+0x64 == 0`, advancing by `packets/ppf`.
   - Initial nominal feedback before the first report = `(rate<<16)/1000` (W7@1400ba802).

   [C]
8. **drmk.sys import** (`DrmForwardContentToDeviceObject`): the driver takes part in the DRM/copy-protection chain (KSPROPERTY_AUDIO_COPY_PROTECTION). A replacement driver does not need it unless protected-content playback is required [C import / I relevance].

### V.3 Net effect on the re-implementation plan

- The wire protocol for the E1 is confirmed: UAC1 FS, IF1 alt2 (24-bit packed, wMaxPacketSize 300), EP 0x83 10.14 feedback, FU5/FU6, GET_MEM jack codes on IF0, and vendor 0x29/0x2A/0x21/0x2E/0x01/0x92/0x2B. Any new driver should be built from this summary, not from Creative's object layout.
- The quality conclusion of §3.5 still holds and is now backed by a live probe: Win10 Creative already streams 24-bit/44.1 kHz to the hardware. The audible difference lies above the kernel (APO/SBX/Dolby chain, endpoint SRC). It does not come from the USB format.
- If a kernel driver keeps Creative's behaviours, do **not** copy the silent 24→16 bandwidth fallback without telling the user. Use the documented iso URB size macro.
