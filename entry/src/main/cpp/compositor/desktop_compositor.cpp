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

void DesktopCompositor::ReorderSubsurfaceLayerAbove(wl_resource* child, wl_resource* sibling)
{
    int myIdx = -1, siblingIdx = -1;
    for (size_t i = 0; i < subsurfaceLayers_.size(); i++) {
        if (subsurfaceLayers_[i].surface == child) myIdx = static_cast<int>(i);
        if (subsurfaceLayers_[i].surface == sibling) siblingIdx = static_cast<int>(i);
    }
    if (myIdx < 0 || siblingIdx < 0 || myIdx == siblingIdx + 1) return;
    int target = siblingIdx;
    if (myIdx < target) target--;
    auto layer = std::move(subsurfaceLayers_[myIdx]);
    subsurfaceLayers_.erase(subsurfaceLayers_.begin() + myIdx);
    subsurfaceLayers_.insert(subsurfaceLayers_.begin() + target + 1, std::move(layer));
}

void DesktopCompositor::ReorderSubsurfaceLayerBelow(wl_resource* child, wl_resource* sibling)
{
    int myIdx = -1, siblingIdx = -1;
    for (size_t i = 0; i < subsurfaceLayers_.size(); i++) {
        if (subsurfaceLayers_[i].surface == child) myIdx = static_cast<int>(i);
        if (subsurfaceLayers_[i].surface == sibling) siblingIdx = static_cast<int>(i);
    }
    if (myIdx < 0 || siblingIdx < 0 || myIdx == siblingIdx - 1) return;
    int target = siblingIdx;
    if (myIdx > target) target++;
    auto layer = std::move(subsurfaceLayers_[myIdx]);
    subsurfaceLayers_.erase(subsurfaceLayers_.begin() + myIdx);
    subsurfaceLayers_.insert(subsurfaceLayers_.begin() + target, std::move(layer));
}

void DesktopCompositor::RemoveZeroCopyKeyLocked(uint64_t surfaceKey)
{
    zeroCopySurfaceKeys_.erase(surfaceKey);
}

