# 接入审计实施方案

Status: **active**（2026-08-06）— 已拍板双源+P1；A0–A4 + `audit_events` 已落地；**A5a–A5d 桌面行为链路已落地（A5b/c 待实验室验收；A5d 管理端敏感展示/导出/事件保留）**；目录 ACL 一期不做  
Related: [CONTEXT.md](../CONTEXT.md)（审计日志 / 控制面）、[gateway.md](./gateway.md)、[adr/0001-media-plane-vnc-adapter.md](./adr/0001-media-plane-vnc-adapter.md)、[viewer-implementation-plan.md](./viewer-implementation-plan.md)

把远控「话单」落到控制面：可查询、可合规抽查；**不含会话录像**。媒体仍 Viewer↔Agent TLS mux 直连，审计事件由端上报，网关不旁路拆媒体包。

## 1. 产品定义（对照 CONTEXT）

| 字段 | 一期要 | 说明 |
|------|--------|------|
| 谁 | ✅ | 操作员：JWT `sub`/`uid`；仅 `viewer.psk` 目录时记 `viewer_psk` / 本机名 |
| 何时 | ✅ | 尝试时刻、接通时刻、断开时刻；时长由后两者推算 |
| 哪台被控端 | ✅ | `agent_id` + 当时 hostname / 优选 IP / media 端口 |
| 发起端 | ✅ | Viewer 本机名；可选 Viewer 出口 IP（Agent 侧 peer） |
| 结果 | ✅ | `ok` / `auth_fail` / `capacity_reject` / `connect_fail` / `tls_fail` / `cancelled` |
| 查看或控制 | ✅ | Viewer 上报会话模式：`control` / `view_only`（本地只读开关）；非控权转移语义 |
| 剪贴板 / 文件 | ✅ | 布尔「本会话是否用过」；文件另记方向、次数与**顶层**名/路径于 `meta.fileTransfer`（目录只记目录本身，不展开子文件）；仅管理端可见 |
| 断开原因 | ✅ | `user_close` / `transport_lost` / `auth_fail` / `host_gone` / `replaced` 等枚举 |
| 互斥拒绝 | ✅ | **A5a**：同时仅 **一路控制**，其余强制 **观看**；主控断开后 **自动提升** 仍在线的最早旁观（`kCtrlSessionRole`）。容量上限仍为 8 |

**不做**：会话录像 / 录屏审计 / 操作回放；键鼠轨迹；剪贴板正文；从 `host-agent.log` 扫日志当正式审计；媒体进网关中继。

## 2. 现状与缺口

```text
操作员 ──JWT/PSK──► 网关(Postgres 编目) ──目录──► Viewer
                                              │
                                              └──TLS mux──► Host Agent
                         ▲ 今日无会话信令 ▲
```

- 网关：编目 + 账号；**无** session/audit 表与 API（`tools/gateway`）。
- 管理端：编目 / 部门 / 用户；**无**审计页（`admin/`）。
- 媒体：`mux_host` / `mux_client` 已有接通、鉴权失败、容量拒绝、断开、剪贴板/文件日志，但是本地文件且**无操作员身份**（媒体口令 ≠ 网关账号）。
- ADR-0001：审计须在控制面，不得长进 RFB/媒体 Fork 内部。

**结论**：必须新增「上报 → 入库 → 管理端查询」；不能靠旁路网关流量推断远控话单。

## 3. 架构决策

### 3.1 谁上报（推荐：双源 + 关联 ID）

| 源 | 优势 | 一期职责 |
|----|------|----------|
| **Viewer** | 知道「谁」、目录里的 `agent_id`、只读开关、用户主动关 | 发起 `session.attempt` / `session.opened` / `session.closed`；补 `mode`、剪贴板/文件布尔 |
| **Host Agent** | 鉴权/容量/peer IP 权威；不依赖 Viewer 诚实关会话 | 上报 `host.auth_ok` / `host.auth_fail` / `host.capacity_reject` / `host.client_gone` |

两端共用 **`session_id`（UUID，Viewer 生成）**：

