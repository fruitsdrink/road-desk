# GM1 控制设备：`sc` 装卸流程（推荐）

Win7 x64 上加载/卸载 Road Desk Mirror **控制设备**（`rdmmirror.sys`）的实测流程。  
`sc` = 系统自带的服务控制工具（Service Control），用来注册/启动/停止内核驱动服务。

> GM1 控制设备**不会**出现在设备管理器「显示适配器」里。  
> 仅 `pnputil -i -a` 往往只把驱动包进仓库，**不会**自动出现 `\\.\RoadDeskMirror`——请用本页的 `copy` + `sc`。

相关：测试签 [test-signing.md](./test-signing.md) · 勾选清单 [gm1-checklist.md](./gm1-checklist.md)  
GM2（显示适配器 XPDM + 脏区）见 **[gm2-checklist.md](./gm2-checklist.md)**，与本页控制设备流程独立。

---

## 需要拷进目标机的文件

建议目录：`c:\rd\`

| 文件 | 来源 |
|------|------|
| `rdmmirror.sys` | `build-driver.ps1` 后再跑 `sign-driver.ps1`（必须已测试签名） |
| `RoadDeskTestCert.cer` | `sign-driver.ps1` 产出 |
| `mirror_probe.exe` | `build-user.ps1` |
| `roaddesk_mirror.inf` | 可选（GM1 主路径不用；留给以后 PnP/XPDM） |

开发机产物目录：`build\mirror-driver\driver\` 与 `build\mirror-driver\mirror_probe.exe`。

---

## 一次性准备（每台 Win7）

管理员 CMD：

```bat
bcdedit /set testsigning on
bcdedit /set nointegritychecks on
```

**重启**。用 `bcdedit` 确认存在：

```text
testsigning             Yes
```

导入测试证书（每台机一次）：

```bat
certutil -addstore Root c:\rd\RoadDeskTestCert.cer
certutil -addstore TrustedPublisher c:\rd\RoadDeskTestCert.cer
```

---

## 第一次安装并启动

管理员 CMD，文件已在 `c:\rd\`：

```bat
copy /Y c:\rd\rdmmirror.sys %SystemRoot%\system32\drivers\rdmmirror.sys

sc create rdmmirror type= kernel start= demand error= normal binPath= \SystemRoot\system32\drivers\rdmmirror.sys DisplayName= "Road Desk Mirror"

sc start rdmmirror
sc query rdmmirror
mirror_probe.exe
```

### 期望

| 步骤 | 期望 |
|------|------|
| `sc create` | `CreateService 成功`（若已存在会报错，可跳过 create） |
| `sc start` | `STATE: 4 RUNNING` |
| `mirror_probe` | `GM1_LOADED ok`；`capture_ready=0` / `xpdm=0` 属正常（GM2 才有） |

### 常见失败

| 现象 | 原因 | 处理 |
|------|------|------|
| `sc start` 错误 **577** | 未签名 / 证书未导入 / 测试签未开 | 用已签名 `.sys`；`certutil -addstore`；检查 `bcdedit` 后重启 |
| `mirror_probe` `gle=2` not_loaded | 服务未 RUNNING | `sc query` → `sc start` |
| `sc create` 已存在 | 以前注册过 | 直接 `sc start`，或先 `sc delete` 再 create |

---

## 日常：停 / 再开（服务已 create）

```bat
sc stop rdmmirror
sc query rdmmirror
mirror_probe.exe
```

停干净后 probe 应为 `FAIL not_loaded`（gle=2）。

再开：

```bat
copy /Y c:\rd\rdmmirror.sys %SystemRoot%\system32\drivers\rdmmirror.sys
sc start rdmmirror
mirror_probe.exe
```

更新了新编的 `.sys` 时：先 `sc stop`，再 `copy`，再 `sc start`。

---

## 彻底删除服务注册

```bat
sc stop rdmmirror
sc delete rdmmirror
```

不删除 `%SystemRoot%\system32\drivers\rdmmirror.sys` 文件；下次按「第一次安装」再 `sc create`。

可选清理驱动包（若曾 `pnputil` 过）：

```bat
pnputil -e
:: 找到 Road Desk / oem##.inf 后：
pnputil -f -d oem##.inf
```

---

## `sc` 命令速查

| 命令 | 作用 |
|------|------|
| `sc create rdmmirror ...` | 注册内核服务（第一次） |
| `sc start rdmmirror` | 加载驱动，创建 `\\.\RoadDeskMirror` |
| `sc query rdmmirror` | 查看 RUNNING / STOPPED |
| `sc stop rdmmirror` | 卸载驱动（设备消失） |
| `sc delete rdmmirror` | 删除服务注册 |

一律使用**管理员**命令提示符。

---

## 与设备管理器的关系（GM1）

- **不需要**在设备管理器里再装一遍控制设备。
- 「显示适配器」里只有显卡（如 VMware SVGA）是正常的。
- GM2 XPDM Mirror 挂接后，才会在显示适配器下多一项（另文档）。
