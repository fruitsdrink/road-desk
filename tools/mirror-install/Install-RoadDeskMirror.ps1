# Road Desk Mirror - Win7 one-click installer (self-contained; runs from stage package dir).
# Double-click Install-RoadDeskMirror.bat (UAC). If already installed: uninstall then reinstall.
# First run may only enable test-signing and ask for reboot; run again after reboot to finish.
param(
    [switch]$UninstallOnly,
    [switch]$NoRebootPrompt
)

$ErrorActionPreference = "Stop"
$ScriptPath = $MyInvocation.MyCommand.Path
$PackageDir = Split-Path -Parent $ScriptPath
$PendingMarker = Join-Path $env:ProgramData "RoadDesk\mirror-install-pending"
$HwIdGm2 = "Root\RoadDesk_XPDM_Mirror1"
$DoUninstallOnly = [bool]$UninstallOnly
$DoNoRebootPrompt = [bool]$NoRebootPrompt

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Ensure-Admin {
    if (Test-IsAdmin) { return }
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`""
    if ($DoUninstallOnly) { $argList += " -UninstallOnly" }
    if ($DoNoRebootPrompt) { $argList += " -NoRebootPrompt" }
    Write-Host "Requesting Administrator elevation..."
    $p = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $p.ExitCode
}

function Show-Msg([string]$Text, [string]$Title = "Road Desk Mirror", [string]$Buttons = "OK") {
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $btn = [System.Windows.Forms.MessageBoxButtons]::OK
        if ($Buttons -eq "YesNo") { $btn = [System.Windows.Forms.MessageBoxButtons]::YesNo }
        $r = [System.Windows.Forms.MessageBox]::Show($Text, $Title, $btn)
        return "$r"
    } catch {
        Write-Host ""
        Write-Host $Text
        if ($Buttons -eq "YesNo") {
            $a = Read-Host "Yes/No [Y/N]"
            if ($a -match '^[Yy]') { return "Yes" }
            return "No"
        }
        Read-Host "Press Enter to continue"
        return "OK"
    }
}

function Get-TestSigningYes {
    $out = & bcdedit 2>&1 | Out-String
    return ($out -match "testsigning\s+Yes")
}

function Enable-TestSigning {
    & bcdedit /set testsigning on | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "bcdedit testsigning on failed" }
    & bcdedit /set nointegritychecks on | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "bcdedit nointegritychecks on failed" }
}

function Import-TestCert {
    $cer = Join-Path $PackageDir "RoadDeskTestCert.cer"
    if (-not (Test-Path $cer)) { throw "Missing $cer" }
    & certutil -addstore Root $cer | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "certutil Root failed" }
    & certutil -addstore TrustedPublisher $cer | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "certutil TrustedPublisher failed" }
}

function Test-ServiceExists([string]$Name) {
    $o = & sc.exe query $Name 2>&1 | Out-String
    return ($o -match "STATE\s*:")
}

function Test-ServiceRunning([string]$Name) {
    $o = & sc.exe query $Name 2>&1 | Out-String
    return ($o -match "RUNNING")
}

function Test-MirrorInstalled {
    if (Test-ServiceExists "rdmmirror") { return $true }
    if (Test-ServiceExists "rdmmini") { return $true }
    $sys = Join-Path $env:SystemRoot "system32\drivers\rdmmirror.sys"
    $mini = Join-Path $env:SystemRoot "system32\drivers\rdmmini.sys"
    if ((Test-Path $sys) -or (Test-Path $mini)) { return $true }
    return $false
}

# --- SetupAPI: create / remove root-enumerated devices (avoids Device Manager wizard) ---
function Ensure-SetupApiType {
    try {
        [void][RoadDesk.SetupApiNative]
        return
    } catch {
        # type not loaded yet
    }
    $src = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace RoadDesk {
  public static class SetupApiNative {
    public const int DIGCF_ALLCLASSES = 0x00000004;
    public const int DIGCF_PRESENT = 0x00000002;
    public const int DICD_GENERATE_ID = 0x00000001;
    public const int SPDRP_HARDWAREID = 0x00000001;
    public const int DIF_REGISTERDEVICE = 0x00000019;
    public const int DIF_REMOVE = 0x00000005;
    public const uint INSTALLFLAG_FORCE = 0x00000001;
    public const int ERROR_NO_MORE_ITEMS = 259;

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA {
      public int cbSize;
      public Guid ClassGuid;
      public int DevInst;
      public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetupDiCreateDeviceInfo(
      IntPtr DeviceInfoSet, string DeviceName, ref Guid ClassGuid,
      string DeviceDescription, IntPtr hwndParent, int CreationFlags,
      ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetupDiSetDeviceRegistryProperty(
      IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData,
      int Property, byte[] PropertyBuffer, int PropertyBufferSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiCallClassInstaller(
      int InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SetupDiGetClassDevs(
      IntPtr ClassGuid, string Enumerator, IntPtr hwndParent, int Flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiEnumDeviceInfo(
      IntPtr DeviceInfoSet, int MemberIndex, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetupDiGetDeviceRegistryProperty(
      IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, int Property,
      out int PropertyRegDataType, byte[] PropertyBuffer, int PropertyBufferSize,
      out int RequiredSize);

    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool UpdateDriverForPlugAndPlayDevices(
      IntPtr hwndParent, string HardwareId, string FullInfPath,
      uint InstallFlags, out bool bRebootRequired);
  }
}
"@
    Add-Type -TypeDefinition $src -Language CSharp -ErrorAction Stop
}

function Install-RootDevice {
    param(
        [string]$HardwareId,
        [string]$InfPath,
        [Guid]$ClassGuid,
        [string]$Description
    )
    Ensure-SetupApiType
    $N = [RoadDesk.SetupApiNative]
    $list = $N::SetupDiCreateDeviceInfoList([ref]$ClassGuid, [IntPtr]::Zero)
    if ($list -eq [IntPtr]::Zero) {
        throw "SetupDiCreateDeviceInfoList failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    try {
        $data = New-Object RoadDesk.SetupApiNative+SP_DEVINFO_DATA
        $data.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($data)
        # DeviceName = class short name style; GENERATE_ID assigns instance
        $ok = $N::SetupDiCreateDeviceInfo($list, "RoadDeskMirror", [ref]$ClassGuid, $Description,
            [IntPtr]::Zero, $N::DICD_GENERATE_ID, [ref]$data)
        if (-not $ok) {
            throw "SetupDiCreateDeviceInfo failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        $hid = [Text.Encoding]::Unicode.GetBytes(($HardwareId + "`0`0"))
        $ok = $N::SetupDiSetDeviceRegistryProperty($list, [ref]$data, $N::SPDRP_HARDWAREID, $hid, $hid.Length)
        if (-not $ok) {
            throw "SetupDiSetDeviceRegistryProperty failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        $ok = $N::SetupDiCallClassInstaller($N::DIF_REGISTERDEVICE, $list, [ref]$data)
        if (-not $ok) {
            throw "DIF_REGISTERDEVICE failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
    } finally {
        [void]$N::SetupDiDestroyDeviceInfoList($list)
    }

    $reboot = $false
    $ok = $N::UpdateDriverForPlugAndPlayDevices([IntPtr]::Zero, $HardwareId, $InfPath,
        $N::INSTALLFLAG_FORCE, [ref]$reboot)
    if (-not $ok) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        # 0xE0000217 / 0xE000020B often mean already installed - continue
        Write-Host "UpdateDriverForPlugAndPlayDevices returned false (Win32=$err); continuing if device exists."
    } else {
        Write-Host "UpdateDriver OK (rebootRequired=$reboot)"
    }
}

function Stop-ProcessWithTimeout {
    param([System.Diagnostics.Process]$Process, [int]$Seconds = 5)
    if (-not $Process) { return }
    $deadline = (Get-Date).AddSeconds($Seconds)
    while (-not $Process.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    if (-not $Process.HasExited) {
        Write-Host "  killing PID $($Process.Id) after ${Seconds}s timeout"
        try { Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue } catch { }
    }
}

function Uninstall-MirrorQuiet {
    # Fast path only: never enumerate all PnP devices (hangs on Win7) and never
    # block forever on mirror_probe detach.
    Write-Host "=== Uninstall existing Road Desk Mirror ==="

    $probe = Join-Path $PackageDir "mirror_probe.exe"
    if (Test-Path $probe) {
        Write-Host "[1/4] mirror_probe detach (max 5s)..."
        try {
            $p = Start-Process -FilePath $probe -ArgumentList "detach" -PassThru -WindowStyle Hidden
            Stop-ProcessWithTimeout -Process $p -Seconds 5
        } catch {
            Write-Host "  skip detach: $($_.Exception.Message)"
        }
    } else {
        Write-Host "[1/4] no mirror_probe.exe - skip detach"
    }

    Write-Host "[2/4] stop/delete services..."
    foreach ($svc in @("rdmmirror", "rdmmini")) {
        if (Test-ServiceExists $svc) {
            Write-Host "  sc stop $svc"
            $stopOut = & sc.exe stop $svc 2>&1 | Out-String
            # 1052 = service does not accept STOP (common for display miniport / PnP).
            if ($stopOut -match "1052") {
                Write-Host "  (ignore 1052: $svc does not accept stop; continue delete)"
            } elseif ($stopOut.Trim()) {
                Write-Host $stopOut.TrimEnd()
            }
            Start-Sleep -Milliseconds 300
            Write-Host "  sc delete $svc"
            & sc.exe delete $svc 2>&1 | Out-Host
        } else {
            Write-Host "  $svc not registered"
        }
    }

    Write-Host "[3/4] pnputil remove Road Desk packages (best effort)..."
    try {
        $enum = & pnputil -e 2>&1 | Out-String
        $oem = $null
        foreach ($line in ($enum -split "`r?`n")) {
            if ($line -match "(oem\d+\.inf)") {
                $oem = $Matches[1]
            }
            if ($oem -and ($line -match "Road Desk|rdm_xpdm|roaddesk_mirror|RoadDesk")) {
                Write-Host "  pnputil -f -d $oem"
                & pnputil -f -d $oem 2>&1 | Out-Host
                $oem = $null
            }
        }
    } catch {
        Write-Host "  pnputil skip: $($_.Exception.Message)"
    }

    Write-Host "[4/4] remove driver files if not locked..."
    foreach ($rel in @(
        "system32\drivers\rdmmirror.sys",
        "system32\drivers\rdmmini.sys",
        "system32\rdmdisp.dll"
    )) {
        $path = Join-Path $env:SystemRoot $rel
        if (Test-Path $path) {
            try {
                Remove-Item -Force $path -ErrorAction Stop
                Write-Host "  removed $path"
            } catch {
                Write-Host "  leave $path (in use; install will overwrite)"
            }
        }
    }
    Write-Host "Uninstall pass done."
}

function Install-Gm1 {
    $sysSrc = Join-Path $PackageDir "rdmmirror.sys"
    if (-not (Test-Path $sysSrc)) { throw "Missing $sysSrc" }
    $sysDst = Join-Path $env:SystemRoot "system32\drivers\rdmmirror.sys"
    Copy-Item -Force $sysSrc $sysDst

    if (-not (Test-ServiceExists "rdmmirror")) {
        & sc.exe create rdmmirror type= kernel start= demand error= normal `
            binPath= "\SystemRoot\system32\drivers\rdmmirror.sys" `
            DisplayName= "Road Desk Mirror" | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "sc create rdmmirror failed" }
    }
    if (-not (Test-ServiceRunning "rdmmirror")) {
        $out = & sc.exe start rdmmirror 2>&1 | Out-String
        Write-Host $out
        if ($out -match "577|INVALID_IMAGE_HASH") {
            throw "sc start rdmmirror error 577 - testsigning/cert not ready (reboot after enabling testsigning, re-run installer)"
        }
        if (-not (Test-ServiceRunning "rdmmirror") -and $out -notmatch "1056|already") {
            throw "sc start rdmmirror failed"
        }
    }
    Write-Host "GM1 OK (rdmmirror RUNNING)"
}

function Install-Gm2 {
    $mini = Join-Path $PackageDir "rdmmini.sys"
    $disp = Join-Path $PackageDir "rdmdisp.dll"
    $inf = Join-Path $PackageDir "rdm_xpdm.inf"
    foreach ($f in @($mini, $disp, $inf)) {
        if (-not (Test-Path $f)) { throw "Missing $f" }
    }
    Copy-Item -Force $mini (Join-Path $env:SystemRoot "system32\drivers\rdmmini.sys")
    Copy-Item -Force $disp (Join-Path $env:SystemRoot "system32\rdmdisp.dll")

    Write-Host "pnputil -i -a rdm_xpdm.inf ..."
    & pnputil -i -a $inf 2>&1 | Out-Host

    $displayGuid = [Guid]"4D36E968-E325-11CE-BFC1-08002BE10318"
    Write-Host "Creating root device $HwIdGm2 ..."
    Install-RootDevice -HardwareId $HwIdGm2 -InfPath $inf -ClassGuid $displayGuid `
        -Description "Road Desk Mirror Driver"
    Write-Host "GM2 staged (reboot required for EnumDisplayDevices)."
}

