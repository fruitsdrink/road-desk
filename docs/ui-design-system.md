# Road Desk Win32 UI 设计规范

Status: **draft v0.1**（2026-08-04）— 先定规范，再统一 Viewer / Agent 实现  
Related: [CONTEXT.md](../CONTEXT.md), [tech-stack.md](./tech-stack.md), [gateway.md](./gateway.md)  
Skill: `.cursor/skills/win32-ui/SKILL.md`

## 1. 目标与范围

把 Viewer 与 Host Agent 的原生 Win32 界面收成**同一产品语言**：同一套颜色、字体、间距、控件尺寸、图标与 DPI 约定。  
Viewer 登录、管理台壳与 Agent 网关配置已共用 `src/ui/rd_tokens.h` / `rd_dpi.h`；应用图标为 `assets/brand/rd_app.ico`（`IDI_RD_APP`，挂主窗 / 登录 / Agent 配置 / 浮出会话）。

**范围内**

- Viewer：登录/网关配置、管理台壳（菜单/工具栏/树/列表/Tab/状态栏）、会话窗与等待态
- Host Agent：首次网关配置及后续必要的配置/状态窗（托盘等目标能力）
- 共用：MessageBox / 文件选择等系统对话框的标题与文案约定

**范围外（本期不规定）**

- 管理端 Web（`admin/`）— 可对齐色相，但不强制同一实现
- 远控画面像素本身（媒体面）— 仅规定会话窗 letterbox / 等待文案色
- Qt6 / Win10+ 产品线 — 另开文档时再映射 token

## 2. 产品视觉原则

| 原则 | 含义 |
|------|------|
| **运维工具，不是消费应用** | 冷静、可读、信息密度适中；忌紫霓虹、大卡片堆叠、装饰性渐变 |
| **品牌可识别** | 配置/登录类窗必须有一致的顶栏品牌区；管理台壳与配置窗同源色板 |
| **两端同族** | Viewer 与 Agent 的配置对话框是同一组件族，只改文案与字段 |
| **Win7 可跑** | 无依赖 Win10+ 视觉 API；圆角/阴影仅作可选增强，经典主题下仍可用 |
| **系统 DPI 先做对** | 布局与字体一律经缩放；Per-Monitor V2 属后续，未处理 `WM_DPICHANGED` 前不得宣称支持 |

参考锚点：现有 Viewer 登录色板（`src/viewer/gateway_config.cpp` 中 `kBg` / `kHeaderBg` 等）视为 v0 品牌种子，本规范将其升格为命名 token。

## 3. 设计 Token

实现集中在 `src/ui/rd_tokens.h`（颜色 / 字号 / 间距 / 控件 DIP）与 `src/ui/rd_dpi.h`（`rd_dip` / `rd_create_font`）；Viewer 与 Agent 禁止再散落魔法 RGB。

### 3.1 颜色

语义名优先于裸 `RGB()`。值以 sRGB 整数给出。

#### 品牌与表面

| Token | RGB | Hex | 用途 |
|-------|-----|-----|------|
| `rd.color.brand.bg` | 22, 30, 46 | `#161E2E` | 顶栏、品牌条、深色强调区 |
| `rd.color.brand.fg` | 255, 255, 255 | `#FFFFFF` | 顶栏主标题 |
| `rd.color.brand.fgMuted` | 160, 176, 200 | `#A0B0C8` | 顶栏副标题 |
| `rd.color.surface.app` | 246, 248, 251 | `#F6F8FB` | 对话框/配置页背景 |
| `rd.color.surface.panel` | 255, 255, 255 | `#FFFFFF` | 输入框、列表工作区、卡片面 |
| `rd.color.surface.chrome` | 236, 240, 245 | `#ECF0F5` | 工具栏、Tab 条、状态栏底 |
| `rd.color.surface.session` | 0, 0, 0 | `#000000` | 远控 letterbox / 无画面底 |

#### 文本

| Token | RGB | Hex | 用途 |
|-------|-----|-----|------|
| `rd.color.text.primary` | 28, 34, 46 | `#1C222E` | 正文、输入文字 |
| `rd.color.text.secondary` | 88, 98, 114 | `#586272` | 字段标签 |
| `rd.color.text.muted` | 140, 150, 164 | `#8C96A4` | 提示、占位说明 |
| `rd.color.text.onBrand` | 255, 255, 255 | `#FFFFFF` | 深色条上文字 |
| `rd.color.text.inverseMuted` | 160, 176, 200 | `#A0B0C8` | 深色条上次级文字 |
| `rd.color.text.sessionHint` | 220, 220, 220 | `#DCDCDC` | 会话等待文案 |

