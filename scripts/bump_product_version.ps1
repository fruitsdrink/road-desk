# Increment ROAD_DESK_VERSION_PATCH in src/product_version.h and refresh VERSION_STRING.
# Used by the viewer CMake target when viewer sources change.
param(
  [string]$Stamp = "",
  [string]$Header = ""
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $Header) {
  $Header = Join-Path $root "src\product_version.h"
}
if (-not (Test-Path $Header)) {
  throw "Version header not found: $Header"
}

$text = Get-Content -Raw -Encoding utf8 $Header
if ($text -notmatch 'ROAD_DESK_VERSION_MAJOR\s+(\d+)') {
  throw "MAJOR not found in $Header"
}
$major = [int]$Matches[1]
if ($text -notmatch 'ROAD_DESK_VERSION_MINOR\s+(\d+)') {
  throw "MINOR not found in $Header"
}
$minor = [int]$Matches[1]
if ($text -notmatch 'ROAD_DESK_VERSION_PATCH\s+(\d+)') {
  throw "PATCH not found in $Header"
}
$patch = [int]$Matches[1] + 1
$ver = "$major.$minor.$patch"

$text2 = [regex]::Replace($text, 'ROAD_DESK_VERSION_PATCH\s+\d+', "ROAD_DESK_VERSION_PATCH $patch")
$text2 = [regex]::Replace($text2, 'ROAD_DESK_VERSION_STRING\s+"[^"]*"',
  "ROAD_DESK_VERSION_STRING `"$ver`"")
# Preserve UTF-8 without BOM for MSVC friendliness.
$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($Header, $text2, $utf8)

if ($Stamp) {
  $dir = Split-Path -Parent $Stamp
  if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
  }
  [System.IO.File]::WriteAllText($Stamp, "ROAD_DESK_VERSION_STRING=$ver`n", $utf8)
}

Write-Host "Bumped product version -> $ver"
