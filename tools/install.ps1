#Requires -Version 5.1
<#
.SYNOPSIS
    Installs E1 Studio (APO + control panel + CLI) on the Sound Blaster E1 endpoints.

.DESCRIPTION
    Must run elevated (64-bit Windows PowerShell). With -WhatIf it runs without elevation as a pure
    dry run: it reads the system and prints every change it would make, and changes nothing.

    Steps
      1. Locate the E1 render and microphone endpoints (MMDevices, device instance id
         USB\VID_041E&PID_323B). "What U Hear" is never touched.
      2. Back up (reg export + JSON manifest) every endpoint FxProperties/Properties key that will
         change, the registrations of the CLSIDs referenced there and any autostart value that will
         be removed -> C:\ProgramData\E1Studio\backup\<timestamp>\ (administrators only).
      3. Copy e1fx.dll (+ e1panel.exe, e1ctl.exe, scripts) to C:\Program Files\E1Studio and
         register the DLL (regsvr32 /s -> DllRegisterServer).
      4. FxProperties: MFX (,6) = E1 Studio CLSID; Creative LFX/GFX/SFX (,1 ,2 ,5) and any EFX/UI/
         composite entries removed; association and DEFAULT processing mode ensured.
      5. C:\ProgramData\E1Studio (owner Administrators): Users may add and modify files there but
         not rename/delete the folder or create subfolders; LOCAL SERVICE Read; backup\ is
         administrators-only, logs\ Users-read. Folders are created atomically with these ACLs, and
         install.json/backups are only trusted when administrators own them. Default settings.bin
         if absent.
      6. Options: -DisableCreativePanel, -SetFormat24, -RegisterReapplyTask.
      7. Restart AudioEndpointBuilder/Audiosrv and verify.

    Idempotent: a second run changes nothing (no new backup, no restart) unless something drifted.

.PARAMETER SourceDir
    Folder with the built binaries. Default: ..\build\x64\Release relative to this script, or the
    script's own folder when it runs from C:\Program Files\E1Studio.
.PARAMETER Reapply
    Silent maintenance mode used by the scheduled task: re-applies the FX registration on E1
    endpoints that were re-created (Storage Sense, driver reinstall) and lost it. Never copies files.
.PARAMETER IncludeNotPresent
    Also configure stale endpoints in the NOTPRESENT state.
.PARAMETER SkipCapture
    Leave the microphone endpoint alone (render only).
.PARAMETER NoRestart
    Do not restart the audio services (changes take effect after the next restart/reboot).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\install.ps1 -WhatIf
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\install.ps1 -DisableCreativePanel -RegisterReapplyTask
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$SourceDir,
    [switch]$DisableCreativePanel,
    [switch]$SetFormat24,
    [switch]$RegisterReapplyTask,
    [switch]$Reapply,
    [switch]$IncludeNotPresent,
    [switch]$SkipCapture,
    [switch]$NoRestart
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'E1Common.psm1') -Force -DisableNameChecking

$cfg = Get-E1Config
$pk = Get-E1PropertyKeys
$dry = [bool]$WhatIfPreference
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:Changed = $false        # any system change performed (or, in -WhatIf, planned)
$script:RegistryChanged = $false
$script:DllChanged = $false
$script:PendingManifest = $null
$script:CopyPlanned = $false

function Fail([string]$msg) { Write-E1Log $msg -Level Error; exit 1 }

# ---------------------------------------------------------------------------------------------
# 0. Preconditions
# ---------------------------------------------------------------------------------------------
if (-not [Environment]::Is64BitProcess) {
    Fail 'run this script in 64-bit Windows PowerShell (C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe)'
}
$isAdmin = Test-E1Admin
if (-not $isAdmin -and -not $dry) {
    Fail 'administrator rights are required. Right-click PowerShell > "Run as administrator", or use -WhatIf for a dry run.'
}
if ($Reapply) { $ConfirmPreference = 'None' }

