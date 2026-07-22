# Makefile — Wine for HarmonyOS 构建编排
#
# 用法:
#   make                                          # 默认: x86_64 全量构建
#   make NATIVE_ARCH=x86_64
#   make NATIVE_ARCH=arm64-v8a
#   make NATIVE_ARCH=all                          # 双架构 HAP
#
#   单个模块: make deps | wine | box64 | native | assemble | hap
#   清理:     make clean

ROOT := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
.DEFAULT_GOAL := all

# ── 配置 ──
NATIVE_ARCH ?= x86_64
GUEST_ARCH ?= x86_64
BUILD_GUEST_GFX ?= 1
BUILD_GUEST_VULKAN ?= 1
TARGET_SDK_VERSION ?= 6.1.0(23)
COMPATIBLE_SDK_VERSION ?= 6.1.0(23)
export NATIVE_ARCH
export GUEST_ARCH
export BUILD_GUEST_GFX
export BUILD_GUEST_VULKAN
export TARGET_SDK_VERSION
export COMPATIBLE_SDK_VERSION

CONFIG    := $(NATIVE_ARCH)
BUILD_DIR := $(ROOT)/build
STAMPS    := $(BUILD_DIR)/.stamps
SCRIPTS   := $(ROOT)/scripts
DXVK_SENTINEL := $(BUILD_DIR)/dxvk/legacy/x64/bin/d3d11.dll
DXVK_STAMP := $(STAMPS)/dxvk-legacy
DXVK_SOURCE_INPUTS := $(shell find $(ROOT)/thirdparty/dxvk/src -type f 2>/dev/null; find $(ROOT)/thirdparty/dxvk -maxdepth 1 -type f 2>/dev/null)

# 架构列表 (NATIVE_ARCH=all 时展开为两个)
ifeq ($(NATIVE_ARCH),all)
ARCHES := arm64-v8a x86_64
else
ARCHES := $(NATIVE_ARCH)
endif

# ── 关键产物 (用于验证构建是否完成) ──
DEPS_SENTINEL   := $(BUILD_DIR)/sysroot-ext/usr/lib/x86_64-linux-ohos/libfreetype.so.6
WINE_SENTINEL   := $(BUILD_DIR)/wine-native/tools/winegcc/winegcc
GUEST_GFX_SENTINEL := $(BUILD_DIR)/guest_gfx/$(GUEST_ARCH)/winehua-guest-gfx.env
GUEST_VULKAN_SENTINEL := $(BUILD_DIR)/guest_vulkan/$(GUEST_ARCH)/manifest.json

# Guest runtime build scripts can also be invoked directly while iterating on
# Mesa/Venus. Track their manifests as assemble inputs so a subsequent
# `make hap` cannot silently reuse an older staged wine-data.zip.
ASSEMBLE_GUEST_INPUTS :=
ifeq ($(BUILD_GUEST_GFX),1)
ASSEMBLE_GUEST_INPUTS += $(wildcard $(GUEST_GFX_SENTINEL))
endif

# ============================================================
# dxvk — managed WineHua DXVK Legacy fork (x64 + x86)
# ============================================================
.PHONY: dxvk
dxvk: $(DXVK_STAMP)

$(DXVK_STAMP): $(SCRIPTS)/build_dxvk.sh $(DXVK_SOURCE_INPUTS) | $(STAMPS)
	@echo "=== dxvk legacy ==="
	bash $(SCRIPTS)/build_dxvk.sh
	touch $@
ifeq ($(BUILD_GUEST_VULKAN),1)
ASSEMBLE_GUEST_INPUTS += $(wildcard $(GUEST_VULKAN_SENTINEL))
endif

# ============================================================
# 默认目标
# ============================================================
.PHONY: all
all: hap

# FORCE: 伪目标，永远"过期"，让 make 总是进入 recipe
# recipe 内部的 find -newer 才是真正的增量判断
.PHONY: FORCE
FORCE:

# 确保 stamps 目录存在
$(STAMPS):
	mkdir -p $(STAMPS)

# 确保架构子目录存在
$(STAMPS)/arm64-v8a $(STAMPS)/x86_64:
	mkdir -p $@

