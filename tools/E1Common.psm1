#Requires -Version 5.1
<#
    E1Common.psm1 - shared helpers for install.ps1 / uninstall.ps1 (E1 Studio).

    Design rule: functions in this module only READ the system or compute plans. Every change is
    made by the calling script inside $PSCmdlet.ShouldProcess(), so -WhatIf lists every change and
    performs none. (Module functions do not inherit the caller's $WhatIfPreference, which is why
    mutations are not hidden in here.)

    Registry access notes (verified on Windows 11 26300, 2026-11-14 and 2026-11-15, read-only):
      MMDevices\Audio\{Render|Capture}\{id}, \Properties and \FxProperties are owned by SYSTEM and
      inherit one DACL:
        NT SERVICE\Audiosrv, NT SERVICE\AudioEndpointBuilder : SetValue, CreateSubKey, CreateLink, Delete, Read
        NT SERVICE\TrustedInstaller                           : FullControl
        BUILTIN\Administrators                                : QueryValues, SetValue, EnumerateSubKeys,
                                                                Notify, ReadPermissions  (NO CreateSubKey,
                                                                NO Delete, NO WRITE_DAC, NO WRITE_OWNER)
        BUILTIN\Users                                         : QueryValues, SetValue, CreateLink (+ read)
        ALL APPLICATION PACKAGES / one capability SID          : read (+ Query/SetValue for the capability)
      Consequences:
        * Values can be set AND deleted (RegDeleteValue needs only KEY_SET_VALUE) by an administrator
          without touching the ACL - but only if the key is opened with exactly those rights.
          Set-ItemProperty/Remove-ItemProperty open with KEY_WRITE (includes KEY_CREATE_SUB_KEY) and
          therefore FAIL with "access denied" even when elevated. We open keys through .NET with
          explicit RegistryRights instead.
        * Creating a missing FxProperties subkey needs KEY_CREATE_SUB_KEY, which Administrators lack.
          For that case we enable SeBackupPrivilege + SeRestorePrivilege and open with
          REG_OPTION_BACKUP_RESTORE: the privileges grant the access, the DACL and owner are never
          modified, so there is nothing to restore afterwards (unlike take-ownership + re-ACL).
#>
Set-StrictMode -Version 3.0

# ---------------------------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------------------------
$script:E1 = [ordered]@{
    Version          = '1.0.0'
    HardwareIdRegex  = 'VID_041E&PID_323B'
    RenderClsid      = '{9041657D-F768-4739-B84D-7CA73B55FE94}'   # include/e1fx_guids.h
    CaptureClsid     = '{6768B677-635E-46A6-9814-8560B27B571D}'   # include/e1fx_guids.h
    FxDisplayName    = 'E1 Studio'
    MMDevRoot        = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio'
    InstallDir       = (Join-Path $env:ProgramFiles 'E1Studio')
    DataDir          = 'C:\ProgramData\E1Studio'                  # E1FX_SETTINGS_DIR (hard-coded in the APO)
    SettingsFile     = 'C:\ProgramData\E1Studio\settings.bin'     # E1FX_SETTINGS_PATH
    BackupRoot       = 'C:\ProgramData\E1Studio\backup'
    LogDir           = 'C:\ProgramData\E1Studio\logs'
    TaskPath         = '\E1Studio\'
    TaskName         = 'E1Studio Reapply'
    InstallFiles     = @('e1fx.dll', 'e1panel.exe', 'e1ctl.exe', 'e1fx.pdb', 'e1panel.pdb', 'e1ctl.pdb')
    ScriptFiles      = @('install.ps1', 'uninstall.ps1', 'E1Common.psm1')
}

# Registry value names = "{fmtid},pid" of the PROPERTYKEY.
$script:PK = [ordered]@{
    FxAssociation      = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},0'
    FxPreMix           = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1'   # LFX
    FxPostMix          = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2'   # GFX
    FxUi               = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},3'
    FxStream           = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5'   # SFX
    FxMode             = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6'   # MFX  (PKEY_FX_ModeEffectClsid)
    FxEndpoint         = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7'   # EFX
    FxCompositeStream  = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13'
    FxCompositeMode    = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14'
    FxCompositeEndpoint= '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15'
    MfxModes           = '{d3993a3f-99c2-4402-b5ec-a92a0367664b},6'   # PKEY_MFX_ProcessingModes_Supported_For_Streaming
    FxName             = '{b725f130-47ef-101a-a5f1-02608c9eebac},10'
    InstanceId         = '{b3f8fa53-0004-438e-9003-51a46e139bfc},2'
    InterfaceName      = '{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
    DevicePath         = '{233164c8-1b2c-4c7d-bc68-b671687a2567},1'
    DeviceDesc         = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    JackSubType        = '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},8'
    DisableSysFx       = '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5'
    DeviceFormat       = '{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0'   # PKEY_AudioEngine_DeviceFormat
    SpatialFormat      = '{8a845654-d6c3-4cd7-b4eb-243d4bd99032},2'   # undocumented: selected spatial format
}

$script:Guids = [ordered]@{
    ModeDefault    = '{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}'   # AUDIO_SIGNALPROCESSINGMODE_DEFAULT
    NodeSpeaker    = '{DFF21CE1-F70F-11D0-B917-00A0C9223196}'   # KSNODETYPE_SPEAKER
    NodeMicrophone = '{DFF21BE1-F70F-11D0-B917-00A0C9223196}'   # KSNODETYPE_MICROPHONE
    NodeDigital    = '{DFF21FE2-F70F-11D0-B917-00A0C9223196}'   # KSNODETYPE_DIGITAL_AUDIO_INTERFACE ("What U Hear")
    NodeAny        = '{00000000-0000-0000-0000-000000000000}'
}

# FX slots that must not run next to our MFX (Creative LFX/GFX/SFX, any EFX, composite lists, UI).
$script:ForeignFxSlots = @('FxPreMix', 'FxPostMix', 'FxUi', 'FxStream', 'FxEndpoint',
                           'FxCompositeStream', 'FxCompositeMode', 'FxCompositeEndpoint')

$script:SpatialNames = @{
    '{1459AC38-3875-49BF-BB59-0FE80F4D395D}' = 'Dolby Atmos for Headphones'
    '{4C81E564-C8EF-4AD9-9F2C-9EF995533790}' = 'Dolby Atmos for Speakers'
    '{B53D940C-B846-4831-9F76-D102B9B725A0}' = 'Windows Sonic for Headphones'
    '{00000000-0000-0000-0000-000000000000}' = 'off'
}

$script:LogFile = $null

