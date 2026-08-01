# Guidance + optional file cleanup for GM2 XPDM Mirror. Detach first if probe present.
param(
    [string]$PackageDir = "",
    [switch]$RemoveFiles
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")
Assert-Admin

$pkg = $null
try { $pkg = Resolve-PackageDir -PackageDir $PackageDir } catch { $pkg = $null }

$probe = $null
if ($pkg) {
    $cand = Join-Path $pkg "mirror_probe.exe"
    if (Test-Path $cand) { $probe = $cand }
}
if (-not $probe) {
    $cand2 = Join-Path (Get-Location) "mirror_probe.exe"
    if (Test-Path $cand2) { $probe = $cand2 }
}

if ($probe) {
    Write-Host "Detaching mirror via $probe ..."
    & $probe detach 2>&1 | Out-Host
} else {
    Write-Host "mirror_probe.exe not found — skip detach (ok if never attached)."
}

Write-Host ""
Write-Host "================================================================"
Write-Host " MANUAL UNINSTALL — Device Manager / pnputil"
Write-Host "================================================================"
Write-Host "1. Device Manager -> Display adapters -> uninstall 'Road Desk Mirror Driver'"
Write-Host "   OR:"
Write-Host "     pnputil -e"
Write-Host "     pnputil -f -d oem##.inf   (Road Desk / rdm_xpdm entry)"
Write-Host "2. Reboot if prompted."
Write-Host "Does NOT remove third-party VNC/Radmin Mirror."
Write-Host ""

if ($RemoveFiles) {
    $dstMini = Join-Path $env:SystemRoot "system32\drivers\rdmmini.sys"
    $dstDisp = Join-Path $env:SystemRoot "system32\rdmdisp.dll"
    foreach ($p in @($dstMini, $dstDisp)) {
        if (Test-Path $p) {
            try {
                Remove-Item -Force $p
                Write-Host "Removed $p"
            } catch {
                Write-Host "Could not remove $p (in use?) — reboot then delete."
            }
        }
    }
}

exit 0
