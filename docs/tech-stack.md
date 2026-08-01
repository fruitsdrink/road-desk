# 技术栈

Status: **media cutover**（2026-08-01：主线已切私有 mux + Mirror/GDI；LibVNC 不再链接；回滚 tag `libvnc-mvp-verified-2026-08-01`）

Related: [CONTEXT.md](../CONTEXT.md), [ADR-0001](./adr/0001-media-plane-vnc-adapter.md), [ADR-0002](./adr/0002-mvp-gpl-then-compliant-media-base.md), [ADR-0004](./adr/0004-compliant-media-self-developed.md), [spike-media-replace.md](./spike-media-replace.md), [dev-environment.md](./dev-environment.md)

## 约束

- **Vibe coding**：Host Agent 虽用 C/C++，但默认由 AI 负责实现与改错；维护者只需具备基础 C++ 阅读能力做方向与验收，不以手写大量 C++ 为前提。
- 工程必须服务于此：小文件、边界清晰、日志可经 Host Sidecar 拉取、避免炫技式现代 C++。
- 目标 OS：Host 含 Win7 x64 / Server 2008 / 2012；Viewer 需能在 Win7 x64 运行；开发机构建在 Win11 x64。

## 已定

| 组件 | 选型 | 说明 |
|------|------|------|
| **Host Agent** | **C/C++（原生 Win32）** | 便于接 VNC 媒体面适配器；与 Sidecar 语言可分离 |
| **Viewer** | **C/C++ / 原生 Win32 管理台** | 无额外运行时；Win7 可跑。壳为 Radmin 式管理台（菜单/工具栏/左树/工作区/状态栏）+ 浏览器式会话 Tab（可拖出独立窗）；画面路径保持 C++/GDI + `MediaClient`。不用 Electron；不用 C#/.NET；**不上 Qt**（Qt6 仅规划给未来 Win10+ 产品线，不进 Win7 构建） |
| **Host Sidecar** | **Go，工具链钉死 1.20.x** | 单文件 HTTP 工具。`GOOS=windows GOARCH=amd64`；**禁止用 Go 1.21+ 打 Win7 用 Sidecar**（1.20 为官方最后支持 Win7/2008/2012 的系列，建议锁定最后补丁如 1.20.14）。开发机可另装更新 Go 编别的东西，Sidecar 构建必须走 1.20.x |
| **C++ 工具链** | **MSVC 2022 + CMake，x64** | Agent/Viewer 主线。静态链 CRT 或附带 VC++ 可再发行组件；真 Win7 验证。若某 VNC 适配器强迫特定生成方式再开例外 |
| **媒体面** | **自研 TLS mux + Mirror/GDI**（`src/media/mux_*`） | 经 `media_plane.h`；默认 `ROAD_DESK_CAPTURE=auto`。不链接 LibVNC（历史源可留仓、不编）。Mirror Host 需管理员。见 ADR-0004 / spike-media-replace RESULTS |
| **传输加密** | **Schannel TLS + 预共享口令（PSK）** | 保密与鉴权分离；`src/media/tls_schannel.*`。mux **无明文路径**（`ROAD_DESK_ALLOW_PLAINTEXT` 会使 listen 失败）。Host 自签证书，Viewer 校验 SHA-256 指纹；调试可 `ROAD_DESK_TLS_INSECURE=1` |
| **仓库布局** | **单仓** | 产品 C++ 与 `tools/host-sidecar`（Go）同仓；见下方目录约定 |

## 仓库目录约定（单仓）

```text
road-desk/
  CONTEXT.md
  docs/                 # ADR、探针、开发环境、本技术栈
  src/
    agent/              # Host Agent（C++）
    viewer/             # Viewer（C++）
    media/              # mux + TLS 媒体面（可替换边界 media_plane.h）
    session/            # 控制面：互斥、预共享口令、会话（无中心）
  tools/
    host-sidecar/       # Go 1.20.x，独立 go.mod
  spikes/               # 探针对照（media-replace、mirror-driver）
```


MVP 不强行做巨型 `common` 静态库；协议字段用小范围共享头/文档约定即可。

## 工程约定（服务 vibe coding）

- 控制面（`session/`）与媒体面适配器（`media/`）目录分开；AI 改媒体面时不顺手改乱控制面
- 坚持 Host Agent 文件日志 + Sidecar `/logs`，便于 Win11 上 AI 读 Host 侧证据
- Agent/Viewer：MSVC 2022 + CMake 单一主线；不在 MVP 引入多生成器宇宙
- Sidecar：`go.mod` / CI / 文档写明 `toolchain go1.20.x`；Win7 真机冒烟必做，防止误用新 Go 编译
- 全程 vibe coding：实现默认由 AI 完成；维护者做方向、联调验收与真 Win7 门禁
