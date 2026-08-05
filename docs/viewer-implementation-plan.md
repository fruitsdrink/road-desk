# Viewer 后续实施方案

Status: **active**（2026-08-05）  
Related: [CONTEXT.md](../CONTEXT.md), [ui-design-system.md](./ui-design-system.md), [gateway.md](./gateway.md), [design/README.md](./design/README.md)

把 Viewer 从「可联调远控」收成「可交付的操作端」：菜单接活、会话韧性、管理台与登录页同一产品语言。媒体协议与 Host 采集不在本方案主线（见 ADR / capture 文档）。

## 1. 范围

### 1.1 做

| 批次 | 主题 | 结果 |
|------|------|------|
| **V1** | 管理台命令闭环 | ✅ 菜单与工具栏同一套命令；灰显占位已去掉 |
| **V1b** | 壳层全键盘 | ✅ Tab/F6/加速键；会话抢键交还不做 |
| **V2** | 会话断线重连 | ✅ 非主动断开自动重连 + 退避；鉴权失败停止 |
| **V3** | UI 规范收口 | ✅ `rd_tokens`/`rd_dpi`；登录+console+Agent 共用；主窗 `rd_app.ico` |
| **V4** | 壳层交互补齐 | 分割条宽度芯片（退出确认 /「关于」已在 V1） |

### 1.2 明确不做 / 暂不实现

| 项 | 依据 |
|----|------|
| **新建连接…** 对话框 | 已拍板去掉；入口仅为目录双击 / 工具栏会话 / CLI `host:port` |
| **多显示器选屏 / 传全部屏** | `CONTEXT.md` 主屏远控：暂不实现、未排期 |
| 独立传文件 UI | 剪贴板 HDROP 已覆盖；CONTEXT 写明仍不做 |
| 旁观 / 控权转移、中继、审计 UI、本地输入锁定、登录前远控 | 目标能力，非本方案批次 |
| Per-Monitor V2 / `WM_DPICHANGED` 热切换 | 规范 P3「评估」；未做满前不上 V2 |
| Qt / Web / 浏览器远控 | tech-stack 禁止进 Win7 线 |
| **会话抢键时的本机交还热键**（如 Ctrl+Alt+Home） | 已拍板不考虑：会话激活且键转远端时，不要求还能全键盘操作管理台；全键盘仅覆盖目录/壳层（菜单、加速键、树/列表等）未抢键场景 |

### 1.3 已具备（本方案不重做）

登录（帐号 / PSK）、网关目录树、演示树、列表启停会话、Tab（溢出 ◀▶、拖出/拖回、关其他/全部）、全屏、只读、截图、letterbox、剪贴板三通道、TLS mux 直连。

---

## 2. V1 — 管理台命令闭环

**目标**：文件 / 查看 / 会话 / 帮助菜单可点，行为与工具栏一致；去掉「新建连接」后的菜单不再留空壳 ID。

### 2.1 任务

1. 为菜单项分配与工具栏共用的命令 ID（`kCmdRefresh` / `kCmdSession` / `kCmdCloseAll` 等）；去掉占位灰显。
2. **文件 → 退出**：关闭控制台主窗（已有会话先确认或直接断开——与「全部断开」策略一致，实现时二选一并写死）。
3. **查看 → 刷新**：同工具栏刷新目录。
4. **会话 → 启动会话 / 关闭会话 / 全部断开**：复用现有 toolbar 逻辑；启用态与 toolbar `TB_ENABLEBUTTON` 同步（可抽 `sync_chrome_commands()`）。
5. **帮助 → 关于**：简单 `MessageBoxW` 或自绘小窗：产品名、版本（与 Agent 版本展示策略一致即可）、版权一行。
6. 更新 `console_window.h` 注释：去掉 “menu … placeholders”。

### 2.2 验收

- [x] 菜单项无灰显占位；无「新建连接」
- [x] 菜单与工具栏对同一会话操作结果一致
- [x] 无会话时「关闭/全部断开」禁用；有列表选中未连接时可启动
- [x] 退出：有会话时确认后断开并关窗；无会话直接退出
- [x] 「关于」显示产品名与 `ROAD_DESK_VERSION_STRING`

### 2.3 主要改动文件

`src/viewer/console_window.cpp`（+ 如需 `session_host` 仅只读版本字符串）

---

## 3. V2 — 断线重连

**目标**：链路异常断开后 Viewer 能退避重连，避免操作员手动重开 Tab；主动关闭不重连。

### 3.1 行为约定

