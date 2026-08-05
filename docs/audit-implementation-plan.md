# 接入审计实施方案

Status: **active**（2026-08-05）— 已拍板双源+P1；A0–A3 已落地，A4 待办  
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
| 互斥拒绝 | ⏳ | 今日仅有 **容量拒绝（≥8）**；独占互斥未实现前不假装「互斥拒绝」 |

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

### 4.2 `audit_events`（可选明细，一期可后置）

若只要列表，可先不做事件表，布尔与结果直接 upsert 主表。需要排障时再加：

`id, session_id, at, source(viewer|agent), type, detail jsonb`

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
GET /v1/admin/audit/sessions?from=&to=&agent_id=&operator=&result=&limit=&cursor=
```

返回分页列表；详情可用 `GET /v1/admin/audit/sessions/{id}`（含 events，若启用）。

**无导出 CSV 也可二期做**；一期管理端表格 + 筛选即可。

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
- Admin：导航「审计」；表格列：时间、操作员、发起端（本机/IP）、被控端、结果、模式、时长、剪贴板/文件、备注；筛选：时间范围、结果、操作员、被控端。

## 7. 分期

| 批次 | 内容 | 验收 |
|------|------|------|
| **A0** | ✅ 表 + upsert API + admin 只读列表（可用 curl 灌数） | 管理员能查假数据 |
| **A1** | ✅ Viewer 上报 attempt/opened/closed/fail + mode；无协议改亦可 | 帐号登录远控一条完整话单 |
| **A2** | ✅ 协议带 `session_id` + Host 上报 auth/容量/peer + 归并 | 故意错 PSK / 打满 8 路可见失败单 |
| **A3** | ✅ 剪贴板/文件布尔；只读切换；保留期任务；（可选）events 表延后 | CONTEXT 字段表一期列齐 |
| **A4** | CSV 导出、按部门过滤、与目录 ACL 对齐（若以后做 ACL） | 运维可导出 |

建议落地顺序：**A0 → A1 → A2 → A3**。A1 即可演示；A2 才达到「权威失败可查」。

## 8. 明确不做 / 延后

| 项 | 原因 |
|----|------|
| 会话录像 | CONTEXT Avoid；与审计话单分离 |
| 中继路径审计 | 尚无中继 |
| 旁观 / 控权转移话单字段 | 能力未做；预留 `mode` / `meta` 即可 |
| 独占互斥拒绝 | 今日共享控制 + 容量上限；勿造假枚举 |
| 无网关演示树进中心库 | 无控制面身份与 agent_id 契约 |
| Sidecar `/logs` 当审计 | 仅实验室 |

## 9. 风险

- **上报失败**：远控优先；本地日志留痕；管理端 `partial`。
- **时钟偏移**：以各端本地 UTC 为准；时长用 opened/closed，列表按 `coalesce(opened_at, attempted_at)`。
- **隐私**：不存剪贴板内容与文件路径；管理端权限仅 admin。
- **重连**：同一 `session_id` 延续；`meta.reconnect_count++`；不要拆成多条话单（与 Viewer V2 重连一致）。

## 10. 文档与验收总清单

- [x] 本文件评审拍板：双源 + P1
- [x] A0 表/API/管理端列表
- [x] A1 Viewer 上报
- [x] A2 协议 `session_id` + Host 上报（auth/容量/peer/close）
- [x] A3 剪贴板/文件/只读 flag + 保留期（`audit_events` 延后）
- [ ] 帐号登录：成功远控一条；错媒体口令一条失败；用户关闭有时长（A1/A2 人工）
- [ ]（A2）容量满拒绝可查（人工：打满 8 路后再连）
- [ ] 管理端筛选可用；无录像入口
- [x] `CONTEXT.md` 审计条从「MVP 不上」改为「一期话单已上、不含录像」并链到本文

---

**维护**：字段或上报源变更先改本文再改代码；与 Viewer V 批次解耦，本方案独立推进。
