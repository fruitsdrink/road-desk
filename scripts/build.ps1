# Build LibVNC (minimal, /MD) + product host-agent / viewer. Requires MSVC + CMake + Ninja.
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

& "$PSScriptRoot\fetch-libvnc.ps1"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$bt = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $bt) {
    Write-Error "MSVC with C++ tools not found. Install VS Build Tools workload VCTools."
}
$cmake = Join-Path $bt "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

# Match spike_host: /MD + LibVNC default CRT (Win7 machine already runs spike_host).
$libvncOpts = @(
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
    "-DWITH_JPEG=OFF", "-DWITH_PNG=OFF", "-DWITH_ZLIB=OFF", "-DWITH_LZO=OFF",
    "-DWITH_OPENSSL=OFF", "-DWITH_GNUTLS=OFF", "-DWITH_GCRYPT=OFF",
    "-DWITH_SDL=OFF", "-DWITH_GTK=OFF", "-DWITH_QT=OFF", "-DWITH_FFMPEG=OFF",
    "-DWITH_SASL=OFF", "-DWITH_WEBSOCKETS=OFF", "-DWITH_LIBSSHTUNNEL=OFF",
    "-DWITH_SYSTEMD=OFF", "-DWITH_XCB=OFF", "-DWITH_EXAMPLES=OFF", "-DWITH_TESTS=OFF",
    "-DBUILD_SHARED_LIBS=OFF", "-DLIBVNCSERVER_INSTALL=OFF",
    "-DWITH_TIGHTVNC_FILETRANSFER=OFF", "-DPREFER_WIN32THREADS=ON"
)

$productOpts = @(
    "-DROAD_DESK_WITH_LIBVNC=ON",
    "-DROAD_DESK_STATIC_CRT=OFF"
)

# If a failed /MT LibVNC tree is present, wipe it (objects reference __imp_ CRT).
if (Test-Path "build\libvncserver\CMakeCache.txt") {
    $cache = Get-Content "build\libvncserver\CMakeCache.txt" -Raw
    if ($cache -match "CMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded([^\n]*)$" -and
        $cache -notmatch "MultiThreadedDLL") {
        Write-Host "Wiping /MT LibVNC tree for /MD rebuild"
        Remove-Item -Recurse -Force "build\libvncserver"
    }
}

cmd /c "`"$bt\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 -no_logo && `"$cmake`" -S third_party/libvncserver -B build/libvncserver -G Ninja -DCMAKE_BUILD_TYPE=Release $($libvncOpts -join ' ') && `"$cmake`" --build build/libvncserver && `"$cmake`" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release $($productOpts -join ' ') && `"$cmake`" --build build"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Artifacts:"
Write-Host "  build\src\agent\host-agent.exe"
Write-Host "  build\src\viewer\viewer.exe"
Write-Host "Default PSK: road-desk  |  Host: host-agent.exe [port] [password]"
Write-Host "Viewer: viewer.exe host:port [password] [tls_sha256_fingerprint]"
Write-Host "Env: ROAD_DESK_PSK, ROAD_DESK_TLS_FINGERPRINT, ROAD_DESK_TLS_INSECURE=1 (debug), ROAD_DESK_ALLOW_PLAINTEXT=1 (debug)"
