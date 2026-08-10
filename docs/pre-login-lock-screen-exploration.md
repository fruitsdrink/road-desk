# 锁屏远程登录 — 探索记录与技术结论

Status: **暂停投入**（2026-08-10 结论：Win7 锁屏捕获需内核级方案，成本高；Win10 目标 2027 升迁，届时重新评估）

Related: [CONTEXT.md](../CONTEXT.md) §登录前远控, [host-os-capture-strategy.md](./host-os-capture-strategy.md), [ADR-0004](./adr/0004-compliant-media-self-developed.md), [Mirror 共存](../spikes/mirror-driver/docs/coexistence.md), [登录前远控实现记录（S1–S5）](../memory/road-desk-pre-login-remote-control.md)

## 1. 目标

实现「系统锁屏 / 登录界面下，Viewer 仍可查看画面并注入输入（如输入凭据解锁）」。这是 CONTEXT.md 中「登录前远控」的核心场景。

**当前结论：Win7 上锁屏画面捕获在用户态 API 范围内不可行。** 见 §5。已投入的探索、验证和工具见下。

## 2. 已确认的技术事实（全部在真机 VM 验证）

### 2.1 锁屏画面的位置与权限

- 锁屏画面渲染在 **Winlogon 安全桌面**（Secure Desktop）。
- Winlogon 桌面运行于 **system IL**（比管理员 High IL 更高一级）。
- 微软官方文档明确：**用户态应用（含 UIAccess）无法访问 system IL 的 UI**，只有「在 UAC 桌面下以 SYSTEM（system IL）启动的进程」才可以。

### 2.2 已验证失败的路径（VM: Win7 x64）

| 路径 | 测试方式 | 结果 |
|------|----------|------|
| GDI 捕获（前台管理员，锁屏时） | `GetDC(nullptr)` + `BitBlt` | `err=6`（无效句柄），0 帧 |
| Mirror 驱动（XPDM，预先 attach 后锁屏） | `EngLockSurface` + `pvBits` | 帧缓冲不更新（Winlogon 绘制不经用户会话镜像设备） |
| `OpenDesktop("Winlogon")`（前台管理员） | 桌面探针 `desktop_probe` | `err=5`（ACCESS_DENIED） |
| `OpenDesktop("Winlogon")`（SYSTEM + 任务计划） | 计划任务以 SYSTEM 运行 | `err=2`（当前窗口站无该桌面；任务计划跑在非交互上下文） |
| 跨会话 `OpenWindowStation(\Sessions\1\...\WinSta0)` | 服务模式 | `err=161`（窗口站不能跨会话用路径打开） |
| `SetTokenInformation(TokenSessionId)`（SYSTEM 服务改自身 token） | 服务模式 | `err=1375`（token 已存在，不能改 SessionId） |

### 2.3 为什么这些都会失败

- **锁屏 = 切换到 Winlogon 安全桌面**，该桌面属 system IL。
- **用户会话的镜像驱动 / GDI DC** 只覆盖「普通桌面」；锁屏后绘制切到安全桌面，不再经过这些捕获路径。
- **要访问安全桌面**，进程必须同时满足：SYSTEM 权限（system IL）+ 在正确的交互会话窗口站。
- **这两个条件的组合**无法通过用户态 API 达成：
  - 管理员 + 交互会话 → 权限不够（High IL < system IL）
  - SYSTEM + Session 0 → 不在交互窗口站
  - SYSTEM 改 token 进交互会话 → SetTokenInformation 限制（1375）

## 3. 成熟产品如何实现（调研结论）

| 产品 | 方案 |
|------|------|
| **RDP / 终端服务** | **内核级显示驱动**（终端服务显示驱动），在系统栈直接推画面，不依赖 OpenDesktop/BitBlt |
| **TeamViewer / AnyDesk / Radmin** | 自研**内核显示驱动** + SYSTEM 服务注入交互会话的组合 |

共同点：**锁屏画面只能通过内核级显示驱动捕获**（在显示链路上直接取帧），用户态捕获 API 无法到达。

## 4. 我们已有的资产与可复用部分

| 资产 | 状态 |
|------|------|
| **XPDM Mirror 驱动**（`spikes/mirror-driver`） | 已能捕获普通桌面（未锁屏）。锁屏时帧缓冲不更新——因为镜像设备挂接在用户会话显示链，Winlogon 绘制不经它。**是驱动改造的基础。** |
| **Session Helper 注入通道（S4）** | 服务用 `WTSQueryUserToken` + `CreateProcessAsUser` 注入交互会话。但注入的是**用户 token**（非 SYSTEM），锁屏时打不开 Winlogon。 |
| **desktop-probe 工具** | `tools/desktop-probe`：桌面/窗口站/权限探针，已含服务模式、跨会话窗口站测试、SYSTEM token 会话注入尝试。留作后续验证。 |
| 引擎安全读帧（`EngLockSurface`+`pvBits`） | Mirror 驱动已改用引擎安全方式读帧，消除了之前的蓝屏（`rdmdisp.dll`）。 |

## 5. 可行性结论与方向

### 5.1 用户态路径（已判死刑）

在 Win7 上，**锁屏画面捕获无法通过用户态 API 实现**。GDI / Mirror / OpenDesktop / 任务计划 SYSTEM / token 注入 均已验证不可行。

### 5.2 内核级路径（唯一确定可行，但成本高）

改造 XPDM Mirror 驱动，让镜像设备挂接到**能覆盖锁屏的显示链**（即锁屏时驱动仍收到绘制回调）。这需要深入理解 Win7 锁屏时显示输出的实际走向，且 XPDM 是旧模型，资料少、调试难。TeamViewer/Radmin 正是这么做的。

**工作量评估：大**（驱动改造 + 装站 + 锁屏场景调试），且 Win7 平台本身即将被替换。

### 5.3 替代方向

- **升级 Win10**（目标 2027）：Win10 有更成熟的方案（如 indirect display driver、`OpenInputDesktop` 结合 UIAccess 的行为差异），锁屏捕获可行性更高。
- **折中方案**：锁屏后仍保持已建立的远控会话（不断开），只接受「不能看到锁屏画面」，解锁后继续。但这不满足「远程解锁」需求。

## 6. 决策

**2026-08-10 决定：暂停锁屏登录投入。**
- 主要部署环境是 Win7，锁屏捕获需内核级方案，成本与平台生命周期不匹配。
- 明年升 Win10 后重新评估（届时检查 indirect display driver / WDDM 方案）。
- 已投入的驱动增强（引擎安全读帧）、Session Helper、desktop-probe 工具保留，不返工。

## 7. 后续线索（给 Win10 阶段的调研）

- Win10 `WDDM` 的 **indirect display driver** / 桌面复制 API 是否覆盖安全桌面。
- RDP 的终端服务显示驱动实现细节。
- UIAccess 在 Win10 锁屏场景的真实行为（文档说 system IL 不可达，但需实测确认）。

## 8. 验证工具

- `tools/desktop-probe/desktop_probe.exe`：桌面/窗口站/权限/会话注入探针。
  - `--service`：以 LocalSystem 服务运行（Session 0）。
  - `--inject <session>`：SYSTEM token 注入目标会话。
  - `--probe-desktop`：注入后子进程模式，测 Winlogon 桌面访问与 BitBlt。
- 结果写入 exe 同目录 `desktop_probe.txt` / `desktop_probe_child.txt`。
- 依赖 MSVC 编译（避免 MinGW 运行时 DLL 缺失）。
