#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/env.sh"

case "$NATIVE_ARCH" in
    arm64-v8a|x86_64) ;;
    *) err "host Vulkan replay requires one native architecture, got: $NATIVE_ARCH" ;;
esac

OUTPUT_ROOT="$BUILD_DIR/host_vulkan/$NATIVE_ARCH"
OUTPUT_MARKER="$OUTPUT_ROOT/bin/heaven_exact_host_replay"
OUTPUT_LIB="$OUTPUT_ROOT/lib/libwinehua_host_heaven_replay.so"
BUNDLE_LIB="$ROOT/entry/libs/$NATIVE_ARCH/libwinehua_host_heaven_replay.so"
SOURCE="$ROOT/smoke/venus_heaven_material_replay.c"

mkdir -p "$OUTPUT_ROOT/bin" "$OUTPUT_ROOT/lib" "$(dirname "$BUNDLE_LIB")"

log "Building native Host Vulkan exact replay ($NATIVE_ARCH)"
"$CLANG" --target="$NATIVE_TARGET" --sysroot="$SYSROOT" \
    -std=c11 -O2 -fPIC -fno-emulated-tls -DWINEHUA_HOST_DIRECT_REPLAY=1 \
    -I"$SYSROOT/usr/include" \
    "$SOURCE" \
    -fuse-ld=lld -shared -Wl,-soname,libwinehua_host_heaven_replay.so -lvulkan \
    -o "$OUTPUT_LIB"
cp "$OUTPUT_LIB" "$BUNDLE_LIB"
printf '%s\n' 'winehua-host-module-v1' > "$OUTPUT_MARKER"

case "$NATIVE_ARCH" in
    arm64-v8a) expected_machine='AArch64' ;;
    x86_64) expected_machine='Advanced Micro Devices X86-64' ;;
esac
actual_machine="$($OHOS_SDK/native/llvm/bin/llvm-readelf -h "$OUTPUT_LIB" | \
    awk -F: '/Machine:/ {sub(/^[[:space:]]+/, "", $2); print $2}')"
[ "$actual_machine" = "$expected_machine" ] || \
    err "Host replay architecture mismatch: expected '$expected_machine', got '$actual_machine'"

marker_sha="$(sha256sum "$OUTPUT_MARKER" | awk '{print $1}')"
library_sha="$(sha256sum "$OUTPUT_LIB" | awk '{print $1}')"
cat > "$OUTPUT_ROOT/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "runtimeVersion": "phase2-host-heaven-v1",
  "architecture": "$NATIVE_ARCH",
  "module": "libwinehua_host_heaven_replay.so",
  "entryPoint": "winehua_host_replay_main",
  "files": {
    "bin/heaven_exact_host_replay": "$marker_sha",
    "lib/libwinehua_host_heaven_replay.so": "$library_sha"
  }
}
EOF

log "Host Vulkan replay ready: $OUTPUT_LIB"
