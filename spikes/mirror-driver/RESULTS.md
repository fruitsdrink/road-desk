# 自研 Mirror 驱动探针结论

Status: **GM2 通过**（VMware Win7：`rdmmini` RUNNING + `gm2` 得脏区；下一闸 M2 接入 `replace_host` / G4c；与 [media-replace](../media-replace/RESULTS.md) 双轨；ADR-0004）  
Branch: `cursor/media-replace-spike`

## 目标

Win7 x64 自研 Mirror：用户态可得脏区+像素；接入换芯采屏后冲档 **C′**（同机不被 VNC/Radmin+Mirror 明显 pass）。**不**采购商业 Mirror；**不**把现场已装 VNC/Radmin Mirror 当正式依赖。

## Gate

| Gate | 含义 | 状态 | 日期 |
|------|------|------|------|
| M0 | 骨架 + 文档 + 用户态 probe；WDK/测试签环境 | **通过** | 2026-08-01 |
| GM1 | 稳定加载/可卸、无必现蓝屏 | **通过**（加载+probe；卸载勾选见下） | 2026-08-01 |
| GM2 | 脏区→用户态 | **通过**（`\\.\DISPLAYV4` attach；`info 1504x957`；dirty + `dirty_sample.bmp`） | 2026-08-01 |
| G4c | 接入后档 C′（记在 media-replace RESULTS） | pending | |

## 共存（现场常见已装第三方 Mirror）

| 项 | 记录 |
|----|------|
| 样机已装驱动 | 例：VNC Mirror Driver、Radmin Mirror Driver V3 |
| 与自研共存策略 | **上道不强制卸/禁用第三方**。正式只打开 `\\.\RoadDeskMirror`。第三方可继续启用（运维兜底）。双 Mirror 冲突时临时禁用做**隔离复现**（非上道政策）。G4c 公平对照可选用临时禁用。详见 [docs/coexistence.md](./docs/coexistence.md) |
| 验证结果 | 2026-08-01 VMware Win7：GM1 `rdmmirror` + GM2 XPDM（与 VMware SVGA 并存）；`rdmmini` RUNNING；`mirror_probe gm2` 得脏区 |

## 进度日志

| 日期 | Gate | 达成？ | 备注 |
|------|------|--------|------|
| 2026-08-01 | 决策 | — | 产品决定自研 Mirror **提前**并行 |
| 2026-08-01 | M0 开轨 | 进行中 | 落 README/共存/测试签/GM1 清单；控制设备骨架 + `mirror_probe`；XPDM disp/mini 占位待 GM2 |
| 2026-08-01 | GM1 | **通过** | Win7 VM：`GM1_LOADED ok`；flags=LOADED；info 仍 0（无 XPDM）；装驱需测试签证书，仅 pnputil 不够（需 copy+sc / 或补设备节点） |
| 2026-08-01 | GM2 | **通过** | Win7 VM：装 INF + 重启；`sc query rdmmini` RUNNING；`gm2 10` → attach `\\.\DISPLAYV4`、`info 1504x957 pitch=6016`、多帧 dirty、写出 `dirty_sample.bmp`（与 VMware SVGA 并存） |
