# E1 Studio — Architecture

Developer documentation for the E1 Studio stack (Creative Sound Blaster E1, `USB\VID_041E&PID_323B`,
Windows 11 x64). The reverse-engineering background is in [`RECONSTRUCTION_SPEC.md`](RECONSTRUCTION_SPEC.md)
and `docs/re/*.md`; user documentation (pt-BR) is the top-level `README.md`.

## 1. Big picture

```
 apps (shared mode, any rate)                                  apps (exclusive mode 44.1/16|24p)
        |                                                                 |
        v                                                                 |
 Windows audio engine (audiodg.exe, LOCAL SERVICE)                        |
   mix at 44.1 kHz float32 -> [MFX: e1fx.dll render APO] -----------------+--> ksUSBaud.sys (Creative, WHQL)
                                  ^        |  status/meters                        |  isoch OUT 24-bit,
                                  |        v                                       |  async feedback EP 0x83
                       Global\E1StudioFxParams_v1  (E1SharedBlock, seqlock)        v
                                  ^        ^                                 Sound Blaster E1
       e1panel.exe (user) --------+        +-------- e1ctl.exe (user)
              |                                           |
              +----> C:\ProgramData\E1Studio\settings.bin <+   (CRC-checked E1SettingsFile, read by the APO
                                                                when it creates the section)
 mic: E1 -> ksUSBaud.sys -> [MFX: e1fx.dll capture APO] -> apps
```

* **No kernel code.** The transport is Creative's signed `ksUSBaud.sys` (bit-transparent 24/44.1 with
  correct async feedback, verified live). The inbox `usbaudio.sys` is the planned alternative once its
  feedback handling is verified (spec §7 T2). Nothing in this repository depends on which one is used,
  except that "What U Hear" only exists with Creative's driver.
* **One APO DLL, two classes**, both registered as **MFX** (`PKEY_FX_ModeEffectClsid`) for the DEFAULT
  processing mode: render `{9041657D-F768-4739-B84D-7CA73B55FE94}`, capture
  `{6768B677-635E-46A6-9814-8560B27B571D}`. No SFX (no per-stream cost, no matrix encoder), no EFX (RAW
  streams stay untouched where the transport offers RAW).
* **Default = bit-exact.** With `master_on == 0` (and, for render, `eq_on == 0`) the chain is a
  zero-latency `memcpy`. The APO also forces bypass when Windows "Audio enhancements" are off
  (`IAudioSystemEffects3`) or when instantiated for RAW mode.

## 2. Components

