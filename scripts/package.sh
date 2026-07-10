#!/bin/bash
# package.sh — HAP 构建 + 签名 + 部署
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
# 工具函数: 动态设置 abiFilters
set_abi_filters() {
    # 根据 NATIVE_ARCH 写 build-profile.json5 的 abiFilters
    local profile="$WINEHUA/entry/build-profile.json5"
    if [ ! -f "$profile" ]; then
        err "build-profile.json5 未找到: $profile"
    fi

    local abi_value
    if [ "$NATIVE_ARCH" = "all" ]; then
        abi_value='"arm64-v8a", "x86_64"'
    else
        abi_value="\"$NATIVE_ARCH\""
    fi

    # 用 python 正则替换, 支持多行 abiFilters
    python3 -c "
import re
with open('$profile', 'r') as f:
    content = f.read()
content = re.sub(r'\"abiFilters\"\s*:\s*\[[^\]]*\]', '\"abiFilters\": [$abi_value]', content)
with open('$profile', 'w') as f:
    f.write(content)
"
    log "abiFilters: [$abi_value]"
}

# ============================================================
package_hap() {
    log "=== 打包 HAP ($NATIVE_ARCH) ==="
    local unsigned_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-unsigned.hap"
    local signed_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    set_abi_filters

    # 移除 hnpPackages (所有平台统一用 rawfile zip)
    local module_json="$WINEHUA/entry/src/main/module.json5"
    python3 -c "
import re
with open('$module_json', 'r') as f:
    content = f.read()
content = re.sub(r',?\s*\"hnpPackages\"\s*:\s*\[[^][]*\]', '', content)
with open('$module_json', 'w') as f:
    f.write(content)
"
    log "  已移除 hnpPackages 配置"

    # 清理非目标架构的 native libs (hvigorw ProcessLibs 会打包所有 libs/)
    local libs_root="$WINEHUA/entry/libs"
    if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        rm -rf "$libs_root/x86_64"
    elif [ "$NATIVE_ARCH" = "x86_64" ]; then
        rm -rf "$libs_root/arm64-v8a"
    fi

    cd "$WINEHUA"
    hvigorw assembleHap || { err "hvigorw assembleHap 失败"; return 1; }

    cd "$WINEHUA"
    python3 sign.py "$unsigned_hap" "$signed_hap"

    ls -lh "$signed_hap"
    log "HAP 构建 + 签名完成 ($NATIVE_ARCH)"
}

# ============================================================
deploy() {
    local device="${1:-192.168.1.4:38879}"
    local hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    if [ ! -f "$hap" ]; then
        err "HAP 文件不存在: $hap"
    fi

    log "=== 部署到 $device ==="
    hdc tconn "$device" || { err "hdc tconn 失败"; }
    hdc shell bm uninstall -n app.hackeris.winehua 2>/dev/null || true
    hdc file send "$hap" /data/local/tmp/ || { err "hdc file send 失败"; }
    hdc shell bm install -p /data/local/tmp/entry-default-signed.hap -r || { err "bm install 失败"; }

    log "部署完成"
}

# ---- main ----
case "${1:-}" in
    hap)  package_hap ;;
    deploy) deploy "${2:-}" ;;
    all)
        package_hap && deploy "${2:-}"
        ;;
    *)    echo "用法: $0 {hap|deploy|all} [device_ip]" >&2; exit 1 ;;
esac
