# Shared helpers for tools/mirror-install/*.ps1
$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    Resolve-Path (Join-Path $PSScriptRoot "..\..")
}

function Get-DefaultPackageDir {
    Join-Path (Get-RepoRoot) "build\mirror-package\win7-x64"
}

function Get-DefaultSpikeDriverOut {
    Join-Path (Get-RepoRoot) "build\mirror-driver\driver"
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-Admin {
    if (-not (Test-IsAdmin)) {
        Write-Error "Administrator required. Re-run from an elevated PowerShell / CMD."
        exit 1
    }
}

function Resolve-PackageDir {
    param([string]$PackageDir)
    if ($PackageDir -and $PackageDir.Trim()) {
        if (-not (Test-Path -LiteralPath $PackageDir)) {
            Write-Error "PackageDir not found: $PackageDir"
            exit 1
        }
        return (Resolve-Path -LiteralPath $PackageDir).Path
    }
    if (Test-Path "C:\rd") {
        return (Resolve-Path "C:\rd").Path
    }
    $staged = Get-DefaultPackageDir
    if (Test-Path $staged) {
        return (Resolve-Path $staged).Path
    }
    Write-Error "Package directory not found. Copy stage package to C:\rd or pass -PackageDir."
    exit 1
}

function Require-File {
    param([string]$Path, [string]$Hint)
    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Error "Missing file: $Path$(if ($Hint) { " — $Hint" })"
        exit 1
    }
}

function Invoke-Sc {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$ScArgs)
    & sc.exe @ScArgs
    return $LASTEXITCODE
}

function Write-Sc577Hint {
    Write-Host ""
    Write-Host "sc start failed with ERROR_INVALID_IMAGE_HASH (577) or similar:"
    Write-Host "  1) enable-testsigning.ps1  then REBOOT"
    Write-Host "  2) import-test-cert.ps1 -PackageDir <dir with RoadDeskTestCert.cer>"
    Write-Host "  3) use signed .sys from stage package (build-and-sign.ps1)"
    Write-Host "See spikes/mirror-driver/docs/test-signing.md"
}
