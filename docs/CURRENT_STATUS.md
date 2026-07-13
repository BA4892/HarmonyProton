# Wine for HarmonyOS — 当前状态

> 更新: 2026-07-13
> 状态: ✅ VirGL | ✅ Audio | ✅ Explorer 桌面 | ✅ 输入 | ✅ 多窗口

---

## 里程碑

### 1. ARM64 Pad Box64 .so 方案 ✅

Box64 编译为共享库 (box64.so)，由 NCP 子进程 (`wine_child.so`) dlopen 加载，通过 `box64_hmos_main()` 在同一进程内模拟执行 x86_64 Wine ELF。

### 2. NCP 子进程架构 ✅

wine、wineserver、virgl_test_server 全部通过 `OH_Ability_StartNativeChildProcess` (NCP) 创建。Broker (`broker.cpp`) 中继 Wine 进程内 fork → NCP 的转换，支持命名多 fd 和环境变量转发。

### 3. Wine prefix 初始化 ✅

`wineboot --init` 完整生成 drive_c 目录结构（windows/system32, users, Program Files 等）和 registry。支持 WoW64 (32-bit PE DLL)。

### 4. Explorer 桌面 ✅

`explorer /desktop=shell,<w>x<h>` 桌面窗口通过 Wayland compositor 渲染上屏。

### 5. 输入框架 ✅

鼠标/键盘事件 ArkTS → NAPI → InputManager → Wayland → Wine 完整链路。支持物理像素 → Wine 逻辑坐标映射。

### 6. 音频 ✅

Host-broker 音频引擎：Wine 侧通过 IPC + ring buffer 传输 PCM，宿主进程集中持有 `OH_AudioRenderer`。支持 WASAPI / DirectSound / waveOut / MCI / MIDI（含 winehua-gm.sf2 SoundFont）。

### 7. VirGL / OpenGL ✅

guest Mesa (virpipe Gallium driver) → Unix socket → virgl_test_server (NCP 子进程) → virglrenderer → host EGL。支持 guest Mesa WGL contexts。性能优化：Native VSync 帧同步、帧缓冲复用、subsurface 合成签名。

---

## 当前架构

```
NCP 子进程 (appspawn)
  wine_child.so: Main() / WineserverMain() / MmapTestMain()
    ├─ arm64: dlopen("box64.so") → box64_hmos_main() → Wine x86_64 ELF
    └─ x86_64: dlopen("ntdll.so") → __wine_main()

  virgl_child.so: VirglTestServerMain()
    嵌入式 Wayland compositor (ARM64 原生, HAP 内)
      ↓ EGL/GLES
    XComponent 上屏
```

- Broker (`broker.cpp`) 中继 Wine 进程内 `CreateProcess` → NCP，转发环境变量
- Desktop 模式：staging → master PR，submodule push 先于主仓库合并

---

## 已修复问题

| # | 问题 | 修复方案 |
|---|------|---------|
| 1 | TEB 分配崩溃 | `anon_mmap_alloc()` fallback |
| 2 | Noexec 文件系统 PROT_EXEC | 匿名 mmap + pread |
| 3 | prctl(0x6a6974) SIGSEGV | Box64 my_prctl 清零 r10/r8 |
| 4 | PR_SET_NAME fallthrough SIGSEGV | Box64 my_prctl 直接 return 0 |
| 5 | Box64 fork 子进程 RWX 失败 | NCP appspawn + .so dlopen 替代 fork |
| 6 | wineserver ARM64 架构不匹配 | ARM64 下编 x86_64 PIE，Box64 加载 |
| 7 | entryParams `\|wine\|` 多余 | `USE_LIBBOX64` 环境变量按需跳过 |
| 8 | Box64 DEBUG 日志 I/O 过慢 | BOX64_LOG=0, WINEDEBUG=-all |
| 9 | dosdevices symlink 不可用 | 四条代码路径 fallback |
| 10 | XKB 键盘数据缺失 | xkeyboard-config 打包 + XKB_CONFIG_ROOT |
| 11 | 鼠标 action 常量错误 | 对齐 ArkTS MouseAction |
| 12 | dxg_surface use-after-free | wl_resource destroy 回调 + pending auto-destroy |
| 13 | 多窗口 XComponent exports 冲突 | surfaceId + XComponentController |

---

## 已知问题

| 问题 | 影响 | 说明 |
|------|------|------|
| services.exe 无法启动 | 非阻塞 | 错误 267 (目录无效) |
| dnsapi / nsiproxy 编译错误 | 跳过 | mingw 交叉编译 musl 不兼容 |
| Explorer 启动的程序 conhost 崩溃 | console 程序无法从桌面启动 | Box64 Signal 3 (SIGTRAP) |
| WINE_MONO=never 未设置 | wineboot 初始化时 Mono 安装尝试崩溃 | mscoree.dll 触发 install_mono → DialogBoxW |

---

## 构建命令

```bash
# arm64
make NATIVE_ARCH=arm64-v8a

# x86_64
make NATIVE_ARCH=x86_64
```

---

## 相关文档

- [BUILD_GUIDE.md](./BUILD_GUIDE.md) — 构建指南
- [ARCHITECTURE.md](./ARCHITECTURE.md) — 架构设计
- [OPENGL_VIRGL_DESIGN.md](./OPENGL_VIRGL_DESIGN.md) — VirGL/OpenGL 设计
- [AUDIO_ARCHITECTURE.md](./AUDIO_ARCHITECTURE.md) — 音频架构
- [.claude/rules/submodule-workflow.md](../.claude/rules/submodule-workflow.md) — Submodule 管理方案