1. Viewer 开会话前生成 `session_id`，先 `POST` attempt（可选），再连媒体。
2. **协议增量（小）**：媒体 Control 鉴权成功后带上 `session_id`（或紧随一条 control 帧），Host 写入该连接上下文并在权威事件里带回。
3. 网关按 `session_id` 归并；缺一侧仍可查，标记 `partial=true`。

若一期要砍范围：**A1 仅 Viewer 上报**也能先出管理端列表；A2 再补 Host 权威鉴权/容量（强烈建议，防 Viewer 漏报失败）。

### 3.2 存哪

- Postgres 新表（与编目同库），迁移 `003_audit.sql`。
- 保留期默认 **90 天**（可配置）；超期批量删除或分区（一期用定时 `DELETE` 即可）。

### 3.3 谁看

- 管理端 **管理员 JWT** 只读查询；操作员 Viewer **不可**拉全站审计（避免横向窥探）。
- 上报接口：Viewer 用目录 JWT 或 `viewer.psk`；Agent 用 `agent-control.psk`。PSK 上报不得伪造任意 `agent_id`（Agent 只能报自己；Viewer 报的 `agent_id` 须在目录可见范围内，一期可先校验存在性）。

## 4. 数据模型（草案）

### 4.1 `audit_sessions`（话单主表，一行一会话）

| 列 | 类型 | 说明 |
|----|------|------|
| `id` | UUID PK | = `session_id` |
| `operator_user_id` | nullable int | JWT uid；PSK 目录为空 |
| `operator_name` | text | 展示用：登录名或 `viewer_psk` |
| `viewer_host` | text | Viewer 本机名 |
| `viewer_ip` | text nullable | Host 观察到的 peer（可后填） |
| `agent_id` | text | 被控端 ID |
| `agent_name` | text | 当时 hostname 快照 |
| `agent_endpoint` | text | `ip:port` 快照 |
| `mode` | text | `control` / `view_only` / `unknown` |
| `result` | text | 见 §1 |
| `disconnect_reason` | text nullable | |
| `used_clipboard` | bool | 默认 false |
| `used_file_transfer` | bool | 默认 false |
| `attempted_at` | timestamptz | |
| `opened_at` | timestamptz nullable | 媒体鉴权成功 |
| `closed_at` | timestamptz nullable | |
| `partial` | bool | 仅单侧上报 |
| `meta` | jsonb | 扩展：重连次数、viewer 版本、agent 版本等 |

索引：`(attempted_at DESC)`、`(agent_id, attempted_at DESC)`、`(operator_user_id, attempted_at DESC)`、`result`。

### 4.2 `audit_events`（会话时间线）

主表仍是「一行一话单」；本表按时间追加过程事件，**同一会话多行**。排障与后续进程开/关审计共用此表，**不改 schema 即可扩展 type**。

| 列 | 类型 | 说明 |
|----|------|------|
| `id` | bigserial | |
| `session_id` | uuid FK → `audit_sessions` ON DELETE CASCADE | 随话单保留期一并清理 |
| `at` | timestamptz | 事件发生时刻（端侧时钟） |
| `source` | text | `viewer` / `agent` / `gateway` |
| `type` | text (1–64, `[a-z][a-z0-9_]*`) | 见下表；未知合法 type 亦接受 |
| `detail` | jsonb | 类型相关细节，默认 `{}` |
| `created_at` | timestamptz | 入库时刻 |

索引：`(session_id, at, id)`、`(type, at DESC)`（跨会话按类型查，如进程）、`(at DESC)`。

**当前 / 预留 type：**

| type | 来源 | detail 要点 |
|------|------|-------------|
| `attempt` / `opened` / `closed` / `failed` / `flag` | upsert `phase` 自动追加 | result、mode、disconnectReason、clipboard 等 |
| `file_transfer` | upsert 带 `meta.fileTransfer` 时由 `flag` 提升 | 同主表 `meta.fileTransfer` |
| `process_open` | Host 行为审计 | `name`、`path`、`pid`、`ppid?`、`cmdline` |
| `process_close` | Host 行为审计 | `name`、`path`、`pid`、可选 `exitCode` |
| `window_focus` | Host 行为审计 | `title`、`pid`、`path?`（前台窗；还原桌面行为必需） |
| `window_title` | Host 可选 | 同窗标题变更（防抖） |

