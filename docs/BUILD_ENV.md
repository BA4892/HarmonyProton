# 构建环境搭建

> 在裸容器（Docker / WSL2 / fresh install）中从零搭建 Wine for HarmonyOS 的构建环境。

## 前提

- **OS:** Ubuntu 26.04 (或其他 Linux, 包名对应调整)
- **OHOS SDK:** `/apps/harmony/` (通过 volume 挂载或本地安装)
- **项目源码:** `/data/share/wineohos/` (带完整 thirdparty submodules)

## 虚拟化场景

### Docker 容器

```bash
docker run -d --name wine \
  -v /path/to/wineohos:/data/share/wineohos \
  -v /path/to/harmony-sdk:/apps/harmony \
  ubuntu:26.04 bash -c 'sleep infinity'
```

### WSL2

直接在工作目录操作，OHOS SDK 路径参考 `scripts/env.sh` 中的默认值 `/apps/harmony/sdk/default/openharmony`。

---

## 依赖安装

### 1. 系统包（apt）

```bash
apt-get update && apt-get install -y \
  build-essential cmake ninja-build meson         `# 编译工具链` \
  bison flex autoconf automake libtool             `# autotools (libffi, Wine configure)` \
  pkgconf zip git file python3 python3-pip         `# 工具` \
  libexpat1-dev libxml2-dev                        `# wayland-scanner 原生构建` \
  libfreetype-dev                                  `# sfnt2fon 字体工具 (Wine 字体 .fon 生成)` \
  gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64                             `# Wine OHOS 交叉 PE 编译` \
  default-jdk                                      `# HAP 签名 (java)`
```

> Wine native 构建只编译 `tools/` 下的 host 工具，不编译任何 DLL。
> 9 个工具中仅 `sfnt2fon` 需要 host freetype（字体转换），其余工具只需 libc。
> wayland/xkbcommon/GL host 头文件完全不需要。

### 2. Python 包（pip）

```bash
pip3 install --break-system-packages pyyaml mako markupsafe
```

| 包 | 用途 |
|---|------|
| `pyyaml` | virglrenderer 构建 (Python YAML 解析) |
| `mako` | Mesa guest_gfx 构建 (代码生成模板) |
| `markupsafe` | mako 依赖 |

### 3. libxml2.so.2 兼容性修复

OHOS SDK 自带的 `ld.lld` 链接器依赖 `libxml2.so.2`，而 Ubuntu 26.04 提供的是 `libxml2.so.16`。创建符号链接：

```bash
ln -sf /usr/lib/x86_64-linux-gnu/libxml2.so.16 /usr/lib/x86_64-linux-gnu/libxml2.so.2
ldconfig
```

验证：

```bash
/apps/harmony/sdk/default/hms/native/BiSheng/bin/ld.lld --version
# LLD 15.0.4 (compatible with GNU linkers)
```

> "no version information available" 警告可忽略，不影响链接。

---

## 验证环境

```bash
# 进入构建环境
docker exec -it wine bash

# 验证关键工具
for tool in gcc g++ make cmake ninja meson bison flex autoconf libtoolize \
            pkg-config git python3 java x86_64-w64-mingw32-gcc; do
  which $tool >/dev/null 2>&1 && echo "✓ $tool" || echo "✗ $tool MISSING"
done

# 验证 Python 模块
for mod in yaml mako markupsafe; do
  python3 -c "import $mod" 2>/dev/null && echo "✓ $mod" || echo "✗ $mod MISSING"
done

# 验证 OHOS SDK
test -f /apps/harmony/sdk/default/openharmony/native/llvm/bin/clang && echo "✓ clang" || echo "✗ clang MISSING"
test -f /apps/harmony/bin/hvigorw && echo "✓ hvigorw" || echo "✗ hvigorw MISSING"

# 验证 ld.lld 可用
/apps/harmony/sdk/default/hms/native/BiSheng/bin/ld.lld --version >/dev/null 2>&1 \
  && echo "✓ ld.lld" || echo "✗ ld.lld MISSING (check libxml2.so.2 symlink)"

# 验证 wayland-scanner
test -f /usr/local/bin/wayland-scanner && echo "✓ wayland-scanner" || echo "~ wayland-scanner (will be built by build_deps.sh)"
```

---

## 脚本修复

在容器环境中，`scripts/build_xkbconfig.sh` 可能因绝对路径 symlink 解引用失败而报错。根本原因是 meson 创建的 `X11/xkb → /usr/share/xkeyboard-config-2` 绝对路径 symlink 在裸容器中不存在目标，`cp -rL` 无法解引用。

**修复:** 直接复制实际数据目录，不依赖 symlink 解引用。

```diff
- cp -rL "$XKBC_INSTALL/usr/share/X11" "$SYSROOT_EXT_SHARE/"
+ cp -r "$XKBC_INSTALL/usr/share/xkeyboard-config-2" "$SYSROOT_EXT_SHARE/X11/xkb"
```

（如果 `build_xkbconfig.sh` 尚未包含此修复，需手动加）

---

## 构建

```bash
cd /data/share/wineohos

# Makefile 方式（推荐）
make NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad

# 或 build.sh 方式
bash build.sh pad arm64
```

### 构建阶段

| 阶段 | 输入 | 产物 |
|------|------|------|
| `deps` | thirdparty/{freetype,libffi,wayland,wayland-protocols,xkbcommon,xkeyboard-config,mesa,libdrm} | `build/sysroot-ext/` (交叉编译, x86_64) |
| `wine` | thirdparty/wine | `build/wine-native/` (winegcc 等 host 工具), `build/wine-ohos/` (OHOS Unix .so + PE DLL) |
| `box64` | thirdparty/box64 | `entry/libs/arm64-v8a/box64.so` (仅 arm64) |
| `native` | thirdparty/{libffi,wayland,libepoxy,virglrenderer} | `entry/libs/arm64-v8a/` (ARM64 原生 compositor 依赖) |
| `assemble` | 以上所有产物 | 组装 Pad/PC 布局 |
| `hap` | assemble 产物 + ArkTS 源码 | `entry/build/default/outputs/default/entry-default-signed.hap` |

### 增量构建

Makefile 使用 stamp 文件跟踪每个阶段的完成状态：

```bash
# 只改了 ArkTS → 直接打包
make hap NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad

# 改了 native compositor C++ 代码
make native NATIVE_ARCH=arm64-v8a  # → make hap

# 改了 Wine C 源码
make wine NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad  # → make hap

# 完全清理
make clean
```

---

## 补充说明

- `scripts/env.sh` 自动检测 `DEVICE_TYPE` 并设置 `PAD_CFLAGS=-DPAD_MODE`、交叉编译工具链等
- Wine native 构建只编译 host 工具（winegcc 等），不编译 DLL，通过 autoconf cache variables 绕过所有库检测
- `gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64` 是 OHOS 交叉构建需要的 PE 编译器后端，不是 native 构建用的
- Ohos SDK 中的 BiSheng 工具链（`ld.lld`）是闭源预编译的，依赖旧版系统库
