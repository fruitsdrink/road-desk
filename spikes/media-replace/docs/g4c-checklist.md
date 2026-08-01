# G4c — 拖窗档 C′（vs VNC / Radmin + Mirror）

目标：同机相对现场已用 Mirror 的 VNC/Radmin，拖窗**不被明显 pass**（ADR-0004 档 C′）。

前置：M2 已通（`capture=mirror`）；Win7 Host；Viewer 本机或同网。

## 环境

| 项 | 要求 |
|----|------|
| Host | Win7；Road Desk Mirror +（对照用）VNC 和/或 Radmin Mirror 仍装着 |
| 分辨率 | 固定一种（记入 RESULTS，如 1504×957） |
| 公平 | 默认同机双 Mirror 共存；若拖窗异常再按 [coexistence.md](../../mirror-driver/docs/coexistence.md) 临时禁一方对照 |
| 停 Host | 用窗口关闭或等会话结束；尽量少用 `Ctrl+C`（已有修饰键清理，但关窗口更干净） |

## 对照三段（同一桌面、同一窗口）

每段约 10–20s：拖同一个资源管理器（或业务窗）在桌面上绕几圈。

1. **replace_host mirror** + `replace_viewer`（端口 5902）
2. **经典 VNC**（现场常用 Viewer，走其 Mirror）
3. **Radmin**（若本机有；没有则跳过并注明）

可选轻量：`replace_host gdi` 只作「相对 GDI 是否仍明显更好」的旁证，不计入 C′ 对打。

## 打分（1–5，5 最好）

主观跟手：撕裂/拖影、延迟、整屏闪刷、光标。

| 档位参考 | 含义 |
|----------|------|
| 5 | 接近本地 |
| 4 | 可用、轻微拖影 |
| 3 | 能跟，明显不如本地 |
| 2 | 肉、块状更新 |
| 1 | 难用 |

**出门：** replace mirror 分 ≥ 对照 VNC/Radmin，或仅低 **不到 1 档** 且备注「不被明显 pass」。若低 ≥1 档 → G4c 未过，回抠脏区/管线。

## 勾选

- [x] 与 VNC 并排录像（`20260801_202934.mp4`，2026-08-01）
- [x] Host 日志确认 `capture=mirror`（管理员、关 DM）
- [x] 分数填入 [`RESULTS.md`](../RESULTS.md)「G4c 拖窗对照」— mirror≈VNC≈4，不被 pass
- [ ] Radmin 对照（可选）
- [x] 结论句：**相对 VNC 档 C′ 过**

## Host / Viewer

```bat
:: VM
c:\rd\replace_host.exe 5902 road-desk mirror

:: 维护机
replace_viewer.exe <host>:5902 road-desk <fingerprint>
```

日志看：`capture=mirror`；拖窗时 `ex=` 是否长期整屏（如 `1504x957`）——整屏风暴是未过时的首要嫌疑。