生命周期事件由 `POST /v1/audit/sessions/upsert` **旁路追加**（upsert 失败才拦远控路径；事件插入失败只打网关日志）。进程/窗口事件不改主表布尔，只写本表。

### 4.3 桌面行为审计（已拍板；A5b/A5c 端侧已实现待验收）

**产品目标**：尽量还原「当前控制端操作员在被控端做了什么」——会话内较全的进程时间线，并带窗口标题；**不是**录像/键鼠轨迹。

**已拍板约束（2026-08-05）：**

1. **尽量还原桌面行为**（进程 + 前台窗口标题流；非像素回放）。
2. **进程/窗口事件归到某一个操作员**（挂该控制会话的 `session_id` → 话单上的 `operator_*`）。
3. **要存**：进程路径、**命令行**、**窗口标题**（敏感；仅 admin；导出策略另定）。
4. **会话模型**：同时 **只允许一路控制**；其它连接 **只能观看（只读）**。无唯一控制方则不做行为归属。

**采集规则（Host）：**

- 仅当存在 **主控（control）会话** 时订阅读进程/窗口；只读/旁观连接 **不采**。
- 主控断开或降为只读 → 停止采集；新主控 → 新 `session_id` 时间线。
- 上报：`POST /v1/audit/events` 批量（≤200）；失败不影响远控。
- 降噪：可丢极短焦点抖动；可对明显内核/服务进程默认折叠（仍以「较全」为原则，名单可配）。
- Win7/Win10 采集 API 需实测可靠后再标「已交付」（ETW / 前台窗轮询等）。

**依赖顺序：**

1. **A5a** ✅ Host：**一路控制、其余强制只读**；主控离开自动提升最早旁观（`kCtrlSessionRole`）；非主控 Input/剪贴板/文件丢弃。
2. **A5b** ✅ 会话内较全 `process_open` / `process_close`（含 path、cmdline；Toolhelp 轮询 + PEB cmdline；仅主控会话）。
3. **A5c** ✅ `window_focus`（+ `window_title` 防抖；`GetForegroundWindow` 轮询，与 A5b 同线程）。
4. **A5d** ✅ 管理端时间线 cmdline/标题默认折叠；行为事件 CSV 导出（默认脱敏，可选含敏感字段）；`ROAD_DESK_AUDIT_EVENTS_RETENTION_DAYS` 可选更短保留。

**做不到的边界**：未新开进程的纯 UI 编辑、未反映在标题上的页面内容、等价录像的操作回放——仍属 CONTEXT Avoid。

```
POST /v1/audit/events
Authorization: Bearer <viewer JWT | viewer.psk | agent PSK>
{ "events": [ { "sessionId", "at?", "source?", "type", "detail?" } ] }  // 单次 ≤200
```

```
GET /v1/admin/audit/sessions/{id} → { "session": …, "events": [ … ] }
```

管理端：点击话单行打开抽屉时间线（含 process_* / window_* 标签）。

## 5. API（草案）

### 5.1 上报

```
POST /v1/audit/sessions/upsert
Authorization: Bearer <viewer JWT | viewer.psk | agent PSK>
```

Body（示例，幂等 upsert by `id`）：

```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "source": "viewer",
  "phase": "opened",
  "operator_name": "alice",
  "operator_user_id": 3,
  "viewer_host": "OPS-PC-01",
  "agent_id": "…",
  "agent_name": "WIN7-LANE-1",
  "agent_endpoint": "192.168.26.131:38471",
  "mode": "control",
  "result": "ok",
  "used_clipboard": false,
  "used_file_transfer": false,
  "attempted_at": "…",
  "opened_at": "…"
}
```

`phase`: `attempt` | `opened` | `closed` | `failed` | `flag`（仅更新剪贴板/文件布尔）。

Host 上报同路径，`source=agent`，可填 `viewer_ip`、`result=auth_fail|capacity_reject` 等。

### 5.2 管理查询

```
GET /v1/admin/audit/sessions?from=&to=&agent_id=&operator=&result=&department_id=&limit=&cursor=
GET /v1/admin/audit/sessions/export?from=&to=&agent_id=&operator=&result=&department_id=  # CSV，最多 5000 行
```

