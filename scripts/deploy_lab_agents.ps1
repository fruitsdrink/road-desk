# Deploy host-agent.exe to lab VMs via Host Sidecar (POST /deploy).
# Usage:
#   $env:ROAD_DESK_PSK = 'road-desk'   # media PSK (for health check)
#   .\scripts\deploy_lab_agents.ps1
#   .\scripts\deploy_lab_agents.ps1 -Id win7,win10 -Build
#
# Inventory (.cursor/debug-agents.json) fields per agent:
#   host, sidecar_port (default 17890), sidecar_token / sidecar_token_env,
#   debug_port, psk_env

param(
  [string]$Inventory = "",
  [string[]]$Id = @(),
  [string]$Exe = "",
  [switch]$Build,
  [switch]$NoStart,
  [switch]$SkipHealth
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
  throw "Inventory not found: $Inventory"
}

if ($Build) {
  & (Join-Path $PSScriptRoot "build.ps1") -Target host-agent
}

if (-not $Exe) {
  $Exe = Join-Path $repoRoot "build\src\agent\host-agent.exe"
}
if (-not (Test-Path $Exe)) {
  throw "Agent binary not found: $Exe (build first or pass -Exe)"
}

$inv = Get-Content -Raw -Encoding UTF8 $Inventory | ConvertFrom-Json
$agents = @($inv.agents)
if ($Id.Count -gt 0) {
  $want = [System.Collections.Generic.HashSet[string]]::new([string[]]$Id)
  $agents = $agents | Where-Object { $want.Contains([string]$_.id) }
  if (-not $agents) { throw "No agents matched -Id $($Id -join ',')" }
}

function Resolve-Psk([object]$agent) {
  if ($agent.psk -and [string]$agent.psk) { return [string]$agent.psk }
  $envName = if ($agent.psk_env) { [string]$agent.psk_env } else { "ROAD_DESK_PSK" }
  $v = [Environment]::GetEnvironmentVariable($envName)
  if (-not $v) { throw "PSK missing for $($agent.id): set $envName or agent.psk" }
  return $v
}

function Resolve-SidecarToken([object]$agent) {
  if ($agent.sidecar_token -and [string]$agent.sidecar_token) {
    return [string]$agent.sidecar_token
  }
  $envName = if ($agent.sidecar_token_env) { [string]$agent.sidecar_token_env } else { "ROAD_DESK_SIDECAR_TOKEN" }
  $v = [Environment]::GetEnvironmentVariable($envName)
  if ($v) { return $v }
  # Fall back to media PSK when labs use the same secret.
  if ($agent.psk -and [string]$agent.psk) { return [string]$agent.psk }
  $pskEnv = if ($agent.psk_env) { [string]$agent.psk_env } else { "ROAD_DESK_PSK" }
  return [Environment]::GetEnvironmentVariable($pskEnv)
}

function Sidecar-Port([object]$agent) {
  if ($agent.sidecar_port) { return [int]$agent.sidecar_port }
  return 17890
}

function Debug-Port([object]$agent) {
  if ($agent.debug_port) { return [int]$agent.debug_port }
  $mp = if ($agent.media_port) { [int]$agent.media_port } else { 38471 }
  return $mp + 1
}

$exeInfo = Get-Item $Exe
Write-Host "Deploying $($exeInfo.FullName) ($($exeInfo.Length) bytes, $($exeInfo.LastWriteTime))"

$startQ = if ($NoStart) { "start=0" } else { "start=1" }
$failed = @()
foreach ($agent in $agents) {
  $id = [string]$agent.id
  $hostName = [string]$agent.host
  $port = Sidecar-Port $agent
  $token = Resolve-SidecarToken $agent
  $base = "http://${hostName}:${port}"
  Write-Host "==> $id $base/deploy?$startQ"

  $headers = @{}
  if ($token) {
    $headers["X-Road-Desk-Token"] = $token
  }

  try {
    $status = Invoke-RestMethod -Uri "$base/status" -Headers $headers -TimeoutSec 8
    Write-Host ("  before: running={0} size={1} mtime={2}" -f $status.agent_running, $status.agent_size, $status.agent_mtime)
  } catch {
    Write-Host "  status FAILED: $($_.Exception.Message)"
    $failed += $id
    continue
  }

  try {
    $resp = Invoke-WebRequest -Method Post -Uri "$base/deploy?$startQ" `
      -Headers $headers -InFile $Exe -ContentType "application/octet-stream" `
      -UseBasicParsing -TimeoutSec 120
    Write-Host "  deploy HTTP $($resp.StatusCode): $($resp.Content.Trim())"
  } catch {
    $detail = $_.ErrorDetails.Message
    Write-Host "  deploy FAILED: $($_.Exception.Message)"
    if ($detail) { Write-Host "  $detail" }
    $failed += $id
    continue
  }

  if ($SkipHealth) { continue }

  $psk = Resolve-Psk $agent
  $dbg = Debug-Port $agent
  $ok = $false
  foreach ($i in 1..15) {
    Start-Sleep -Milliseconds 800
    try {
      $h = Invoke-RestMethod -Uri "http://${hostName}:${dbg}/v1/debug/health" `
        -Headers @{ Authorization = "Bearer $psk" } -TimeoutSec 3
      Write-Host ("  health: version={0} built={1}" -f $h.version, $h.built)
      $ok = $true
      break
    } catch {
      # still starting
    }
  }
  if (-not $ok) {
    Write-Host "  health FAILED (agent may need ROAD_DESK_PSK / elevation)"
    $failed += $id
  }
}

if ($failed.Count -gt 0) {
  throw "Deploy incomplete: $($failed -join ', ')"
}
Write-Host "Done."
