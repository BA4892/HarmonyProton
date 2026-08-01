#include "desktop_compositor.h"
#include "toplevel_manager.h"
#include "compositor_utils.h"
#include "geometry.h"
#include "compositor/surface_data.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

DesktopCompositor::DesktopCompositor(ToplevelManager& tmgr,
                                     const DisplayPolicy& policy,
                                     const uint32_t& desktopRootToplevelId,
                                     const int32_t& outputW,
                                     const int32_t& outputH)
    : tmgr_(tmgr)
    , policy_(policy)
    , desktopRootToplevelId_(desktopRootToplevelId)
    , outputW_(outputW)
    , outputH_(outputH)
{
}

void DesktopCompositor::MarkDesktopRootDirtyLocked()
{
    tmgr_.MarkToplevelDirtyLocked(desktopRootToplevelId_);
}

void DesktopCompositor::UpdateSubsurfaceLayerLocalPosition(wl_resource* surface, int32_t x, int32_t y)
{
    for (auto& layer : subsurfaceLayers_) {
        if (layer.surface == surface) {
            layer.localX = x;
            layer.localY = y;
            return;
        }
    }
}

bool DesktopCompositor::RemoveSubsurfaceLayer(wl_resource* surface)
{
    auto it = std::find_if(subsurfaceLayers_.begin(), subsurfaceLayers_.end(),
                           [surface](const SubsurfaceLayer& l) { return l.surface == surface; });
    if (it == subsurfaceLayers_.end()) return false;
    subsurfaceLayers_.erase(it);
    return true;
}

std::vector<uint8_t> DesktopCompositor::UpsertSubsurfaceLayer(
    SubsurfaceLayer&& layer, std::vector<uint8_t>&& newPixels)
{
    for (auto& l : subsurfaceLayers_) {
        if (l.surface == layer.surface) {
            auto oldPixels = std::move(l.pixels);
            l = std::move(layer);
            l.pixels = std::move(newPixels);
            return oldPixels;
        }
    }
    layer.pixels = std::move(newPixels);
    subsurfaceLayers_.push_back(std::move(layer));
    return {};
}

bool DesktopCompositor::ReorderSubsurfaceLayerAbove(wl_resource* child, wl_resource* sibling)
{
    int myIdx = -1, siblingIdx = -1;
    for (size_t i = 0; i < subsurfaceLayers_.size(); i++) {
        if (subsurfaceLayers_[i].surface == child) myIdx = static_cast<int>(i);
        if (subsurfaceLayers_[i].surface == sibling) siblingIdx = static_cast<int>(i);
    }
    if (myIdx < 0 || siblingIdx < 0 || myIdx == siblingIdx + 1) return false;
    int target = siblingIdx;
    if (myIdx < target) target--;
    auto layer = std::move(subsurfaceLayers_[myIdx]);
    subsurfaceLayers_.erase(subsurfaceLayers_.begin() + myIdx);
    subsurfaceLayers_.insert(subsurfaceLayers_.begin() + target + 1, std::move(layer));
    return true;
}

bool DesktopCompositor::ReorderSubsurfaceLayerBelow(wl_resource* child, wl_resource* sibling)
{
    int myIdx = -1, siblingIdx = -1;
    for (size_t i = 0; i < subsurfaceLayers_.size(); i++) {
        if (subsurfaceLayers_[i].surface == child) myIdx = static_cast<int>(i);
        if (subsurfaceLayers_[i].surface == sibling) siblingIdx = static_cast<int>(i);
    }
    if (myIdx < 0 || siblingIdx < 0 || myIdx == siblingIdx - 1) return false;
    int target = siblingIdx;
    if (myIdx > target) target++;
    auto layer = std::move(subsurfaceLayers_[myIdx]);
    subsurfaceLayers_.erase(subsurfaceLayers_.begin() + myIdx);
    subsurfaceLayers_.insert(subsurfaceLayers_.begin() + target, std::move(layer));
    return true;
}

void DesktopCompositor::RemoveZeroCopyKeyLocked(uint64_t surfaceKey)
{
    zeroCopySurfaceKeys_.erase(surfaceKey);
    zeroCopyProtocolGeometryLogged_.erase(surfaceKey);
}

bool DesktopCompositor::HasZeroCopyLayerForToplevelLocked(uint32_t id) const
{
    for (const auto& layer : subsurfaceLayers_)
        if (layer.parentToplevel == id && zeroCopySurfaceKeys_.count(layer.surfaceKey))
            return true;
    return false;
}

