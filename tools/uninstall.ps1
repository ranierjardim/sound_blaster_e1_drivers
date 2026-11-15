#Requires -Version 5.1
<#
.SYNOPSIS
    Removes E1 Studio and restores the Sound Blaster E1 endpoints exactly as they were backed up.

.DESCRIPTION
    Must run elevated. -WhatIf runs without elevation and only prints what would change.

    Restore rules
      * Each endpoint's FxProperties is made IDENTICAL to its snapshot: values added since are
        deleted, changed/removed values are written back with their original type and bytes.
      * Default snapshot choice, per endpoint: the oldest backup of the current installation taken
        before E1 Studio was applied to it (the original Creative state), even when later
        "-Reapply" runs created newer backups for re-created endpoints. -Backup <name> restores
        only that backup instead (see -List).
      * Endpoints that still reference E1 Studio but have no snapshot (re-created and re-applied
        without a backup) get the E1 Studio values removed.
      * PKEY_AudioEngine_DeviceFormat is restored only if install.ps1 -SetFormat24 changed it.
      * SBE.exe autostart values removed by -DisableCreativePanel are put back.
      * Nothing in the backup folder is trusted unless Administrators/SYSTEM own the E1Studio
        folder, backup\ and every manifest, and nobody else can write them. Manifest records are
        validated before use: only Render/Capture endpoint ids with property-key value names, and
        only SBE.exe string values of a Run key, are ever written.
      * After a restore without errors (also with -KeepFiles) install.json is retired, so a later
        install starts a new installation whose first backup is the then-current state.
    Then: scheduled task removed, audio services restarted (audiodg releases e1fx.dll), DLL
    unregistered, C:\Program Files\E1Studio deleted (files still in use are deleted at reboot).
    Backups are always kept. settings.bin is kept unless -RemoveSettings.

.PARAMETER Backup
    Name of a backup folder under C:\ProgramData\E1Studio\backup to restore (default: automatic).
.PARAMETER List
    List the available backups and exit.
.PARAMETER KeepFiles
    Restore the endpoints but leave C:\Program Files\E1Studio and the COM registration in place.
.PARAMETER RemoveSettings
    Also delete C:\ProgramData\E1Studio\settings.bin.
.PARAMETER NoRestart
    Do not restart the audio services.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File "C:\Program Files\E1Studio\uninstall.ps1" -WhatIf
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$Backup,
    [switch]$List,
    [switch]$KeepFiles,
    [switch]$RemoveSettings,
    [switch]$NoRestart
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'E1Common.psm1') -Force -DisableNameChecking

$cfg = Get-E1Config
$pk = Get-E1PropertyKeys
$dry = [bool]$WhatIfPreference
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:Errors = 0
$script:RegistryChanged = $false
$script:Planned = 0

function Fail([string]$msg) { Write-E1Log $msg -Level Error; exit 1 }

if (-not [Environment]::Is64BitProcess) { Fail 'run this script in 64-bit Windows PowerShell' }
$isAdmin = Test-E1Admin

# A standard user must not be able to supply the manifests an elevated restore applies.
$trust = $null
try { $trust = Get-E1StateTrustProblem } catch { $trust = $null }   # not elevated: backup ACL unreadable
if ($trust) {
    if (-not $dry -and -not $List) { Fail "$trust - refusing to use the backups or install state there. Investigate before restoring (the folder may have been replaced by a non-administrator)." }
    Write-Warning "$trust - a real uninstall would refuse to use these backups"
}
$manifests = @(Get-E1BackupManifests)
$state = Get-E1InstallState

if ($List) {
    if ($manifests.Count -eq 0) { Write-Host "no backups under $($cfg.BackupRoot)"; exit 0 }
    foreach ($m in $manifests) {
        $eps = (@($m.Endpoints) | ForEach-Object { "$($_.Flow) $($_.Id)$(if ($_.WasApplied) { ' (already E1 Studio)' })" }) -join '; '
        $cur = if ($state -and $m.InstallId -eq $state.InstallId) { '*' } else { ' ' }
        Write-Host ("{0} {1,-24} {2,-8} {3}  {4}" -f $cur, $m.Name, $m.Kind, $m.Created, $eps)
    }
    Write-Host '* = belongs to the current installation'
    exit 0
}

