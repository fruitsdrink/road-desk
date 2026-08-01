# GM2 Win7 脏区进用户态清单

前置：GM1 已通；本机已编并**测试签名** `rdmmini.sys` + `rdmdisp.dll`。  
默认**不**禁用现场 VNC/Radmin Mirror。

## 准备

- [ ] `testsigning Yes`
- [ ] 已导入 `RoadDeskTestCert.cer`（Root + TrustedPublisher）
- [ ] 文件在 `c:\rd\`：`rdmmini.sys`、`rdmdisp.dll`、`rdm_xpdm.inf`、`mirror_probe.exe`、证书

先手动拷文件（`pnputil` **不会**自动出现显示适配器节点）：

```bat
copy /Y c:\rd\rdmmini.sys %SystemRoot%\system32\drivers\
copy /Y c:\rd\rdmdisp.dll %SystemRoot%\system32\
dir %SystemRoot%\system32\drivers\rdmmini.sys
dir %SystemRoot%\system32\rdmdisp.dll
```

## 安装设备节点（关键）

`pnputil -i -a rdm_xpdm.inf` 往往只进驱动仓库（如 `oem10.inf`），**显示适配器里仍只有 VMware SVGA**——必须再建设备：

1. 打开**设备管理器**
2. 菜单 **操作** → **添加过时硬件**
3. **下一步** → 选 **安装我手动从列表选择的硬件(高级)** → 下一步
4. 选 **显示适配器** → 下一步
5. **从磁盘安装** → 浏览到 `c:\rd\rdm_xpdm.inf` → 打开
6. 列表中选 **Road Desk Mirror Driver** → 下一步 → 完成  
   （若提示未签名，选仍然安装）
7. 回到设备管理器 → **显示适配器**  
   应同时看到 **VMware SVGA 3D** 与 **Road Desk Mirror Driver**

可选：先 `pnputil -i -a c:\rd\rdm_xpdm.inf` 把包导入仓库，再走上面向导（向导里也能直接从磁盘选 INF）。

- [ ] 显示适配器出现 **Road Desk Mirror Driver**
- [ ] 无必现蓝屏/花屏
- [ ] **重启**（刚装完 INF 时，设备管理器已有节点，但 `EnumDisplayDevices` 往往还没有；attach 会 FAIL）

重启后自检：

```bat
sc query rdmmini
dir %SystemRoot%\system32\drivers\rdmmini.sys
dir %SystemRoot%\system32\rdmdisp.dll
mirror_probe.exe list
```

`list` 里应出现带 `MIRROR` / `Road Desk Mirror` 的一项（如 `\\.\DISPLAY2`）。若没有，再查驱动是否签名/测试签、设备是否带感叹号。

## 探测脏区

```bat
mirror_probe.exe gm2 10
```

期间**拖动窗口**。期望：`GM2_DIRTY ok rects=N`。

```bat
mirror_probe.exe attach
mirror_probe.exe detach
```

## 卸载

```bat
mirror_probe.exe detach
```

设备管理器卸载 **Road Desk Mirror Driver**；或 `pnputil -e` 找到 oem## 后 `pnputil -f -d oem##.inf`。

## 结果粘贴（RESULTS）

```
日期: 2026-08-01
样机: VMware Win7 x64 (VMware SVGA 3D 并存)
INF/设备节点: OK (Road Desk Mirror Driver + 重启)
gm2 probe: OK (device=\\.\DISPLAYV4 info 1504x957; dirty + dirty_sample.bmp)
蓝屏: 无
备注: rdmmini RUNNING; list 需新版 mirror_probe（旧版会误走 GM1）
```