std::vector<DesktopCompositor::CompositorLayer> DesktopCompositor::BuildLayerListLocked(int rootW, int rootH)
{
    std::vector<CompositorLayer> layers;
    const uint32_t rootId = desktopRootToplevelId_;
    size_t zIndex = 0;

    {
        CompositorLayer rootLayer;
        rootLayer.type = CompositorLayer::Type::Root;
        rootLayer.zIndex = zIndex++;
        rootLayer.visible = true;
        rootLayer.w = rootW;
        rootLayer.h = rootH;
        layers.push_back(std::move(rootLayer));
    }

    // toplevel 层 (z-order 升序): root 由 Root 层表示, 不在 z-order 里重复。
    // 可见性判定与原合成/输入循环同源 (IsToplevelVisibleLocked)。
    for (uint32_t childId : tmgr_.toplevelZOrder()) {
        if (childId == rootId) continue;
        const auto* cst = tmgr_.FindToplevelLocked(childId);
        if (!cst) continue;
        CompositorLayer layer;
        layer.type = CompositorLayer::Type::Toplevel;
        layer.zIndex = zIndex++;
        layer.visible = tmgr_.IsToplevelVisibleLocked(childId, rootId);
        layer.toplevelId = childId;
        layer.x = cst->x;
        layer.y = cst->y;
        layer.w = cst->w;
        layer.h = cst->h;
        layer.fullscreen = cst->fullscreen;
        layers.push_back(std::move(layer));
    }

    // subsurface 层 (原顺序, 全部在 toplevel 之后): 与旧合成双循环
    // (toplevel 段先、subsurface 段后) 顺序等价。位置已 Resolve 为桌面
    // 坐标; zcLayer 由 zeroCopySurfaceKeys_ 派生 (阶段 1 仍由合成/输入跳过)。
    for (const auto& sl : subsurfaceLayers_) {
        int lx = 0, ly = 0;
        ResolveSubsurfaceLayerPositionLocked(sl, lx, ly);
        CompositorLayer layer;
        layer.type = CompositorLayer::Type::Subsurface;
        layer.zIndex = zIndex++;
        layer.visible = (sl.parentToplevel == rootId) ||
                        tmgr_.IsToplevelVisibleLocked(sl.parentToplevel, rootId);
        layer.zcLayer = zeroCopySurfaceKeys_.count(sl.surfaceKey) > 0;
        layer.toplevelId = sl.parentToplevel;
        layer.x = lx;
        layer.y = ly;
        layer.w = sl.w;
        layer.h = sl.h;
        layer.sub = &sl;
        layers.push_back(std::move(layer));
    }
    return layers;
}

void DesktopCompositor::ResolveSubsurfaceLayerPositionLocked(
    const SubsurfaceLayer& layer, int& x, int& y) const
{
    x = layer.x;
    y = layer.y;
    if (layer.isExternal) return;

    const auto it = tmgr_.toplevels().find(layer.parentToplevel);
    if (it != tmgr_.toplevels().end() && it->second.hasPosition) {
        x = it->second.x + layer.localX;
        y = it->second.y + layer.localY;
    }
}