if (-not $isAdmin -and -not $dry) { Fail 'administrator rights are required (or use -WhatIf for a dry run)' }
if (-not $dry) {
    if (Test-E1ReparsePoint -Path $cfg.LogDir) { Fail "$($cfg.LogDir) is a reparse point; refusing to continue" }
    if (Test-Path -LiteralPath $cfg.LogDir) { Set-E1LogFile -Path (Join-Path $cfg.LogDir "uninstall-$stamp.log") }
}
Write-E1Log "E1 Studio uninstaller $($cfg.Version)$(if ($dry) { ' - DRY RUN (-WhatIf): nothing will be changed' })" -Level Step

# ---------------------------------------------------------------------------------------------
# Choose snapshots
# ---------------------------------------------------------------------------------------------
if ($Backup) {
    if (-not ($manifests | Where-Object { $_.Name -eq $Backup })) { Fail "backup '$Backup' not found (use -List)" }
    $pool = $manifests
} elseif ($state) {
    $pool = @($manifests | Where-Object { $_.InstallId -eq $state.InstallId -or ($state.Backups -contains $_.Name) })
} else {
    Write-E1Log 'install.json not found: considering every backup' -Level Warn
    $pool = $manifests
}
$chosen = @(Select-E1Snapshots -Manifests $pool -BackupName $Backup)
if ($chosen.Count -eq 0 -and -not $state) { Write-E1Log 'E1 Studio does not appear to be installed (no install.json, no backup); only leftovers will be removed' -Level Warn }
Write-E1Log "snapshots: $($chosen.Count) endpoint(s) from $(@($chosen | ForEach-Object { $_.Backup } | Sort-Object -Unique) -join ', ')"

# ---------------------------------------------------------------------------------------------
# Restore endpoints
# ---------------------------------------------------------------------------------------------
function Invoke-E1Changes([string]$SubPath, [object[]]$Changes, [string]$Label) {
    if ($Changes.Count -eq 0) { Write-E1Log "$Label : already identical to the backup"; return }
    $script:Planned += $Changes.Count
    $h = $null
    try {
        foreach ($c in $Changes) {
            $desc = if ($c.Action -eq 'Delete') { "Delete value $($c.Name) (current $(Format-E1Record $c.Old))" }
                    else { "Set value $($c.Name) = $(Format-E1Record $c.New) (current $(Format-E1Record $c.Old))" }
            if (-not $PSCmdlet.ShouldProcess("HKLM\$SubPath", $desc)) { continue }
            if (-not $h) { $h = Open-E1KeyForWrite -SubPath $SubPath -CreateIfMissing }
            if ($c.Action -eq 'Delete') { $h.Key.DeleteValue($c.Name, $false) }
            else { $d = ConvertTo-E1RegData -Record $c.New; $h.Key.SetValue($c.Name, $d.Value, $d.Kind) }
            $script:RegistryChanged = $true
        }
        if ($h) { Write-E1Log "$Label : restored ($($Changes.Count) value(s), access via $($h.Method))" }
    } catch {
        $script:Errors++
        Write-E1Log "$Label : restore failed: $($_.Exception.Message)" -Level Error
    } finally { if ($h) { $h.Key.Dispose() } }
}

