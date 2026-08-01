# Build mirror_probe.exe (no WDK required).
$ErrorActionPreference = "Stop"
$spikeRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$repoRoot = Resolve-Path (Join-Path $spikeRoot "../..")
$userDir = Join-Path $spikeRoot "user"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$bt = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $bt) {
    Write-Error "MSVC with C++ tools not found."
}
$cmake = Join-Path $bt "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

$buildDir = Join-Path $repoRoot "build\mirror-driver"
cmd /c "`"$bt\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 -no_logo && `"$cmake`" -S `"$userDir`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build `"$buildDir`""
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Artifact:"
Write-Host "  $buildDir\mirror_probe.exe"
Write-Host "Usage: mirror_probe.exe   (expects \\.\RoadDeskMirror after driver install)"
