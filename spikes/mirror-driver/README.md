# Track M — Road Desk Mirror Driver Spike

Win7 x64 **自研** Mirror 探针。与 [media-replace](../media-replace/)（Track P）双轨并行。

计划：[spike-media-replace.md](../../docs/spike-media-replace.md) §阶段 M · ADR-0004  
结论：[RESULTS.md](./RESULTS.md)

## 范围（本切片 M0 → GM1）

| 做 | 不做 |
|----|------|
| 测试签下装上 / 卸掉控制设备 | 接入 `replace_host` |
| `mirror_probe` 打开设备、读状态 | 冲 G4c 手感 |
| IOCTL ABI 预留（脏区/帧缓冲） | 正式 WHQL |
| XPDM display/miniport 目录占位（GM2 填） | 调用现场 VNC/Radmin Mirror |

**蓝屏停手**：必现蓝屏 → 停 Track M，Track P 可续；记 RESULTS。

## 目录

```
spikes/mirror-driver/
  README.md
  RESULTS.md
  docs/coexistence.md      # 与第三方 Mirror 共存（上道不强制卸）
  docs/test-signing.md     # 测试签 / 签名
  docs/sc-workflow.md      # GM1 推荐：copy + sc create/start/stop
  docs/gm1-checklist.md    # Win7 真机 GM1 装卸清单
  user/                    # 用户态 ABI + probe
  driver/                  # 控制设备 sys（GM1）+ XPDM 占位（GM2）
  scripts/build-user.ps1
  scripts/build-driver.ps1
  scripts/sign-driver.ps1
```

## 快速构建（用户态，无需 WDK）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\build-user.ps1
```

产物：`build\mirror-driver\mirror_probe.exe`

```text
mirror_probe.exe          # 尝试打开 \\.\RoadDeskMirror
```

无驱动时退出码非 0、打印明确错误（正常）。装上驱动后再跑一次应为 OK / `GM1_LOADED`。

## 驱动构建（需要 WDK 7.1）

见 [docs/test-signing.md](./docs/test-signing.md)。摘要：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\build-driver.ps1
```

未检测到 WDK 时脚本打印安装指引并失败（非静默成功）。

签名（开发机）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\sign-driver.ps1
```

Win7 上加载：见 **[docs/sc-workflow.md](./docs/sc-workflow.md)**（不要只靠 `pnputil`）。

## 设备名

- 控制设备：`\\.\RoadDeskMirror`（内核 `\Device\RoadDeskMirror`）
- 服务名：`rdmmirror`
- **不**使用 VNC/Radmin 的设备名或 INF 硬件 ID

## 合规

- 不链接 GPL Mirror / 商业 Mirror SDK
- WDK mirror 样例仅作 GM2 XPDM 起点，不整包进正式产品