Write-E1Log 'Endpoint effects' -Level Step
$live = @(Get-E1Endpoints -IncludeNotPresent)
$handled = New-Object System.Collections.Generic.HashSet[string]([StringComparer]::OrdinalIgnoreCase)
foreach ($s in $chosen) {
    $ep = $s.Endpoint
    $bad = Test-E1EndpointRecord -Endpoint $ep
    if ($bad) { $script:Errors++; Write-E1Log "backup $($s.Backup): endpoint record rejected ($bad); not restored" -Level Error; continue }
    $key = "$($ep.Flow)\$($ep.Id)"
    [void]$handled.Add($key)
    $sub = "$($cfg.MMDevRoot)\$($ep.Flow)\$($ep.Id)"
    if (-not (Test-E1Key -SubPath $sub)) {
        Write-E1Log "$key (backup $($s.Backup)): endpoint no longer exists (re-created by Windows); nothing to restore" -Level Warn
        continue
    }
    $cur = @{}
    foreach ($r in @(Get-E1KeyValues -SubPath "$sub\FxProperties")) { $cur[$r.Name] = $r }
    $plan = @(Get-E1RestorePlan -Current $cur -Snapshot @($ep.FxValues))
    if (-not $ep.FxExisted -and $plan.Count -gt 0) { Write-E1Log "$key : FxProperties did not exist before; all its values are removed (the empty key is harmless)" }
    Invoke-E1Changes -SubPath "$sub\FxProperties" -Changes $plan -Label "$key FxProperties (backup $($s.Backup))"

    if ($ep.FormatChanged) {
        $pcur = $null
        foreach ($r in @(Get-E1KeyValues -SubPath "$sub\Properties")) { if ($r.Name -eq $pk.DeviceFormat) { $pcur = $r } }
        $fplan = @()
        if ($ep.DeviceFormat) {
            if (-not (Test-E1RecordEqual $pcur $ep.DeviceFormat)) { $fplan += [pscustomobject]@{ Action = 'Set'; Name = $pk.DeviceFormat; Old = $pcur; New = $ep.DeviceFormat } }
        } elseif ($pcur) {
            $fplan += [pscustomobject]@{ Action = 'Delete'; Name = $pk.DeviceFormat; Old = $pcur; New = $null }
        }
        Invoke-E1Changes -SubPath "$sub\Properties" -Changes $fplan -Label "$key device format"
    }
}
# E1 endpoints that still point at E1 Studio without a snapshot
foreach ($e in $live) {
    if ($handled.Contains("$($e.Flow)\$($e.Id)")) { continue }
    $plan = @()
    foreach ($n in @($e.Fx.Keys)) {
        $v = [string]$e.Fx[$n].Data
        if ($v -ieq $cfg.RenderClsid -or $v -ieq $cfg.CaptureClsid -or ($n -eq $pk.FxName -and $v -eq $cfg.FxDisplayName)) {
            $plan += [pscustomobject]@{ Action = 'Delete'; Name = $n; Old = $e.Fx[$n]; New = $null }
        }
    }
    if ($plan.Count -gt 0) {
        Write-E1Log "$($e.Flow) $($e.Id): uses E1 Studio but has no backup; removing the E1 Studio values (Windows applies no effects there afterwards)" -Level Warn
        Invoke-E1Changes -SubPath $e.FxSubPath -Changes $plan -Label "$($e.Flow)\$($e.Id) FxProperties"
    }
}

# ---------------------------------------------------------------------------------------------
# Creative control panel autostart
# ---------------------------------------------------------------------------------------------
$runs = @()
foreach ($m in ($pool | Sort-Object Created)) {
    if ($Backup -and $m.Name -ne $Backup) { continue }
    foreach ($r in @($m.RunEntries)) { if ($r -and -not ($runs | Where-Object { $_.Root -eq $r.Root -and $_.Name -eq $r.Name })) { $runs += $r } }
}
if ($runs.Count -gt 0) { Write-E1Log 'Creative control panel autostart' -Level Step }
foreach ($r in $runs) {
    $bad = Test-E1RunEntryRecord -Entry $r
    if ($bad) { $script:Errors++; Write-E1Log "autostart record rejected ($bad): $($r.Root)\$($r.SubPath)\$($r.Name)" -Level Error; continue }
    $k = $null
    try {
        $k = Open-E1RunKey -Root $r.Root -SubPath $r.SubPath          # read-only: also works in an unelevated -WhatIf
        $cur = if ($k) { $k.GetValue($r.Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) } else { $null }
        if ($k) { $k.Dispose(); $k = $null }
        if ($cur -eq $r.Value) { Write-E1Log "'$($r.Name)': already present"; continue }
        if ($PSCmdlet.ShouldProcess("$($r.Root)\$($r.SubPath)", "Set value '$($r.Name)' = $($r.Value)")) {
            $k = Open-E1RunKey -Root $r.Root -SubPath $r.SubPath -Writable
            if (-not $k) { throw "cannot open $($r.Root)\$($r.SubPath)" }
            $k.SetValue($r.Name, [string]$r.Value, [Microsoft.Win32.RegistryValueKind]$r.Kind)
            Write-E1Log "'$($r.Name)': restored"
        }
    } catch { $script:Errors++; Write-E1Log "restoring autostart '$($r.Name)' failed: $($_.Exception.Message)" -Level Error }
    finally { if ($k) { $k.Dispose() } }
}

