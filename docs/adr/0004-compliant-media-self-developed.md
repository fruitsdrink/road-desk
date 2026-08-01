# 合规媒体面：自研适配器实现（非 RFB、非商业 Mirror）

Status: accepted（2026-08-01）

Related: [ADR-0001](./0001-media-plane-vnc-adapter.md), [ADR-0002](./0002-mvp-gpl-then-compliant-media-base.md), [ADR-0003](./0003-tech-stack-mvp.md), [spike-media-replace.md](../spike-media-replace.md), [`media_plane.h`](../../src/media/media_plane.h)

ADR-0002 要求正式交付前去掉 GPL 等 copyleft 媒体依赖。媒体面探针（[`spikes/media-plane/RESULTS.md`](../../spikes/media-plane/RESULTS.md)）总判「继续 A」：MVP 内部可继续用 LibVNC，交付前必须替换。本 ADR 钉死替换路径——**同仓 C/C++ 自研媒体面适配器实现**，而不是采购商业 VNC/Mirror SDK，也不是把 Agent/Viewer 迁到 Go/Rust。

## Decision

1. **替换目标**：去掉 LibVNCServer/LibVNCClient；正式包不链接 GPL 媒体栈。
2. **协议**：私有帧/消息协议（非 RFB）；**不兼容**第三方 VNC Viewer；自带 Viewer。
3. **传输**：复用现有 **Schannel TLS** 包裹；默认 **单 TLS 连接上逻辑 mux**（Video / Input / Control；File、Clipboard 后期）。发送调度 Input/Control 优先于 Video。
4. **API 边界**：保住 [`media_plane.h`](../../src/media/media_plane.h) 的会话语义（listen/serve、client start/stop、帧拷贝、指针/键盘）；实现可演进，控制面（PSK、`SessionMutex`）不重写。
5. **语言与工具链**：**继续 C/C++（MSVC 2022 + CMake）**；不把媒体面迁到 Go 或 Rust（见 spike 文档「语言选型」）。
6. **采集**：近中期 **仅用户态 GDI**；**放弃采购**商业 Mirror/采集 SDK；**自研 Mirror 靠后**、另立项，不进换芯探针范围。
7. **手感承诺**：换芯硬指标为相对现 LibVNC 路径 **拖窗可感知更跟手（档 B）**；不承诺 ToDesk/向日葵同级（档 C）。
8. **工程落点**：先在 `spikes/media-replace/` 做可扔掉探针；达标后再切入 `src/media/`，主线 LibVNC 路径可并行对照直至切换门禁通过。

## Considered Options

- **采购商业 Mirror / VNC SDK** — 拒绝（近中期）。询价路径（UltraVNC Mirror、DFMirage 等）不启动；不把交付绑在第三方驱动授权与签名链上。
- **开源 Mirror（WDK 样例 / VNC 附带 / GitHub 改版）** — 拒绝进正式包。要么是样例（≈自研起点），要么 GPL/许可不清，要么是 Win10+ 虚拟屏而非 Win7 镜像采集。
- **媒体面用 Go 1.20 重写** — 拒绝。Sidecar 用 Go 合适；实时媒体忌 GC、Win7 钉死 1.20；GDI/Schannel/注入仍要大量 Win32，换语言不省边界成本。
- **媒体面用 Rust** — 拒绝。仓库无 Rust；Win7 工具链不可靠；与 vibe coding /「维护者可读基础 C++」单主线冲突。
- **自研 C++ 适配器 + GDI + TLS mux** — **采纳**为合规替换与换芯探针路径。
- **一上来自研 Mirror Driver** — 拒绝进近中期。历史全量自研失败与驱动成本高；仅当 GDI+管线仍达不到可接受档 B、且产品明确要冲档 C 时再单独立项。

## Consequences

- MVP 阶段可继续链接 LibVNC（内部验证）；装站/对外交付前必须完成本 ADR 路径或经书面批准的等价合规底座。
- 第三方 VNC 客户端不再是验收或联调手段；对照基线改为 **同机构建的现 LibVNC MVP 路径**。
- 文件传输继续按 RESULTS P4：**自有通道**，不绑 RFB 扩展；换芯探针 P0 可不实现 File，但 mux 预留 channel id。
- 登录前/锁屏仍按 RESULTS P3：**目标阶段单列**；换芯探针不承诺登录前。
- 拖窗粉紫拖影等 GDI/overlay 固有现象不单独构成换芯失败（与 media-plane 探针一致）；根治不在本 ADR 范围。
- 实施节奏与验收见 [spike-media-replace.md](../spike-media-replace.md)；结论记入 [`spikes/media-replace/RESULTS.md`](../../spikes/media-replace/RESULTS.md)。
