#include "input_resolver.h"
#include "toplevel_manager.h"
#include "desktop_compositor.h"
#include "compositor_utils.h"
#include "geometry.h"
#include "compositor/surface_data.h"
#include <algorithm>
#include <cmath>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

InputResolver::InputResolver(ToplevelManager& tmgr, DesktopCompositor& compositor,
                             const uint32_t& desktopRootToplevelId,
                             const int32_t& outputW, const int32_t& outputH)
    : tmgr_(tmgr)
    , compositor_(compositor)
    , desktopRootToplevelId_(desktopRootToplevelId)
    , outputW_(outputW)
    , outputH_(outputH)
{
}

uint32_t InputResolver::FindToplevelAt(int x, int y)
{
    InputTarget target;
    FindInputTargetAt(x, y, target);
    return target.toplevelId;
}

bool InputResolver::FindInputTargetAt(int x, int y, InputTarget& out)
{
    auto lk = tmgr_.Lock();
    uint32_t rootId = desktopRootToplevelId_;

    /*
     * 全屏窗口独占输入: 命中判定走与渲染相同的保比例缩放几何
     * (ComputeFitRect, 见 TakeToplevelFrame), 只有该窗口及其
     * subsurface 层可交互; 黑边事件归属全屏窗口并标 swallow
     * (调用方只吞 PRESS, MOVE/RELEASE 照常透传)
     */
    {
        const auto* rootSt = tmgr_.FindToplevelLocked(rootId);
        const int rootW = (rootSt && rootSt->w > 0) ? rootSt->w : outputW_;
        const int rootH = (rootSt && rootSt->h > 0) ? rootSt->h : outputH_;
        // 两遍扫描与渲染侧 (TakeToplevelFrame) 同序: pass 0 跳过 explorer 窗口,
        // 多窗口同时全屏时游戏优先; pass 1 接受 explorer 兜底。
        // 命中任一候选即在循环体内 return, pass 1 只在 pass 0 无候选时执行
        for (int pass = 0; pass < 2; ++pass) {
        for (auto zit = tmgr_.toplevelZOrder().rbegin(); zit != tmgr_.toplevelZOrder().rend(); ++zit) {
            const auto* zst = tmgr_.FindToplevelLocked(*zit);
            if (!zst || !zst->fullscreen || !tmgr_.IsToplevelVisibleLocked(*zit, desktopRootToplevelId_)) continue;
            if (pass == 0 && zst->isExplorerWindow) continue;
            // 逆变换尺寸必须与渲染一致: ZC 游戏用全屏前尺寸 (游戏分辨率),
            // SHM 游戏用实际 buffer 尺寸 (geometry.h SelectFullscreenContentSize)
            int contentW = 0, contentH = 0;
            SelectFullscreenContentSize(zst->preFsW, zst->preFsH, zst->w, zst->h,
                                        compositor_.HasZeroCopyLayerForToplevelLocked(*zit),
                                        contentW, contentH);
            FitRect transform;
            if (!ComputeFitRect(rootW, rootH, contentW, contentH, transform)) break;
            const uint32_t fullscreenId = *zit;
            // 诊断: 全屏输入目标选取 (仅目标变化时输出 — 多窗口同时全屏时
            // 选错窗口的点击路由问题靠它定位, 例如 explorer 窗口压在游戏上)
            static uint32_t sLastPicked = 0;
            if (fullscreenId != sLastPicked) {
                sLastPicked = fullscreenId;
                OH_LOG_INFO(LOG_APP,
                    "[Input] fs-pick tl=#%{public}u pass=%{public}d shell=%{public}d zc=%{public}d"
                    " preFs=%{public}dx%{public}d buf=%{public}dx%{public}d → content=%{public}dx%{public}d",
                    fullscreenId, pass, zst->isExplorerWindow ? 1 : 0,
                    compositor_.HasZeroCopyLayerForToplevelLocked(*zit) ? 1 : 0,
                    zst->preFsW, zst->preFsH, zst->w, zst->h, contentW, contentH);
            }
            // 该窗口的 subsurface 层绘制在窗口内容之上, 先命中 (同一变换)
            for (auto it = compositor_.subsurfaceLayers().rbegin(); it != compositor_.subsurfaceLayers().rend(); ++it) {
                if (compositor_.zeroCopySurfaceKeys().count(it->surfaceKey)) continue;
                if (it->parentToplevel != fullscreenId || it->w <= 0 || it->h <= 0) continue;
                int layerX = 0, layerY = 0;
                compositor_.ResolveSubsurfaceLayerPositionLocked(*it, layerX, layerY);
                const int layerDispW = it->vpDstW > 0 ? std::min(it->vpDstW, it->w) : it->w;
                const int layerDispH = it->vpDstH > 0 ? std::min(it->vpDstH, it->h) : it->h;
                const int layerScrX = static_cast<int>(lround(FitMapX(transform, layerX - zst->x)));
                const int layerScrY = static_cast<int>(lround(FitMapY(transform, layerY - zst->y)));
                const int layerScrW = std::max(1, static_cast<int>(lround(layerDispW * transform.scale)));
                const int layerScrH = std::max(1, static_cast<int>(lround(layerDispH * transform.scale)));
                if (x >= layerScrX && x < layerScrX + layerScrW && y >= layerScrY && y < layerScrY + layerScrH) {
                    out.toplevelId = fullscreenId;
                    out.surface = it->surface;
                    out.originX = layerScrX;
                    out.originY = layerScrY;
                    out.scale = static_cast<float>(transform.scale);
                    return out.surface != nullptr;
                }
            }
            if (x >= transform.offX && x < transform.offX + transform.dstW &&
                y >= transform.offY && y < transform.offY + transform.dstH) {
                out.toplevelId = fullscreenId;
                out.surface = tmgr_.GetSurfaceForToplevel(fullscreenId);
                out.originX = transform.offX;
                out.originY = transform.offY;
                out.scale = static_cast<float>(transform.scale);
                return out.surface != nullptr;
            }
            // 黑边: 事件归属仍是全屏窗口 (返回其 surface/原点/缩放),
            // 但标记 swallow — 调用方吞掉 PRESS (防幻影点击/焦点切换),
            // MOVE/RELEASE 必须照常透传: 坐标越界由 winewayland 的 motion
            // clamp 夹回窗口边缘; 若连 RELEASE 一起吞, 内容区按下拖到黑边
            // 松手会丢失 release, pressedButtons_ 按键状态永久卡死
            out.toplevelId = fullscreenId;
            out.surface = tmgr_.GetSurfaceForToplevel(fullscreenId);
            out.originX = transform.offX;
            out.originY = transform.offY;
            out.scale = static_cast<float>(transform.scale);
            out.swallow = true;
            return out.surface != nullptr;
        }
        }
    }

    /*
     * subsurface 命中优先于 toplevel (渲染在上层):
     * - 内部菜单: enter 层自己的 wl_surface, 坐标以层原点为基。
     *   层可伸出父窗口边界 — 若改走父窗口 surface, 伸出部分产生越界的
     *   窗口相对坐标, 会被 winewayland 的 motion clamp
     *   (wayland_pointer.c "bring them within bounds") 夹回窗口内,
     *   菜单项永远收不到该区域的点击
     * - 外部菜单 (isExternal): 任务栏弹出等, subsurface offset 是 Wine
     *   虚拟屏幕坐标 → 走 root, Wine explorer 内部处理点击分发
     */
    for (auto it = compositor_.subsurfaceLayers().rbegin(); it != compositor_.subsurfaceLayers().rend(); ++it) {
        // zero-copy GL 层不参与置顶命中: 渲染时它按窗口 z 位被遮挡重绘压回
        // (egl_renderer occluder redraw), 命中同样交给下方 toplevel z-order 循环,
        // 否则被挡住的 GL 窗口仍会收到点击。
        // 查的是实时集合: GPU→CPU fallback 时 key 被移出 zeroCopySurfaceKeys_,
        // 该层自动恢复为普通 subsurface (CPU 合成置顶, 命中也置顶), 无需特判
        if (compositor_.zeroCopySurfaceKeys().count(it->surfaceKey)) continue;
        if (it->parentToplevel != rootId && !tmgr_.IsToplevelVisibleLocked(it->parentToplevel, desktopRootToplevelId_)) continue;
        if (it->w <= 0 || it->h <= 0) continue;
        int layerX = 0, layerY = 0;
        compositor_.ResolveSubsurfaceLayerPositionLocked(*it, layerX, layerY);
        if (x >= layerX && x < layerX + it->w && y >= layerY && y < layerY + it->h) {
            if (it->isExternal) {
                out.toplevelId = rootId;
                out.surface = tmgr_.GetSurfaceForToplevel(rootId);
                out.originX = 0;
                out.originY = 0;
            } else {
                out.toplevelId = it->parentToplevel;
                out.surface = it->surface;
                out.originX = layerX;
                out.originY = layerY;
            }
            return out.surface != nullptr;
        }
    }

    for (auto it = tmgr_.toplevelZOrder().rbegin(); it != tmgr_.toplevelZOrder().rend(); ++it) {
        uint32_t id = *it;
        if (!tmgr_.IsToplevelVisibleLocked(id, desktopRootToplevelId_)) continue;
        const auto* st = tmgr_.FindToplevelLocked(id);
        if (!st) continue;
        if (x >= st->x && x < st->x + st->w && y >= st->y && y < st->y + st->h) {
            wl_resource* surf = tmgr_.GetSurfaceForToplevel(id);
            if (surf) {
                auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surf));
                if (sd && sd->inputRegionEmpty) continue;
            }
            out.toplevelId = id;
            out.surface = surf;
            out.originX = st->x;
            out.originY = st->y;
            return out.surface != nullptr;
        }
    }

    out.toplevelId = rootId;
    out.surface = tmgr_.GetSurfaceForToplevel(rootId);
    out.originX = 0;
    out.originY = 0;
    return out.surface != nullptr;
}

