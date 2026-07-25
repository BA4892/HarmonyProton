# Wine for HarmonyOS — 技术文档

## 项目概述

将 Wine 移植到 HarmonyOS (OpenHarmony)，使 Windows 程序在鸿蒙系统上运行。

当前架构：Wine (x86_64, musl) + Box64 (ARM64) → Wayland compositor → 鸿蒙 XComponent 上屏。

## 文档索引

### 当前状态
- **[CURRENT_STATUS.md](CURRENT_STATUS.md)** — 里程碑、已修复问题、已知问题

### 架构与设计
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — Wine 内部架构、Wayland compositor 设计
- **[OPENGL_VIRGL_DESIGN.md](OPENGL_VIRGL_DESIGN.md)** — VirGL/OpenGL 设计
- **[AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md)** — 音频架构

### 构建
- **[BUILD_GUIDE.md](BUILD_GUIDE.md)** — 构建步骤、产物说明
- **[BUILD_ENV.md](BUILD_ENV.md)** — 从零搭建构建环境

### 技术分析
- **[WINE_MUSL_GLIBC_DIFF.md](WINE_MUSL_GLIBC_DIFF.md)** — glibc → musl 逐项适配分析
- **[NOEXEC_MMAP_ANALYSIS.md](NOEXEC_MMAP_ANALYSIS.md)** — noexec 文件系统上 mmap+PROT_EXEC 问题深度分析
- **[OHOS_MMAP_ANALYSIS.md](OHOS_MMAP_ANALYSIS.md)** — OHOS mmap 权限调研报告
- **[BOX32_MMAP_PROBE.md](BOX32_MMAP_PROBE.md)** — Box32 32-bit mmap 探针测试结果

### 优化指南
- **[virgl_display_optimization_guide.md](virgl_display_optimization_guide.md)** — 显示管线优化设计

### 规划
- **[UNCERTAINTIES.md](UNCERTAINTIES.md)** — 剩余技术风险和待解决问题
- **[CPP_REFACTOR_PLAN.md](CPP_REFACTOR_PLAN.md)** — compositor 重构原则、Phase 0-6 执行记录

## 关键里程碑

| 日期 | 里程碑 |
|------|--------|
| 2026-06-12 | cmd.exe 在设备上运行 |
| 2026-06-13 | notepad.exe GUI headless 验证 |
| 2026-06-14 | NAPI 沙箱 + Wayland 渲染上屏 |
| 2026-06-15 | 多窗口架构 + 输入框架 |
| 2026-06-21 | ARM64 Pad Box64 .so 方案完成 |
| 2026-07-06 | 音频 Host Broker 引擎完成 |
| 2026-07-09 | VirGL / OpenGL guest Mesa 渲染完成 |
| 2026-07-13 | 渲染管线性能优化 (Native VSync, 合成签名, 缓冲复用) |
