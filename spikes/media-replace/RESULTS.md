# 媒体面换芯探针结论

Status: **G5 passed — 切入主线**（计划见 [spike-media-replace.md](../../docs/spike-media-replace.md)；避坑见 [spike-media-replace-pitfalls.md](../../docs/spike-media-replace-pitfalls.md)；ADR-0004）  
Branch: `cursor/media-replace-spike`

对照基线：现主线 LibVNC MVP（`host-agent` / `viewer`）。前序探针：[media-plane RESULTS](../media-plane/RESULTS.md)。

**LibVNC 已验证备份（勿删至切入主线实施完成）**

| 引用 | 指向 |
|------|------|
| 分支 | `backup/libvnc-mvp-verified` |
| Tag | `libvnc-mvp-verified-2026-08-01` |
| 提交 | `6f650d5`（同 `cursor/control-plane-psk-tls`：PSK + 互斥 + Schannel TLS + LibVNC） |

G5 已过；**迁移 PR 落地前**仍勿直接覆盖 `src/media/libvnc_*`（按迁移任务整批替换）。

## 总判（到期三选一）

- [x] **切入主线**
- [ ] **管线再迭代**
- [ ] **GDI 上限结案**（另议 Mirror 立项；不默认采购）

依据（2026-08-01）：

| 门槛 | 结论 |
|------|------|
| **档 A**（合规意图 / 看屏+键鼠 / TLS+PSK+指纹） | 通过：私有 mux + Schannel TLS，无 LibVNC 进探针产物；G1–G3 真环通过 |
| **档 B**（vs LibVNC GDI） | 通过：G0 LibVNC≈2；M2 `capture=mirror` 拖窗「明显更好」；与 G4c≈4 一致，相对 GDI 可感知改善 |
| **档 C′**（vs VNC+Mirror） | 通过：并排 `20260801_202934.mp4`，mirror≈VNC≈4，不被明显 pass；Radmin 本机未装，跳过 |
| **Mirror** | GM1/GM2/M2 通过；共存/管理员/Attach.ToDesktop/Basic 主题见 `mirror-driver/docs/coexistence.md` |

**不构成失败（已接受）：** 未达 ToDesk 档 C；未做 File/剪贴板/登录前；Radmin 未测。

**G5 之后（非本探针门禁）：** 将 `spikes/media-replace` + `spikes/mirror-driver` 迁入 `src/media/` 与装站路径，去主线 LibVNC 链接，带测试签 Mirror 部署说明。

### 迁移待办：Host 管理员权限（必须处理）

**事实（2026-08-01）：** Mirror 采帧本身不要求管理员；但 **清 HKLM `Attach.ToDesktop` / peer scrub** 需要管理员。非提升进程开设备管理器 → 键鼠失效（已对照）。

| 项 | 要求 |
|----|------|
| 上道 | `capture=mirror`（及 auto 走到 Mirror）的 Host **必须以管理员运行** |
| 产品/装站须落地 | 清单 `requireAdministrator`、或安装为 **LocalSystem/管理员服务**、或启动器 UAC 提权；禁止静默以标准用户跑 Mirror 会话 |
| 失败体验 | 非提升时：启动即明确失败或降级提示（现探针仅 WARN）；**不得**假装 Mirror 正常却在开 DM 时死键鼠 |
| 文档 | 详见 [`../mirror-driver/docs/coexistence.md`](../mirror-driver/docs/coexistence.md)「管理员硬条件」 |

## 对照基线（阶段 1 / G0）

| 项 | 记录 |
|----|------|
| Host / Viewer 环境 | Host：Win7 车道真机（例 `TZ-ETC-ENU1`，Intel HD 4600）；Viewer：维护者环境（正式路径验收 2026-08-01） |
| 生产 Mirror 环境 | **现场无「无 Mirror」生产机**：设备管理器常见已装 `VNC Mirror Driver`、`Radmin Mirror Driver V3` 等。对照/验收默认在此环境做，不假设干净 GDI-only 现场。 |
| Road Desk 是否使用 Mirror | **换芯 Host：M2 可走自研 Mirror**（`ROAD_DESK_CAPTURE=auto|mirror|gdi`）；LibVNC 主线仍为 GDI。不调用现场第三方 Mirror |
| 正式路径（PSK/TLS/指纹/互斥） | **通过**（2026-08-01；维护者确认） |
| ≥30 分钟浸泡 | **通过**（2026-08-01；维护者确认：无黑屏/闪屏/异常掉线） |
| 拖窗快照（轻量） | **已填**（录像分析，见下） |
| 录像 | `c:\Users\haight\Videos\20260801_150258.mp4`（约 86s，2026-08-01） |
| Road Desk / LibVNC 拖窗分 | **2 / 5** |
| vs 经典 VNC Viewer（同机） | **约 3 / 5**；Road Desk 明显更肉一档（非并排精密计时） |
| （原多场景表） | 延后到 G4/G4c |
| Viewer DPI 100% / 125% / 150% 点击 | **Host 缩放抽测通过**（2026-08-01）：Win7 Host @ **125%** / **150%** 点远端图标正常；Viewer 侧 125%/150% 若另测再补 |