# ============================================================
# deps — 交叉编译依赖 → build/sysroot-ext/ (架构无关)
# ============================================================
.PHONY: deps
deps: $(STAMPS)/deps

$(STAMPS)/deps: $(SCRIPTS)/build_deps.sh $(SCRIPTS)/build_ohos_guest_gfx.sh \
	$(SCRIPTS)/build_ohos_guest_vulkan.sh $(ROOT)/smoke/guest_vulkan_smoke.c \
	$(ROOT)/smoke/venus_sampled_image_probe.c \
	$(ROOT)/smoke/venus_depth_cube_probe.inc \
	$(ROOT)/smoke/venus_depth_cube_graphics_replay.inc \
	$(ROOT)/smoke/venus_fullscreen_triangle.vert \
	$(ROOT)/smoke/venus_depth_cube_golden.frag \
	$(ROOT)/smoke/venus_depth_cube_fail.spvasm \
	$(ROOT)/smoke/venus_storage_write.comp \
	$(ROOT)/smoke/venus_storage_read.comp \
	$(ROOT)/smoke/venus_image_fetch.comp \
	$(ROOT)/smoke/venus_combined_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_unknown_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_spec_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_vector_spec_sample.comp \
	$(ROOT)/smoke/venus_depth_array_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_sample.comp \
	$(ROOT)/smoke/venus_depth_cube_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_separated_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_dxvk_contract_compare.spvasm \
	$(ROOT)/smoke/venus_depth_cube_array_sample.comp \
	$(ROOT)/smoke/venus_depth_cube_array_2d_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_array_compare.comp \
	$(ROOT)/smoke/venus_spirv_replay.c \
	$(wildcard $(ROOT)/replay_spv/CS_*.remapped.spv) \
	$(ROOT)/smoke/venus_separated_sample.comp \
	$(SCRIPTS)/env.sh FORCE | $(STAMPS)
	@guest_gfx_ready=1; \
	if [ "$(BUILD_GUEST_GFX)" = "1" ] && [ ! -f "$(GUEST_GFX_SENTINEL)" ]; then \
	    guest_gfx_ready=0; \
	fi; \
	guest_vulkan_ready=1; \
	if [ "$(BUILD_GUEST_VULKAN)" = "1" ] && [ ! -f "$(GUEST_VULKAN_SENTINEL)" ]; then \
	    guest_vulkan_ready=0; \
	fi; \
	if [ -f $@ ] && [ -f $(DEPS_SENTINEL) ] && [ "$$guest_gfx_ready" = "1" ] && \
	    [ "$$guest_vulkan_ready" = "1" ] && \
	    ! [ "$(SCRIPTS)/build_ohos_guest_gfx.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_ohos_guest_vulkan.sh" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/guest_vulkan_smoke.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_sampled_image_probe.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_probe.inc" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_graphics_replay.inc" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_fullscreen_triangle.vert" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_golden.frag" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_fail.spvasm" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_storage_write.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_storage_read.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_image_fetch.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_combined_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_dxvk_contract_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_array_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_separated_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_dxvk_contract_compare.spvasm" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_2d_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_spirv_replay.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_separated_sample.comp" -nt $@ ] && \
	    ! find $(ROOT)/thirdparty/freetype \
	           $(ROOT)/thirdparty/libffi \
	           $(ROOT)/thirdparty/wayland \
	           $(ROOT)/thirdparty/wayland-protocols \
	           $(ROOT)/thirdparty/libxml2 \
	           $(ROOT)/thirdparty/libxkbcommon \
	           $(ROOT)/thirdparty/xkeyboard-config \
	           $(ROOT)/thirdparty/mesa \
	           $(ROOT)/thirdparty/libdrm \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.py' -o -name '*.xml' \
	              -o -name '*.ac' -o -name 'Makefile.am' -o -name '*.m4' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [deps] up to date"; \
	else \
	    echo "=== deps ==="; \
	    bash $(SCRIPTS)/build_deps.sh && touch $@; \
	fi

# ============================================================
# wine — Wine 交叉编译 + wineserver
# ============================================================
.PHONY: wine
wine: $(STAMPS)/wine-$(CONFIG)

$(STAMPS)/wine-$(CONFIG): $(SCRIPTS)/build_wine.sh $(SCRIPTS)/env.sh $(STAMPS)/deps FORCE | $(STAMPS)
	@if [ -f $@ ] && [ -f $(WINE_SENTINEL) ] && \
	    ! find $(ROOT)/thirdparty/wine \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.ac' -o -name 'Makefile.am' \
	              -o -name '*.m4' -o -name '*.in' -o -name '*.rc' -o -name '*.spec' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [wine] up to date"; \
	else \
	    echo "=== wine ($(CONFIG)) ==="; \
	    bash $(SCRIPTS)/build_wine.sh && touch $@; \
	fi

# ============================================================
# wine32 — 32-bit PE DLL (i686-mingw32, WoW64 必需)
# ============================================================
WINE32_SENTINEL := $(BUILD_DIR)/wine-i386-pe/dlls/ntdll/i386-windows/ntdll.dll

.PHONY: wine32
wine32: $(STAMPS)/wine32

$(STAMPS)/wine32: $(SCRIPTS)/build_wine32_pe.sh $(SCRIPTS)/env.sh $(STAMPS)/deps FORCE | $(STAMPS)
	@if [ -f $@ ] && [ -f $(WINE32_SENTINEL) ] && \
	    ! find $(ROOT)/thirdparty/wine \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name '*.rc' -o -name '*.spec' -o -name '*.idl' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [wine32] up to date"; \
	else \
	    echo "=== wine32 ==="; \
	    bash $(SCRIPTS)/build_wine32_pe.sh && touch $@; \
	fi

# ============================================================
# box64 — ARM64 翻译器 (始终 arm64-v8a 架构, 编译为 box64.so dlopen 加载)
# ============================================================
.PHONY: box64
box64: $(STAMPS)/box64-arm64-v8a

$(STAMPS)/box64-arm64-v8a: $(SCRIPTS)/build_box64.sh $(SCRIPTS)/env.sh FORCE | $(STAMPS)
	@if [ "$(NATIVE_ARCH)" = "x86_64" ]; then \
	    echo "  [box64] skip (x86_64)"; \
	    mkdir -p $(dir $@) && touch $@; \
	elif [ -f $@ ] && \
	    ! find $(ROOT)/thirdparty/box64 \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.S' \
	              -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [box64] up to date"; \
	else \
	    echo "=== box64 ==="; \
	    NATIVE_ARCH=arm64-v8a bash $(SCRIPTS)/build_box64.sh && touch $@; \
	fi

# ============================================================
# native — Native compositor 依赖 → entry/libs/ (架构相关)
# ============================================================
.PHONY: native
native: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/native)

