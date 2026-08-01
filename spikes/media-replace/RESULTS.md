# 媒体面换芯探针结论

Status: **G0 passed**（计划见 [spike-media-replace.md](../../docs/spike-media-replace.md)；避坑见 [spike-media-replace-pitfalls.md](../../docs/spike-media-replace-pitfalls.md)；ADR-0004）  
Branch: `cursor/media-replace-spike`

对照基线：现主线 LibVNC MVP（`host-agent` / `viewer`）。前序探针：[media-plane RESULTS](../media-plane/RESULTS.md)。

**LibVNC 已验证备份（勿删至切入主线后）**

| 引用 | 指向 |
|------|------|
| 分支 | `backup/libvnc-mvp-verified` |
| Tag | `libvnc-mvp-verified-2026-08-01` |
| 提交 | `6f650d5`（同 `cursor/control-plane-psk-tls`：PSK + 互斥 + Schannel TLS + LibVNC） |

G5 通过前：换芯代码不得覆盖 `src/media/libvnc_*`。

## 总判（到期三选一）

- [ ] **切入主线**
- [ ] **管线再迭代**
- [ ] **GDI 上限结案**（另议 Mirror 立项；不默认采购）

依据：（未填）

## 对照基线（阶段 1 / G0）

| 项 | 记录 |
|----|------|
| Host / Viewer 环境 | Host：Win7 车道真机（例 `TZ-ETC-ENU1`，Intel HD 4600）；Viewer：维护者环境（正式路径验收 2026-08-01） |
| 生产 Mirror 环境 | **现场无「无 Mirror」生产机**：设备管理器常见已装 `VNC Mirror Driver`、`Radmin Mirror Driver V3` 等。对照/验收默认在此环境做，不假设干净 GDI-only 现场。 |
| Road Desk 是否使用 Mirror | **当前否**（LibVNC/GDI）；**目标：自研 Mirror**（提前并行，见 [mirror-driver RESULTS](../mirror-driver/RESULTS.md)）。不调用现场已装第三方 Mirror |
| 正式路径（PSK/TLS/指纹/互斥） | **通过**（2026-08-01；维护者确认） |
| ≥30 分钟浸泡 | **通过**（2026-08-01；维护者确认：无黑屏/闪屏/异常掉线） |
| 拖窗快照（轻量） | **已填**（录像分析，见下） |
| 录像 | `c:\Users\haight\Videos\20260801_150258.mp4`（约 86s，2026-08-01） |
| Road Desk / LibVNC 拖窗分 | **2 / 5** |
| vs 经典 VNC Viewer（同机） | **约 3 / 5**；Road Desk 明显更肉一档（非并排精密计时） |
| （原多场景表） | 延后到 G4/G4c |
| Viewer DPI 100% / 125% / 150% 点击 | 未测（G3 再测） |

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
| G3 | Input + 优先级 | **代码已出**（2026-08-01；Win7 待点选验证） | 2026-08-01 |
| G4 | 拖窗档 B（vs LibVNC GDI） | pending | |
| GM1/GM2 | 自研 Mirror 加载/脏区（见 [mirror-driver RESULTS](../mirror-driver/RESULTS.md)） | **GM1/GM2 通过**（Win7 VM：`gm2` 得脏区） | 2026-08-01 |
| G4c | 拖窗档 C′（vs VNC/Radmin+Mirror） | pending | |
| G5 | RESULTS 收口 | pending | |

## 拖窗对照（阶段 5 / G4）

| 场景 | LibVNC 卡顿 1–5 | 换芯卡顿 1–5 | 是否可感知改善 | 备注 |
|------|-----------------|--------------|----------------|------|
| | | | | |

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
| 2026-08-01 | Track M 开轨 | 进行中 | `spikes/mirror-driver`：控制设备骨架 + probe；共存=上道不强制卸第三方；GM1 见 mirror-driver 清单 | — | 真机 GM1 |
| 2026-08-01 | GM1 | **通过** | Win7 VM：`mirror_probe` → `GM1_LOADED`；见 mirror-driver RESULTS | — | 开 GM2（脏区/XPDM） |
| 2026-08-01 | GM2 | **通过** | Win7 VM：`DISPLAYV4` + dirty；见 mirror-driver RESULTS | — | M2 接入 replace_host / G4c |

## 合规

| 依赖 | 许可 | 探针是否链接 |
|------|------|--------------|
| LibVNC | GPL-2.0 | 目标：**否** |
| 其它编解码（若引入） | | 登记于此 |

## 避坑抽查（§J）

| 日期 | E1 NODELAY | C2 最新帧 | C3 Input 优先 | E8 断连释放 | G1 Win7 结论 | L8 125/150% | 备注 |
|------|------------|-----------|---------------|-------------|--------------|--------------|------|
| | | | | | | | |

## 明确不做（本探针，须遵守）

商业 Mirror SDK、调用现场第三方 Mirror、第三方 VNC 兼容、Go/Rust 媒体面、文件/剪贴板/登录前、整屏 JPEG 默认、跳过 G0。自研 Mirror 在 [mirror-driver](../mirror-driver/) 并行，不在本目录实现驱动。
