# GM1 Win7 真机装卸清单

在装有（或未装）现场 VNC/Radmin Mirror 的 Win7 x64 上勾选。默认 **不**禁用第三方。  
操作细节见 **[sc-workflow.md](./sc-workflow.md)**。

## 准备

- [ ] 已开测试签名并重启（`bcdedit` 见 `testsigning Yes`）
- [ ] 已拷贝 `rdmmirror.sys`（已签）、`RoadDeskTestCert.cer`、`mirror_probe.exe`（如 `c:\rd\`）
- [ ] `certutil -addstore Root/TrustedPublisher` 已导入证书
- [ ] 记录样机名、是否已装 VNC/Radmin Mirror

## 安装（sc 流程）

- [ ] `copy` → `system32\drivers\rdmmirror.sys`
- [ ] `sc create rdmmirror ...`（或服务已存在）
- [ ] `sc start rdmmirror` → `STATE: RUNNING`（非错误 577）
- [ ] 桌面无花屏；第三方远控（若开着）仍可用
- [ ] **无需**在设备管理器「显示适配器」里再装控制设备

## 探测

- [ ] `mirror_probe.exe` → `GM1_LOADED ok`，退出码 0
- [ ] 再跑一次仍稳定
- [ ] `capture_ready=0` / `xpdm=0` 可接受（GM2）

## 卸载

- [ ] `sc stop rdmmirror` → `STOPPED`
- [ ] `mirror_probe.exe` → `FAIL not_loaded`（gle=2）
- [ ] （可选）`sc delete rdmmirror`
- [ ] （可选）重启一次仍正常进桌面

## 失败时

- [ ] Bugcheck 码 / 转储路径记入 [RESULTS.md](../RESULTS.md)
- [ ] 若疑似双 Mirror：临时禁用第三方复现 → 记「隔离结果」→ **恢复启用**
- [ ] 必现蓝屏：停 M，标注 Track P 可续

## 结果粘贴（填 RESULTS）

```
日期:
样机:
第三方 Mirror 状态（启用/禁用）:
装: OK / FAIL
probe: OK / FAIL
卸: OK / FAIL
蓝屏: 无 / 有（码=）
备注:
```