bool DesktopCompositor::GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                                             int fallbackWidth, int fallbackHeight,
                                             ZeroCopyLayerInfo& info)
{
    auto lk = tmgr_.Lock();
    auto* wlRes = tmgr_.FindSurfaceResource(surfaceKey);
    if (!wlRes) return false;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(wlRes));
    if (!sd) return false;

    info = {};
    info.surfaceKey = surfaceKey;
    info.clientPid = sd->clientPid;
    info.surfaceId = sd->protocolId;
    if (sd->isSubsurface && sd->parentSurface)
    {
        auto* parent = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
        if (!parent || !parent->hasToplevel) return false;
        info.parentToplevel = parent->toplevelId;
        info.width = sd->vpDstW > 0 ? sd->vpDstW : sd->w;
        info.height = sd->vpDstH > 0 ? sd->vpDstH : sd->h;
        if (policy_.RootCompositing())
        {
            if (rendererToplevelId != desktopRootToplevelId_ ||
                (info.parentToplevel != desktopRootToplevelId_ &&
                 !tmgr_.IsToplevelVisibleLocked(info.parentToplevel, desktopRootToplevelId_)))
                return false;
            for (const auto& layer : subsurfaceLayers_)
            {
                if (layer.surface != wlRes) continue;
                ResolveSubsurfaceLayerPositionLocked(layer, info.x, info.y);
                info.width = layer.vpDstW > 0 ? layer.vpDstW : layer.w;
                info.height = layer.vpDstH > 0 ? layer.vpDstH : layer.h;
                info.shmCommitSerial = layer.shmCommitSerial;
                info.desktopCoordinates = true;
                if (const auto* pst = tmgr_.FindToplevelLocked(layer.parentToplevel))
                    info.fullscreen = pst->fullscreen;
                info.protocolOnly = false;
                return info.width > 0 && info.height > 0;
            }

            // Vulkan private-present surfaces may have no wl_shm commit. Wayland
            // still supplies the parent/offset while the present protocol supplies
            // the image dimensions.
            int sx = sd->subsurfaceX;
            int sy = sd->subsurfaceY;
            const auto* parentState = tmgr_.FindToplevelLocked(info.parentToplevel);
            if (parentState && parentState->minimized) {
                if (sx > 16000) sx -= 32000;
                if (sy > 16000) sy -= 32000;
            }
            const int compX = parentState ? parentState->x : 0;
            const int compY = parentState ? parentState->y : 0;
            const int wineX = parentState ? parentState->wineX : 0;
            const int wineY = parentState ? parentState->wineY : 0;
            const int compW = parentState ? parentState->w : 0;
            const int compH = parentState ? parentState->h : 0;
            const bool insideWin = sx >= 0 && sx < compW && sy >= 0 && sy < compH;
            info.x = (insideWin ? compX : wineX) + sx;
            info.y = (insideWin ? compY : wineY) + sy;
            info.width = sd->vpDstW > 0 ? sd->vpDstW : sd->w;
            info.height = sd->vpDstH > 0 ? sd->vpDstH : sd->h;
            if (info.width <= 0) info.width = fallbackWidth;
            if (info.height <= 0) info.height = fallbackHeight;
            info.shmCommitSerial = sd->shmCommitSerial.load(std::memory_order_acquire);
            info.desktopCoordinates = true;
            info.protocolOnly = true;
            if (parentState) info.fullscreen = parentState->fullscreen;
            if (zeroCopyProtocolGeometryLogged_.insert(surfaceKey).second) {
                OH_LOG_INFO(LOG_APP,
                            "[MW-ZC] protocol-only geometry key=%{public}llu "
                            "pid=%{public}u surface=%{public}u parent=%{public}u "
                            "offset=%{public}d,%{public}d layer=%{public}dx%{public}d "
                            "fallback=%{public}dx%{public}d",
                            static_cast<unsigned long long>(surfaceKey), info.clientPid,
                            info.surfaceId, info.parentToplevel, sx, sy, info.width,
                            info.height, fallbackWidth, fallbackHeight);
            }
            return info.width > 0 && info.height > 0;
        }

        if (rendererToplevelId != info.parentToplevel) return false;
        info.x = sd->subsurfaceX - parent->geoX;
        info.y = sd->subsurfaceY - parent->geoY;
        info.shmCommitSerial = sd->shmCommitSerial.load(std::memory_order_acquire);
        return info.width > 0 && info.height > 0;
    }

    if (!sd->hasToplevel) return false;
    info.parentToplevel = sd->toplevelId;
    info.width = sd->w;
    info.height = sd->h;
    if (info.width <= 0) info.width = fallbackWidth;
    if (info.height <= 0) info.height = fallbackHeight;
    info.shmCommitSerial = sd->shmCommitSerial.load(std::memory_order_acquire);
    if (policy_.RootCompositing())
    {
        if (rendererToplevelId != desktopRootToplevelId_ ||
            (sd->toplevelId != desktopRootToplevelId_ && !tmgr_.IsToplevelVisibleLocked(sd->toplevelId, desktopRootToplevelId_)))
            return false;
        if (const auto* st = tmgr_.FindToplevelLocked(sd->toplevelId)) {
            info.x = st->x;
            info.y = st->y;
            info.fullscreen = st->fullscreen;
        }
        info.desktopCoordinates = true;
        return info.width > 0 && info.height > 0;
    }
    return rendererToplevelId == sd->toplevelId && info.width > 0 && info.height > 0;
}

void DesktopCompositor::SetSurfaceZeroCopy(uint64_t surfaceKey, bool enabled)
{
    if (!surfaceKey) return;
    auto lk = tmgr_.Lock();
    if (enabled)
        zeroCopySurfaceKeys_.insert(surfaceKey);
    else
        zeroCopySurfaceKeys_.erase(surfaceKey);
    MarkDesktopRootDirtyLocked();
    desktopCompositionSignature_ = 0;
}