#### 边线与状态

| Token | RGB | Hex | 用途 |
|-------|-----|-----|------|
| `rd.color.border.subtle` | 220, 226, 234 | `#DCE2EA` | 分隔线、输入边框默认 |
| `rd.color.border.strong` | 180, 190, 204 | `#B4BECC` | 聚焦环外框、分割条 |
| `rd.color.accent` | 47, 107, 168 | `#2F6BA8` | 主按钮、选中、链接（品牌蓝，偏工具向） |
| `rd.color.accent.hover` | 38, 90, 145 | `#265A91` | 主按钮悬停 |
| `rd.color.accent.pressed` | 30, 74, 122 | `#1E4A7A` | 主按钮按下 |
| `rd.color.success` | 46, 125, 90 | `#2E7D5A` | 成功提示（如 PSK 已导入） |
| `rd.color.warning` | 180, 120, 40 | `#B47828` | 警告文案 |
| `rd.color.danger` | 176, 60, 60 | `#B03C3C` | 错误、破坏性操作 |
| `rd.color.focus.ring` | 47, 107, 168 | `#2F6BA8` | 键盘焦点（与 accent 同色相） |

#### 列表 / 树 / Tab（管理台）

| Token | RGB | Hex | 用途 |
|-------|-----|-----|------|
| `rd.color.row.hover` | 232, 238, 246 | `#E8EEF6` | 行悬停 |
| `rd.color.row.selected` | 210, 226, 244 | `#D2E2F4` | 行选中 |
| `rd.color.tab.active` | 255, 255, 255 | `#FFFFFF` | 活动 Tab 面 |
| `rd.color.tab.inactive` | 236, 240, 245 | `#ECF0F5` | 非活动 Tab |
| `rd.color.splitter` | 200, 208, 218 | `#C8D0DA` | 树/工作区分割条 |

**约定**

- 配置类自绘窗：**禁止**再混用 `COLOR_BTNFACE` 作为主背景；用 `rd.color.surface.*`。
- 管理台若暂保留部分系统控件皮肤，外壳（工具栏底、Tab、状态栏、分割条）仍须用上表 chrome/surface token，避免「登录一套、主界面另一套」。
- 高对比/高对比主题：MVP 不强制跟随系统高对比；若控件无法自绘，可回退系统色，但品牌顶栏保持 `brand.bg`。

### 3.2 字体

| Token | 字号 (pt) | 字重 | 用途 |
|-------|-----------|------|------|
| `rd.font.brand` | 16 | Semibold (600) | 顶栏 / 品牌栏产品名 |
| `rd.font.loginTitle` | 18 | Semibold (600) | Viewer 登录右侧欢迎标题 |
| `rd.font.title` | 12 | Semibold (600) | 对话框内小节标题（少用） |
| `rd.font.body` | 10 | Regular (400) | 正文、编辑框、按钮 |
| `rd.font.label` | 9 | Regular (400) | 字段标签 |
| `rd.font.caption` | 9 | Regular (400) | 状态栏、辅助说明 |
| `rd.font.mono` | 10 | Regular (400) | 端口、指纹、路径（可选） |

**字族栈（按序）**

1. `Segoe UI`（Vista+ / Win7+ 常见）
2. `Microsoft YaHei UI`（中文 UI 回退）
3. `Tahoma`（最后回退）

实现：`CreateFontIndirectW`，`CLEARTYPE_QUALITY`，`lfHeight = -MulDiv(pt, dpi, 72)`，`dpi` 取目标窗或屏幕 `LOGPIXELSY`。  
**禁止** Agent/Viewer 一侧设字体、另一侧依赖对话框默认字体。

所有控件创建后必须 `WM_SETFONT`（含 STATIC / EDIT / BUTTON / 自绘区）。

### 3.3 间距（4px 网格）

逻辑像素（96 DPI 下的 px）；实际绘制用 `rd_dip(v)` 缩放（见 §6）。

