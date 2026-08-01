# 媒体面探针结论

Status: **concluded**（2026-08-01）  
Related: [spike-media-plane.md](../../docs/spike-media-plane.md), ADR-0001, ADR-0002

候选：`LibVNCServer/LibVNCClient 0.9.15`（GPL-2.0），探针：`spike_host` / `spike_viewer`。  
口令：`spike`。构建：`spikes/media-plane/scripts/build.ps1` → `build/spike-media/`。

## 总判

- [x] **继续 A**：VNC 适配器用于 MVP（画面+键鼠），目标能力（文件等）走控制面/自有通道，**正式交付前换合规实现**（ADR-0002）
- [ ] **局部自研**
- [ ] **调整选型**

依据：Win7 车道真环上，探针与经典 TightVNC 并排对照，看屏与键鼠达到「GUI 可操作、手感同级」；未发现必须换 VNC 候选或重回全量自研的 P1 失败项。

**未跑项**：连续 ≥30 分钟稳定性尚未实测。收口不阻塞总判；MVP 联调/浸泡中补跑，若出现黑屏、闪屏、异常掉线再重开媒体面选型。

## P1 — 真实桌面采集与注入

| 项 | 结果 |
|----|------|
| 环境 | Viewer：Win11 开发机；Host：Win7 x64 车道机（约 `10.213.8.23`），常与经典 VNC 并排（探针多占 `:5901`） |
| GUI 可操作 | **通过** — 开窗、点选、拖动、文本/资源管理器交互可用 |
| 对照 WinVNC 手感 | **可接受** — 产品接线后拖窗曾因 `raw` 优先/局部 blit 变差或花屏；现回退为探针策略（`hextile` 优先、整帧绘制、发送反压），拖窗手感可接受 |
| ≥30 分钟稳定 | **未测** — 见总判残留风险 |
| 结论 | **通过（附条件）** — 足以支撑 MVP 看屏+键鼠；30 分钟浸泡跟主线补 |

### 实现与已知限制（摘要）

| 项 | 说明 |
|----|------|
| 采集 | GDI `BitBlt`（`SRCCOPY\|CAPTUREBLT`）→ 稳定 `frameBuffer` 指针 + 脏区 |
| 注入 | Host `SendInput`；断连/失焦松开修饰键 |
| Viewer | 等比缩放+letterbox；`WH_KEYBOARD_LL` 聚焦时抓 Alt+Tab/Win |
| 拖窗粉紫拖影 | **经典 VNC 与探针均有**；Host「拖动时显示窗口内容」已关仍出现 → 视频 overlay/色键 + GDI，**非探针独有**；不据此判 P1 失败；根治需 Mirror Driver 等，**探针/MVP 不做** |
| 其它 | 监听失败须 fail-fast（避免 LibVNC 空 `select` 刷屏）；单会话拒绝第二客户端 |

## P2 — 桌面内视频区干扰

| 项 | 结果 |
|----|------|
| 场景 | 车道 ETC/机电界面含视频预览时远控 |
| GUI 是否仍可用 | **是** — 可完成 GUI 操作；视频区卡顿/拖窗色键拖影可接受 |
| 结论 | **通过** — 维持验收文案「视频区不保证」；不为视频区优化拖垮 GUI |

## P3 — 登录前 / 锁屏

| 项 | 结果 |
|----|------|
| 实测 | **未做**（本探针未覆盖锁屏/登录界面） |
| 预判 | 当前会话内 GDI + `SendInput` 路径**不足以**承诺登录前；多半需服务会话/高权限或 Mirror Driver |
| 结论选项 | **目标阶段单列里程碑** — 不阻塞 MVP 已登录桌面远控；不据此提前换芯 |

## P4 — 文件传输通道边界

| 结论 | **应自研文件通道**，与画面/键鼠媒体面并列 |
|----|------|
| 理由 | LibVNC/Tight/Ultra 文件扩展 GPL 且与适配器耦合；替换媒体面时若文件绑在 RFB 扩展上要重写 |
| 交付 | 不把 GPL 文件扩展打进正式安装包 |

## P5 — 合规替换工作量（粗估）

| 依赖 | 许可 | 交付前必须替换？ |
|------|------|------------------|
| LibVNCServer / LibVNCClient 0.9.15 | GPL-2.0 | **是** |
| 探针未链 zlib/jpeg/OpenSSL（最小构建） | — | 产品启用时再登记 |

| 项 | 人周（粗） |
|----|------------|
| 保持 `media/` API，替换 LibVNC 实现 | 3–8 |
| 若登录前要求驱动级采集（P3） | +2–6 |
| 文件通道自研（P4） | 1–3 |

建议时间点：**MVP 内部验证继续用 LibVNC；正式装站/对外交付前必须完成替换**。P1 已在 Win7 通过（附 30 分钟补测），可继续堆控制面，不必先换候选再探针。

## 明确不做（本探针，已遵守）

全量自研协议栈、中心/审计/中继、浏览器端、以视频区 FPS 为优化目标、把探针包当正式安装包、Mirror Driver。

## 主线后续

1. ~~控制面接入时把 LibVNC 关在 `src/media/` 适配器后~~ — **已接线**（`host-agent` / `viewer` + `scripts/build.ps1`；默认口令 `road-desk`）
2. 补跑 ≥30 分钟浸泡；失败则重开选型，不放宽「不黑屏/不闪屏」
3. ~~控制面：PSK 会话、加密、单会话互斥接到真连~~ — **已接线**（`authenticate_psk` fail-closed；`SessionMutex` on accept/gone；Schannel TLS 包裹 RFB，默认无明文；指纹信任 / `ROAD_DESK_TLS_INSECURE`）
4. 文件通道按自有会话设计；登录前单列
5. 交付前执行 P5 替换 — 路径已钉死：[ADR-0004](../../docs/adr/0004-compliant-media-self-developed.md)、[spike-media-replace.md](../../docs/spike-media-replace.md)（分支 `cursor/media-replace-spike`）
