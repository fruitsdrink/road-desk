# 合规媒体面：自研适配器 + 自研 Mirror（非 RFB、非商业 Mirror SDK）

Status: accepted（2026-08-01；**同日修订**：自研 Mirror 由「靠后」改为 **提前并行**）

Related: [ADR-0001](./0001-media-plane-vnc-adapter.md), [ADR-0002](./0002-mvp-gpl-then-compliant-media-base.md), [ADR-0003](./0003-tech-stack-mvp.md), [spike-media-replace.md](../spike-media-replace.md), [`media_plane.h`](../../src/media/media_plane.h)

ADR-0002 要求正式交付前去掉 GPL 等 copyleft 媒体依赖。媒体面探针总判「继续 A」：MVP 内部可继续用 LibVNC，交付前必须替换。本 ADR 钉死替换路径——**同仓 C/C++ 自研媒体面（私有协议 + TLS mux）**，并 **提前自研 Win7 可用的 Mirror 采集驱动**；不采购商业 Mirror/VNC SDK，不把 Agent/Viewer 迁到 Go/Rust。

## Decision

1. **替换目标**：去掉 LibVNCServer/LibVNCClient；正式包不链接 GPL 媒体栈。
2. **协议**：私有帧/消息协议（非 RFB）；**不兼容**第三方 VNC Viewer；自带 Viewer。
3. **传输**：复用现有 **Schannel TLS** 包裹；默认 **单 TLS 连接上逻辑 mux**（Video / Input / Control；File、Clipboard 后期）。发送调度 Input/Control 优先于 Video。
4. **API 边界**：保住 [`media_plane.h`](../../src/media/media_plane.h) 的会话语义；控制面（PSK、`SessionMutex`）不重写。采集后端可切换：GDI（过渡）→ **自研 Mirror**（交付手感目标）。
5. **语言与工具链**：**继续 C/C++（MSVC 2022 + CMake）** 做用户态；Mirror 驱动按 WDK/XPDM 工具链（与 Agent 分目录、分构建），仍不引入 Go/Rust 媒体面。
6. **采集（2026-08-01 修订）**：
   - **放弃采购**商业 Mirror/采集 SDK（UltraVNC Mirror、DFMirage 等询价路径不启动）。
   - **自研 Mirror 提前**：进入近中期主路径，与换芯 mux **并行**，不再「靠后另议」。
   - GDI 仅作 mux 联调与 Mirror 未就绪时的回退，**不是**上道手感终态。
7. **手感承诺（修订）**：
   - 档 A：合规可交付（必须）。
   - 档 B：相对现 LibVNC（GDI）拖窗可感知更跟手（mux 管线；中间门禁）。
   - 档 **C′（产品）**：同机相对 **现场已用 Mirror 的 VNC/Radmin**，拖窗手感不被明显 pass——**依赖自研 Mirror 达标**；不对标公网 ToDesk 全特性。
8. **工程落点**：`spikes/media-replace/`（协议/管线）+ `spikes/mirror-driver/`（或同 spike 下驱动子树）；G5 切入 `src/media/` 前 **不得**覆盖 `libvnc_*`。LibVNC 备份：`backup/libvnc-mvp-verified` / tag `libvnc-mvp-verified-2026-08-01`。

## Considered Options

- **采购商业 Mirror / VNC SDK** — 仍拒绝。现场虽常已装第三方 Mirror，正式包不绑其授权与签名链。
- **开源 Mirror 直接进正式包** — 仍拒绝（GPL/许可不清）；WDK `mirror` 样例仅作 **自研起点**，不整包当产品。
- **仅 GDI + mux、永不做 Mirror** — **撤回为终态**。生产无「无 Mirror」对标环境；不上 Mirror 则拖窗手感会被 VNC/Radmin pass。
- **自研 Mirror 靠后** — **撤回**。改为与换芯协议并行提前。
- **媒体面用 Go / Rust** — 仍拒绝（见既有理由）。
- **自研 C++ 适配器 + TLS mux + 自研 Mirror** — **采纳**为近中期交付路径。

## Consequences

- MVP 可继续用 LibVNC（内部）；装站/对外交付前须完成：无 GPL 媒体栈 + **自研 Mirror 可装可跑**（或产品书面接受 GDI 上限——默认不接受）。
- 现场 OS 上已有 VNC/Radmin Mirror **≠** Road Desk 可调用；必须自有驱动与用户态对接。
- 驱动带来：测试签名/正式签名、安装卸载、蓝屏风险、与已装第三方 Mirror 共存策略——须在 Mirror 探针 RESULTS 单列。
- **Host 管理员权限（交付必须处理）：** Mirror 会话需提升进程才能 scrub HKLM `Attach.ToDesktop`；非管理员开设备管理器会导致键鼠失效（2026-08-01 对照）。切入主线时须产品化提权（清单 `requireAdministrator` / 服务账户 / 启动器 UAC），不得依赖手工「以管理员运行」。见 [`coexistence.md`](../../spikes/mirror-driver/docs/coexistence.md)。
- 登录前/锁屏仍可受益于 Mirror，但 **目标阶段单列验收**；换芯 P0 仍先保已登录桌面。
- 实施节奏见 [spike-media-replace.md](../spike-media-replace.md)；结论记入 [`spikes/media-replace/RESULTS.md`](../../spikes/media-replace/RESULTS.md) 与 Mirror 子结论页。