| Token | 值 | 用途 |
|-------|----|------|
| `rd.space.1` | 4 | 紧凑内边距 |
| `rd.space.2` | 8 | 控件内小间隙 |
| `rd.space.3` | 12 | 字段垂直间距（默认） |
| `rd.space.4` | 16 | 区块间距 |
| `rd.space.5` | 20 | 小节边距（小窗） |
| `rd.space.6` | 24 | 区块分隔 |
| `rd.space.7` | 28 | 对话框水平内边距（标准） |
| `rd.space.8` | 32 | 大区块 |

**对话框布局约定（标准配置窗，如 Agent 网关）**

| 元素 | Token / 值 |
|------|------------|
| 水平内边距 | `rd.space.7`（28） |
| 顶栏高度 | 72 |
| 标签高度 | 18 |
| 编辑框高度 | 28 |
| 字段间距（标签底→下一标签） | 编辑高 + `rd.space.3`（约 12） |
| 主/次按钮高 | 30 |
| 主按钮宽 | 100（中文两字～四字）；次按钮同宽 |
| 按钮间距 | 12 |
| 底栏按钮距客户区底 | 48（含按钮高） |
| 标准配置对话框客户区 | 420 × 384 |

**登录窗布局（Viewer，对齐管理系统登录页）**

参考 Vben Admin 等中后台：左右分栏，左侧品牌叙事、右侧表单；整体更大、留白更足。

| 元素 | Token / 值 |
|------|------------|
| 客户区宽 × 高 | 800 × 540（已导入 PSK 时 800 × 500） |
| 左侧品牌栏宽 | 320（`rd.color.brand.bg` 通高） |
| 右侧表单区内边距 | 40（水平）/ 36（顶） |
| 表单欢迎标题 | 18pt Semibold，`text.primary` |
| 表单副文案 | 9–10pt，`text.muted` |
| 编辑框高度 | 32 |
| 主登录按钮 | 宽随表单栏、高 36（`BS_DEFPUSHBUTTON`） |
| 次按钮（取消） | 100 × 36 |
| 品牌栏内边距 | 36 |
| 品牌标「RD」块 | 48 × 48，底 `accent` |

Agent 网关配置窗仍用标准顶栏配置尺寸；Viewer 登录用上表分栏尺寸。色板、字体、文案（`浏览…`）两端同源，不得一边 20px 边距、一边 40px 却混用色。

### 3.4 圆角与描边

原生 Win32 经典控件默认直角。产品层约定：

| Token | 值 | 适用范围 |
|-------|----|----------|
| `rd.radius.none` | 0 | 系统标准控件、分割条、状态栏 |
| `rd.radius.sm` | 2 | 自绘小芯片、Tag |
| `rd.radius.md` | 4 | 自绘按钮、自绘输入容器、轻量面板 |
| `rd.radius.lg` | 8 | 拖拽幽灵窗预览（可选） |

- 未做自绘前：允许直角，但**同一对话框内不要混用**圆角自绘与完全未对齐的系统按钮观感而不加统一背景。
- 禁止大圆角（≥16）与胶囊按钮——不符合运维台气质，且 Win7 上更显突兀。
- 描边默认 1 DIP：`rd.color.border.subtle`。

### 3.5 控件尺寸（96 DPI 逻辑值）

| 控件 | 高 | 备注 |
|------|----|------|
| 单行 EDIT | 28 | 含边框视觉 |
| BUTTON 主/次 | 30 | 最小点击高 |
| 工具栏条 | 32 | 启用：白 `panel` + `border.subtle` 圆角面（32×28）；悬停加强 `border.strong`；按下 `row.selected`；勾选（只读）`accent` 实心 + 白图标；禁用无面、`text.muted` 图标。会话钮：列表选中未连接设备 → 显示器图标「启动会话」；否则 ×「关闭会话」。左右 pad 8 DIP；全部断开 = lucide x-circle |
| Tab 条 | max(30, 字体高+10) | 与现逻辑一致；溢出时两端导航钮各 24×24（lucide chevron） |
| 状态栏 | 24 | |
| 树默认宽 | 240 | 夹取 120 … 客户宽-200；拖动调宽（见 `docs/design/viewer-console.pen` Frame D） |
| 详情默认宽 | 400 | 目录态右侧被控端详情；夹取 200 … 工作区一半 |
| 分割条宽 | 4 | 树/工作区之间；悬停与拖动用 accent + 握点；光标 `IDC_SIZEWE` |
| Tab 关闭热区 | 18×18 | |
| 列表列表示例 | 160 / 120 / 60 / 70 / 90 / 120 / 60 / 60 / 140 | 名称 / IP / 端口 / 版本 / 设备角色 / 位置 / 车道 / 状态 / 备注 |

