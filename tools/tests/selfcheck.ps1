#Requires -Version 5.1
<#
    selfcheck.ps1 - Pester-free checks for install.ps1 / uninstall.ps1 / E1Common.psm1.
    Read-only: parses the scripts, unit-tests the pure helpers, reads the live endpoint list and runs
    both scripts with -WhatIf, then proves the registry is unchanged. Never needs elevation.
    Exit code 0 = all checks passed.
#>
[CmdletBinding()]
param([string]$BuildDir, [switch]$SkipLive)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$tools = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $tools
$script:n = 0
$script:fail = 0

function Check([bool]$cond, [string]$msg) {
    $script:n++
    if (-not $cond) { $script:fail++; Write-Host "  FAIL: $msg" -ForegroundColor Red }
}
function Section([string]$s) { Write-Host "[$s]" }

# ---------------------------------------------------------------------------------------------
Section 'parse + lint'
$scripts = @('install.ps1', 'uninstall.ps1', 'E1Common.psm1') | ForEach-Object { Join-Path $tools $_ }
$scripts += $PSCommandPath
$automatic = @('args', 'input', '_', 'psitem', 'matches', 'error', 'host', 'home', 'pid', 'pwd', 'true', 'false', 'null', 'this', 'myinvocation', 'psboundparameters')
foreach ($f in $scripts) {
    $tokens = $null; $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($f, [ref]$tokens, [ref]$errors)
    Check ($errors.Count -eq 0) "$(Split-Path -Leaf $f): parse errors: $($errors | ForEach-Object { "$($_.Extent.StartLineNumber): $($_.Message)" })"
    $assign = $ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true)
    foreach ($a in $assign) {
        $left = $a.Left
        if ($left -is [System.Management.Automation.Language.ConvertExpressionAst]) { $left = $left.Child }
        if ($left -is [System.Management.Automation.Language.VariableExpressionAst] -and $automatic -contains $left.VariablePath.UserPath.ToLowerInvariant()) {
            Check $false "$(Split-Path -Leaf $f):$($a.Extent.StartLineNumber) assigns automatic variable `$$($left.VariablePath.UserPath)"
        }
    }
    # Under -WhatIf, "ForEach-Object Name" (member form) and Get-FileHash silently return nothing.
    $cmds = $ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.CommandAst] }, $true)
    foreach ($c in $cmds) {
        $cn = $c.GetCommandName()
        if ($cn -in 'ForEach-Object', '%', 'Where-Object', '?' -and $c.CommandElements.Count -ge 2 -and
            $c.CommandElements[1] -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
            Check $false "$(Split-Path -Leaf $f):$($c.Extent.StartLineNumber) uses '$cn <member>' (affected by -WhatIf); use a script block"
        }
        if ($cn -eq 'Get-FileHash') { Check $false "$(Split-Path -Leaf $f):$($c.Extent.StartLineNumber) uses Get-FileHash (returns nothing under -WhatIf)" }
    }
    # every Set/Delete on the registry or file system in the scripts must sit under ShouldProcess
    if ((Split-Path -Leaf $f) -in 'install.ps1', 'uninstall.ps1') {
        $calls = $ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and $a.Member.Extent.Text -in 'SetValue', 'DeleteValue', 'DeleteSubKeyTree', 'SetAccessControl' }, $true)
        foreach ($c in $calls) {
            $p = $c.Parent; $guarded = $false
            while ($p) {
                if ($p -is [System.Management.Automation.Language.IfStatementAst] -and $p.Clauses[0].Item1.Extent.Text -match 'ShouldProcess') { $guarded = $true; break }
                if ($p -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $p.Name -in 'Invoke-E1Changes', 'Invoke-E1Rollback') { $guarded = $true; break }
                $p = $p.Parent
            }
            # log-folder creation in install.ps1 happens only when not -WhatIf (guarded by "if (-not $dry)")
            if (-not $guarded) {
                $p = $c.Parent
                while ($p) { if ($p -is [System.Management.Automation.Language.IfStatementAst] -and $p.Clauses[0].Item1.Extent.Text -match '-not \$dry') { $guarded = $true; break }; $p = $p.Parent }
            }
            Check $guarded "$(Split-Path -Leaf $f):$($c.Extent.StartLineNumber) '$($c.Member.Extent.Text)' is not guarded by ShouldProcess"
        }
    }
}

