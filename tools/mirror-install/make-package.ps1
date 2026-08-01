# One-shot on the WDK *development* PC: build + SignTool-sign + stage.
# Signing NEVER runs on the Win7 host — only pre-signed binaries + .cer are copied there.
# Output: <repo>\build\mirror-package\win7-x64\
param(
    [switch]$SkipUser,
    # Escape hatch only if binaries were already signed in build\mirror-driver\driver.
    [switch]$SkipSign
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot

Write-Host "=== Road Desk Mirror: make-package (dev PC) ==="
Write-Host "Driver signing runs HERE with WDK SignTool — not on Win7."
Write-Host ""

if ($SkipSign) {
    Write-Host "WARN: -SkipSign set; will stage existing files (must already be signed)."
}

$buildArgs = @()
if ($SkipUser) { $buildArgs += "-SkipUser" }
if ($SkipSign) { $buildArgs += "-SkipSign" }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "build-and-sign.ps1") @buildArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "build-and-sign failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "stage-package.ps1")
if ($LASTEXITCODE -ne 0) {
    Write-Error "stage-package failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

$out = Join-Path (Resolve-Path (Join-Path $here "..\..")) "build\mirror-package\win7-x64"
$cer = Join-Path $out "RoadDeskTestCert.cer"
if (-not (Test-Path $cer)) {
    Write-Error "Package missing RoadDeskTestCert.cer — signing did not run. Do not use -SkipSign unless already signed."
    exit 1
}

Write-Host ""
Write-Host "========================================"
Write-Host " FINAL PACKAGE READY (signed on this PC)"
Write-Host "========================================"
Write-Host "  $out"
Write-Host ""
Write-Host "Win7 host only:"
Write-Host "  - import RoadDeskTestCert.cer"
Write-Host "  - enable OS testsigning mode (bcdedit)"
Write-Host "  - install the already-signed .sys/.dll"
Write-Host "  (no makecert / signtool on Win7)"
Write-Host ""
Write-Host "Copy the whole folder to Win7, then double-click:"
Write-Host "  Install-RoadDeskMirror.bat"
Write-Host "========================================"
exit 0
