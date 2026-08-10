# Bump Road Desk Mirror package version (installer + driver + INF) by one patch.
# Version source of truth: mirror_version.txt (e.g. "10.0.2").
# Updates in lockstep:
#   - native/version.rc   (RoadDeskMirrorSetup.exe file/product version)
#   - driver/rdm_xpdm.inf (DriverVer date,version)
#   - driver/display/mirror.rc (rdmdisp.dll VER_*)
param(
  [string]$Stamp = ""
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$verFile = Join-Path $here "mirror_version.txt"

if (-not (Test-Path $verFile)) {
  throw "Version file not found: $verFile"
}
$cur = (Get-Content -Raw $verFile).Trim()
if ($cur -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
  throw "Unrecognized version format: '$cur'"
}
$major = [int]$Matches[1]
$minor = [int]$Matches[2]
$patch = [int]$Matches[3] + 1
$ver = "$major.$minor.$patch"
$verComma = "$major,$minor,$patch,0"
$today = Get-Date -Format "MM/dd/yyyy"

# 1) mirror_version.txt
[System.IO.File]::WriteAllText($verFile, $ver + "`n", (New-Object System.Text.UTF8Encoding $false))

# 2) native/version.rc (installer EXE version resource)
$rcPath = Join-Path $here "native\version.rc"
$rc = Get-Content -Raw $rcPath
$rc = [regex]::Replace($rc, 'FILEVERSION [0-9,]+', "FILEVERSION $verComma")
$rc = [regex]::Replace($rc, 'PRODUCTVERSION [0-9,]+', "PRODUCTVERSION $verComma")
$rc = [regex]::Replace($rc, 'VALUE "FileVersion", "[^"]*"', "VALUE `"FileVersion`", `"$ver`"")
$rc = [regex]::Replace($rc, 'VALUE "ProductVersion", "[^"]*"', "VALUE `"ProductVersion`", `"$ver`"")
[System.IO.File]::WriteAllText($rcPath, $rc, (New-Object System.Text.UTF8Encoding $false))

# 3) driver/rdm_xpdm.inf (DriverVer date,version)
$infPath = Join-Path $here "..\..\spikes\mirror-driver\driver\rdm_xpdm.inf"
$inf = Get-Content -Raw $infPath
$inf = [regex]::Replace($inf, 'DriverVer\s*=\s*[0-9/]+,[0-9.]+', "DriverVer   = $today,$ver")
[System.IO.File]::WriteAllText($infPath, $inf, (New-Object System.Text.UTF8Encoding $false))

# 4) driver/display/mirror.rc (rdmdisp.dll version resource)
$mrc = Join-Path $here "..\..\spikes\mirror-driver\driver\display\mirror.rc"
$m = Get-Content -Raw $mrc
$m = [regex]::Replace($m, 'VER_PRODUCTVERSION\s+[0-9,]+', "VER_PRODUCTVERSION          $verComma")
$m = [regex]::Replace($m, 'VER_PRODUCTVERSION_STR\s+"[^"]*"', "VER_PRODUCTVERSION_STR      `"$ver`"")
$m = [regex]::Replace($m, 'VER_FILEVERSION\s+[0-9,]+', "VER_FILEVERSION             $verComma")
$m = [regex]::Replace($m, 'VER_FILEVERSION_STR\s+"[^"]*"', "VER_FILEVERSION_STR         `"$ver`"")
[System.IO.File]::WriteAllText($mrc, $m, (New-Object System.Text.UTF8Encoding $false))

if ($Stamp) {
  $dir = Split-Path -Parent $Stamp
  if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
  }
  [System.IO.File]::WriteAllText($Stamp, "RDM_MIRROR_VERSION=$ver`n", (New-Object System.Text.UTF8Encoding $false))
}

Write-Host "Bumped mirror version -> $ver"
