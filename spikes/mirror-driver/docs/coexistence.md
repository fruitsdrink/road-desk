# 与现场第三方 Mirror 共存

现场车道机常见已装 `VNC Mirror Driver`、`Radmin Mirror Driver V3` 等。Road Desk **正式依赖自研** `RoadDeskMirror`，不调用第三方。

## 政策（已定）

| 场景 | 做法 |
|------|------|
| **上道 / 日常** | 第三方 Mirror **可继续安装并启用**（运维兜底远控）。自研驱动并列挂接。 |
| **正式采屏** | 用户态 **只**打开 `\\.\RoadDeskMirror`。 |
| **GM1/GM2 验收** | 默认在「第三方仍启用」环境下装/卸/打开。 |
| **隔离排障** | 双 Mirror 导致必现蓝屏/花屏时，**临时**在设备管理器禁用其一做复现；测完恢复。隔离 ≠ 上道政策。 |
| **G4c 对照（可选）** | 需要「单 Mirror 公平对打」时可临时禁用第三方；测完恢复。不写进装站硬条件。 |

## 设备管理器打开后鼠标失效（根因与修复）

**根因：** 挂接时用了 `CDS_UPDATEREGISTRY`，把 `Attach.ToDesktop=1` 写进注册表。设备管理器一刷新 PnP，系统按注册表把 **Road Desk + VNC** 等 Mirror 再挂一遍，Win7 上易导致鼠标/焦点失效。  
「测远控别开设备管理器」只是绕开，不是方案。

**修复（代码 + INF，对齐 VNC 系「用完卸干净」）：**

1. INF 默认 `Attach.ToDesktop=0`
2. 挂接前 / Host 启动：`rdm_force_detach` 清注册表 + CDS 卸挂
3. `rdm_attach_mirror`：先写注册表 `=1` 再 CDS 挂接（UltraVNC 顺序）；挂上后**立刻把注册表写回 0**（会话仍附着，避免 PnP/设备管理器按注册表再挂死鼠标）
4. 会话结束 / Host 启动：`force_detach` CDS 卸挂 + 注册表 0  
5. 仍建议：远控会话中途尽量别开设备管理器（VNC Mirror 自己的 `Attach.ToDesktop=1` 我们改不了）

请更新 VM 上的 `replace_host.exe` + `mirror_probe.exe`。一次清理：

```bat
c:\rd\mirror_probe.exe detach
```

验证：detach 后开设备管理器，鼠标应正常；再跑 `replace_host … mirror` 远控，停 Host 后再开设备管理器仍应正常。

## 临时禁用（仅隔离 / 对照）

1. `Win+Pause` → 设备管理器 →「显示适配器」或「显示」下找到第三方 Mirror。
2. 右键 → 禁用设备（不要卸载，除非明确要清干净）。
3. 复现 / 对照结束后 → 启用设备。

**注意：** 已出现「开设备管理器鼠标死」时，优先键盘 `mirror_probe detach` 或禁用 **Road Desk Mirror**，不要再靠鼠标点。

## 自研驱动识别

| 项 | 值 |
|----|-----|
| 设备 / 符号链接 | `RoadDeskMirror` |
| 服务 | `rdmmirror` |
| INF | `roaddesk_mirror.inf` |
| 硬件 ID（控制设备） | `Root\RoadDeskMirror` |

若设备管理器里看不到上述名称，说明装的不是本探针包。
