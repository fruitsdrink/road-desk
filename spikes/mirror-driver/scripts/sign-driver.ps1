# Test-sign GM1 + GM2 driver binaries.
$ErrorActionPreference = "Stop"

$spikeRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$repoRoot = Resolve-Path (Join-Path $spikeRoot "../..")
$outDir = Join-Path $repoRoot "build\mirror-driver\driver"
$cer = Join-Path $outDir "RoadDeskTestCert.cer"
$certName = "RoadDeskTestCert"

function Find-Wdk71 {
    foreach ($c in @($env:DDKROOT, "C:\WinDDK\7600.16385.1", "C:\WinDDK\7600.16385.0")) {
        if ($c -and (Test-Path (Join-Path $c "bin\amd64\SignTool.exe"))) {
            return (Resolve-Path $c).Path
        }
    }
    return $null
}

$wdk = Find-Wdk71
if (-not $wdk) { Write-Error "WDK 7.1 not found (need SignTool)." }

$makecert = Join-Path $wdk "bin\amd64\MakeCert.exe"
$signtool = Join-Path $wdk "bin\amd64\SignTool.exe"

$files = @(
    (Join-Path $outDir "rdmmirror.sys"),
    (Join-Path $outDir "rdmmini.sys"),
    (Join-Path $outDir "rdmdisp.dll")
)
foreach ($f in $files) {
    if (-not (Test-Path $f)) { Write-Error "Missing $f — run build-driver.ps1 first." }
}

if (-not (Test-Path $cer)) {
    Write-Host "Creating self-signed cert CN=$certName ..."
    & $makecert -r -pe -ss PrivateCertStore -n "CN=$certName" $cer
    if ($LASTEXITCODE -ne 0) { Write-Error "makecert failed." }
}

foreach ($f in $files) {
    Write-Host "Signing $f ..."
    # /a picks a cert when multiple RoadDeskTestCert entries exist in the store.
    & $signtool sign /v /a /s PrivateCertStore /n $certName $f
    if ($LASTEXITCODE -ne 0) { Write-Error "signtool failed on $f" }
}

Write-Host "Signed binaries + cert:"
Write-Host "  $outDir\rdmmirror.sys"
Write-Host "  $outDir\rdmmini.sys"
Write-Host "  $outDir\rdmdisp.dll"
Write-Host "  $cer"
Write-Host "Win7: import cer, install rdm_xpdm.inf (XPDM), see docs\gm2-checklist.md"
