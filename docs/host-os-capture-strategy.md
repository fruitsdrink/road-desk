# Host OS 分策采集：实施方案

Status: **S0+S1+S2 已落地**（分支 `feature/host-os-capture-strategy`；2026-08-04）

Related: [tech-stack.md](./tech-stack.md), [ADR-0004](./adr/0004-compliant-media-self-developed.md), [spike-media-replace.md](./spike-media-replace.md), [避坑清单](./spike-media-replace-pitfalls.md), [Mirror 共存](../spikes/mirror-driver/docs/coexistence.md)

## 1. 背景与目的

现场 Host **同时存在** Win7 与 Win10/11 为常态；`tech-stack` 目标 OS 含 **Win7 / Server 2008 / Server 2012**。

当前 Host 无 OS 分支：`ROAD_DESK_CAPTURE=auto` 一律试 XPDM Mirror，失败再 GDI。结果：

| 环境 | 实际路径 | 问题 |
|------|----------|------|
| Win7 / 2008 R2 | Mirror（理想）或 GDI | 已验证主路径 |
| **Server 2008（NT 6.0）** | 应走 Mirror/GDI | 若误判为「非 Win7 → DXGI」会挂 |
| Win8+ / Server 2012+ | Mirror 几乎不可用 → **整屏 GDI** | 脏区优势丢失，CPU/拖窗差 |

目的：Host **按 OS 能力分策**，在 **不破坏 Win7 / Server 2008 门禁** 的前提下，为 Win8+ 准备 DXGI 脏区路径，并立刻去掉 modern 上无用的 Mirror 尝试。

本方案 **不改变** mux 协议、Viewer、编码格式（短期内仍 zlib/raw）；只改 Host 采集后端选择与实现。

## 2. 已拍板约束（实施不得再议）

| 项 | 决定 |
|----|------|
| 分界线 | **NT ≥ 6.2 → modern**；**NT &lt; 6.2 → legacy**（**不是**「是否 Win7」） |
| Server 2008 | **NT 6.0，属 legacy**；与 Win7 同默认策略（Mirror→GDI）；**禁止 DXGI** |
| Server 2008 R2 / Win7 | NT 6.1，legacy |
| Server 2012 / Win8+ | modern；默认 DXGI（就绪后），失败 GDI；**默认不试 XPDM Mirror** |
| Win7 门禁 | 仍以 Win7+Mirror 为档 C′ 主验收；Win11 冒烟不得代替（避坑 G1） |
| DXGI 进仓方式 | **运行时探测 + 动态加载（或独立 TU）**；Win7/2008 进程 **永不进入 DXGI 代码路径**；满足避坑 B7/G5 精神 |
| 环境变量覆盖 | `ROAD_DESK_CAPTURE` 仍可强制 `mirror` / `gdi` /（后续）`dxgi` / `auto` |
| 协议 / Viewer | 不变；仍收脏矩形 + BGRA |
| 多屏 / 硬编 / 音频 / Per-Monitor V2 | **本方案不做** |
| 提权 | legacy+Mirror 仍 `requireAdministrator`；modern 仅 GDI/DXGI 时是否降权 **另议**，本方案 P0 可保持现状以免分叉行为 |

## 3. 策略矩阵

```text
RtlGetVersion → (major, minor, build)
        │
        ├─ NT < 6.2  →  strategy = legacy
        │                 auto: Mirror → GDI
        │                 禁止加载 DXGI
        │
        └─ NT ≥ 6.2  →  strategy = modern
                          auto: DXGI → GDI
                          跳过 XPDM Mirror（除非 env 强制 mirror）
```

| 策略档 | OS 例 | `auto` 默认链 | 禁止 |
|--------|-------|---------------|------|
| **legacy** | Server 2008、Win7、2008 R2 | Mirror → GDI | DXGI |
| **modern** | Server 2012、Win8/8.1、Win10/11 | DXGI → GDI | 默认不 Attach Mirror |

