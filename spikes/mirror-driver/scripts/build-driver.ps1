# Build Road Desk Mirror drivers with WDK 7.1:
#   rdmmirror.sys  (GM1 control)
#   rdmmini.sys + rdmdisp.dll (GM2 XPDM)
$ErrorActionPreference = "Stop"
$spikeRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$repoRoot = Resolve-Path (Join-Path $spikeRoot "../..")
$driverRoot = Join-Path $spikeRoot "driver"
$outDir = Join-Path $repoRoot "build\mirror-driver\driver"

function Find-Wdk71 {
    foreach ($c in @($env:DDKROOT, "C:\WinDDK\7600.16385.1", "C:\WinDDK\7600.16385.0", "D:\WinDDK\7600.16385.1")) {
        if ($c -and (Test-Path (Join-Path $c "bin\setenv.bat"))) {
            return (Resolve-Path $c).Path
        }
    }
    return $null
}

$wdk = Find-Wdk71
if (-not $wdk) {
    Write-Host "WDK 7.1 not found. See docs\test-signing.md"
    exit 1
}

Write-Host "Using WDK: $wdk"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Build-One($subdir) {
    $dir = Join-Path $driverRoot $subdir
    $bat = Join-Path $outDir ("_build_" + $subdir + ".bat")
    @(
        "@echo off"
        "setlocal"
        "call `"$wdk\bin\setenv.bat`" $wdk fre x64 WIN7"
        "if errorlevel 1 exit /b 1"
        "cd /d `"$dir`""
        "build -cZ"
        "exit /b %ERRORLEVEL%"
    ) | Set-Content -Path $bat -Encoding Ascii
    Write-Host "Building $subdir ..."
    cmd /c "`"$bat`""
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed in $subdir (exit $LASTEXITCODE)."
    }
}

Build-One "control"
Build-One "miniport"
Build-One "display"

function Find-Built($name, $root) {
    Get-ChildItem -Path $root -Filter $name -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "objfre" } |
        Select-Object -First 1
}

$ctrl = Find-Built "rdmmirror.sys" (Join-Path $driverRoot "control")
$mini = Find-Built "rdmmini.sys" (Join-Path $driverRoot "miniport")
$disp = Find-Built "rdmdisp.dll" (Join-Path $driverRoot "display")
if (-not $ctrl) { Write-Error "rdmmirror.sys not found" }
if (-not $mini) { Write-Error "rdmmini.sys not found" }
if (-not $disp) { Write-Error "rdmdisp.dll not found" }

Copy-Item -Force $ctrl.FullName (Join-Path $outDir "rdmmirror.sys")
Copy-Item -Force $mini.FullName (Join-Path $outDir "rdmmini.sys")
Copy-Item -Force $disp.FullName (Join-Path $outDir "rdmdisp.dll")
Copy-Item -Force (Join-Path $driverRoot "roaddesk_mirror.inf") (Join-Path $outDir "roaddesk_mirror.inf")
Copy-Item -Force (Join-Path $driverRoot "rdm_xpdm.inf") (Join-Path $outDir "rdm_xpdm.inf")

Write-Host "Artifacts:"
Write-Host "  $outDir\rdmmirror.sys"
Write-Host "  $outDir\rdmmini.sys"
Write-Host "  $outDir\rdmdisp.dll"
Write-Host "  $outDir\rdm_xpdm.inf"
Write-Host "Next: scripts\sign-driver.ps1"
