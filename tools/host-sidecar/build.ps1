# Build Host Sidecar with Go 1.20.x (required for Win7 Hosts).
# Usage: .\build.ps1

$ErrorActionPreference = "Stop"

function Find-Go120Root {
    $candidates = @(
        $env:ROAD_DESK_GO120,
        "C:\Users\haight\.g\versions\1.20.14",
        "$env:USERPROFILE\.g\versions\1.20.14",
        "$env:USERPROFILE\sdk\go1.20.14"
    ) | Where-Object { $_ }

    foreach ($root in $candidates) {
        if (Test-Path (Join-Path $root "bin\go.exe")) {
            return $root
        }
    }

    $existing = Get-Command go -ErrorAction SilentlyContinue
    if ($existing) {
        $ver = & go version 2>$null
        if ($ver -match "go1\.20\.") {
            return (& go env GOROOT)
        }
    }
    return $null
}

$go120 = Find-Go120Root
if (-not $go120) {
    Write-Error "Go 1.20.x not found. Install 1.20.14 (e.g. g install 1.20.14) or set ROAD_DESK_GO120."
}

$env:GOROOT = $go120
$env:Path = "$(Join-Path $go120 'bin');" + $env:Path

$ver = & go version
Write-Host "Using $ver (GOROOT=$env:GOROOT)"
if ($ver -notmatch "go1\.20\.") {
    Write-Error "Refusing to build: need go1.20.x, got: $ver"
}

Push-Location $PSScriptRoot
try {
    go build -o host-sidecar.exe .
    Write-Host "Wrote $(Join-Path $PSScriptRoot 'host-sidecar.exe')"
}
finally {
    Pop-Location
}
