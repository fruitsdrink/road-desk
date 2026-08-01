# Stage a field-deployable win7-x64 Mirror package from build\mirror-driver\.
param(
    [string]$OutDir = "",
    [string]$SourceDriverDir = "",
    [string]$SourceProbe = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_common.ps1")

$repo = Get-RepoRoot
if (-not $OutDir) { $OutDir = Get-DefaultPackageDir }
if (-not $SourceDriverDir) { $SourceDriverDir = Get-DefaultSpikeDriverOut }
if (-not $SourceProbe) {
    $SourceProbe = Join-Path $repo "build\mirror-driver\mirror_probe.exe"
}

$required = @(
    "rdmmirror.sys",
    "rdmmini.sys",
    "rdmdisp.dll",
    "roaddesk_mirror.inf",
    "rdm_xpdm.inf",
    "RoadDeskTestCert.cer"
)

foreach ($f in $required) {
    Require-File (Join-Path $SourceDriverDir $f) "Run build-and-sign.ps1 first"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($f in $required) {
    Copy-Item -Force (Join-Path $SourceDriverDir $f) (Join-Path $OutDir $f)
}

if (Test-Path $SourceProbe) {
    Copy-Item -Force $SourceProbe (Join-Path $OutDir "mirror_probe.exe")
} else {
    Write-Host "WARN: mirror_probe.exe not found at $SourceProbe (optional for deploy)"
}

$sha = ""
try {
    Push-Location $repo
    $sha = (git rev-parse --short HEAD 2>$null)
    if (-not $sha) { $sha = "unknown" }
} finally {
    Pop-Location
}
$ver = @"
road-desk mirror package (win7-x64)
git=$sha
date=$(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
source=$SourceDriverDir
"@
Set-Content -Path (Join-Path $OutDir "VERSION.txt") -Value $ver -Encoding Ascii

# Bundle installer entrypoints. Force encodings Win7 CMD/PS2 accept:
#   .bat  = ASCII + CRLF (LF-only bats eat leading chars on Win7 CMD)
#   .ps1  = UTF-8 with BOM + CRLF (no-BOM UTF-8 breaks Chinese Win7 PS2 parse)
function Copy-Win7Script([string]$Name, [string]$DestDir) {
    $src = Join-Path $PSScriptRoot $Name
    if (-not (Test-Path $src)) {
        Write-Host "WARN: missing bundle file $Name"
        return
    }
    $dst = Join-Path $DestDir $Name
    $raw = [System.IO.File]::ReadAllText($src)
    $raw = $raw -replace "`r`n", "`n" -replace "`r", "`n"
    $raw = $raw -replace "`n", "`r`n"
    if ($Name -like "*.bat") {
        [System.IO.File]::WriteAllText($dst, $raw, [System.Text.Encoding]::ASCII)
    } elseif ($Name -like "*.ps1") {
        $utf8Bom = New-Object System.Text.UTF8Encoding $true
        [System.IO.File]::WriteAllText($dst, $raw, $utf8Bom)
    } else {
        Copy-Item -Force $src $dst
    }
}

foreach ($f in @(
    "Install-RoadDeskMirror.bat",
    "Install-RoadDeskMirror.ps1",
    "Uninstall-RoadDeskMirror.bat",
    "INSTALL-WIN7.md"
)) {
    if ($f -like "*.md") {
        $src = Join-Path $PSScriptRoot $f
        if (Test-Path $src) { Copy-Item -Force $src (Join-Path $OutDir $f) }
    } else {
        Copy-Win7Script $f $OutDir
    }
}

Write-Host "Staged package:"
Write-Host "  $OutDir"
Get-ChildItem $OutDir | ForEach-Object { Write-Host ("  {0,12} {1}" -f $_.Length, $_.Name) }
Write-Host ""
Write-Host "Copy this folder to the Win7 host, then double-click Install-RoadDeskMirror.bat"
