# XPDM display driver (GM2)

占位：将 WDK 7.1 **mirror** 样例的 `disp` 侧裁剪到此目录，设备/驱动名与 `RoadDeskMirror` / 本探针 INF 对齐。

GM1 **不**依赖本目录；采屏脏区与帧缓冲在此实现后，经控制设备 IOCTL（`IOCTL_RDM_GET_DIRTY` / `MAP_FB`）暴露给用户态。

**不要**把 GPL/商业 VNC Mirror 源码整包拷入。
