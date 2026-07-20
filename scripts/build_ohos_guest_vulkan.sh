#!/bin/bash
# Build the B1 x86_64 guest Vulkan stack for an ARM64 HarmonyOS device:
# Vulkan Loader -> Mesa Venus ICD -> vtest -> host virglrenderer/Vulkan.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

GUEST_ARCH="${NATIVE_ARCH:-x86_64}"
[ "$GUEST_ARCH" = "x86_64" ] || err "guest Vulkan must be built as x86_64, got $GUEST_ARCH"

LOADER_TAG="v1.3.290"
LOADER_COMMIT="f8616928ee19f6c7fd648c1cf1f456cba3771855"
HEADERS_TAG="v1.3.290"
HEADERS_COMMIT="b379292b2ab6df5771ba9870d53cf8b2c9295daf"
GIT_PROXY="${WINEHUA_GIT_PROXY:-http://172.28.112.1:8080}"

LOADER_SOURCE="$ROOT/tmp/Vulkan-Loader-$LOADER_TAG"
HEADERS_SOURCE="$ROOT/tmp/Vulkan-Headers-$HEADERS_TAG"
BUILD_ROOT="$ROOT/build/guest_vulkan_build/$GUEST_ARCH"
HEADERS_INSTALL="$BUILD_ROOT/headers-install"
LOADER_INSTALL="$BUILD_ROOT/loader-install"
OUTPUT_ROOT="$ROOT/build/guest_vulkan/$GUEST_ARCH"
MESA_INSTALL="$BUILD_ROOT/mesa-venus-install"
LOADER_PATCH="$ROOT/patches/vulkan-loader-v1.3.290-ohos.patch"

fetch_pinned_source() {
    local url="$1" tag="$2" commit="$3" destination="$4"
    if [ ! -d "$destination/.git" ]; then
        [ ! -e "$destination" ] || err "incomplete managed source exists: $destination"
        git -c http.proxy="$GIT_PROXY" -c http.version=HTTP/1.1 \
            clone --depth 1 --branch "$tag" "$url" "$destination"
    fi
    local actual
    actual="$(git -C "$destination" rev-parse HEAD)"
    [ "$actual" = "$commit" ] || \
        err "pinned source mismatch for $destination: expected $commit got $actual"
}

fetch_pinned_source \
    https://github.com/KhronosGroup/Vulkan-Headers.git \
    "$HEADERS_TAG" "$HEADERS_COMMIT" "$HEADERS_SOURCE"
fetch_pinned_source \
    https://github.com/KhronosGroup/Vulkan-Loader.git \
    "$LOADER_TAG" "$LOADER_COMMIT" "$LOADER_SOURCE"

[ -f "$LOADER_PATCH" ] || err "Vulkan Loader OHOS patch missing: $LOADER_PATCH"
if ! grep -q 'Linux|BSD|DragonFly|GNU|OHOS' "$LOADER_SOURCE/CMakeLists.txt"; then
    git -C "$LOADER_SOURCE" apply --check "$LOADER_PATCH"
    git -C "$LOADER_SOURCE" apply "$LOADER_PATCH"
fi

mkdir -p "$BUILD_ROOT"

log "--- Mesa Venus ICD (x86_64-linux-ohos, offscreen) ---"
WINEHUA_GUEST_VULKAN_ONLY=1 \
WINEHUA_GUEST_GFX_PLATFORM=wayland \
WINEHUA_GUEST_GFX_BUILD_ROOT="$BUILD_ROOT/mesa-venus-offscreen-v2" \
WINEHUA_GUEST_GFX_INSTALL_ROOT="$MESA_INSTALL" \
NATIVE_ARCH=x86_64 \
    bash "$SCRIPT_DIR/build_ohos_guest_gfx.sh" --platform wayland --no-package
[ -f "$MESA_INSTALL/lib/libvulkan_virtio.so" ] || \
    err "Mesa Venus ICD build did not produce libvulkan_virtio.so"