# ---------------------------------------------------------------------------------------------
# Scheduled task
# ---------------------------------------------------------------------------------------------
$task = Get-ScheduledTask -TaskPath $cfg.TaskPath -TaskName $cfg.TaskName -ErrorAction SilentlyContinue
if ($task) {
    Write-E1Log 'Scheduled task' -Level Step
    if ($PSCmdlet.ShouldProcess("$($cfg.TaskPath)$($cfg.TaskName)", 'Unregister scheduled task')) {
        try { Unregister-ScheduledTask -TaskPath $cfg.TaskPath -TaskName $cfg.TaskName -Confirm:$false; Write-E1Log 'removed' }
        catch { $script:Errors++; Write-E1Log "could not remove the task: $($_.Exception.Message)" -Level Error }
    }
}

# ---------------------------------------------------------------------------------------------
# Restart audio so audiodg.exe releases e1fx.dll and loads the restored chain
# ---------------------------------------------------------------------------------------------
if (-not $NoRestart -and ($script:RegistryChanged -or ($dry -and $script:Planned -gt 0))) {
    Write-E1Log 'Audio services' -Level Step
    if ($PSCmdlet.ShouldProcess('AudioEndpointBuilder + Audiosrv', 'Restart (audio stops for a few seconds)')) {
        try { $done = Restart-E1AudioServices; Write-E1Log "restarted: AudioEndpointBuilder, $($done -join ', ')" }
        catch { $script:Errors++; Write-E1Log "audio service restart failed: $($_.Exception.Message)" -Level Error }
    }
}

# ---------------------------------------------------------------------------------------------
# Files and COM registration
# ---------------------------------------------------------------------------------------------
if (-not $KeepFiles) {
    Write-E1Log "Files ($($cfg.InstallDir))" -Level Step
    $panelExe = Join-Path $cfg.InstallDir 'e1panel.exe'
    $running = @(Get-Process -Name 'e1panel' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0 -and (Test-Path -LiteralPath $panelExe)) {
        if ($PSCmdlet.ShouldProcess('e1panel.exe', 'Ask the running control panel to quit (e1panel.exe /exit)')) {
            Start-Process -FilePath $panelExe -ArgumentList '/exit' -WindowStyle Hidden
            foreach ($proc in $running) { try { $proc.WaitForExit(5000) | Out-Null } catch { Write-Verbose "$_" } }
        }
    }
    foreach ($proc in @(Get-Process -Name 'e1panel' -ErrorAction SilentlyContinue)) {
        if ($PSCmdlet.ShouldProcess("e1panel.exe (pid $($proc.Id))", 'Stop')) { try { Stop-Process -Id $proc.Id -Force } catch { Write-E1Log "could not stop e1panel.exe: $($_.Exception.Message)" -Level Warn } }
    }
    # "Start with Windows" of the panel (per user; only the account running the uninstaller)
    $runKey = Open-E1RunKeyForWrite -Root HKCU -SubPath 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
    if ($runKey) {
        try {
            $v = [string]$runKey.GetValue('E1Studio', '')
            if ($v -match '(?i)e1panel\.exe' -and $PSCmdlet.ShouldProcess('HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Run', "Delete value 'E1Studio' = $v")) {
                $runKey.DeleteValue('E1Studio', $false)
            }
        } finally { $runKey.Dispose() }
    }
    $lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\E1 Studio.lnk'
    if ((Test-Path -LiteralPath $lnk) -and $PSCmdlet.ShouldProcess($lnk, 'Delete Start menu shortcut')) {
        Remove-Item -LiteralPath $lnk -Force
    }
    $dll = Join-Path $cfg.InstallDir 'e1fx.dll'
    if (Test-Path -LiteralPath $dll) {
        if ($PSCmdlet.ShouldProcess($dll, 'Unregister COM server + APO (regsvr32 /u /s -> DllUnregisterServer)')) {
            $rs = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\regsvr32.exe') -ArgumentList '/u', '/s', "`"$dll`"" -Wait -PassThru -WindowStyle Hidden
            if ($rs.ExitCode -ne 0) { Write-E1Log "regsvr32 /u returned $($rs.ExitCode)" -Level Warn }
        }
    }
    foreach ($c in @($cfg.RenderClsid, $cfg.CaptureClsid)) {
        $r = Get-E1ApoRegistration -Clsid $c
        foreach ($k in 'Com', 'Apo') {
            if (-not $r.$k) { continue }
            $path = $r."${k}Path"
            if ($PSCmdlet.ShouldProcess("HKLM\$path", 'Delete leftover registration key')) {
                try { $h = Get-E1Hklm; try { $h.DeleteSubKeyTree($path, $false) } finally { $h.Dispose() } }
                catch { Write-E1Log "could not delete HKLM\$path : $($_.Exception.Message)" -Level Warn }
            }
        }
    }
    if (Test-Path -LiteralPath $cfg.InstallDir) {
        if ($PSCmdlet.ShouldProcess($cfg.InstallDir, 'Delete folder and its files')) {
            foreach ($f in @(Get-ChildItem -LiteralPath $cfg.InstallDir -File -Force)) {
                try { Remove-Item -LiteralPath $f.FullName -Force }
                catch {
                    Initialize-E1Native
                    try { [E1Studio.Native]::DeleteAtReboot($f.FullName); Write-E1Log "$($f.Name) is in use; it will be deleted at the next reboot" -Level Warn }
                    catch { $script:Errors++; Write-E1Log "could not delete $($f.FullName): $($_.Exception.Message)" -Level Error }
                }
            }
            if (-not (Get-ChildItem -LiteralPath $cfg.InstallDir -Force)) { Remove-Item -LiteralPath $cfg.InstallDir -Force }
            else { Initialize-E1Native; try { [E1Studio.Native]::DeleteAtReboot($cfg.InstallDir) } catch { Write-Verbose "$_" } }
        }
    }
}

