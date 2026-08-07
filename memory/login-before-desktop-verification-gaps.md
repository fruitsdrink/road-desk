---
name: login-before-desktop-verification-gaps
description: Items not yet verified for S1–S5 login-before-desktop remote control: UIAccess code signing, real machine lock screen/reboot full-chain, SAS injection, Win7 compatibility
metadata:
  type: project
---

登录前远控 S1–S5 代码已完成，但以下验证项需要在真机上跑通才算真正交付：

1. **UIAccess 代码签名**：Win7+ 的 `OpenDesktop("Winlogon")` 需要 `uiAccess="true"` manifest + 代码签名 + 二进制在 `%ProgramFiles%` 下。当前 manifest 是 `uiAccess="false"`，开发机自签证书未配置。
2. **真机锁屏/重启全链路**：开发机无法模拟完整的 Win+L 锁屏 + 远程解锁流程。
3. **SAS (Ctrl+Alt+Del) 注入**：`CONTEXT.md` 明确不含 UAC 安全桌面，但登录界面的 SAS 场景仍需验证输入密码+Enter 是否足够。
4. **Win7 兼容性**：`OpenDesktop("Winlogon")` 和 `CreateProcessAsUser` 在 Win7 上的行为可能需要不同的 API 路径。

**How to apply:** 需要一台 Win7 真机或 Win10/11 测试机，安装 service，验证 viewer 连接后能看到登录/锁屏画面并完成解锁操作。

**Related:** [[road-desk-pre-login-remote-control]]