int DesktopCompositor::GetZeroCopyOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                                            ZeroCopyOccluderRect* out, int maxOut)
{
    if (!out || maxOut <= 0) return 0;
    ZeroCopyLayerInfo info;
    if (!GetZeroCopyLayerInfo(surfaceKey, rendererToplevelId, 0, 0, info) ||
        !info.desktopCoordinates)
        return 0;

    auto lk = tmgr_.Lock();
    const int layerL = info.x;
    const int layerT = info.y;
    const int layerR = info.x + info.width;
    const int layerB = info.y + info.height;
    const auto* rootSt = tmgr_.FindToplevelLocked(desktopRootToplevelId_);
    if (!rootSt) return 0;
    const int rootW = rootSt->w;
    const int rootH = rootSt->h;
    int count = 0;
    auto pushRect = [&](int x, int y, int w, int h) {
        if (count >= maxOut || w <= 0 || h <= 0) return;
        const int l = std::max({x, layerL, 0});
        const int t = std::max({y, layerT, 0});
        const int r = std::min({x + w, layerR, rootW});
        const int b = std::min({y + h, layerB, rootH});
        if (r <= l || b <= t) return;
        out[count++] = {l, t, r - l, b - t};
    };

    auto zbegin = tmgr_.toplevelZOrder().begin();
    if (info.parentToplevel != desktopRootToplevelId_) {
        const auto zit = std::find(tmgr_.toplevelZOrder().begin(), tmgr_.toplevelZOrder().end(),
                                   info.parentToplevel);
        if (zit != tmgr_.toplevelZOrder().end()) zbegin = std::next(zit);
    }
    for (auto zit = zbegin; zit != tmgr_.toplevelZOrder().end() && count < maxOut; ++zit) {
        const uint32_t cid = *zit;
        if (!tmgr_.IsToplevelVisibleLocked(cid, desktopRootToplevelId_)) continue;
        const auto* cst = tmgr_.FindToplevelLocked(cid);
        if (!cst) continue;
        if (cst->fullscreen) pushRect(0, 0, rootW, rootH);
        else pushRect(cst->x, cst->y, cst->w, cst->h);
    }

    for (const auto& layer : subsurfaceLayers_) {
        if (count >= maxOut) break;
        if (zeroCopySurfaceKeys_.count(layer.surfaceKey)) continue;
        if (layer.parentToplevel != desktopRootToplevelId_ &&
            !tmgr_.IsToplevelVisibleLocked(layer.parentToplevel, desktopRootToplevelId_)) continue;
        int x = 0, y = 0;
        ResolveSubsurfaceLayerPositionLocked(layer, x, y);
        pushRect(x, y,
                 layer.vpDstW > 0 ? layer.vpDstW : layer.w,
                 layer.vpDstH > 0 ? layer.vpDstH : layer.h);
    }
    return count;
}

// ============================================================================
// TakeToplevelFrame: 桌面合成核心 (~390 行)
// ============================================================================

