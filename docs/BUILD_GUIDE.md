# Wine for HarmonyOS — 构建指南

> 最后更新: 2026-07-03

## 环境

构建环境搭建详见 [BUILD_ENV.md](./BUILD_ENV.md)。

快速检查：

```bash
# 必备工具
make cmake ninja meson bison flex autoconf libtoolize gcc-mingw-w64-x86-64 java
# OHOS SDK: /apps/harmony/sdk/default/openharmony/
# wayland-scanner: 首次构建时自动从 thirdparty/wayland 编译
```

---

## 平台速查

| 平台 | Makefile 命令 | `NATIVE_ARCH` | Wine | Box64 | 打包 |
|------|-------------|---------------|------|-------|------|
| arm64 PC (execve) | `make DEVICE_TYPE=pc` | `arm64-v8a` | x86_64 .so | box64 可执行文件 | HNP |
| x86_64 PC (execve) | `make NATIVE_ARCH=x86_64 DEVICE_TYPE=pc` | `x86_64` | x86_64 .so | 无 | HNP |
| arm64 Pad (fork) | `make DEVICE_TYPE=pad` | `arm64-v8a` | x86_64 .so → rawfile zip | box64.so (dlopen) | rawfile zip |
| x86_64 Pad (fork) | `make NATIVE_ARCH=x86_64 DEVICE_TYPE=pad` | `x86_64` | x86_64 .so | 无 | rawfile zip |
| 双架构 HAP | `make NATIVE_ARCH=all` | 双架构 | 同上 | arm64 含 Box64 | 按 DEVICE_TYPE |

默认值：`NATIVE_ARCH=x86_64 DEVICE_TYPE=pc`。

> **关键区别:** PC 有 execve → Wine/wineserver 编为可执行文件，HNP 打包。Pad 仅 fork → Wine 放入 rawfile zip 运行时解压，arm64 下 Box64 编为 .so 由 NCP 子进程 dlopen。

---

## Makefile 构建（推荐）

Makefile 使用 stamp 文件跟踪各阶段，支持自动增量构建。

### 完整构建

```bash
# 默认: x86_64 PC
make

# arm64 Pad (当前主要调试目标)
make NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad

# 双架构 HAP
make NATIVE_ARCH=all DEVICE_TYPE=pad
```

### 单阶段构建

```bash
make deps                          # 交叉编译依赖 → build/sysroot-ext/
make wine                          # Wine + wineserver
make box64                         # Box64 ARM64 翻译器 (仅 arm64)
make native                        # 各架构原生 compositor 依赖
make native-arm64-v8a              # 仅 arm64 原生依赖
make native-x86_64                 # 仅 x86_64 原生依赖
make assemble                      # 组装 HNP/Pad 布局
make hap                           # HAP 打包 + 签名
```

### 增量构建

改了什么就只跑对应阶段：

```bash
# 只改 ArkTS / ets 文件
make hap NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad

# 改了 entry/src/main/cpp/ 下的 C++ 代码 (napi_init.cpp 等)
# hvigorw 的 cmake+ninja 自身有增量检测, 直接 hap 即可
make hap NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad

# 改了 Wine C 源码 (thirdparty/wine/)
make wine NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad && make hap ...

# 改了 native compositor 依赖 (thirdparty/libffi, wayland, libepoxy, virglrenderer)
make native NATIVE_ARCH=arm64-v8a && make hap ...

# 改了 thirdparty/freetype 或 Mesa 等 sysroot-ext 依赖
make deps && make wine ... && make hap ...

# 完全清理
make clean
```

### Stamp 文件

构建状态记录在 `build/.stamps/` 下：

```
build/.stamps/
├── deps                                 # sysroot-ext 依赖就绪
├── wine-arm64-v8a-pad                   # Wine 交叉编译完成
├── box64-arm64-v8a-pad                  # Box64 编译完成
├── arm64-v8a/
│   ├── native                           # ARM64 compositor 依赖完成
│   └── assemble-pad                     # Pad 布局组装完成
└── x86_64/
    ├── native
    └── assemble-pad
```

删除对应 stamp 文件即可强制重跑该阶段（保留中间产物）。

---

## build.sh 构建（兼容）

Makefile 封装了对 `scripts/` 下各脚本的调用，`build.sh` 仍可直接使用：