### 3.6 图标

| 约定 | 说明 |
|------|------|
| 格式 | 应用图标用 `.ico`（含 16/32/48/256）；工具栏用 16×16 与 20×20（150% 用 24 更佳，见 DPI） |
| 风格 | 线性 Lucide 同款（24×24 viewBox、stroke 2、圆角端点）；登录窗用 GDI+ 按 Lucide 几何绘制 |
| 色 | 默认 `rd.color.text.muted` / `secondary`；主按钮上用 `onBrand`；忌彩色 Emoji 风 |
| 资源 | 参考 `assets/icons/lucide_*.svg`；运行时不依赖 SVG 解析。品牌图 `assets/brand/rd_login_brand.png` |
| 命名 | `rd_app.ico`、`rd_toolbar_connect.ico`、`rd_status_ok.ico` 等 `rd_` 前缀 |
| 未就绪时 | 窗口 `hIcon`/`hIconSm` 至少挂应用图标；工具栏可用 Segoe UI Symbol / 短文字，但两端文案一致 |

MessageBox 使用系统图标类型（`MB_ICONERROR` 等），标题统一为 `Road Desk`。

### 3.7 运动与层级

| Token | 值 | 用途 |
|-------|----|------|
| 拖拽幽灵不透明度 | 210 / 255 | 与现控制台一致 |
| 动画 | 默认无 | 远控工具避免炫目过渡；Tab 切换瞬时即可 |
| 阴影 | 默认无 | 可用 1px 分隔线代替 elevation |

## 4. 组件约定

### 4.1 品牌区（Brand Chrome）

「产品配置 / 登录」模态窗必须有一致品牌色与文案族；布局分两种：

**A. 顶栏型（Agent 等标准配置窗）**

```
┌──────────────────────────────────────┐
│  [brand.bg, h=72]                    │
│   Road Desk          ← brand 16pt    │
│   副标题…            ← muted 9–10pt  │
├──────────────────────────────────────┤
│  surface.app 内容区…                 │
└──────────────────────────────────────┘
```

**B. 左右分栏型（Viewer 登录，管理系统登录页）**

```
┌────────────────┬─────────────────────────┐
│ brand 背景图   │  surface.panel / app    │
│ + 深色蒙层     │  欢迎登录               │
│  [RD]          │  [icon] 输入框…         │
│  Road Desk     │  [icon] 密码… [eye]     │
│  操作端登录    │  □ 记住密码             │
│  slogan        │  [folder]浏览 / [→]登录 │
└────────────────┴─────────────────────────┘
```

左侧使用 `assets/brand/rd_login_brand.png`（铺满 + 半透明 `brand.bg` 蒙层保证文案对比度）；缺失时回退纯色 `brand.bg`。

| 窗 | 品牌主文案 | 副文案 / slogan |
|----|------------|-----------------|
| Viewer 登录 | `Road Desk` | 副：`操作端登录`；slogan：`连接目录服务，拉取被控端列表` |
| Agent 网关配置 | `Road Desk` | `被控端网关配置` |

禁止一端有品牌区、另一端纯系统对话框。

### 4.2 配置 / 登录对话框

**标准配置（Agent）** 字段顺序与视觉：

1. 顶栏  
2. 标签（`text.secondary` + `font.label`）  
3. 编辑框（白底、`text.primary`）  
4. 可选「浏览…」次按钮（与主按钮同高）  
5. 成功/错误行（`success` / `danger`）  
6. 底部分隔线（`border.subtle`）  
7. 右对齐：`取消`（次）+ `确定`（主，`BS_DEFPUSHBUTTON`）

**Viewer 登录** 另加：

