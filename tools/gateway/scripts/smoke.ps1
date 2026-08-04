# Road Desk gateway smoke checklist (API-level).
# Usage: pwsh -File tools/gateway/scripts/smoke.ps1
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

$fail = 0
function Pass($msg) { Write-Host "[PASS] $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red; $script:fail++ }
function Info($msg) { Write-Host "[INFO] $msg" -ForegroundColor Cyan }

$Base = if ($env:ROAD_DESK_SMOKE_BASE) { $env:ROAD_DESK_SMOKE_BASE } else { "http://127.0.0.1:8743" }
$OnlineAfter = 45

Info "Base=$Base  cwd=$Root"

# --- 1. compose + health + PSK files + admin creds ---
$pg = docker ps --format "{{.Names}}" 2>$null | Select-String -Pattern "^road-desk-pg$"
if ($pg) { Pass "Postgres container road-desk-pg running" } else { Fail "Postgres container not running (docker compose up -d)" }

try {
  $hz = curl.exe -s -f "$Base/healthz"
  if ($hz -match '"ok"\s*:\s*true') { Pass "GET /healthz => $hz" } else { Fail "healthz unexpected: $hz" }
} catch { Fail "healthz: $_" }

foreach ($f in @("data/agent-control.psk", "data/viewer.psk")) {
  if (Test-Path $f) { Pass "PSK file exists: $f" } else { Fail "missing $f" }
}
if (Test-Path "data/admin-credentials.txt") {
  Pass "admin-credentials.txt present (local)"
} elseif (Test-Path "data") {
  Info "admin-credentials.txt missing (ok if password only in first-boot log)"
}

$agentPsk = (Get-Content -Raw "data/agent-control.psk").Trim()
$viewerPsk = (Get-Content -Raw "data/viewer.psk").Trim()
if ($agentPsk.Length -lt 16) { Fail "agent PSK too short" } else { Pass "agent PSK loaded ($($agentPsk.Length) chars)" }
if ($viewerPsk.Length -lt 16) { Fail "viewer PSK too short" } else { Pass "viewer PSK loaded ($($viewerPsk.Length) chars)" }

$adminUser = "admin"
$adminPass = $null
if (Test-Path "data/admin-credentials.txt") {
  foreach ($line in Get-Content "data/admin-credentials.txt") {
    if ($line -match '^\s*password:\s*(.+)\s*$') { $adminPass = $Matches[1].Trim() }
  }
}
if (-not $adminPass) {
  Fail "cannot read admin password from data/admin-credentials.txt"
  Write-Host "Smoke aborted."; exit 1
}

# --- 2. fake agent register ---
$agentId = "smoke-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
$reg = @{
  agentId       = $agentId
  hostname      = "smoke-host"
  version       = "0.1.0-smoke"
  mediaPort     = 38471
  ipv4s         = @("10.9.9.9", "10.9.9.10")
  preferredIpv4 = "10.9.9.9"
} | ConvertTo-Json -Compress
Set-Content -Path "$env:TEMP\rd-smoke-reg.json" -Value $reg -Encoding ascii
$regOut = curl.exe -s -w "`n%{http_code}" -X POST "$Base/v1/agents/register" `
  -H "Authorization: Bearer $agentPsk" -H "Content-Type: application/json" `
  --data-binary "@$env:TEMP\rd-smoke-reg.json"
$code = ($regOut -split "`n")[-1]
$body = ($regOut -split "`n" | Select-Object -SkipLast 1) -join "`n"
if ($code -eq "200" -and $body -match $agentId) { Pass "POST /v1/agents/register ($agentId)" } else { Fail "register code=$code body=$body" }

# bad PSK
$bad = curl.exe -s -o NUL -w "%{http_code}" -X POST "$Base/v1/agents/register" `
  -H "Authorization: Bearer wrong-psk" -H "Content-Type: application/json" `
  --data-binary "@$env:TEMP\rd-smoke-reg.json"
if ($bad -eq "401") { Pass "register with bad PSK => 401" } else { Fail "bad PSK expected 401 got $bad" }

# --- 4 early: unauth 401 ---
$u = curl.exe -s -o NUL -w "%{http_code}" "$Base/v1/admin/agents"
if ($u -eq "401") { Pass "GET /v1/admin/agents without token => 401" } else { Fail "unauth expected 401 got $u" }

# --- 4. admin login + catalog + secrets ---
Set-Content -Path "$env:TEMP\rd-smoke-login.json" -Value (@{ username = $adminUser; password = $adminPass } | ConvertTo-Json -Compress) -Encoding ascii
$loginOut = curl.exe -s -X POST "$Base/v1/admin/login" -H "Content-Type: application/json" --data-binary "@$env:TEMP\rd-smoke-login.json"
$login = $loginOut | ConvertFrom-Json
if (-not $login.token) { Fail "admin login failed: $loginOut"; Write-Host "Smoke aborted."; exit 1 }
Pass "POST /v1/admin/login"
$token = $login.token
$AH = @("Authorization: Bearer $token")

# departments + users
$deptsOut = curl.exe -s "$Base/v1/admin/departments" -H $AH[0]
$depts = $deptsOut | ConvertFrom-Json
if ($depts -and $depts.Count -ge 1) { Pass "GET /v1/admin/departments ($($depts.Count))" } else { Fail "list departments: $deptsOut" }
$sysDept = $depts | Where-Object { $_.name -eq "系统管理" } | Select-Object -First 1
if (-not $sysDept) { Fail "missing system department 系统管理" }

Set-Content -Path "$env:TEMP\rd-smoke-dept.json" -Value '{"name":"冒烟部门","sortOrder":20}' -Encoding utf8
$dOut = curl.exe -s -X POST "$Base/v1/admin/departments" -H $AH[0] -H "Content-Type: application/json" --data-binary "@$env:TEMP\rd-smoke-dept.json"
$dept = $dOut | ConvertFrom-Json
if ($dept.id) { Pass "create department id=$($dept.id)" } else { Fail "create department: $dOut" }

$viewerUser = "smoke-viewer-" + [guid]::NewGuid().ToString("N").Substring(0, 6)
$viewerPass = "SmokePass1"
Set-Content -Path "$env:TEMP\rd-smoke-user.json" -Value (@{
  username = $viewerUser; password = $viewerPass; role = "viewer"; departmentId = [int64]$dept.id; enabled = $true
} | ConvertTo-Json -Compress) -Encoding utf8
$uOut = curl.exe -s -X POST "$Base/v1/admin/users" -H $AH[0] -H "Content-Type: application/json" --data-binary "@$env:TEMP\rd-smoke-user.json"
$user = $uOut | ConvertFrom-Json
if ($user.id -and $user.role -eq "viewer") { Pass "create viewer user id=$($user.id)" } else { Fail "create user: $uOut" }

Set-Content -Path "$env:TEMP\rd-smoke-viewer-login.json" -Value (@{ username = $viewerUser; password = $viewerPass } | ConvertTo-Json -Compress) -Encoding ascii
$vLoginDenied = curl.exe -s -w "`n%{http_code}" -X POST "$Base/v1/admin/login" -H "Content-Type: application/json" --data-binary "@$env:TEMP\rd-smoke-viewer-login.json"
$vDeniedCode = ($vLoginDenied -split "`n")[-1]
if ($vDeniedCode -eq "401") { Pass "viewer cannot admin-login => 401" } else { Fail "viewer admin-login expected 401 got $vDeniedCode" }

$vLoginOut = curl.exe -s -X POST "$Base/v1/viewer/login" -H "Content-Type: application/json" --data-binary "@$env:TEMP\rd-smoke-viewer-login.json"
$vLogin = $vLoginOut | ConvertFrom-Json
if ($vLogin.token -and $vLogin.role -eq "viewer") { Pass "POST /v1/viewer/login" } else { Fail "viewer login: $vLoginOut" }
$treeJwt = curl.exe -s -o NUL -w "%{http_code}" "$Base/v1/directory/tree" -H "Authorization: Bearer $($vLogin.token)"
if ($treeJwt -eq "200") { Pass "directory tree with viewer JWT => 200" } else { Fail "directory JWT expected 200 got $treeJwt" }

# create group under root
Set-Content -Path "$env:TEMP\rd-smoke-group.json" -Value '{"name":"冒烟站","sortOrder":10}' -Encoding utf8
$gOut = curl.exe -s -X POST "$Base/v1/admin/groups" -H $AH[0] -H "Content-Type: application/json" --data-binary "@$env:TEMP\rd-smoke-group.json"
$group = $gOut | ConvertFrom-Json
if ($group.id) { Pass "create group id=$($group.id) name=$($group.name)" } else { Fail "create group: $gOut" }

# create tag
Set-Content -Path "$env:TEMP\rd-smoke-tag.json" -Value '{"name":"冒烟标签"}' -Encoding utf8
$tOut = curl.exe -s -X POST "$Base/v1/admin/tags" -H $AH[0] -H "Content-Type: application/json" --data-binary "@$env:TEMP\rd-smoke-tag.json"
$tag = $tOut | ConvertFrom-Json
if ($tag.id) { Pass "create tag id=$($tag.id)" } else { Fail "create tag: $tOut" }

# rename + move group + tag agent
$patch = @{ displayName = "冒烟改名机"; groupId = [int64]$group.id; tagIds = @([int64]$tag.id) } | ConvertTo-Json -Compress
Set-Content -Path "$env:TEMP\rd-smoke-patch.json" -Value $patch -Encoding utf8
$pOut = curl.exe -s -X PATCH "$Base/v1/admin/agents/$agentId" -H $AH[0] -H "Content-Type: application/json" --data-binary "@$env:TEMP\rd-smoke-patch.json"
$patched = $pOut | ConvertFrom-Json
if ($patched.displayName -eq "冒烟改名机" -and $patched.groupId -eq $group.id) {
  Pass "PATCH agent rename/move/tag"
} else { Fail "patch agent: $pOut" }

# download secrets
$apk = curl.exe -s -o "$env:TEMP\rd-dl-agent.psk" -w "%{http_code}" "$Base/v1/admin/secrets/agent-psk" -H $AH[0]
$vpk = curl.exe -s -o "$env:TEMP\rd-dl-viewer.psk" -w "%{http_code}" "$Base/v1/admin/secrets/viewer-psk" -H $AH[0]
$apkBody = (Get-Content -Raw "$env:TEMP\rd-dl-agent.psk").Trim()
$vpkBody = (Get-Content -Raw "$env:TEMP\rd-dl-viewer.psk").Trim()
if ($apk -eq "200" -and $apkBody -eq $agentPsk) { Pass "download agent-psk" } else { Fail "download agent-psk code=$apk" }
if ($vpk -eq "200" -and $vpkBody -eq $viewerPsk) { Pass "download viewer-psk" } else { Fail "download viewer-psk code=$vpk" }

# --- 6. Viewer directory tree sees rename + connect target ---
$treeOut = curl.exe -s "$Base/v1/directory/tree" -H "Authorization: Bearer $viewerPsk"
if ($treeOut -match "冒烟改名机" -and $treeOut -match "10\.9\.9\.9" -and $treeOut -match "38471") {
  Pass "GET /v1/directory/tree shows renamed agent + preferredIpv4:mediaPort fields"
} else { Fail "directory tree missing expected fields: $treeOut" }

$treeBad = curl.exe -s -o NUL -w "%{http_code}" "$Base/v1/directory/tree" -H "Authorization: Bearer wrong"
if ($treeBad -eq "401") { Pass "directory tree bad key => 401" } else { Fail "directory bad key expected 401 got $treeBad" }

# SPA
$spa = curl.exe -s -o NUL -w "%{http_code}" "$Base/"
if ($spa -eq "200") { Pass "SPA / => 200" } else { Fail "SPA / => $spa" }

# --- 5. host-agent binary (optional elevated register) ---
$agentExe = Resolve-Path (Join-Path $Root "..\..\build\src\agent\host-agent.exe") -ErrorAction SilentlyContinue
if ($agentExe) {
  Pass "host-agent.exe present: $agentExe"
  Info "真机注册/断网关媒体直连需管理员进程与媒体面联调，本脚本用假 Agent 覆盖控制面"
} else {
  Info "host-agent.exe not built (skip binary presence)"
}

# --- 3. offline after threshold (force last_seen via no heartbeat + wait, or SQL) ---
Info "Forcing last_seen_at older than ${OnlineAfter}s via SQL (equiv. stopped heartbeats)..."
$sql = "UPDATE agents SET last_seen_at = NOW() - INTERVAL '$OnlineAfter seconds' - INTERVAL '5 seconds' WHERE agent_id = '$agentId';"
docker exec road-desk-pg psql -U roaddesk -d roaddesk -c $sql | Out-Null
$agentsOut = curl.exe -s "$Base/v1/admin/agents" -H $AH[0]
$agents = $agentsOut | ConvertFrom-Json
$mine = $agents | Where-Object { $_.agentId -eq $agentId }
if ($mine -and -not $mine.online) {
  Pass "agent online=false after lastSeen beyond ${OnlineAfter}s"
} else {
  Fail "expected offline; got: $($mine | ConvertTo-Json -Compress)"
}

# delete offline agent
$del = curl.exe -s -o NUL -w "%{http_code}" -X DELETE "$Base/v1/admin/agents/$agentId" -H $AH[0]
if ($del -eq "204") { Pass "DELETE offline agent => 204" } else { Fail "delete offline expected 204 got $del" }

# cleanup group/tag/user/dept best-effort
curl.exe -s -o NUL -X DELETE "$Base/v1/admin/groups/$($group.id)" -H $AH[0] | Out-Null
curl.exe -s -o NUL -X DELETE "$Base/v1/admin/tags/$($tag.id)" -H $AH[0] | Out-Null
if ($user.id) { curl.exe -s -o NUL -X DELETE "$Base/v1/admin/users/$($user.id)" -H $AH[0] | Out-Null }
if ($dept.id) { curl.exe -s -o NUL -X DELETE "$Base/v1/admin/departments/$($dept.id)" -H $AH[0] | Out-Null }

Write-Host ""
if ($fail -eq 0) {
  Write-Host "SMOKE OK — all automated checks passed ($fail failures)" -ForegroundColor Green
  exit 0
} else {
  Write-Host "SMOKE FAILED — $fail check(s) failed" -ForegroundColor Red
  exit 1
}