# ---------------------------------------------------------------------------------------------
Section 'module + contract constants'
Import-Module (Join-Path $tools 'E1Common.psm1') -Force -DisableNameChecking
$cfg = Get-E1Config
$pk = Get-E1PropertyKeys
$g = Get-E1Guids
$guidsH = Get-Content -Raw (Join-Path $root 'include\e1fx_guids.h')
$ipcH = Get-Content -Raw (Join-Path $root 'include\e1fx_ipc.h')
Check ($guidsH -match 'E1FX_RENDER_CLSID_STR\s+L"(\{[^"]+\})"' -and $Matches[1] -eq $cfg.RenderClsid) 'render CLSID matches include/e1fx_guids.h'
Check ($guidsH -match 'E1FX_CAPTURE_CLSID_STR\s+L"(\{[^"]+\})"' -and $Matches[1] -eq $cfg.CaptureClsid) 'capture CLSID matches include/e1fx_guids.h'
Check ($ipcH -match 'E1FX_SETTINGS_DIR\s+L"([^"]+)"' -and ($Matches[1] -replace '\\\\', '\') -eq $cfg.DataDir) 'data dir matches E1FX_SETTINGS_DIR'
Check ($ipcH -match 'E1FX_SETTINGS_PATH\s+L"([^"]+)"' -and ($Matches[1] -replace '\\\\', '\') -eq $cfg.SettingsFile) 'settings path matches E1FX_SETTINGS_PATH'

# ---------------------------------------------------------------------------------------------
Section 'value records'
$rt = @(
    (ConvertTo-E1ValueRecord -Name 'a' -Kind 'DWord' -Value ([int]-1)),
    (ConvertTo-E1ValueRecord -Name 'b' -Kind 'Binary' -Value ([byte[]](0x0b, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0))),
    (ConvertTo-E1ValueRecord -Name 'c' -Kind 'MultiString' -Value ([string[]]('{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}', 'x'))),
    (ConvertTo-E1ValueRecord -Name 'd' -Kind 'String' -Value '{41528545-B167-495e-BB36-E11A17CF64B3}'),
    (ConvertTo-E1ValueRecord -Name 'e' -Kind 'QWord' -Value ([int64]-5)),
    (ConvertTo-E1ValueRecord -Name 'f' -Kind 'ExpandString' -Value '%SystemRoot%\x')
)
Check ($rt[0].Data -eq 4294967295) 'DWORD 0xFFFFFFFF stored unsigned'
$d0 = ConvertTo-E1RegData -Record $rt[0]
Check ($d0.Value -eq -1 -and $d0.Kind -eq 'DWord') 'DWORD round-trips to Int32 -1'
foreach ($r in $rt) {
    $json = ($r | ConvertTo-Json -Depth 4) | ConvertFrom-Json
    $back = ConvertTo-E1RegData -Record $json
    $again = ConvertTo-E1ValueRecord -Name $r.Name -Kind $r.Kind -Value $back.Value
    Check (Test-E1RecordEqual $r $again) "record $($r.Kind) survives JSON + SetValue conversion"
}
Check (-not (Test-E1RecordEqual $rt[3] (ConvertTo-E1ValueRecord -Name 'd' -Kind 'String' -Value '{41528545-b167-495e-bb36-e11a17cf64b3}'))) 'record compare is case-sensitive (exact restore)'
Check ((ConvertTo-E1ValueRecord -Name 'm' -Kind 'MultiString' -Value ([string[]]@('one'))).Data -is [array]) 'single-item MultiString stays an array'

# ---------------------------------------------------------------------------------------------
Section 'FX plan'
function Rec($name, $kind, $value) { ConvertTo-E1ValueRecord -Name $name -Kind $kind -Value $value }
function Apply-Plan([hashtable]$values, [object[]]$plan) {
    $v = $values.Clone()
    foreach ($c in $plan) { if ($c.Action -eq 'Delete') { $v.Remove($c.Name) } else { $v[$c.Name] = $c.New } }
    return $v
}
# the live Creative render endpoint (2026-11-14)
$creative = @{}
foreach ($r in @(
    (Rec $pk.FxName 'String' 'Creative System Effect'),
    (Rec $pk.FxAssociation 'String' $g.NodeSpeaker),
    (Rec $pk.FxPreMix 'String' '{559493DD-3D1F-40a7-8E94-95762EAD6E95}'),
    (Rec $pk.FxPostMix 'String' '{2EF9BA5B-FA37-4195-95C5-9483B6FE2104}'),
    (Rec $pk.FxStream 'String' '{E9B73398-425E-4c41-B4C9-F4D1E6922A27}'),
    (Rec $pk.FxMode 'String' '{41528545-B167-495e-BB36-E11A17CF64B3}'),
    (Rec '{d3993a3f-99c2-4402-b5ec-a92a0367664b},5' 'MultiString' ([string[]]@($g.ModeDefault))),
    (Rec $pk.MfxModes 'MultiString' ([string[]]@($g.ModeDefault))),
    (Rec '{5b4777a4-8ad4-4d34-893a-df34da0e56ca},0' 'Binary' ([byte[]](0x0b, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0))),
    (Rec '{59fbb5c6-4c98-4741-adc0-40abb474a963},0' 'DWord' 2)
)) { $creative[$r.Name] = $r }
$plan = @(Get-E1FxPlan -Role render -Values $creative -JackSubType $g.NodeSpeaker)
$names = @($plan | ForEach-Object { "$($_.Action) $($_.Name)" })
Check ($names -contains "Delete $($pk.FxPreMix)") 'plan removes LFX'
Check ($names -contains "Delete $($pk.FxPostMix)") 'plan removes GFX'
Check ($names -contains "Delete $($pk.FxStream)") 'plan removes SFX'
Check ($names -contains "Set $($pk.FxMode)") 'plan sets MFX'
Check ($names -contains "Set $($pk.FxName)") 'plan renames the effect'
Check (-not ($names -match [regex]::Escape($pk.FxAssociation))) 'existing association kept'
Check (-not ($names -match [regex]::Escape($pk.MfxModes))) 'existing DEFAULT mode kept'
Check (-not ($names -match '5b4777a4|59fbb5c6')) "Creative's private parameter values are left alone"
Check ($plan.Count -eq 5) "plan has 5 changes (got $($plan.Count))"
$mfx = $plan | Where-Object { $_.Name -eq $pk.FxMode }
Check ($mfx.New.Data -eq $cfg.RenderClsid -and $mfx.New.Kind -eq 'String') 'MFX = render CLSID as REG_SZ'
$applied = Apply-Plan $creative $plan
Check (@(Get-E1FxPlan -Role render -Values $applied -JackSubType $g.NodeSpeaker).Count -eq 0) 'plan is idempotent (second run: no change)'
$micPlan = @(Get-E1FxPlan -Role mic -Values @{} -JackSubType $g.NodeMicrophone)
Check (($micPlan | Where-Object { $_.Name -eq $pk.FxMode }).New.Data -eq $cfg.CaptureClsid) 'mic gets the capture CLSID'
Check (($micPlan | Where-Object { $_.Name -eq $pk.FxAssociation }).New.Data -eq $g.NodeMicrophone) 'missing association = jack subtype'
Check ((@(($micPlan | Where-Object { $_.Name -eq $pk.MfxModes }).New.Data)) -contains $g.ModeDefault) 'missing MFX modes get DEFAULT'
$weird = @{ ($pk.MfxModes) = (Rec $pk.MfxModes 'MultiString' ([string[]]@('{4780004E-7133-41D8-8C74-660DADD2C0EE}'))) }
$wp = @(Get-E1FxPlan -Role render -Values $weird -JackSubType '')
$newModes = @(($wp | Where-Object { $_.Name -eq $pk.MfxModes }).New.Data)
Check ($newModes.Count -eq 2 -and $newModes -contains $g.ModeDefault) 'existing modes preserved, DEFAULT appended'
Check ((($wp | Where-Object { $_.Name -eq $pk.FxAssociation }).New.Data) -eq $g.NodeAny) 'no jack subtype -> KSNODETYPE_ANY'

Section 'restore plan'
$snap = @($creative.Values)
$undo = @(Get-E1RestorePlan -Current $applied -Snapshot $snap)
$restored = Apply-Plan $applied $undo
Check ($restored.Count -eq $creative.Count) 'restore gives the same number of values'
$same = $true
foreach ($k in $creative.Keys) { if (-not (Test-E1RecordEqual $creative[$k] $restored[$k])) { $same = $false } }
Check $same 'restore reproduces every original value exactly'
$extra = $applied.Clone(); $extra['{new},1'] = Rec '{new},1' 'String' 'x'
Check (@(Get-E1RestorePlan -Current $extra -Snapshot $snap | Where-Object { $_.Action -eq 'Delete' -and $_.Name -eq '{new},1' }).Count -eq 1) 'values added after the backup are deleted'
Check (@(Get-E1RestorePlan -Current $creative -Snapshot $snap).Count -eq 0) 'restore of an untouched endpoint is a no-op'
Check (@(Get-E1RestorePlan -Current $applied -Snapshot @()).Count -eq $applied.Count) 'empty snapshot deletes everything'

Section 'snapshot selection'
$epX0 = [pscustomobject]@{ Flow = 'Render'; Id = '{x}'; WasApplied = $false; FxValues = @() }
$epY0 = [pscustomobject]@{ Flow = 'Capture'; Id = '{y}'; WasApplied = $false; FxValues = @() }
$epX1 = [pscustomobject]@{ Flow = 'Render'; Id = '{x}'; WasApplied = $true; FxValues = @() }
$epZ1 = [pscustomobject]@{ Flow = 'Render'; Id = '{z}'; WasApplied = $false; FxValues = @() }
$mA = [pscustomobject]@{ Name = '20261114-210000'; Created = '2026-11-14T21:00:00'; Endpoints = @($epX0, $epY0) }
$mB = [pscustomobject]@{ Name = '20261120-080000-reapply'; Created = '2026-11-20T08:00:00'; Endpoints = @($epX1, $epZ1) }
$sel = @(Select-E1Snapshots -Manifests @($mB, $mA))
Check ($sel.Count -eq 3) 'one snapshot per endpoint'
Check ((($sel | Where-Object { $_.Endpoint.Id -eq '{x}' }).Backup) -eq $mA.Name) 'oldest pre-install snapshot wins'
Check ((($sel | Where-Object { $_.Endpoint.Id -eq '{z}' }).Backup) -eq $mB.Name) 're-created endpoint restored from the reapply backup'
$mC = [pscustomobject]@{ Name = 'c'; Created = '2026-11-01T00:00:00'; Endpoints = @($epX1) }
$sel2 = @(Select-E1Snapshots -Manifests @($mC, $mA))
Check ((($sel2 | Where-Object { $_.Endpoint.Id -eq '{x}' }).Backup) -eq $mA.Name) 'an "already applied" snapshot never beats a pre-install one'
$sel3 = @(Select-E1Snapshots -Manifests @($mA, $mB) -BackupName $mB.Name)
Check ($sel3.Count -eq 2 -and -not ($sel3 | Where-Object { $_.Backup -ne $mB.Name })) '-Backup restricts to one backup'

Section 'endpoint roles'
Check ((Get-E1EndpointRole -Flow Render -JackSubType '' -Association $null -Name 'x') -eq 'render') 'render'
Check ((Get-E1EndpointRole -Flow Capture -JackSubType $g.NodeMicrophone -Association $null -Name 'x') -eq 'mic') 'mic by jack subtype'
Check ((Get-E1EndpointRole -Flow Capture -JackSubType $g.NodeDigital -Association $null -Name 'Microphone') -eq 'loopback') 'What U Hear by jack subtype'
Check ((Get-E1EndpointRole -Flow Capture -JackSubType '' -Association $g.NodeMicrophone -Name 'x') -eq 'mic') 'mic by FX association'
Check ((Get-E1EndpointRole -Flow Capture -JackSubType '' -Association $null -Name 'What U Hear (Sound Blaster E1)') -eq 'loopback') 'What U Hear by name'
Check ((Get-E1EndpointRole -Flow Capture -JackSubType '' -Association $null -Name 'Line') -eq 'unknown') 'unknown capture left alone'

Section 'wave format'
function HexBytes([string]$h) { [byte[]]($h -split ' ' | ForEach-Object { [Convert]::ToByte($_, 16) }) }
$liveRender = HexBytes '41 00 00 00 01 00 00 00 fe ff 02 00 44 ac 00 00 98 09 04 00 06 00 18 00 16 00 18 00 03 00 00 00 01 00 00 00 00 00 10 00 80 00 00 aa 00 38 9b 71'
$liveMic = HexBytes '41 00 00 00 01 00 00 00 fe ff 01 00 44 ac 00 00 88 58 01 00 02 00 10 00 16 00 10 00 04 00 00 00 01 00 00 00 00 00 10 00 80 00 00 aa 00 38 9b 71'
$ri = Get-E1WaveFormatInfo -Blob $liveRender
$mi = Get-E1WaveFormatInfo -Blob $liveMic
Check ($ri.Is44k24Stereo) "live render format decodes as 44.1k/24/2 ($($ri.Text))"
Check (-not $mi.Is44k24Stereo -and $mi.Bits -eq 16 -and $mi.Channels -eq 1 -and $mi.Mask -eq 4) "mic decodes as 16-bit mono FC ($($mi.Text))"
$gen = New-E1WaveFormat24Blob -Existing $liveMic
Check ($gen.Length -eq 48) 'generated blob is header + WAVEFORMATEXTENSIBLE (48 bytes)'
Check ([BitConverter]::ToString($gen) -eq [BitConverter]::ToString($liveRender)) 'generated 24-bit blob is byte-identical to what the Creative INF/engine wrote'
$hdr = HexBytes '41 00 7c 74 28 00 00 00'
Check ([BitConverter]::ToString((New-E1WaveFormat24Blob -Existing ([byte[]]($hdr + (New-Object byte[] 40)))), 0, 8) -eq [BitConverter]::ToString($hdr)) 'existing 8-byte header preserved'
Check ([BitConverter]::ToString((New-E1WaveFormat24Blob -Existing $null), 0, 8) -eq '41-00-00-00-01-00-00-00') 'default header when no value exists'
Check ($null -eq (Get-E1WaveFormatInfo -Blob ([byte[]](1, 2, 3)))) 'short blob -> $null'

Section 'spatial format'
$sp = HexBytes '41 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 38 ac 59 14 75 38 bf 49 bb 59 0f e8 0f 4d 39 5d'
Check ((Get-E1SpatialFormatName -Blob $sp) -eq 'Dolby Atmos for Headphones') 'Dolby Atmos for Headphones decoded'
$off = [byte[]]($sp[0..15] + (New-Object byte[] 16))
Check ((Get-E1SpatialFormatName -Blob $off) -eq 'off') 'zero GUID = off'

Section 'folder ACLs'
$dataSd = (New-E1DirectorySecurity -Kind data).GetSecurityDescriptorSddlForm('Access')
$adminSd = (New-E1DirectorySecurity -Kind admin).GetSecurityDescriptorSddlForm('Access')
$logSd = (New-E1DirectorySecurity -Kind log).GetSecurityDescriptorSddlForm('Access')
Check ($dataSd -like 'D:P*') 'data ACL does not inherit'
Check ($dataSd -match '\(A;OIIO;0x1301bf;;;BU\)') "data ACL: Users Modify on files only ($dataSd)"
Check ($dataSd -match '\(A;;0x1200ab;;;BU\)') "data ACL: Users list + add files on the folder itself ($dataSd)"
$usersOnFolder = 0
foreach ($m in [regex]::Matches($dataSd, '\(A;([A-Z]*);(0x[0-9a-fA-F]+|[A-Z]+);;;BU\)')) {
    if ($m.Groups[1].Value -notmatch 'IO') { $usersOnFolder = $usersOnFolder -bor [Convert]::ToInt32($m.Groups[2].Value, 16) }
}
Check (($usersOnFolder -band 0x10000) -eq 0 -and ($usersOnFolder -band 0x4) -eq 0 -and ($usersOnFolder -band 0x40000) -eq 0) "data ACL: Users cannot delete/rename the folder, create subfolders or change its ACL (0x$('{0:X}' -f $usersOnFolder))"
Check ((New-E1DirectorySecurity -Kind data).GetOwner([Security.Principal.SecurityIdentifier]).Value -eq 'S-1-5-32-544') 'data ACL: owner Administrators'
Check ($dataSd -match '\(A;OICI;0x1200a9;;;LS\)') 'data ACL: LOCAL SERVICE Read'
Check ($dataSd -match '\(A;OICI;FA;;;SY\)' -and $dataSd -match '\(A;OICI;FA;;;BA\)') 'data ACL: SYSTEM + Administrators full'
Check ($adminSd -notmatch ';BU\)' -and $adminSd -notmatch ';LS\)') "backup ACL: administrators only ($adminSd)"
Check ($logSd -match '\(A;OICI;0x1200a9;;;BU\)' -and $logSd -notmatch '0x1301bf') 'log ACL: Users read only'
# apply the DACL to a scratch folder (owner unchanged) and read it back: the installer's
# "ACL already correct" comparison must hold after a real apply, or every run would re-ACL.
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("e1selfcheck-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $ds = New-Object Security.AccessControl.DirectorySecurity
    $ds.SetSecurityDescriptorSddlForm($dataSd, 'Access')
    (Get-Item -LiteralPath $tmp).SetAccessControl($ds)
    $back = (Get-Item -LiteralPath $tmp).GetAccessControl('Access').GetSecurityDescriptorSddlForm('Access')
    Check (Test-E1DaclEquivalent $back $dataSd) "applied ACL reads back equivalent ($back)"
    Check (-not (Test-E1DaclEquivalent $back $logSd)) 'different ACLs are not equivalent'
    Check (-not (Test-E1DaclEquivalent ($dataSd -replace '^D:P[A-Z]*', 'D:AI') $dataSd)) 'an inheriting ACL is not equivalent to a protected one'
} finally {
    $reset = New-Object Security.AccessControl.DirectorySecurity
    $reset.SetAccessRuleProtection($false, $false)
    try { (Get-Item -LiteralPath $tmp).SetAccessControl($reset) } catch { Write-Verbose "$_" }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Section 'trust of the data folder (owner + ACL)'
$tmp2 = Join-Path ([IO.Path]::GetTempPath()) ("e1trust-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp2 | Out-Null
try {
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $isAdminNow = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $ownerNow = (Get-Item -LiteralPath $tmp2).GetAccessControl('Owner').GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($ownerNow -eq $me) {
        Check ([bool](Get-E1TrustProblem -Path $tmp2 -Kind admin -OwnerOnly)) 'a folder owned by a standard user is not trusted (owner check)'
        $f = Join-Path $tmp2 'install.json'
        Set-Content -LiteralPath $f -Value '{}' -Encoding UTF8
        Check ([bool](Get-E1TrustProblem -Path $f -Kind file)) 'a state file owned by a standard user is not trusted'
    }
    # exact admin DACL applied by the current (non-admin) owner: still rejected (the owner keeps WRITE_DAC)
    $ds = New-Object Security.AccessControl.DirectorySecurity
    $ds.SetSecurityDescriptorSddlForm((New-E1DirectorySecurity -Kind admin).GetSecurityDescriptorSddlForm('Access'), 'Access')
    if (-not $isAdminNow) {
        Check (-not (Test-E1SecureDirectory -Path $tmp2 -Kind admin)) 'Test-E1SecureDirectory: correct DACL but non-admin owner is NOT "correct"'
    }
} finally {
    Remove-Item -LiteralPath $tmp2 -Recurse -Force -ErrorAction SilentlyContinue
}

Section 'manifest record validation'
$runOk = [pscustomobject]@{ Root = 'HKLM32'; SubPath = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run'; Name = 'Sound Blaster E-Series Control Panel'; Kind = 'String'; Value = '"C:\Program Files (x86)\Creative\Sound Blaster E-Series\Sound Blaster E-Series Control Panel\SBE.exe" /r' }
Check ($null -eq (Test-E1RunEntryRecord -Entry $runOk)) 'genuine SBE.exe Run entry accepted'
$poc1 = [pscustomobject]@{ Root = 'HKLM64'; SubPath = 'SYSTEM\CurrentControlSet\Services\Audiosrv'; Name = 'ImagePath'; Kind = 'ExpandString'; Value = 'C:\evil.exe' }
$poc2 = [pscustomobject]@{ Root = 'HKCU'; SubPath = 'Environment'; Name = 'E1_POC_MARKER'; Kind = 'String'; Value = 'written-by-uninstall' }
$poc3 = [pscustomobject]@{ Root = 'HKLM64'; SubPath = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run'; Name = 'x'; Kind = 'String'; Value = 'C:\evil.exe' }
$poc4 = [pscustomobject]@{ Root = 'HKLM'; SubPath = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run'; Name = 'x'; Kind = 'Binary'; Value = 'C:\SBE.exe' }
Check ([bool](Test-E1RunEntryRecord -Entry $poc1)) 'Run record pointing at a service ImagePath rejected'
Check ([bool](Test-E1RunEntryRecord -Entry $poc2)) 'Run record pointing at HKCU\Environment rejected'
Check ([bool](Test-E1RunEntryRecord -Entry $poc3)) 'Run record that does not start SBE.exe rejected'
Check ([bool](Test-E1RunEntryRecord -Entry $poc4)) 'Run record with a foreign root/kind rejected'
$epOk = [pscustomobject]@{ Flow = 'Render'; Id = '{7e65a67a-04d8-403f-b379-885cdec445d5}'; FxValues = @((ConvertTo-E1ValueRecord -Name '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6' -Kind 'String' -Value 'x')) }
Check ($null -eq (Test-E1EndpointRecord -Endpoint $epOk)) 'genuine endpoint record accepted'
$epBad1 = [pscustomobject]@{ Flow = 'Render\..\..\..\Services'; Id = '{7e65a67a-04d8-403f-b379-885cdec445d5}'; FxValues = @() }
$epBad2 = [pscustomobject]@{ Flow = 'Render'; Id = '{x}\..\..'; FxValues = @() }
$epBad3 = [pscustomobject]@{ Flow = 'Capture'; Id = '{7e65a67a-04d8-403f-b379-885cdec445d5}'; FxValues = @((ConvertTo-E1ValueRecord -Name 'Debugger' -Kind 'String' -Value 'x')) }
Check ([bool](Test-E1EndpointRecord -Endpoint $epBad1)) 'endpoint record with a path in Flow rejected'
Check ([bool](Test-E1EndpointRecord -Endpoint $epBad2)) 'endpoint record with a path in Id rejected'
Check ([bool](Test-E1EndpointRecord -Endpoint $epBad3)) 'endpoint record with a non-property-key value name rejected'

Section 'native helper compiles'
try { Initialize-E1Native; Check ([bool]('E1Studio.Native' -as [type])) 'E1Studio.Native type loaded' }
catch { Check $false "Add-Type of the native helper failed: $_" }

# ---------------------------------------------------------------------------------------------
if (-not $SkipLive) {
    Section 'live system (read-only)'
    $eps = @(Get-E1Endpoints -IncludeNotPresent)
    foreach ($e in $eps) { Write-Host ("  {0,-7} {1} {2,-10} {3,-8} managed={4} fx={5} spatial={6}" -f $e.Flow, $e.Id, $e.StateName, $e.Role, $e.Managed, $e.Fx.Count, $e.SpatialFormat) }
    $e1present = [bool]($eps | Where-Object { $_.Role -eq 'render' -and $_.StateName -eq 'active' })
    if (-not $e1present) {
        Write-Host '  E1 not connected: live checks skipped'
    } else {
        Check (@($eps | Where-Object { $_.Role -eq 'loopback' -and $_.Managed }).Count -eq 0) 'What U Hear is never managed'
        Check (@($eps | Where-Object { $_.Role -eq 'render' -and $_.Managed }).Count -ge 1) 'render endpoint found'

        # snapshot everything the scripts could touch, run both with -WhatIf, compare
        function Get-Fingerprint {
            $sb = New-Object Text.StringBuilder
            foreach ($e in @(Get-E1Endpoints -IncludeNotPresent)) {
                foreach ($sub in $e.FxSubPath, $e.PropSubPath) {
                    foreach ($r in @(Get-E1KeyValues -SubPath $sub)) { [void]$sb.AppendLine("$sub|$($r.Name)|$($r.Kind)|$(@($r.Data) -join ',')") }
                }
            }
            foreach ($r in @(Get-E1CreativePanelRunEntries)) { [void]$sb.AppendLine("run|$($r.Root)|$($r.Name)|$($r.Value)") }
            foreach ($p in $cfg.DataDir, $cfg.InstallDir, $cfg.BackupRoot) { [void]$sb.AppendLine("path|$p|$(Test-Path -LiteralPath $p)") }
            [void]$sb.AppendLine("task|$([bool](Get-ScheduledTask -TaskPath $cfg.TaskPath -TaskName $cfg.TaskName -ErrorAction SilentlyContinue))")
            foreach ($c in $cfg.RenderClsid, $cfg.CaptureClsid) { $r = Get-E1ApoRegistration -Clsid $c; [void]$sb.AppendLine("clsid|$c|$($r.Com)|$($r.Apo)") }
            return $sb.ToString()
        }
        $before = Get-Fingerprint
        $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $srcArgs = @()
        if ($BuildDir) { $srcArgs = @('-SourceDir', $BuildDir) }

        Write-Host '  install.ps1 -WhatIf -DisableCreativePanel -SetFormat24 -RegisterReapplyTask'
        $out = & $ps -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $tools 'install.ps1') -WhatIf -DisableCreativePanel -SetFormat24 -RegisterReapplyTask @srcArgs 2>&1 | Out-String
        $rc = $LASTEXITCODE
        $out -split "`r?`n" | ForEach-Object { Write-Host "    | $_" }
        Check ($rc -eq 0) "install.ps1 -WhatIf exit code 0 (got $rc)"
        Check ($out -match 'dry run complete: no changes were made') 'install dry run completed'
        $alreadyInstalled = [bool]($eps | Where-Object { $_.Role -eq 'render' -and $_.Fx.ContainsKey($pk.FxMode) -and [string]$_.Fx[$pk.FxMode].Data -eq $cfg.RenderClsid })
        if (-not $alreadyInstalled) {
            Check ($out -match [regex]::Escape("Set value $($pk.FxMode) = String $($cfg.RenderClsid)")) 'dry run lists the MFX change'
            Check ($out -match 'What if: Performing the operation "Create backup') 'dry run lists the backup'
            Check ($out -match 'What if: .*Restart') 'dry run lists the audio restart'
        }

        Write-Host '  uninstall.ps1 -WhatIf'
        $out2 = & $ps -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $tools 'uninstall.ps1') -WhatIf 2>&1 | Out-String
        $rc2 = $LASTEXITCODE
        $out2 -split "`r?`n" | ForEach-Object { Write-Host "    | $_" }
        Check ($rc2 -eq 0) "uninstall.ps1 -WhatIf exit code 0 (got $rc2)"
        $out3 = & $ps -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $tools 'uninstall.ps1') -List 2>&1 | Out-String
        Check ($LASTEXITCODE -eq 0) 'uninstall.ps1 -List exit code 0'

        # Only when NOT elevated: proves a real run refuses without admin (elevated, it would install).
        if (-not (Test-E1Admin)) {
            $out4 = & $ps -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $tools 'install.ps1') 2>&1 | Out-String
            Check ($LASTEXITCODE -eq 1 -and $out4 -match 'administrator rights are required') 'install.ps1 without -WhatIf refuses to run unelevated'
            $out5 = & $ps -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $tools 'uninstall.ps1') 2>&1 | Out-String
            Check ($LASTEXITCODE -eq 1 -and $out5 -match 'administrator rights are required') 'uninstall.ps1 without -WhatIf refuses to run unelevated'
        }
        $after = Get-Fingerprint
        Check ($before -eq $after) 'registry, files and task are unchanged after the dry runs'
    }
}

Write-Host ("selfcheck: {0} checks, {1} failed" -f $script:n, $script:fail)
if ($script:fail) { exit 1 }
exit 0
