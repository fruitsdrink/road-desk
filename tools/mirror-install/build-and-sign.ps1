# Build + SignTool-sign Mirror drivers on the WDK *development* machine only.
# Do not run this on the Win7 deploy host.
param(
    [switch]$SkipUser,
    [switch]$SkipSign
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")

$repo = Get-RepoRoot
$spikeScripts = Join-Path $repo "spikes\mirror-driver\scripts"

function Invoke-SpikeScript([string]$Name) {
    $path = Join-Path $spikeScripts $Name
    if (-not (Test-Path $path)) {
        Write-Error "Missing spike script: $path"
        exit 1
    }
    Write-Host "==> $Name"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $path
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$Name failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

Invoke-SpikeScript "build-driver.ps1"

if (-not $SkipSign) {
    Invoke-SpikeScript "sign-driver.ps1"
} else {
    Write-Host "Skipping sign-driver.ps1 (-SkipSign)"
}

if (-not $SkipUser) {
    Invoke-SpikeScript "build-user.ps1"
} else {
    Write-Host "Skipping build-user.ps1 (-SkipUser)"
}

Write-Host ""
Write-Host "Done. Next: stage-package.ps1"
Write-Host "  Outputs under: $(Join-Path $repo 'build\mirror-driver')"
