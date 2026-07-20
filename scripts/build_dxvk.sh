#!/bin/bash
# Build the managed WineHua DXVK Legacy fork for both PE architectures.
# This runs inside the canonical Docker build container; the Meson build
# directories are intentionally kept beside the fork so later DXVK changes
# can be rebuilt incrementally without touching the Wine tree.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

[ -d "$DXVK_SRC" ] || err "DXVK fork missing: $DXVK_SRC"
setup_if_missing() {
    local build_dir="$1"
    local cross_file="$2"
    local prefix="$3"
    if [ ! -f "$build_dir/build.ninja" ]; then
        log "Configuring DXVK $(basename "$build_dir")"
        meson setup "$build_dir" "$DXVK_SRC" \
            --cross-file "$DXVK_SRC/$cross_file" \
            --prefix "$prefix" -Dbuildtype=release
    fi
}

setup_if_missing "$DXVK_SRC/build.winehua64" build-win64.txt "$DXVK_BUILD_ROOT/x64"
setup_if_missing "$DXVK_SRC/build.winehua32" build-win32.txt "$DXVK_BUILD_ROOT/x86"

log "--- DXVK Legacy fork ($DXVK_SRC) ---"
ninja -C "$DXVK_SRC/build.winehua64" install
ninja -C "$DXVK_SRC/build.winehua32" install

for dll in d3d11.dll dxgi.dll; do
    [ -f "$DXVK_BUILD_ROOT/x64/bin/$dll" ] || \
        err "DXVK x64 artifact missing: $DXVK_BUILD_ROOT/x64/bin/$dll"
    [ -f "$DXVK_BUILD_ROOT/x86/bin/$dll" ] || \
        err "DXVK x86 artifact missing: $DXVK_BUILD_ROOT/x86/bin/$dll"
done

log "DXVK Legacy ready: $(git -c safe.directory="$DXVK_SRC" -C "$DXVK_SRC" rev-parse --short HEAD)"
