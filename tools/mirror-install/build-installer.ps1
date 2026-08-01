# Build single-file RoadDeskMirrorSetup.exe with signed drivers embedded as RCDATA.
# Prerequisite: build-and-sign.ps1 outputs under build\mirror-driver\driver\.
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here "..\..")).Path
$src = Join-Path $here "native"
$driverDir = Join-Path $repo "build\mirror-driver\driver"
$buildDir = Join-Path $repo "build\mirror-install-native"
$outDir = Join-Path $repo "build\mirror-install"
$outExe = Join-Path $outDir "RoadDeskMirrorSetup.exe"
$payloadRc = Join-Path $buildDir "payload.rc"

$required = @(
    "rdmmirror.sys",
    "rdmmini.sys",
    "rdmdisp.dll",
    "rdm_xpdm.inf",
    "RoadDeskTestCert.cer"
)
foreach ($f in $required) {
    $p = Join-Path $driverDir $f
    if (-not (Test-Path $p)) {
        Write-Error "Missing signed payload: $p — run build-and-sign.ps1 first"
        exit 1
    }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$bt = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $bt) {
    Write-Error "Visual Studio C++ tools not found"
    exit 1
}
$cmake = Join-Path $bt "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Rc-Path([string]$Path) {
    return ($Path -replace '\\', '/')
}

$resH = Rc-Path (Join-Path $src "resource.h")
$rcLines = @(
    "#include `"$resH`""
    "IDR_RDMMIRROR_SYS RCDATA `"$(Rc-Path (Join-Path $driverDir 'rdmmirror.sys'))`""
    "IDR_RDMMINI_SYS   RCDATA `"$(Rc-Path (Join-Path $driverDir 'rdmmini.sys'))`""
    "IDR_RDMDISP_DLL   RCDATA `"$(Rc-Path (Join-Path $driverDir 'rdmdisp.dll'))`""
    "IDR_RDM_XPDM_INF  RCDATA `"$(Rc-Path (Join-Path $driverDir 'rdm_xpdm.inf'))`""
    "IDR_TEST_CERT     RCDATA `"$(Rc-Path (Join-Path $driverDir 'RoadDeskTestCert.cer'))`""
)
[System.IO.File]::WriteAllText($payloadRc, ($rcLines -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)
Write-Host "Wrote $payloadRc"

$inner = @(
    "call `"$bt\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 -no_logo"
    "if errorlevel 1 exit /b 1"
    "`"$cmake`" -S `"$src`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release `"-DROAD_DESK_PAYLOAD_RC=$payloadRc`""
    "if errorlevel 1 exit /b 1"
    "`"$cmake`" --build `"$buildDir`""
    "if errorlevel 1 exit /b 1"
) -join "`r`n"

$bat = Join-Path $buildDir "_build.bat"
[System.IO.File]::WriteAllText($bat, $inner + "`r`n", [System.Text.Encoding]::ASCII)
cmd /c "`"$bat`""
if ($LASTEXITCODE -ne 0) {
    Write-Error "RoadDeskMirrorSetup build failed"
    exit $LASTEXITCODE
}

$built = Join-Path $buildDir "RoadDeskMirrorSetup.exe"
if (-not (Test-Path $built)) {
    Write-Error "Missing $built"
    exit 1
}
Copy-Item -Force $built $outExe
$size = (Get-Item $outExe).Length
Write-Host "Built single-file installer: $outExe ($size bytes)"
exit 0