$script:EarlyNotes = @()
if (-not $dry) {
    # Nothing under C:\ProgramData\E1Studio is used (log written or rotated, install.json or a
    # backup read) before it is proven to be under administrator control: a standard user can
    # create folders in C:\ProgramData, and with the ACL of earlier versions could rename this one.
    foreach ($p in @($cfg.DataDir, $cfg.BackupRoot, $cfg.LogDir)) {
        if (Test-E1ReparsePoint -Path $p) { Fail "$p is a reparse point (junction/symlink); refusing to continue" }
    }
    $rootProblem = Get-E1TrustProblem -Path $cfg.DataDir -Kind data -OwnerOnly
    if ($rootProblem) {
        if ($Reapply) { Fail "$rootProblem; refusing to reapply (run install.ps1 as administrator to repair)" }
        foreach ($c in @($cfg.BackupRoot, $cfg.LogDir)) {
            if (Test-Path -LiteralPath $c) {
                Fail "$rootProblem, so $c inside it cannot be trusted. Move $($cfg.DataDir) away (keep settings.bin if you want it) and run the installer again."
            }
        }
    }
    foreach ($c in @(@{ Path = $cfg.BackupRoot; Kind = 'admin' }, @{ Path = $cfg.LogDir; Kind = 'log' })) {
        $prob = Get-E1TrustProblem -Path $c.Path -Kind $c.Kind
        if ($prob) { Fail "$prob; refusing to trust it. It was not created by this installer: remove it and run again." }
    }
    # Root and log folder get their final owner + ACL (created atomically when missing) before
    # anything is written.
    foreach ($c in @(@{ Path = $cfg.DataDir; Kind = 'data' }, @{ Path = $cfg.LogDir; Kind = 'log' })) {
        $res = Set-E1SecureDirectory -Path $c.Path -Kind $c.Kind
        if ($res -ne 'ok') { $script:Changed = $true; $script:EarlyNotes += "$($c.Path): $res (owner Administrators, $($c.Kind) ACL)" }
    }
    Set-E1LogFile -Path (Join-Path $cfg.LogDir ("{0}-{1}.log" -f $(if ($Reapply) { 'reapply' } else { 'install' }), $stamp))
    # keep the 30 most recent logs
    Get-ChildItem -LiteralPath $cfg.LogDir -Filter '*.log' -File | Sort-Object LastWriteTime -Descending |
        Select-Object -Skip 30 | Remove-Item -Force -WhatIf:$false -ErrorAction SilentlyContinue
}

$quiet = [bool]$Reapply
Write-E1Log "E1 Studio installer $($cfg.Version)$(if ($Reapply) { ' (reapply)' })$(if ($dry) { ' - DRY RUN (-WhatIf): nothing will be changed' })" -Level Step -Quiet:$quiet
if ($dry -and -not $isAdmin) { Write-E1Log 'not elevated: read-only inspection; a real install needs administrator rights' -Quiet:$quiet }
foreach ($n in $script:EarlyNotes) { Write-E1Log $n -Quiet:$quiet }
if ($dry) {
    try { $tp = Get-E1StateTrustProblem } catch { $tp = $null }   # not elevated: backup ACL unreadable
    if ($tp) { Write-E1Log "$tp - a real install would refuse to continue" -Level Warn }
}

$state = Get-E1InstallState
if ($Reapply) {
    if (-not $state) { Write-E1Log 'E1 Studio is not installed (no install.json); nothing to reapply' -Quiet:$quiet; exit 0 }
    if (-not (Test-Path -LiteralPath (Join-Path $cfg.InstallDir 'e1fx.dll'))) { Write-E1Log 'e1fx.dll missing from the install folder; nothing to reapply' -Level Warn; exit 0 }
    if ($state.PSObject.Properties['SetFormat24'] -and $state.SetFormat24) { $SetFormat24 = $true }
    if ($state.PSObject.Properties['SkipCapture'] -and $state.SkipCapture) { $SkipCapture = $true }
}

# Source binaries
if (-not $Reapply) {
    if (-not $SourceDir) {
        $candidate = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\x64\Release'
        $SourceDir = if (Test-Path -LiteralPath (Join-Path $candidate 'e1fx.dll')) { $candidate } else { $PSScriptRoot }
    }
    $SourceDir = [IO.Path]::GetFullPath($SourceDir)
    $srcDll = Join-Path $SourceDir 'e1fx.dll'
    if (-not (Test-Path -LiteralPath $srcDll)) {
        if ($dry) { Write-E1Log "e1fx.dll not found in $SourceDir (run build.cmd first); continuing the dry run" -Level Warn }
        else { Fail "e1fx.dll not found in $SourceDir - run build.cmd first or pass -SourceDir" }
    } else {
        $machine = Get-E1PeMachine -Path $srcDll
        if ($machine -ne 0x8664) { Fail ("{0} is not an x64 DLL (machine 0x{1:X})" -f $srcDll, $machine) }
    }
}

# ---------------------------------------------------------------------------------------------
# 1. Locate endpoints
# ---------------------------------------------------------------------------------------------
Write-E1Log 'Locating Sound Blaster E1 endpoints (USB\VID_041E&PID_323B)' -Level Step -Quiet:$quiet
$all = @(Get-E1Endpoints -IncludeNotPresent:$IncludeNotPresent)
foreach ($e in $all) {
    $what = if ($e.Managed -and -not ($SkipCapture -and $e.Role -eq 'mic')) { 'configure' } else { 'leave alone' }
    $mfx = if ($e.Fx.ContainsKey($pk.FxMode)) { [string]$e.Fx[$pk.FxMode].Data } else { '-' }
    Write-E1Log ("{0,-7} {1} {2,-10} {3,-8} MFX={4}  -> {5}  [{6}]" -f $e.Flow, $e.Id, $e.StateName, $e.Role, $mfx, $what, $e.Name) -Quiet:$quiet
}
$targets = @($all | Where-Object { $_.Managed -and -not ($SkipCapture -and $_.Role -eq 'mic') })
if (-not ($targets | Where-Object { $_.Role -eq 'render' })) {
    if ($Reapply) { Write-E1Log 'no E1 render endpoint present; nothing to do' -Quiet:$quiet; exit 0 }
    Fail 'no Sound Blaster E1 render endpoint found. Plug the E1 in (and check Device Manager) and run again.'
}