bool InputResolver::IsSurfaceAlive(wl_resource* surface)
{
    if (!surface) return false;
    auto lk = tmgr_.Lock();
    return tmgr_.ContainsSurfaceResource(surface);
}

bool InputResolver::IsZcGameSurface(wl_resource* surface)
{
    const uint32_t tl = tmgr_.FindToplevelBySurface(surface);
    if (!tl) return false;
    auto lk = tmgr_.Lock();
    return compositor_.HasZeroCopyLayerForToplevelLocked(tl);
}

bool InputResolver::SurfaceLocalToDesktop(wl_resource* surface, double lx, double ly,
                                          double& dx, double& dy)
{
    const uint32_t tl = tmgr_.FindToplevelBySurface(surface);
    if (!tl) return false;
    auto lk = tmgr_.Lock();
    const auto* st = tmgr_.FindToplevelLocked(tl);
    if (!st) return false;
    if (st->fullscreen) {
        // 与 FindInputTargetAt 全屏分支同一几何 (SelectFullscreenContentSize
        // + ComputeFitRect), 保证 warp 锚点与输入逆映射互为正反变换
        const auto* rootSt = tmgr_.FindToplevelLocked(desktopRootToplevelId_);
        const int rootW = (rootSt && rootSt->w > 0) ? rootSt->w : outputW_;
        const int rootH = (rootSt && rootSt->h > 0) ? rootSt->h : outputH_;
        int contentW = 0, contentH = 0;
        SelectFullscreenContentSize(st->preFsW, st->preFsH, st->w, st->h,
                                    compositor_.HasZeroCopyLayerForToplevelLocked(tl),
                                    contentW, contentH);
        FitRect transform;
        if (!ComputeFitRect(rootW, rootH, contentW, contentH, transform)) return false;
        dx = transform.offX + lx * transform.scale;
        dy = transform.offY + ly * transform.scale;
        return true;
    }
    dx = st->x + lx;
    dy = st->y + ly;
    return true;
}
