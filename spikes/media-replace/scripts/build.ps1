# Build Track P media-replace spike (no LibVNC).
$ErrorActionPreference = "Stop"
$spikeRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$repoRoot = Resolve-Path (Join-Path $spikeRoot "../..")
Set-Location $spikeRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$bt = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $bt) {
    Write-Error "MSVC with C++ tools not found."
}
$cmake = Join-Path $bt "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

$buildDir = Join-Path $repoRoot "build\media-replace"
cmd /c "`"$bt\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 -no_logo && `"$cmake`" -S `"$spikeRoot`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build `"$buildDir`""
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Artifacts:"
Write-Host "  $buildDir\replace_host.exe"
Write-Host "  $buildDir\replace_viewer.exe"
Write-Host "Logs (overwrite each start, next to exe):"
Write-Host "  $buildDir\replace_host.log"
Write-Host "  $buildDir\replace_viewer.log"
Write-Host "Host:  replace_host.exe [port=5902] [password=road-desk]"
Write-Host "Viewer: replace_viewer.exe host:port [password] [tls_sha256]"
Write-Host "Env: ROAD_DESK_PSK, ROAD_DESK_TLS_FINGERPRINT, ROAD_DESK_TLS_INSECURE=1"