bool DesktopCompositor::TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, int& w, int& h) {
    struct TakeBreakdownWindow {
        uint64_t count = 0;
        uint64_t sums[6] = {};
        uint64_t maxima[6] = {};

        void Add(uint64_t lockWait, uint64_t rootCopy, uint64_t children,
                 uint64_t subsurfaces, uint64_t output, uint64_t total) {
            const uint64_t values[6] = {lockWait, rootCopy, children, subsurfaces, output, total};
            for (size_t i = 0; i < 6; ++i) {
                sums[i] += values[i];
                maxima[i] = std::max(maxima[i], values[i]);
            }
            if (++count != 120) return;
            OH_LOG_INFO(LOG_APP,
                        "[GL-TAKE] samples=120 avg_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                        "max_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu",
                        static_cast<unsigned long long>(sums[0] / count),
                        static_cast<unsigned long long>(sums[1] / count),
                        static_cast<unsigned long long>(sums[2] / count),
                        static_cast<unsigned long long>(sums[3] / count),
                        static_cast<unsigned long long>(sums[4] / count),
                        static_cast<unsigned long long>(sums[5] / count),
                        static_cast<unsigned long long>(maxima[0]),
                        static_cast<unsigned long long>(maxima[1]),
                        static_cast<unsigned long long>(maxima[2]),
                        static_cast<unsigned long long>(maxima[3]),
                        static_cast<unsigned long long>(maxima[4]),
                        static_cast<unsigned long long>(maxima[5]));
            count = 0;
            for (size_t i = 0; i < 6; ++i) {
                sums[i] = 0;
                maxima[i] = 0;
            }
        }
    };
    static TakeBreakdownWindow breakdown;

    using TakeClock = std::chrono::steady_clock;
    const auto takeStarted = TakeClock::now();
    auto lk = tmgr_.Lock();
    const auto lockAcquired = TakeClock::now();

    if (policy_.RootCompositing() && id == desktopRootToplevelId_) {
        auto* rst = tmgr_.FindToplevelLocked(id);
        if (!rst || !ToplevelManager::HasFrame(*rst)) return false;
        if (!rst->dirty) return false;

        int rootW = rst->w;
        int rootH = rst->h;

        bool hasChildren = false;
        for (uint32_t cid : tmgr_.toplevelZOrder()) {
            if (cid == id) continue;
            const auto* cst = tmgr_.FindToplevelLocked(cid);
            if (cst && ToplevelManager::HasFrame(*cst)) {
                hasChildren = true;
                break;
            }
        }
        if (!hasChildren && subsurfaceLayers_.empty()) {
            out = rst->pixels;
            w = rootW;
            h = rootH;
            rst->dirty = false;
            return true;
        }

        // 层序单一数据源 (阶段 1): 一帧全部内容来源的层列表, 构建一次,
        // fs-pick / 覆盖判定 / 签名 / 合成共用。zIndex: root < toplevel <
        // subsurface — 顺序与旧双循环等价 (见 CompositorLayer 注释)。
        const auto layers = BuildLayerListLocked(rootW, rootH);

        uint32_t fullscreenId = 0;
        FitRect transform;
        bool hasFullscreen = false;
        // ZC 游戏 (画面在 zero-copy GL 层): 全屏独占输出, 见下方填黑分支
        bool isZcGame = false;
        int fullscreenX = 0, fullscreenY = 0;
        // 全屏选取与输入侧 (FindInputTargetAt) 同规则: 可见全屏窗口中取
        // fsPriority 最大者 — 多窗口可同时 fullscreen (显示模式切换时 Wine
        // 会把足够大的旧窗口连带标记, 请求到达顺序不定), 规则原因/局限见
        // ToplevelState::fsPriority 注释
        const ToplevelManager::ToplevelState* fsWin = nullptr;
        for (const auto& layer : layers) {
            if (layer.type != CompositorLayer::Type::Toplevel || !layer.visible || !layer.fullscreen) continue;
            const auto* zst = tmgr_.FindToplevelLocked(layer.toplevelId);
            if (!zst) continue;
            if (!fsWin || zst->fsPriority > fsWin->fsPriority) { fsWin = zst; fullscreenId = layer.toplevelId; }
        }
        if (fsWin) {
            fullscreenX = fsWin->x;
            fullscreenY = fsWin->y;
            hasFullscreen = ComputeFitRect(rootW, rootH, fsWin->w, fsWin->h, transform);
            isZcGame = HasZeroCopyLayerForToplevelLocked(fullscreenId);
        }

        bool fullscreenContentCovered = false;
        if (hasFullscreen) {
            const auto* fst = tmgr_.FindToplevelLocked(fullscreenId);
            const int winW = fst ? fst->w : 0;
            const int winH = fst ? fst->h : 0;
            for (const auto& layer : layers) {
                if (layer.type != CompositorLayer::Type::Subsurface) continue;
                if (layer.toplevelId != fullscreenId) continue;
                if (layer.zcLayer) continue;
                const auto& sl = *layer.sub;
                if (sl.w <= 0 || sl.h <= 0) continue;
                if (sl.shmFormat == 0 && !sl.opaque) continue;
                const int dispW = sl.vpDstW > 0 ? std::min(sl.vpDstW, sl.w) : sl.w;
                const int dispH = sl.vpDstH > 0 ? std::min(sl.vpDstH, sl.h) : sl.h;
                const int relX = layer.x - fullscreenX;
                const int relY = layer.y - fullscreenY;
                if (relX <= 0 && relY <= 0 &&
                    relX + dispW >= winW && relY + dispH >= winH) {
                    fullscreenContentCovered = true;
                    break;
                }
            }
        }

        uint64_t compositionSignature = compositor_consts::kFnv1aOffsetBasis;
        auto mixSignature = [&](uint64_t value) {
            compositionSignature ^= value;
            compositionSignature *= compositor_consts::kFnv1aPrime;
        };
        mixSignature(id);
        mixSignature(static_cast<uint32_t>(rootW));
        mixSignature(static_cast<uint32_t>(rootH));
        // 签名遍历 Layer 列表: 每个可见 toplevel/subsurface 的几何与标记
        // (与旧两个循环 mix 序列等价; 不可见 toplevel 的 (id,0) 不再混入,
        // 仅影响 rebuildBase 触发时机, 不影响输出像素 — 不可见窗口不参与
        // 合成, root 像素变化仍由 desktopRootFrameSerial_ 兜底)。
        for (const auto& layer : layers) {
            if (layer.type == CompositorLayer::Type::Toplevel) {
                mixSignature(layer.toplevelId);
                mixSignature(layer.visible ? 1 : 0);
                if (!layer.visible) continue;
                mixSignature(static_cast<uint32_t>(layer.x));
                mixSignature(static_cast<uint32_t>(layer.y));
                mixSignature(static_cast<uint32_t>(layer.w));
                mixSignature(static_cast<uint32_t>(layer.h));
                mixSignature(layer.fullscreen ? 1 : 0);
            } else if (layer.type == CompositorLayer::Type::Subsurface) {
                mixSignature(reinterpret_cast<uintptr_t>(layer.sub->surface));
                mixSignature(layer.zcLayer ? 1 : 0);
                mixSignature(layer.toplevelId);
                mixSignature(layer.visible ? 1 : 0);
                mixSignature(static_cast<uint32_t>(layer.x));
                mixSignature(static_cast<uint32_t>(layer.y));
                mixSignature(static_cast<uint32_t>(layer.w));
                mixSignature(static_cast<uint32_t>(layer.h));
                mixSignature(static_cast<uint32_t>(layer.sub->vpDstW));
                mixSignature(static_cast<uint32_t>(layer.sub->vpDstH));
            }
        }

        const size_t rootBytes = static_cast<size_t>(rootW) * rootH * 4;
        const bool rebuildBase = !desktopOutputInitialized_ ||
            out.size() != rootBytes ||
            desktopOutputRootFrameSerial_ != desktopRootFrameSerial_ ||
            desktopCompositionSignature_ != compositionSignature;
        if (rebuildBase) {
            out = rst->pixels;
            desktopOutputInitialized_ = true;
            desktopOutputRootFrameSerial_ = desktopRootFrameSerial_;
            desktopCompositionSignature_ = compositionSignature;
        }
        auto& composited = out;
        const auto rootCopied = TakeClock::now();

        // 合成单循环 (阶段 1): 按 zIndex 升序遍历 Layer 列表 — 等价旧
        // toplevel 循环 + subsurface 循环的两段顺序 (Layer zIndex 分配保证)。
        // 全屏独占/跳过特判原样保留 (等价形式), 行为不变。
        auto blitToplevel = [&](const CompositorLayer& layer) {
            if (!layer.visible) return;
            // 跳过非主全屏的 toplevel: ZC 游戏独占输出 (其它 toplevel 的
            // SHM 内容不是游戏画面, 画上会在黑边区残留杂色); SHM 游戏只跳过
            // 被连带标 fullscreen 的旧窗口 (notepad/explorer 等, 显示模式
            // 切换时 winewayland 批量标记, fsPriority 选了游戏但它仍在
            // z-order 高位, 普通 blit 会盖在游戏上面), 非全屏弹窗/对话框保留
            if (hasFullscreen && layer.toplevelId != fullscreenId) {
                if (isZcGame) return;
                if (layer.fullscreen) return;
            }
            auto* cst = tmgr_.FindToplevelLocked(layer.toplevelId);
            if (!cst) return;
            auto& childPx = cst->pixels;
            int childW = cst->w;
            int childH = cst->h;
            int posX = cst->x;
            int posY = cst->y;
            if (layer.toplevelId == fullscreenId && hasFullscreen) {
                if (isZcGame) {
                    // ZC 游戏: 整幅填黑, 跳过 SHM BlitScaled — 其 SHM 内容是
                    // explorer 桌面而非游戏画面, 实际画面由 GL ZC 层渲染
                    // (egl_renderer zeroCopyFullscreen_ 路径)。
                    // 必须填不透明黑 0xFF000000, 不能图省事 memset 0:
                    // 渲染 context 不开 GL_BLEND 时 alpha=0 恰好无害, 但那是
                    // 隐式依赖 — 一旦以后给桌面纹理开混合, 黑边就会变透明
                    std::fill_n(reinterpret_cast<uint32_t*>(composited.data()),
                                composited.size() / 4, 0xFF000000u);
                    return;
                }
                auto fillBlackRect = [&](int fx, int fy, int fw, int fh) {
                    if (fw <= 0 || fh <= 0) return;
                    for (int row = fy; row < fy + fh; ++row)
                        std::fill_n(reinterpret_cast<uint32_t*>(composited.data()) +
                                    static_cast<size_t>(row) * rootW + fx, fw, 0xFF000000u);
                };
                const bool contentOpaque = (cst->shmFormat != 0) || fullscreenContentCovered;
                if (contentOpaque) {
                    fillBlackRect(0, 0, rootW, transform.offY);
                    fillBlackRect(0, transform.offY + transform.dstH, rootW,
                                  rootH - transform.offY - transform.dstH);
                    fillBlackRect(0, transform.offY, transform.offX, transform.dstH);
                    fillBlackRect(transform.offX + transform.dstW, transform.offY,
                                  rootW - transform.offX - transform.dstW, transform.dstH);
                } else {
                    std::fill_n(reinterpret_cast<uint32_t*>(composited.data()),
                                composited.size() / 4, 0xFF000000u);
                }
                if (!fullscreenContentCovered) {
                    BlitScaled(composited.data(), rootW, rootH,
                               childPx.data(), childW, childW, childH,
                               transform.offX, transform.offY, transform.dstW, transform.dstH,
                               cst->shmFormat == 0);
                }
                return;
            }
            int dstX = (posX > 0) ? posX : 0;
            int dstY = (posY > 0) ? posY : 0;
            int srcX = (posX < 0) ? -posX : 0;
            int srcY = (posY < 0) ? -posY : 0;
            int copyW = childW - srcX;
            int copyH = childH - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) return;
            const bool childArgb = (cst->shmFormat == 0);
            for (int y = 0; y < copyH; y++) {
                auto* srcRow = &childPx[(srcY + y) * childW * 4];
                auto* dstRow = &composited[(dstY + y) * rootW * 4];
                if (!childArgb) {
                    memcpy(&dstRow[dstX * 4], &srcRow[srcX * 4], copyW * 4);
                    continue;
                }
                for (int x = 0; x < copyW; x++) {
                    const uint8_t* sp = srcRow + (srcX + x) * 4;
                    uint8_t* dp = dstRow + (dstX + x) * 4;
                    uint8_t a = sp[3];
                    if (a == 0) continue;
                    if (a == 255) {
                        memcpy(dp, sp, 4);
                    } else {
                        unsigned inv = 255 - a;
                        unsigned b = sp[0] + (dp[0] * inv) / 255;
                        unsigned g = sp[1] + (dp[1] * inv) / 255;
                        unsigned r = sp[2] + (dp[2] * inv) / 255;
                        dp[0] = b > 255 ? 255 : b;
                        dp[1] = g > 255 ? 255 : g;
                        dp[2] = r > 255 ? 255 : r;
                        dp[3] = 255;
                    }
                }
            }
        };
        auto blitSubsurface = [&](const CompositorLayer& layer) {
            if (layer.zcLayer) return;
            if (!layer.visible) return;
            if (layer.w <= 0 || layer.h <= 0) return;
            if (hasFullscreen && layer.toplevelId != fullscreenId) return;
            const auto& sl = *layer.sub;
            int layerX = layer.x;
            int layerY = layer.y;
            size_t expectSz = (size_t)sl.w * sl.h * 4;
            if (sl.pixels.size() < expectSz) {
                OH_LOG_WARN(LOG_APP, "[MW-SUBSURF] layer size mismatch: w=%{public}d h=%{public}d px=%{public}zu expected=%{public}zu",
                            sl.w, sl.h, sl.pixels.size(), expectSz);
                return;
            }
            if (hasFullscreen && layer.toplevelId == fullscreenId) {
                const int layerDispW = sl.vpDstW > 0 ? std::min(sl.vpDstW, sl.w) : sl.w;
                const int layerDispH = sl.vpDstH > 0 ? std::min(sl.vpDstH, sl.h) : sl.h;
                const int layerDstX = transform.offX + static_cast<int>(lround((layerX - fullscreenX) * transform.scale));
                const int layerDstY = transform.offY + static_cast<int>(lround((layerY - fullscreenY) * transform.scale));
                const int layerDstW = std::max(1, static_cast<int>(lround(layerDispW * transform.scale)));
                const int layerDstH = std::max(1, static_cast<int>(lround(layerDispH * transform.scale)));
                BlitScaled(composited.data(), rootW, rootH,
                           sl.pixels.data(), sl.w, layerDispW, layerDispH,
                           layerDstX, layerDstY, layerDstW, layerDstH,
                           sl.shmFormat == 0 && !sl.opaque);
                return;
            }
            int srcX = (layerX < 0) ? -layerX : 0;
            int srcY = (layerY < 0) ? -layerY : 0;
            int dstX = (layerX > 0) ? layerX : 0;
            int dstY = (layerY > 0) ? layerY : 0;
            int copyW = sl.w - srcX;
            int copyH = sl.h - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) return;
            int renderW = copyW, renderH = copyH;
            int renderSrcX = srcX, renderSrcY = srcY;
            int renderDstX = dstX, renderDstY = dstY;
            if (sl.vpDstW > 0 && sl.vpDstW < copyW) renderW = sl.vpDstW;
            if (sl.vpDstH > 0 && sl.vpDstH < copyH) renderH = sl.vpDstH;
            if (sl.dmgW > 0 && sl.dmgH > 0) {
                const int damageLeft = std::max(renderSrcX, sl.dmgX);
                const int damageTop = std::max(renderSrcY, sl.dmgY);
                const int damageRight = std::min(renderSrcX + renderW, sl.dmgX + sl.dmgW);
                const int damageBottom = std::min(renderSrcY + renderH, sl.dmgY + sl.dmgH);
                if (damageRight <= damageLeft || damageBottom <= damageTop) return;
                renderDstX += damageLeft - renderSrcX;
                renderDstY += damageTop - renderSrcY;
                renderSrcX = damageLeft;
                renderSrcY = damageTop;
                renderW = damageRight - damageLeft;
                renderH = damageBottom - damageTop;
            }
            const bool needsAlphaBlend = sl.shmFormat == 0 && !sl.opaque;
            for (int y = 0; y < renderH; y++) {
                const uint8_t* srcRow = sl.pixels.data() +
                    ((renderSrcY + y) * sl.w + renderSrcX) * 4;
                uint8_t* dstRow = composited.data() +
                    ((renderDstY + y) * rootW + renderDstX) * 4;
                if (!needsAlphaBlend) {
                    std::memcpy(dstRow, srcRow, static_cast<size_t>(renderW) * 4);
                    continue;
                }
                for (int x = 0; x < renderW; x++) {
                    const uint8_t* srcPixel = srcRow + x * 4;
                    uint8_t* dstPixel = dstRow + x * 4;
                    uint8_t a = srcPixel[3];
                    if (a == 0) continue;
                    if (a == 255) {
                        std::memcpy(dstPixel, srcPixel, 4);
                    } else {
                        unsigned inv = 255 - a;
                        dstPixel[0] = (srcPixel[0] * a + dstPixel[0] * inv) / 255;
                        dstPixel[1] = (srcPixel[1] * a + dstPixel[1] * inv) / 255;
                        dstPixel[2] = (srcPixel[2] * a + dstPixel[2] * inv) / 255;
                    }
                }
            }
        };
        for (const auto& layer : layers) {
            switch (layer.type) {
                case CompositorLayer::Type::Root:
                    break;  // 基底已在 rebuildBase 时拷贝
                case CompositorLayer::Type::Toplevel:
                    blitToplevel(layer);
                    break;
                case CompositorLayer::Type::Subsurface:
                    blitSubsurface(layer);
                    break;
            }
        }
        const auto childrenComposited = TakeClock::now();
        // 旧双循环有两个分段时间点; 单循环后合并为一个
        const auto subsurfacesComposited = childrenComposited;

        const auto outputMoved = TakeClock::now();
        auto elapsedUs = [](TakeClock::time_point begin, TakeClock::time_point end) {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin).count());
        };
        breakdown.Add(elapsedUs(takeStarted, lockAcquired),
                      elapsedUs(lockAcquired, rootCopied),
                      elapsedUs(rootCopied, childrenComposited),
                      elapsedUs(childrenComposited, subsurfacesComposited),
                      elapsedUs(subsurfacesComposited, outputMoved),
                      elapsedUs(takeStarted, outputMoved));
        w = rootW;
        h = rootH;
        rst->dirty = false;
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] root #%{public}u %{public}dx%{public}d children=%{public}zu subsurfaces=%{public}zu",
                    id, w, h, tmgr_.toplevelZOrder().size(), subsurfaceLayers_.size());
        return true;
    }

    auto* st = tmgr_.FindToplevelLocked(id);
    if (!st || !st->dirty) return false;
    out = st->pixels;
    w = st->w;
    h = st->h;
    st->dirty = false;
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu", id, w, h, out.size());
    return true;
}
