# Stage GM2 XPDM files + pnputil; Device Manager wizard + reboot still required.
param(
    [string]$PackageDir = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")
Assert-Admin

$pkg = Resolve-PackageDir -PackageDir $PackageDir
$mini = Join-Path $pkg "rdmmini.sys"
$disp = Join-Path $pkg "rdmdisp.dll"
$inf = Join-Path $pkg "rdm_xpdm.inf"
Require-File $mini "Need signed rdmmini.sys"
Require-File $disp "Need signed rdmdisp.dll"
Require-File $inf "Need rdm_xpdm.inf"

$dstMini = Join-Path $env:SystemRoot "system32\drivers\rdmmini.sys"
$dstDisp = Join-Path $env:SystemRoot "system32\rdmdisp.dll"
Write-Host "Copy $mini -> $dstMini"
Copy-Item -Force $mini $dstMini
Write-Host "Copy $disp -> $dstDisp"
Copy-Item -Force $disp $dstDisp

Write-Host "pnputil -i -a $inf  (adds driver package to store; may NOT create display adapter node)"
& pnputil -i -a $inf
Write-Host "pnputil exit: $LASTEXITCODE"

Write-Host ""
Write-Host "================================================================"
Write-Host " MANUAL STEP REQUIRED — Device Manager (pnputil alone is not enough)"
Write-Host "================================================================"
Write-Host "1. Open Device Manager"
Write-Host "2. Action -> Add legacy hardware"
Write-Host "3. Next -> Install the hardware that I manually select from a list -> Next"
Write-Host "4. Display adapters -> Next"
Write-Host "5. Have Disk -> browse to:"
Write-Host "     $inf"
Write-Host "6. Select 'Road Desk Mirror Driver' -> Next -> Finish"
Write-Host "   (If unsigned warning: Install anyway)"
Write-Host "7. Confirm under Display adapters: Road Desk Mirror Driver (+ your GPU)"
Write-Host ""
Write-Host "*** REBOOT REQUIRED ***"
Write-Host "After reboot:  sc query rdmmini"
Write-Host "               mirror_probe.exe list"
Write-Host "               mirror_probe.exe gm2 10   (drag a window)"
Write-Host ""
Write-Host "Do NOT uninstall third-party VNC/Radmin Mirror."
Write-Host "INF defaults Attach.ToDesktop=0 (see coexistence.md)."
Write-Host "Host-agent must run as Administrator for Mirror scrub."
exit 0