| 情况 | 行为 |
|------|------|
| 操作员关闭 Tab / 全部断开 / 退出 | **不**重连 |
| TCP/TLS/mux 异常、Host 杀进程、拔网线 | 进入重连：状态栏「正在重连… (n)」；画面保持最后一帧或等待态 |
| 退避 | 例如 1s → 2s → 4s … 上限 30s；成功后清零 |
| 永久失败（PSK/指纹错误等鉴权类） | 停止重连，状态栏短句 + 可选 MessageBox |

### 3.2 任务

1. 在 `SessionHost` / `MediaClient` 边界区分 `user_close` vs `transport_lost`。
2. 控制台 Tab 持有重连策略（每会话独立 timer / 状态机）。
3. 重连期间冻结键鼠注入（等同短暂只读）；恢复后按当前只读开关。
4. 日志：`viewer.log` 记录每次尝试与原因码。

### 3.3 验收

- [x] 主动关 Tab 无重连日志（`user_closing_` / 不 schedule）
- [x] 杀 Host 或断网后自动重连（`WM_MEDIA_TRANSPORT_LOST` + 退避 timer）
- [x] 退避 1s→2s→…→30s（`reconnect_delay_ms`）
- [x] 鉴权/配置失败不循环（`MediaClientFail::Auth|Config` + MessageBox）

### 3.4 主要改动文件

`src/viewer/session_host.*`、`src/viewer/console_window.cpp`；必要时 `src/media` 客户端断开回调（只加原因码，不改协议）

---

## 4. V3 — UI 规范收口（对齐设计分期）

对应 `docs/ui-design-system.md` §7，Viewer 侧落地：

| 子项 | 内容 |
|------|------|
| **V3a ≈ P0** | ✅ `src/ui/rd_tokens.h`；Viewer 登录 / console / Agent 配置共用命名常量 |
| **V3b ≈ P1** | ✅ 管理台壳走 chrome/surface/accent + `rd_create_font`（Segoe→YaHei→Tahoma） |
| **V3c ≈ P2** | ✅ `assets/brand/rd_app.ico` 嵌入 Viewer/Agent；`hIcon`/`hIconSm` + 登录/配置/浮出会话窗 |

Agent 配置对话框已同批对齐 token，避免两端分叉。

### 4.1 验收

- [x] 代码侧无散落魔法 RGB（登录 / console / Agent / session 等待与重连条）
- [ ] 登录 + 管理台主窗 100% / 150% 截图对照设计稿 Frame A/B（人工）
- [x] 无「登录精修、管理台系统灰」代码割裂（同 token）
- [x] 对照 `rd.color.*` / `rd.space.*`（见 `src/ui/rd_tokens.h`）
- [x] 主窗应用图标（`rd_app.ico` 16/32/48/256；资源 `IDI_RD_APP`）

---

## 5. V4 — 壳层交互补齐

1. **分割条宽度芯片**（`viewer-console.pen` Frame D）：拖动树宽时显示当前 DIP 宽度；松手消失。
2. **退出确认**（若 V1 未做）：有活动会话时询问是否全部断开并退出。
3. **文档同步**：本方案勾选完成后，更新 `docs/design/README.md` 实现状态一行；菜单相关 Avoid 已在 CONTEXT。

### 5.1 验收

- [ ] 拖分割条可见宽度反馈，夹取仍 120 … 客户宽−200
- [ ] 有会话时退出不静默丢连接（除非产品明确要静默）

---

## 6. 建议顺序与依赖

```text
V1 菜单闭环  ──┐
               ├──► V4 芯片/退出润色（可与 V3 并行）
V2 重连      ──┘
V3 UI token/壳 可与 V1 并行；勿阻塞 V2
```

优先：**V1 → V2 → V3 → V4**。若只做观感，可 V1∥V3；会话稳定性出门以 V2 为准。

预估（单人熟悉本仓）：V1 0.5–1d，V2 1.5–3d，V3 2–4d，V4 0.5d。

---

## 7. 风险与约束

- Win7 x64 可跑：仅 Win32 + ComCtl + GDI；不引入 Qt/ImGui。
- 重连不得绕过 TLS 指纹 / PSK；`ROAD_DESK_TLS_INSECURE` 仅调试。
- 多 Viewer 共享控制已存在；重连成功后仍共享注入，不引入旁观语义。
- 主屏 only；选屏 UI 不得以「顺手」进 PR。

---

## 8. 验收总清单（出门）

- [ ] V1–V4 分项验收勾完
- [ ] 网关目录双击建会话 + 演示树路径仍通（`docs/gateway.md` 人工项）
- [ ] 100% / 125% / 150%：菜单/工具栏可点、letterbox 点击不系统性偏移（System DPI Aware，非 V2）
- [ ] `CONTEXT.md` / 本文件「不做」项未被悄悄实现

---

**维护**：范围变更（例如重连策略、是否要「关于」窗）先改本文件再改代码。