返回分页列表；详情 `GET /v1/admin/audit/sessions/{id}` → `{ session, events }`（见 §4.2）。

批量事件（进程等）：`POST /v1/audit/events`（与 upsert 同鉴权）。

部门过滤：按操作员账号所属 `users.department_id`（无 `operator_user_id` 的 PSK 话单不会命中部门筛选）。**目录 ACL（机器级可见范围）一期不做**；管理员审计仍看全站话单。

**CSV 导出**：管理端「导出 CSV」；UTF-8 BOM，含部门与 `meta`。

## 6. 端侧改动要点

### 6.1 Viewer（`src/viewer`）

1. `open_session_for_device` / 直连 CLI：生成 `session_id`，持有到 `SessionHost`。
2. 连接前/成功/失败/用户关闭/重连放弃：异步 HTTP upsert（失败只打 `viewer.log`，**不阻断远控**）。
3. 只读切换、首次剪贴板/文件成功：`phase=flag`。
4. 网关基址与凭据复用现有 `GatewayConfig`；无网关 / `--demo-book`：**不上报**（或写本地，不进中心）。

### 6.2 Host Agent（`src/media/mux_host.cpp` + agent）

1. 接受连接时 `getpeername` → 待鉴权上下文。
2. 鉴权成功：解析/接收 `session_id`；上报 `opened` + peer IP。
3. 鉴权失败 / 容量拒绝：上报 `failed`（无 `session_id` 时网关生成匿名 id 或按 `agent_id+peer+time` 弱关联——**优先要求 Viewer 先发 attempt 再连**，失败也可带同一 id）。
4. `close_client`：若已知 session，上报 `closed`（Viewer 已关则可幂等忽略）。

### 6.3 协议

最小改动优先：

- **方案 P1（推荐）**：在现有 auth 成功后增加一条 control 消息 `kCtrlSessionMeta`（UUID 字符串）；旧端忽略未知帧则需版本协商——今日 mux 若严格，可改为 **auth payload 后追加 optional UTF-8 session_id**（长度前缀），旧 Host 忽略多余字节需确认现有解析是否容错。
- **方案 P2**：不做协议改；仅 Viewer 上报。Host 侧 A2 用「时间窗 + peer IP」弱关联（准确率差，仅过渡）。

**已拍板：双源 + P1。** A0 仅控制面；A1/A2 再改端与协议（A2 前 spike 鉴权帧半日）。

### 6.4 网关 / 管理端

- Go：`internal/store` + `api` 路由；迁移 `003_audit.sql`。
- Admin：导航「审计」；表格列：时间、操作员、部门、发起端（本机/IP）、被控端、结果、模式、时长、剪贴板/文件、备注；筛选：时间范围、结果、操作员、部门、被控端；导出 CSV。

## 7. 分期

| 批次 | 内容 | 验收 |
|------|------|------|
| **A0** | ✅ 表 + upsert API + admin 只读列表（可用 curl 灌数） | 管理员能查假数据 |
| **A1** | ✅ Viewer 上报 attempt/opened/closed/fail + mode；无协议改亦可 | 帐号登录远控一条完整话单 |
| **A2** | ✅ 协议带 `session_id` + Host 上报 auth/容量/peer + 归并 | 错 PSK 可见失败单；容量满拒绝人工暂缓（无 8 路条件） |
| **A3** | ✅ 剪贴板/文件布尔；只读切换；保留期；`audit_events` 时间线（进程 type 预留） | CONTEXT 字段表一期列齐 |
| **A4** | ✅ CSV 导出、按部门过滤；**目录 ACL 明确不做（一期）** | ✅ 人工验收通过（2026-08-05） |
| **A5a** | ✅ 一路控制、其余强制只读；主控离开自动提升旁观 | ✅ 人工验收中（2026-08-05；含提升续测） |
| **A5b** | ✅ Host：主控会话进程开/关（path + cmdline）→ `POST /v1/audit/events` | 📋 实验室：主控期间开/关 notepad 等可见；旁观不采；归属该操作员 |
| **A5c** | ✅ Host：前台 `window_focus` / 防抖 `window_title` | 📋 实验室：切换窗口/改标题可见；与进程时间线可对照 |
| **A5d** | ✅ 时间线敏感字段折叠；行为事件导出；单会话文本导出；事件独立保留期 env | ✅ 管理端可见折叠；导出菜单；文档 env |