# -------------------- main --------------------
Ensure-Admin

Write-Host "Package: $PackageDir"
Write-Host ""

if ($DoUninstallOnly) {
    Uninstall-MirrorQuiet
    Show-Msg "Road Desk Mirror 已卸载。若驱动文件仍被占用，请重启后再删残留。"
    exit 0
}

# Required package files
foreach ($f in @(
    "rdmmirror.sys", "rdmmini.sys", "rdmdisp.dll",
    "rdm_xpdm.inf", "RoadDeskTestCert.cer"
)) {
    if (-not (Test-Path (Join-Path $PackageDir $f))) {
        Show-Msg "安装包不完整，缺少: $f`n请使用 make-package.ps1 生成的完整目录。"
        exit 1
    }
}

# Phase A: test-signing
if (-not (Get-TestSigningYes)) {
    Write-Host "testsigning is OFF - enabling (reboot required before drivers load)..."
    Enable-TestSigning
    $dir = Split-Path $PendingMarker -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Set-Content -Path $PendingMarker -Value $PackageDir -Encoding Ascii
    $ans = Show-Msg "已打开系统「测试模式」(bcdedit testsigning)。`n`n这不是在本机签名驱动——驱动应已在开发机 SignTool 签好。`n`n必须先重启，重启后请再次双击 Install-RoadDeskMirror.bat 完成安装。`n`n是否立即重启？" "Road Desk Mirror" "YesNo"
    if ($ans -eq "Yes") {
        & shutdown.exe /r /t 15 /c "Road Desk Mirror: reboot to apply testsigning"
    }
    exit 0
}

# Phase B: import cert, replace install
Write-Host "Importing test certificate..."
Import-TestCert

if (Test-MirrorInstalled) {
    Write-Host "Existing install detected - uninstalling first..."
    Uninstall-MirrorQuiet
    Start-Sleep -Seconds 1
}

Write-Host "Installing GM1..."
Install-Gm1
Write-Host "Installing GM2..."
Install-Gm2

if (Test-Path $PendingMarker) { Remove-Item -Force $PendingMarker -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "Install finished. Reboot is required."
if (-not $DoNoRebootPrompt) {
    $ans = Show-Msg "Road Desk Mirror 已安装（使用的是开发机已签名的驱动）。`n`n必须重启后才能正常使用。`nHost 请以管理员运行 host-agent.exe。`n`n是否立即重启？" "Road Desk Mirror" "YesNo"
    if ($ans -eq "Yes") {
        & shutdown.exe /r /t 15 /c "Road Desk Mirror installed - rebooting"
    }
}
exit 0