| Path | Output | Owner | Notes |
|---|---|---|---|
| `include/` | — | lead | **Contracts** (see §3). Never change declarations; append-only. |
| `src/dsp/` | `libe1dsp.lib` | dsp-core / dsp-spatial / dsp-voice | Pure C, no OS headers, no allocation after `*_create`. |
| `src/common/` | `e1common.lib` | apo | `e1fx_ipc.c`: section, seqlock, settings file, CRC-32. |
| `src/apo/` | `e1fx.dll` | apo | C COM: `IAudioProcessingObject{,RT,Configuration}`, `IAudioSystemEffects2/3`, notifications; `.def` exports `DllGetClassObject/CanUnloadNow/RegisterServer/UnregisterServer`. |
| `src/panel/` | `e1panel.exe` | panel | Native Win32 C UI (own artwork, neutral names, pt-BR/en). |
| `tools/` | `e1ctl.exe`, `install.ps1`, `uninstall.ps1`, `E1Common.psm1` | tools | CLI + installer. |
| `tests/<dir>/`, `tools/tests/` | test programs | each owner | Run by `build.cmd test`. |
| `build.cmd` | `build\x64\<cfg>\` | tools | Single entry point for building and testing. |

## 3. Contracts

### 3.1 Parameters — `include/e1fx_params.h`
`E1Params { version, size, E1RenderParams render, E1CaptureParams capture }`: plain old data, fixed
layout, copied verbatim through shared memory and the settings file. **Never reorder fields**; append and
bump `E1FX_PARAMS_VERSION`. Every consumer runs `e1fx_params_sanitize()` (clamps, NaN/Inf -> default)
before use. `e1fx_params_default()` gives master switches OFF (bit-exact) with sub-effects at the original
panel defaults (surround 0.40, clarity 0.65, bass 50 / 80 Hz, smart volume 0.74, dialog 0.5,
limiter on).

`tools/e1ctl_keys.c` maps every field to a dotted key (`render.surround_level`, `render.eq_gain_db[3]`
alias `render.eq.250`, `capture.vfx.pitch`, ...). `tools/tests/test_e1ctl_keys.c` proves that **every byte
of `render` and `capture` is reachable through exactly one key**, so the CLI cannot drift from the
contract silently, and that every legal value survives `e1fx_params_sanitize` unchanged.

### 3.2 IPC — `include/e1fx_ipc.h`
* `Global\E1StudioFxParams_v1`: one `E1SharedBlock` (magic, size, seqlock counter, params, per-direction
  `E1ApoStatus` with heartbeat, rate, channels, bypass flag, peaks, gain reduction, xruns). SDDL grants
  SYSTEM/Administrators/LOCAL SERVICE full and interactive/authenticated users read/write. Created by
  whichever side comes first; the APO (service context) can always create Global objects, the panel may
  not (`SeCreateGlobalPrivilege`) and then only writes the settings file and retries.
* **Seqlock**: writers (panel, `e1ctl`, section initialisation) are mutually exclusive: a writer claims
  the odd state with a compare-exchange from an even value, so `seq` only grows and a reader never accepts
  a mix of two writers' data; a writer that died mid-update (odd for > 100 ms) is taken over. Readers are
  wait-free in the real-time thread and start with `last_seq = -1`.
* `C:\ProgramData\E1Studio\settings.bin`: `E1SettingsFile { magic, crc32(params), params }`, written
  atomically (temp file + `MoveFileEx(REPLACE_EXISTING)`), read by the APO when it creates the section.
  The read never blocks: `NtCreateFile` with `FILE_COMPLETE_IF_OPLOCKED` (an oplock holder that never
  acknowledges the break only costs four 5 ms retries, then defaults are used), reparse points are not
  followed and UNC paths are refused — the file is user-writable and the APO reads it on audiodg's
  control path.
* Test-only overrides (honoured by `e1fx_ipc.c`): `E1FX_SECTION_OVERRIDE` (only `Local\...` names) and
  `E1FX_SETTINGS_OVERRIDE`. `tests/apo` and `tools/tests/test_e1ctl_cli.c` use them, so no test touches
  the production section or ProgramData.

### 3.3 COM identities — `include/e1fx_guids.h`
Our own GUID namespace (nothing Creative). `tools/E1Common.psm1` repeats the two CLSID strings;
`tools/tests/selfcheck.ps1` fails if they ever differ from the header.

### 3.4 DSP — `src/dsp/e1dsp.h`, `e1dsp_modules.h`, `e1_biquad.h`
Chain API (`e1_render_*`, `e1_capture_*`) and the internal module contract: planar 128-frame blocks
(the original engine's granularity, so all time constants match), any host frame count re-blocked
internally, in-place safe, finite output for any input, exact 0.0f decay for digital silence, double
precision recursive state, click-free parameter ramps, constant reported latency while active and a
20 ms crossfade between bypass and active.

Warm-up (click-free switching): a module that fades in from fully off is reset, then runs on the real
signal with its crossfade weight held at 0 for its *warm* time — its latency plus a settle time (another
latency for STFT/look-ahead modules, 256 frames for IIR modules, and for Surround its pre-delay plus nine
round trips of the longest all-pass plus the FIR length, `e1_surround_settle_frames`, ≈ 3.5k frames). Only
then does the 20 ms raised-cosine fade start, so the module side of the mix never contains start-up zeros
(VoiceFX/NR used to mute the mic for 24–30 ms) or onset transients (Surround's ambience used to click in at
+573 frames). Likewise bypass → active keeps the output on the zero-latency dry signal for
`latency + max(settle of the enabled slots)` frames before the top-level fade starts. Turning effects on
therefore takes roughly 0.1 s to complete (render with Surround, capture with VoiceFX: warm-up
≈ 85–100 ms + 20 ms fade); turning them off is unchanged (20 ms). `tests\dsp\test_chain.c` toggles every slot and the master at
random times on tones and requires the maximum second difference to stay at its steady-state value.

Integration rules every component relies on (checked by `tests\dsp\test_chain.c` and `tests\apo\apohost.c`):
* Render is active when `master_on || eq_on || scout_on`. Scout Mode alone engages the chain (Scout EQ +
  surround 0.9). The APO's Windows gates (enhancements off, `Disable_SysFx`, RAW mode) therefore clear all
  three, and its effects list counts Scout Mode as active.
* The safety limiter (-0.3 dBFS) is in the path whenever the chain is active (render: and `limiter_on`;
  capture: always). Below the ceiling it is an exact delay, so this costs no transparency, and module
  crossfades can never overshoot the ceiling.
* Latency while active is constant: render 288 frames (6.5 ms at 44.1 kHz), capture 2592 frames
  (58.8 ms, dominated by the VoiceFX STFT) whenever any capture effect is on. Bypass is zero latency.
  The APO resets the chain at the end of `LockForProcess`, so it starts directly in its target state and
  `GetLatency` (read once by the engine while it builds the graph) is already the latency of the path that
  will run. A later bypass/active change does not renegotiate the graph: the engine keeps the latency it
  read at Lock time until the stream is re-locked.
* Clarity is loudness-neutral on its static tilt: its output gain is 1/sqrt(P), P = the K-weighted pink
  power gain of the LF/HF shelves at the current level (tabulated at create time). Measured: ±0.1 LU on
  pink noise at every level; on music about +1.3 LU remains from the transient-driven dynamic boost.
* Smart Volume keeps short loud events out of its slow loudness estimate: frames 6 dB above
  target + headroom (after the slow gain) are left to the fast stage for up to 1 s, and every frame can pull
  the 400 ms average up by at most a frame 6 dB above it. The mic tuning's slow cut is as slow as its
  boost (1.5 s). A 100 ms cough at -3 dBFS over -26 dBFS speech now leaves the voice within 1 dB after
  about 1 s (was -8 dB after 2 s and -1.3 dB after 8 s).
* The Bass module's LR4 crossover is minimum phase: below the crossover the signal carries its group
  delay (about 6 ms at 80 Hz) on top of the reported latency. Higher bands are sample-aligned.

## 4. Real-time rules (APO process path and every `*_process`)
No allocation, locks, I/O, syscalls or logging; bounded time; parameter pickup through the wait-free
seqlock reader once per buffer; status written without locks (torn reads are acceptable for meters).
Denormals: FTZ/DAZ in the APO plus flush helpers in the DSP. The build forbids `/fp:fast`.

## 5. Build system (`build.cmd`)

```
build.cmd [release|debug] [test] [full] [clean] [nowx] [nops]
```

* Finds MSVC through `vswhere` (falls back to the 2022 Build Tools path) and calls `vcvars64.bat` unless
  an x64 developer environment is already active.
* **Wildcard components**: every `*.c` in `src\dsp`, `src\common`, `src\apo`, `src\panel` and `tools` is
  compiled, so new files need no build edits. `*.rc` are compiled with `rc`, a `*.def` in `src\apo` is
  passed to the linker, `*.manifest` in `src\panel` is embedded (or the linker manifest is disabled when
  an `.rc` already contains `RT_MANIFEST`). Extra import libraries: a `link.libs` file in the component
  directory.
* **Flags** (all code): `/std:c11 /W4 /WX /utf-8 /fp:precise /guard:cf /GS /Zi`, `/O2 /MT` (Release) or
  `/Od /MTd /RTC1` (Debug); link `/DEBUG /OPT:REF /OPT:ICF /GUARD:CF /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA`.
  x64's baseline is SSE2, so no `/arch` switch (AVX2 is not allowed). The static CRT (`/MT`) lets
  `e1fx.dll` load into `audiodg.exe` without a Visual C++ redistributable.
* **Outputs**: `build\x64\<cfg>\` (`libe1dsp.lib`, `e1common.lib`, `e1fx.dll`, `e1panel.exe`,
  `e1ctl.exe`, PDBs, test programs and tools); objects and response files in `build\obj\x64\<cfg>\`.
* **Test conventions** (`tests\<dir>\`, `tools\tests\`): each `test_*.c` is a program; a non-test `.c` with
  `main`/`wmain`/`WinMain` is a *tool* (built, not run — e.g. `e1wavfx`); other `.c` files are helpers
  linked into every program of the directory; a directory without `test_*.c` builds one program
  `test_<dir>.exe` (e.g. `tests\apo\apohost.c` -> `test_apo.exe`). Programs link `libe1dsp.lib`,
  `e1common.lib` and a *static library* of the component with the same name (`tests\apo` ->
  `apo_t.lib`, `tools\tests` -> `e1ctl_t.lib`), so only referenced objects are pulled in (the APO host
  re-defines the DLL's bookkeeping symbols; linking the objects directly would clash). Arguments come from
  `<testdir>\<program>.args`; `test_spatial` gets `quick` unless `full` is given. Tests run in the output
  folder (so `apohost` finds `e1fx.dll` and `test_e1ctl_cli` finds `e1ctl.exe`) with `E1_ROOT` and
  `E1_TEST_SRC` set. A test passes when it exits with 0; then `tools\tests\selfcheck.ps1` runs (`nops`
  skips it).
* `E1_SKIP="panel apo ..."` skips a component that is temporarily broken while someone works on it; the
  build reports it and `test` counts it as a failure, so it cannot go unnoticed.
* The component-local scripts (`src\apo\build_e1fx.cmd`, `tests\*\build*.cmd`) remain usable for quick
  iteration by their owners; `build.cmd` is the reference build.

## 6. Installer (`tools/install.ps1`, `tools/uninstall.ps1`, `tools/E1Common.psm1`)

### 6.1 Principles
* Elevation required, **except `-WhatIf`**, which runs unelevated, only reads, and prints one
  `What if:` line per change (registry value, file, folder ACL, service, task). All mutations live in the
  two scripts inside `$PSCmdlet.ShouldProcess`; the module only reads and plans. `selfcheck.ps1` statically
  checks that every `SetValue`/`DeleteValue`/`DeleteSubKeyTree`/`SetAccessControl` call is guarded, and
  forbids `ForEach-Object <member>` / `Get-FileHash`, which silently return nothing under `-WhatIf`.
* **Idempotent**: plans are computed from the current state; a second run finds an empty plan, writes no
  backup and does not restart audio.
* 64-bit registry view always; well-known SIDs instead of account names (the user's Windows may be
  localised, e.g. "Usuários").

### 6.2 Endpoint discovery
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\{Render|Capture}\{id}`: an endpoint belongs
to the E1 when `Properties\{b3f8fa53-0004-438e-9003-51a46e139bfc},2` (device instance id) or
`{233164c8-1b2c-4c7d-bc68-b671687a2567},1` (KS filter path) contains `VID_041E&PID_323B`. Role from
`PKEY_AudioEndpoint_JackSubType` `{1da5d803-...},8`: `KSNODETYPE_SPEAKER` -> render, `KSNODETYPE_MICROPHONE`
-> mic, `KSNODETYPE_DIGITAL_AUDIO_INTERFACE` -> "What U Hear" loopback (never touched); fall back to the FX
association and the name. NOTPRESENT endpoints are skipped unless `-IncludeNotPresent`. Live system
(2026-11-14): render `{7e65a67a-...}`, mic `{389c9c75-...}`, What U Hear `{1d53f1ec-...}`, stale mic
`{3ea0f972-...}` (NOTPRESENT).