foreach ($e in $targets) {
    if ($e.DisableSysFx) { Write-E1Log "$($e.Flow) $($e.Id): 'Audio enhancements' is OFF for this endpoint - Windows will not load E1 Studio there until it is turned back on (Settings > Sound > device > Audio enhancements = Device default effects)" -Level Warn }
    if ($e.Role -eq 'render' -and $e.SpatialFormat -and $e.SpatialFormat -ne 'off') {
        Write-E1Log "$($e.Flow) $($e.Id): spatial sound '$($e.SpatialFormat)' is enabled. It virtualises the signal before E1 Studio; turn it off for bit-exact output (Settings > Sound > Speakers (Sound Blaster E1) > Spatial audio > Off)" -Level Warn -Quiet:$quiet
    }
}
$pad = Get-E1ProtectedAudioDg
if ($null -ne $pad -and [int]$pad -ne 1) {
    Write-E1Log 'DisableProtectedAudioDG is not 1: a self-signed APO may be refused by a protected audiodg.exe' -Level Warn
}

# ---------------------------------------------------------------------------------------------
# Plan
# ---------------------------------------------------------------------------------------------
$plans = @()
foreach ($e in $targets) {
    $fxPlan = @(Get-E1FxPlan -Role $e.Role -Values $e.Fx -JackSubType $e.JackSubType)
    $fmtPlan = $null
    if ($SetFormat24 -and $e.Role -eq 'render') {
        $info = Get-E1WaveFormatInfo -Blob $e.DeviceFormat
        if (-not $info -or -not $info.Is44k24Stereo) {
            $old = if ($e.DeviceFormat) { ConvertTo-E1ValueRecord -Name $pk.DeviceFormat -Kind 'Binary' -Value $e.DeviceFormat } else { $null }
            $new = ConvertTo-E1ValueRecord -Name $pk.DeviceFormat -Kind 'Binary' -Value (New-E1WaveFormat24Blob -Existing $e.DeviceFormat)
            $fmtPlan = [pscustomobject]@{ Old = $old; New = $new; OldText = $(if ($info) { $info.Text } else { '<absent>' }) }
        }
    }
    $plans += [pscustomobject]@{ Endpoint = $e; Fx = $fxPlan; Format = $fmtPlan; WasApplied = ($fxPlan.Count -eq 0) }
}
$runEntries = @()
if ($DisableCreativePanel) { $runEntries = @(Get-E1CreativePanelRunEntries) }
$regWork = @($plans | Where-Object { $_.Fx.Count -gt 0 -or $_.Format })
$needBackup = ($regWork.Count -gt 0) -or ($runEntries.Count -gt 0)

Write-E1Log 'Planned registry changes' -Level Step -Quiet:$quiet
if (-not $needBackup) { Write-E1Log 'none: every E1 endpoint already runs E1 Studio' -Quiet:$quiet }
foreach ($p in $regWork) {
    foreach ($c in $p.Fx) {
        Write-E1Log ("{0} {1}\FxProperties  {2} {3}: {4} -> {5}  ({6})" -f $p.Endpoint.Flow, $p.Endpoint.Id, $c.Action, $c.Name, (Format-E1Record $c.Old), (Format-E1Record $c.New), $c.Reason) -Quiet:$quiet
    }
    if ($p.Format) { Write-E1Log ("{0} {1}\Properties  Set DeviceFormat: {2} -> 44100 Hz, 24-bit PCM, 2 ch" -f $p.Endpoint.Flow, $p.Endpoint.Id, $p.Format.OldText) -Quiet:$quiet }
}
foreach ($r in $runEntries) { Write-E1Log "Run key $($r.Root)\$($r.SubPath)  Delete '$($r.Name)' = $($r.Value)" -Quiet:$quiet }

