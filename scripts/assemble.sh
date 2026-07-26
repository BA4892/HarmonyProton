#!/bin/bash
# assemble.sh — 组装 HAP 打包布局
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
# 文件分流到 libs/ + rawfile/
# ============================================================
assemble_pad() {
    log "=== 组装布局 ($NATIVE_ARCH) ==="

    local wine_data="$STAGING_DIR/wine-data"
    local guest_arch="${GUEST_ARCH:-x86_64}"
    rm -rf "$STAGING_DIR"
    rm -rf "$wine_data"
    mkdir -p "$wine_data/bin/x86_64-windows"
    mkdir -p "$wine_data/bin/x86_64-unix"
    mkdir -p "$wine_data/share/wine/nls"
    mkdir -p "$wine_data/share/wine/fonts"
    mkdir -p "$wine_data/share/wine/winmd"
    mkdir -p "$wine_data/share/wine/mono"
    mkdir -p "$wine_data/share/X11"

    # SoundFont (MIDI 音色库)
    local soundfont="$WINEHUA/entry/src/main/resources/rawfile/winehua-gm.sf2"
    if [ -f "$soundfont" ]; then
        mkdir -p "$wine_data/audio"
        cp "$soundfont" "$wine_data/audio/winehua-gm.sf2"
        log "    winehua-gm.sf2 → rawfile audio/"
    else
        warn "winehua-gm.sf2 not found; MIDI output will be unavailable"
    fi

    # -- 1. 原生 .so → libs/$NATIVE_ARCH/ (由各 build 脚本完成) --
    mkdir -p "$NATIVE_LIBS"

    if [ "$NATIVE_ARCH" = "x86_64" ]; then
        # x86_64 Pad: Wine .so 是原生架构, 直接放 libs/
        log "  → Wine .so → libs/x86_64/"

        # 所有 Wine Unix .so → libs/x86_64/ (系统 linker 通过文件名搜索)
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done
        log "    Wine .so: $(ls "$BUILD_DIR/wine-ohos/dlls/"*/*.so 2>/dev/null | wc -l) files"

        # 交叉编译依赖 → libs/x86_64/
        # (系统 linker 自动搜索此路径, 无需 x86_64-unix 子目录)
        _pick_lib_pad() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$NATIVE_LIBS"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
                cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad "libz.so"                      "libz.so"
        _pick_lib_pad "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"
        _pick_lib_pad "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad "libffi.so.8.1.4"              "libffi.so.8"
        log "    交叉编译依赖 → libs/x86_64/"

        # libc.so → libs/x86_64/
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$NATIVE_LIBS/"

        # libfreetype 已由 _pick_lib_pad 放入 libs/x86_64/，系统 linker 可直接找到

        # libwineserver.so (Pad fork+dlopen 入口)
        if [ -f "$BUILD_DIR/wine_server/libwineserver.so" ]; then
            cp "$BUILD_DIR/wine_server/libwineserver.so" "$NATIVE_LIBS/"
            log "    libwineserver.so → libs/x86_64/"
        else
            warn "libwineserver.so 未找到！请先执行: bash scripts/build_wine.sh"
        fi
    elif [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        # arm64 Pad: Wine .so 是 x86_64, 不放 libs/, 放 rawfile zip
        # box64.so 由 build_box64.sh 放入 NATIVE_LIBS
        log "  → Wine x86_64 .so → rawfile zip"

        # ARM64 原生库 → libs/arm64-v8a/ (Box64 dlopen bridge libraries)
        # Box64 模拟 x86_64 时需要加载 ARM64 原生的 freetype/xkbcommon 等,
        # 系统 linker 搜索 libs/arm64-v8a/
        local aarch64_lib="$SYSROOT_EXT/usr/lib/$NATIVE_TARGET"
        _pick_arm64_native() {
            local soname="$1" linker="${2:-}"
            if [ -f "$aarch64_lib/$soname" ]; then
                cp "$aarch64_lib/$soname" "$NATIVE_LIBS/$soname"
            else
                warn "ARM64 原生库 $soname 未找到, 跳过"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$NATIVE_LIBS/$linker" ]; then
                cp "$aarch64_lib/$soname" "$NATIVE_LIBS/$linker"  # HAP 不支持 symlink, 实体复制
            fi
        }
        # Box64 native bridge libs: soname 文件 + linker 名拷贝
        _pick_arm64_native "libfreetype.so.6"   "libfreetype.so"
        _pick_arm64_native "libxkbcommon.so.0"   "libxkbcommon.so"
        _pick_arm64_native "libxkbregistry.so.0" "libxkbregistry.so"
        _pick_arm64_native "libxml2.so.2"        "libxml2.so"
        _pick_arm64_native "libwayland-client.so.0" "libwayland-client.so"
        _pick_arm64_native "libwayland-server.so.0" "libwayland-server.so"
        _pick_arm64_native "libffi.so.8"         "libffi.so"

        # box64.so → libs/arm64-v8a/ (ARM64 原生翻译器)
        if [ -f "$BUILD_DIR/box64_build/box64.so" ]; then
            cp "$BUILD_DIR/box64_build/box64.so" "$NATIVE_LIBS/"
            log "    box64.so → libs/arm64-v8a/"
        else
            warn "box64.so 未找到！请先执行: bash scripts/build_box64.sh"
        fi

        # ntdll.so → rawfile
        cp "$BUILD_DIR/wine-ohos/dlls/ntdll/ntdll.so" "$wine_data/bin/"

        # x86_64-unix/ .so → rawfile
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            [ "$(basename "$so")" = "ntdll.so" ] && continue
            cp "$so" "$wine_data/bin/x86_64-unix/"
        done

        # 交叉编译依赖 → rawfile
        _pick_lib_pad_rf() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$wine_data/bin/x86_64-unix"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
                cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad_rf "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad_rf "libz.so"                      "libz.so"
        _pick_lib_pad_rf "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad_rf "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"    "libwayland-egl.so"
        _pick_lib_pad_rf "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad_rf "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad_rf "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad_rf "libffi.so.8.1.4"              "libffi.so.8"

        # libfreetype → bin/ (box64 按名 dlopen 搜索路径: .)
        cp "$wine_data/bin/x86_64-unix/libfreetype.so.6" "$wine_data/bin/"
        cp "$wine_data/bin/x86_64-unix/libfreetype.so" "$wine_data/bin/"

        # libc.so → bin/ (当前目录) + x86_64-unix/ (BOX64_LD_LIBRARY_PATH)
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_data/bin/"
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_data/bin/x86_64-unix/"

        # wine + wineserver (x86_64 ELF, 由 box64 加载)
        cp "$BUILD_DIR/wine-ohos/loader/wine" "$wine_data/bin/"
        if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
            cp "$BUILD_DIR/wine_server/wineserver" "$wine_data/bin/"
        elif [ -f "$BUILD_DIR/wine-ohos/server/wineserver" ]; then
            cp "$BUILD_DIR/wine-ohos/server/wineserver" "$wine_data/bin/"
        fi
    fi

    # -- 2. PE DLL + 数据文件 → rawfile (两种架构共用) --
    # x86_64-windows/ — 复制所有运行时 PE 文件
    # 注意: .cpl 不打包, wineboot 初始化时 mscoree.dll 触发 appwiz.cpl
    # → install_mono → DialogBoxW 模态框在 OHOS 无头环境永久阻塞
    for ext in dll drv exe sys acm ax ocx tlb; do
        for f in "$BUILD_DIR/wine-ohos/dlls/"*/x86_64-windows/*.$ext; do
            [ -f "$f" ] && cp "$f" "$wine_data/bin/x86_64-windows/"
        done
    done
    log "  x86_64-windows → $(ls "$wine_data/bin/x86_64-windows" | wc -l) files"

    # strip PE 调试符号 (DWARF .debug_*, 缩减 ~50%)
    log "  stripping debug symbols..."
    if command -v x86_64-w64-mingw32-strip &>/dev/null; then
        for f in "$wine_data/bin/x86_64-windows/"*.dll "$wine_data/bin/x86_64-windows/"*.drv "$wine_data/bin/x86_64-windows/"*.exe "$wine_data/bin/x86_64-windows/"*.sys; do
            [ -f "$f" ] && x86_64-w64-mingw32-strip "$f" 2>/dev/null
        done
        log "  64-bit PE stripped"
    else
        warn "  x86_64-w64-mingw32-strip not found, skipping strip"
    fi
    if command -v i686-w64-mingw32-strip &>/dev/null; then
        for f in "$wine_data/bin/i386-windows/"*.dll "$wine_data/bin/i386-windows/"*.drv "$wine_data/bin/i386-windows/"*.exe "$wine_data/bin/i386-windows/"*.sys; do
            [ -f "$f" ] && i686-w64-mingw32-strip "$f" 2>/dev/null
        done
        log "  32-bit PE stripped"
    else
        warn "  i686-w64-mingw32-strip not found, skipping strip"
    fi

    # i386-windows/ (32-bit PE DLL for WoW64)
    # 只取核心 DLL (~20 个), 其余 600+ 个 (d3dx9/msi/media等) 暂不需要.
    # 完整列表在 build/wine-i386-pe/dlls/*/i386-windows/*.dll.
    # 日后需要某个缺失的 DLL 时, 在此处加名即可.
    if [ -d "$BUILD_DIR/wine-i386-pe" ]; then
        # 32-bit PE DLL for WoW64: 复制所有运行时 PE 文件
        mkdir -p "$wine_data/bin/i386-windows"
        for ext in dll drv exe sys acm ax ocx tlb; do
            for f in "$BUILD_DIR/wine-i386-pe/dlls/"*/i386-windows/*.$ext; do
                [ -f "$f" ] && cp "$f" "$wine_data/bin/i386-windows/"
            done
        done
        log "  i386-windows → $(ls "$wine_data/bin/i386-windows" | wc -l) files (ALL)"

        # 32-bit exe stubs, 放在 bin/i386-windows/.
        # Wine 通过 WINEARCH 或 exe header 判断 32/64, 自动加载对应 DLL.
        for exe in "$BUILD_DIR/wine-i386-pe/programs/"*/i386-windows/*.exe; do
            [ -f "$exe" ] && cp "$exe" "$wine_data/bin/i386-windows/"
        done
        log "  i386 exe stubs → $(ls "$wine_data/bin/i386-windows"/*.exe 2>/dev/null | wc -l) files"
    else
        warn "  i386-windows: SKIP (build/wine-i386-pe not found)"
    fi

    # *.exe stubs → rawfile
    for exe in "$BUILD_DIR/wine-ohos/programs/"*/x86_64-windows/*.exe; do
        cp "$exe" "$wine_data/bin/"
    done
    # graphics smoke test (OHOS 交叉编译产物, 不在 build-native/)
    if [ -f "$BUILD_DIR/wine-ohos/programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe" ]; then
        cp "$BUILD_DIR/wine-ohos/programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe" "$wine_data/bin/x86_64-windows/"
        log "  winehua_graphics_smoke.exe → x86_64-windows/"
    fi

    # Versioned, App-managed C:\smoke payload.  Keep it separate from Wine's
    # DLL search directories so a prefix refresh can update tests without
    # touching user files or relying on Explorer.
    local smoke_dir="$wine_data/smoke"
    mkdir -p "$smoke_dir/x64" "$smoke_dir/x86" "$smoke_dir/assets"
    local cube_source="$WINEHUA/smoke/winehua_d3d_switch_cube.c"
    x86_64-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x64/winehua_d3d_switch_cube.exe" "$cube_source" \
        -ld3d9 -ld3d11 -ldxgi -ld3dcompiler -luuid -lshell32 -luser32 -lgdi32 -lm
    i686-w64-mingw32-gcc -O2 -s -mwindows -o \
        "$smoke_dir/x86/winehua_d3d_switch_cube.exe" "$cube_source" \
        -ld3d9 -ld3d11 -ldxgi -ld3dcompiler -luuid -lshell32 -luser32 -lgdi32 -lm
    local win32_driver_source="$WINEHUA/smoke/winehua_win32_driver.c"
    x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows -o \
        "$smoke_dir/x64/winehua_win32_driver.exe" "$win32_driver_source" \
        -lshell32 -luser32
    i686-w64-mingw32-gcc -O2 -s -municode -mwindows -o \
        "$smoke_dir/x86/winehua_win32_driver.exe" "$win32_driver_source" \
        -lshell32 -luser32
    local guest_shader_root="$BUILD_DIR/guest_vulkan/$guest_arch/share/winehua"
    local smoke_shader
    for smoke_shader in venus_storage_write venus_storage_read venus_image_fetch venus_combined_sample venus_separated_sample; do
        [ -f "$guest_shader_root/$smoke_shader.spv" ] || err "Wine Vulkan sampled-image shader missing: $guest_shader_root/$smoke_shader.spv"
        cp "$guest_shader_root/$smoke_shader.spv" "$smoke_dir/assets/$smoke_shader.spv"
    done
    local dxvk_root="$DXVK_BUILD_ROOT"
    [ -f "$dxvk_root/x64/bin/d3d11.dll" ] || err "DXVK Legacy x64 d3d11.dll missing: $dxvk_root/x64/bin/d3d11.dll"
    [ -f "$dxvk_root/x64/bin/dxgi.dll" ] || err "DXVK Legacy x64 dxgi.dll missing: $dxvk_root/x64/bin/dxgi.dll"
    [ -f "$dxvk_root/x86/bin/d3d11.dll" ] || err "DXVK Legacy x86 d3d11.dll missing: $dxvk_root/x86/bin/d3d11.dll"
    [ -f "$dxvk_root/x86/bin/dxgi.dll" ] || err "DXVK Legacy x86 dxgi.dll missing: $dxvk_root/x86/bin/dxgi.dll"
    mkdir -p "$wine_data/dxvk/legacy/x64" "$wine_data/dxvk/legacy/x86"
    cp "$dxvk_root/x64/bin/d3d11.dll" "$wine_data/dxvk/legacy/x64/d3d11.dll"
    cp "$dxvk_root/x64/bin/dxgi.dll" "$wine_data/dxvk/legacy/x64/dxgi.dll"
    cp "$dxvk_root/x86/bin/d3d11.dll" "$wine_data/dxvk/legacy/x86/d3d11.dll"
    cp "$dxvk_root/x86/bin/dxgi.dll" "$wine_data/dxvk/legacy/x86/dxgi.dll"
    # The DXVK binaries are runtime-owned overlays.  Do not place them next
    # to the smoke executables: that would make the test layout look like a
    # game distribution and would force real games to carry WineHua-specific
    # DLLs.  SpawnWineProgram exposes this versioned directory through
    # WINEDLLPATH only for a selected dxvk_* backend.
    local smoke_program
    for smoke_program in winehua_audio_smoke winehua_graphics_smoke winehua_vulkan_smoke winehua_d3d11_smoke; do
        local smoke64="$BUILD_DIR/wine-ohos/programs/$smoke_program/x86_64-windows/$smoke_program.exe"
        local smoke32="$BUILD_DIR/wine-i386-pe/programs/$smoke_program/i386-windows/$smoke_program.exe"
        if [ ! -f "$smoke32" ]; then
            smoke32="$BUILD_DIR/wine-ohos/programs/$smoke_program/i386-windows/$smoke_program.exe"
        fi
        [ -f "$smoke64" ] || err "managed smoke x64 artifact missing: $smoke64"
        [ -f "$smoke32" ] || err "managed smoke x86 artifact missing: $smoke32"
        cp "$smoke64" "$smoke_dir/x64/$smoke_program.exe"
        cp "$smoke32" "$smoke_dir/x86/$smoke_program.exe"
    done
    local audio64_sha graphics64_sha vulkan64_sha d3d1164_sha cube64_sha driver64_sha
    local audio32_sha graphics32_sha vulkan32_sha d3d1132_sha cube32_sha driver32_sha
    local storage_write_sha storage_read_sha image_fetch_sha combined_sample_sha separated_sample_sha
    audio64_sha="$(sha256sum "$smoke_dir/x64/winehua_audio_smoke.exe" | awk '{print $1}')"
    graphics64_sha="$(sha256sum "$smoke_dir/x64/winehua_graphics_smoke.exe" | awk '{print $1}')"
    vulkan64_sha="$(sha256sum "$smoke_dir/x64/winehua_vulkan_smoke.exe" | awk '{print $1}')"
    d3d1164_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d11_smoke.exe" | awk '{print $1}')"
    cube64_sha="$(sha256sum "$smoke_dir/x64/winehua_d3d_switch_cube.exe" | awk '{print $1}')"
    driver64_sha="$(sha256sum "$smoke_dir/x64/winehua_win32_driver.exe" | awk '{print $1}')"
    audio32_sha="$(sha256sum "$smoke_dir/x86/winehua_audio_smoke.exe" | awk '{print $1}')"
    graphics32_sha="$(sha256sum "$smoke_dir/x86/winehua_graphics_smoke.exe" | awk '{print $1}')"
    vulkan32_sha="$(sha256sum "$smoke_dir/x86/winehua_vulkan_smoke.exe" | awk '{print $1}')"
    d3d1132_sha="$(sha256sum "$smoke_dir/x86/winehua_d3d11_smoke.exe" | awk '{print $1}')"
    cube32_sha="$(sha256sum "$smoke_dir/x86/winehua_d3d_switch_cube.exe" | awk '{print $1}')"
    driver32_sha="$(sha256sum "$smoke_dir/x86/winehua_win32_driver.exe" | awk '{print $1}')"
    storage_write_sha="$(sha256sum "$smoke_dir/assets/venus_storage_write.spv" | awk '{print $1}')"
    storage_read_sha="$(sha256sum "$smoke_dir/assets/venus_storage_read.spv" | awk '{print $1}')"
    image_fetch_sha="$(sha256sum "$smoke_dir/assets/venus_image_fetch.spv" | awk '{print $1}')"
    combined_sample_sha="$(sha256sum "$smoke_dir/assets/venus_combined_sample.spv" | awk '{print $1}')"
    separated_sample_sha="$(sha256sum "$smoke_dir/assets/venus_separated_sample.spv" | awk '{print $1}')"
    local dxvk_commit
    dxvk_commit="$(git -c safe.directory="$DXVK_SRC" -C "$DXVK_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
    local dxvk64_d3d11_sha dxvk64_dxgi_sha dxvk32_d3d11_sha dxvk32_dxgi_sha
    dxvk64_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/d3d11.dll" | awk '{print $1}')"
    dxvk64_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x64/dxgi.dll" | awk '{print $1}')"
    dxvk32_d3d11_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/d3d11.dll" | awk '{print $1}')"
    dxvk32_dxgi_sha="$(sha256sum "$wine_data/dxvk/legacy/x86/dxgi.dll" | awk '{print $1}')"
    cat > "$wine_data/dxvk/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "backend": "dxvk",
  "profile": "legacy",
  "runtimeRoot": "dxvk",
  "version": "1.10.3",
  "commit": "$dxvk_commit",
  "requiredCapabilities": {
    "vulkanApi": "1.1",
    "bcFormats": false,
    "descriptorIndexing": false
  },
  "runtimes": {
    "x64": {"d3d11.dll": "$dxvk64_d3d11_sha", "dxgi.dll": "$dxvk64_dxgi_sha"},
    "x86": {"d3d11.dll": "$dxvk32_d3d11_sha", "dxgi.dll": "$dxvk32_dxgi_sha"}
  }
}
EOF
    cat > "$smoke_dir/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "suiteVersion": "phase2-vulkan-dxvk-legacy-v4-runtime",
  "enabledSuites": ["core", "audio", "opengl", "wine-vulkan", "dxvk"],
  "managedRoot": "C:\\\\smoke",
  "files": {
    "x64/winehua_audio_smoke.exe": "$audio64_sha",
    "x64/winehua_graphics_smoke.exe": "$graphics64_sha",
    "x64/winehua_vulkan_smoke.exe": "$vulkan64_sha",
    "x64/winehua_d3d11_smoke.exe": "$d3d1164_sha",
    "x64/winehua_d3d_switch_cube.exe": "$cube64_sha",
    "x64/winehua_win32_driver.exe": "$driver64_sha",
    "x86/winehua_audio_smoke.exe": "$audio32_sha",
    "x86/winehua_graphics_smoke.exe": "$graphics32_sha",
    "x86/winehua_vulkan_smoke.exe": "$vulkan32_sha",
    "x86/winehua_d3d11_smoke.exe": "$d3d1132_sha",
    "x86/winehua_d3d_switch_cube.exe": "$cube32_sha",
    "x86/winehua_win32_driver.exe": "$driver32_sha",
    "assets/venus_storage_write.spv": "$storage_write_sha",
    "assets/venus_storage_read.spv": "$storage_read_sha",
    "assets/venus_image_fetch.spv": "$image_fetch_sha",
    "assets/venus_combined_sample.spv": "$combined_sample_sha",
    "assets/venus_separated_sample.spv": "$separated_sample_sha"
  }
}
EOF
    log "  managed smoke payload → smoke/{x64,x86}"

    # fonts
    cp "$WINE_SRC/fonts/"*.ttf "$wine_data/share/wine/fonts/"
    # NLS
    cp "$BUILD_DIR/wine-ohos/nls/"*.nls "$wine_data/share/wine/nls/"
    # winmd
    cp "$BUILD_DIR/wine-ohos/include/"*.winmd "$wine_data/share/wine/winmd/"
    # Wine Mono (.NET 运行时)
    if ls "$BUILD_DIR/wine-ohos/share/wine/mono/"*.msi >/dev/null 2>&1; then
        cp "$BUILD_DIR/wine-ohos/share/wine/mono/"*.msi "$wine_data/share/wine/mono/"
        log "    wine-mono.msi → rawfile share/wine/mono/"
    fi
    # wine.inf (含 OHOS font substitutes)
    cp "$BUILD_DIR/wine-ohos/loader/wine.inf" "$wine_data/share/wine/"
    sed_i '/^\[MCI\]$/i\
;; OHOS font substitutes\
HKLM,%FontSubStr%,"System",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Sans Serif",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Shell Dlg",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"MS Shell Dlg 2",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Arial",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Arial Black",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Calibri",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Cambria",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Candara",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Comic Sans MS",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Constantia",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Corbel",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Impact",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Palatino Linotype",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Segoe UI",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Tahoma",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Trebuchet MS",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Verdana",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Georgia",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Times New Roman",,"HarmonyOS Sans SC"\
;; CJK: 简体中文\
HKLM,%FontSubStr%,"Microsoft JhengHei",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Microsoft JhengHei UI",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Microsoft YaHei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"Microsoft YaHei UI",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"SimSun",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"NSimSun",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"SimHei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FangSong",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"KaiTi",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"YouYuan",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"LiSu",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"DengXian",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STSong",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STKaiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STFangsong",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STHeiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXihei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STLiti",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXingkai",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STXinwei",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STHupo",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STCaiyun",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STZhongSong",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"STBaoli",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FZShuTi",,"HarmonyOS Sans SC"\
HKLM,%FontSubStr%,"FZYaoti",,"HarmonyOS Sans SC"\
;; CJK: 繁体中文\
HKLM,%FontSubStr%,"MingLiU",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"PMingLiU",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"DFKai-SB",,"HarmonyOS Sans TC"\
HKLM,%FontSubStr%,"Consolas",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier New",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Fixedsys",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Lucida Console",,"Noto Sans Mono"' "$wine_data/share/wine/wine.inf"
    # XKB
    if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ]; then
        cp -r "$SYSROOT_EXT_SHARE/X11/xkb" "$wine_data/share/X11/"
    fi

    # guest GPU 库 (Mesa/VirGL, 供 GraphicsBroker 注入到 Wine LD_LIBRARY_PATH)
    if [ -d "$BUILD_DIR/guest_gfx/$guest_arch/lib" ]; then
        mkdir -p "$wine_data/bin/guest_gfx"
        cp -a "$BUILD_DIR/guest_gfx/$guest_arch/"* "$wine_data/bin/guest_gfx/"
        log "  guest_gfx ($guest_arch): $(ls "$wine_data/bin/guest_gfx/lib"/*.so* 2>/dev/null | wc -l) .so files"
    else
        if [ "${BUILD_GUEST_GFX:-0}" = "1" ]; then
            err "BUILD_GUEST_GFX=1 but build/guest_gfx/$guest_arch/lib is missing"
        fi
        log "  guest_gfx: SKIP (build/guest_gfx/$guest_arch/lib not found)"
    fi

    # Guest Linux Vulkan runtime is intentionally outside C:\\smoke: it is an
    # x86_64 OHOS ELF/Loader/ICD stack launched through Box64 for the B1 gate.
    if [ -f "$BUILD_DIR/guest_vulkan/$guest_arch/manifest.json" ]; then
        mkdir -p "$wine_data/bin/guest_vulkan"
        cp -a "$BUILD_DIR/guest_vulkan/$guest_arch/"* "$wine_data/bin/guest_vulkan/"
        log "  guest_vulkan ($guest_arch): Loader + Venus ICD + offscreen smoke"
    elif [ "${BUILD_GUEST_VULKAN:-0}" = "1" ]; then
        err "BUILD_GUEST_VULKAN=1 but build/guest_vulkan/$guest_arch/manifest.json is missing"
    else
        log "  guest_vulkan: SKIP"
    fi

    # Native offscreen replay runs in the App/NCP security domain and links the
    # system Host Vulkan loader. Captured resources remain in guest_vulkan so
    # there is one authoritative exact-replay input set for the Host/Venus A/B.
    local host_vulkan_root="$BUILD_DIR/host_vulkan/$NATIVE_ARCH"
    [ -f "$host_vulkan_root/manifest.json" ] || \
        err "Host Vulkan replay manifest missing: $host_vulkan_root/manifest.json"
    [ -f "$host_vulkan_root/bin/heaven_exact_host_replay" ] || \
        err "Host Vulkan replay marker missing: $host_vulkan_root/bin/heaven_exact_host_replay"
    [ -f "$host_vulkan_root/lib/libwinehua_host_heaven_replay.so" ] || \
        err "Host Vulkan replay module missing: $host_vulkan_root/lib/libwinehua_host_heaven_replay.so"
    mkdir -p "$wine_data/bin/host_vulkan"
    cp -a "$host_vulkan_root/"* "$wine_data/bin/host_vulkan/"
    log "  host_vulkan ($NATIVE_ARCH): native exact replay"

    # -- 3. 打包 zip → rawfile (不带 wine-data/ 前缀) --
    local rawfile_dir="$WINEHUA/entry/src/main/resources/rawfile"
    mkdir -p "$rawfile_dir"
    local zip_name="wine-data.zip"
    cd "$wine_data"
    rm -f "$STAGING_DIR/$zip_name"
    zip -r "$STAGING_DIR/$zip_name" . -x '*.git*'
    cp "$STAGING_DIR/$zip_name" "$rawfile_dir/"
    local payload_sha
    payload_sha="$(sha256sum "$rawfile_dir/$zip_name" | awk '{print $1}')"
    cat > "$rawfile_dir/wine-runtime-manifest.json" <<EOF
{
  "schemaVersion": 1,
  "payload": "wine-data.zip",
  "payloadSha256": "$payload_sha",
  "smokeSuiteVersion": "phase2-vulkan-b3-v1"
}
EOF
    log "  $zip_name → rawfile/ ($(du -h "$rawfile_dir/$zip_name" | cut -f1))"

    log "Pad 布局组装完成 ($NATIVE_ARCH)"
    echo ""
    echo "  libs/$NATIVE_ARCH/"
    ls -la "$NATIVE_LIBS/" 2>/dev/null || echo "    (empty)"
    echo "  rawfile/$zip_name"
}

log "=== 组装布局 ($NATIVE_ARCH) ==="

# 统一使用 rawfile zip 布局
assemble_pad
