---
name: agent-debug-pull
description: >-
  Pulls Host Agent auto-session recordings (MP4 or AVI fallback) and host-agent
  logs from one or more debug lab machines via debug HTTP, then analyzes
  drag/ghost/capture symptoms. Use when the user asks to pull agent recordings,
  download session video/logs from Win7/Win10 (or other) agents, run
  agent-debug-pull, or debug capture quality across multiple Host Agent clients.
---

# Agent debug pull

Pull session video + logs from every Host Agent listed in the lab inventory, then analyze.

## Inventory (multi-agent)

1. Prefer `.cursor/debug-agents.json` (local; may contain IPs). If missing, copy from `.cursor/debug-agents.example.json` and fill hosts.
2. Each agent entry: `id`, `label`, `host`, `media_port` (default 38471), `debug_port` (default media+1), `psk_env` (default `ROAD_DESK_PSK`) or `psk`.
3. Add new lab machines as extra objects under `agents[]` — do not hardcode only Win7/Win10.

## Pull

```powershell
$env:ROAD_DESK_PSK = '<media PSK>'
.\scripts\pull_agent_debug.ps1
# optional:
.\scripts\pull_agent_debug.ps1 -Id win7,win10
.\scripts\pull_agent_debug.ps1 -ListOnly
.\scripts\pull_agent_debug.ps1 -AllFiles
```

Script prints the batch directory (under `build/debug-pull/<stamp>/`) and writes `pull-summary.json`.

Requires: Host Agents running a build with auto-record + debug HTTP (`debug-http: listening on …` in log). Debug port = media port + 1. Auth = media PSK (`ROAD_DESK_PSK`), not gateway `agentPsk`.

## After pull — analyze

For each agent dir under the batch:

1. Open `latest.json` and the session `*.mp4` or `*.avi` (or ask user to scrub if video tools unavailable).
2. Read `host-agent_*.log` and live `host-agent.log`. Focus on lines with:
   - `session-record:`
   - `tick=` / `copy=` / `wire=` / `drop=` / `jpeg=` / `modern_lossy`
   - capture backend (Mirror / DXGI / GDI)
3. Compare Win7 vs Win10 (and any other `id`s) side-by-side: ghost trails, tearing, drop spikes, unexpected `copy>0` on Mirror.
4. Report per-agent: what the video shows, matching log evidence, likely cause, next fix. Keep it concise.

## Agent-side facts

- Auto record: `<exe>/debug/session_YYYYMMDD_HHMMSS.mp4` (MF H.264/MJPG) or `.avi` (WIC JPEG + VFW MJPEG fallback, typical Win7). ~10 fps. Disable with `ROAD_DESK_AUTO_RECORD=0`.
- Retention: only the **latest** session video + log snapshot; older `session_*.mp4` / `session_*.avi` / `host-agent_*.log` deleted on new record / session end.
- Host tick log: `drop=` is **interval** drops since last stat line; `drop_total=` is cumulative. A flat `drop=20` with no interval growth used to be misleading.
- On session end: log snapshot `host-agent_<session>.log` + `latest.json`.
- Debug HTTP: `GET /v1/debug/health|latest|list|file/<name>` on media_port+1. Disable with `ROAD_DESK_DEBUG_HTTP=0`.
- **Version check**: `/v1/debug/health` returns JSON `version` + `built`. Pull script prints them. Host log line: `host-agent starting … version=… built=… path=…`. Expect **0.4.3+**; mismatch → wrong binary on that client.
- Firewall: allow TCP debug_port from the pull machine.
- Pull while Viewer is connected may yield incomplete MP4 (no moov). Disconnect first; AVI is usable mid-session more often but prefer after disconnect.

## Checklist

```
Task progress:
- [ ] Inventory hosts current (all agents under test)
- [ ] ROAD_DESK_PSK set; pull script succeeded
- [ ] Per-agent version/built matches expected build
- [ ] Per-agent video + log reviewed
- [ ] Cross-agent comparison written
```
