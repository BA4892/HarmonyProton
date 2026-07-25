#pragma once
#include <cstdint>

// 保比例适配 (letterbox) 几何: 正/逆映射的唯一实现。
//
// 背景: "把 Wine 帧保比例缩放、居中显示到窗口/桌面" 这一映射曾有多份独立实现
// (egl_renderer letterbox、ComputeFullscreenTransform、输入侧手写逆映射),
// 系数和取整方式不一致是全屏/黑边鼠标问题的温床 (docs/CPP_REFACTOR_PLAN.md Phase 1)。
// 本模块为纯函数, 不依赖 wayland/OHOS, 宿主机可直接单测 (host_tests/geometry_test.cpp,
// make test)。

struct FitRect {
    int srcW = 0, srcH = 0;  // 内容尺寸 (Wine 帧)
    int dstW = 0, dstH = 0;  // 内容在显示区内的缩放后尺寸 (黑边之内)
    int offX = 0, offY = 0;  // 内容区在显示区中的原点 (黑边偏移)
    double scale = 1.0;      // dst/src 的未取整缩放系数
};

// 计算保比例缩放 + 居中的适配矩形。任一尺寸 <= 0 返回 false (out 不动)。
bool ComputeFitRect(int rootW, int rootH, int winW, int winH, FitRect& out);

// -- 正/逆映射 (数学定义, 用未取整 scale) --
// desktop 合成/命中路径用: 合成用未取整 scale blit, 输入按同一 scale 除回, 严格互逆。
inline double FitMapX(const FitRect& t, double x) { return t.offX + x * t.scale; }
inline double FitMapY(const FitRect& t, double y) { return t.offY + y * t.scale; }
inline double FitUnmapX(const FitRect& t, double px) { return (px - t.offX) / t.scale; }
inline double FitUnmapY(const FitRect& t, double py) { return (py - t.offY) / t.scale; }

// -- 正/逆映射 (按取整后的 dst 尺寸) --
// glViewport 路径用: 实际显示占据 dstW x dstH 整数像素, 与屏幕上可见像素的换算
// 严格一致。egl_renderer zero-copy 层视口 (正) 和 CoordTransform 输入换算 (逆) 用。
// 全部带零尺寸防御: ComputeFitRect 失败时 FitRect 保持全零, 除零会直接 SIGFPE。
inline int FitMapDisplayX(const FitRect& t, int64_t x) { return t.srcW > 0 ? t.offX + static_cast<int>(x * t.dstW / t.srcW) : 0; }
inline int FitMapDisplayY(const FitRect& t, int64_t y) { return t.srcH > 0 ? t.offY + static_cast<int>(y * t.dstH / t.srcH) : 0; }
// 尺寸正映射 (同上, 不带原点偏移)
inline int FitSizeDisplayW(const FitRect& t, int64_t w) { return t.srcW > 0 ? static_cast<int>(w * t.dstW / t.srcW) : 0; }
inline int FitSizeDisplayH(const FitRect& t, int64_t h) { return t.srcH > 0 ? static_cast<int>(h * t.dstH / t.srcH) : 0; }
inline double FitUnmapDisplayX(const FitRect& t, double px) { return t.dstW > 0 ? (px - t.offX) * t.srcW / t.dstW : 0.0; }
inline double FitUnmapDisplayY(const FitRect& t, double py) { return t.dstH > 0 ? (py - t.offY) * t.srcH / t.dstH : 0.0; }
