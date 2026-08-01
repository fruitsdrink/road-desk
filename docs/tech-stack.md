# 技术栈

Status: **frozen**（MVP 选型已冻结；TLS 库与 LibVNC 精确版本由 Win7 探针钉死）

Related: [CONTEXT.md](../CONTEXT.md), [ADR-0001](./adr/0001-media-plane-vnc-adapter.md), [ADR-0002](./adr/0002-mvp-gpl-then-compliant-media-base.md), [dev-environment.md](./dev-environment.md)

## 约束

- **Vibe coding**：Host Agent 虽用 C/C++，但默认由 AI 负责实现与改错；维护者只需具备基础 C++ 阅读能力做方向与验收，不以手写大量 C++ 为前提。
- 工程必须服务于此：小文件、边界清晰、日志可经 Host Sidecar 拉取、避免炫技式现代 C++。
- 目标 OS：Host 含 Win7 x64 / Server 2008 / 2012；Viewer 需能在 Win7 x64 运行；开发机构建在 Win11 x64。

## 已定

| 组件 | 选型 | 说明 |
|------|------|------|
| **Host Agent** | **C/C++（原生 Win32）** | 便于接 VNC 媒体面适配器；与 Sidecar 语言可分离 |
| **Viewer** | **C/C++ 薄 UI（原生 Win32）** | 无额外运行时；Win7 可跑；本地壳仅为连接栏 + 远端画面。不用 Electron（与 Win7 冲突）；不用 C#/.NET（避免现场装运行时） |
| **Host Sidecar** | **Go，工具链钉死 1.20.x** | 单文件 HTTP 工具。`GOOS=windows GOARCH=amd64`；**禁止用 Go 1.21+ 打 Win7 用 Sidecar**（1.20 为官方最后支持 Win7/2008/2012 的系列，建议锁定最后补丁如 1.20.14）。开发机可另装更新 Go 编别的东西，Sidecar 构建必须走 1.20.x |
| **C++ 工具链** | **MSVC 2022 + CMake，x64** | Agent/Viewer 主线。静态链 CRT 或附带 VC++ 可再发行组件；真 Win7 验证。若某 VNC 适配器强迫特定生成方式再开例外 |
| **媒体面 VNC 库** | **LibVNC（Server + Client）作适配器** | 不整包 Fork UltraVNC。体验对照用经典 WinVNC 安装版。内部验证许可见 ADR-0002；探针 P1 在真 Win7 锁定可用版本 |
| **传输加密** | **Schannel TLS 包裹 + 预共享口令（PSK）** | 保密与鉴权分离；不依赖 VNC/VeNCrypt/OpenSSL。MVP 在 `src/media/tls_schannel.*` 用系统 Schannel 包裹 RFB TCP（Win7 友好，免随包 OpenSSL DLL）。默认无明文；本机调试可设 `ROAD_DESK_ALLOW_PLAINTEXT=1`。Host 自签证书，Viewer 默认校验 SHA-256 指纹（`ROAD_DESK_TLS_FINGERPRINT`）；调试可 `ROAD_DESK_TLS_INSECURE=1` |
| **仓库布局** | **单仓** | 产品 C++ 与 `tools/host-sidecar`（Go）同仓；见下方目录约定 |

## 仓库目录约定（单仓）

```text
road-desk/
  CONTEXT.md
  docs/                 # ADR、探针、开发环境、本技术栈
  src/
    agent/              # Host Agent（C++）
    viewer/             # Viewer（C++）
    media/              # VNC/TLS 媒体面适配器（可替换边界）
    session/            # 控制面：互斥、预共享口令、会话（无中心）
  tools/
    host-sidecar/       # Go 1.20.x，独立 go.mod
  third_party/          # LibVNC 等（或 submodule），许可备注
```

MVP 不强行做巨型 `common` 静态库；协议字段用小范围共享头/文档约定即可。

## 工程约定（服务 vibe coding）

- 控制面（`session/`）与媒体面适配器（`media/`）目录分开；AI 改媒体面时不顺手改乱控制面
- 坚持 Host Agent 文件日志 + Sidecar `/logs`，便于 Win11 上 AI 读 Host 侧证据
- Agent/Viewer：MSVC 2022 + CMake 单一主线；不在 MVP 引入多生成器宇宙
- Sidecar：`go.mod` / CI / 文档写明 `toolchain go1.20.x`；Win7 真机冒烟必做，防止误用新 Go 编译
- 全程 vibe coding：实现默认由 AI 完成；维护者做方向、联调验收与真 Win7 门禁
