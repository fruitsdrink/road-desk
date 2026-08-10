# Road Desk

[![CI](https://github.com/fruitsdrink/road-desk/actions/workflows/ci.yml/badge.svg)](https://github.com/fruitsdrink/road-desk/actions/workflows/ci.yml)

高速公路路段场景下的远程桌面远控软件：让授权人员像使用 VNC 一样，远程查看并操作另一端 Windows 计算机的桌面，用于机电设备远程维护。

**当前版本：0.4.34** | 语言：C++ / Go / TypeScript | 平台：Windows x64

## 架构

```
操作员 (Viewer) ──TLS mux──► 被控端 (Host Agent)
       │                          │
       └── 网关 (Go) ◄── 注册/心跳 ──┘
              │
              └── 管理端 (React)
```

- **Viewer** — Win32 操作端，含目录管理台与远控会话窗。支持 Win7 x64。
- **Host Agent** — 被控端常驻服务，采集桌面 + 接受键鼠注入。支持 Win7 / Server 2008 / 2012 / Win10+。
- **网关** — Go 服务 + Docker Postgres，提供编目、账号、审计。中心机部署，不经手媒体流量。
- **管理端** — Vite + React + Ant Design，供管理员维护编目、用户、部门、审计查询。
- **媒体面** — 自研 TLS mux 协议，Mirror/DXGI/GDI 采集，H.264/zlib 编码。直连，不经网关。

远控全程加密（Schannel TLS + PSK），不提供明文模式。详细术语与产品边界见 [CONTEXT.md](CONTEXT.md)。

## 快速开始

### 环境要求

| 组件 | 要求 |
|------|------|
| 编译器 | MSVC 2022 (VS 2022 Build Tools) + C++ 桌面 workload |
| 构建工具 | CMake 3.20+ + Ninja |
| Go | 1.20.x（Sidecar）；1.21+（网关） |
| Node.js | 18+（管理端） |
| Docker | 桌面版（网关 Postgres） |

### 构建 Host Agent + Viewer

```powershell
# 一键构建（含版本号自增）
.\scripts\build.ps1

# 产物
#   build\src\agent\host-agent.exe
#   build\src\viewer\viewer.exe
```

### 运行测试

```powershell
cmake --build build --target auth_test session_mutex_test mux_parse_test clipboard_test tls_test capture_strategy_test session_monitor_test
ctest --test-dir build --output-on-failure
```

### 启动开发环境

参见 [docs/dev-environment.md](docs/dev-environment.md) 获取完整的机器角色、Sidecar 推送与联调流程。

### 网关与管理端

```powershell
# 1. 启动 Postgres
cd tools/gateway
docker compose up -d

# 2. 启动网关
go run ./cmd/gateway

# 3. 构建并启动管理端（开发模式）
cd admin
pnpm install
pnpm dev
```

详见 [docs/gateway.md](docs/gateway.md)。

## 仓库目录

```text
road-desk/
├── CONTEXT.md                  # 产品术语表与边界定义
├── docs/                       # ADR、实施方案、设计规范、技术栈
│   ├── adr/                    # 架构决策记录
│   ├── design/                 # UI 设计稿（Pencil .pen 文件）
│   ├── audit-implementation-plan.md
│   ├── gateway.md
│   ├── tech-stack.md
│   └── viewer-implementation-plan.md
├── src/
│   ├── agent/                  # Host Agent（C++）
│   ├── viewer/                 # Viewer（C++）
│   ├── media/                  # mux + TLS 媒体面（采集、编码、剪贴板、文件传输）
│   │   └── tests/              # 媒体面与协议单元测试
│   ├── session/                # 控制面：PSK 鉴权、会话计数
│   │   └── tests/              # 控制面单元测试
│   └── ui/                     # 共用 UI token、DPI helper、应用图标资源
├── admin/                      # 管理端（React + Ant Design）
├── tools/
│   ├── gateway/                # Go 网关 + Docker Compose
│   ├── host-sidecar/           # Go 开发辅助（仅实验室，不进产品）
│   └── mirror-install/         # Mirror 驱动安装/签名脚本
├── spikes/                     # 技术探针对照（media-replace、mirror-driver）
├── scripts/                    # 构建、部署、版本号脚本
└── third_party/                # 第三方源码（miniz 等）
```

## 关键文档

| 文档 | 内容 |
|------|------|
| [CONTEXT.md](CONTEXT.md) | 产品术语表与功能边界 |
| [docs/tech-stack.md](docs/tech-stack.md) | 技术选型与工程约定 |
| [docs/dev-environment.md](docs/dev-environment.md) | 开发机/被控端/Host Sidecar 联调 |
| [docs/gateway.md](docs/gateway.md) | 网关部署、API、管理端 |
| [docs/audit-implementation-plan.md](docs/audit-implementation-plan.md) | 审计实施方案（A0–A5d 已落地） |
| [docs/viewer-implementation-plan.md](docs/viewer-implementation-plan.md) | Viewer V1–V4 实施（已完成） |
| [docs/host-os-capture-strategy.md](docs/host-os-capture-strategy.md) | Host 桌面采集 OS 分策 |
| [docs/ui-design-system.md](docs/ui-design-system.md) | Win32 UI 颜色/字体/间距规范 |
| [docs/adr/](docs/adr/) | 架构决策记录（媒体面选型、合规换芯等） |

## Host Agent 用法

```powershell
host-agent.exe [port] [password] [--no-gateway]
```

- 默认端口：38471，默认 PSK：`road-desk`
- 环境变量：`ROAD_DESK_PSK`、`ROAD_DESK_CAPTURE`（`auto` / `mirror` / `gdi` / `dxgi`）、`ROAD_DESK_TLS_INSECURE=1`（仅调试）
- Mirror 采集需要 Administrator 权限
- 首次运行弹窗配置网关；`--no-gateway` 跳过注册心跳

### 安装为 Windows 服务

被控端常驻以服务方式运行（Session 0，LocalSystem，自动启动），支持登录前远控。

```powershell
host-agent.exe --install      # 安装并自动启动服务
host-agent.exe --uninstall    # 卸载服务
```

- 服务名：`RoadDeskAgentSrv`（显示名 "Road Desk Host Agent"）
- 需要 Administrator 权限
- 服务日志写在 exe 同目录的 `host-agent.log`

### 服务管理命令

服务名 `RoadDeskAgentSrv`。以下命令均需**管理员**终端。

**CMD：**

```cmd
sc start RoadDeskAgentSrv     :: 启动
sc stop RoadDeskAgentSrv      :: 停止
sc stop RoadDeskAgentSrv & sc start RoadDeskAgentSrv   :: 重启
sc query RoadDeskAgentSrv     :: 查状态
```

**PowerShell**（注意 `sc` 在 PS 里是 `Set-Content` 别名，必须用 cmdlet 或 `sc.exe`）：

```powershell
Start-Service -Name RoadDeskAgentSrv          # 启动
Stop-Service -Name RoadDeskAgentSrv           # 停止
Restart-Service -Name RoadDeskAgentSrv        # 重启
Get-Service -Name RoadDeskAgentSrv            # 查状态
```

状态字段：`RUNNING` = 运行中，`STOPPED` = 已停止。升级覆盖 `host-agent.exe` 前先停止服务，否则文件被占用。

## Viewer 用法

```powershell
viewer.exe                          # 控制台模式（网关目录或 --demo-book）
viewer.exe host:port [password]     # CLI 直连单窗
```

- 环境变量：`ROAD_DESK_GATEWAY_URL`、`ROAD_DESK_VIEWER_KEY`（跳过登录框）、`ROAD_DESK_PSK`、`ROAD_DESK_TLS_FINGERPRINT`
- `--demo-book` 使用内置演示目录树（无需网关）

## 技术要点

- **采集分策**：NT < 6.2 → Mirror → GDI（legacy）；NT ≥ 6.2 → DXGI → GDI（modern）。详见 [host-os-capture-strategy.md](docs/host-os-capture-strategy.md)。
- **媒体面**：自研 TLS mux，不链接 LibVNC。频道：Control / Input / Video / Cursor / Clipboard / File。
- **编码**：H.264（Media Foundation）> JPEG（WIC）> zlib BGRA > raw BGRA > CopyRect。按脏矩形自适应选择。
- **剪贴板**：三通道 — 纯文本（CF_UNICODETEXT）、位图（CF_DIB，zlib）、文件（CF_HDROP，mux File 通道分块）。
- **审计**：A0–A5d 已落地，双源上报（Viewer + Host Agent），管理端可查可导出。
- **传输加密**：全程 TLS（Schannel），Host 自签证书，Viewer 校验 SHA-256 指纹。无明文路径。
- **vibe coding**：Host Agent 默认由 AI 实现与改错；小文件、边界清晰、C++17、无炫技。

## 许可

内部验证阶段。正式交付前媒体面须换合规底座（见 ADR-0002/ADR-0004）。