function Get-E1Config { [CmdletBinding()] param() return $script:E1 }
function Get-E1PropertyKeys { [CmdletBinding()] param() return $script:PK }
function Get-E1Guids { [CmdletBinding()] param() return $script:Guids }

# ---------------------------------------------------------------------------------------------
# Logging / environment
# ---------------------------------------------------------------------------------------------
function Set-E1LogFile {
    [CmdletBinding()] param([string]$Path)
    $script:LogFile = $Path
}

function Write-E1Log {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Message,
          [ValidateSet('Info', 'Step', 'Warn', 'Error', 'Ok')][string]$Level = 'Info',
          [switch]$Quiet)
    $prefix = switch ($Level) { 'Step' { '==> ' } 'Warn' { 'WARNING: ' } 'Error' { 'ERROR: ' } 'Ok' { 'OK: ' } default { '    ' } }
    if (-not $Quiet) {
        $color = switch ($Level) { 'Step' { 'Cyan' } 'Warn' { 'Yellow' } 'Error' { 'Red' } 'Ok' { 'Green' } default { 'Gray' } }
        Write-Host ($prefix + $Message) -ForegroundColor $color
    }
    if ($script:LogFile) {
        try {
            Add-Content -LiteralPath $script:LogFile -Value ('{0:yyyy-MM-dd HH:mm:ss} {1}{2}' -f (Get-Date), $prefix, $Message) -Encoding UTF8 -WhatIf:$false
        } catch { Write-Verbose "log write failed: $_" }
    }
}

function Test-E1Admin {
    [CmdletBinding()] param()
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-E1Sha256 {
    [CmdletBinding()] param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    # Not Get-FileHash: it honours -WhatIf and then returns nothing.
    $sha = [Security.Cryptography.SHA256]::Create()
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try { return ([BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '') }
    finally { $fs.Dispose(); $sha.Dispose() }
}

# Returns the PE machine type of an image (0x8664 = x64), or 0 if it is not a PE file.
function Get-E1PeMachine {
    [CmdletBinding()] param([Parameter(Mandatory)][string]$Path)
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $br = New-Object IO.BinaryReader($fs)
        if ($fs.Length -lt 0x40 -or $br.ReadUInt16() -ne 0x5A4D) { return 0 }
        $fs.Position = 0x3C
        $pe = $br.ReadInt32()
        if ($pe -le 0 -or $pe + 6 -gt $fs.Length) { return 0 }
        $fs.Position = $pe
        if ($br.ReadUInt32() -ne 0x00004550) { return 0 }
        return [int]$br.ReadUInt16()
    } finally { $fs.Dispose() }
}

# ---------------------------------------------------------------------------------------------
# Registry access (64-bit view always)
# ---------------------------------------------------------------------------------------------
function Get-E1Hklm {
    [CmdletBinding()] param()
    return [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,
                                                      [Microsoft.Win32.RegistryView]::Registry64)
}

# Opens HKLM\<SubPath>. -Writable requests only the rights Administrators actually hold on the
# MMDevices keys (see the header). Returns $null when the key does not exist.
function Open-E1Key {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$SubPath, [switch]$Writable)
    $hklm = Get-E1Hklm
    try {
        if (-not $Writable) { return $hklm.OpenSubKey($SubPath, $false) }
        $rights = [Security.AccessControl.RegistryRights]'QueryValues, SetValue, EnumerateSubKeys, Notify, ReadPermissions'
        return $hklm.OpenSubKey($SubPath, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, $rights)
    } finally { $hklm.Dispose() }
}

function Test-E1Key {
    [CmdletBinding()] param([Parameter(Mandatory)][string]$SubPath)
    $k = Open-E1Key -SubPath $SubPath
    if ($k) { $k.Dispose(); return $true }
    return $false
}

function Initialize-E1Native {
    [CmdletBinding()] param()
    if ('E1Studio.Native' -as [type]) { return }
    $src = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace E1Studio {
public static class Native {
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct LuidAndAttributes { public long Luid; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct TokenPrivileges { public uint Count; public LuidAndAttributes P; }

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeValueW(string system, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges state,
                                             uint len, IntPtr prev, IntPtr retLen);
    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    static extern int RegCreateKeyExW(IntPtr hKey, string subKey, uint reserved, string cls, uint options,
                                      uint sam, IntPtr sa, out SafeRegistryHandle result, out uint disposition);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileExW(string existing, string newName, uint flags);

    public static void EnablePrivilege(string name) {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x0020 | 0x0008, out tok)) throw new Win32Exception();
        try {
            long luid;
            if (!LookupPrivilegeValueW(null, name, out luid)) throw new Win32Exception();
            TokenPrivileges tp = new TokenPrivileges();
            tp.Count = 1; tp.P.Luid = luid; tp.P.Attributes = 2; /* SE_PRIVILEGE_ENABLED */
            if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero)) throw new Win32Exception();
            int err = Marshal.GetLastWin32Error();
            if (err == 1300) throw new Win32Exception(err, name + " is not held by this account (run elevated)");
        } finally { CloseHandle(tok); }
    }

    /* Opens (creating if needed) HKLM\subKey with REG_OPTION_BACKUP_RESTORE. Access comes from
       SeBackupPrivilege/SeRestorePrivilege; the key's DACL and owner are neither used nor changed. */
    public static RegistryKey OpenHklmBackupRestore(string subKey) {
        EnablePrivilege("SeBackupPrivilege");
        EnablePrivilege("SeRestorePrivilege");
        SafeRegistryHandle h;
        uint disp;
        IntPtr hklm = new IntPtr(unchecked((int)0x80000002));
        int rc = RegCreateKeyExW(hklm, subKey, 0, null, 4 /* REG_OPTION_BACKUP_RESTORE */,
                                 0x0100 /* KEY_WOW64_64KEY */, IntPtr.Zero, out h, out disp);
        if (rc != 0) throw new Win32Exception(rc);
        return RegistryKey.FromHandle(h, RegistryView.Registry64);
    }

    public static void DeleteAtReboot(string path) {
        if (!MoveFileExW(path, null, 4 /* MOVEFILE_DELAY_UNTIL_REBOOT */)) throw new Win32Exception();
    }
}
}
'@
    Add-Type -TypeDefinition $src -Language CSharp -ErrorAction Stop
}

