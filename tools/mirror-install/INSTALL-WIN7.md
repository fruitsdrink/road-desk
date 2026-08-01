# Road Desk Mirror — Win7 安装

**只需一个文件：`RoadDeskMirrorSetup.exe`**

驱动与测试证书已打进该 exe（开发机 `make-package.ps1` 签名并嵌入）。  
Win7 上不要装 WDK，不要跑 makecert / signtool，也不需要旁边再放 `.sys`。

## 安装

1. 把 `RoadDeskMirrorSetup.exe` 拷到 Win7（任意目录）  
2. 双击运行，同意 UAC  
3. 若提示先重启（打开系统测试模式）→ 重启后**再双击一次**  
4. 装完再按提示重启  
5. **管理员**运行 `host-agent.exe`；日志应见 `capture=mirror`

卸载：`RoadDeskMirrorSetup.exe /uninstall`

## 说明

- 系统「测试模式」≠ 在本机签名  
- 勿卸现场 VNC/Radmin Mirror  
- Host 须管理员  
