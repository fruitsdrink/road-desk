# Install / start GM1 control driver (rdmmirror.sys) via copy + sc.
param(
    [string]$PackageDir = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")
Assert-Admin

$pkg = Resolve-PackageDir -PackageDir $PackageDir
$sysSrc = Join-Path $pkg "rdmmirror.sys"
Require-File $sysSrc "Need signed rdmmirror.sys in package dir"

$sysDst = Join-Path $env:SystemRoot "system32\drivers\rdmmirror.sys"
Write-Host "Copy $sysSrc -> $sysDst"
Copy-Item -Force $sysSrc $sysDst

$qBefore = & sc.exe query rdmmirror 2>&1 | Out-String
# Present if query shows a STATE line (RUNNING/STOPPED/...).
$exists = $qBefore -match "STATE\s*:"

if (-not $exists) {
    Write-Host "Creating service rdmmirror ..."
    & sc.exe create rdmmirror type= kernel start= demand error= normal `
        binPath= "\SystemRoot\system32\drivers\rdmmirror.sys" `
        DisplayName= "Road Desk Mirror"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "sc create failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
} else {
    Write-Host "Service rdmmirror already registered."
}

$qNow = & sc.exe query rdmmirror 2>&1 | Out-String
if ($qNow -match "RUNNING") {
    Write-Host "Already RUNNING."
} else {
    Write-Host "Starting rdmmirror ..."
    $startOut = & sc.exe start rdmmirror 2>&1 | Out-String
    $startCode = $LASTEXITCODE
    Write-Host $startOut
    if ($startOut -match "577|INVALID_IMAGE_HASH") {
        Write-Sc577Hint
        exit 577
    }
    if ($startCode -ne 0 -and $startOut -notmatch "1056|already been started|已经运行") {
        $qFail = & sc.exe query rdmmirror 2>&1 | Out-String
        if ($qFail -notmatch "RUNNING") {
            if ($startCode -eq 577) {
                Write-Sc577Hint
                exit 577
            }
            Write-Error "sc start failed (exit $startCode). If 577: testsigning/cert."
            Write-Sc577Hint
            exit $startCode
        }
    }
}

& sc.exe query rdmmirror
Write-Host ""
Write-Host "GM1 install OK if STATE is RUNNING."
Write-Host "Optional: mirror_probe.exe  (expect GM1_LOADED ok)"
exit 0