# Opens HKLM\<SubPath> for value writes: normal access first, privileged backup/restore fallback.
# Returns @{ Key = <RegistryKey>; Method = 'acl' | 'backup-restore' }.
function Open-E1KeyForWrite {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$SubPath, [switch]$CreateIfMissing)
    $k = $null
    try { $k = Open-E1Key -SubPath $SubPath -Writable } catch [System.Security.SecurityException] { $k = $null } catch [UnauthorizedAccessException] { $k = $null }
    if ($k) { return @{ Key = $k; Method = 'acl' } }
    if (-not (Test-E1Key -SubPath $SubPath) -and -not $CreateIfMissing) { throw "registry key HKLM\$SubPath does not exist" }
    Initialize-E1Native
    return @{ Key = [E1Studio.Native]::OpenHklmBackupRestore($SubPath); Method = 'backup-restore' }
}

function Get-E1KeySddl {
    [CmdletBinding()] param([Parameter(Mandatory)][string]$SubPath)
    $k = Open-E1Key -SubPath $SubPath
    if (-not $k) { return $null }
    try { return $k.GetAccessControl([Security.AccessControl.AccessControlSections]'Owner, Access').GetSecurityDescriptorSddlForm('Owner, Access') }
    finally { $k.Dispose() }
}

# ---------------------------------------------------------------------------------------------
# Registry value (de)serialisation for manifests
# ---------------------------------------------------------------------------------------------
function ConvertFrom-E1RegValue {
    # Registry value -> JSON-friendly object { Name, Kind, Data }.
    [CmdletBinding()]
    param([Parameter(Mandatory)][Microsoft.Win32.RegistryKey]$Key, [Parameter(Mandatory)][AllowEmptyString()][string]$Name)
    $kind = $Key.GetValueKind($Name)
    $raw = $Key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    return (ConvertTo-E1ValueRecord -Name $Name -Kind $kind.ToString() -Value $raw)
}

function ConvertTo-E1ValueRecord {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Name, [Parameter(Mandatory)][string]$Kind, [AllowNull()]$Value)
    # (no "switch" here: switch output would unroll arrays)
    if ($Kind -eq 'Binary') { $data = if ($null -eq $Value) { '' } else { [Convert]::ToBase64String([byte[]]$Value) } }
    elseif ($Kind -eq 'DWord') { $data = [int64]([int64]$Value -band 0xFFFFFFFFL) }
    elseif ($Kind -eq 'QWord') { $data = [int64]$Value }
    elseif ($Kind -eq 'MultiString') { $data = [string[]]@($Value | Where-Object { $null -ne $_ }) }
    else { $data = [string]$Value }
    $rec = [pscustomobject][ordered]@{ Name = $Name; Kind = $Kind; Data = $null }
    $rec.Data = $data
    return $rec
}

function ConvertTo-E1RegData {
    # Manifest record -> @{ Value; Kind } ready for RegistryKey.SetValue.
    [CmdletBinding()] param([Parameter(Mandatory)]$Record)
    $kind = [Microsoft.Win32.RegistryValueKind]$Record.Kind
    # (no "switch" here: switch output would unroll byte[]/string[] into object[])
    if ($Record.Kind -eq 'Binary') {
        $value = if ([string]::IsNullOrEmpty([string]$Record.Data)) { New-Object byte[] 0 } else { [Convert]::FromBase64String([string]$Record.Data) }
    } elseif ($Record.Kind -eq 'DWord') {
        $u = [uint32]([int64]$Record.Data)
        $value = [BitConverter]::ToInt32([BitConverter]::GetBytes($u), 0)
    } elseif ($Record.Kind -eq 'QWord') {
        $value = [int64]$Record.Data
    } elseif ($Record.Kind -eq 'MultiString') {
        $value = [string[]]@($Record.Data | Where-Object { $null -ne $_ })
    } else {
        $value = [string]$Record.Data
    }
    return @{ Value = $value; Kind = $kind }
}

function Get-E1KeyValues {
    # All values of HKLM\<SubPath> as records (empty array when the key is missing).
    [CmdletBinding()] param([Parameter(Mandatory)][string]$SubPath)
    $k = Open-E1Key -SubPath $SubPath
    if (-not $k) { return @() }
    try {
        return @($k.GetValueNames() | Sort-Object | ForEach-Object { ConvertFrom-E1RegValue -Key $k -Name $_ })
    } finally { $k.Dispose() }
}

function Test-E1RecordEqual {
    [CmdletBinding()] param($A, $B)
    if ($null -eq $A -or $null -eq $B) { return ($null -eq $A -and $null -eq $B) }
    if ($A.Kind -ne $B.Kind) { return $false }
    if ($A.Kind -eq 'MultiString') { return ((@($A.Data) -join "`0") -ceq (@($B.Data) -join "`0")) }
    return ([string]$A.Data -ceq [string]$B.Data)
}

function Format-E1Record {
    [CmdletBinding()] param($Record)
    if ($null -eq $Record) { return '<absent>' }
    $d = switch ($Record.Kind) {
        'MultiString' { '[' + (@($Record.Data) -join ', ') + ']' }
        'Binary'      { $b = [Convert]::FromBase64String([string]$Record.Data); "$($b.Length) bytes " + (($b | Select-Object -First 16 | ForEach-Object { $_.ToString('x2') }) -join ' ') + $(if ($b.Length -gt 16) { ' ...' } else { '' }) }
        default       { [string]$Record.Data }
    }
    return "$($Record.Kind) $d"
}

# ---------------------------------------------------------------------------------------------
# Endpoint discovery
# ---------------------------------------------------------------------------------------------
function Get-E1StateName {
    [CmdletBinding()] param([int]$State)
    switch ($State -band 0xF) { 1 { 'active' } 2 { 'disabled' } 4 { 'notpresent' } 8 { 'unplugged' } default { "state$State" } }
}

function Get-E1EndpointRole {
    # Pure: decide what an E1 endpoint is from its properties.
    [CmdletBinding()]
    param([Parameter(Mandatory)][ValidateSet('Render', 'Capture')][string]$Flow,
          [AllowNull()][string]$JackSubType, [AllowNull()][string]$Association, [AllowNull()][string]$Name)
    if ($Flow -eq 'Render') { return 'render' }
    if ($JackSubType -and $JackSubType -ieq $script:Guids.NodeMicrophone) { return 'mic' }
    if ($JackSubType -and $JackSubType -ieq $script:Guids.NodeDigital) { return 'loopback' }
    if ($Association -and $Association -ieq $script:Guids.NodeMicrophone) { return 'mic' }
    if ($Name -and $Name -match 'What U Hear|Stereo Mix|Loopback') { return 'loopback' }
    if ($Name -and $Name -match 'Microphone|Mic') { return 'mic' }
    return 'unknown'
}