NATIVE_SENTINEL_arm64_v8a := $(ROOT)/entry/libs/arm64-v8a/libvirglrenderer.so.1
NATIVE_SENTINEL_x86_64    := $(ROOT)/entry/libs/x86_64/libvirglrenderer.so.1

define native_rule
.PHONY: native-$(1)
native-$(1): $$(STAMPS)/$(1)/native

$$(STAMPS)/$(1)/native: $(SCRIPTS)/build_native.sh $(SCRIPTS)/env.sh FORCE | $$(STAMPS)/$(1)
	@sentinel="$(NATIVE_SENTINEL_$(subst -,_,$(1)))"; \
	libs_dir="$(ROOT)/entry/libs/$(1)"; \
		if [ -f $$@ ] && [ -f "$$$$sentinel" ] && \
		    [ -f "$$$$libs_dir/libfreetype.so.6" ] && \
		    [ -f "$$$$libs_dir/libxkbcommon.so.0" ] && \
		    [ -f "$$$$libs_dir/libxml2.so.2" ] && \
		    [ -f "$$$$libs_dir/libwinehua_vtest_server.so" ] && \
	    ! [ "$(SCRIPTS)/build_native.sh" -nt $$@ ] && \
	    ! find $(ROOT)/thirdparty/wayland \
	           $(ROOT)/thirdparty/libffi \
	           $(ROOT)/thirdparty/libepoxy \
	           $(ROOT)/thirdparty/virglrenderer \
	           -newer $$@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.ac' -o -name 'Makefile.am' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [native/$(1)] up to date"; \
	else \
	    echo "=== native ($(1)) ==="; \
	    NATIVE_ARCH=$(1) bash $(SCRIPTS)/build_native.sh && touch $$@; \
	fi
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call native_rule,$(a))))

