# Import RoadDeskTestCert.cer into Root + TrustedPublisher (once per machine).
param(
    [string]$PackageDir = "",
    [string]$CerPath = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")
Assert-Admin

if (-not $CerPath) {
    $pkg = Resolve-PackageDir -PackageDir $PackageDir
    $CerPath = Join-Path $pkg "RoadDeskTestCert.cer"
}
Require-File $CerPath "Run build-and-sign.ps1 / stage-package.ps1 first"

Write-Host "Importing $CerPath into Root ..."
& certutil -addstore Root $CerPath
if ($LASTEXITCODE -ne 0) {
    Write-Error "certutil Root failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Host "Importing $CerPath into TrustedPublisher ..."
& certutil -addstore TrustedPublisher $CerPath
if ($LASTEXITCODE -ne 0) {
    Write-Error "certutil TrustedPublisher failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Host "OK: test cert imported."
exit 0
