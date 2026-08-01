# Track M — Road Desk Mirror Driver Spike

Win7 x64 **自研** Mirror 探针。与 [media-replace](../media-replace/)（Track P）双轨并行。

计划：[spike-media-replace.md](../../docs/spike-media-replace.md) §阶段 M · ADR-0004  
结论：[RESULTS.md](./RESULTS.md)

## 范围

| 做 | 不做 |
|----|------|
| GM1 控制设备装/卸 + probe | 接入 `replace_host`（M2） |
| GM2 XPDM Mirror + ExtEscape 脏区 | G4c 手感 / 正式 WHQL |
| 测试签文档与 sc / INF 流程 | 调用现场 VNC/Radmin Mirror |

**蓝屏停手**：必现蓝屏 → 停 Track M，Track P 可续。

## 目录

```
spikes/mirror-driver/
  docs/sc-workflow.md      # GM1：copy + sc
  docs/gm2-checklist.md    # GM2：XPDM INF + mirror_probe gm2
  driver/control/          # rdmmirror.sys
  driver/miniport/         # rdmmini.sys
  driver/display/          # rdmdisp.dll + dirty.c
  user/                    # mirror_probe + mirror_client
  scripts/build-*.ps1 sign-driver.ps1
```

## 构建

```powershell
# 用户态
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\build-user.ps1

# 驱动（WDK 7.1）
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\build-driver.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\sign-driver.ps1
```

产物在 `build\mirror-driver\`：

- `mirror_probe.exe`
- `driver\rdmmirror.sys`（GM1）
- `driver\rdmmini.sys` + `rdmdisp.dll` + `rdm_xpdm.inf`（GM2）
- `driver\RoadDeskTestCert.cer`

## 探测

```bat
mirror_probe.exe           :: GM1 控制设备
mirror_probe.exe gm2 10    :: GM2 挂接并轮询脏区（期间拖窗口）
```

- GM1 流程：[docs/sc-workflow.md](./docs/sc-workflow.md)
- GM2 流程：[docs/gm2-checklist.md](./docs/gm2-checklist.md)

## 设备名

| 角色 | 名称 |
|------|------|
| GM1 控制 | `\\.\RoadDeskMirror` / 服务 `rdmmirror` |
| GM2 显示 | DeviceString **Road Desk Mirror Driver** / `rdmmini` + `rdmdisp` |

## 合规

- 不链接 GPL / 商业 Mirror SDK
- WDK mirror 样例为起点，已裁剪重命名，不整包进正式产品