强制覆盖：

| `ROAD_DESK_CAPTURE` | legacy | modern |
|---------------------|--------|--------|
| `auto` | 上表 | 上表 |
| `mirror` | 必须 Mirror，失败可按现逻辑 | 允许尝试（排障），预期多失败 |
| `gdi` | 强制 GDI | 强制 GDI |
| `dxgi`（新增） | **拒绝并 WARN，回退 GDI** | 必须 DXGI，失败再定（建议 fail 或 GDI，日志标明） |

## 4. 阶段与门禁

### 阶段 0 — 探测与策略选择（无换采屏后端）

| 做什么 | 验收 |
|--------|------|
| `os_version` 小模块：`RtlGetVersion`（ntdll），失败则保守 **legacy** + WARN | 单元/日志：6.0 / 6.1 → legacy；6.2+ / 10.0 → modern |
| `HostCaptureStrategy` + 解析 `ROAD_DESK_CAPTURE` | 日志一行：`os=6.1.7601 strategy=legacy capture=auto resolved=mirror\|gdi` |
| **modern + auto：跳过 Mirror 尝试**，直接 GDI | Win10 Host：`host-agent.log` 无 Mirror Attach/CDS；无主题闪 Basic |
| 文档：本文件 + tech-stack / 避坑交叉引用 | 明确 Server 2008 = legacy |

**出门 Gate S0：** Win7 行为与现网一致（仍可 Mirror）；Win10 auto 不再空试 Mirror。

### 阶段 1 — 管线减压（与 OS 无关，建议同分支或紧随）

| 做什么 | 验收 |
|--------|------|
| Video：**一编多发**（zlib 一次，fan-out 已压缩缓冲） | 2 Viewer 时 Host encode CPU 明显低于「×N」；协议帧内容不变 |
| （可选）GDI/空闲节奏：无脏时空闲间隔放宽 | 空桌面 BitBlt 频率下降，拖窗仍跟手 |

**出门 Gate S1：** 多 Viewer 压力可测；不回归单 Viewer 手感。

### 阶段 2 — modern DXGI Desktop Duplication

| 做什么 | 验收 |
|--------|------|
| 新后端实现脏矩形 + 像素，对齐 `SessionCapture` 语义（或并行 `DxgiCapture` 由 Session 调度） | Win10：`backend=dxgi`；拖窗相对 GDI 可感知更好 |
| DXGI 仅在 `strategy==modern` 且解析为 dxgi/auto 时加载 | Win7 / Server 2008：**进程内无 DXGI import 依赖导致启动失败**（动态加载验证） |
| 失败回退 GDI，会话不掉 | 拔显卡输出 / 桌面切换等：日志 + GDI 续跑 |
| 主屏 only（与现 Mirror/GDI 一致） | 坐标仍 = 主屏帧缓冲像素 |

**出门 Gate S2：**

- legacy：Win7 + Server 2008（至少其一真机）Mirror 或 GDI 冒烟通过  
- modern：Win10/11 `capture=dxgi` 看屏+点选通过；相对同机 GDI 拖窗达 **档 B′**（modern 侧自比）  
- 不以 Win11 DXGI 结论代替 Win7 Mirror 档 C′

### 阶段 3 — 产品化收尾（可另 PR）

- modern 是否仍强制管理员（Mirror scrub 不再需要时）  
- Agent manifest `supportedOS` 补 Win8 GUID（若嵌入清单）  
- ADR 修订：采集后端 = legacy Mirror / modern DXGI / 共用 GDI  
- 更新避坑 B7/G5 措辞：允许 modern 路径，禁止写入 legacy 二进制依赖  

## 5. 代码落点（预期）

