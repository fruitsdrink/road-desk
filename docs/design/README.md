# Viewer UI 设计稿（Pencil）

栈：Win32 + ComCtl32 + GDI（见 [tech-stack.md](../tech-stack.md)）。Token：[ui-design-system.md](../ui-design-system.md)。

| 屏 | 文件 | 说明 |
|----|------|------|
| S1 登录 | [viewer-login.pen](./viewer-login.pen) | 左右分栏 800×520；已有实现 `gateway_config.cpp` |
| S2 目录台 | [viewer-console.pen](./viewer-console.pen) Frame A | 菜单/工具栏/树/分割条/列表/状态栏 |
| S3 会话台 | 同上 Frame B | Tab 溢出 + ◀▶ 导航；letterbox；分割条拖动态；右键浮层 |
| Tab 右键 | 同上 Frame C | 关闭 / 关闭其他·全部 / 拖出·拖回 / 全屏 / 只读 |
| 侧栏调宽 | 同上 Frame D | 分割条默认/悬停态 + 拖动中预览（宽度芯片） |
| Tab 溢出 | 同上 Frame E | 左端/中段/右端三态导航按钮 |
| Agent 对照 | [agent-gateway-config.pen](./agent-gateway-config.pen) | 顶栏型配置窗，同源色板 |

在 Cursor 中用 Pencil 打开对应 `.pen` 预览。本 Agent 会话若 Tools & MCP 中无 `pencil`，可直接编辑 JSON 或新开对话后再用 MCP 改画布。

后续实现批次见 [viewer-implementation-plan.md](../viewer-implementation-plan.md)（V1–V4 已完成）。
