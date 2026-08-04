# Pull Host Agent session recordings + logs via debug HTTP (media_port+1).
# Usage:
#   $env:ROAD_DESK_PSK = '...'
#   .\scripts\pull_agent_debug.ps1
#   .\scripts\pull_agent_debug.ps1 -Inventory .cursor\debug-agents.json -Id win7,win10
#   .\scripts\pull_agent_debug.ps1 -ListOnly

param(
  [string]$Inventory = "",
  [string[]]$Id = @(),
  [string]$OutDir = "",
  [switch]$ListOnly,
  [switch]$AllFiles
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Inventory) {
  $Inventory = Join-Path $repoRoot ".cursor\debug-agents.json"
  if (-not (Test-Path $Inventory)) {
    $Inventory = Join-Path $repoRoot ".cursor\debug-agents.example.json"
  }
}
if (-not (Test-Path $Inventory)) {
  throw "Inventory not found: $Inventory (copy .cursor/debug-agents.example.json)"
}

$inv = Get-Content -Raw -Encoding UTF8 $Inventory | ConvertFrom-Json
if (-not $OutDir) {
  if ($inv.out_dir) {
    $OutDir = Join-Path $repoRoot $inv.out_dir
  } else {
    $OutDir = Join-Path $repoRoot "build\debug-pull"
  }
}

function Resolve-Psk([object]$agent) {
  if ($agent.psk -and [string]$agent.psk) { return [string]$agent.psk }
  $envName = if ($agent.psk_env) { [string]$agent.psk_env } else { "ROAD_DESK_PSK" }
  $v = [Environment]::GetEnvironmentVariable($envName)
  if (-not $v) { throw "PSK missing for agent $($agent.id): set env $envName or agent.psk" }
  return $v
}

function Debug-Port([object]$agent) {
  if ($agent.debug_port) { return [int]$agent.debug_port }
  $mp = if ($agent.media_port) { [int]$agent.media_port } else { 38471 }
  return $mp + 1
}

function Invoke-AgentGet([string]$base, [string]$path, [string]$psk, [string]$outFile = $null) {
  $uri = "$base$path"
  $headers = @{ Authorization = "Bearer $psk" }
  if ($outFile) {
    Invoke-WebRequest -Uri $uri -Headers $headers -OutFile $outFile -UseBasicParsing | Out-Null
    return $null
  }
  $resp = Invoke-WebRequest -Uri $uri -Headers $headers -UseBasicParsing
  return $resp.Content
}

$agents = @($inv.agents)
if ($Id.Count -gt 0) {
  $want = [System.Collections.Generic.HashSet[string]]::new([string[]]$Id)
  $agents = $agents | Where-Object { $want.Contains([string]$_.id) }
  if (-not $agents) { throw "No agents matched -Id $($Id -join ',')" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$batchDir = Join-Path $OutDir $stamp
New-Item -ItemType Directory -Force -Path $batchDir | Out-Null

$summary = @()
foreach ($agent in $agents) {
  $id = [string]$agent.id
  $hostName = [string]$agent.host
  $port = Debug-Port $agent
  $psk = Resolve-Psk $agent
  $base = "http://${hostName}:${port}"
  $agentDir = Join-Path $batchDir $id
  New-Item -ItemType Directory -Force -Path $agentDir | Out-Null

  Write-Host "==> $id ($($agent.label)) $base"
  try {
    $health = Invoke-AgentGet $base "/v1/debug/health" $psk
    Set-Content -Path (Join-Path $agentDir "health.json") -Value $health -Encoding UTF8
    $ok = $false
    $ver = "?"
    $built = "?"
    if ($health -eq "ok") {
      $ok = $true
      $ver = "(legacy plain ok)"
    } else {
      try {
        $hj = $health | ConvertFrom-Json
        if ($hj.ok -eq $true -or $hj.version) { $ok = $true }
        if ($hj.version) { $ver = [string]$hj.version }
        if ($hj.built) { $built = [string]$hj.built }
      } catch { }
    }
    if (-not $ok) { throw "health=$health" }
    Write-Host "  version=$ver built=$built"
  } catch {
    Write-Warning "SKIP $id : $_"
    $summary += [pscustomobject]@{ id = $id; ok = $false; error = "$_" }
    continue
  }

  if ($ListOnly) {
    $list = Invoke-AgentGet $base "/v1/debug/list" $psk
    Set-Content -Path (Join-Path $agentDir "list.json") -Value $list -Encoding UTF8
    Write-Host $list
    $summary += [pscustomobject]@{ id = $id; ok = $true; list = $true }
    continue
  }

  $latestRaw = Invoke-AgentGet $base "/v1/debug/latest" $psk
  Set-Content -Path (Join-Path $agentDir "latest.json") -Value $latestRaw -Encoding UTF8
  $latest = $latestRaw | ConvertFrom-Json

  $files = @()
  if ($latest.video) { $files += [string]$latest.video }
  if ($latest.log) { $files += [string]$latest.log }
  $files += "host-agent.log"
  $files += "latest.json"

  if ($AllFiles) {
    $listRaw = Invoke-AgentGet $base "/v1/debug/list" $psk
    Set-Content -Path (Join-Path $agentDir "list.json") -Value $listRaw -Encoding UTF8
    $listObj = $listRaw | ConvertFrom-Json
    foreach ($f in @($listObj.files)) {
      if ($f -and ($files -notcontains $f)) { $files += [string]$f }
    }
  }

  foreach ($name in ($files | Select-Object -Unique)) {
    if (-not $name) { continue }
    # latest.json already saved; skip re-download of remote latest under debug/
    if ($name -eq "latest.json") { continue }
    $dest = Join-Path $agentDir $name
    try {
      Invoke-AgentGet $base ("/v1/debug/file/" + [uri]::EscapeDataString($name)) $psk $dest
      Write-Host "  downloaded $name"
    } catch {
      Write-Warning "  failed $name : $_"
    }
  }

  $summary += [pscustomobject]@{
    id = $id
    ok = $true
    dir = $agentDir
    video = $latest.video
    log = $latest.log
    version = $(if ($latest.version) { $latest.version } else { $ver })
    built = $(if ($latest.built) { $latest.built } else { $built })
  }
}

$summaryPath = Join-Path $batchDir "pull-summary.json"
($summary | ConvertTo-Json -Depth 5) | Set-Content -Path $summaryPath -Encoding UTF8
Write-Host "Done. Artifacts: $batchDir"
Write-Host "Summary: $summaryPath"
Write-Output $batchDir