function Get-E1Endpoints {
    <# Lists every MMDevices endpoint that belongs to USB\VID_041E&PID_323B (read-only). #>
    [CmdletBinding()] param([switch]$IncludeNotPresent)
    $result = New-Object System.Collections.Generic.List[object]
    foreach ($flow in 'Render', 'Capture') {
        $flowPath = "$($script:E1.MMDevRoot)\$flow"
        $fk = Open-E1Key -SubPath $flowPath
        if (-not $fk) { continue }
        try { $ids = $fk.GetSubKeyNames() } finally { $fk.Dispose() }
        foreach ($id in $ids) {
            $sub = "$flowPath\$id"
            $ek = Open-E1Key -SubPath $sub
            if (-not $ek) { continue }
            try { $state = [int]$ek.GetValue('DeviceState', 0) } finally { $ek.Dispose() }
            $pk = Open-E1Key -SubPath "$sub\Properties"
            if (-not $pk) { continue }
            try {
                $inst = [string]$pk.GetValue($script:PK.InstanceId, '')
                $path = [string]$pk.GetValue($script:PK.DevicePath, '')
                if ($inst -notmatch $script:E1.HardwareIdRegex -and $path -notmatch $script:E1.HardwareIdRegex) { continue }
                $jack = [string]$pk.GetValue($script:PK.JackSubType, '')
                $desc = [string]$pk.GetValue($script:PK.DeviceDesc, '')
                $iface = [string]$pk.GetValue($script:PK.InterfaceName, '')
                $sysfx = $pk.GetValue($script:PK.DisableSysFx, $null)
                $fmt = $pk.GetValue($script:PK.DeviceFormat, $null)
                $spatial = $pk.GetValue($script:PK.SpatialFormat, $null)
            } finally { $pk.Dispose() }
            $fxk = Open-E1Key -SubPath "$sub\FxProperties"
            $fx = @{}
            $fxExists = [bool]$fxk
            if ($fxk) {
                try { foreach ($n in $fxk.GetValueNames()) { $fx[$n] = ConvertFrom-E1RegValue -Key $fxk -Name $n } } finally { $fxk.Dispose() }
            }
            $assoc = if ($fx.ContainsKey($script:PK.FxAssociation)) { [string]$fx[$script:PK.FxAssociation].Data } else { $null }
            $name = if ($iface) { "$desc ($iface)" } else { $desc }
            $role = Get-E1EndpointRole -Flow $flow -JackSubType $jack -Association $assoc -Name $name
            $stateName = Get-E1StateName -State $state
            $managed = ($role -in 'render', 'mic') -and ($stateName -ne 'notpresent' -or $IncludeNotPresent)
            $result.Add([pscustomobject][ordered]@{
                Flow          = $flow
                Id            = $id
                SubPath       = $sub
                FxSubPath     = "$sub\FxProperties"
                PropSubPath   = "$sub\Properties"
                Name          = $name
                InstanceId    = $(if ($inst) { $inst } else { $path })
                State         = $state
                StateName     = $stateName
                JackSubType   = $jack
                Role          = $role
                Managed       = $managed
                FxExists      = $fxExists
                Fx            = $fx
                DisableSysFx  = ($null -ne $sysfx -and [int64]$sysfx -ne 0)
                DeviceFormat  = $(if ($fmt -is [byte[]]) { [byte[]]$fmt } else { $null })
                SpatialFormat = $(if ($spatial -is [byte[]]) { Get-E1SpatialFormatName -Blob ([byte[]]$spatial) } else { $null })
            })
        }
    }
    return $result.ToArray()
}

# ---------------------------------------------------------------------------------------------
# Plans (pure functions: current values in, list of changes out)
# ---------------------------------------------------------------------------------------------
function New-E1Change {
    param([string]$Action, [string]$Name, $Old, $New, [string]$Reason)
    return [pscustomobject][ordered]@{ Action = $Action; Name = $Name; Old = $Old; New = $New; Reason = $Reason }
}

function Get-E1FxPlan {
    <#
        Computes the FxProperties changes that make an endpoint run only E1 Studio:
          MFX (,6) = our CLSID; LFX/GFX/SFX/EFX/UI/composite lists removed; association present;
          DEFAULT processing mode listed for MFX; effect display name "E1 Studio".
        Values: hashtable name -> record (as produced by ConvertFrom-E1RegValue).
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)][ValidateSet('render', 'mic')][string]$Role,
          [Parameter(Mandatory)][hashtable]$Values,
          [AllowNull()][AllowEmptyString()][string]$JackSubType)
    $changes = New-Object System.Collections.Generic.List[object]
    $want = if ($Role -eq 'render') { $script:E1.RenderClsid } else { $script:E1.CaptureClsid }

    foreach ($slot in $script:ForeignFxSlots) {
        $n = $script:PK[$slot]
        if ($Values.ContainsKey($n)) { $changes.Add((New-E1Change 'Delete' $n $Values[$n] $null "remove $slot (other vendor effect)")) }
    }
    $cur = if ($Values.ContainsKey($script:PK.FxMode)) { $Values[$script:PK.FxMode] } else { $null }
    $new = ConvertTo-E1ValueRecord -Name $script:PK.FxMode -Kind 'String' -Value $want
    if (-not $cur -or $cur.Kind -ne 'String' -or [string]$cur.Data -ne $want) {
        $changes.Add((New-E1Change 'Set' $script:PK.FxMode $cur $new 'MFX = E1 Studio APO'))
    }
    if (-not $Values.ContainsKey($script:PK.FxAssociation)) {
        $assoc = if ($JackSubType) { $JackSubType } else { $script:Guids.NodeAny }
        $changes.Add((New-E1Change 'Set' $script:PK.FxAssociation $null (ConvertTo-E1ValueRecord -Name $script:PK.FxAssociation -Kind 'String' -Value $assoc) 'FX association (KS node type)'))
    }
    $modes = if ($Values.ContainsKey($script:PK.MfxModes)) { $Values[$script:PK.MfxModes] } else { $null }
    $modeList = @()
    if ($modes -and $modes.Kind -eq 'MultiString') { $modeList = @($modes.Data | Where-Object { $_ }) }
    if (-not ($modeList | Where-Object { $_ -ieq $script:Guids.ModeDefault })) {
        $newModes = ConvertTo-E1ValueRecord -Name $script:PK.MfxModes -Kind 'MultiString' -Value ([string[]](@($modeList) + $script:Guids.ModeDefault))
        $changes.Add((New-E1Change 'Set' $script:PK.MfxModes $modes $newModes 'MFX runs in the DEFAULT processing mode'))
    }
    $nm = if ($Values.ContainsKey($script:PK.FxName)) { $Values[$script:PK.FxName] } else { $null }
    if (-not $nm -or $nm.Kind -ne 'String' -or [string]$nm.Data -ne $script:E1.FxDisplayName) {
        $changes.Add((New-E1Change 'Set' $script:PK.FxName $nm (ConvertTo-E1ValueRecord -Name $script:PK.FxName -Kind 'String' -Value $script:E1.FxDisplayName) 'effects display name'))
    }
    return $changes.ToArray()   # emitted element by element; callers wrap the call in @()
}

