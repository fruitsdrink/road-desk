# 自研 Mirror 驱动探针结论

Status: **not started**（与 [media-replace](../media-replace/RESULTS.md) 双轨；计划见 [spike-media-replace.md](../../docs/spike-media-replace.md) §阶段 M；ADR-0004）  
Branch: `cursor/media-replace-spike`

## 目标

Win7 x64 自研 Mirror：用户态可得脏区+像素；接入换芯采屏后冲档 **C′**（同机不被 VNC/Radmin+Mirror 明显 pass）。**不**采购商业 Mirror；**不**把现场已装 VNC/Radmin Mirror 当正式依赖。

## Gate

| Gate | 含义 | 状态 | 日期 |
|------|------|------|------|
| M0 | WDK + 测试签环境 | pending | |
| GM1 | 稳定加载/可卸、无必现蓝屏 | pending | |
| GM2 | 脏区→用户态 | pending | |
| G4c | 接入后档 C′（记在 media-replace RESULTS） | pending | |

## 共存（现场常见已装第三方 Mirror）

| 项 | 记录 |
|----|------|
| 样机已装驱动 | 例：VNC Mirror Driver、Radmin Mirror Driver V3 |
| 与自研共存策略 | 未定（禁用其一 / 文档要求卸第三方 / 其它） |
| 验证结果 | |

## 进度日志

| 日期 | Gate | 达成？ | 备注 |
|------|------|--------|------|
| 2026-08-01 | 决策 | — | 产品决定自研 Mirror **提前**并行 |