### 拖窗快照详情（录像）

| 项 | 观察 |
|----|------|
| 路径 | Road Desk Viewer `1600x900`，日志 `hextile` / `127.0.0.1`（LibVNC MVP）；Host 车道 UI「ETC入口车道系统」含实时预览 |
| ~5–15s | 在远端桌面拖 `D:\test` 资源管理器，压在车道画面+日志上；更新偏「块状/跟手差」，属 GDI+hextile 典型拖窗 |
| ~30–43s | 同机再开「田庄入口… - VNC Viewer」叠在车道窗上拖动；内容仍有撕裂/错位帧，但整体比 Road Desk 段更跟得上一些 |
| 结论句 | **现 LibVNC/Road Desk 拖窗手感约 2 分；同机经典 VNC（多半吃 Mirror）约 3 分，已能感到被 pass 一档。** G0 足够；精密对照留 G4/G4c |

## Gate 进度

| Gate | 含义 | 状态 | 日期 |
|------|------|------|------|
| G0 | MVP 基线 + 拖窗快照 | **通过**（正式路径 + 浸泡 + 录像拖窗快照） | 2026-08-01 |
| G1 | TLS + Control 骨架 | **通过**（2026-08-01；Win7：`G1 PASS auth ok desktop=1600x900`；指纹 `39a0462c…4086`） | 2026-08-01 |
| G2 | Video 看屏闭环 | **通过**（2026-08-01；Win7：`video frame id=0/50` @ 1600x900） | 2026-08-01 |
| G3 | Input + 优先级 | **通过**（点选+Alt+Tab 钩子；Host @125%/150% 点图标正常） | 2026-08-01 |
| G4 | 拖窗档 B（vs LibVNC GDI） | **通过**（G0 LibVNC≈2 → M2/G4c mirror≈4；可感知改善） | 2026-08-01 |
| GM1/GM2 | 自研 Mirror 加载/脏区（见 [mirror-driver RESULTS](../mirror-driver/RESULTS.md)） | **GM1/GM2 通过**（Win7 VM：`gm2` 得脏区） | 2026-08-01 |
| M2 | Mirror 脏区接入 `replace_host` | **通过**（Win7：`capture=mirror`；拖窗明显改善；软光标不闪） | 2026-08-01 |
| G4c | 拖窗档 C′（vs VNC/Radmin+Mirror） | **VNC 段通过**（并排录像；Radmin 跳过） | 2026-08-01 |
| G5 | RESULTS 收口 | **通过 — 切入主线** | 2026-08-01 |

## 拖窗对照（阶段 5 / G4）

| 场景 | LibVNC 卡顿 1–5 | 换芯卡顿 1–5 | 是否可感知改善 | 备注 |
|------|-----------------|--------------|----------------|------|
| 拖资源管理器（G0 同型场景） | **2**（G0 录像） | **~4**（M2/G4c） | **是** | 未另录 G4 专段；以 G0+M2+G4c 结案 |

## G4c 拖窗对照（档 C′）

清单：[docs/g4c-checklist.md](docs/g4c-checklist.md)

| 项 | 记录 |
|----|------|
| Host / 分辨率 | Win7 VM；1504×957；管理员 + `capture=mirror`；测时关闭设备管理器 |
| 录像路径 | `c:\Users\haight\Videos\20260801_202934.mp4`（约 50s；左 Road Desk Viewer / 右 VNC Viewer 并排） |
| replace mirror 分 1–5 | **~4**（维护者：与 VNC 差不多） |
| 同机 VNC 分 1–5 | **~4** |
| 同机 Radmin 分 1–5 | N/A（跳过：本机未装） |
| 是否被明显 pass | **否** |
| 结论 | **相对 VNC：档 C′ 达成**；Radmin 跳过，不挡 G5 |
| 条件备注 | 对照时管理员 + `capture=mirror`。开 DM 键鼠失效属 `Attach.ToDesktop`/PnP 坑，与 Mirror 采帧无关（见 `mirror-driver/docs/coexistence.md`）；勿再为 DM 切 GDI |

## 进度日志

