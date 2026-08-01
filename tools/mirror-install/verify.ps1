# Verify GM1/GM2 install state on Win7 Host.
param(
    [string]$PackageDir = "",
    [switch]$SkipProbe,
    [int]$Gm2Seconds = 0
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")

$failed = $false

function Show-Service([string]$Name) {
    Write-Host "--- sc query $Name ---"
    $out = & sc.exe query $Name 2>&1 | Out-String
    Write-Host $out
    if ($out -match "RUNNING") {
        Write-Host "[OK] $Name RUNNING"
        return $true
    }
    Write-Host "[FAIL] $Name not RUNNING"
    return $false
}

if (-not (Show-Service "rdmmirror")) { $failed = $true }
if (-not (Show-Service "rdmmini")) { $failed = $true }

$mini = Join-Path $env:SystemRoot "system32\drivers\rdmmini.sys"
$disp = Join-Path $env:SystemRoot "system32\rdmdisp.dll"
$ctrl = Join-Path $env:SystemRoot "system32\drivers\rdmmirror.sys"
foreach ($p in @($ctrl, $mini, $disp)) {
    if (Test-Path $p) {
        Write-Host "[OK] $p"
    } else {
        Write-Host "[FAIL] missing $p"
        $failed = $true
    }
}

$ts = & bcdedit 2>&1 | Out-String
if ($ts -match "testsigning\s+Yes") {
    Write-Host "[OK] testsigning Yes"
} else {
    Write-Host "[WARN] testsigning not Yes — signed drivers may fail to load (577)"
}

if (-not $SkipProbe) {
    $pkg = $null
    try { $pkg = Resolve-PackageDir -PackageDir $PackageDir } catch { }
    $probe = $null
    if ($pkg) {
        $c = Join-Path $pkg "mirror_probe.exe"
        if (Test-Path $c) { $probe = $c }
    }
    if (-not $probe -and (Test-Path ".\mirror_probe.exe")) {
        $probe = (Resolve-Path ".\mirror_probe.exe").Path
    }
    if ($probe) {
        Write-Host "--- mirror_probe (GM1) ---"
        & $probe 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed = $true }

        Write-Host "--- mirror_probe list ---"
        & $probe list 2>&1 | Out-Host

        if ($Gm2Seconds -gt 0) {
            Write-Host "--- mirror_probe gm2 $Gm2Seconds (drag a window) ---"
            & $probe gm2 $Gm2Seconds 2>&1 | Out-Host
            if ($LASTEXITCODE -ne 0) { $failed = $true }
        }
    } else {
        Write-Host "[WARN] mirror_probe.exe not found — skip probe checks"
    }
}

Write-Host ""
if ($failed) {
    Write-Host "verify: FAILED (see above)"
    exit 1
}
Write-Host "verify: OK — next: run host-agent.exe as Administrator; host-agent.log should show capture=mirror"
exit 0
