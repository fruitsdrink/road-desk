# Build rdmmirror.sys with WDK 7.1 (Windows 7 DDK). Fails with install hints if missing.
$ErrorActionPreference = "Stop"
$spikeRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$repoRoot = Resolve-Path (Join-Path $spikeRoot "../..")
$controlDir = Join-Path $spikeRoot "driver\control"
$outDir = Join-Path $repoRoot "build\mirror-driver\driver"
$infSrc = Join-Path $spikeRoot "driver\roaddesk_mirror.inf"

function Find-Wdk71 {
    $candidates = @()
    if ($env:DDKROOT) { $candidates += $env:DDKROOT }
    if ($env:WLHKIT) { $candidates += $env:WLHKIT }
    if ($env:BASEDDIR) { $candidates += $env:BASEDDIR }
    $candidates += @(
        "C:\WinDDK\7600.16385.1",
        "C:\WinDDK\7600.16385.0",
        "D:\WinDDK\7600.16385.1"
    )
    foreach ($c in $candidates) {
        if (-not $c) { continue }
        $setenv = Join-Path $c "bin\setenv.bat"
        if (Test-Path $setenv) { return (Resolve-Path $c).Path }
    }
    return $null
}

$wdk = Find-Wdk71
if (-not $wdk) {
    Write-Host "WDK 7.1 not found."
    Write-Host "Install Windows Driver Kit 7.1.0:"
    Write-Host "  https://www.microsoft.com/download/details.aspx?id=11800"
    Write-Host "Typical path: C:\WinDDK\7600.16385.1"
    Write-Host "Then re-run this script, or open 'x64 Free Build Environment' and:"
    Write-Host "  cd `"$controlDir`""
    Write-Host "  build -cZ"
    Write-Host "Docs: spikes\mirror-driver\docs\test-signing.md"
    exit 1
}

Write-Host "Using WDK: $wdk"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# cmd /c cannot take PowerShell newlines reliably — write a temp .bat
$bat = Join-Path $outDir "_build_rdmmirror.bat"
@(
    "@echo off"
    "setlocal"
    "call `"$wdk\bin\setenv.bat`" $wdk fre x64 WIN7"
    "if errorlevel 1 exit /b 1"
    "cd /d `"$controlDir`""
    "build -cZ"
    "exit /b %ERRORLEVEL%"
) | Set-Content -Path $bat -Encoding Ascii

Write-Host "Running: $bat"
cmd /c "`"$bat`""
$buildExit = $LASTEXITCODE
if ($buildExit -ne 0) {
    Write-Error "WDK build failed (exit $buildExit)."
}

# Locate sys from WDK obj tree (amd64)
$sys = Get-ChildItem -Path $controlDir -Filter "rdmmirror.sys" -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $sys) {
    Write-Error "rdmmirror.sys not found under $controlDir after build. Open build*.log in that folder."
}

Copy-Item -Force $sys.FullName (Join-Path $outDir "rdmmirror.sys")
Copy-Item -Force $infSrc (Join-Path $outDir "roaddesk_mirror.inf")

Write-Host "Artifacts:"
Write-Host "  $outDir\rdmmirror.sys"
Write-Host "  $outDir\roaddesk_mirror.inf"
Write-Host "Next: docs\test-signing.md + docs\gm1-checklist.md"
