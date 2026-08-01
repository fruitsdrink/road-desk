# Enable or disable Windows test-signing (Win7 Host). Requires reboot.
param(
    [switch]$Off,
    [switch]$StatusOnly
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")
Assert-Admin

function Get-TestSigningState {
    $out = & bcdedit 2>&1 | Out-String
    if ($out -match "testsigning\s+Yes") { return "Yes" }
    if ($out -match "testsigning\s+No") { return "No" }
    return "unknown"
}

$state = Get-TestSigningState
Write-Host "Current testsigning: $state"

if ($StatusOnly) {
    if ($state -ne "Yes") { exit 2 }
    exit 0
}

if ($Off) {
    Write-Host "Disabling testsigning + nointegritychecks ..."
    & bcdedit /set testsigning off
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & bcdedit /set nointegritychecks off
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    Write-Host "Enabling testsigning + nointegritychecks ..."
    & bcdedit /set testsigning on
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & bcdedit /set nointegritychecks on
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host ""
Write-Host "*** REBOOT REQUIRED ***"
Write-Host "After reboot, confirm with:  enable-testsigning.ps1 -StatusOnly"
Write-Host "Expected: testsigning Yes"
exit 0