log "--- Vulkan-Headers $HEADERS_TAG ---"
cmake -S "$HEADERS_SOURCE" -B "$BUILD_ROOT/headers" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HEADERS_INSTALL" \
    -DVULKAN_HEADERS_ENABLE_TESTS=OFF
cmake --build "$BUILD_ROOT/headers" --target install --parallel "$JOBS"

log "--- Vulkan-Loader $LOADER_TAG (x86_64-linux-ohos) ---"
cmake -S "$LOADER_SOURCE" -B "$BUILD_ROOT/loader" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
    -DOHOS_ARCH=x86_64 \
    -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$LOADER_INSTALL" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$HEADERS_INSTALL" \
    -DBUILD_TESTS=OFF \
    -DBUILD_WERROR=OFF \
    -DBUILD_WSI_XCB_SUPPORT=OFF \
    -DBUILD_WSI_XLIB_SUPPORT=OFF \
    -DBUILD_WSI_WAYLAND_SUPPORT=OFF \
    -DBUILD_WSI_DIRECTFB_SUPPORT=OFF
cmake --build "$BUILD_ROOT/loader" --parallel "$JOBS"
cmake --install "$BUILD_ROOT/loader"

loader_binary="$(find "$LOADER_INSTALL/lib" -maxdepth 1 -type f -name 'libvulkan.so.1*' | sort | tail -n 1)"
[ -n "$loader_binary" ] || err "Vulkan Loader install did not produce libvulkan.so.1"

rm -rf "$OUTPUT_ROOT"
mkdir -p "$OUTPUT_ROOT/bin" "$OUTPUT_ROOT/lib" "$OUTPUT_ROOT/share/vulkan/icd.d"

# Rawfile extraction and HAP packaging must not depend on symlink preservation.
cp -L "$loader_binary" "$OUTPUT_ROOT/lib/libvulkan.so.1"
cp -L "$loader_binary" "$OUTPUT_ROOT/lib/libvulkan.so"
cp -L "$MESA_INSTALL/lib/libvulkan_virtio.so" "$OUTPUT_ROOT/lib/libvulkan_virtio.so"

cat > "$OUTPUT_ROOT/share/vulkan/icd.d/venus_icd.x86_64.json" <<'EOF'
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "../../../lib/libvulkan_virtio.so",
    "api_version": "1.3.0"
  }
}
EOF

log "--- winehua_guest_vulkan_smoke (x86_64-linux-ohos) ---"
SHADER_OUTPUT="$OUTPUT_ROOT/share/winehua"
GLSLANG_VALIDATOR="${GLSLANG_VALIDATOR:-glslangValidator}"
mkdir -p "$SHADER_OUTPUT"
for shader in \
    venus_storage_write \
    venus_storage_read \
    venus_image_fetch \
    venus_combined_sample \
    venus_separated_sample \
    venus_dxvk_contract_sample \
    venus_dxvk_contract_unknown_sample \
    venus_dxvk_contract_spec_sample \
    venus_dxvk_contract_vector_spec_sample; do
    "$GLSLANG_VALIDATOR" -V --target-env vulkan1.1 \
        "$ROOT/smoke/$shader.comp" -o "$SHADER_OUTPUT/$shader.spv" \
        >/dev/null
done
# Optional diagnostic payload: freeze the currently captured DXVK CS shaders
# so the replay can separate specialization/default handling from generated
# instruction semantics.  This never changes the product DXVK binaries.
FROZEN_OUTPUT="$OUTPUT_ROOT/share/winehua/replay_frozen"
mkdir -p "$FROZEN_OUTPUT"
for replay_shader in "$ROOT"/replay_spv/CS_*.remapped.spv; do
    [ -f "$replay_shader" ] || continue
    replay_name="$(basename "$replay_shader")"
    spirv-opt --freeze-spec-const "$replay_shader" \
        -o "$FROZEN_OUTPUT/$replay_name" >/dev/null 2>&1 || rm -f "$FROZEN_OUTPUT/$replay_name"
