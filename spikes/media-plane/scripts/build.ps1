# Build LibVNC (minimal deps) + spike_host / spike_viewer.
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
Set-Location $root

& "$PSScriptRoot\fetch-libvnc.ps1"

$bt = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
if (-not (Test-Path "$bt\Common7\Tools\VsDevCmd.bat")) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $bt = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $bt) {
    Write-Error "MSVC Build Tools with C++ not found."
}
$cmake = Join-Path $bt "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) {
    $cmake = "cmake"
}

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

cmd /c "`"$bt\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 -no_logo && `"$cmake`" -S third_party/libvncserver -B build/libvncserver -G Ninja -DCMAKE_BUILD_TYPE=Release $($libvncOpts -join ' ') && `"$cmake`" --build build/libvncserver && `"$cmake`" -S spikes/media-plane -B build/spike-media -G Ninja -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build build/spike-media"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Artifacts:"
Write-Host "  build\spike-media\spike_host.exe"
Write-Host "  build\spike-media\spike_viewer.exe"
Write-Host "Password: spike  |  Host listens on :5900 by default"
