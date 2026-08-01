# Road Desk Mirror — Win7 安装（现场包）

**驱动签名只在开发机完成**（`make-package.ps1` / SignTool）。  
本目录里的 `.sys` / `.dll` 应已签好；Win7 **不要**安装 WDK，也 **不要** 运行 makecert / signtool。

本机安装程序只会：导入 `RoadDeskTestCert.cer`、打开系统测试模式（`bcdedit`）、复制并注册已签名驱动。

## 安装（推荐）

1. 用**开发机**重新跑 `make-package.ps1`，再把新的 `win7-x64` 整份拷到 Win7（勿用未签名旧包）
2. **双击** `Install-RoadDeskMirror.bat`（同意 UAC）
3. 若提示「先重启再装」→ 那是打开系统测试模式，不是签名 → 重启后**再双击一次**同一 bat
4. 安装结束后按提示**重启**
5. **管理员**运行 `host-agent.exe`；`host-agent.log` 应见 `capture=mirror`

已装过会先自动卸载再重装。

若 bat 报 `tlocal` / `tle` 一类「不是内部或外部命令」，说明包是旧版（LF 换行）；请重新 `make-package` 再拷贝。

## 卸载

双击 `Uninstall-RoadDeskMirror.bat`。

## 说明

- 系统「测试模式」≠ 在本机签名；签名产物来自开发机
- 勿卸现场 VNC/Radmin Mirror
- Host 须管理员（任务 3 提权产品化另做）
