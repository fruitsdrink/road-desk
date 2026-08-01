# 开发与联调环境

Related: [CONTEXT.md](../CONTEXT.md), [spike-media-plane.md](./spike-media-plane.md)

## 机器角色

| 机器 | 角色 | 跑什么 |
|------|------|--------|
| Mac mini M4 | 文档 / git（可选） | 不承担 Host/Viewer 兼容验收 |
| **Windows 11 真机** | **开发机** | 编译 Viewer、Host Agent；日常跑 **Viewer**；构建成功后**手动推送** Agent 到 Host |
| **Windows 7 x64 客户机** | **Host（真环）** | 常驻 **Host Sidecar**；运行被推送的 **Host Agent** |

Mac 上 Parallels 的 Win11 虚拟机非主链路，需要时仅作第二台 Viewer。

## 文档用语（中英）

| 中文 | 英文 | 说明 |
|------|------|------|
| 被控端 | **Host** | 被远控的 Win7 客户机 |
| 被控端代理 | **Host Agent** | 产品远控程序（推送到 Host） |
| 操作端 | **Viewer** | 开发机上发起远控的客户端；口语「控制端」= Viewer，书面用 Viewer |
| 操作员 | **Operator** | 人 |
| （开发工具） | **Host Sidecar** | 仅实验室：收 Agent 推送、替换进程、提供读日志 API |

Host Sidecar **不是**产品的一部分，不得打进正式交付包。

## 日常闭环

```text
1. 首次：手动将 Host Sidecar 复制到 Host 某目录，启动并保持运行
2. Win11：构建 Host Agent（及 Viewer）
3. Win11：构建成功后，手动将 Agent 推送到 Host Sidecar
4. Host Sidecar：若 Agent 进程在跑则先结束，再覆盖同目录 Agent，并拉起新进程（若约定自动启动）
5. Win11：用 Viewer 填 Host IP 直连联调
6. Win11 上的 AI：调用 Host Sidecar 的接口读取 Host 侧日志
```

快环（可选）：Agent 与 Viewer 都在 Win11 上跑，只作改代码冒烟；**MVP / 探针结论以 Win7 Host 真环为准**。

## Host Sidecar — 使用方法

### 首次安装（手动一次）

1. 在 Host（Win7）上任选一目录，例如 `C:\RoadDesk\sidecar\`
2. 将 Sidecar 单文件程序复制到该目录
3. 启动程序，保持运行（控制台可打印状态，便于开发期观察）
4. Agent 与 Sidecar **同目录**（或 Sidecar 配置所指向的固定目录）：推送覆盖的也是这里的 Agent 文件

### 推送 Agent（每次构建后，开发机手动触发）

1. 在 Win11 上构建出 Host Agent 产物
2. 开发机（或脚本 / AI）调用 Sidecar 的**接收接口**，将 Agent 推送到 Host
3. Sidecar 收到后：
   - 若检测到 Host Agent 进程在运行 → **先结束该进程**
   - 将新文件**覆盖**同目录原 Agent
   - （建议）再启动新的 Host Agent，避免每次再远程手工双击

「手动推送」指由开发者或开发机上的命令/AI 显式触发，而不是 Host 去扫开发机目录。

### 读取日志（给开发机 AI）

- Sidecar 在 Host 上暴露**只读日志接口**（局域网 HTTP 即可）
- Win11 上的 AI 通过该接口拉取 Host Agent（及 Sidecar 自身）日志，无需再登录 Win7 翻文件
- Host Agent 应把日志写到约定路径（文件）；Sidecar 负责对外提供读取，而不是改写远控协议

### 建议接口形状（实现时可微调）

| 方法 | 路径 | 作用 |
|------|------|------|
| `POST` | `/agent`（或 `/deploy`） | 上传 Agent 产物；杀旧进程 → 覆盖 → 可选拉起 |
| `GET` | `/logs` | 返回/流式读取 Host Agent 日志（可支持 `tail` 行数或从某偏移） |
| `GET` | `/status` | Sidecar 是否运行、Agent 是否在跑、当前文件版本/时间戳 |

仅绑定局域网；无鉴权或仅共享开发口令。**禁止**按生产远控标准暴露到公网。

## 构建与兼容约定

- 发布/联调目标：**Windows x64**
- Viewer 日常在 Win11；里程碑出门前在 Win7 上打开 Viewer 连一次（操作端需支持 Win7）
- 推送物须包含 Agent 运行所需依赖（或静态链接），避免 Win7 缺 DLL 误判
- 媒体面 MVP 可用 GPL 组件做内部验证；正式交付前换合规底座（见 ADR-0002）

## 与产品边界

| Host Sidecar 做 | Host Sidecar 不做 |
|-----------------|-------------------|
| 收推送、杀进程、覆盖 Agent | 远控画面/键鼠（产品媒体面） |
| 提供开发用读日志 API | 审计、目录、中继、账号 |
| 开发期控制台状态 | 现场静默安装与交付 |
| （远期）测试编排所需的 Host 侧动作 | 完整自动测试框架本身 |

## 远期：用于自动测试

可以。Host Sidecar 适合作为 **Host 侧测试执行器**，由开发机上的脚本/CI/AI 编排；不适合演变为第二套远控产品。

### 适合放进 Sidecar 的能力

| 能力 | 说明 |
|------|------|
| 推送并替换 Agent | 已定：杀进程 → 覆盖 → 拉起 |
| 读日志 / 清日志 | 供断言「已监听」「认证失败」等 |
| 查状态 | Agent 是否在跑、版本/文件时间、监听端口是否打开 |
| 启停 Agent | 不推送文件也只重启/停止 |
| **抓屏（测试用）** | 返回 Host 当前桌面一帧（如 PNG），用于「黑屏/未拉起」等粗断言；**不是**远控媒体面，不要求连续帧与键鼠 |

### 应留在开发机编排层（不塞进 Sidecar）的能力

| 能力 | 说明 |
|------|------|
| 编译 | 仍在 Win11 构建 |
| 跑 Viewer / 建远控会话 | 测的是产品通路，应用 Viewer 或测试专用 Viewer 驱动 |
| 键鼠注入断言 | 属远控正确性，走产品会话，不走 Sidecar |
| 用例编排、报告、失败归档 | pytest / 脚本 / CI；Sidecar 只提供 HTTP 动作 |

### 典型自动测试流水线（目标形态）

```text
Win11 CI/脚本
  1. 构建 Host Agent（及 Viewer）
  2. POST Sidecar /deploy  → Host 上新 Agent 就绪
  3. GET  Sidecar /status   → 进程与端口正常
  4. GET  Sidecar /screenshot → 可选：桌面非黑屏
  5. 启动 Viewer，对 Host 发起真实远控（产品路径）
  6. GET  Sidecar /logs     → 断言连接/鉴权/错误码
  7. 失败时：归档日志 + 截屏到开发机
```

### 约束

- Sidecar 与抓屏 API **仅实验室/CI 网段**；不得随正式 Host Agent 交付到收费站
- 自动测试的「远控是否成功」以 **Viewer↔Agent 产品会话** 为准；Sidecar 抓屏只做部署与环境烟测
- Win7 真环仍是兼容门禁；CI 若只有 Win11，只能冒烟，不能替代 Win7 结论
