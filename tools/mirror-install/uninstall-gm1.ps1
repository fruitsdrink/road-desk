# Stop and delete GM1 control service (rdmmirror). Does not remove third-party mirrors.
param(
    [switch]$RemoveFile
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")
Assert-Admin

Write-Host "Stopping rdmmirror ..."
& sc.exe stop rdmmirror 2>&1 | Out-Null
Start-Sleep -Milliseconds 500

Write-Host "Deleting service rdmmirror ..."
& sc.exe delete rdmmirror
if ($LASTEXITCODE -ne 0) {
    Write-Host "sc delete exit $LASTEXITCODE (service may already be gone)"
}

if ($RemoveFile) {
    $sysDst = Join-Path $env:SystemRoot "system32\drivers\rdmmirror.sys"
    if (Test-Path $sysDst) {
        Remove-Item -Force $sysDst
        Write-Host "Removed $sysDst"
    }
}

Write-Host "GM1 uninstall done. (Does not touch VNC/Radmin Mirror.)"
exit 0