1. 右侧欢迎标题 `欢迎登录`（不用再叠一层顶栏）  
2. 分区顺序：目录服务（主机/端口）→ 操作员帐号 → 记住密码 → 可选 PSK  
3. 输入框左侧线性图标（主机=服务器、用户=人像、密码=锁、PSK=钥匙）；左内边距 28 DIP  
4. 密码框右侧眼睛按钮：点击切换明文 / 掩码（`EM_SETPASSWORDCHAR`）  
5. 「记住密码」`BS_AUTOCHECKBOX`：勾选则把密码写入 `%ProgramData%\RoadDesk\viewer.json`（与 `username` 同文件）；取消勾选则清除落盘密码  
6. 「取消」「浏览…」「登录」为自绘按钮并带图标（× / 文件夹 / 登录箭头）；主按钮 accent 实心  
7. 主按钮「登录」占表单栏剩余宽度；「取消」固定宽在左  
8. PSK 分隔文案：`或导入 Viewer PSK`  
9. 登录分栏默认 **800×540**（已导入 PSK 时可略矮至 500）  
10. 右侧表单区底部右对齐显示 `vMAJOR.MINOR.PATCH`（`text.muted` / caption）

文案：

- 浏览按钮统一 `浏览…`（不要混用 `选文件…`）
- 窗类名可不同，**色板 / 字体 / DIP 缩放约定必须同源**

### 4.3 管理台壳（Console Shell）

布局（已有结构保留，换皮对齐 token）：

```
菜单
工具栏 (chrome)
┌────────┬──────────────────┬──────────┐
│ 树     │ 列表             │ 被控端详情│
│        │ Tab / 会话宿主   │（目录态） │
├────────┴──────────────────┴──────────┤
│ 状态栏                                │
└──────────────────────────────────────┘
```

- 背景：树/列表/详情 `surface.panel`；外壳 `surface.chrome`
- 状态栏左侧为操作提示；版本号用 `\t\t` 右对齐到状态栏右缘（`vMAJOR.MINOR.PATCH`）。修改 `src/viewer` 源码并编译 viewer 时自动递增 PATCH（见 `scripts/bump_product_version.ps1`）。
- 窗口标题：`Road Desk Viewer  MAJOR.MINOR.PATCH`；帮助→关于含版本与编译时间；登录页右侧表单区右下角同样显示 `vMAJOR.MINOR.PATCH`（`text.muted`）。
- 目录态选中列表行时，右侧详情显示 Host Agent 版本、主机名、网卡、OS/CPU/内存、心跳采样的 CPU/内存占用与开机时长、磁盘分区（来自心跳 `inventory`）；会话态隐藏详情面板。
- 菜单栏（`MIIM_BITMAP` + 加速键）：文件 退出(`log-out`, Alt+F4)；查看 刷新(`refresh-cw`, F5) / 下一标签(Ctrl+Tab) / 上一标签(Ctrl+Shift+Tab) / 切换窗格(F6)；会话 启动(`monitor`, Ctrl+Enter) / 关闭(`x`, Ctrl+W) / 全部断开(`x-circle`, Ctrl+Shift+W)；帮助 关于(`info`, F1)。顶层带 `&` 助记键。树/列表/Tab 条 `WS_TABSTOP` + `IsDialogMessage`；Tab 条方向键切页、Delete 关会话；列表/树 Enter 启动。远控会话持有键鼠时 LL hook 优先转发远端。**会话抢键期间不要求本机全键盘**（无交还热键；见 `viewer-implementation-plan.md`）。
- 侧栏分割条：树右缘 4 DIP（`rd.color.splitter`）；悬停/拖动 `accent` + 细握点；光标 `IDC_SIZEWE`；树宽夹取 120 … 客户宽−200（默认 240）；拖动中在分割条旁显示宽度芯片（`brand.bg` + 当前 DIP `N px`，松手消失）。稿见 Frame D。
- 会话 Tab：活动/非活动用 §3.1 Tab 色；关闭「×」热区 18 DIP
- **Tab 溢出导航**：标签总宽超过可视区时，条两端显示 ◀ / ▶（各 24×24）；未溢出则隐藏。到头侧按钮 `text.muted` 禁用，可滚动侧用 panel+border。新开/激活 Tab 时 `scrollIntoView`。稿见 Frame B / E。
- **右键菜单**（`CreatePopupMenu` + `MIIM_BITMAP` 图标，非系统灰 MessageBox）：关闭(`x`) / 关闭其他(`copy-x`) / 关闭全部(`x-circle`)；分隔；拖出(`panel-top-open`，已拖出则灰显) / 拖回(`panel-top-close`，停靠则灰显)；分隔；全屏(`maximize`)；只读(`eye`)。顶栏标题；已拖出 Tab 显示角标。稿见 `docs/design/viewer-console.pen` Frame C。

