# One-shot on the WDK *development* PC: build + SignTool-sign + stage.
# Signing NEVER runs on the Win7 host — only pre-signed binaries + .cer are copied there.
# Output: <repo>\build\mirror-package\win7-x64\
param(
    [switch]$SkipUser,
    # Escape hatch only if binaries were already signed in build\mirror-driver\driver.
    [switch]$SkipSign,
    # Do not bump the package version (reuse current). Bumps by default.
    [switch]$NoBump
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot

Write-Host "=== Road Desk Mirror: make-package (dev PC) ==="
Write-Host "Driver signing runs HERE with WDK SignTool — not on Win7."
Write-Host ""

if ($SkipSign) {
    Write-Host "WARN: -SkipSign set; will stage existing files (must already be signed)."
}

# Bump package version (installer + driver + INF in lockstep) unless told not to.
if ($NoBump) {
    Write-Host "WARN: -NoBump set; reusing current version."
} else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "bump-mirror-version.ps1")
    if ($LASTEXITCODE -ne 0) {
        Write-Error "bump-mirror-version failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

$buildArgs = @()
if ($SkipUser) { $buildArgs += "-SkipUser" }
if ($SkipSign) { $buildArgs += "-SkipSign" }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "build-and-sign.ps1") @buildArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "build-and-sign failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "==> build native installer (RoadDeskMirrorSetup.exe)"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "build-installer.ps1")
if ($LASTEXITCODE -ne 0) {
    Write-Error "build-installer failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "stage-package.ps1")
if ($LASTEXITCODE -ne 0) {
    Write-Error "stage-package failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

$out = Join-Path (Resolve-Path (Join-Path $here "..\..")) "build\mirror-package\win7-x64"
$exe = Join-Path $out "RoadDeskMirrorSetup.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Package missing RoadDeskMirrorSetup.exe"
    exit 1
}
$exeSize = (Get-Item $exe).Length
if ($exeSize -lt 100000) {
    Write-Host "WARN: installer size $exeSize looks small — payload may not be embedded"
}

Write-Host ""
Write-Host "========================================"
Write-Host " SINGLE-FILE INSTALLER READY"
Write-Host "========================================"
Write-Host "  $exe"
Write-Host "  size=$exeSize bytes (drivers+cert embedded; signed on this PC)"
Write-Host ""
Write-Host "Copy ONLY RoadDeskMirrorSetup.exe to the Win7 host and double-click."
Write-Host "Win7 does not need WDK / makecert / signtool / loose .sys files."
Write-Host "========================================"
exit 0