function Get-E1RestorePlan {
    <# Changes that turn the current values into exactly the snapshot values. #>
    [CmdletBinding()]
    param([Parameter(Mandatory)][hashtable]$Current, [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Snapshot)
    $changes = New-Object System.Collections.Generic.List[object]
    $snap = @{}
    foreach ($r in $Snapshot) { $snap[$r.Name] = $r }
    foreach ($n in ($Current.Keys | Sort-Object)) {
        if (-not $snap.ContainsKey($n)) { $changes.Add((New-E1Change 'Delete' $n $Current[$n] $null 'not present in backup')) }
    }
    foreach ($n in ($snap.Keys | Sort-Object)) {
        $cur = if ($Current.ContainsKey($n)) { $Current[$n] } else { $null }
        if (-not (Test-E1RecordEqual $cur $snap[$n])) { $changes.Add((New-E1Change 'Set' $n $cur $snap[$n] 'restore backed-up value')) }
    }
    return $changes.ToArray()
}

function Select-E1Snapshots {
    <#
        Chooses, per endpoint (Flow+Id), the snapshot to restore.
        Default: the OLDEST snapshot of the current installation that was taken before E1 Studio
        touched the endpoint (WasApplied = $false) - i.e. the original state, even if later
        -Reapply runs added newer backups. With -BackupName only that backup is used.
        Manifests: objects with .Name, .Created (sortable string) and .Endpoints[].
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Manifests, [string]$BackupName)
    $pool = if ($BackupName) { @($Manifests | Where-Object { $_.Name -eq $BackupName }) } else { @($Manifests) }
    $chosen = [ordered]@{}
    foreach ($m in ($pool | Sort-Object -Property Created)) {
        foreach ($e in @($m.Endpoints)) {
            $key = "$($e.Flow)\$($e.Id)"
            if (-not $chosen.Contains($key)) {
                $chosen[$key] = [pscustomobject]@{ Backup = $m.Name; Endpoint = $e }
            } elseif ($chosen[$key].Endpoint.WasApplied -and -not $e.WasApplied) {
                $chosen[$key] = [pscustomobject]@{ Backup = $m.Name; Endpoint = $e }
            }
        }
    }
    return @($chosen.Values)
}

# ---------------------------------------------------------------------------------------------
# Wave format helpers (PKEY_AudioEngine_DeviceFormat = 8-byte PROPVARIANT header + WAVEFORMATEX[TENSIBLE])
# ---------------------------------------------------------------------------------------------
function Get-E1WaveFormatInfo {
    [CmdletBinding()] param([AllowNull()][byte[]]$Blob)
    if ($null -eq $Blob -or $Blob.Length -lt 8 + 18) { return $null }
    $o = 8
    $tag = [BitConverter]::ToUInt16($Blob, $o)
    $info = [ordered]@{
        Tag        = $tag
        Channels   = [BitConverter]::ToUInt16($Blob, $o + 2)
        Rate       = [BitConverter]::ToUInt32($Blob, $o + 4)
        BlockAlign = [BitConverter]::ToUInt16($Blob, $o + 12)
        Bits       = [BitConverter]::ToUInt16($Blob, $o + 14)
        ValidBits  = [BitConverter]::ToUInt16($Blob, $o + 14)
        Mask       = 0
        SubFormat  = $null
    }
    if ($tag -eq 0xFFFE -and $Blob.Length -ge 8 + 40) {
        $info.ValidBits = [BitConverter]::ToUInt16($Blob, $o + 18)
        $info.Mask = [BitConverter]::ToUInt32($Blob, $o + 20)
        $g = New-Object byte[] 16
        [Array]::Copy($Blob, $o + 24, $g, 0, 16)
        $info.SubFormat = ([guid]$g).ToString('B').ToUpperInvariant()
    }
    $isPcm = ($tag -eq 1) -or ($info.SubFormat -eq '{00000001-0000-0010-8000-00AA00389B71}')
    $info.IsPcm = $isPcm
    $info.Is44k24Stereo = $isPcm -and $info.Rate -eq 44100 -and $info.Bits -eq 24 -and $info.ValidBits -eq 24 -and $info.Channels -eq 2 -and $info.BlockAlign -eq 6
    $info.Text = '{0} Hz, {1}-bit {2} ({3} valid), {4} ch' -f $info.Rate, $info.Bits, $(if ($isPcm) { 'PCM' } elseif ($tag -eq 3 -or $info.SubFormat -like '{00000003-*') { 'float' } else { "tag 0x{0:X}" -f $tag }), $info.ValidBits, $info.Channels
    return [pscustomobject]$info
}

function New-E1WaveFormat24Blob {
    <# 44.1 kHz / 24-bit packed / stereo WAVEFORMATEXTENSIBLE, keeping the existing 8-byte header. #>
    [CmdletBinding()] param([AllowNull()][byte[]]$Existing)
    $out = New-Object byte[] 48
    if ($Existing -and $Existing.Length -ge 8) { [Array]::Copy($Existing, 0, $out, 0, 8) }
    else { [Array]::Copy([byte[]](0x41, 0, 0, 0, 1, 0, 0, 0), 0, $out, 0, 8) }
    $w = New-Object IO.BinaryWriter((New-Object IO.MemoryStream(, $out)))
    $w.Seek(8, 'Begin') | Out-Null
    $w.Write([uint16]0xFFFE)      # WAVE_FORMAT_EXTENSIBLE
    $w.Write([uint16]2)           # channels
    $w.Write([uint32]44100)       # rate
    $w.Write([uint32](44100 * 6)) # avg bytes/s
    $w.Write([uint16]6)           # block align
    $w.Write([uint16]24)          # container bits
    $w.Write([uint16]22)          # cbSize
    $w.Write([uint16]24)          # valid bits
    $w.Write([uint32]3)           # FL | FR
    $w.Write(([guid]'00000001-0000-0010-8000-00aa00389b71').ToByteArray())  # KSDATAFORMAT_SUBTYPE_PCM
    $w.Flush()
    return , $out
}

function Get-E1SpatialFormatName {
    [CmdletBinding()] param([AllowNull()][byte[]]$Blob)
    if ($null -eq $Blob -or $Blob.Length -lt 32) { return $null }
    $g = New-Object byte[] 16
    [Array]::Copy($Blob, 16, $g, 0, 16)
    $s = ([guid]$g).ToString('B').ToUpperInvariant()
    if ($script:SpatialNames.ContainsKey($s)) { return $script:SpatialNames[$s] }
    return "third-party format $s"
}