done
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
    -std=c11 -O2 -fPIE -fno-emulated-tls \
    -I"$HEADERS_INSTALL/include" \
    "$ROOT/smoke/guest_vulkan_smoke.c" \
    -L"$LOADER_INSTALL/lib" -Wl,-rpath,'$ORIGIN/../lib' \
    -Wl,--enable-new-dtags -pie -lvulkan -ldl -lpthread \
    -o "$OUTPUT_ROOT/bin/winehua_guest_vulkan_smoke"
chmod +x "$OUTPUT_ROOT/bin/winehua_guest_vulkan_smoke"
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
    -std=c11 -O2 -fPIE -fno-emulated-tls \
    -I"$HEADERS_INSTALL/include" \
    "$ROOT/smoke/venus_sampled_image_probe.c" \
    -L"$LOADER_INSTALL/lib" -Wl,-rpath,'$ORIGIN/../lib' \
    -Wl,--enable-new-dtags -pie -lvulkan -ldl -lpthread \
    -o "$OUTPUT_ROOT/bin/venus_sampled_image_probe"
chmod +x "$OUTPUT_ROOT/bin/venus_sampled_image_probe"
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
    -std=c11 -O2 -fPIE -fno-emulated-tls \
    -I"$HEADERS_INSTALL/include" \
    "$ROOT/smoke/venus_spirv_replay.c" \
    -L"$LOADER_INSTALL/lib" -Wl,-rpath,'$ORIGIN/../lib' \
    -Wl,--enable-new-dtags -pie -lvulkan -ldl -lpthread \
    -o "$OUTPUT_ROOT/bin/venus_spirv_replay"
chmod +x "$OUTPUT_ROOT/bin/venus_spirv_replay"

loader_sha="$(sha256sum "$OUTPUT_ROOT/lib/libvulkan.so.1" | awk '{print $1}')"
icd_sha="$(sha256sum "$OUTPUT_ROOT/lib/libvulkan_virtio.so" | awk '{print $1}')"
smoke_sha="$(sha256sum "$OUTPUT_ROOT/bin/winehua_guest_vulkan_smoke" | awk '{print $1}')"
probe_sha="$(sha256sum "$OUTPUT_ROOT/bin/venus_sampled_image_probe" | awk '{print $1}')"
replay_sha="$(sha256sum "$OUTPUT_ROOT/bin/venus_spirv_replay" | awk '{print $1}')"
cat > "$OUTPUT_ROOT/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "runtimeVersion": "phase2-venus-b1-v1",
  "architecture": "x86_64-linux-ohos",
  "loaderVersion": "$LOADER_TAG",
  "loaderCommit": "$LOADER_COMMIT",
  "headersVersion": "$HEADERS_TAG",
  "headersCommit": "$HEADERS_COMMIT",
  "files": {
    "bin/winehua_guest_vulkan_smoke": "$smoke_sha",
    "bin/venus_sampled_image_probe": "$probe_sha",
    "bin/venus_spirv_replay": "$replay_sha",
    "lib/libvulkan.so.1": "$loader_sha",
    "lib/libvulkan_virtio.so": "$icd_sha"
  }
}
EOF

cat > "$OUTPUT_ROOT/BUILD_INFO.txt" <<EOF
arch=x86_64-linux-ohos
loader_tag=$LOADER_TAG
loader_commit=$LOADER_COMMIT
headers_tag=$HEADERS_TAG
headers_commit=$HEADERS_COMMIT
mesa_commit=$(git -c safe.directory="$ROOT/thirdparty/mesa" -C "$ROOT/thirdparty/mesa" rev-parse HEAD)
built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

file "$OUTPUT_ROOT/bin/winehua_guest_vulkan_smoke" "$OUTPUT_ROOT/lib/libvulkan.so.1" \
    "$OUTPUT_ROOT/lib/libvulkan_virtio.so"
log "guest Vulkan runtime ready: $OUTPUT_ROOT"
