---
name: road-desk-pre-login-remote-control
description: S1-S5 pre-login remote control implementation: service scaffold, session monitor, GDI cross-desktop capture, injection pipe IPC, protocol byte, mux_host wire-up. All committed to dev, needs CI upgrade run.
metadata:
  type: project
---

登录前远控 S1–S5 **全部完成，干净提交**。
跑通：构建（`host-agent.exe` + `viewer.exe`）、6 个 CTest（`auth_test` / `session_mutex_test` / `capture_strategy_test` / `mux_parse_test` / `clipboard_test` / `tls_test`）、Service install/uninstall 验证。

提交链：`7be13ae → c0cd74b → f5a5f89 → f2bf6f6 → de47bcb → 9a2343f → 3366f68`。都在 `dev` 分支，已 push。

**Why:** 从仅支持已登录桌面 → 登录界面、锁屏界面可远控。Service (Session 0) + GDI cross-desktop DC + Session Helper IPC + AuthOk protocol byte。

**How to apply:**
- `host-agent.exe --install` → LocalSystem 服务开箱自启
- `cmake --build build --target host-agent viewer`
- `ctest --test-dir build --output-on-failure`
- 未验证项：UIAccess 签名、真机锁屏/重启全链路、SAS、Win7 兼容（见 [[login-before-desktop-verification-gaps]]）

**Files:**
- `src/agent/` (7 files) + `src/media/` (7 files) + `src/viewer/` (1 file) + `src/common/` (3 files) + `src/session/` (3 tests) + `.github/workflows/ci.yml` + `README.md`