# ---------------------------------------------------------------------------------------------
# Other system state (read-only)
# ---------------------------------------------------------------------------------------------
function Get-E1CreativePanelRunEntries {
    <# Autostart values that launch Creative's SBE.exe (HKLM 64/32-bit views and the current user). #>
    [CmdletBinding()] param()
    $runPath = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
    $list = New-Object System.Collections.Generic.List[object]
    $roots = @(
        @{ Hive = 'HKLM'; View = [Microsoft.Win32.RegistryView]::Registry64; BaseHive = [Microsoft.Win32.RegistryHive]::LocalMachine; Label = 'HKLM64' },
        @{ Hive = 'HKLM'; View = [Microsoft.Win32.RegistryView]::Registry32; BaseHive = [Microsoft.Win32.RegistryHive]::LocalMachine; Label = 'HKLM32' },
        @{ Hive = 'HKCU'; View = [Microsoft.Win32.RegistryView]::Default; BaseHive = [Microsoft.Win32.RegistryHive]::CurrentUser; Label = 'HKCU' }
    )
    foreach ($r in $roots) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($r.BaseHive, $r.View)
        try {
            $k = $base.OpenSubKey($runPath, $false)
            if (-not $k) { continue }
            try {
                foreach ($n in $k.GetValueNames()) {
                    $v = [string]$k.GetValue($n, '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    if ($v -match '(?i)\\SBE\.exe') {
                        $list.Add([pscustomobject][ordered]@{ Root = $r.Label; SubPath = $runPath; Name = $n; Kind = $k.GetValueKind($n).ToString(); Value = $v })
                    }
                }
            } finally { $k.Dispose() }
        } finally { $base.Dispose() }
    }
    return $list.ToArray()
}

function Open-E1RunKey {
    <# Opens a Run key (read-only unless -Writable). Returns $null when the key does not exist. #>
    [CmdletBinding()] param([Parameter(Mandatory)][ValidateSet('HKLM64', 'HKLM32', 'HKCU')][string]$Root,
                            [Parameter(Mandatory)][string]$SubPath, [switch]$Writable)
    $map = @{
        HKLM64 = @([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
        HKLM32 = @([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry32)
        HKCU   = @([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Default)
    }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($map[$Root][0], $map[$Root][1])
    try { return $base.OpenSubKey($SubPath, [bool]$Writable) } finally { $base.Dispose() }
}

function Open-E1RunKeyForWrite {
    [CmdletBinding()] param([Parameter(Mandatory)][ValidateSet('HKLM64', 'HKLM32', 'HKCU')][string]$Root, [Parameter(Mandatory)][string]$SubPath)
    return (Open-E1RunKey -Root $Root -SubPath $SubPath -Writable)
}

# ---------------------------------------------------------------------------------------------
# Validation of backup manifests before an elevated restore uses them
# ---------------------------------------------------------------------------------------------
$script:RunSubPath = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$script:ValueKinds = @('String', 'ExpandString', 'Binary', 'DWord', 'QWord', 'MultiString')

function Test-E1RunEntryRecord {
    <# $null when a manifest RunEntries record is exactly what install.ps1 -DisableCreativePanel can
       have removed (an SBE.exe value of a Run key); otherwise the reason it must not be restored. #>
    [CmdletBinding()] param([AllowNull()]$Entry)
    if ($null -eq $Entry) { return 'empty record' }
    foreach ($p in 'Root', 'SubPath', 'Name', 'Kind', 'Value') { if (-not $Entry.PSObject.Properties[$p]) { return "missing $p" } }
    if ([string]$Entry.Root -cnotin @('HKLM64', 'HKLM32', 'HKCU')) { return "root '$($Entry.Root)' is not a Run key root" }
    if (-not [string]::Equals([string]$Entry.SubPath, $script:RunSubPath, [StringComparison]::OrdinalIgnoreCase)) { return "path '$($Entry.SubPath)' is not the Run key" }
    if ([string]$Entry.Kind -cnotin @('String', 'ExpandString')) { return "kind '$($Entry.Kind)' is not a string" }
    $n = [string]$Entry.Name
    if ($n.Length -eq 0 -or $n.Length -gt 255 -or $n -match '[\x00-\x1f\\]') { return 'invalid value name' }
    if ([string]$Entry.Value -notmatch '(?i)\\SBE\.exe') { return 'value does not start SBE.exe' }
    return $null
}

function Test-E1EndpointRecord {
    <# $null when a manifest endpoint record only addresses an E1 endpoint's FxProperties/DeviceFormat
       values; otherwise the reason it must not be used. #>
    [CmdletBinding()] param([AllowNull()]$Endpoint)
    if ($null -eq $Endpoint) { return 'empty record' }
    foreach ($p in 'Flow', 'Id', 'FxValues') { if (-not $Endpoint.PSObject.Properties[$p]) { return "missing $p" } }
    if ([string]$Endpoint.Flow -cnotin @('Render', 'Capture')) { return "flow '$($Endpoint.Flow)'" }
    if ([string]$Endpoint.Id -notmatch '^\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}$') { return "endpoint id '$($Endpoint.Id)'" }
    foreach ($r in @($Endpoint.FxValues)) {
        if ($null -eq $r) { continue }
        if (-not $r.PSObject.Properties['Name'] -or -not $r.PSObject.Properties['Kind']) { return 'value record without Name/Kind' }
        if ([string]$r.Name -notmatch '^\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\},\d{1,10}$') { return "value name '$($r.Name)' is not a property key" }
        if ([string]$r.Kind -cnotin $script:ValueKinds) { return "value kind '$($r.Kind)'" }
    }
    if ($Endpoint.PSObject.Properties['DeviceFormat'] -and $null -ne $Endpoint.DeviceFormat) {
        $f = $Endpoint.DeviceFormat
        if ([string]$f.Name -ne $script:PK.DeviceFormat -or [string]$f.Kind -cne 'Binary') { return 'device format record' }
    }
    return $null
}

function Get-E1ApoRegistration {
    <# Where a COM/APO CLSID is registered (64-bit view). #>
    [CmdletBinding()] param([Parameter(Mandatory)][string]$Clsid)
    $paths = [ordered]@{
        Com    = "SOFTWARE\Classes\CLSID\$Clsid"
        Com32  = "SOFTWARE\Classes\WOW6432Node\CLSID\$Clsid"
        Apo    = "SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$Clsid"
    }
    $res = [ordered]@{ Clsid = $Clsid; InprocServer = $null }
    foreach ($k in $paths.Keys) {
        $key = Open-E1Key -SubPath $paths[$k]
        $res[$k] = [bool]$key
        $res["${k}Path"] = $paths[$k]
        if ($key) { $key.Dispose() }
    }
    $ip = Open-E1Key -SubPath "$($paths.Com)\InprocServer32"
    if ($ip) { try { $res.InprocServer = [string]$ip.GetValue('') } finally { $ip.Dispose() } }
    return [pscustomobject]$res
}

function Get-E1ProtectedAudioDg {
    [CmdletBinding()] param()
    $k = Open-E1Key -SubPath 'SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'
    if (-not $k) { return $null }
    try { return $k.GetValue('DisableProtectedAudioDG', $null) } finally { $k.Dispose() }
}

# ---------------------------------------------------------------------------------------------
# Data directory ACLs (well-known SIDs: names are localised, e.g. "Usuarios" on pt-BR Windows)
# ---------------------------------------------------------------------------------------------
function New-E1DirectorySecurity {
    <#
        Kind 'data'   : SYSTEM/Administrators Full, LOCAL SERVICE Read; Users: on the folder itself
                        only list + create files (no delete/rename of the folder, no subfolders, no
                        ACL change), and Modify on files only (settings.bin: the panel writes a temp
                        file and renames it over settings.bin).
        Kind 'admin'  : SYSTEM/Administrators Full only                              (backups, install state)
        Kind 'log'    : SYSTEM/Administrators Full, Users Read
        Owner: Administrators. Inheritance from the parent is disabled.
    #>
    [CmdletBinding()] param([Parameter(Mandatory)][ValidateSet('data', 'admin', 'log')][string]$Kind)
    $sid = @{
        System = New-Object Security.Principal.SecurityIdentifier('S-1-5-18')
        Admins = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
        Users  = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')
        LocalService = New-Object Security.Principal.SecurityIdentifier('S-1-5-19')
    }
    $inh = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    $prop = [Security.AccessControl.PropagationFlags]::None
    $allow = [Security.AccessControl.AccessControlType]::Allow
    $ds = New-Object Security.AccessControl.DirectorySecurity
    $ds.SetAccessRuleProtection($true, $false)
    $ds.SetOwner($sid.Admins)
    $ds.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid.System, 'FullControl', $inh, $prop, $allow)))
    $ds.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid.Admins, 'FullControl', $inh, $prop, $allow)))
    switch ($Kind) {
        'data' {
            $none = [Security.AccessControl.InheritanceFlags]::None
            $ds.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid.Users, [Security.AccessControl.FileSystemRights]'ReadAndExecute, CreateFiles', $none, $prop, $allow)))
            $ds.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid.Users, 'Modify', [Security.AccessControl.InheritanceFlags]::ObjectInherit, [Security.AccessControl.PropagationFlags]::InheritOnly, $allow)))
            $ds.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid.LocalService, 'ReadAndExecute', $inh, $prop, $allow)))
        }
        'log' {
            $ds.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid.Users, 'ReadAndExecute', $inh, $prop, $allow)))
        }
    }
    return $ds
}