| 区域 | 文件（预期） | 变更 |
|------|--------------|------|
| OS 探测 | 新建 `src/media/os_version.h/.cpp`（或 `src/agent`） | `RtlGetVersion`；`is_legacy_capture_host()` |
| 模式解析 | `mirror_capture.h` / `mux_host.cpp` | `CaptureMode` 增 `Dxgi`；`auto` 按 strategy 解析 |
| 会话采集 | `mirror_capture.*` | modern auto 跳过 `begin_mirror`；接入 DXGI |
| DXGI | 新建 `src/media/dxgi_capture.*` | Output Duplication；动态 `dxgi.dll` / `d3d11.dll` |
| 编码 fan-out | `mux_host.cpp` | 共享压缩缓冲 |
| 构建 | `src/media/CMakeLists.txt` | DXGI TU；**勿**对 Win7 静态强链导致无法启动（动态加载优先） |
| 文档 | 本文件、`tech-stack.md`、避坑交叉链、可选 ADR-0005 |

**保持不变：** `media_plane.h` 会话语义、TLS/PSK、注入坐标约定、Viewer。

## 6. 检测实现要点

```text
typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
// ntdll!RtlGetVersion — 不受清单谎报影响（仍建议 Agent 嵌入 supportedOS）

legacy  := !(major > 6 || (major == 6 && minor >= 2))
modern  := !legacy

例：
  6.0.*  Server 2008 / Vista     → legacy
  6.1.*  Win7 / 2008 R2          → legacy
  6.2.*  Win8 / 2012             → modern
  6.3.*  8.1 / 2012 R2           → modern
  10.0.* Win10/11 / Server 2016+ → modern
```

探测失败 → **legacy** + `WARN os_version_failed strategy=legacy`（宁可不用 DXGI，也不要在 2008 上误开）。

## 7. 风险与明确不做

| 风险 | 缓解 |
|------|------|
| 把「非 Win7」当成 modern，误伤 Server 2008 | 分界用 **6.2**，文档与测试列出 6.0 |
| DXGI 静态链接导致 Win7 无法加载 | 动态加载；legacy 零调用 |
| modern 上强制 Mirror 破坏现场 | auto 跳过；仅 env 强制 |
| 用 Win11 结论替代 Win7 | Gate 分列 legacy / modern |
| DXGI 与安全桌面 / 会话切换 | 失败回退 GDI；登录前仍非本阶段目标 |

**不做：** WGC、H.264/NVENC、多监视器虚拟桌面、音频、采购商业 Mirror、改 Viewer 协议。

## 8. 验收环境清单

| # | Host | 期望 strategy / backend |
|---|------|-------------------------|
| 1 | Win7 x64 + Mirror 已装 | legacy / mirror |
| 2 | Server 2008 x64（或明确书面延期） | legacy / mirror 或 gdi |
| 3 | Win10/11 x64 | modern / dxgi（S2 后）或 gdi（S0–S1） |
| 4 | Server 2012+（有则测） | modern |
| 5 | 环境变量强制 `gdi` / `mirror` | 覆盖生效且打日志 |

## 9. 建议实施顺序（PR 切片）

1. **PR-A（S0）：** OS 探测 + strategy 日志 + modern 跳过 Mirror（行为变更小、立刻减痛）  
2. **PR-B（S1）：** encode 一编多发  
3. **PR-C（S2）：** DXGI 后端 + auto 接入 modern  
4. **PR-D（S3）：** 文档/ADR/提权策略收口  

## 10. 当前分支状态

- 分支：`feature/host-os-capture-strategy`（自 `dev`）
- **S0：** `os_version` + `capture_resolve`；modern 跳过 Mirror
- **S1：** `video_encode` 一编多发
- **S2：** `dxgi_capture`（动态加载 dxgi/d3d11）；modern+auto/`dxgi` → DXGI，失败回退 GDI；`provides_dirties()` 接入 mux
- 单元测试：`capture_strategy_test`
- 下一步：真机 Gate（Win10 `capture started dxgi`；Win7 仍 Mirror）→ S3 文档/提权收口