### 6.3 FX plan (per endpoint, `Get-E1FxPlan`)
| Value in `FxProperties` | Action |
|---|---|
| `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6` (MFX) | set to our CLSID (REG_SZ) |
| `,1` LFX, `,2` GFX, `,3` UI, `,5` SFX, `,7` EFX, `,13/,14/,15` composite lists | delete |
| `,0` association | set to the jack subtype (or `KSNODETYPE_ANY`) only if missing |
| `{d3993a3f-99c2-4402-b5ec-a92a0367664b},6` (MFX processing modes) | ensure it contains `AUDIO_SIGNALPROCESSINGMODE_DEFAULT` |
| `{b725f130-47ef-101a-a5f1-02608c9eebac},10` (effects display name) | "E1 Studio" |
| Creative private parameter values (`{5b4777a4-...}` etc.) | left alone (inert without Creative's APO; restored anyway) |

`-SetFormat24` additionally writes `PKEY_AudioEngine_DeviceFormat` `{f19f064d-...},0` in `Properties`
(8-byte PROPVARIANT header preserved + `WAVEFORMATEXTENSIBLE` 44100 Hz, 24/24-bit PCM, stereo, mask 0x3)
when it differs; the generated blob is byte-identical to what the Creative INF wrote on this PC.

### 6.4 Registry ACL findings (read-only inspection, 2026-11-14, re-checked 2026-11-15, build 26300)
`MMDevices\Audio\{flow}\{id}`, `\Properties` and `\FxProperties` are owned by SYSTEM and inherit one DACL
(every ACE `IsInherited`; the explicit ACEs sit on the `MMDevices\Audio` parent):

| Trustee | Rights |
|---|---|
| NT SERVICE\Audiosrv, NT SERVICE\AudioEndpointBuilder | SetValue, CreateSubKey, CreateLink, Delete, Read |
| NT SERVICE\TrustedInstaller | FullControl |
| BUILTIN\Administrators | QueryValues, SetValue, EnumerateSubKeys, Notify, ReadPermissions — **no** CreateSubKey, Delete, WRITE_DAC or WRITE_OWNER |
| BUILTIN\Users | QueryValues, SetValue, CreateLink (+ read) |
| ALL APPLICATION PACKAGES, one capability SID | read (the capability SID also Query/SetValue) |

Consequences:
* An elevated administrator can set **and delete** values (RegDeleteValue needs only `KEY_SET_VALUE`)
  without touching the ACL — **if** the key is opened with exactly those rights. PowerShell's
  `Set-ItemProperty`/`Remove-ItemProperty` open with `KEY_WRITE` (includes `KEY_CREATE_SUB_KEY`) and fail
  with "access denied" even elevated, so the scripts open keys through `RegistryKey.OpenSubKey` with
  explicit `RegistryRights`.
* Creating a missing `FxProperties` key needs `KEY_CREATE_SUB_KEY`. For that (and for any key whose ACL
  ever denies us) the scripts enable `SeBackupPrivilege` + `SeRestorePrivilege` and open with
  `REG_OPTION_BACKUP_RESTORE` (small C# helper compiled with `Add-Type`). The privileges grant the access;
  the DACL and owner are neither read nor modified, so — unlike take-ownership + re-ACL — there is nothing
  to restore and no window with a weakened ACL. The SDDL of each touched key is recorded in the backup
  manifest for auditing.
* Users already hold SetValue on these keys (that is how Creative's 32-bit panel wrote its parameters);
  E1 Studio does not rely on it — the panel talks to the APO through the shared section instead.

### 6.5 Backup and restore
`C:\ProgramData\E1Studio\backup\<yyyyMMdd-HHmmss>[-reapply]\`: `reg export` of each changed endpoint's
`FxProperties` and `Properties`, of the COM/APO registrations of every foreign CLSID found there
(`Classes\CLSID`, `Classes\WOW6432Node\CLSID`, `Classes\AudioEngine\AudioProcessingObjects`) and
`manifest.json`:

```json
{ "Format": 1, "Name": "...", "Created": "ISO-8601", "Kind": "install|reapply", "InstallId": "guid",
  "Endpoints": [ { "Flow": "Render", "Id": "{...}", "Role": "render", "WasApplied": false, "FxExisted": true,
                   "FxValues": [ { "Name": "{fmtid},pid", "Kind": "String|Binary|DWord|QWord|MultiString|ExpandString",
                                   "Data": "... (Binary = base64, DWord unsigned)" } ],
                   "FxSddl": "O:SYG:SYD:AI(...)", "FormatChanged": false, "DeviceFormat": null } ],
  "RunEntries": [ { "Root": "HKLM32", "SubPath": "...\\Run", "Name": "...", "Kind": "String", "Value": "..." } ],
  "ForeignClsids": [ "{...}" ], "RegExports": [ { "Key": "...", "File": "...", "Ok": true } ] }
```

`backup\install.json` records the installation (`InstallId`, options, backup list). The uninstaller makes
each endpoint's `FxProperties` **identical** to its snapshot (extra values deleted, every original value
rewritten with its original type and bytes). Snapshot choice per endpoint: the oldest snapshot of the
current installation taken before E1 Studio was applied (the original Creative state), so later
`-Reapply` backups of re-created endpoints never shadow the original; `-Backup <name>` restores one backup
only. E1 endpoints that use E1 Studio without any snapshot get our values removed. The `.reg` files are
for manual recovery only and are **never imported automatically**.

If any registry write fails during install, the script rolls the endpoints of this run back to their
snapshot before exiting.

### 6.6 Files, folders and ACLs
| Path | ACL | Contents |
|---|---|---|
| `C:\Program Files\E1Studio\` | inherited (admins write, users read) | `e1fx.dll`, `e1panel.exe`, `e1ctl.exe`, PDBs, the three scripts (used by the task and for uninstall), README |
| `C:\ProgramData\E1Studio\` | owner Administrators; protected: SYSTEM/Administrators full, **LOCAL SERVICE Read**; **Users: on the folder itself only list + create files** (no delete/rename, no subfolders, no ACL change), **Modify on files only** (inherit-only) | `settings.bin` (panel writes a temp file and renames it over `settings.bin`, APO reads) |
| `...\backup\` | protected: SYSTEM/Administrators only | backups, `install.json` |
| `...\logs\` | protected: SYSTEM/Administrators full, Users read | install/reapply/uninstall logs (30 kept) |

Security notes: anything an elevated or SYSTEM process later trusts (backups, install state, the scripts the
scheduled task runs) lives in admin-only locations; the installer refuses to continue if any of the
ProgramData folders is a reparse point; the SYSTEM re-apply task never writes into the user-writable folder
(`settings.bin` is only created by the interactive admin install, and only when absent).
* Folders are created atomically with their final security descriptor (`Directory.CreateDirectory(path,
  DirectorySecurity)`), so there is no window with the inherited `C:\ProgramData` ACL (where Users may
  create subfolders). "ACL correct" means owner Administrators/SYSTEM **and** the exact DACL.
* Before install, re-apply or uninstall use anything there (and before a log is written or rotated):
  the `E1Studio` root must be owned by Administrators/SYSTEM (a planted replacement tree is owned by its
  creator), and `backup\` and `logs\` must be owned by Administrators/SYSTEM with no write, delete,
  WRITE_DAC or WRITE_OWNER right for anyone else; otherwise the scripts refuse. `install.json` and every
  `manifest.json` are additionally ignored unless Administrators/SYSTEM own them and nobody else can
  write them. (Earlier versions gave Users Modify on the root; an upgrade resets it.)
* Manifest records are validated before an elevated restore writes anything: endpoint `Flow` must be
  `Render`/`Capture`, `Id` a GUID, value names `{fmtid},pid`, and `RunEntries` must be `HKLM64`/`HKLM32`/
  `HKCU` + exactly `SOFTWARE\Microsoft\Windows\CurrentVersion\Run` + a string value that starts
  `SBE.exe`. The Run key is read through a read-only handle and opened for write only inside
  `ShouldProcess`, so an unelevated `-WhatIf` shows "already present" / the planned change.

### 6.7 Locked DLL, COM registration, services
`e1fx.dll` is copied only when its SHA-256 differs; if `audiodg.exe` holds it, the old file is renamed
(`*.old-<stamp>`, deleted on the next run) and the new one copied. Registration is `regsvr32 /s`
(`DllRegisterServer` writes `HKLM\SOFTWARE\Classes\CLSID\{clsid}\InprocServer32` (ThreadingModel Both) and
`...\AudioEngine\AudioProcessingObjects\{clsid}`), verified afterwards. Audio restart:
`Restart-Service AudioEndpointBuilder -Force` (it caches the endpoint property stores) followed by
starting Audiosrv and every dependent service that was running before.

### 6.8 Re-apply task
`-RegisterReapplyTask` registers `\E1Studio\E1Studio Reapply` (SYSTEM, highest, at startup +30 s and at any
logon, 5 min limit) running `install.ps1 -Reapply` from Program Files. It exits quietly when nothing is
installed or nothing drifted; otherwise it backs up the re-created endpoint (`-reapply` backup), applies
the plan with the options stored in `install.json`, and restarts audio. A run with `-RegisterReapplyTask`
leaves an existing task alone when its action, triggers and principal already match (idempotent).

### 6.9 Uninstall order
Restore endpoints -> restore SBE autostart -> remove task -> restart audio (audiodg drops `e1fx.dll`) ->
`e1panel.exe /exit` (then stop) -> remove the panel's HKCU autostart and the Start menu shortcut ->
`regsvr32 /u` + leftover keys -> delete Program Files folder (files still in use are deleted at reboot via
`MoveFileEx(DELAY_UNTIL_REBOOT)`) -> rename `install.json`. Backups are always kept; `-RemoveSettings` also
deletes `settings.bin` and `HKCU\Software\E1Studio`. With `-KeepFiles`, `install.json` is retired too when
the restore had no errors: going back to Creative, changing Creative settings and installing again then
starts a new installation whose first backup is that newer Creative state (previously the next uninstall
restored the months-old first snapshot of the old installation).

## 7. Testing

| Suite | What it covers |
|---|---|
| `tests\dsp\test_*` | EQ, bass, limiter, params, chain (bit-exactness, latency, crossfade), performance |
| `tests\dsp_spatial\test_spatial` | surround, clarity, dialog+, smart volume (`quick` by default) |
| `tests\dsp_voice\test_voice` | FFT/STFT, noise reduction, VoiceFX |
| `tests\apo` -> `test_apo` | in-process APO host: IPC, COM, format negotiation, bit-exact passthrough, gates, registration (HKLM redirected to HKCU) |
| `tools\tests\test_e1ctl_keys` | key table vs contract coverage, parsing, ranges, formatting, presets, report helpers |
| `tools\tests\test_e1ctl_cli` | `e1ctl.exe` end to end against a private `Local\` section + temp settings file with a fake heartbeat |
| `tools\tests\selfcheck.ps1` | parse + lint of the scripts, pure-function unit tests (plans, restore, snapshot choice, value round trips, wave/spatial blobs, ACL equivalence), live **read-only** endpoint scan and `install.ps1 -WhatIf` / `uninstall.ps1 -WhatIf` runs with a before/after fingerprint of every key, file and task they could touch |

No test changes the system: no registration, no MMDevices writes, no service restarts.

## 8. Diagnostics and known limitations
* `e1ctl find`: E1 endpoints, roles, device format, FX CLSIDs per slot (ours / Creative / other), the
  Enhancements switch, the selected spatial format (undocumented value `{8a845654-...},2`) and whether the
  spatial engine is active (`ISpatialAudioClient` max dynamic objects).
* `e1ctl status [--watch]`: heartbeat (buffers/s), locked format, bypass state, peaks, gain reduction,
  xruns, and the parameters with their source (live block / settings file / defaults).
* `e1ctl` and the panel are both writers of the seqlock. Writers are serialized by the compare-exchange
  claim (§3.2), so concurrent writes are applied one after the other and never mixed. The panel does not
  observe `e1ctl` changes until it re-reads the block.
* The APO is not signed; it loads because `DisableProtectedAudioDG = 1` on this PC (install warns otherwise).
* Not implemented (unknown device protocol): jack retask, battery level (spec §1.6/§1.7, T4/T5).
* Control-panel scope vs `docs/re/control-panel.md` §4 (deliberate):
  * **Set as Windows default** and **Mic Monitoring** (the E1 has no mixer unit, so the original fell back
    to software monitoring): the Voice page has a "Windows settings…" button that opens the recording
    devices (`mmsys.cpl`, Recording tab) — "Set Default" and the Listen tab's "Listen to this device" do
    exactly this inside the audio engine. A panel-owned capture→render loopback stream (a second
    always-running WASAPI client with its own latency, tied to the panel process) is not implemented, and
    nor are the Mixer page's per-input listen levels.
  * **VoiceFX custom presets (Add/Delete)**: not implemented; the 18 stock presets are selectable and a
    profile stores the raw VoiceFX parameters, which covers saving a tweaked voice.
  * **Mic loopback "Test" and per-channel speaker "Test"**: not implemented (they need bundled test
    recordings; Windows' own speaker "Test" in Sound settings works on the E1).
  * **Profile avatar** and **"Audio Device Selection…"**: not implemented (cosmetic / single-device panel).
  * **Language**: title menu "Language…" (Automatic / English / Português), stored in
    `HKCU\Software\E1Studio\Panel\Language`, used from the next start; `/lang:` overrides it.
  * **Effects and RAW mode**: the Advanced page's engine box tooltip explains that apps opening the device
    in RAW mode bypass the effects by design.
  * Scout hot keys must use Ctrl, Alt or Win (Shift only together with one of them) unless the key is
    F1..F24, so a hot key never swallows ordinary typing.
  * The Studio status reads "bit-exact passthrough" only when the render chain really bypasses; with the
    master off it shows "Equalizer active" or "Scout Mode active" when those keep the chain processing.
* `PSScriptAnalyzer` is not installed on the development machine; `selfcheck.ps1` provides the parser and
  lint checks listed above instead.