建议落地顺序：**A0 → A1 → A2 → A3**。A1 即可演示；A2 才达到「权威失败可查」。

## 8. 明确不做 / 延后

| 项 | 原因 |
|----|------|
| 会话录像 | CONTEXT Avoid；与审计话单分离 |
| 中继路径审计 | 尚无中继 |
| 旁观 / 控权转移话单字段 | A5a：非主控 `view_only`；主控离开自动提升旁观（同会话改 `mode`，不新开话单） |
| 多路同时控制 | **行为审计前提下不做**；已拍板仅一路控制 |
| 独占互斥拒绝 | A5a 实现前仍勿造假；实现后补 `control_busy`（名待定）与容量拒绝区分 |
| 会话内键鼠轨迹 / 录像 | CONTEXT Avoid；行为审计止于进程+窗口元数据 |
| 无网关演示树进中心库 | 无控制面身份与 agent_id 契约 |
| 目录 ACL / 机器级审计裁剪 | 一期管理员看全站；按部门只筛操作员归属；ACL 未做前不对齐 |
| Sidecar `/logs` 当审计 | 仅实验室 |

## 9. 风险

- **上报失败**：远控优先；本地日志留痕；管理端 `partial`。
- **时钟偏移**：以各端本地 UTC 为准；时长用 opened/closed，列表按 `coalesce(opened_at, attempted_at)`。
- **隐私**：剪贴板**正文**仍不存；文件仅顶层名/路径。行为审计 **cmdline / 窗口标题** → 仅 admin；时间线默认折叠；行为事件 CSV 默认脱敏（`include_sensitive=1` 才带出）；可用 `ROAD_DESK_AUDIT_EVENTS_RETENTION_DAYS` 短于话单保留。
- **重连**：同一 `session_id` 延续；`meta.reconnect_count++`；不要拆成多条话单（与 Viewer V2 重连一致）。
- **归属**：行为事件只挂主控会话；多路观看者不产生 process/window 行。

## 10. 文档与验收总清单

- [x] 本文件评审拍板：双源 + P1
- [x] A0 表/API/管理端列表
- [x] A1 Viewer 上报
- [x] A2 协议 `session_id` + Host 上报（auth/容量/peer/close）
- [x] A3 剪贴板/文件/只读 flag + 保留期
- [x] `audit_events` 明细表 + upsert 旁路追加 + 管理端时间线（进程 type 预留；端侧上报延后）
- [x] 帐号登录：成功远控一条；错媒体口令一条失败；用户关闭有时长（A1/A2 人工，2026-08-05）
- [ ]（A2）容量满拒绝可查 — **暂缓**：实验室暂无条件打满 8 路并发
- [x] 管理端筛选可用；无录像入口
- [x] `CONTEXT.md` 审计条从「MVP 不上」改为「一期话单已上、不含录像」并链到本文
- [x] 文件方向 + 顶层名/路径进管理端（人工，同日）
- [x] A4 CSV 导出 + 按操作员部门过滤（人工验收 2026-08-05；**目录 ACL 一期不做**）
- [x] 桌面行为审计目标拍板：尽量还原；归单一操作员；path+cmdline+窗口标题；**一路控制、其余只读**（§4.3）
- [x] A5a 一路控制 / 其余强制只读；主控离开自动提升旁观（人工验收续测 2026-08-05）
- [x] A5b Host 进程开/关时间线上报（path + cmdline；待实验室验收 2026-08-06）
- [x] A5c 窗口焦点/标题时间线（focus 稳定 2 拍 + title 1.5s 防抖；待实验室验收）
- [x] A5d 管理端敏感字段展示/导出/保留策略（2026-08-06）

---

**维护**：字段或上报源变更先改本文再改代码；与 Viewer V 批次解耦，本方案独立推进。
