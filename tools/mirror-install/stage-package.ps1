# Stage customer deliverable: single-file RoadDeskMirrorSetup.exe (+ short readme).
param(
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")

$repo = Get-RepoRoot
if (-not $OutDir) { $OutDir = Get-DefaultPackageDir }

$setup = Join-Path $repo "build\mirror-install\RoadDeskMirrorSetup.exe"
Require-File $setup "Run build-installer.ps1 (via make-package.ps1) first"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Wipe previous multi-file package leftovers so the folder is clearly single-file.
Get-ChildItem -Force $OutDir | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

Copy-Item -Force $setup (Join-Path $OutDir "RoadDeskMirrorSetup.exe")

$installMd = Join-Path $PSScriptRoot "INSTALL-WIN7.md"
if (Test-Path $installMd) {
    Copy-Item -Force $installMd (Join-Path $OutDir "INSTALL-WIN7.md")
}

$sha = "unknown"
try {
    Push-Location $repo
    $sha = (git rev-parse --short HEAD 2>$null)
    if (-not $sha) { $sha = "unknown" }
} finally {
    Pop-Location
}

$ver = @"
road-desk mirror single-file installer (win7-x64)
git=$sha
date=$(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
file=RoadDeskMirrorSetup.exe
note=Drivers and RoadDeskTestCert.cer are embedded; copy only the EXE to the host.
"@
Set-Content -Path (Join-Path $OutDir "VERSION.txt") -Value $ver -Encoding Ascii

Write-Host "Staged single-file package:"
Write-Host "  $OutDir"
Get-ChildItem $OutDir | ForEach-Object { Write-Host ("  {0,12} {1}" -f $_.Length, $_.Name) }
Write-Host ""
Write-Host "Copy ONLY RoadDeskMirrorSetup.exe to the Win7 host and double-click it."
