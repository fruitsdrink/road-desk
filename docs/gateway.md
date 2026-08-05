# Road Desk 网关 / 管理端 / Viewer 目录（一期）

打通 **Agent 注册心跳 → 网关编目 → 管理端维护 → Viewer 真目录直连媒体**。媒体仍走 Viewer↔Agent TLS mux，不经网关中继。

接入审计（远控话单）方案见 [audit-implementation-plan.md](./audit-implementation-plan.md)（控制面入库；媒体直连需端上报）。

## 组件

| 路径 | 说明 |
|------|------|
| `tools/gateway/` | Go 网关 + Docker Postgres |
| `admin/` | Vite/React 管理端（构建到 `tools/gateway/web/dist`） |
| `host-agent` | 配置弹窗 + 注册/心跳（`--no-gateway` 可跳过） |
| `viewer` | 拉取 `GET /v1/directory/tree`；`--demo-book` 回退演示树 |

## 启动 Postgres

需本机 Docker Desktop 已运行：

```powershell
cd tools/gateway
docker compose up -d
```

默认：`postgres://roaddesk:roaddesk@127.0.0.1:5433/roaddesk?sslmode=disable`（容器内仍为 5432；宿主机映射 5433，避免与本机 Postgres 冲突）

## 配置（环境变量 / `.env`）

网关启动时会读取工作目录下的 **`.env`**（可用 `.env.example` 复制）。规则：

- 仅填充**尚未**出现在进程环境中的变量（系统/Shell 环境优先）
- 常用键：`DATABASE_URL`、`ROAD_DESK_GATEWAY_LISTEN`、`ROAD_DESK_GATEWAY_DATA`、`ROAD_DESK_GATEWAY_WEB`

```powershell
cd tools/gateway
copy .env.example .env
# 按需改 DATABASE_URL 等
```

## 启动网关

```powershell
cd tools/gateway
go run ./cmd/gateway
```

监听默认 `:8743`。首次启动日志会打印：

- 初始管理员 `admin` 与随机口令（仅一次）
- `data/agent-control.psk`、`data/viewer.psk` 路径

本地开发若口令已生成，可看 `tools/gateway/data/admin-credentials.txt`（该目录已 gitignore，勿提交）。

健康检查：`GET http://127.0.0.1:8743/healthz`

## 密钥用途

| 文件 | 给谁 | 用途 |
|------|------|------|
| `agent-control.psk` | Host Agent（被控端） | 注册 / 心跳 |
| `viewer.psk` | Viewer（控制端） | 只读目录 API |

管理端用用户名口令，不用这两把。媒体面另用 `ROAD_DESK_PSK`。

## 管理端

```powershell
cd admin
pnpm install
pnpm build   # 输出到 tools/gateway/web/dist，由网关托管
# 开发：
pnpm dev     # http://127.0.0.1:5173 ，API 代理到 :8743
```

浏览器打开网关根路径或 Vite 开发地址 → 登录 → 分组/标签/Agent 编目 → 下载两把密钥。

## Agent 装机

1. 从管理端下载 `agent-control.psk`
2. 运行 `host-agent.exe`（管理员）。无 `%ProgramData%\RoadDesk\agent.json` 时弹窗填写网关 IP/端口与 PSK
3. 媒体口默认 `38471`；网关心跳失败只打日志重试，不退出
4. 控制台调试可加 `--no-gateway`

## Viewer 配网关

启动控制台模式且未配置时，会弹出 **Viewer 登录** 对话框：

1. **帐号登录**：管理端的 `admin`（管理员）或 `viewer`（操作员）→ `POST /v1/viewer/login` 取 JWT 拉目录
2. **或导入 Viewer PSK**：管理端下载的 `viewer.psk`（选文件或粘贴）

每次冷启动都会弹出登录框（JWT 仅本次进程有效，不落盘）。若曾导入过 Viewer PSK，会写入 `%ProgramData%\RoadDesk\viewer.json` 的 `viewerPsk`（另存 `gatewayUrl`、可选 `username`），下次可跳过重新选文件。勾选「记住密码」时另存 `rememberPassword` + `password`，下次预填密码框；取消勾选则清除落盘密码。目录拉取失败会再次弹出登录框。

也可不经弹窗，直接用环境变量：

- `ROAD_DESK_GATEWAY_URL=http://<gateway>:8743`
- `ROAD_DESK_VIEWER_KEY=<viewer.psk 或 JWT>`（设置后跳过登录框）
- `ROAD_DESK_VIEWER_USER=<用户名>`（可选）

仍需媒体面：`ROAD_DESK_PSK`、TLS fingerprint 或 `ROAD_DESK_TLS_INSECURE=1`。

取消登录则退出。未配置网关或加 `--demo-book` 时使用内置演示树。

## 验收清单（冒烟）

自动化（网关已起、`data/admin-credentials.txt` 可用时）：

```powershell
cd tools/gateway
pwsh -File scripts/smoke.ps1
```

覆盖：Postgres / healthz / 两把 PSK、假 Agent 注册、坏密钥 401、登录编目（改名/挪组/打标签）、密钥下载、目录树可见改名与 IP:端口、离线判定、删离线 Agent、SPA。

仍需人工（媒体面 / UI）：

- [ ] 真机 `host-agent` 注册进管理端；断网关后 Viewer 仍可直连媒体
- [ ] Viewer 控制台双击目录项建立远控会话

## API 摘要

```
POST /v1/agents/register|heartbeat     # Agent-PSK
POST /v1/admin/login                   # 仅 role=admin
POST /v1/viewer/login                  # viewer 或 admin（目录）
GET/POST/PATCH/DELETE /v1/admin/groups|tags|agents...
GET/POST/PATCH/DELETE /v1/admin/departments|users...
GET /v1/admin/secrets/agent-psk|viewer-psk
POST /v1/audit/sessions/upsert         # Viewer JWT/PSK 或 Agent-PSK；话单幂等
GET /v1/admin/audit/sessions[/{id}]    # 管理员查审计
GET /v1/directory/tree|agents          # Viewer-PSK 或 viewer/admin JWT
GET /healthz
```

灌一条假话单（网关已起、持有 admin JWT 或 viewer.psk）：

```powershell
# $tok = 管理端登录后的 Bearer；也可用 viewer.psk
curl -s -X POST http://127.0.0.1:8743/v1/audit/sessions/upsert `
  -H "Authorization: Bearer $tok" -H "Content-Type: application/json" `
  -d '{"id":"550e8400-e29b-41d4-a716-446655440000","phase":"opened","operatorName":"demo","agentId":"lab-1","agentName":"WIN10-LAB","agentEndpoint":"192.168.26.132:38471","mode":"control","result":"ok","partial":true}'
```

管理端「审计」页可筛时间 / 操作员 / 被控端 / 结果。端上报与协议见 [audit-implementation-plan.md](./audit-implementation-plan.md)。

## 部门与用户

- 用户必须归属某个部门；系统预置部门「系统管理」。
- 角色：`admin` 可登录管理端；`viewer` 仅可通过 `POST /v1/viewer/login` 获取 JWT 访问目录（Viewer 仍可用 `viewer.psk`）。
- 管理端导航：编目 / 部门 / 用户 / 审计。