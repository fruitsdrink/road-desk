---
name: road-desk-pre-login-remote-control
description: S1–S5 pre-login remote control implementation status: service scaffold, session monitor, cross-desktop GDI capture, injection pipe IPC, protocol extension, and mux_host wire-up all committed to dev branch
metadata:
  type: project
---

Road Desk 登录前远控 S1–S5 已全部实现、装配和提交到 `dev` 分支，共 5 个 commit（c0cd74b, f5a5f89, f2bf6f6, de47bcb, 9a2343f）。

**Why:** Host Agent 从仅支持已登录桌面远控 → 支持 Windows 登录界面、锁屏界面查看并操作。核心是 Service Wrapper（Session 0）+ 跨桌面 GDI 采集 + Session Helper 注入 IPC + 协议扩展。

**How to apply:**
- 构建后 `host-agent.exe --install` 注册为 LocalSystem 服务
- 服务启动后自动采集 winlogon/locked 桌面、通过 GDI `OpenDesktop("Winlogon")` 获取 DC
- 注入：登录/锁屏桌面直接 `SendInput`，用户桌面通过 `CreateProcessAsUser` 启动 Session Helper + 命名管道 IPC
- `mux_host.cpp` 的 `flush_pending_input()` 已在 S4 wire-up 中连接到 injection_pipe：有 helper 时走管道，否则直接注入
- Viewer 侧在 AuthOk 中收到 host_desktop_state 字节（0=console, 1=logon, 2=locked），状态栏显示"登录界面"/"已锁定"
- 尚未验证：UIAccess 代码签名、真机锁屏/重启全链路、SAS 注入、Win7 兼容性
- 验证计划见 [[login-before-desktop-verification-gaps]]

**Reference:** `CONTEXT.md` §登录前远控, `docs/spike-media-replace-pitfalls.md` row D5, plan `C:\Users\haight\.claude\plans\snazzy-honking-origami.md`
