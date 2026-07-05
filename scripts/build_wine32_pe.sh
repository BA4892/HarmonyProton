#!/bin/bash
# build_wine32_pe.sh — Wine 32-bit PE DLL 交叉编译 (i686-mingw32)
#
# 32-bit PE DLL 用于 WoW64: 64-bit Wine 进程加载 32-bit Windows 程序时，
# 需要 32-bit PE DLL (不含 Unix .so, 后者保持 64-bit)。
#
# 不依赖 OHOS 工具链，仅需 mingw-w64 i686 交叉编译器。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建 Wine 32-bit PE DLL (i686-mingw32) ==="

CROSS_CC="i686-w64-mingw32-gcc"
CROSS_CXX="i686-w64-mingw32-g++"

BUILD_DIR="$ROOT/build/wine-i386-pe"
WINE_TOOLS="$ROOT/build/wine-native"
WINE32_SENTINEL="$BUILD_DIR/dlls/ntdll/i386-windows/ntdll.dll"

# 确保 host tools 存在
if [ ! -f "$WINE_TOOLS/tools/winegcc/winegcc" ]; then
    warn "wine-native tools not found at $WINE_TOOLS, building first..."
    NATIVE_ARCH=x86_64 DEVICE_TYPE=pc bash "$SCRIPT_DIR/build_wine.sh"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -f "Makefile" ]; then
    log "Configuring Wine 32-bit PE..."
    "$WINE_SRC/configure" \
        --host=i686-w64-mingw32 \
        --with-wine-tools="$WINE_TOOLS" \
        --with-mingw=gcc \
        --disable-tests \
        --without-x --without-alsa --without-wayland \
        --without-freetype \
        --without-opengl --without-vulkan \
        CC="$CROSS_CC" \
        CROSSCC="$CROSS_CC" \
        CXX="$CROSS_CXX"
fi

log "Building 32-bit PE DLLs..."
make -j"$JOBS" \
    CC="$CROSS_CC" \
    CROSSCC="$CROSS_CC"

# 统计产出
PE_COUNT=$(find "$BUILD_DIR/dlls/"*/i386-windows -name '*.dll' 2>/dev/null | wc -l)
DRV_COUNT=$(find "$BUILD_DIR/dlls/"*/i386-windows -name '*.drv' 2>/dev/null | wc -l)
EXE_COUNT=$(find "$BUILD_DIR/programs/"*/i386-windows -name '*.exe' 2>/dev/null | wc -l)
log "32-bit PE 构建完成: dll=${PE_COUNT}, drv=${DRV_COUNT}, exe=${EXE_COUNT}"