| 日期 | Gate 目标 | 达成？ | Win7 复现要点 | 拖窗分 | 是否进下一阶段 |
|------|-----------|--------|---------------|--------|----------------|
| 2026-08-01 | G0：正式路径 | 部分 | PSK/TLS/指纹/互斥已通过 | — | 否；先补浸泡+拖窗基线 |
| 2026-08-01 | G0：≥30min 浸泡 | 通过 | 无黑屏/闪屏/异常掉线 | — | — |
| 2026-08-01 | G0 拖窗减负 | 决策 | 不强制 ≥3 场景/并排打分；多场景表留 G4/G4c | — | — |
| 2026-08-01 | G0：拖窗快照（录像） | 通过 | Road Desk/LibVNC≈2；经典 VNC≈3；见上表 | 2 | **是** — G0 关门，可开 Track P/M（错周） |
| 2026-08-01 | 策略：自研 Mirror 提前 | 决策 | 上道手感绑 Mirror；不采购商业 SDK；双轨 P+M | — | G0 后 P/M 错周并行 |
| 2026-08-01 | G1：mux TLS+Control | 本地通过 | `replace_host`/`replace_viewer`；端口 5902；PSK+指纹；非阻塞 listen 已用 select | — | — |
| 2026-08-01 | G1：Win7 真环 | **通过** | `auth ok desktop=1600x900`；peer_fp=`39a0462c…4086` | — | **是** — 开 G2 Video（GDI） |
| 2026-08-01 | G2：GDI Video | **通过** | Win7 `video frame id=0/50` 1600x900 | — | 开 G3 |
| 2026-08-01 | G3：Input | 代码已出 | Host 单线程 select 优先 drain Input；Viewer 发指针/VK；`tls_get_socket` | — | Win7 点选验证 |
| 2026-08-01 | G3：点选+DPI | **通过** | Host Win7 @125%/150% 点图标正常；Viewer `WH_KEYBOARD_LL` 后 Alt+Tab 进远端 | — | 可进 G5 / 可选 G4 |
| 2026-08-01 | Track M 开轨 | 进行中 | `spikes/mirror-driver`：控制设备骨架 + probe；共存=上道不强制卸第三方；GM1 见 mirror-driver 清单 | — | 真机 GM1 |
| 2026-08-01 | GM1 | **通过** | Win7 VM：`mirror_probe` → `GM1_LOADED`；见 mirror-driver RESULTS | — | 开 GM2（脏区/XPDM） |
| 2026-08-01 | GM2 | **通过** | Win7 VM：`DISPLAYV4` + dirty；见 mirror-driver RESULTS | — | M2 接入 replace_host |
| 2026-08-01 | M2 | **通过** | Win7：`capture=mirror device=\\.\DISPLAYV4`；拖窗手感明显好于 GDI；去 `CAPTUREBLT` + Viewer 离屏合成后光标静止/移动均不闪 | — | G4c 对照 |
| 2026-08-01 | G4c 开测 | 进行中 | 清单 `docs/g4c-checklist.md`；同机 mirror vs VNC/Radmin 拖窗打分 | | 待录像+分数 |
| 2026-08-01 | G4c vs VNC | **通过** | 并排 `20260801_202934.mp4`；维护者感觉与 VNC 差不多；条件=管理员+关 DM+mirror | ~4/~4 | Radmin 可选；可进管线收尾/G5 准备 |
| 2026-08-01 | DM+键鼠坑结案 | **记录** | 根因=`Attach.ToDesktop=1`/PnP，非 Mirror 采帧；误切 GDI 会卡+改 Basic；现开 DM 保持 Mirror（同 VNC）已核实可用 | — | 见 coexistence.md |
| 2026-08-01 | DM×权限对照 | **确认** | 同构建不切 GDI：管理员开 DM=正常；非管理员开 DM=键鼠失效 → **必须管理员跑 mirror Host** | — | coexistence.md |
| 2026-08-01 | G5 收口 | **通过** | 总判 **切入主线**：A+B+C′（VNC）+ Mirror 门禁齐；Radmin 跳过；迁移主线另开任务 | — | 见上文总判 |

## 合规

| 依赖 | 许可 | 探针是否链接 |
|------|------|--------------|
| LibVNC | GPL-2.0 | 目标：**否** |
| 其它编解码（若引入） | | 登记于此 |

## 避坑抽查（§J）

| 日期 | E1 NODELAY | C2 最新帧 | C3 Input 优先 | E8 断连释放 | G1 Win7 结论 | L8 125/150% | 备注 |
|------|------------|-----------|---------------|-------------|--------------|--------------|------|
| 2026-08-01 | | | 是（drain 优先） | 是（失焦/断连松键） | G1 已过 | **Host @125%/150% 点选 OK** | Viewer 本机高 DPI 另补可选 |

## 明确不做（本探针，须遵守）

商业 Mirror SDK、调用现场第三方 Mirror、第三方 VNC 兼容、Go/Rust 媒体面、文件/剪贴板/登录前、整屏 JPEG 默认、跳过 G0。自研 Mirror 在 [mirror-driver](../mirror-driver/) 并行，不在本目录实现驱动。
