# Mirror 装站 / 测试签（任务 2）

Win7 x64 安装 Road Desk 自研 Mirror（GM1 + GM2）。  
驱动源码与深潜文档：[`spikes/mirror-driver/`](../../spikes/mirror-driver/)。

| 深潜文档 | 路径 |
|----------|------|
| 测试签 | [test-signing.md](../../spikes/mirror-driver/docs/test-signing.md) |
| GM1 / GM2 / 共存 | [sc-workflow.md](../../spikes/mirror-driver/docs/sc-workflow.md) · [gm2-checklist.md](../../spikes/mirror-driver/docs/gm2-checklist.md) · [coexistence.md](../../spikes/mirror-driver/docs/coexistence.md) |

**不做：** WHQL；卸现场第三方 Mirror；Host `requireAdministrator`（任务 3）。

---

## 职责划分（重要）

| 机器 | 做什么 | 不做什么 |
|------|--------|----------|
| **开发机**（WDK） | 编驱动 + **SignTool 测试签名** + 打 stage 包 | — |
| **Win7 Host** | 导入 `.cer`、打开系统「测试模式」(`bcdedit`)、安装**已签名**的 sys/dll | **不**跑 makecert / signtool |

### 1) 开发机（WDK 7.1）— 一条命令出包（含签名）

```powershell
cd <repo>
powershell -NoProfile -ExecutionPolicy Bypass -File tools\mirror-install\make-package.ps1
```

内部：`build-driver` → **`sign-driver`（本机签名）** → `stage-package`。

产物目录（内含已签名二进制 + 公钥证书）：

```text
build\mirror-package\win7-x64\
  Install-RoadDeskMirror.bat   ← 现场双击这个
  rdmmirror.sys / rdmmini.sys / rdmdisp.dll   ← 已在开发机签好
  RoadDeskTestCert.cer                       ← 仅供 Win7 导入信任
  …
```

把 **整个文件夹** 拷到 Win7。

### 2) Win7 — 双击安装（不签名）

1. 双击 **`Install-RoadDeskMirror.bat`**（UAC）
2. 若系统尚未开**测试模式**：安装程序只改 `bcdedit` 并提示重启 → 重启后**再双击一次**装驱动（仍不签名）
3. 已装过 → 先卸再装；装完再提示重启
4. **管理员**跑 `host-agent.exe`；`host-agent.log` 见 `capture=mirror`

卸载：双击 `Uninstall-RoadDeskMirror.bat`。

```text
[开发机]  make-package.ps1  (= 编译 + SignTool 签名 + 打包)
              ↓ 拷贝 win7-x64
[Win7]    Install-RoadDeskMirror.bat  (= 导入证书 + 测试模式 + 装已签名驱动)
              ↓
          重启 → host-agent（管理员）
```

---

## 高级：分步脚本（排障用）

| 脚本 | 作用 |
|------|------|
| `build-and-sign.ps1` / `stage-package.ps1` | `make-package` 内部调用 |
| `enable-testsigning.ps1` | 仅 bcdedit |
| `import-test-cert.ps1` | 仅证书 |
| `install-gm1.ps1` / `install-gm2.ps1` | 分步装卸 |
| `verify.ps1` | 服务 / probe 检查 |

`sc start` 错误 **577** → 测试签未生效或证书未导入；用双击安装程序走完「重启后再装」即可。
