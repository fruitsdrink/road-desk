# MVP 技术栈：C++ Agent/Viewer、Go 1.20 Sidecar、LibVNC 适配器、TLS 包裹

Status: accepted

在 Win7 x64 Host、Win11 开发机、全程 vibe coding 的约束下，MVP 冻结如下：Host Agent 与 Viewer 使用 C/C++（MSVC 2022 + CMake，x64，Viewer 为薄原生 UI）；Host Sidecar 使用 Go 且工具链钉死 1.20.x（禁止 1.21+ 打 Win7 包）；媒体面以 LibVNC 为可替换适配器并对照经典 WinVNC；传输采用 TLS 包裹 + 预共享口令鉴权；代码单仓布局（`src/*` + `tools/host-sidecar`）。不采用 Electron Viewer（与 Win7 冲突），不整包 Fork UltraVNC。TLS 具体库与 LibVNC 精确版本由 Win7 探针确认后写入 `docs/tech-stack.md`，不因此重新打开整栈讨论。

## Considered Options

- Viewer：Electron / C# — 因 Win7 或运行时成本放弃
- Sidecar：与 Agent 同用 C++，或 Go 新版本 — 因工具效率与 Win7 兼容放弃后者新版本
- 媒体面：UltraVNC 整包 / 仅外挂 WinVNC 进程 — 与可替换适配器策略冲突或不可控
