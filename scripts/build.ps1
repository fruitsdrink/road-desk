# Build product host-agent / viewer (mux media plane, no LibVNC). Requires MSVC + CMake + Ninja.
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$bt = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $bt) {
    Write-Error "MSVC with C++ tools not found. Install VS Build Tools workload VCTools."
}
$cmake = Join-Path $bt "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

$productOpts = @(
    "-DROAD_DESK_WITH_MEDIA=ON",
    "-DROAD_DESK_STATIC_CRT=OFF"
)

cmd /c "`"$bt\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 -no_logo && `"$cmake`" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release $($productOpts -join ' ') && `"$cmake`" --build build"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Artifacts:"
Write-Host "  build\src\agent\host-agent.exe"
Write-Host "  build\src\viewer\viewer.exe"
Write-Host "Default PSK: road-desk  |  Default port: 38471 (not VNC 5900)"
Write-Host "Host: host-agent.exe [port] [password] [--no-gateway]"
Write-Host "Viewer: viewer.exe  (console; gateway dir or --demo-book)"
Write-Host "        viewer.exe host:port [password] [tls_sha256_fingerprint]  (direct window)"
Write-Host "Env: ROAD_DESK_PSK, ROAD_DESK_TLS_FINGERPRINT, ROAD_DESK_TLS_INSECURE=1 (debug),"
Write-Host "     ROAD_DESK_DEMO_HOST / ROAD_DESK_DEMO_PORT (console demo target),"
Write-Host "     ROAD_DESK_GATEWAY_URL / ROAD_DESK_VIEWER_KEY (viewer directory),"
Write-Host "     ROAD_DESK_CAPTURE=auto|mirror|gdi (Host Mirror/GDI)"
Write-Host "Gateway: docs/gateway.md (tools/gateway + admin)"
Write-Host "Note: Mirror Host needs Administrator (HKLM Attach.ToDesktop scrub)."
