# 媒体面换芯探针结论

Status: **not started**（计划见 [spike-media-replace.md](../../docs/spike-media-replace.md)；避坑见 [spike-media-replace-pitfalls.md](../../docs/spike-media-replace-pitfalls.md)；ADR-0004）  
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
| Host / Viewer 环境 | |
| 正式路径（PSK/TLS/指纹/互斥） | 未测 / 通过 / 失败 |
| ≥30 分钟浸泡 | 未测 / 通过 / 失败 |
| 拖窗场景 1（描述 + 卡顿 1–5） | |
| 拖窗场景 2 | |
| 拖窗场景 3 | |
| Viewer DPI 100% / 125% / 150% 点击 | 未测 / 通过 / 失败 |

## Gate 进度

| Gate | 含义 | 状态 | 日期 |
|------|------|------|------|
| G0 | MVP 基线 + 拖窗笔记 | pending | |
| G1 | TLS + Control 骨架 | pending | |
| G2 | Video 看屏闭环 | pending | |
| G3 | Input + 优先级 | pending | |
| G4 | 拖窗档 B | pending | |
| G5 | RESULTS 收口 | pending | |

## 拖窗对照（阶段 5 / G4）

| 场景 | LibVNC 卡顿 1–5 | 换芯卡顿 1–5 | 是否可感知改善 | 备注 |
|------|-----------------|--------------|----------------|------|
| | | | | |

## 进度日志

| 日期 | Gate 目标 | 达成？ | Win7 复现要点 | 拖窗分 | 是否进下一阶段 |
|------|-----------|--------|---------------|--------|----------------|
| | | | | | |

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

商业 Mirror、自研 Mirror、第三方 VNC 兼容、Go/Rust 媒体面、文件/剪贴板/登录前、整屏 JPEG 默认、跳过 G0。
