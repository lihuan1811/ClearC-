# C 盘清理大师 C++ Qt

本仓库只包含独立的 C++/Qt 桌面程序。Python/Flutter 项目位于同级目录
`../One-click-cleaning-of-C-drive`，构建和发布互不影响。

## 产品功能

- C 盘深度清理：临时文件、Windows 更新缓存、系统日志与 DUMP、缩略图与回收站、浏览器缓存、冗余安装包和压缩镜像。
- 软件强力卸载：桌面程序与微软商店应用、搜索排序、批量卸载、注册表备份、残留文件与启动项清理。
- 系统智能优化：办公稳定、电竞提帧、NVIDIA/AMD 支持项检测、高级系统管控，以及逐项还原。
- 磁盘文件管理器：磁盘空间可视化、类型筛选、批量文件操作、文件粉碎、权限修复、普通迁移和系统目录迁移。
- CMD 系统修复：SFC、CHKDSK、DNS、Winsock、DISM 和 Windows 更新组件修复。

所有修改类操作均提供确认、日志，并在底部提供全局一键还原入口。程序启动时自动申请管理员权限。

## 本地构建

```bash
cmake -S . -B build
cmake --build build --config Release
```

CMake 同时支持 Qt 5.15 和 Qt 6。Windows 7 兼容构建使用 Qt 5.15；GitHub Actions 会生成 x86、x64 两种压缩包和单文件便携版 EXE，并发布到 GitHub Release。