# ============================================================
# assemble — 组装布局 (架构 + 设备类型相关)
# ============================================================
.PHONY: assemble
assemble: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/assemble)

define assemble_rule
.PHONY: assemble-$(1)

assemble-$(1): $$(STAMPS)/$(1)/assemble

$$(STAMPS)/$(1)/assemble: $(SCRIPTS)/assemble.sh $(SCRIPTS)/env.sh $(DXVK_SENTINEL) $(DXVK_STAMP) \
	$(ROOT)/smoke/winehua_d3d_switch_cube.c \
	$(ROOT)/smoke/winehua_win32_driver.c \
	$$(STAMPS)/deps $$(STAMPS)/wine-$(1) $$(STAMPS)/$(1)/native \
	$$(ASSEMBLE_GUEST_INPUTS) | $$(STAMPS)/$(1)
	@echo "=== assemble ($(1)) ==="
	NATIVE_ARCH=$(1) GUEST_ARCH=$(GUEST_ARCH) BUILD_GUEST_GFX=$(BUILD_GUEST_GFX) bash $(SCRIPTS)/assemble.sh
	@touch $$@
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call assemble_rule,$(a))))

# arm64 assemble 额外依赖 box64 + wine32 (WoW64 32-bit PE DLL)
$(STAMPS)/arm64-v8a/assemble: $(STAMPS)/box64-arm64-v8a $(STAMPS)/wine32

# ============================================================
# hap — HAP 构建 + 签名 (统一 rawfile zip)
# ============================================================
.PHONY: hap
hap: assemble
	@echo "=== hap ($(CONFIG)) ==="
	bash $(SCRIPTS)/package.sh hap
	@echo ""
	@echo "HAP: $(ROOT)/entry/build/default/outputs/default/entry-default-signed.hap"
	@ls -lh $(ROOT)/entry/build/default/outputs/default/entry-default-signed.hap 2>/dev/null || true

# ============================================================
# clean
# ============================================================
.PHONY: clean
clean:
	@echo "=== clean ==="
	rm -rf $(BUILD_DIR)
	rm -f $(ROOT)/entry/libs/arm64-v8a/*.so*
	rm -f $(ROOT)/entry/libs/arm64-v8a/virgl_test_server
	rm -f $(ROOT)/entry/libs/x86_64/*.so*
	rm -f $(ROOT)/entry/libs/x86_64/virgl_test_server
	rm -rf $(ROOT)/entry/build
	rm -f $(ROOT)/entry/src/main/resources/rawfile/wine-data.zip
	@echo "  已清理所有中间产物"

# ============================================================
# 帮助
# ============================================================
.PHONY: help
help:
	@echo "用法: make [target] [NATIVE_ARCH=x86_64|arm64-v8a|all]"
	@echo ""
	@echo "默认: NATIVE_ARCH=x86_64"
	@echo "SDK: target=$(TARGET_SDK_VERSION), compatible=$(COMPATIBLE_SDK_VERSION)"
	@echo ""
	@echo "全部构建:"
	@echo "  make                                          # 默认配置全量 → HAP"
	@echo "  make NATIVE_ARCH=arm64-v8a                    # ARM64"
	@echo "  make NATIVE_ARCH=all                          # 双架构 HAP"
	@echo ""
	@echo "单模块:"
	@echo "  make deps      # 交叉编译依赖 → sysroot-ext"
	@echo "  make wine      # Wine + wineserver"
	@echo "  make box64     # Box64 (仅 arm64)"
	@echo "  make native    # Native compositor 依赖"
	@echo "  make assemble  # 组装布局"
	@echo "  make hap       # HAP 打包 + 签名"
	@echo ""
	@echo "每个架构:"
	@echo "  make native-x86_64  make native-arm64-v8a"
	@echo ""
	@echo "清理:"
	@echo "  make clean     # 删除所有中间产物"
	@echo ""
	@echo "产物统一在 build/ 下"
