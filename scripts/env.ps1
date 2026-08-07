# dot-source this in PowerShell to set MSVC build env for the current terminal.
# Usage:  . .\scripts\env.ps1
#         cmake --build build --config Release

$msvcRoot = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231"
$sdkRoot  = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer   = "10.0.26100.0"

$env:INCLUDE = @(
    "$msvcRoot\include",
    "$sdkRoot\Include\$sdkVer\ucrt",
    "$sdkRoot\Include\$sdkVer\shared",
    "$sdkRoot\Include\$sdkVer\um",
    "$sdkRoot\Include\$sdkVer\winrt",
    "$sdkRoot\Include\$sdkVer\cppwinrt"
) -join ";"

$env:LIB = @(
    "$msvcRoot\lib\x64",
    "$sdkRoot\Lib\$sdkVer\um\x64",
    "$sdkRoot\Lib\$sdkVer\ucrt\x64"
) -join ";"

Write-Host "MSVC build env set for this terminal. cmake --build build --config Release"