# Access check: can these keys be written with the rights we hold?
if ($isAdmin) {
    foreach ($p in $regWork) {
        foreach ($sub in @($p.Endpoint.FxSubPath) + $(if ($p.Format) { @($p.Endpoint.PropSubPath) } else { @() })) {
            $k = $null
            try { $k = Open-E1Key -SubPath $sub -Writable } catch { $k = $null }
            if ($k) { $k.Dispose(); Write-E1Log "access: HKLM\$sub writable with the Administrators ACE (QueryValues+SetValue); ACL left untouched" -Quiet:$quiet }
            else { Write-E1Log "access: HKLM\$sub not writable through its ACL; will use SeBackup/SeRestore privileges (REG_OPTION_BACKUP_RESTORE), ACL left untouched" -Level Warn }
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 5a. Data folders (needed by the backup) - created with their final ACLs
# ---------------------------------------------------------------------------------------------
Write-E1Log 'Data folders' -Level Step -Quiet:$quiet
foreach ($d in @(@{ Path = $cfg.DataDir; Kind = 'data' }, @{ Path = $cfg.BackupRoot; Kind = 'admin' }, @{ Path = $cfg.LogDir; Kind = 'log' })) {
    if (Test-E1ReparsePoint -Path $d.Path) { Fail "$($d.Path) is a reparse point (junction/symlink); refusing to continue" }
    $exists = Test-Path -LiteralPath $d.Path
    $same = $false
    if ($exists) { try { $same = Test-E1SecureDirectory -Path $d.Path -Kind $d.Kind } catch { $same = $false } }   # owner + DACL
    if ($exists -and $same) { Write-E1Log "$($d.Path): exists, owner and ACL correct" -Quiet:$quiet; continue }
    $acl = (New-E1DirectorySecurity -Kind $d.Kind).GetSecurityDescriptorSddlForm('Owner, Access')
    if ($PSCmdlet.ShouldProcess($d.Path, "$(if ($exists) { 'Reset owner + ACL of' } else { 'Create folder (atomically, with its ACL)' }) ($($d.Kind)): $acl")) {
        $res = Set-E1SecureDirectory -Path $d.Path -Kind $d.Kind
        Write-E1Log "$($d.Path): $res [$acl]" -Quiet:$quiet
        $script:Changed = $true
    }
}

# ---------------------------------------------------------------------------------------------
# 2. Backup
# ---------------------------------------------------------------------------------------------
$backupName = $null
if ($needBackup) {
    Write-E1Log 'Backup' -Level Step -Quiet:$quiet
    $backupName = $stamp + $(if ($Reapply) { '-reapply' } else { '' })
    $bdir = Join-Path $cfg.BackupRoot $backupName
    if ($PSCmdlet.ShouldProcess($bdir, 'Create backup (reg export of endpoint FxProperties/Properties, CLSID registrations, Run values + manifest.json)')) {
        New-Item -ItemType Directory -Path $bdir -Force | Out-Null
        $reg = Join-Path $env:SystemRoot 'System32\reg.exe'
        $exports = New-Object System.Collections.Generic.List[object]
        $export = {
            param([string]$hkPath, [string]$file)
            & $reg export $hkPath (Join-Path $bdir $file) /y /reg:64 2>&1 | Out-Null
            $ok = ($LASTEXITCODE -eq 0)
            $exports.Add([pscustomobject]@{ Key = $hkPath; File = $file; Ok = $ok })
            if (-not $ok) { Write-E1Log "reg export failed for $hkPath" -Level Warn }
        }
        $epRecords = @()
        $clsids = New-Object System.Collections.Generic.HashSet[string]([StringComparer]::OrdinalIgnoreCase)
        foreach ($p in $regWork) {
            $e = $p.Endpoint
            $tag = "$($e.Flow)-$($e.Id.Trim('{}'))"
            if ($e.FxExists) { & $export "HKLM\$($e.FxSubPath)" "$tag-FxProperties.reg" }
            & $export "HKLM\$($e.PropSubPath)" "$tag-Properties.reg"
            foreach ($slot in 'FxPreMix', 'FxPostMix', 'FxUi', 'FxStream', 'FxMode', 'FxEndpoint') {
                $n = $pk[$slot]
                if ($e.Fx.ContainsKey($n) -and [string]$e.Fx[$n].Data -match '^\{[0-9A-Fa-f-]{36}\}$') { [void]$clsids.Add([string]$e.Fx[$n].Data) }
            }
            $epRecords += [pscustomobject][ordered]@{
                Flow          = $e.Flow
                Id            = $e.Id
                Role          = $e.Role
                Name          = $e.Name
                InstanceId    = $e.InstanceId
                State         = $e.StateName
                WasApplied    = $p.WasApplied
                FxExisted     = $e.FxExists
                FxValues      = @(Get-E1KeyValues -SubPath $e.FxSubPath)
                FxSddl        = Get-E1KeySddl -SubPath $e.FxSubPath
                FormatChanged = [bool]$p.Format
                DeviceFormat  = $(if ($p.Format) { $p.Format.Old } else { $null })
            }
        }
        foreach ($c in $clsids) {
            if ($c -ieq $cfg.RenderClsid -or $c -ieq $cfg.CaptureClsid) { continue }
            $r = Get-E1ApoRegistration -Clsid $c
            if ($r.Com) { & $export "HKLM\$($r.ComPath)" "CLSID-$($c.Trim('{}')).reg" }
            if ($r.Com32) { & $export "HKLM\$($r.Com32Path)" "CLSID32-$($c.Trim('{}')).reg" }
            if ($r.Apo) { & $export "HKLM\$($r.ApoPath)" "APO-$($c.Trim('{}')).reg" }
        }
        $manifest = [ordered]@{
            Format      = 1
            Name        = $backupName
            Created     = (Get-Date).ToString('o')
            Kind        = $(if ($Reapply) { 'reapply' } else { 'install' })
            Version     = $cfg.Version
            Computer    = $env:COMPUTERNAME
            User        = [Security.Principal.WindowsIdentity]::GetCurrent().Name
            InstallId   = $(if ($state) { $state.InstallId } else { $null })
            Endpoints   = $epRecords
            RunEntries  = @($runEntries)
            ForeignClsids = @($clsids)
            RegExports  = $exports.ToArray()
        }
        $script:PendingManifest = $manifest
        $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $bdir 'manifest.json') -Encoding UTF8
        Write-E1Log "backup written to $bdir ($($exports.Count) .reg files)" -Quiet:$quiet
    }
}

# ---------------------------------------------------------------------------------------------
# 3. Files + COM registration
# ---------------------------------------------------------------------------------------------
if (-not $Reapply) {
    Write-E1Log "Files -> $($cfg.InstallDir)" -Level Step
    if (-not (Test-Path -LiteralPath $cfg.InstallDir)) {
        if ($PSCmdlet.ShouldProcess($cfg.InstallDir, 'Create folder (inherits the Program Files ACL: administrators write, users read)')) {
            New-Item -ItemType Directory -Path $cfg.InstallDir -Force | Out-Null
            $script:Changed = $true
        }
    } else {
        foreach ($old in @(Get-ChildItem -LiteralPath $cfg.InstallDir -Filter '*.old-*' -File -ErrorAction SilentlyContinue)) {
            if ($PSCmdlet.ShouldProcess($old.FullName, 'Delete leftover from a previous update')) {
                try { Remove-Item -LiteralPath $old.FullName -Force } catch { Write-E1Log "still in use, kept: $($old.Name)" }
            }
        }
    }
    $copies = @()
    foreach ($f in $cfg.InstallFiles) { $copies += [pscustomobject]@{ Src = Join-Path $SourceDir $f; Name = $f; Required = ($f -eq 'e1fx.dll') } }
    foreach ($f in $cfg.ScriptFiles) { $copies += [pscustomobject]@{ Src = Join-Path $PSScriptRoot $f; Name = $f; Required = $true } }
    $readme = Join-Path (Split-Path -Parent $PSScriptRoot) 'README.md'
    if (Test-Path -LiteralPath $readme) { $copies += [pscustomobject]@{ Src = $readme; Name = 'README.md'; Required = $false } }
    foreach ($c in $copies) {
        $dst = Join-Path $cfg.InstallDir $c.Name
        if (-not (Test-Path -LiteralPath $c.Src)) {
            if ($c.Required -and -not $dry) { Fail "missing $($c.Src)" }
            if ($c.Required -or $c.Name -notlike '*.pdb') { Write-E1Log "not built, skipped: $($c.Name)" -Level $(if ($c.Required) { 'Warn' } else { 'Info' }) }
            continue
        }
        if ([IO.Path]::GetFullPath($c.Src) -ieq [IO.Path]::GetFullPath($dst)) { continue }   # running from the install folder
        $hs = Get-E1Sha256 -Path $c.Src
        $hd = Get-E1Sha256 -Path $dst
        if ($hs -eq $hd) { Write-E1Log "$($c.Name): up to date" ; continue }
        if ($c.Name -eq 'e1fx.dll') { $script:CopyPlanned = $true }
        if ($PSCmdlet.ShouldProcess($dst, "Copy $($c.Src) (sha256 $($hs.Substring(0,16))...)")) {
            if ($hd) {
                try { Copy-Item -LiteralPath $c.Src -Destination $dst -Force }
                catch {
                    # A loaded DLL cannot be overwritten but can be renamed; the old copy is deleted next time.
                    $aside = "$dst.old-$stamp"
                    Rename-Item -LiteralPath $dst -NewName (Split-Path -Leaf $aside) -Force
                    Copy-Item -LiteralPath $c.Src -Destination $dst -Force
                    Write-E1Log "$($c.Name) was in use; old copy renamed to $(Split-Path -Leaf $aside)"
                }
            } else {
                Copy-Item -LiteralPath $c.Src -Destination $dst -Force
            }
            if ((Get-E1Sha256 -Path $dst) -ne $hs) { Fail "copy verification failed for $dst" }
            Write-E1Log "$($c.Name): installed"
            $script:Changed = $true
            if ($c.Name -eq 'e1fx.dll') { $script:DllChanged = $true }
        }
    }

    # Start menu shortcut for the panel (all users)
    $panel = Join-Path $cfg.InstallDir 'e1panel.exe'
    $lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\E1 Studio.lnk'
    if ((Test-Path -LiteralPath (Join-Path $SourceDir 'e1panel.exe')) -and -not (Test-Path -LiteralPath $lnk)) {
        if ($PSCmdlet.ShouldProcess($lnk, "Create Start menu shortcut to $panel")) {
            $sh = New-Object -ComObject WScript.Shell
            try {
                $s = $sh.CreateShortcut($lnk)
                $s.TargetPath = $panel
                $s.WorkingDirectory = $cfg.InstallDir
                $s.Description = 'E1 Studio - Sound Blaster E1 control panel'
                $s.Save()
            } finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($sh) }
            $script:Changed = $true
        }
    }

    $dstDll = Join-Path $cfg.InstallDir 'e1fx.dll'
    $regR = Get-E1ApoRegistration -Clsid $cfg.RenderClsid
    $regC = Get-E1ApoRegistration -Clsid $cfg.CaptureClsid
    $registered = $regR.Com -and $regR.Apo -and $regC.Com -and $regC.Apo -and ($regR.InprocServer -ieq $dstDll) -and ($regC.InprocServer -ieq $dstDll)
    if ($registered -and -not $script:DllChanged) {
        Write-E1Log 'COM/APO registration: up to date'
    } elseif ($PSCmdlet.ShouldProcess($dstDll, 'Register COM server + APO (regsvr32 /s -> DllRegisterServer: HKCR\CLSID and HKCR\AudioEngine\AudioProcessingObjects)')) {
        $rs = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\regsvr32.exe') -ArgumentList '/s', "`"$dstDll`"" -Wait -PassThru -WindowStyle Hidden
        if ($rs.ExitCode -ne 0) { Fail "regsvr32 failed with exit code $($rs.ExitCode) (DllRegisterServer)" }
        foreach ($c in @($cfg.RenderClsid, $cfg.CaptureClsid)) {
            $r = Get-E1ApoRegistration -Clsid $c
            if (-not $r.Com) { Fail "DllRegisterServer did not create HKCR\CLSID\$c" }
            if (-not $r.Apo) { Fail "DllRegisterServer did not create HKCR\AudioEngine\AudioProcessingObjects\$c" }
            if ($r.InprocServer -ine $dstDll) { Write-E1Log "CLSID $c InprocServer32 = '$($r.InprocServer)' (expected $dstDll)" -Level Warn }
        }
        Write-E1Log 'COM/APO registration: done'
        $script:Changed = $true
        $script:DllChanged = $true
    }
} else {
    $regR = Get-E1ApoRegistration -Clsid $cfg.RenderClsid
    if (-not ($regR.Com -and $regR.Apo)) { Write-E1Log 'E1 Studio APO is not registered (run install.ps1 again as administrator)' -Level Error; exit 1 }
}

# ---------------------------------------------------------------------------------------------
# 4. FxProperties (+ optional device format)
# ---------------------------------------------------------------------------------------------
function Invoke-E1Rollback {
    # Puts every endpoint captured in this run's backup back to its snapshot (used when step 4 fails).
    if (-not $script:PendingManifest) { return }
    foreach ($ep in @($script:PendingManifest.Endpoints)) {
        try {
            $sub = "$($cfg.MMDevRoot)\$($ep.Flow)\$($ep.Id)\FxProperties"
            $cur = @{}
            foreach ($r in @(Get-E1KeyValues -SubPath $sub)) { $cur[$r.Name] = $r }
            $undo = @(Get-E1RestorePlan -Current $cur -Snapshot @($ep.FxValues))
            if ($undo.Count -eq 0) { continue }
            $h = Open-E1KeyForWrite -SubPath $sub
            try {
                foreach ($c in $undo) {
                    if ($c.Action -eq 'Delete') { $h.Key.DeleteValue($c.Name, $false) }
                    else { $d = ConvertTo-E1RegData -Record $c.New; $h.Key.SetValue($c.Name, $d.Value, $d.Kind) }
                }
            } finally { $h.Key.Dispose() }
            if ($ep.FormatChanged -and $ep.DeviceFormat) {
                $h = Open-E1KeyForWrite -SubPath "$($cfg.MMDevRoot)\$($ep.Flow)\$($ep.Id)\Properties"
                try { $d = ConvertTo-E1RegData -Record $ep.DeviceFormat; $h.Key.SetValue($pk.DeviceFormat, $d.Value, $d.Kind) } finally { $h.Key.Dispose() }
            }
            Write-E1Log "rolled back $($ep.Flow) $($ep.Id)" -Level Warn
        } catch { Write-E1Log "rollback of $($ep.Flow) $($ep.Id) failed: $($_.Exception.Message) - run uninstall.ps1" -Level Error }
    }
}

if ($regWork.Count -gt 0) { Write-E1Log 'Endpoint effects' -Level Step -Quiet:$quiet }
try {
foreach ($p in $regWork) {
    $e = $p.Endpoint
    if ($p.Fx.Count -gt 0) {
        $h = $null
        foreach ($c in $p.Fx) {
            $desc = if ($c.Action -eq 'Delete') { "Delete value $($c.Name) (was $(Format-E1Record $c.Old))" }
                    else { "Set value $($c.Name) = $(Format-E1Record $c.New) (was $(Format-E1Record $c.Old))" }
            if ($PSCmdlet.ShouldProcess("HKLM\$($e.FxSubPath)", $desc)) {
                if (-not $h) {
                    $h = Open-E1KeyForWrite -SubPath $e.FxSubPath -CreateIfMissing
                    Write-E1Log "$($e.Flow) $($e.Id): FxProperties opened via $($h.Method)" -Quiet:$quiet
                }
                if ($c.Action -eq 'Delete') { $h.Key.DeleteValue($c.Name, $false) }
                else { $d = ConvertTo-E1RegData -Record $c.New; $h.Key.SetValue($c.Name, $d.Value, $d.Kind) }
                $script:Changed = $true
                $script:RegistryChanged = $true
            }
        }
        if ($h) { $h.Key.Dispose() }
    }
    if ($p.Format) {
        if ($PSCmdlet.ShouldProcess("HKLM\$($e.PropSubPath)", "Set value $($pk.DeviceFormat) (PKEY_AudioEngine_DeviceFormat) = 44100 Hz / 24-bit PCM / stereo (was $($p.Format.OldText))")) {
            $h = Open-E1KeyForWrite -SubPath $e.PropSubPath
            try { $d = ConvertTo-E1RegData -Record $p.Format.New; $h.Key.SetValue($pk.DeviceFormat, $d.Value, $d.Kind) } finally { $h.Key.Dispose() }
            $script:Changed = $true
            $script:RegistryChanged = $true
        }
    }
}
} catch {
    Write-E1Log "writing the endpoint registry failed: $($_.Exception.Message)" -Level Error
    Invoke-E1Rollback
    exit 1
}

# ---------------------------------------------------------------------------------------------
# 5b. settings.bin (only if absent; never from the SYSTEM reapply task)
# ---------------------------------------------------------------------------------------------
if (-not $Reapply) {
    $ctl = Join-Path $cfg.InstallDir 'e1ctl.exe'
    if (Test-Path -LiteralPath $cfg.SettingsFile) {
        Write-E1Log "settings.bin present; kept (user settings are never overwritten)"
    } elseif ($PSCmdlet.ShouldProcess($cfg.SettingsFile, 'Write default settings (all effects off = bit-exact) via e1ctl init-settings')) {
        if (Test-Path -LiteralPath $ctl) {
            & $ctl init-settings | ForEach-Object { Write-E1Log $_ }
            if ($LASTEXITCODE -ne 0) { Write-E1Log 'e1ctl init-settings failed; the APO falls back to built-in defaults' -Level Warn }
            $script:Changed = $true
        } else {
            Write-E1Log 'e1ctl.exe not installed; the APO uses built-in defaults until the panel saves settings' -Level Warn
        }
    }
}

# ---------------------------------------------------------------------------------------------
# 6. Options
# ---------------------------------------------------------------------------------------------
if ($DisableCreativePanel) {
    Write-E1Log 'Creative control panel autostart' -Level Step
    if ($runEntries.Count -eq 0) { Write-E1Log 'no SBE.exe autostart value found' }
    foreach ($r in $runEntries) {
        if ($PSCmdlet.ShouldProcess("$($r.Root)\$($r.SubPath)", "Delete value '$($r.Name)' = $($r.Value) (backed up in the manifest)")) {
            $k = Open-E1RunKeyForWrite -Root $r.Root -SubPath $r.SubPath
            try { $k.DeleteValue($r.Name, $false) } finally { $k.Dispose() }
            $script:Changed = $true
        }
    }
    foreach ($proc in @(Get-Process -Name 'SBE' -ErrorAction SilentlyContinue)) {
        if ($PSCmdlet.ShouldProcess("SBE.exe (pid $($proc.Id))", 'Stop the running Creative control panel')) {
            try { Stop-Process -Id $proc.Id -Force } catch { Write-E1Log "could not stop SBE.exe pid $($proc.Id): $($_.Exception.Message)" -Level Warn }
        }
    }
}

if ($RegisterReapplyTask) {
    Write-E1Log 'Scheduled re-apply task' -Level Step
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $taskArgs = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$(Join-Path $cfg.InstallDir 'install.ps1')`" -Reapply"
    $taskSame = $false
    $existing = Get-ScheduledTask -TaskPath $cfg.TaskPath -TaskName $cfg.TaskName -ErrorAction SilentlyContinue
    if ($existing) {
        $acts = @($existing.Actions)
        $trs = @($existing.Triggers)
        $boot = @($trs | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskBootTrigger' })
        $logon = @($trs | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskLogonTrigger' })
        $taskSame = ($acts.Count -eq 1) -and ([string]$acts[0].Execute -ieq $ps) -and ([string]$acts[0].Arguments -ceq $taskArgs) -and
                    ($trs.Count -eq 2) -and ($boot.Count -eq 1) -and ([string]$boot[0].Delay -eq 'PT30S') -and
                    ($logon.Count -eq 1) -and (-not [string]$logon[0].UserId) -and
                    ([string]$existing.Principal.UserId -in @('SYSTEM', 'NT AUTHORITY\SYSTEM', 'S-1-5-18')) -and
                    ([string]$existing.Principal.RunLevel -eq 'Highest')
    }
    if ($taskSame) {
        Write-E1Log 'task already registered with the same action, triggers and principal'
    } elseif ($PSCmdlet.ShouldProcess("Task Scheduler $($cfg.TaskPath)$($cfg.TaskName)", "Register task as SYSTEM, triggers: at startup (+30 s) and at any logon; action: $ps $taskArgs")) {
        $action = New-ScheduledTaskAction -Execute $ps -Argument $taskArgs
        $t1 = New-ScheduledTaskTrigger -AtStartup
        $t1.Delay = 'PT30S'
        $t2 = New-ScheduledTaskTrigger -AtLogOn
        $principal = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -LogonType ServiceAccount -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
        Register-ScheduledTask -TaskPath $cfg.TaskPath -TaskName $cfg.TaskName -Action $action -Trigger @($t1, $t2) -Principal $principal -Settings $settings -Description 'E1 Studio: re-applies the APO registration on Sound Blaster E1 endpoints that Windows re-created.' -Force | Out-Null
        Write-E1Log 'task registered'
        $script:Changed = $true
    }
}

# ---------------------------------------------------------------------------------------------
# Install state
# ---------------------------------------------------------------------------------------------
if (-not $dry -and ($script:Changed -or -not $state)) {
    $now = (Get-Date).ToString('o')
    $backups = @()
    if ($state -and $state.PSObject.Properties['Backups']) { $backups = @($state.Backups) }
    if ($backupName) { $backups += $backupName }
    $newState = [ordered]@{
        Format       = 1
        InstallId    = $(if ($state) { $state.InstallId } else { [guid]::NewGuid().ToString() })
        Version      = $cfg.Version
        InstalledAt  = $(if ($state) { $state.InstalledAt } else { $now })
        UpdatedAt    = $now
        InstallDir   = $cfg.InstallDir
        Backups      = $backups
        SetFormat24  = [bool]($SetFormat24 -or ($state -and $state.PSObject.Properties['SetFormat24'] -and $state.SetFormat24))
        SkipCapture  = [bool]$SkipCapture
        ReapplyTask  = [bool]($RegisterReapplyTask -or ($state -and $state.PSObject.Properties['ReapplyTask'] -and $state.ReapplyTask))
        DisabledCreativePanel = [bool]($DisableCreativePanel -or ($state -and $state.PSObject.Properties['DisabledCreativePanel'] -and $state.DisabledCreativePanel))
    }
    # The first backup was written before install.json existed: stamp it with the new id.
    if (-not $state -and $backupName -and $script:PendingManifest) {
        $mf = Join-Path (Join-Path $cfg.BackupRoot $backupName) 'manifest.json'
        $script:PendingManifest.InstallId = $newState.InstallId
        $script:PendingManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $mf -Encoding UTF8
    }
    $newState | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $cfg.BackupRoot 'install.json') -Encoding UTF8
}

# ---------------------------------------------------------------------------------------------
# 7. Restart audio + verify
# ---------------------------------------------------------------------------------------------
$needRestart = $script:RegistryChanged -or $script:DllChanged -or ($dry -and (($regWork.Count -gt 0) -or $script:CopyPlanned))
if ($needRestart -and -not $NoRestart) {
    Write-E1Log 'Audio services' -Level Step -Quiet:$quiet
    $deps = @((Get-Service AudioEndpointBuilder).DependentServices | Where-Object { $_.Status -eq 'Running' } | ForEach-Object { $_.Name })
    if ($PSCmdlet.ShouldProcess('AudioEndpointBuilder + Audiosrv', "Restart (dependents restarted afterwards: $($deps -join ', ')) - audio stops for a few seconds")) {
        $done = Restart-E1AudioServices
        Write-E1Log "restarted: AudioEndpointBuilder, $($done -join ', ')" -Quiet:$quiet
    }
} elseif ($needRestart) {
    Write-E1Log 'audio services NOT restarted (-NoRestart): changes apply after the next restart of the audio service or a reboot' -Level Warn
}

if (-not $dry) {
    Write-E1Log 'Verification' -Level Step -Quiet:$quiet
    $ok = $true
    foreach ($e in @(Get-E1Endpoints -IncludeNotPresent:$IncludeNotPresent | Where-Object { $_.Managed -and -not ($SkipCapture -and $_.Role -eq 'mic') })) {
        $left = @(Get-E1FxPlan -Role $e.Role -Values $e.Fx -JackSubType $e.JackSubType)
        if ($left.Count -eq 0) { Write-E1Log "$($e.Flow) $($e.Id): MFX = $([string]$e.Fx[$pk.FxMode].Data) (E1 Studio), no other effects" -Level Ok -Quiet:$quiet }
        else { $ok = $false; Write-E1Log "$($e.Flow) $($e.Id): $($left.Count) value(s) still differ: $(($left | ForEach-Object { $_.Name }) -join ', ')" -Level Error }
    }
    $r = Get-E1ApoRegistration -Clsid $cfg.RenderClsid
    if (-not ($r.Com -and $r.Apo)) { $ok = $false; Write-E1Log 'render APO CLSID not registered' -Level Error }
    $ctl = Join-Path $cfg.InstallDir 'e1ctl.exe'
    if (-not $Reapply -and (Test-Path -LiteralPath $ctl)) {
        Write-E1Log 'e1ctl status (the APO creates its shared block when the first stream starts on the E1):'
        & $ctl status 2>&1 | Select-Object -First 30 | ForEach-Object { Write-E1Log "  $_" }
    }
    if (-not $ok) { Write-E1Log 'installation finished with errors; uninstall.ps1 restores the backup' -Level Error; exit 1 }
    if ($script:Changed) { Write-E1Log "E1 Studio installed$(if ($backupName) { "; backup: $(Join-Path $cfg.BackupRoot $backupName)" })" -Level Ok -Quiet:$quiet }
    else { Write-E1Log 'nothing to do: E1 Studio is already installed and applied' -Level Ok -Quiet:$quiet }
} else {
    Write-E1Log 'dry run complete: no changes were made' -Level Ok
}
exit 0