if ($RemoveSettings) {
    if ((Test-Path -LiteralPath $cfg.SettingsFile) -and $PSCmdlet.ShouldProcess($cfg.SettingsFile, 'Delete effect settings')) {
        Remove-Item -LiteralPath $cfg.SettingsFile -Force
    }
    if ((Test-Path -LiteralPath 'HKCU:\Software\E1Studio') -and $PSCmdlet.ShouldProcess('HKCU\Software\E1Studio', 'Delete control panel preferences of the current user')) {
        Remove-Item -LiteralPath 'HKCU:\Software\E1Studio' -Recurse -Force
    }
}

# Retire the installation after a full uninstall, and after any restore without errors (also
# -KeepFiles): a later install.ps1 then starts a new installation whose first backup is the state
# the user has at that time, instead of reusing this installation's (older) original snapshot.
$stateFile = Join-Path $cfg.BackupRoot 'install.json'
if ($state -and (-not $KeepFiles -or $script:Errors -eq 0) -and (Test-Path -LiteralPath $stateFile)) {
    if ($PSCmdlet.ShouldProcess($stateFile, "Rename to install-uninstalled-$stamp.json (backups are kept)")) {
        Rename-Item -LiteralPath $stateFile -NewName "install-uninstalled-$stamp.json"
    }
}

# ---------------------------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------------------------
if (-not $dry) {
    Write-E1Log 'Verification' -Level Step
    foreach ($e in @(Get-E1Endpoints -IncludeNotPresent)) {
        $ours = @($e.Fx.Values | Where-Object { [string]$_.Data -ieq $cfg.RenderClsid -or [string]$_.Data -ieq $cfg.CaptureClsid })
        $mfx = if ($e.Fx.ContainsKey($pk.FxMode)) { [string]$e.Fx[$pk.FxMode].Data } else { '-' }
        if ($ours.Count) { $script:Errors++; Write-E1Log "$($e.Flow) $($e.Id) still references E1 Studio" -Level Error }
        else { Write-E1Log "$($e.Flow) $($e.Id) [$($e.Role)]: MFX = $mfx" -Level Ok }
    }
    if ($script:Errors) { Write-E1Log "uninstall finished with $($script:Errors) error(s); see the log in $($cfg.LogDir)" -Level Error; exit 1 }
    Write-E1Log "E1 Studio removed. Backups kept in $($cfg.BackupRoot)" -Level Ok
} else {
    Write-E1Log 'dry run complete: no changes were made' -Level Ok
}
exit 0
