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

## 坑：开设备管理器后键鼠失效（与「Mirror 采帧」无关）

**结论（2026-08-01 真环核实）：** 开 DM 时键鼠失效 **不是**「会话中开着 Mirror 采帧」导致的。VNC / 管理员 Road Desk 开着 Mirror 开 DM 都正常。

**复测对照（同构建、不开 DM→GDI）：**

| Host 权限 | 开 DM | 结果 |
|-----------|-------|------|
| **管理员** | 保持 Mirror | **正常**（键鼠可用） |
| **非管理员** | 保持 Mirror | **键鼠失效** |

→ 可确认：**管理员权限决定了开 DM 后键鼠是否失效**（非管理员写不了 HKLM，清不掉 `Attach.ToDesktop` / peer scrub 失效）。

**机制根因：** `Attach.ToDesktop=1` 粘在注册表（挂接曾用 `CDS_UPDATEREGISTRY`，或 peer VNC/Radmin 未清）。DM 一刷 PnP，系统按注册表把 Mirror **再挂一遍** → Win7 键鼠/焦点失效。会话中反复写自身 `DeviceKey` 也会加重。非管理员无法完成启动时 `force_detach` 与会话中 peer scrub，故一开 DM 必现。

**错误绕开（已废弃，勿再加回）：**

| 错误做法 | 后果 |
|----------|------|
| 开 DM → CDS 卸 Mirror → 临时 GDI | 拖窗明显卡（`cap_ms` 百毫秒级）；CDS 还会触发「配色方案已更改为 Windows 7 Basic」 |
| 开 DM → 软切 GDI（不 CDS） | 不改主题，但仍慢；且没有必要 |
| 「测远控别开设备管理器」 | 只是绕开，不是方案 |

**正确策略（对齐 VNC）：**

1. INF 默认 `Attach.ToDesktop=0`
2. 挂接：注册表写 `1` → `CDS`（优先 **无** `UPDATEREGISTRY`）→ 立刻清回 `0`；失败才回退带 `UPDATEREGISTRY` 的 CDS，事后仍清 `0`
3. **`replace_host … mirror` 必须管理员**（硬条件）：启动 `rdm_force_detach` + 清 peer/`rdmmini`；非管理员清 HKLM 失败 → 开 DM 键鼠失效
4. 会话中：**只 scrub 第三方** Mirror（约 2s）；**禁止**反复写正在附着的 Road Desk `DeviceKey`
5. **开着设备管理器/计算机管理：继续 Mirror 采帧**，不要为 DM 切 GDI、不要为 DM 做 CDS 卸挂
6. CDS attach/detach 后按挂接前快照恢复 DWM（避免无谓改主题）；DM 路径本身不应触发 CDS
7. 会话结束：`force_detach` + 全量 scrub

**已冻住时（旧二进制 / Attach=1 残留）：**

```bat
c:\rd\mirror_probe.exe detach
```

或重启 Host（管理员）。

## 坑：连上远控后客户机变成 Windows 7 Basic

**现象：** 起 Viewer / VNC 连上后，客户机弹出「配色方案已更改为 Windows 7 Basic」。

**原因：** Win7 规定 **XPDM Mirror 与 Aero（DWM）不兼容**。会话挂接 Mirror（Road Desk 或 VNC）时，系统关掉桌面合成并切到 Basic。**不是 Viewer 改主题**；VNC 用 Mirror 时同样会变 Basic（已对照）。

**预期：** 远控会话期间多为 Basic；断开并卸 Mirror 后多数会回到 Aero。若要会话中保持 Aero，只能走 GDI（更卡）或 Win10+ DXGI，不能靠 Win7 Mirror。

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
