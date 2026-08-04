---
name: win32-ui
description: >-
  Applies Road Desk native Win32 UI design tokens and conventions for Viewer and
  Host Agent. Use when creating or restyling Win32 dialogs, console chrome,
  fonts, colors, spacing, icons, DPI scaling, gateway/login UI, or any
  src/viewer or src/agent UI code.
---

# Road Desk Win32 UI

## When to use

Any change that affects native UI chrome in Viewer or Host Agent (dialogs, console shell, fonts, colors, layout, icons, DPI).

## Source of truth

1. Read [docs/ui-design-system.md](../../../docs/ui-design-system.md) before editing UI.
2. Follow product terms in [CONTEXT.md](../../../CONTEXT.md) (操作端/被控端, Viewer/Host Agent).
3. Stack constraints: raw Win32 + Common Controls + GDI only — no Qt/ImGui/Electron for Win7 line ([docs/tech-stack.md](../../../docs/tech-stack.md)).

## Hard rules

- **One product language**: Viewer and Agent share the same brand colors, fonts, and「浏览…」label. Viewer login uses the split auth layout (left brand / right form); Agent uses the compact top-header config dialog.
- **Named tokens only**: use `rd.color.*` / `rd.space.*` / `rd.font.*` values from the design doc; do not invent new RGB or padding without updating the doc first.
- **Fonts everywhere**: Segoe UI stack + `WM_SETFONT` on all controls; pt→px via `MulDiv` and DPI.
- **DPI**: scale **layout and fonts** with DIP helpers; keep System DPI Aware; do not enable Per-Monitor V2 unless `WM_DPICHANGED` is fully handled.
- **Radius**: 0 for stock controls; max 4 DIP for owner-draw; no capsule/pill UI.
- **No demo drift**: do not leave Agent as a plain system dialog while Viewer is branded.

## Workflow

```
Task progress:
- [ ] Read docs/ui-design-system.md (tokens + component section you touch)
- [ ] Match existing UI helper patterns in the same binary; prefer shared src/ui when it exists
- [ ] Apply tokens (color/font/space/control sizes)
- [ ] Scale all CreateWindow coordinates with DIP
- [ ] Verify copy (Road Desk titles, 浏览…, CONTEXT terms)
- [ ] Note 100%/125%/150% checks in the PR or reply
```

## Quick token cheat sheet

| Role | Value |
|------|--------|
| Brand bar bg | `RGB(22,30,46)` |
| App surface | `RGB(246,248,251)` |
| Panel/input | `RGB(255,255,255)` |
| Label text | `RGB(88,98,114)` |
| Body text | `RGB(28,34,46)` |
| Border | `RGB(220,226,234)` |
| Accent | `RGB(47,107,168)` |
| Success | `RGB(46,125,90)` |
| Dialog pad X | 28 DIP（配置窗）/ 40 DIP（登录表单区） |
| Header H | 72 DIP（配置顶栏） |
| Login split | 800×540，左栏 320 DIP + brand 背景图 |
| Edit H | 28 DIP（配置）/ 32 DIP（登录，含前缀图标） |
| Button | 100×30（配置）/ 登录主钮满宽×36（含图标） |
| Password | 右侧 eye 按钮切换显隐 |
| Body font | 10pt Segoe UI |
| Label font | 9pt Segoe UI |
| Brand font | 16pt Semibold |
| Login title | 18pt Semibold |

Full tables, anti-patterns, and phased rollout: [docs/ui-design-system.md](../../../docs/ui-design-system.md).

## Out of scope

- Do not restyle the admin Web app under this skill unless asked.
- Do not change media-plane capture/encode for "UI polish".
- Do not edit design tokens in code only — update the doc in the same change.