function Test-E1DaclEquivalent {
    <# True when two "D:..." SDDL strings are both protected and hold the same set of ACEs
       (Windows may store the ACEs in a different order than they were added). #>
    [CmdletBinding()] param([AllowNull()][string]$A, [AllowNull()][string]$B)
    if (-not $A -or -not $B) { return $false }
    $parse = {
        param([string]$s)
        if ($s -notmatch '^D:([A-Z]*)(\(.*\))?$') { return $null }
        $flags = $Matches[1]
        $aces = @([regex]::Matches($s, '\([^)]*\)') | ForEach-Object { $_.Value.ToUpperInvariant() } | Sort-Object)
        return [pscustomobject]@{ Protected = $flags.Contains('P'); Aces = ($aces -join '') }
    }
    $x = & $parse $A
    $y = & $parse $B
    if (-not $x -or -not $y) { return $false }
    return ($x.Protected -and $y.Protected -and $x.Aces -eq $y.Aces)
}

$script:TrustedSids = @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
# rights that let a principal change an object or what it contains: WriteData/AddFile 0x2,
# AppendData/AddSubdirectory 0x4, WriteEA 0x10, DeleteChild 0x40, WriteAttributes 0x100, DELETE,
# WRITE_DAC, WRITE_OWNER, GENERIC_ALL/GENERIC_WRITE, MAXIMUM_ALLOWED
$script:WriteMask = 0x2 -bor 0x4 -bor 0x10 -bor 0x40 -bor 0x100 -bor 0x10000 -bor 0x40000 -bor 0x80000 -bor 0x10000000 -bor 0x40000000 -bor 0x02000000

function Get-E1TrustProblem {
    <#
        $null when only SYSTEM/Administrators (or TrustedInstaller) can change the folder or file at
        $Path: it is not a reparse point, its owner is Administrators or SYSTEM, and no other principal
        holds a right that modifies it, its contents or its ACL. Otherwise a description of the problem.
        -Kind data: the E1Studio root, where Users may add files and modify files (but not change the
        folder itself, delete/rename it, or add subfolders). -OwnerOnly: check reparse + owner only.
        Read-only; throws when the ACL cannot be read (not elevated).
    #>
    [CmdletBinding()] param([Parameter(Mandatory)][string]$Path, [ValidateSet('data', 'admin', 'log', 'file')][string]$Kind = 'admin', [switch]$OwnerOnly)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { return "$Path is a reparse point (junction/symlink)" }
    $acl = $item.GetAccessControl([Security.AccessControl.AccessControlSections]'Owner, Access')
    $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin $script:TrustedSids) { return "$Path is owned by $owner, not by Administrators or SYSTEM" }
    if ($OwnerOnly) { return $null }
    $isDir = $item.PSIsContainer
    foreach ($r in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($r.AccessControlType -ne [Security.AccessControl.AccessControlType]::Allow) { continue }
        $sid = $r.IdentityReference.Value
        if ($sid -in $script:TrustedSids) { continue }
        $mask = [int]$r.FileSystemRights
        if (($mask -band $script:WriteMask) -eq 0) { continue }
        $inheritOnly = [bool]($r.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly)
        $toDirs = [bool]($r.InheritanceFlags -band [Security.AccessControl.InheritanceFlags]::ContainerInherit)
        if (-not $isDir -and $inheritOnly) { continue }                     # never applies to a file
        if ($Kind -eq 'data' -and $sid -eq 'S-1-5-32-545') {
            if ($inheritOnly -and -not $toDirs) { continue }                # Users Modify on files only
            if (-not $inheritOnly -and -not $toDirs -and -not [bool]($r.InheritanceFlags -band [Security.AccessControl.InheritanceFlags]::ObjectInherit) -and
                ($mask -band $script:WriteMask) -eq 0x2) { continue }       # Users may add files to the folder
        }
        return ("{0}: {1} may modify it (rights 0x{2:X})" -f $Path, $sid, $mask)
    }
    return $null
}

