# Road Desk 网关 / 管理端 / Viewer 目录（一期）

打通 **Agent 注册心跳 → 网关编目 → 管理端维护 → Viewer 真目录直连媒体**。媒体仍走 Viewer↔Agent TLS mux，不经网关中继。

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

环境变量（或 `%ProgramData%\RoadDesk\viewer.json`）：

- `ROAD_DESK_GATEWAY_URL=http://<gateway>:8743`
- `ROAD_DESK_VIEWER_KEY=<viewer.psk 内容>`

`viewer.json` 字段：`gatewayUrl`、`viewerKey`。

仍需媒体面：`ROAD_DESK_PSK`、TLS fingerprint 或 `ROAD_DESK_TLS_INSECURE=1`。

未配置网关或加 `--demo-book` 时使用内置演示树。

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
POST /v1/admin/login
GET/POST/PATCH/DELETE /v1/admin/groups|tags|agents...
GET /v1/admin/secrets/agent-psk|viewer-psk
GET /v1/directory/tree|agents          # Viewer-PSK
GET /healthz
```