### PC

| 命令 | 说明 |
|------|------|
| `bash build.sh deps` | 构建交叉编译依赖 |
| `bash build.sh wine` | 构建 Wine |
| `bash build.sh box64` | 构建 Box64 (仅 arm64) |
| `bash build.sh hap` | 编译 ArkTS + Native → .hap → 签名 |
| `bash build.sh deploy <ip>` | 推送 HAP + 安装 |
| `bash build.sh quick <ip>` | deps → wine → box64 → assemble → hnp → hap → deploy |

### Pad

| 命令 | 说明 |
|------|------|
| `bash build.sh pad <arch>` | 完整构建 Pad HAP (`arm64` / `x86_64`) |
| `bash build.sh hap <arch>` | 仅 HAP (跳过 Wine 重编译) |
| `bash build.sh pad-deploy <ip>` | 推送并安装 |

> **注意:** build.sh 使用 `arm64`，Makefile 使用 `arm64-v8a`，对应关系：`arm64` → `arm64-v8a`。

---

## 环境变量

构建时关键变量（由 `scripts/env.sh` 自动设置）：

| 变量 | 默认 | 说明 |
|------|------|------|
| `DEVICE_TYPE` | `pc` | `pc` / `pad`，影响 assemble 分支和 `PAD_MODE` 宏 |
| `NATIVE_ARCH` | 随架构 | `arm64-v8a` 或 `x86_64` |
| `OHOS_SDK` | `/apps/harmony/sdk/default/openharmony` | HarmonyOS SDK 路径 |
| `TOOL_HOME` | `/apps/harmony` | hvigorw 路径 |

运行时变量（由 `wine_child.cpp` / `napi_init.cpp` 设置）：

| 变量 | 作用 |
|------|------|
| `BOX64_LD_LIBRARY_PATH` | Box64 搜索 x86_64 .so 的路径 |
| `BOX64_LOG` | Box64 日志级别 |
| `WINEDEBUG` | Wine 调试频道 (`-all` 关闭) |
| `XKB_CONFIG_ROOT` | XKB 键盘布局数据路径 |
| `LIBBOX64_SO` | Box64 cmake flag |

---

## 产物说明

### PC (HNP 布局)

```
out/staging/opt/winehua/
├── bin/
│   ├── wine, wineserver, box64        # 可执行文件
│   ├── ntdll.so, *.exe                # PE stubs
│   ├── x86_64-windows/                # PE DLL
│   └── x86_64-unix/                   # Unix .so
├── lib/arm64-v8a/
│   └── libc.so
└── share/
    ├── wine/ (nls/, fonts/, wine.inf)
    └── X11/xkb/
```

### Pad (rawfile zip + libs/)

```
entry/
├── libs/arm64-v8a/                    # ARM64 原生 .so
│   ├── box64.so                       # Box64, in-process dlopen
│   ├── libwine_child.so               # NCP 子进程入口
│   ├── libentry.so                    # NAPI 桥接
│   ├── libffi.so, libwayland-*.so     # compositor 依赖
│   ├── libepoxy.so, libvirglrenderer.so
│   └── virgl_test_server              # VirGL test server
├── libs/x86_64/                       # x86_64 Pad 原生 .so
│   └── ... (Wine x86_64 .so 直接放 libs)
└── resources/rawfile/
    └── wine-data.zip                  # 运行时解压
        ├── bin/
        │   ├── wine, wineserver       # x86_64 ELF
        │   ├── ntdll.so, *.exe
        │   ├── x86_64-windows/
        │   └── x86_64-unix/
        └── share/
            ├── wine/
            └── X11/xkb/
```

arm64 Pad: Wine x86_64 .so 全部在 zip 内由 Box64 加载，libs/ 只放 ARM64 原生 .so。
x86_64 Pad: Wine x86_64 .so 直接放 libs/x86_64/。

---

## 相关文档

- [BUILD_ENV.md](./BUILD_ENV.md) — 从零搭建构建环境（Docker / WSL2 / 裸机）
- [ARCHITECTURE.md](./ARCHITECTURE.md) — 项目架构概览
- [CURRENT_STATUS.md](./CURRENT_STATUS.md) — 当前功能状态
- [.claude/rules/build-and-log.md](../.claude/rules/build-and-log.md) — 调试日志速查
