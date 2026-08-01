# Win7 x64 测试签名与装卸

目标：GM1 — 测试签下稳定加载 `rdmmirror.sys`，`mirror_probe` 可打开设备，可卸，无必现蓝屏。

**装卸主路径（推荐）**：见 **[sc-workflow.md](./sc-workflow.md)**（`copy` + `sc create/start/stop`）。  
本文侧重测试签与签名；`pnputil`  alone 不足以让 GM1 设备出现。

## 前置

- Win7 SP1 x64 真机（或同架构虚拟机）
- 管理员命令提示符
- 已产出：`rdmmirror.sys`（`build-driver.ps1`）+ 测试签名（`sign-driver.ps1`）+ `RoadDeskTestCert.cer` + `mirror_probe.exe`

## 1. 打开测试签名（一次性）

管理员 cmd：

```bat
bcdedit /set testsigning on
bcdedit /set nointegritychecks on
```

重启。用 `bcdedit` 确认 `testsigning Yes`（右下角「测试模式」水印可能被非正版水印盖住，以 `bcdedit` 为准）。

关闭（验收结束后如需）：

```bat
bcdedit /set testsigning off
bcdedit /set nointegritychecks off
```

再重启。

## 2. 签名驱动（开发机）

推荐脚本（WDK 7.1 已装）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\build-driver.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\sign-driver.ps1
```

产物：

- `build\mirror-driver\driver\rdmmirror.sys`（已签）
- `build\mirror-driver\driver\RoadDeskTestCert.cer`

把 `.sys` + `.cer` + `mirror_probe.exe` 拷到 Win7（如 `c:\rd\`）。

手动等价（可选）：

```bat
makecert -r -pe -ss PrivateCertStore -n CN=RoadDeskTestCert RoadDeskTestCert.cer
signtool sign /v /s PrivateCertStore /n RoadDeskTestCert rdmmirror.sys
```

## 3. 目标机导入证书 + `sc` 加载

见 **[sc-workflow.md](./sc-workflow.md)**。摘要：

```bat
certutil -addstore Root c:\rd\RoadDeskTestCert.cer
certutil -addstore TrustedPublisher c:\rd\RoadDeskTestCert.cer
copy /Y c:\rd\rdmmirror.sys %SystemRoot%\system32\drivers\rdmmirror.sys
sc create rdmmirror type= kernel start= demand error= normal binPath= \SystemRoot\system32\drivers\rdmmirror.sys DisplayName= "Road Desk Mirror"
sc start rdmmirror
mirror_probe.exe
```

期望：`GM1_LOADED ok`。错误 577 = 签名/证书/测试签问题。

## 4. 卸载

```bat
sc stop rdmmirror
mirror_probe.exe
```

期望：`FAIL not_loaded`。彻底删除服务：`sc delete rdmmirror`。

## 5. 回滚 / 蓝屏

- 蓝屏：安全模式 `sc stop` / `sc delete`，记 bugcheck 到 RESULTS。
- **必现蓝屏 → Track M 停手**，P 轨可续。

## 构建机 WDK 7.1

1. 安装 [WDK 7.1.0](https://www.microsoft.com/download/details.aspx?id=11800)（ISO 内跑 `KitSetup.exe`）。
2. 典型路径：`C:\WinDDK\7600.16385.1`。
3. 本仓：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\build-driver.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File spikes\mirror-driver\scripts\sign-driver.ps1
```

或开始菜单 **x64 Free Build Environment** → `cd` 到 `driver\control` → `build -cZ`。