bool DesktopCompositor::HasZeroCopyLayerForToplevelLocked(uint32_t id) const
{
    for (const auto& layer : subsurfaceLayers_)
        if (layer.parentToplevel == id && zeroCopySurfaceKeys_.count(layer.surfaceKey))
            return true;
    return false;
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
                return info.width > 0 && info.height > 0;
            }
            return false;
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
    if (!GetZeroCopyLayerInfo(surfaceKey, rendererToplevelId, info) ||
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
        for (uint32_t childId : tmgr_.toplevelZOrder()) {
            const auto* zst = tmgr_.FindToplevelLocked(childId);
            if (!zst || !zst->fullscreen || !tmgr_.IsToplevelVisibleLocked(childId, desktopRootToplevelId_)) continue;
            if (!fsWin || zst->fsPriority > fsWin->fsPriority) { fsWin = zst; fullscreenId = childId; }
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
            for (const auto& layer : subsurfaceLayers_) {
                if (layer.parentToplevel != fullscreenId) continue;
                if (zeroCopySurfaceKeys_.count(layer.surfaceKey)) continue;
                if (layer.w <= 0 || layer.h <= 0) continue;
                if (layer.shmFormat == 0 && !layer.opaque) continue;
                int layerX = 0, layerY = 0;
                ResolveSubsurfaceLayerPositionLocked(layer, layerX, layerY);
                const int dispW = layer.vpDstW > 0 ? std::min(layer.vpDstW, layer.w) : layer.w;
                const int dispH = layer.vpDstH > 0 ? std::min(layer.vpDstH, layer.h) : layer.h;
                const int relX = layerX - fullscreenX;
                const int relY = layerY - fullscreenY;
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
        for (uint32_t childId : tmgr_.toplevelZOrder()) {
            mixSignature(childId);
            const bool visible = tmgr_.IsToplevelVisibleLocked(childId, desktopRootToplevelId_);
            mixSignature(visible ? 1 : 0);
            if (!visible) continue;
            const auto* cst = tmgr_.FindToplevelLocked(childId);
            if (!cst) continue;
            mixSignature(static_cast<uint32_t>(cst->x));
            mixSignature(static_cast<uint32_t>(cst->y));
            mixSignature(static_cast<uint32_t>(cst->w));
            mixSignature(static_cast<uint32_t>(cst->h));
            mixSignature(cst->fullscreen ? 1 : 0);
        }
        for (const auto& layer : subsurfaceLayers_) {
            int layerX = 0, layerY = 0;
            ResolveSubsurfaceLayerPositionLocked(layer, layerX, layerY);
            mixSignature(reinterpret_cast<uintptr_t>(layer.surface));
            mixSignature(zeroCopySurfaceKeys_.count(layer.surfaceKey) ? 1 : 0);
            mixSignature(layer.parentToplevel);
            mixSignature(layer.parentToplevel == id || tmgr_.IsToplevelVisibleLocked(layer.parentToplevel, desktopRootToplevelId_));
            mixSignature(static_cast<uint32_t>(layerX));
            mixSignature(static_cast<uint32_t>(layerY));
            mixSignature(static_cast<uint32_t>(layer.w));
            mixSignature(static_cast<uint32_t>(layer.h));
            mixSignature(static_cast<uint32_t>(layer.vpDstW));
            mixSignature(static_cast<uint32_t>(layer.vpDstH));
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

        for (uint32_t childId : tmgr_.toplevelZOrder()) {
            if (!tmgr_.IsToplevelVisibleLocked(childId, desktopRootToplevelId_)) continue;
            // 跳过非主全屏的 toplevel: ZC 游戏独占输出 (其它 toplevel 的
            // SHM 内容不是游戏画面, 画上会在黑边区残留杂色); SHM 游戏只跳过
            // 被连带标 fullscreen 的旧窗口 (notepad/explorer 等, 显示模式
            // 切换时 winewayland 批量标记, fsPriority 选了游戏但它仍在
            // z-order 高位, 普通 blit 会盖在游戏上面), 非全屏弹窗/对话框保留
            if (hasFullscreen && childId != fullscreenId) {
                if (isZcGame) continue;
                auto* cst = tmgr_.FindToplevelLocked(childId);
                if (cst && cst->fullscreen) continue;
            }
            auto* cst = tmgr_.FindToplevelLocked(childId);
            if (!cst) continue;
            auto& childPx = cst->pixels;
            int childW = cst->w;
            int childH = cst->h;
            int posX = cst->x;
            int posY = cst->y;
            if (childId == fullscreenId && hasFullscreen) {
                if (isZcGame) {
                    // ZC 游戏: 整幅填黑, 跳过 SHM BlitScaled — 其 SHM 内容是
                    // explorer 桌面而非游戏画面, 实际画面由 GL ZC 层渲染
                    // (egl_renderer zeroCopyFullscreen_ 路径)。
                    // 必须填不透明黑 0xFF000000, 不能图省事 memset 0:
                    // 渲染 context 不开 GL_BLEND 时 alpha=0 恰好无害, 但那是
                    // 隐式依赖 — 一旦以后给桌面纹理开混合, 黑边就会变透明
                    std::fill_n(reinterpret_cast<uint32_t*>(composited.data()),
                                composited.size() / 4, 0xFF000000u);
                    continue;
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
                continue;
            }
            int dstX = (posX > 0) ? posX : 0;
            int dstY = (posY > 0) ? posY : 0;
            int srcX = (posX < 0) ? -posX : 0;
            int srcY = (posY < 0) ? -posY : 0;
            int copyW = childW - srcX;
            int copyH = childH - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) continue;
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
        }
        const auto childrenComposited = TakeClock::now();

        for (auto& layer : subsurfaceLayers_) {
            if (zeroCopySurfaceKeys_.count(layer.surfaceKey)) continue;
            if (layer.parentToplevel != id && !tmgr_.IsToplevelVisibleLocked(layer.parentToplevel, desktopRootToplevelId_)) continue;
            if (layer.w <= 0 || layer.h <= 0) continue;
            if (hasFullscreen && layer.parentToplevel != fullscreenId) continue;
            int layerX = 0, layerY = 0;
            ResolveSubsurfaceLayerPositionLocked(layer, layerX, layerY);
            size_t expectSz = (size_t)layer.w * layer.h * 4;
            if (layer.pixels.size() < expectSz) {
                OH_LOG_WARN(LOG_APP, "[MW-SUBSURF] layer size mismatch: w=%{public}d h=%{public}d px=%{public}zu expected=%{public}zu",
                            layer.w, layer.h, layer.pixels.size(), expectSz);
                continue;
            }
            if (hasFullscreen && layer.parentToplevel == fullscreenId) {
                const int layerDispW = layer.vpDstW > 0 ? std::min(layer.vpDstW, layer.w) : layer.w;
                const int layerDispH = layer.vpDstH > 0 ? std::min(layer.vpDstH, layer.h) : layer.h;
                const int layerDstX = transform.offX + static_cast<int>(lround((layerX - fullscreenX) * transform.scale));
                const int layerDstY = transform.offY + static_cast<int>(lround((layerY - fullscreenY) * transform.scale));
                const int layerDstW = std::max(1, static_cast<int>(lround(layerDispW * transform.scale)));
                const int layerDstH = std::max(1, static_cast<int>(lround(layerDispH * transform.scale)));
                BlitScaled(composited.data(), rootW, rootH,
                           layer.pixels.data(), layer.w, layerDispW, layerDispH,
                           layerDstX, layerDstY, layerDstW, layerDstH,
                           layer.shmFormat == 0 && !layer.opaque);
                continue;
            }
            int srcX = (layerX < 0) ? -layerX : 0;
            int srcY = (layerY < 0) ? -layerY : 0;
            int dstX = (layerX > 0) ? layerX : 0;
            int dstY = (layerY > 0) ? layerY : 0;
            int copyW = layer.w - srcX;
            int copyH = layer.h - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) continue;
            int renderW = copyW, renderH = copyH;
            int renderSrcX = srcX, renderSrcY = srcY;
            int renderDstX = dstX, renderDstY = dstY;
            if (layer.vpDstW > 0 && layer.vpDstW < copyW) renderW = layer.vpDstW;
            if (layer.vpDstH > 0 && layer.vpDstH < copyH) renderH = layer.vpDstH;
            if (layer.dmgW > 0 && layer.dmgH > 0) {
                const int damageLeft = std::max(renderSrcX, layer.dmgX);
                const int damageTop = std::max(renderSrcY, layer.dmgY);
                const int damageRight = std::min(renderSrcX + renderW, layer.dmgX + layer.dmgW);
                const int damageBottom = std::min(renderSrcY + renderH, layer.dmgY + layer.dmgH);
                if (damageRight <= damageLeft || damageBottom <= damageTop) continue;
                renderDstX += damageLeft - renderSrcX;
                renderDstY += damageTop - renderSrcY;
                renderSrcX = damageLeft;
                renderSrcY = damageTop;
                renderW = damageRight - damageLeft;
                renderH = damageBottom - damageTop;
            }
            const bool needsAlphaBlend = layer.shmFormat == 0 && !layer.opaque;
            for (int y = 0; y < renderH; y++) {
                const uint8_t* srcRow = layer.pixels.data() +
                    ((renderSrcY + y) * layer.w + renderSrcX) * 4;
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
        }
        const auto subsurfacesComposited = TakeClock::now();

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
