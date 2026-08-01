# M2 — Mirror 接入 replace_host

前置：GM2 已通（`rdmmini` + 显示适配器 **Road Desk Mirror Driver**）；本机已编 `replace_host.exe`。

## 运行

```bat
:: Host（默认 auto：有 Mirror 则用，否则 GDI）
replace_host.exe 5902 road-desk
:: 或强制：
replace_host.exe 5902 road-desk mirror
replace_host.exe 5902 road-desk gdi
set ROAD_DESK_CAPTURE=mirror
```

期望 Host 日志：`capture=mirror device=\\.\DISPLAYn`（或 `capture=gdi`）。

Viewer 同 G3：

```bat
replace_viewer.exe 127.0.0.1:5902 road-desk <fingerprint>
```

## 勾选

- [x] `capture=mirror` 会话可看屏、可点选（2026-08-01 Win7 VM）
- [x] 拖窗相对 GDI 明显改善（录像确认）
- [x] 软光标：去 CAPTUREBLT + Viewer 离屏合成后不闪
- [ ] 无 Mirror 机：`auto` 回退 `gdi` 仍可用（未单测）
- [x] 无必现蓝屏/花屏

## 对照（冲 G4c 前）

| 模式 | 拖窗观感 1–5 | 备注 |
|------|--------------|------|
| replace_host gdi | ~偏低 | 此前 GDI 上限 |
| replace_host mirror | **明显更好** | 2026-08-01；光标已不闪 |
| 同机 VNC/Radmin | | 见 [g4c-checklist.md](g4c-checklist.md) |