function Set-E1SecureDirectory {
    <#
        Makes $Path a folder owned by Administrators with exactly the ACL of -Kind. A missing folder is
        created atomically WITH that security descriptor (no window with the inherited ProgramData ACL,
        where Users may create subfolders). Returns 'ok', 'created' or 'reset'. Callers guard it with
        ShouldProcess.
    #>
    [CmdletBinding()] param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][ValidateSet('data', 'admin', 'log')][string]$Kind)
    $want = New-E1DirectorySecurity -Kind $Kind
    if (-not (Test-Path -LiteralPath $Path)) {
        [void][IO.Directory]::CreateDirectory($Path, $want)
        return 'created'
    }
    if (Test-E1ReparsePoint -Path $Path) { throw "$Path is a reparse point (junction/symlink)" }
    if (Test-E1SecureDirectory -Path $Path -Kind $Kind) { return 'ok' }
    (Get-Item -LiteralPath $Path -Force).SetAccessControl($want)
    return 'reset'
}

function Test-E1SecureDirectory {
    <# True when $Path is owned by Administrators/SYSTEM and its DACL is exactly the -Kind ACL. #>
    [CmdletBinding()] param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][ValidateSet('data', 'admin', 'log')][string]$Kind)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $cur = (Get-Item -LiteralPath $Path -Force).GetAccessControl([Security.AccessControl.AccessControlSections]'Owner, Access')
    $owner = $cur.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin @('S-1-5-18', 'S-1-5-32-544')) { return $false }
    $want = New-E1DirectorySecurity -Kind $Kind
    return (Test-E1DaclEquivalent $cur.GetSecurityDescriptorSddlForm('Access') $want.GetSecurityDescriptorSddlForm('Access'))
}

function Get-E1StateTrustProblem {
    <#
        Before install/uninstall/reapply read install.json or backup manifests: the E1Studio root must
        be owned by Administrators/SYSTEM (a standard user who could rename the old root may have
        planted a new tree), and backup\ and logs\ must be owned by Administrators/SYSTEM with no
        write right for anybody else. $null = trusted. Missing folders are fine.
    #>
    [CmdletBinding()] param()
    $p = Get-E1TrustProblem -Path $script:E1.DataDir -Kind data -OwnerOnly
    if ($p) { return $p }
    $p = Get-E1TrustProblem -Path $script:E1.BackupRoot -Kind admin
    if ($p) { return $p }
    return (Get-E1TrustProblem -Path $script:E1.LogDir -Kind log)
}

function Test-E1ReparsePoint {
    [CmdletBinding()] param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $item = Get-Item -LiteralPath $Path -Force
    return [bool]($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}

# ---------------------------------------------------------------------------------------------
# Install state + backups
# ---------------------------------------------------------------------------------------------
function Test-E1TrustedStateFile {
    <# install.json / manifest.json may only be trusted when Administrators/SYSTEM own it and nobody
       else can write it. Unreadable ACL (not elevated: dry run) counts as trusted for reading. #>
    [CmdletBinding()] param([Parameter(Mandatory)][string]$Path)
    try { $p = Get-E1TrustProblem -Path $Path -Kind file }
    catch { return $true }
    if ($p) { Write-Warning "ignoring $Path : $p"; return $false }
    return $true
}

function Get-E1InstallState {
    [CmdletBinding()] param()
    $f = Join-Path $script:E1.BackupRoot 'install.json'
    # The backup root is administrators-only: a non-elevated dry run cannot see into it.
    try { if (-not (Test-Path -LiteralPath $f -ErrorAction Stop)) { return $null } }
    catch [System.UnauthorizedAccessException] { Write-Warning "install.json not readable without elevation (dry run)"; return $null }
    if (-not (Test-E1TrustedStateFile -Path $f)) { return $null }
    return (Get-Content -LiteralPath $f -Raw -Encoding UTF8 | ConvertFrom-Json)
}

function Get-E1BackupManifests {
    <# Every backup manifest under the backup root, oldest first. #>
    [CmdletBinding()] param([string]$Root = $script:E1.BackupRoot)
    try {
        if (-not (Test-Path -LiteralPath $Root -ErrorAction Stop)) { return @() }
        $dirs = @(Get-ChildItem -LiteralPath $Root -Directory -ErrorAction Stop | Sort-Object Name)
    } catch [System.UnauthorizedAccessException] {
        Write-Warning "backups in $Root not readable without elevation (dry run)"; return @()
    }
    $list = foreach ($d in $dirs) {
        $mf = Join-Path $d.FullName 'manifest.json'
        if ((Test-Path -LiteralPath $mf) -and (Test-E1TrustedStateFile -Path $mf)) {
            try {
                $m = Get-Content -LiteralPath $mf -Raw -Encoding UTF8 | ConvertFrom-Json
                $m | Add-Member -NotePropertyName Path -NotePropertyValue $d.FullName -Force
                $m
            } catch { Write-Warning "unreadable manifest $mf : $_" }
        }
    }
    return @($list)
}

function Restart-E1AudioServices {
    <#
        Restarts AudioEndpointBuilder (which also stops Audiosrv and the other dependents) and
        starts again every dependent that was running before. Restarting the endpoint builder is
        what makes it re-read the FxProperties/Properties it caches; Audiosrv restart recreates
        audiodg.exe, which loads the APOs. Returns the list of services restarted.
    #>
    [CmdletBinding()] param([int]$TimeoutSec = 30)
    $aeb = Get-Service -Name AudioEndpointBuilder
    $running = @($aeb.DependentServices | Where-Object { $_.Status -eq 'Running' } | ForEach-Object { $_.Name })
    Restart-Service -Name AudioEndpointBuilder -Force -ErrorAction Stop
    $order = @('Audiosrv') + @($running | Where-Object { $_ -ne 'Audiosrv' })
    foreach ($s in $order) {
        try { Start-Service -Name $s -ErrorAction Stop } catch { Write-Warning "could not restart service $s : $($_.Exception.Message)" }
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $st = (Get-Service -Name Audiosrv).Status
        if ($st -eq 'Running') { break }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    if ((Get-Service -Name Audiosrv).Status -ne 'Running') { throw "Audiosrv did not reach Running within $TimeoutSec s" }
    return $order
}

Export-ModuleMember -Function *-E1*
