# Test-sign rdmmirror.sys with a local self-signed cert (WDK tools).
# Produces: build\mirror-driver\driver\rdmmirror.sys (signed)
#           build\mirror-driver\driver\RoadDeskTestCert.cer (import on Win7 VM)
$ErrorActionPreference = "Stop"

$spikeRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$repoRoot = Resolve-Path (Join-Path $spikeRoot "../..")
$outDir = Join-Path $repoRoot "build\mirror-driver\driver"
$sys = Join-Path $outDir "rdmmirror.sys"
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
if (-not (Test-Path $sys)) { Write-Error "Missing $sys — run build-driver.ps1 first." }

$makecert = Join-Path $wdk "bin\amd64\MakeCert.exe"
$signtool = Join-Path $wdk "bin\amd64\SignTool.exe"
$certmgr = Join-Path $wdk "bin\amd64\CertMgr.exe"

# Create cert in CurrentUser PrivateCertStore if missing
$existing = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq "CN=$certName" }
if (-not $existing) {
    Write-Host "Creating self-signed cert CN=$certName ..."
    & $makecert -r -pe -ss PrivateCertStore -n "CN=$certName" $cer
    if ($LASTEXITCODE -ne 0) { Write-Error "makecert failed." }
} else {
    Write-Host "Reusing existing cert CN=$certName"
    if (-not (Test-Path $cer)) {
        Export-Certificate -Cert $existing[0] -FilePath $cer | Out-Null
    }
}

# Import to LocalMachine root + trustedpublisher (helps sign/verify on this PC)
& $certmgr /add $cer /s /r localMachine root 2>$null
& $certmgr /add $cer /s /r localMachine trustedpublisher 2>$null

Write-Host "Signing $sys ..."
& $signtool sign /v /s PrivateCertStore /n $certName $sys
if ($LASTEXITCODE -ne 0) { Write-Error "signtool sign failed." }

& $signtool verify /pa /v $sys
if ($LASTEXITCODE -ne 0) {
    Write-Host "verify /pa reported issues (often OK for test cert); continuing."
}

Write-Host "Signed:"
Write-Host "  $sys"
Write-Host "  $cer"
Write-Host "On Win7 VM (admin):"
Write-Host "  1) Import cer: certmgr /add RoadDeskTestCert.cer /s /r localMachine root"
Write-Host "     certmgr /add RoadDeskTestCert.cer /s /r localMachine trustedpublisher"
Write-Host "  2) copy /Y rdmmirror.sys %SystemRoot%\system32\drivers\"
Write-Host "  3) sc start rdmmirror"
Write-Host "  4) mirror_probe.exe"