### 4.4 会话表面（Session Surface）

- 无帧：`surface.session` 填充
- 等待文案：`text.sessionHint`，`font.body`，居中
- 有帧：letterbox 黑边 + 居中缩放（保持现有 `fit_rect` 行为）

### 4.5 系统对话框

- `MessageBoxW` 标题：`Road Desk`
- 不自造彩色 MessageBox；错误细节进日志，UI 给短句

## 5. 文案与用语

遵循 `CONTEXT.md`：

- 界面：「操作端 / 被控端」，代码与英文 UI 标识：Viewer / Host（Agent）
- 避免：客户端、主控端、Slave、用户（过泛）
- 中文 UI 用简体；术语与网关文档一致（网关、PSK、目录）

## 6. DPI 与缩放

### 6.1 进程级（当前基线）

| 项 | 要求 |
|----|------|
| API | `SetProcessDPIAware()`（与现一致） |
| Manifest | Viewer **与 Agent** 均声明 `<dpiAware>true</dpiAware>`；Viewer 保留 Common Controls 6 |
| 目标 | System DPI Aware；在 100% / 125% / 150% 主屏验收 |

**明确不做（直到实现完整处理）**：Per-Monitor V2、`WM_DPICHANGED` 动态改布局。文档与探针（`spike-media-replace-pitfalls`）已说明：未处理 per-monitor 前不要开 V2。

### 6.2 缩放函数（约定）

逻辑单位 = 96 DPI 下的像素。

```text
rd_dpi(hwnd)     → 窗或 DC 的 LOGPIXELSY（至少 96）
rd_dip(hwnd, v)  → MulDiv(v, rd_dpi(hwnd), 96)
rd_font(pt, w)   → lfHeight = -MulDiv(pt, dpi, 72)
```

**必须缩放**：窗客户区、控件 CreateWindow 坐标与宽高、顶栏高、间距、图标尺寸、自绘坐标。  
**必须用 pt→px**：所有 UI 字体。  
**禁止**：只缩放字体、布局仍写死 96 DPI 坐标（登录页现状的主要问题）。

### 6.3 验收矩阵

| DPI | 检查 |
|-----|------|
| 100% | 基准观感 |
| 125% | 无文字裁切、按钮可点、顶栏高度与内容间距协调 |
| 150% | 同上；图标不模糊（有多分辨率资源时） |

远控画面路径另按媒体探针验收；与 chrome 缩放分开看。

## 7. 实现分期（规范层，非本提交改代码）

| 阶段 | 内容 |
|------|------|
| **P0** | ✅ `src/ui/rd_tokens.h` + `rd_dpi.h`；Agent 配置对齐 Viewer 登录壳色/字/间距 |
| **P1** | ✅ 管理台外壳 chrome/surface/accent；统一 UI 字体；会话等待 `font.body` |
| **P2** | ✅ 应用图标 `rd_app.ico` + 工具栏 Lucide DIP；共享 `src/ui` 字体/DPI 辅助 |
| **P3** | 评估自绘主按钮与 `rd.radius.md`；评估 Per-Monitor（需 `WM_DPICHANGED`） |

## 8. 反模式

- Viewer 登录精修、Agent 配置保持系统灰窗
- 管理台继续纯 `GetSysColor`，与登录页两套产品气质
- 魔法数字散落且两端不一致（20 vs 28 边距、24 vs 26 编辑高）
- 仅 `SetProcessDPIAware` 却不缩放布局
- Agent 无 `dpiAware` manifest / 无 Common Controls 6（若使用主题控件）
- 引入 ImGui / Qt / WebView「只为好看」——与 tech-stack 冲突
- 大圆角、紫渐变、卡片瀑布、Emoji 图标条

## 9. 变更流程

1. 先改本文件 token / 组件约定  
2. 再改代码；PR 说明对照的 token 名  
3. 截图验收：Viewer 登录、Agent 配置、管理台主窗（至少 100% 与 150%）

---

**维护**：视觉或交互约定变更时更新本文与 `.cursor/skills/win32-ui/`；避免只改一端源码。
