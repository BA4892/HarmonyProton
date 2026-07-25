#pragma once
#include <cstdint>

// compositor 全局常量。
// 原为散落于 wayland_server.cpp / desktop_compositor.cpp 的 magic number,
// 集中命名并注明来源/原因 (重构原则: 特例有名有姓有原因, 见 docs/CPP_REFACTOR_PLAN.md)。
namespace compositor_consts {

// -- 虚拟 wl_output 上报参数 --
// Wine winewayland 读 mode/geometry 推算 DPI; 上报值不影响渲染内容。
constexpr int32_t kDefaultOutputWidth = 1280;
constexpr int32_t kDefaultOutputHeight = 720;
constexpr int32_t kOutputRefreshMillihertz = 60000;  // 60Hz, 协议单位为 mHz
// 默认分辨率下的物理尺寸 (mm): 1280x720 → 340x190 ≈ 96 DPI; 实际分辨率按比例折算
constexpr int32_t kOutputPhysWidthMm = 340;
constexpr int32_t kOutputPhysHeightMm = 190;

// -- FNV-1a 64 位哈希 (ARGB 形状掩码哈希 / 桌面合成签名共用) --
constexpr uint64_t kFnv1aOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t kFnv1aPrime = 1099511628211ULL;

// -- ARGB 窗口剪影掩码 (setWindowMask 用) --
// 阈值 128: 半透明抗锯齿边缘向内收半像素, 避免灰边外扩
constexpr uint8_t kArgbMaskAlphaThreshold = 128;

// -- 任务栏启发 --
// Wine/explorer 不提供任务栏身份标记, 只能按几何特征推断: 底部对齐 + 高度 < 100
constexpr int32_t kTaskbarMaxHeight = 100;

// -- 最小化自动恢复阈值 --
// Wine 没有 unset_minimized 协议, 还原时直接 commit 正常尺寸内容;
// 最小化标题栏约 200x30, 大于该尺寸的 commit 判定为真实窗口恢复
constexpr int32_t kRestoreMinContentWidth = 200;
constexpr int32_t kRestoreMinContentHeight = 50;

// -- Wine 最小化坐标补偿 --
// Wine 把最小化窗口移到 (-32000,-32000), subsurface offset 因此偏 +32000;
// 超过阈值即判定为偏移并减回 (详见 wayland_server.cpp surface_commit 注释)
constexpr int32_t kMinimizedCoordThreshold = 16000;
constexpr int32_t kMinimizedCoordOffset = 32000;

} // namespace compositor_consts
