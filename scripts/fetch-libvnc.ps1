# Fetch LibVNCServer 0.9.15 into third_party/libvncserver (GPL, internal MVP only).
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$dest = Join-Path $root "third_party\libvncserver"
if (Test-Path (Join-Path $dest ".git")) {
    Write-Host "LibVNC already present at $dest"
    exit 0
}
New-Item -ItemType Directory -Force -Path (Join-Path $root "third_party") | Out-Null
git clone --depth 1 --branch LibVNCServer-0.9.15 https://github.com/LibVNC/libvncserver.git $dest
Write-Host "Fetched $dest"
