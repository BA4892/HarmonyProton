#include "wayland_server.h"
#include "seat.h"
#include "input_manager.h"
#include "xdg_shell.h"
#include "fps_counter.h"
#include "include/viewporter-server-protocol.h"
#include "include/xdg-shell-server-protocol.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cerrno>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace {

uint64_t MakeSurfaceKey(uint32_t clientPid, uint32_t surfaceId)
{
    return (static_cast<uint64_t>(clientPid) << 32) | surfaceId;
}

uint32_t GetWaylandClientPid(wl_client* client)
{
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    if (client) wl_client_get_credentials(client, &pid, &uid, &gid);
    return pid > 0 ? static_cast<uint32_t>(pid) : 0;
}

// 全屏变换: 保比例缩放 + 居中黑边 (letterbox)。
// Wine 全屏窗口可坚持自己的分辨率 (小于输出尺寸, 见 winewayland
// wayland_surface_config_is_compatible 对 fullscreen "任意尺寸兼容" 的注释),
// xdg-shell 约定由 compositor 负责把 surface 放大铺满输出。
// 该变换同时用于桌面合成 (正变换) 和输入坐标映射 (逆变换), 两处必须是同一份几何
struct FullscreenTransform {
    double scale = 1.0;
    int offX = 0, offY = 0;  // 内容区在 root 帧中的原点 (黑边之后)
    int dstW = 0, dstH = 0;  // 内容区缩放后尺寸
};

bool ComputeFullscreenTransform(int rootW, int rootH, int winW, int winH, FullscreenTransform& t)
{
    if (rootW <= 0 || rootH <= 0 || winW <= 0 || winH <= 0) return false;
    t.scale = std::min(rootW / static_cast<double>(winW), rootH / static_cast<double>(winH));
    t.dstW = std::max(1, static_cast<int>(lround(winW * t.scale)));
    t.dstH = std::max(1, static_cast<int>(lround(winH * t.scale)));
    t.offX = (rootW - t.dstW) / 2;
    t.offY = (rootH - t.dstH) / 2;
    return true;
}

// 双线性缩放 blit (16.16 固定点)。src/dst 均为 4 字节像素。
// srcStride = 源行跨度(像素), srcW/srcH = 参与采样的源区域
// (viewport destination 可小于 buffer 实际尺寸, 见 TakeToplevelFrame 的 vpDst clamp)。
// alphaBlend=false (XRGB): 直接覆盖并置不透明 (X 字节是垃圾, 不采样);
// alphaBlend=true (预乘 ARGB): 对预乘源直接采样后 over 混合 (wl_shm alpha 均为预乘)
void BlitScaled(uint8_t* dst, int rootW, int rootH,
                const uint8_t* src, int srcStride, int srcW, int srcH,
                int dstX, int dstY, int dstW, int dstH, bool alphaBlend)
{
    if (!dst || !src || srcStride <= 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    if (srcW > srcStride) srcW = srcStride;
    // dst 矩形裁剪到 root 帧范围 (采样起点经 (x - dstX) 回算, 裁剪不偏移内容)
    const int x0 = std::max(0, dstX), y0 = std::max(0, dstY);
    const int x1 = std::min(rootW, dstX + dstW), y1 = std::min(rootH, dstY + dstH);
    if (x1 <= x0 || y1 <= y0) return;

    // 像素中心映射 (16.16): src = (dx + 0.5) * srcW/dstW - 0.5
    const int64_t stepX = (static_cast<int64_t>(srcW) << 16) / dstW;
    const int64_t stepY = (static_cast<int64_t>(srcH) << 16) / dstH;
    const int64_t maxFx = static_cast<int64_t>(srcW - 1) << 16;
    const int64_t maxFy = static_cast<int64_t>(srcH - 1) << 16;

    // 每列采样位置/权重预计算 (行内复用)
    std::vector<int> sx0(x1 - x0), sx1(x1 - x0), wx0(x1 - x0), wx1(x1 - x0);
    for (int i = 0; i < x1 - x0; ++i) {
        int64_t fx = static_cast<int64_t>(x0 + i - dstX) * stepX + (stepX >> 1) - (1 << 15);
        fx = std::max<int64_t>(0, std::min(maxFx, fx));
        sx0[i] = static_cast<int>(fx >> 16);
        sx1[i] = std::min(sx0[i] + 1, srcW - 1);
        wx1[i] = static_cast<int>((fx >> 8) & 0xFF);
        wx0[i] = 256 - wx1[i];
    }

    for (int y = y0; y < y1; ++y) {
        int64_t fy = static_cast<int64_t>(y - dstY) * stepY + (stepY >> 1) - (1 << 15);
        fy = std::max<int64_t>(0, std::min(maxFy, fy));
        const int sy = static_cast<int>(fy >> 16);
        const int sy1 = std::min(sy + 1, srcH - 1);
        const unsigned wy1 = static_cast<unsigned>((fy >> 8) & 0xFF);
        const unsigned wy0 = 256 - wy1;
        const uint8_t* row0 = src + static_cast<size_t>(sy) * srcStride * 4;
        const uint8_t* row1 = src + static_cast<size_t>(sy1) * srcStride * 4;
        uint8_t* drow = dst + (static_cast<size_t>(y) * rootW + x0) * 4;
        for (int i = 0; i < x1 - x0; ++i) {
            const uint8_t* p00 = row0 + sx0[i] * 4;
            const uint8_t* p01 = row0 + sx1[i] * 4;
            const uint8_t* p10 = row1 + sx0[i] * 4;
            const uint8_t* p11 = row1 + sx1[i] * 4;
            const unsigned w00 = static_cast<unsigned>(wx0[i]) * wy0;
            const unsigned w01 = static_cast<unsigned>(wx1[i]) * wy0;
            const unsigned w10 = static_cast<unsigned>(wx0[i]) * wy1;
            const unsigned w11 = static_cast<unsigned>(wx1[i]) * wy1;
            uint8_t* dpx = drow + i * 4;
            // 权重和恒为 65536, >>16 后必在 [0,255]
            const unsigned b = (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) >> 16;
            const unsigned g = (p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) >> 16;
            const unsigned r = (p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) >> 16;
            if (!alphaBlend) {
                dpx[0] = static_cast<uint8_t>(b);
                dpx[1] = static_cast<uint8_t>(g);
                dpx[2] = static_cast<uint8_t>(r);
                dpx[3] = 255;
                continue;
            }
            const unsigned a = (p00[3] * w00 + p01[3] * w01 + p10[3] * w10 + p11[3] * w11) >> 16;
            if (a == 0) continue;
            if (a >= 255) {
                dpx[0] = static_cast<uint8_t>(b);
                dpx[1] = static_cast<uint8_t>(g);
                dpx[2] = static_cast<uint8_t>(r);
                dpx[3] = 255;
            } else {
                // 预乘 over: dst = src + dst * (1-a); 采样误差可能使 b > a, 需钳制
                const unsigned inv = 255 - a;
                const unsigned nb = b + (dpx[0] * inv) / 255;
                const unsigned ng = g + (dpx[1] * inv) / 255;
                const unsigned nr = r + (dpx[2] * inv) / 255;
                dpx[0] = static_cast<uint8_t>(std::min(nb, 255u));
                dpx[1] = static_cast<uint8_t>(std::min(ng, 255u));
                dpx[2] = static_cast<uint8_t>(std::min(nr, 255u));
                dpx[3] = 255;
            }
        }
    }
}

} // namespace

extern "C" void RegisterXdgShell(wl_display* display);

#undef LOG_TAG
#define LOG_TAG "WL_Server"
#include <hilog/log.h>
#include "plugin_manager.h"

// -- wl_surface 接口实现表 --
static const struct wl_surface_interface kSurfaceImpl = {
    .destroy           = WaylandServer::surface_destroy,
    .attach            = WaylandServer::surface_attach,
    .damage            = WaylandServer::surface_damage,
    .frame             = WaylandServer::surface_frame,
    .set_opaque_region = WaylandServer::surface_set_opaque_region,
    .set_input_region  = WaylandServer::surface_set_input_region,
    .commit            = WaylandServer::surface_commit,
    .set_buffer_transform = WaylandServer::surface_set_buffer_transform,
    .set_buffer_scale  = WaylandServer::surface_set_buffer_scale,
    .damage_buffer     = WaylandServer::surface_damage_buffer,
    .offset            = WaylandServer::surface_offset,
};

static const struct wl_region_interface kRegionImpl = {
    .destroy  = WaylandServer::region_destroy,
    .add      = WaylandServer::region_add,
    .subtract = WaylandServer::region_subtract,
};

static const struct wl_subcompositor_interface kSubcompositorImpl = {
    .destroy        = WaylandServer::subcompositor_destroy,
    .get_subsurface = WaylandServer::subcompositor_get_subsurface,
};

static const struct wl_subsurface_interface kSubsurfaceImpl = {
    .destroy       = WaylandServer::subsurface_destroy,
    .set_position  = WaylandServer::subsurface_set_position,
    .place_above   = WaylandServer::subsurface_place_above,
    .place_below   = WaylandServer::subsurface_place_below,
    .set_sync      = WaylandServer::subsurface_set_sync,
    .set_desync    = WaylandServer::subsurface_set_desync,
};

static const struct wp_viewporter_interface kViewporterImpl = {
    .destroy      = WaylandServer::viewporter_destroy,
    .get_viewport = WaylandServer::viewporter_get_viewport,
};

static const struct wp_viewport_interface kViewportImpl = {
    .destroy        = WaylandServer::viewport_destroy,
    .set_source     = WaylandServer::viewport_set_source,
    .set_destination = WaylandServer::viewport_set_destination,
};

static const struct wl_output_interface kOutputImpl = {
    .release = WaylandServer::output_release,
};

static const struct wl_compositor_interface kCompositorImpl = {
    .create_surface = WaylandServer::compositor_create_surface,
    .create_region  = WaylandServer::compositor_create_region,
};

// -- 单例 --
WaylandServer* WaylandServer::GetInstance() {
    static WaylandServer s;
    return &s;
}

bool WaylandServer::Start(const std::string& socketPath) {
    if (running_) {
        OH_LOG_WARN(LOG_APP, "[WL] already running");
        return true;
    }

    OH_LOG_INFO(LOG_APP, "[WL] Starting compositor, socket=%{public}s", socketPath.c_str());

    // 清理残留 socket
    unlink(socketPath.c_str());

    // 确保 socket 目录存在
    auto pos = socketPath.find_last_of('/');
    std::string dir = socketPath.substr(0, pos);
    std::string name = socketPath.substr(pos + 1);
    int rc = mkdir(dir.c_str(), 0700);
    OH_LOG_INFO(LOG_APP, "[WL] mkdir(%{public}s) = %{public}d, errno=%{public}d",
                dir.c_str(), rc, errno);

    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);

    display_ = wl_display_create();
    if (!display_) {
        OH_LOG_ERROR(LOG_APP, "[WL] wl_display_create failed, errno=%{public}d", errno);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[WL] wl_display created");

    if (wl_display_add_socket(display_, name.c_str()) != 0) {
        OH_LOG_ERROR(LOG_APP, "[WL] wl_display_add_socket(%{public}s) failed, errno=%{public}d",
                     name.c_str(), errno);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[WL] socket added: %{public}s", name.c_str());

    setenv("WAYLAND_DISPLAY", name.c_str(), 1);

    // 注册 global 对象
    wl_global_create(display_, &wl_compositor_interface, 4, this, compositor_bind);
    wl_display_init_shm(display_);
    RegisterXdgShell(display_);
    wl_global_create(display_, &wl_subcompositor_interface, 1, this, subcompositor_bind);
    wl_global_create(display_, &wp_viewporter_interface, 1, this, viewporter_bind);
    wl_global_create(display_, &wl_output_interface, 3, this, output_bind);
    Seat::GetInstance()->Register(display_);
    InputManager::GetInstance()->Initialize(display_);
    OH_LOG_INFO(LOG_APP, "[WL] globals registered (compositor+shm+xdg+subcompositor+viewporter+output+seat+input)");

    running_ = true;
    firstFrame_ = false;
    thread_ = std::thread(&WaylandServer::EventLoop, this);
    OH_LOG_INFO(LOG_APP, "[WL] compositor started OK");
    return true;
}

void WaylandServer::Stop() {
    if (!running_) return;
    running_ = false;
    InputManager::GetInstance()->Shutdown();
    Seat::GetInstance()->Unregister();
    if (display_) wl_display_terminate(display_);
    if (thread_.joinable()) thread_.join();
    if (display_) {
        wl_display_destroy(display_);
        display_ = nullptr;
    }
    firstFrame_ = false;
}

void WaylandServer::EventLoop() {
    int tick = 0;
    while (running_) {
        wl_event_loop* loop = wl_display_get_event_loop(display_);
        int ret = wl_event_loop_dispatch(loop, 50); // 50ms timeout
        if (ret < 0) {
            OH_LOG_ERROR(LOG_APP, "[WL-ERR] event loop error: %{public}s (errno=%{public}d)",
                         strerror(errno), errno);
        }
        wl_display_flush_clients(display_);  // dispatch 可能写数据, 之后 flush

        // 每 30 秒输出一次资源快照 (50ms * 600 = 30s)
        if (++tick % 600 == 0) {
            size_t renderers = PluginManager::GetInstance()->GetRendererCount();
            OH_LOG_INFO(LOG_APP, "[WL-STAT] toplevels=%{public}zu surfaces=%{public}zu renderers=%{public}zu",
                        toplevelResources_.size(), toplevelSurfaceMap_.size(), renderers);
        }
    }
}

// -- compositor 实现 --
void WaylandServer::compositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(res, &kCompositorImpl, data, nullptr);
}

void WaylandServer::compositor_create_surface(wl_client* client, wl_resource* compRes, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] client created wl_surface id=%{public}u", id);
    auto* sd = new SurfaceData();
    wl_resource* surfRes = wl_resource_create(client, &wl_surface_interface,
                                              wl_resource_get_version(compRes), id);
    sd->surface = surfRes;
    sd->clientPid = GetWaylandClientPid(client);
    sd->protocolId = id;
    sd->surfaceKey = MakeSurfaceKey(sd->clientPid, id);
    wl_resource_set_implementation(surfRes, &kSurfaceImpl, sd, [](wl_resource* r) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(r));
        auto* self = GetInstance();
        uint32_t removedPopup = 0, popupParent = 0;
        {
            std::lock_guard<std::mutex> lk(self->toplevelMutex_);
            const uint64_t surfaceKey = sd ? sd->surfaceKey : 0;
            self->surfaceResources_.erase(surfaceKey);
            self->zeroCopySurfaceKeys_.erase(surfaceKey);
            for (auto it = self->subsurfaceLayers_.begin(); it != self->subsurfaceLayers_.end(); ) {
                if (it->surface == r) it = self->subsurfaceLayers_.erase(it);
                else ++it;
            }
            // PC popup 记录一并清除 (client 断开时 libwayland 走此路径)
            if (sd) {
                popupParent = self->RemovePopupBySurfaceKeyLocked(sd->surfaceKey, removedPopup);
            }
            self->MarkDesktopRootDirtyLocked();
        }
        // 无条件重置输入焦点: 任何 surface (含 desktop 菜单 subsurface) 销毁时
        // 都可能是当前 pointer/keyboard 焦点 — 焦点悬垂后下一次 leave 会引用
        // 已复用的对象 id, client 报 "invalid object ... leave(uo)" 并断开。
        // 内部按指针比较, 非焦点 surface 是 no-op
        InputManager::GetInstance()->OnSurfaceDestroyed(r);
        if (sd && sd->hasToplevel) {
            {
                std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
                self->toplevelSurfaceMap_.erase(sd->toplevelId);
            }
            self->FireToplevelEvent(sd->toplevelId, "destroyed");
        }
        if (removedPopup) {
            char json[64];
            snprintf(json, sizeof(json), "{\"popupId\":%u}", removedPopup);
            self->FireToplevelEvent(popupParent, "popup_hide", json);
        }
        delete sd;
    });
    {
        auto* self = static_cast<WaylandServer*>(wl_resource_get_user_data(compRes));
        std::lock_guard<std::mutex> lk(self->toplevelMutex_);
        self->surfaceResources_[sd->surfaceKey] = surfRes;
    }
}

void WaylandServer::compositor_create_region(wl_client* client, wl_resource* compRes, uint32_t id) {
    int* rectCount = new int(0);
    wl_resource* res = wl_resource_create(client, &wl_region_interface,
                                          wl_resource_get_version(compRes), id);
    wl_resource_set_implementation(res, &kRegionImpl, rectCount, [](wl_resource* r) {
        delete static_cast<int*>(wl_resource_get_user_data(r));
    });
}

// -- subcompositor 实现 --
void WaylandServer::subcompositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] wl_subcompositor bound v=%{public}u", version);
    wl_resource* res = wl_resource_create(client, &wl_subcompositor_interface, version, id);
    wl_resource_set_implementation(res, &kSubcompositorImpl, data, nullptr);
}

void WaylandServer::subcompositor_get_subsurface(wl_client* client, wl_resource*,
                                                  uint32_t id, wl_resource* surface,
                                                  wl_resource* parent) {
    // 追踪 subsurface 父子关系:
    // 子 surface → 记录父 surface 指针, 标记 isSubsurface
    // wl_subsurface 的 user_data 存子 surface, 供 set_position 查找
    auto* childSd = static_cast<SurfaceData*>(wl_resource_get_user_data(surface));
    if (childSd) {
        childSd->parentSurface = parent;
        childSd->isSubsurface = true;
        OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] subsurface created: child=%{public}p parent=%{public}p",
                    surface, parent);
    }

    // wl_subsurface resource 的 user_data = 子 surface (供 set_position 查找 SurfaceData)
    wl_resource* ss = wl_resource_create(client, &wl_subsurface_interface, 1, id);
    wl_resource_set_implementation(ss, &kSubsurfaceImpl, surface, nullptr);
}

void WaylandServer::subsurface_set_position(wl_client*, wl_resource* ssRes,
                                             int32_t x, int32_t y) {
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(ssRes));
    if (!childSurf) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(childSurf));
    if (!sd) return;
    if (sd->subsurfaceX == x && sd->subsurfaceY == y) return;
    sd->subsurfaceX = x;
    sd->subsurfaceY = y;
    auto* self = GetInstance();
    {
        std::lock_guard<std::mutex> lk(self->toplevelMutex_);
        for (auto& layer : self->subsurfaceLayers_) {
            if (layer.surface != childSurf) continue;
            layer.localX = x;
            layer.localY = y;
            break;
        }
        self->MarkDesktopRootDirtyLocked();
    }
    // PC 模式: 更新已登记 popup 的偏移, 通知 ArkTS 移动子窗口
    uint32_t movePopupId = 0, moveParent = 0;
    int32_t moveOffX = 0, moveOffY = 0;
    {
        std::lock_guard<std::mutex> lk(self->toplevelMutex_);
        auto pit = self->popupBySurfaceKey_.find(sd->surfaceKey);
        if (pit != self->popupBySurfaceKey_.end()) {
            auto rit = self->popups_.find(pit->second);
            if (rit != self->popups_.end()) {
                auto& rec = rit->second;
                int32_t geoX = 0, geoY = 0;
                if (sd->parentSurface) {
                    auto* parentSd = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
                    if (parentSd) { geoX = parentSd->geoX; geoY = parentSd->geoY; }
                }
                rec.offX = x - geoX;
                rec.offY = y - geoY;
                movePopupId = rec.popupId;
                moveParent = rec.parentToplevel;
                moveOffX = rec.offX;
                moveOffY = rec.offY;
            }
        }
    }
    if (movePopupId) {
        char json[128];
        snprintf(json, sizeof(json), "{\"popupId\":%u,\"x\":%d,\"y\":%d}",
                 movePopupId, moveOffX, moveOffY);
        self->FireToplevelEvent(moveParent, "popup_move", json);
    }
    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] set_position: child=%{public}p parent=%{public}p pos=(%{public}d,%{public}d)",
                childSurf, sd->parentSurface, x, y);
}

void WaylandServer::subsurface_place_above(wl_client*, wl_resource* ssRes, wl_resource* sibling) {
    /*
     * 风险标注 (P2): place_above/place_below 只维护 desktop 模式的
     * subsurfaceLayers_ 顺序。PC 模式的 popup 是独立 OHOS 子窗口,
     * z-order 由窗口系统按创建顺序决定, 此处的重排不会映射到子窗口。
     * 桌面语义上 popup 恒在父窗口之上 (OHOS 子窗口天然满足),
     * 多 popup (子菜单链) 交叠顺序极端情况下可能与客户端预期不符。
     */
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(ssRes));
    if (!childSurf || !sibling) return;
    auto* self = GetInstance();
    std::lock_guard<std::mutex> lk(self->toplevelMutex_);
    int myIdx = -1, siblingIdx = -1;
    for (size_t i = 0; i < self->subsurfaceLayers_.size(); i++) {
        if (self->subsurfaceLayers_[i].surface == childSurf) myIdx = (int)i;
        if (self->subsurfaceLayers_[i].surface == sibling) siblingIdx = (int)i;
    }
    if (myIdx < 0 || siblingIdx < 0 || myIdx == siblingIdx) return;
    auto layer = std::move(self->subsurfaceLayers_[myIdx]);
    self->subsurfaceLayers_.erase(self->subsurfaceLayers_.begin() + myIdx);
    int target = (myIdx < siblingIdx) ? siblingIdx - 1 : siblingIdx;
    self->subsurfaceLayers_.insert(self->subsurfaceLayers_.begin() + target + 1, std::move(layer));
    if (self->IsDesktopMode()) self->MarkDesktopRootDirtyLocked();
    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] place_above child=%{public}p above sibling=%{public}p",
                childSurf, sibling);
}

void WaylandServer::subsurface_place_below(wl_client*, wl_resource* ssRes, wl_resource* sibling) {
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(ssRes));
    if (!childSurf || !sibling) return;
    auto* self = GetInstance();
    std::lock_guard<std::mutex> lk(self->toplevelMutex_);
    int myIdx = -1, siblingIdx = -1;
    for (size_t i = 0; i < self->subsurfaceLayers_.size(); i++) {
        if (self->subsurfaceLayers_[i].surface == childSurf) myIdx = (int)i;
        if (self->subsurfaceLayers_[i].surface == sibling) siblingIdx = (int)i;
    }
    if (myIdx < 0 || siblingIdx < 0 || myIdx == siblingIdx) return;
    auto layer = std::move(self->subsurfaceLayers_[myIdx]);
    self->subsurfaceLayers_.erase(self->subsurfaceLayers_.begin() + myIdx);
    int target = (myIdx < siblingIdx) ? siblingIdx - 1 : siblingIdx;
    self->subsurfaceLayers_.insert(self->subsurfaceLayers_.begin() + target, std::move(layer));
    if (self->IsDesktopMode()) self->MarkDesktopRootDirtyLocked();
    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] place_below child=%{public}p below sibling=%{public}p",
                childSurf, sibling);
}

void WaylandServer::subsurface_destroy(wl_client*, wl_resource* r) {
    // role 移除: surface 变回普通 surface。按 unmap 处理:
    // 清 desktop layer / PC popup 记录, 通知 ArkTS 销毁 popup 子窗口。
    auto* childSurf = static_cast<wl_resource*>(wl_resource_get_user_data(r));
    if (childSurf) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(childSurf));
        if (sd) {
            sd->isSubsurface = false;
            sd->parentSurface = nullptr;
            auto* self = GetInstance();
            uint32_t removedPopup = 0, popupParent = 0;
            {
                std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                for (auto it = self->subsurfaceLayers_.begin(); it != self->subsurfaceLayers_.end(); ) {
                    if (it->surface == childSurf) it = self->subsurfaceLayers_.erase(it);
                    else ++it;
                }
                popupParent = self->RemovePopupBySurfaceKeyLocked(sd->surfaceKey, removedPopup);
            }
            if (removedPopup) {
                char json[64];
                snprintf(json, sizeof(json), "{\"popupId\":%u}", removedPopup);
                self->FireToplevelEvent(popupParent, "popup_hide", json);
            }
        }
    }
    wl_resource_destroy(r);
}

// -- viewporter 实现 --
void WaylandServer::viewporter_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] wp_viewporter bound v=%{public}u", version);
    wl_resource* res = wl_resource_create(client, &wp_viewporter_interface, version, id);
    wl_resource_set_implementation(res, &kViewporterImpl, data, nullptr);
}

void WaylandServer::viewporter_get_viewport(wl_client* client, wl_resource*,
                                             uint32_t id, wl_resource* surface) {
    wl_resource* vp = wl_resource_create(client, &wp_viewport_interface, 1, id);
    // 把 surface resource 存为 viewport 的 user_data,
    // 这样 viewport_set_destination 就能通过 surface 找到 SurfaceData
    wl_resource_set_implementation(vp, &kViewportImpl, surface, nullptr);
}

void WaylandServer::viewport_set_source(wl_client*, wl_resource* vpRes,
                                        wl_fixed_t fx, wl_fixed_t fy, wl_fixed_t fw, wl_fixed_t fh) {
    auto* surf = static_cast<wl_resource*>(wl_resource_get_user_data(vpRes));
    if (!surf) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surf));
    if (!sd) return;
    if (wl_fixed_to_int(fw) == -1 && wl_fixed_to_int(fh) == -1) {
        // unset: 恢复全 buffer (注意参数是 wl_fixed_t, unset 编码为 wl_fixed_from_int(-1))
        sd->vpSrcX = 0;
        sd->vpSrcY = 0;
        sd->vpSrcW = -1;
        sd->vpSrcH = -1;
        return;
    }
    sd->vpSrcX = wl_fixed_to_int(fx);
    sd->vpSrcY = wl_fixed_to_int(fy);
    sd->vpSrcW = wl_fixed_to_int(fw);
    sd->vpSrcH = wl_fixed_to_int(fh);
    OH_LOG_INFO(LOG_APP, "[MW-VP] set_source surf=%{public}p tl=%{public}u src=(%{public}d,%{public}d %{public}dx%{public}d)",
                surf, sd->toplevelId, sd->vpSrcX, sd->vpSrcY, sd->vpSrcW, sd->vpSrcH);
}

void WaylandServer::viewport_set_destination(wl_client*, wl_resource* vpRes, int32_t w, int32_t h) {
    auto* surf = static_cast<wl_resource*>(wl_resource_get_user_data(vpRes));
    if (!surf) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surf));
    if (!sd) return;
    sd->vpDstW = w;
    sd->vpDstH = h;
}

// -- output 实现 --
void WaylandServer::output_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[WL] wl_output bound v=%{public}u", version);
    wl_resource* res = wl_resource_create(client, &wl_output_interface, version, id);
    wl_resource_set_implementation(res, &kOutputImpl, data, nullptr);
    auto* self = static_cast<WaylandServer*>(data);
    // 发送虚拟显示器信息 (尺寸由 ArkTS 初始化时设置)
    int32_t pw = self->outputW_, ph = self->outputH_;
    int32_t physW = pw * 340 / 1280;  // 约 96DPI 物理尺寸
    int32_t physH = ph * 190 / 720;
    OH_LOG_INFO(LOG_APP, "[WL] output_bind: %{public}dx%{public}d (phys %{public}dx%{public}d)",
                pw, ph, physW, physH);
    wl_output_send_geometry(res, 0, 0, physW, physH,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN, "Wine", "Virtual",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        pw, ph, 60000);
    if (version >= 2) wl_output_send_scale(res, 1);
    if (version >= 4) {
        wl_output_send_name(res, "Wine-Virtual-0");
        wl_output_send_description(res, "Virtual output for Wine Wayland driver");
    }
    wl_output_send_done(res);
}

// -- surface 实现 --
void WaylandServer::surface_destroy(wl_client*, wl_resource* r) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(r));
    OH_LOG_INFO(LOG_APP, "[MW-Life] surface_destroy surf=%{public}p hasToplevel=%{public}d toplevelId=%{public}u isSubsurface=%{public}d",
                r, sd ? sd->hasToplevel : 0, sd ? sd->toplevelId : 0, sd ? sd->isSubsurface : 0);
    if (sd && sd->hasToplevel) {
        // 清理 surface 映射
        auto* self = GetInstance();
        {
            std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
            self->toplevelSurfaceMap_.erase(sd->toplevelId);
        }
        // 清理 toplevel 像素数据 + 标记 root dirty
        // (root 被销毁时 OnToplevelDestroyed 内部已复位 desktopRootToplevelId_)
        self->OnToplevelDestroyed(sd->toplevelId);
        // 重置 InputManager 焦点: 防止后续 Inject*Leave 引用已销毁的 surface
        // (否则 Wine 收到 invalid object 协议错误 → 断开连接)
        InputManager::GetInstance()->OnSurfaceDestroyed(r);
        self->FireToplevelEvent(sd->toplevelId, "destroyed");
    }
    // subsurface 销毁: 清除 layer + 标记 root dirty 触发重绘 (移除残留像素)
    if (sd && sd->isSubsurface) {
        auto* self = GetInstance();
        uint32_t removedPopup = 0, popupParent = 0;
        {
            std::lock_guard<std::mutex> lk(self->toplevelMutex_);
            for (auto it = self->subsurfaceLayers_.begin(); it != self->subsurfaceLayers_.end(); ) {
                if (it->surface == r) it = self->subsurfaceLayers_.erase(it);
                else ++it;
            }
            // PC popup 记录一并清除
            popupParent = self->RemovePopupBySurfaceKeyLocked(sd->surfaceKey, removedPopup);
            if (self->IsDesktopMode()) self->MarkDesktopRootDirtyLocked();
        }
        if (removedPopup) {
            // 防止 pointer focus 悬在已销毁的 popup surface 上 (协议错误会断开 Wine)
            InputManager::GetInstance()->OnSurfaceDestroyed(r);
            char json[64];
            snprintf(json, sizeof(json), "{\"popupId\":%u}", removedPopup);
            self->FireToplevelEvent(popupParent, "popup_hide", json);
        }
    }
    wl_resource_destroy(r);
}

void WaylandServer::surface_attach(wl_client*, wl_resource* surfRes, wl_resource* buffer, int32_t, int32_t) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    sd->pendingBuffer = buffer;
    // 新 buffer → 重置 damage
    sd->damageX = 0; sd->damageY = 0;
    sd->damageW = 0; sd->damageH = 0;
}

void WaylandServer::surface_damage(wl_client*, wl_resource* surfRes,
                                    int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    if (!sd) return;
    // 累积 damage 包围盒 (union)
    if (sd->damageW == 0 || sd->damageH == 0) {
        sd->damageX = x; sd->damageY = y;
        sd->damageW = w; sd->damageH = h;
    } else {
        int32_t rx = std::min(sd->damageX, x);
        int32_t ry = std::min(sd->damageY, y);
        int32_t rr = std::max(sd->damageX + sd->damageW, x + w);
        int32_t rb = std::max(sd->damageY + sd->damageH, y + h);
        sd->damageX = rx; sd->damageY = ry;
        sd->damageW = rr - rx; sd->damageH = rb - ry;
    }
}

void WaylandServer::surface_set_input_region(wl_client*, wl_resource* surfRes, wl_resource* region) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    if (!sd) return;
    if (region) {
        int* count = static_cast<int*>(wl_resource_get_user_data(region));
        sd->inputRegionEmpty = (count && *count == 0);
    } else {
        sd->inputRegionEmpty = false;  // NULL region = 整面接受输入
    }
}

void WaylandServer::surface_frame(wl_client* client, wl_resource* surfRes, uint32_t cbId) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    wl_resource* cbRes = wl_resource_create(client, &wl_callback_interface, 1, cbId);
    sd->frameCallbacks.push_back(cbRes);
}

void WaylandServer::surface_commit(wl_client*, wl_resource* surfRes) {
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surfRes));
    // NULL buffer → surface 无内容: 清除对应 subsurface layer / PC popup
    if (!sd->pendingBuffer) {
        if (sd->isSubsurface) {
            auto* self = GetInstance();
            uint32_t removedPopup = 0, popupParent = 0;
            {
                std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                size_t before = self->subsurfaceLayers_.size();
                for (auto it = self->subsurfaceLayers_.begin(); it != self->subsurfaceLayers_.end(); ) {
                    if (it->surface == surfRes) it = self->subsurfaceLayers_.erase(it);
                    else ++it;
                }
                if (self->subsurfaceLayers_.size() != before) {
                    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] NULL buffer commit → removed layer, layers %{public}zu→%{public}zu",
                                before, self->subsurfaceLayers_.size());
                    if (self->IsDesktopMode()) self->MarkDesktopRootDirtyLocked();
                }
                // PC popup: unmap (菜单关闭) → 销毁 ArkTS 子窗口
                popupParent = self->RemovePopupBySurfaceKeyLocked(sd->surfaceKey, removedPopup);
            }
            if (removedPopup) {
                OH_LOG_INFO(LOG_APP, "[MW-POPUP] hide popup=#%{public}u parent=#%{public}u (NULL buffer commit)",
                            removedPopup, popupParent);
                char json[64];
                snprintf(json, sizeof(json), "{\"popupId\":%u}", removedPopup);
                self->FireToplevelEvent(popupParent, "popup_hide", json);
            }
        }
        return;
    }

    // 读取 wl_shm buffer 像素
    wl_shm_buffer* shm = wl_shm_buffer_get(sd->pendingBuffer);
    if (shm) {
        int32_t w = wl_shm_buffer_get_width(shm);
        int32_t h = wl_shm_buffer_get_height(shm);
        int32_t stride = wl_shm_buffer_get_stride(shm);
        int32_t rowBytes = w * 4;  // 紧密排列的每行字节数
        // WL_SHM_FORMAT_ARGB8888=0 (有意义 alpha, layered/shaped 窗口), XRGB8888=1 (无 alpha)
        const uint32_t shmFormat = wl_shm_buffer_get_format(shm);

        wl_shm_buffer_begin_access(shm);
        const uint8_t* src = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));

        // 确定实际内容区域: 优先用 window_geometry, 否则全 buffer
        int contentW = w, contentH = h;
        int contentOffX = 0, contentOffY = 0;
        int screenX = 0, screenY = 0;  // 桌面模式: 虚拟桌面位置
        if (sd->hasWindowGeometry && sd->geoW > 0 && sd->geoH > 0) {
            contentW = sd->geoW;
            contentH = sd->geoH;
            if (sd->hasToplevel) {
                // 桌面模式: toplevel content 永远从 buffer 原点开始,
                // geoX/geoY 是虚拟桌面屏幕位置
                contentOffX = 0;
                contentOffY = 0;
                screenX = sd->geoX;
                screenY = sd->geoY;
            } else {
                // subsurface: geoX/geoY 是相对父 surface 的内容偏移
                contentOffX = sd->geoX;
                contentOffY = sd->geoY;
            }
            /*
             * 防御: geometry 与 buffer 是异步更新的 — 显示模式切换瞬间
             * Wine 会先发新 geometry (如 1400x920) 而 buffer 仍是旧尺寸
             * (如 896x640, GL readback 管线尚未跟上)。content 必须 clamp
             * 进 buffer 实际范围, 否则 copyTight 越界读 shm → SIGSEGV
             * (实测: 游戏退出恢复桌面分辨率的瞬间崩溃于 memcpy)。
             * 该帧显示为部分内容, 下一帧 buffer 跟上后自然恢复。
             */
            if (contentOffX < 0 || contentOffX >= w) contentOffX = 0;
            if (contentOffY < 0 || contentOffY >= h) contentOffY = 0;
            if (contentOffX + contentW > w) contentW = w - contentOffX;
            if (contentOffY + contentH > h) contentH = h - contentOffY;
            if (contentW <= 0 || contentH <= 0) {
                contentOffX = contentOffY = 0;
                contentW = w;
                contentH = h;
            }
            OH_LOG_INFO(LOG_APP, "[MW-GEO] using window_geometry: src=%{public}dx%{public}d geo=(%{public}d,%{public}d %{public}dx%{public}d) screen=(%{public}d,%{public}d) vpSrc=(%{public}d,%{public}d %{public}dx%{public}d) vpDst=%{public}dx%{public}d",
                        w, h, contentOffX, contentOffY, contentW, contentH, screenX, screenY,
                        sd->vpSrcX, sd->vpSrcY, sd->vpSrcW, sd->vpSrcH, sd->vpDstW, sd->vpDstH);
        }

        // 复制像素时 strip stride padding, 只提取 content 区域
        int contentRowBytes = contentW * 4;
        bool strideMismatch = (stride != rowBytes);
        if (strideMismatch) {
            OH_LOG_WARN(LOG_APP, "[MW-STRIDE] stride mismatch! w=%{public}d h=%{public}d stride=%{public}d rowBytes=%{public}d",
                        w, h, stride, rowBytes);
        }
        auto copyTight = [&](std::vector<uint8_t>& dst) {
            dst.resize(contentRowBytes * contentH);
            uint8_t* d = dst.data();
            const uint8_t* rowStart = src + contentOffY * stride + contentOffX * 4;
            for (int32_t y = 0; y < contentH; y++) {
                std::memcpy(d, rowStart + y * stride, contentRowBytes);
                d += contentRowBytes;
            }
        };

        // per-surface buffer (全量, 兼容)
        auto copyTightFull = [&](std::vector<uint8_t>& dst) {
            dst.resize(rowBytes * h);
            uint8_t* d = dst.data();
            for (int32_t y = 0; y < h; y++) {
                std::memcpy(d, src + y * stride, rowBytes);
                d += rowBytes;
            }
        };
        copyTightFull(sd->pixels);
        sd->w = contentW;
        sd->h = contentH;
        sd->dirty = true;
        const uint64_t shmCommitSerial =
            sd->shmCommitSerial.fetch_add(1, std::memory_order_release) + 1;
        OH_LOG_INFO(LOG_APP, "[MW-COMMIT] surface w=%{public}d h=%{public}d stride=%{public}d stored=%{public}zu content=%{public}dx%{public}d geo=%{public}s",
                    w, h, stride, sd->pixels.size(), contentW, contentH,
                    sd->hasWindowGeometry ? "yes" : "no");

        auto* self = GetInstance();
        // global framebuffer (deprecated, keep for backward compat)
        {
            std::lock_guard<std::mutex> lk(self->mutex_);
            copyTightFull(self->pixels_);
            self->width_ = w;
            self->height_ = h;
            self->dirty_ = true;
        }
        // toplevel framebuffer
        bool isFirstCommit = false;
        if (sd->hasToplevel) {
            // Register surface mapping for input focus lookup
            {
                std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
                self->toplevelSurfaceMap_[sd->toplevelId] = surfRes;
            }
            std::lock_guard<std::mutex> lk(self->toplevelMutex_);
            auto& st = self->EnsureToplevelLocked(sd->toplevelId);  // 首次 commit 在此建档
            copyTight(st.pixels);
            st.w = contentW;
            st.h = contentH;
            if (sd->toplevelId == self->desktopRootToplevelId_) {
                ++self->desktopRootFrameSerial_;
            }
            /*
             * 自动恢复最小化窗口:
             * Wine 没有 "unset_minimized" 协议。当用户点击任务栏还原窗口时,
             * Wine 直接 commit 新的窗口内容 (正常尺寸 > 最小化标题栏 ~200x30)。
             * 检测到 contentW>200 && contentH>50 即判定为真实窗口 → 清除 minimized 状态。
             * (Wine 最小化标题栏会定期 commit 约 200x30 的小表面, 不应触发恢复)
             * 注意: 此处已持有 toplevelMutex_, 不能调 SetToplevelRestored。
             */
            if (sd->minimized && contentW > 200 && contentH > 50) {
                sd->minimized = false;
                st.minimized = false;
                OH_LOG_INFO(LOG_APP, "[MW] auto-restore tl=%{public}u size=%{public}dx%{public}d",
                            sd->toplevelId, contentW, contentH);
            }
            // 首帧判定只认 hasPosition, 不认条目存在
            // (pre-commit 的 SetToplevelMinimized 等路径可能已建档)
            isFirstCommit = !st.hasPosition;
            if (isFirstCommit) {
                st.x = screenX;
                st.y = screenY;
                st.wineX = screenX;
                st.wineY = screenY;
                st.hasPosition = true;
                OH_LOG_INFO(LOG_APP, "[MW-MOVE] initial pos tl=%{public}u (%{public}d,%{public}d)",
                    sd->toplevelId, screenX, screenY);
            }
            /*
             * PC 模式: created 延迟到首帧 (此时 wl_shm 格式已确定):
             * - XRGB → "created": 走 WineWindowAbility (multiton 主窗口)
             * - ARGB → "argb_created": 走子窗口 + setWindowMask 异型窗口路线
             *   (2in1 主窗口无 alpha 通道/背景透明被钳制, 实测不可行)
             */
            if (isFirstCommit && !self->IsDesktopMode()) {
                char json[160];
                if (shmFormat == 0) {
                    snprintf(json, sizeof(json), "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                             screenX, screenY, contentW, contentH);
                    OH_LOG_INFO(LOG_APP, "[MW] argb_created tl=%{public}u geo=(%{public}d,%{public}d %{public}dx%{public}d)",
                                sd->toplevelId, screenX, screenY, contentW, contentH);
                    self->FireToplevelEvent(sd->toplevelId, "argb_created", json);
                } else {
                    snprintf(json, sizeof(json), "{\"w\":%d,\"h\":%d}", contentW, contentH);
                    self->FireToplevelEvent(sd->toplevelId, "created", json);
                }
            }
            /*
             * ARGB 窗口: Wine 位置为权威 (桌面小部件由 Wine 决定屏幕位置)。
             * 普通 PC 窗口后续 commit 忽略 geoX/geoY (OHOS 窗口管理器为权威),
             * ARGB 窗口相反: geo 变化 → 通知 ArkTS 移动子窗口。
             */
            if (!self->IsDesktopMode() && shmFormat == 0 && !isFirstCommit &&
                (st.x != screenX || st.y != screenY)) {
                st.x = screenX;
                st.y = screenY;
                char json[96];
                snprintf(json, sizeof(json), "{\"x\":%d,\"y\":%d}", screenX, screenY);
                self->FireToplevelEvent(sd->toplevelId, "argb_move", json);
            }
            // 后续 commit: 忽略 geoX/geoY, compositor 位置为权威
            st.dirty = true;
            // 记录 shm 格式 (ARGB8888=layered/shaped 有意义 alpha), 变化时通知
            // ArkTS 切换窗口背景 (PC 模式透明背景才能透过 per-pixel alpha)
            // 注意: 首帧必发 argb 事件 (即使首帧就是默认的 XRGB), 与旧的
            // "format 表无此 id" 判定等价 — 只按值比较会吞掉首帧事件
            if (isFirstCommit || st.shmFormat != shmFormat) {
                st.shmFormat = shmFormat;
                if (!self->IsDesktopMode()) {
                    char json[32];
                    snprintf(json, sizeof(json), "{\"argb\":%d}", shmFormat == 0 ? 1 : 0);
                    OH_LOG_INFO(LOG_APP, "[MW] toplevel #%{public}u shm format → %{public}s",
                                sd->toplevelId, shmFormat == 0 ? "ARGB8888" : "XRGB8888");
                    self->FireToplevelEvent(sd->toplevelId, "argb", json);
                }
            }
            /*
             * ARGB 窗口: 从 alpha 通道生成 0/1 剪影掩码 (setWindowMask 用)。
             * - 阈值 128: 半透明抗锯齿边缘向内收半像素, 避免灰边外扩
             * - 形状哈希没变就不重建: 时钟类静态形状零开销,
             *   动画类 (桌面宠物) 每帧变形才按帧重算
             * - 掩码是帧分辨率 (Wine 逻辑像素); setWindowMask 要求等于
             *   窗口物理尺寸, ArkTS 侧按 effectiveScale 最近邻放大
             */
            if (shmFormat == 0 && !self->IsDesktopMode()) {
                const auto& px = st.pixels;
                const size_t pixCount = static_cast<size_t>(contentW) * contentH;
                uint64_t hash = 1469598103934665603ULL;
                for (size_t i = 3; i < pixCount * 4; i += 4) {
                    hash ^= (px[i] >= 128) ? 1 : 0;
                    hash *= 1099511628211ULL;
                }
                auto& m = st.mask;
                if (hash != m.hash || m.w != contentW || m.h != contentH) {
                    m.hash = hash;
                    m.w = contentW;
                    m.h = contentH;
                    m.bits.resize(pixCount);
                    for (size_t i = 0; i < pixCount; i++) {
                        m.bits[i] = (px[i * 4 + 3] >= 128) ? 1 : 0;
                    }
                    m.dirty = true;
                    self->FireToplevelEvent(sd->toplevelId, "mask_dirty", "{}");
                }
            }
            // 新 toplevel 加到 Z-order 顶层
            if (self->IsDesktopMode() && sd->toplevelId != self->desktopRootToplevelId_) {
                auto zit = std::find(self->toplevelZOrder_.begin(), self->toplevelZOrder_.end(), sd->toplevelId);
                if (zit == self->toplevelZOrder_.end()) self->toplevelZOrder_.push_back(sd->toplevelId);
            }
            OH_LOG_INFO(LOG_APP, "[MW-COMMIT] toplevel #%{public}u frame %{public}dx%{public}d stride=%{public}d stored=%{public}zu",
                        sd->toplevelId, contentW, contentH, stride, st.pixels.size());

            // 检测尺寸变化 -> 通知 ArkTS 调整子窗口
            if (contentW != st.lastReportedW || contentH != st.lastReportedH) {
                st.lastReportedW = contentW;
                st.lastReportedH = contentH;
                char json[64];
                snprintf(json, sizeof(json), "{\"w\":%d,\"h\":%d}", contentW, contentH);
                OH_LOG_INFO(LOG_APP, "[MW] toplevel #%{public}u size changed: %{public}dx%{public}d max=%{public}s -> ArkTS",
                            sd->toplevelId, contentW, contentH,
                            sd->maximized ? "yes" : "no");
                self->FireToplevelEvent(sd->toplevelId, "resize", json);
            }
        }
        // Desktop 模式子窗口 commit → root 识别/切换 (决策入锁) + 标记 root dirty。
        // isBackground/pendingDesktopRoot/desktopRootToplevelId_ 与渲染线程共享,
        // 决策树必须在 toplevelMutex_ 内; MoveRendererToToplevel / FireToplevelEvent
        // 延迟到锁外执行 (与 PromotePendingDesktopRoot 同一约定);
        // GetSurfaceForToplevel 留在锁内 (锁序 toplevelMutex_ → toplevelSurfaceMutex_
        // 已由 RemovePopupDataLocked 确立)
        if (self->IsDesktopMode() && sd->hasToplevel &&
            sd->toplevelId != self->GetDesktopRootToplevelId()) {
            uint32_t moveRendererFrom = 0, moveRendererTo = 0;  // 0,0 = 不动
            bool fireDesktopRoot = false;
            {
                std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                uint32_t rootId = self->desktopRootToplevelId_;
                // 仅首次 commit 识别 explorer 桌面 (防止最大化误切 root)
                if (isFirstCommit) {
                    bool isExplorer = (sd->appId.find("explorer") != std::string::npos);
                    bool isFullSize = (contentW >= self->outputW_ * 8 / 10 &&
                                       contentH >= self->outputH_ * 8 / 10);

                    if (isExplorer && isFullSize) {
                        if (!self->desktopRootRecognitionEnabled_) {
                            if (!sd->title.empty()) {
                                self->pendingDesktopRootToplevelId_ = sd->toplevelId;
                                self->EnsureToplevelLocked(sd->toplevelId).isBackground = false;
                                OH_LOG_INFO(LOG_APP,
                                            "[MW] full-size explorer #%{public}u pending as desktop root while recognition is disabled title=%{public}s",
                                            sd->toplevelId, sd->title.c_str());
                            } else {
                                self->EnsureToplevelLocked(sd->toplevelId).isBackground = true;
                                OH_LOG_INFO(LOG_APP,
                                            "[MW] full-size explorer #%{public}u ignored while desktop root recognition is disabled (no title)",
                                            sd->toplevelId);
                            }
                        } else if (rootId == 0) {
                            if (self->pendingDesktopRootToplevelId_ > 0 &&
                                self->pendingDesktopRootToplevelId_ != sd->toplevelId) {
                                self->EnsureToplevelLocked(sd->toplevelId).isBackground = true;
                                OH_LOG_INFO(LOG_APP,
                                            "[MW] full-size explorer #%{public}u -> background, pending root #%{public}u exists",
                                            sd->toplevelId, self->pendingDesktopRootToplevelId_);
                            } else {
                                OH_LOG_INFO(LOG_APP, "[MW] desktop root: #%{public}u appId=explorer",
                                            sd->toplevelId);
                                moveRendererFrom = 0;
                                moveRendererTo = sd->toplevelId;
                                self->desktopRootToplevelId_ = sd->toplevelId;
                                self->pendingDesktopRootToplevelId_ = 0;
                                fireDesktopRoot = true;
                            }
                        } else if (!sd->title.empty()) {
                            wl_resource* oldSurf = self->GetSurfaceForToplevel(rootId);
                            auto* oldSd = oldSurf ? static_cast<SurfaceData*>(wl_resource_get_user_data(oldSurf)) : nullptr;
                            if (oldSd && oldSd->title.empty()) {
                                OH_LOG_INFO(LOG_APP, "[MW] root switch: #%{public}u (empty) -> #%{public}u (%{public}s)",
                                            rootId, sd->toplevelId, sd->title.c_str());
                                self->EnsureToplevelLocked(rootId).isBackground = true;
                                moveRendererFrom = rootId;
                                moveRendererTo = sd->toplevelId;
                                self->desktopRootToplevelId_ = sd->toplevelId;
                                fireDesktopRoot = true;
                            } else {
                                self->EnsureToplevelLocked(sd->toplevelId).isBackground = true;
                                OH_LOG_INFO(LOG_APP, "[MW] extra full-size explorer #%{public}u -> background",
                                            sd->toplevelId);
                            }
                        } else {
                            self->EnsureToplevelLocked(sd->toplevelId).isBackground = true;
                            OH_LOG_INFO(LOG_APP, "[MW] extra full-size explorer #%{public}u (no title) -> background",
                                        sd->toplevelId);
                        }
                    }
                }
                // 每次子窗口 commit 都标记 root dirty (包括 resize; 新 root 刚设置也生效)
                self->MarkDesktopRootDirtyLocked();
            }
            if (moveRendererTo)
                PluginManager::GetInstance()->MoveRendererToToplevel(moveRendererFrom, moveRendererTo);
            if (fireDesktopRoot)
                self->FireToplevelEvent(sd->toplevelId, "desktop_root", "{}");
        }
        // subsurface: Desktop 模式存 layer 信息, 多窗口模式合成到父 toplevel
        if (sd->isSubsurface && sd->parentSurface && sd->pixels.size() > 0) {
            auto* parentSd = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
            if (parentSd && parentSd->hasToplevel) {
                uint32_t parentId = parentSd->toplevelId;
                if (self->IsDesktopMode()) {
                    bool opaque = shmFormat != 0;
                    if (!opaque) {
                        opaque = true;
                        const uint8_t* pixels = sd->pixels.data();
                        for (size_t i = 3; i < sd->pixels.size(); i += 4) {
                            if (pixels[i] != 0xff) {
                                opaque = false;
                                break;
                            }
                        }
                    }
                    // Desktop 模式: 存 layer, 在 TakeToplevelFrame 中合成 (不进入 per-toplevel 帧缓冲)
                    std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                    const auto* pst = self->FindToplevelLocked(parentId);
                    SubsurfaceLayer layer;
                    layer.surface = surfRes;  // 用 subsurface 自己的 surface 做 key
                    layer.surfaceKey = sd->surfaceKey;
                    layer.w = sd->w;
                    layer.h = sd->h;
                    int32_t sx = sd->subsurfaceX, sy = sd->subsurfaceY;
                    /*
                     * Wine 最小化时 Windows 窗口管理器将窗口移到 (-32000, -32000)。
                     * 此时如果弹出 context menu (如任务栏右键菜单):
                     *   winewayland.drv/wayland_surface.c:745
                     *     local_x = surface->window.rect.left - toplevel->window.rect.left
                     *             = 正常屏幕坐标 - (-32000) = 正常坐标 + 32000
                     * compositor 收到偏了 32000 的 subsurface offset,
                     * 再用 x/y (compositor 位置) 累加 → 菜单跑出屏幕。
                     * 补偿: 检测 offset > 16000 时减去 32000 还原真实偏移。
                     */
                    if (pst && pst->minimized) {
                        if (sx > 16000) sx -= 32000;
                        if (sy > 16000) sy -= 32000;
                    }
                    /*
                     * Wine 和 compositor 有两套坐标系:
                     * - wineX/wineY: Wine 认为的窗口位置 (首次 commit 后不变)
                     * - x/y:         compositor 管理的桌面位置 (move grab 后会变)
                     *
                     * 窗口内菜单: subsurface offset 是相对于窗口内部的 → 跟 compositor 位置
                     * 外部菜单 (如任务栏右击): subsurface offset 是 Wine 虚拟屏幕坐标
                     *   → 用 Wine 原始位置 (不含 move grab 偏移) 避免双重偏移
                     *
                     * 判断方法: offset 是否在窗口内容范围内
                     */
                    int wineX = pst ? pst->wineX : 0;
                    int wineY = pst ? pst->wineY : 0;
                    int compX = pst ? pst->x : 0;
                    int compY = pst ? pst->y : 0;
                    int compW = pst ? pst->w : 0;
                    int compH = pst ? pst->h : 0;
                    bool insideWin = (sx >= 0 && sx < compW && sy >= 0 && sy < compH);
                    layer.isExternal = !insideWin;
                    layer.localX = sx;
                    layer.localY = sy;
                    layer.shmCommitSerial = shmCommitSerial;
                    if (insideWin) {
                        layer.x = compX + sx;
                        layer.y = compY + sy;
                    } else {
                        layer.x = wineX + sx;
                        layer.y = wineY + sy;
                    }
                    layer.parentToplevel = parentId;
                    layer.shmFormat = shmFormat;
                    layer.opaque = opaque;
                    layer.vpDstW = sd->vpDstW; layer.vpDstH = sd->vpDstH;
                    layer.dmgX = sd->damageX; layer.dmgY = sd->damageY;
                    layer.dmgW = sd->damageW; layer.dmgH = sd->damageH;
                    // 替换已有 layer
                    bool found = false;
                    for (auto& l : self->subsurfaceLayers_) {
                        if (l.surface == surfRes) {
                            // Keep two pixel allocations rotating between the Wayland
                            // commit side and compositor side. Moving sd->pixels away
                            // without returning the previous layer buffer caused a
                            // 2 MB allocation and free on every 960x540 GL frame.
                            auto reusablePixels = std::move(l.pixels);
                            l = std::move(layer);
                            l.pixels = std::move(sd->pixels);
                            sd->pixels = std::move(reusablePixels);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        layer.pixels = std::move(sd->pixels);
                        self->subsurfaceLayers_.push_back(std::move(layer));
                    }
                    self->MarkDesktopRootDirtyLocked();
                    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] stored layer %{public}dx%{public}d at (%{public}d,%{public}d) parent=#%{public}u",
                                layer.w, layer.h, layer.x, layer.y, parentId);
                } else {
                    /*
                     * PC 多窗口模式: popup 登记为伪 toplevel, 由 ArkTS 独立
                     * OHOS 子窗口渲染。不再 blit 进父 buffer (会被窗口边缘裁剪)。
                     * 参考 weston/wlroots: subsurface 可越出父 surface 边界,
                     * compositor 不做父边界裁剪。
                     *
                     * wp_viewport: Wine popup 的 shm buffer 常按 2 的幂次对齐
                     * 填充, 大于真实菜单内容。真实显示尺寸 =
                     * min(buffer, set_source, set_destination), 从源矩形原点裁剪
                     * (与 desktop 路径 vpDst clamp / toplevel 的 window_geometry
                     * 裁剪同语义)。
                     *
                     * 风险标注 (P2): 父 toplevel 销毁后 popup 会被级联清理
                     * (OnToplevelDestroyed), 但若该 popup surface 在父窗口销毁后
                     * 恰好又 commit 一帧, 会在此重新登记并向已销毁的 parentId 发
                     * popup_show, ArkTS 侧因窗口不存在而积压 (竞态极小, 仅微量
                     * 内存)。如需根治可在此处检查 parentId 是否仍存活。
                     */
                    int32_t offX = sd->subsurfaceX - parentSd->geoX;
                    int32_t offY = sd->subsurfaceY - parentSd->geoY;
                    int dispW = sd->w, dispH = sd->h;
                    int cropX = 0, cropY = 0;
                    if (sd->vpSrcW > 0 && sd->vpSrcH > 0) {
                        cropX = std::max(0, std::min(sd->vpSrcX, sd->w - 1));
                        cropY = std::max(0, std::min(sd->vpSrcY, sd->h - 1));
                        if (sd->vpSrcW < dispW) dispW = sd->vpSrcW;
                        if (sd->vpSrcH < dispH) dispH = sd->vpSrcH;
                    }
                    if (sd->vpDstW > 0 && sd->vpDstW < dispW) dispW = sd->vpDstW;
                    if (sd->vpDstH > 0 && sd->vpDstH < dispH) dispH = sd->vpDstH;
                    dispW = std::min(dispW, sd->w - cropX);
                    dispH = std::min(dispH, sd->h - cropY);
                    // 防御: pixels 须为完整 w*h*4 (subsurface 若设 window_geometry
                    // 则 sd->w/h 是 content 尺寸而 pixels 是全 buffer, 不成立)
                    const size_t expectSz = static_cast<size_t>(sd->w) * sd->h * 4;
                    if (sd->pixels.size() < expectSz) {
                        OH_LOG_WARN(LOG_APP, "[MW-POPUP] pixels size mismatch: %{public}zu < %{public}zu (w=%{public}d h=%{public}d), skip frame",
                                    sd->pixels.size(), expectSz, sd->w, sd->h);
                    } else if (dispW > 0 && dispH > 0) {
                        uint32_t popupId = 0;
                        bool isNew = false;
                        bool sizeChanged = false;
                        bool posChanged = false;
                        {
                            std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                            auto keyIt = self->popupBySurfaceKey_.find(sd->surfaceKey);
                            if (keyIt == self->popupBySurfaceKey_.end()) {
                                popupId = self->NextToplevelId();
                                isNew = true;
                                PopupRecord rec;
                                rec.popupId = popupId;
                                rec.parentToplevel = parentId;
                                rec.surface = surfRes;
                                rec.surfaceKey = sd->surfaceKey;
                                rec.offX = offX;
                                rec.offY = offY;
                                rec.w = dispW;
                                rec.h = dispH;
                                self->popups_[popupId] = rec;
                                self->popupBySurfaceKey_[sd->surfaceKey] = popupId;
                            } else {
                                popupId = keyIt->second;
                                auto rit = self->popups_.find(popupId);
                                if (rit == self->popups_.end()) {
                                    // 两表不同步 (不应发生): 清孤儿 key, 跳过本帧, 下帧重建
                                    self->popupBySurfaceKey_.erase(keyIt);
                                    popupId = 0;
                                } else {
                                    auto& rec = rit->second;
                                    sizeChanged = (rec.w != dispW || rec.h != dispH);
                                    posChanged = (rec.offX != offX || rec.offY != offY);
                                    rec.offX = offX;
                                    rec.offY = offY;
                                    rec.w = dispW;
                                    rec.h = dispH;
                                }
                            }
                            if (popupId > 0) {
                                auto& pbuf = self->EnsureToplevelLocked(popupId);
                                auto& buf = pbuf.pixels;
                                if (cropX == 0 && cropY == 0 && dispW == sd->w && dispH == sd->h) {
                                    // 无裁剪: 像素双缓冲轮换 (同 desktop layer 做法)
                                    auto reusablePixels = std::move(buf);
                                    buf = std::move(sd->pixels);
                                    sd->pixels = std::move(reusablePixels);
                                } else {
                                    // 裁剪出真实内容区域 (紧凑排列)
                                    buf.resize(static_cast<size_t>(dispW) * dispH * 4);
                                    for (int y = 0; y < dispH; y++) {
                                        std::memcpy(buf.data() + static_cast<size_t>(y) * dispW * 4,
                                                    sd->pixels.data() + (static_cast<size_t>(cropY + y) * sd->w + cropX) * 4,
                                                    static_cast<size_t>(dispW) * 4);
                                    }
                                }
                                pbuf.w = dispW;
                                pbuf.h = dispH;
                                pbuf.dirty = true;
                                pbuf.shmFormat = shmFormat;
                            }
                        }
                        if (popupId == 0) {
                            // 记录异常, 跳过本帧 (下帧按新 popup 重建)
                        } else if (isNew) {
                            {
                                std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
                                self->toplevelSurfaceMap_[popupId] = surfRes;
                            }
                            char json[256];
                            snprintf(json, sizeof(json),
                                     "{\"popupId\":%u,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"argb\":%d}",
                                     popupId, offX, offY, dispW, dispH, shmFormat == 0 ? 1 : 0);
                            OH_LOG_INFO(LOG_APP, "[MW-POPUP] show popup=#%{public}u parent=#%{public}u off=(%{public}d,%{public}d) %{public}dx%{public}d (buffer %{public}dx%{public}d src=%{public}d,%{public}d %{public}dx%{public}d dst=%{public}dx%{public}d)",
                                        popupId, parentId, offX, offY, dispW, dispH, sd->w, sd->h,
                                        sd->vpSrcX, sd->vpSrcY, sd->vpSrcW, sd->vpSrcH, sd->vpDstW, sd->vpDstH);
                            self->FireToplevelEvent(parentId, "popup_show", json);
                        } else {
                            if (sizeChanged) {
                                char json[128];
                                snprintf(json, sizeof(json), "{\"popupId\":%u,\"w\":%d,\"h\":%d}",
                                         popupId, dispW, dispH);
                                self->FireToplevelEvent(parentId, "popup_resize", json);
                            }
                            if (posChanged) {
                                char json[128];
                                snprintf(json, sizeof(json), "{\"popupId\":%u,\"x\":%d,\"y\":%d}",
                                         popupId, offX, offY);
                                self->FireToplevelEvent(parentId, "popup_move", json);
                            }
                        }
                    }
                }
            }
        }
        wl_shm_buffer_end_access(shm);
    }

    // release buffer + frame done
    wl_buffer_send_release(sd->pendingBuffer);

    uint32_t now = static_cast<uint32_t>(time(nullptr) * 1000);
    for (auto* cb : sd->frameCallbacks) {
        wl_callback_send_done(cb, now);
        wl_resource_destroy(cb);
    }
    sd->frameCallbacks.clear();
    sd->pendingBuffer = nullptr;

    // 首帧通知 + 预设 pointer/keyboard focus (参考 HarmonyBox)
    bool expected = false;
    if (GetInstance()->firstFrame_.compare_exchange_strong(expected, true)) {
        GetInstance()->FireState("active");
        // 预设 focus: Wine 在用户操作前就需要 enter
        // 安全检查: 只有 resource 已创建才注入 (否则 Inject*Enter 内部会 DROP)
        uint32_t tl = sd->toplevelId;
        if (Seat::GetInstance()->HasPointerResource()) {
            InputManager::GetInstance()->InjectPointerEnter(tl, surfRes, wl_fixed_from_int(0), wl_fixed_from_int(0));
        }
        if (Seat::GetInstance()->HasKeyboardResource()) {
            InputManager::GetInstance()->InjectKeyboardEnter(tl, surfRes);
        }
    }
}

// -- 帧数据接口 --
void WaylandServer::ResolveSubsurfaceLayerPositionLocked(
    const SubsurfaceLayer& layer, int& x, int& y) const
{
    x = layer.x;
    y = layer.y;
    if (layer.isExternal) return;

    const auto it = toplevels_.find(layer.parentToplevel);
    if (it != toplevels_.end() && it->second.hasPosition) {
        x = it->second.x + layer.localX;
        y = it->second.y + layer.localY;
    }
}

bool WaylandServer::GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                                         ZeroCopyLayerInfo& info)
{
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    const auto resourceIt = surfaceResources_.find(surfaceKey);
    if (resourceIt == surfaceResources_.end()) return false;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(resourceIt->second));
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
        if (desktopMode_)
        {
            if (rendererToplevelId != desktopRootToplevelId_ ||
                (info.parentToplevel != desktopRootToplevelId_ &&
                 !IsToplevelVisibleLocked(info.parentToplevel)))
                return false;
            for (const auto& layer : subsurfaceLayers_)
            {
                if (layer.surface != resourceIt->second) continue;
                ResolveSubsurfaceLayerPositionLocked(layer, info.x, info.y);
                info.width = layer.vpDstW > 0 ? layer.vpDstW : layer.w;
                info.height = layer.vpDstH > 0 ? layer.vpDstH : layer.h;
                info.shmCommitSerial = layer.shmCommitSerial;
                info.desktopCoordinates = true;
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
    if (desktopMode_)
    {
        if (rendererToplevelId != desktopRootToplevelId_ ||
            (sd->toplevelId != desktopRootToplevelId_ && !IsToplevelVisibleLocked(sd->toplevelId)))
            return false;
        if (const auto* st = FindToplevelLocked(sd->toplevelId)) {
            info.x = st->x;
            info.y = st->y;
        }
        info.desktopCoordinates = true;
        return info.width > 0 && info.height > 0;
    }
    return rendererToplevelId == sd->toplevelId && info.width > 0 && info.height > 0;
}

void WaylandServer::SetSurfaceZeroCopy(uint64_t surfaceKey, bool enabled)
{
    if (!surfaceKey) return;
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    if (enabled)
        zeroCopySurfaceKeys_.insert(surfaceKey);
    else
        zeroCopySurfaceKeys_.erase(surfaceKey);
    MarkDesktopRootDirtyLocked();
    desktopCompositionSignature_ = 0;
}

int WaylandServer::GetZeroCopyOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                                        ZeroCopyOccluderRect* out, int maxOut)
{
    if (!out || maxOut <= 0) return 0;
    // 先复用 layer 查询 (内部自行加锁): 非桌面模式 / layer 不可用时无遮挡概念,
    // 返回 0 让渲染器保持 overlay 原样绘制 (与修复前行为一致的安全回退)
    ZeroCopyLayerInfo info;
    if (!GetZeroCopyLayerInfo(surfaceKey, rendererToplevelId, info) ||
        !info.desktopCoordinates)
        return 0;

    // 注意: info 来自上一次加锁, 与本锁期间的 z-order/几何可能差一拍,
    // 最坏情况是一帧的遮挡矩形偏差, 可接受; 不要为此合并成一次加锁去
    // 复制 GetZeroCopyLayerInfo 的整套判定逻辑
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    const int layerL = info.x;
    const int layerT = info.y;
    const int layerR = info.x + info.width;
    const int layerB = info.y + info.height;
    // 遮挡矩形同时裁剪到 root 帧范围内, 保证渲染器换算的纹理 UV 不越界
    const auto* rootSt = FindToplevelLocked(desktopRootToplevelId_);
    if (!rootSt) return 0;  // root 尚无状态 → 无遮挡概念 (旧实现同样返回 0 个矩形)
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

    // toplevelZOrder_ 尾部 = 最顶层 (RaiseToplevel push_back, 任务栏强制最后)。
    // GL 窗口之后的可见 toplevel 都压在它上面; GL 直接挂在 root 上时所有窗口都在其上。
    // 找不到 z 位时 zbegin 保持 begin(): 保守按最底层处理 —— 宁可被挡一帧,
    // 不可错误置顶 (置顶就是本函数要修的那个 bug)
    auto zbegin = toplevelZOrder_.begin();
    if (info.parentToplevel != desktopRootToplevelId_) {
        const auto zit = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(),
                                   info.parentToplevel);
        if (zit != toplevelZOrder_.end()) zbegin = std::next(zit);
    }
    for (auto zit = zbegin; zit != toplevelZOrder_.end() && count < maxOut; ++zit) {
        const uint32_t cid = *zit;
        if (!IsToplevelVisibleLocked(cid)) continue;
        const auto* cst = FindToplevelLocked(cid);
        if (!cst) continue;
        // 全屏窗口显示整幅桌面 (保比例缩放+黑边), 遮挡按整幅算
        if (cst->fullscreen) pushRect(0, 0, rootW, rootH);
        else pushRect(cst->x, cst->y, cst->w, cst->h);
    }

    // popup subsurface 层 (菜单等) 在 CPU 合成中画在所有 toplevel 之后,
    // 与 TakeToplevelFrame 合成循环保持同一可见性条件 —— 因此对 GL overlay
    // 而言它们同样是遮挡者, 无论其父窗口 z 位高低。
    // 遮挡按层完整矩形算: damage 只是增量更新优化, 层在屏幕上占据的是整个矩形
    for (const auto& layer : subsurfaceLayers_) {
        if (count >= maxOut) break;
        if (zeroCopySurfaceKeys_.count(layer.surfaceKey)) continue;
        if (layer.parentToplevel != desktopRootToplevelId_ &&
            !IsToplevelVisibleLocked(layer.parentToplevel)) continue;
        int x = 0, y = 0;
        ResolveSubsurfaceLayerPositionLocked(layer, x, y);
        pushRect(x, y,
                 layer.vpDstW > 0 ? layer.vpDstW : layer.w,
                 layer.vpDstH > 0 ? layer.vpDstH : layer.h);
    }
    return count;
}

bool WaylandServer::TakeFrame(std::vector<uint8_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!dirty_) return false;
    out = pixels_;
    w = width_;
    h = height_;
    dirty_ = false;
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] global frame %{public}dx%{public}d px=%{public}zu", w, h, out.size());
    return true;
}

bool WaylandServer::TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, int& w, int& h) {
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
    std::unique_lock<std::mutex> lk(toplevelMutex_);
    const auto lockAcquired = TakeClock::now();

    // Desktop mode: 合成所有非 root toplevel 到 root framebuffer
    // 因为 Wine 不会在子窗口变化时重新提交桌面 surface，
    // 所以必须手动把子窗口像素覆盖到桌面帧上。
    if (desktopMode_ && id == desktopRootToplevelId_) {
        auto* rst = FindToplevelLocked(id);
        if (!rst || !HasFrame(*rst)) return false;
        if (!rst->dirty) return false;

        int rootW = rst->w;
        int rootH = rst->h;

        // 快速路径: 无子窗口且无 subsurface 层 → 零拷贝直接输出 root pixels
        // 节省每帧 3.7MB 的 vector 分配+拷贝 (1280×725×4 × 60fps ≈ 222MB/s)
        bool hasChildren = false;
        for (uint32_t cid : toplevelZOrder_) {
            if (cid == id) continue;
            const auto* cst = FindToplevelLocked(cid);
            if (cst && HasFrame(*cst)) {
                hasChildren = true;
                break;
            }
        }
        if (!hasChildren && subsurfaceLayers_.empty()) {
            // 拷贝而非 move: 子窗口后续 commit 会设 dirty=true 但不填充 root pixels,
            // move 后 root pixels 为空 → 下次合成时越界 SIGSEGV
            out = rst->pixels;
            w = rootW;
            h = rootH;
            rst->dirty = false;
            return true;
        }

        // 全屏 toplevel: z-order 中最顶层的可见全屏窗口 (同一时刻只有一个生效)。
        // 其绘制 = 整幅填黑 + 内容保比例缩放居中; 几何与 FindInputTargetAt
        // 的输入逆映射共用 ComputeFullscreenTransform, 两边必须是同一套参数
        uint32_t fullscreenId = 0;
        FullscreenTransform transform;
        bool hasFullscreen = false;
        int fullscreenX = 0, fullscreenY = 0;
        for (auto zit = toplevelZOrder_.rbegin(); zit != toplevelZOrder_.rend(); ++zit) {
            const auto* zst = FindToplevelLocked(*zit);
            if (!zst || !zst->fullscreen || !IsToplevelVisibleLocked(*zit)) continue;
            fullscreenId = *zit;
            fullscreenX = zst->x;
            fullscreenY = zst->y;
            hasFullscreen = ComputeFullscreenTransform(rootW, rootH, zst->w, zst->h, transform);
            break;
        }

        // 覆盖检测: 存在不透明 subsurface 层覆盖全屏窗口整个内容区时,
        // children 循环里父帧的缩放可跳过 — 层循环随后会不透明地画满同一矩形,
        // 父帧没有任何像素可见 (游戏场景: 父帧是过渡期陈旧帧, 实时内容全在层上)
        bool fullscreenContentCovered = false;
        if (hasFullscreen) {
            const auto* fst = FindToplevelLocked(fullscreenId);
            const int winW = fst ? fst->w : 0;
            const int winH = fst ? fst->h : 0;
            for (const auto& layer : subsurfaceLayers_) {
                if (layer.parentToplevel != fullscreenId) continue;
                if (zeroCopySurfaceKeys_.count(layer.surfaceKey)) continue;
                if (layer.w <= 0 || layer.h <= 0) continue;
                // 半透明层不算覆盖: 透明处会露出父帧, 父帧必须照常缩放
                if (layer.shmFormat == 0 && !layer.opaque) continue;
                int layerX = 0, layerY = 0;
                ResolveSubsurfaceLayerPositionLocked(layer, layerX, layerY);
                // 与层缩放分支同一套 rel/disp 算法, 保证判定与绘制几何一致
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

        // Rebuild the clean desktop base only when its pixels or composition
        // structure changed. Animated child windows overwrite their complete
        // rectangles, so re-copying the unchanged 4 MB root every frame is
        // unnecessary. Geometry, visibility, Z-order and layer changes are
        // folded into the signature and force a clean rebuild.
        uint64_t compositionSignature = 1469598103934665603ULL;
        auto mixSignature = [&](uint64_t value) {
            compositionSignature ^= value;
            compositionSignature *= 1099511628211ULL;
        };
        mixSignature(id);
        mixSignature(static_cast<uint32_t>(rootW));
        mixSignature(static_cast<uint32_t>(rootH));
        for (uint32_t childId : toplevelZOrder_) {
            mixSignature(childId);
            const bool visible = IsToplevelVisibleLocked(childId);
            mixSignature(visible ? 1 : 0);
            if (!visible) continue;
            const auto* cst = FindToplevelLocked(childId);
            if (!cst) continue;
            mixSignature(static_cast<uint32_t>(cst->x));
            mixSignature(static_cast<uint32_t>(cst->y));
            mixSignature(static_cast<uint32_t>(cst->w));
            mixSignature(static_cast<uint32_t>(cst->h));
            mixSignature(cst->fullscreen ? 1 : 0);  // 全屏进出改变布局, 强制重建基底
        }
        for (const auto& layer : subsurfaceLayers_) {
            int layerX = 0, layerY = 0;
            ResolveSubsurfaceLayerPositionLocked(layer, layerX, layerY);
            mixSignature(reinterpret_cast<uintptr_t>(layer.surface));
            mixSignature(zeroCopySurfaceKeys_.count(layer.surfaceKey) ? 1 : 0);
            mixSignature(layer.parentToplevel);
            mixSignature(layer.parentToplevel == id || IsToplevelVisibleLocked(layer.parentToplevel));
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

        // 按 Z-order 合成: 先合成的在后面, 后合成的覆盖前面。
        // Wine explorer 会创建多个全尺寸 toplevel (#1=背景层, #3=新桌面),
        // #1 需合成到 root 之上以显示桌面背景, 不因同尺寸跳过。
        for (uint32_t childId : toplevelZOrder_) {
            if (!IsToplevelVisibleLocked(childId)) continue;
            auto* cst = FindToplevelLocked(childId);
            if (!cst) continue;
            auto& childPx = cst->pixels;
            int childW = cst->w;
            int childH = cst->h;
            int posX = cst->x;
            int posY = cst->y;
            // 全屏窗口: 填黑 + 内容保比例缩放居中。
            // z 序在它之后的窗口 (如被 pin 的任务栏) 仍画在其上
            if (childId == fullscreenId && hasFullscreen) {
                // 必须填不透明黑 (0xFF000000), 不能图省事 memset 0:
                // 渲染 context 目前不开 GL_BLEND, alpha=0 恰好无害,
                // 但这是一处隐式依赖 — 一旦以后给桌面纹理开混合, 黑边就会变透明
                auto fillBlackRect = [&](int fx, int fy, int fw, int fh) {
                    if (fw <= 0 || fh <= 0) return;
                    for (int row = fy; row < fy + fh; ++row)
                        std::fill_n(reinterpret_cast<uint32_t*>(composited.data()) +
                                    static_cast<size_t>(row) * rootW + fx, fw, 0xFF000000u);
                };
                // 内容区随后必然被不透明写满时 (窗口自身 XRGB, 或被不透明层覆盖),
                // 只需填黑边 (~0.6MB); ARGB 窗口可能有透明洞, 退回整幅填充 (5MB)
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
                // 内容区被不透明层完全覆盖时跳过父帧缩放:
                // 层循环随后会不透明地画满同一矩形, 父帧无像素可见
                if (!fullscreenContentCovered) {
                    BlitScaled(composited.data(), rootW, rootH,
                               childPx.data(), childW, childW, childH,
                               transform.offX, transform.offY, transform.dstW, transform.dstH,
                               cst->shmFormat == 0);
                }
                continue;
            }
            // 计算子窗口在 root framebuffer 中的可见区域
            int dstX = (posX > 0) ? posX : 0;
            int dstY = (posY > 0) ? posY : 0;
            int srcX = (posX < 0) ? -posX : 0;
            int srcY = (posY < 0) ? -posY : 0;
            int copyW = childW - srcX;
            int copyH = childH - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) continue;
            // WL_SHM_FORMAT_ARGB8888=0: layered/shaped (异型) 窗口, 有意义 alpha,
            // 需按预乘 over 混合 (wl_shm alpha 格式均为预乘), 否则透明区域显示黑块
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
                        // 预乘 over: dst = src + dst * (1-a)
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

        // 合成 subsurface 弹出层 (菜单/popup)
        for (auto& layer : subsurfaceLayers_) {
            if (zeroCopySurfaceKeys_.count(layer.surfaceKey)) continue;
            if (layer.parentToplevel != id && !IsToplevelVisibleLocked(layer.parentToplevel)) continue;
            if (layer.w <= 0 || layer.h <= 0) continue;
            // 全屏期间其它窗口的弹出层不绘制 (全屏窗口独占显示,
            // 与 FindInputTargetAt 的输入独占保持一致)
            if (hasFullscreen && layer.parentToplevel != fullscreenId) continue;
            int layerX = 0, layerY = 0;
            ResolveSubsurfaceLayerPositionLocked(layer, layerX, layerY);
            size_t expectSz = (size_t)layer.w * layer.h * 4;
            if (layer.pixels.size() < expectSz) {
                OH_LOG_WARN(LOG_APP, "[MW-SUBSURF] layer size mismatch: w=%{public}d h=%{public}d px=%{public}zu expected=%{public}zu",
                            layer.w, layer.h, layer.pixels.size(), expectSz);
                continue;
            }
            // 全屏窗口的层随窗口一起缩放: 相对窗口原点 → 缩放 → 加黑边偏移。
            // 不做 damage 增量 (缩放后 damage 需重算, 全屏场景通常整帧变化)
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
            // wp_viewport: Wine 用 destination 指定实际显示尺寸 (buffer 可能更大)
            int renderW = copyW, renderH = copyH;
            int renderSrcX = srcX, renderSrcY = srcY;
            int renderDstX = dstX, renderDstY = dstY;
            if (layer.vpDstW > 0 && layer.vpDstW < copyW) renderW = layer.vpDstW;
            if (layer.vpDstH > 0 && layer.vpDstH < copyH) renderH = layer.vpDstH;
            // Intersect damage in source coordinates, then carry the offset
            // into destination coordinates. The old loop sampled the damaged
            // source rectangle but painted it at the layer's top-left corner.
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
            // WL_SHM_FORMAT_ARGB8888=0 (有alpha), WL_SHM_FORMAT_XRGB8888=1 (无alpha)
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
                    // ARGB8888: alpha 混合 (Weston PIXMAN_OP_OVER)
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
                    id, w, h, toplevelZOrder_.size(), subsurfaceLayers_.size());
        return true;
    }

    auto* st = FindToplevelLocked(id);
    if (!st || !st->dirty) return false;
    // 拷贝而非 move: 同 root 快速路径的 SIGSEGV 教训
    out = st->pixels;
    w = st->w;
    h = st->h;
    st->dirty = false;
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu", id, w, h, out.size());
    return true;
}

void WaylandServer::RaiseToplevel(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    auto it = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), id);
    if (it != toplevelZOrder_.end()) toplevelZOrder_.erase(it);
    toplevelZOrder_.push_back(id);
    // 任务栏始终在顶层: 底部对齐 + 高度 <100 的 toplevel;
    // 全屏窗口例外 — 游戏全屏必须压过任务栏
    uint32_t taskbarId = 0;
    for (auto& [tid, st] : toplevels_) {
        if (st.h > 0 && st.h < 100 && st.y + st.h >= outputH_) {
            taskbarId = tid;
            break;
        }
    }
    bool raisedFullscreen = false;
    if (const auto* rst = FindToplevelLocked(id)) raisedFullscreen = rst->fullscreen;
    if (taskbarId > 0 && taskbarId != id && !raisedFullscreen) {
        auto tit = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), taskbarId);
        if (tit != toplevelZOrder_.end()) toplevelZOrder_.erase(tit);
        toplevelZOrder_.push_back(taskbarId);
    }
    MarkDesktopRootDirtyLocked();
}

// -- 交互式窗口移动 (xdg_toplevel.move) --
void WaylandServer::StartMoveGrab(uint32_t toplevelId, uint32_t serial) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    moveGrabToplevelId_ = toplevelId;
    moveGrabSerial_ = serial;
    moveGrabLastWineX_ = 0;
    moveGrabLastWineY_ = 0;
    OH_LOG_INFO(LOG_APP, "[MW-MOVE] start interactive move tl=%{public}u serial=%{public}u",
                toplevelId, serial);
    // PC 模式: 通知 ArkTS, 下一个 TouchMove 时调用 startMoving() 原生拖拽
    if (!IsDesktopMode()) {
        FireToplevelEvent(toplevelId, "move_start");
    }
}

void WaylandServer::EndMoveGrab() {
    OH_LOG_INFO(LOG_APP, "[MW-MOVE] end interactive move tl=%{public}u", moveGrabToplevelId_);
    uint32_t tl = moveGrabToplevelId_;
    {
        std::lock_guard<std::mutex> lk(toplevelMutex_);
        moveGrabToplevelId_ = 0;
        moveGrabSerial_ = 0;
        moveGrabLastWineX_ = 0;
        moveGrabLastWineY_ = 0;
    }
    // PC 模式: 通知 ArkTS 拖拽结束
    if (!IsDesktopMode() && tl != 0) {
        FireToplevelEvent(tl, "move_end");
    }
}

bool WaylandServer::ProcessMoveGrabMotion(wl_fixed_t wx, wl_fixed_t wy) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    uint32_t tl = moveGrabToplevelId_;
    if (tl == 0) return false;
    auto* st = FindToplevelLocked(tl);
    if (!st || !st->hasPosition) return false;

    int32_t rx = wl_fixed_to_int(wx) + st->x;
    int32_t ry = wl_fixed_to_int(wy) + st->y;

    if (moveGrabLastWineX_ == 0 && moveGrabLastWineY_ == 0) {
        // 首帧: 只记录位置, 不移动
        moveGrabLastWineX_ = rx;
        moveGrabLastWineY_ = ry;
        return true;
    }

    int32_t dx = rx - moveGrabLastWineX_;
    int32_t dy = ry - moveGrabLastWineY_;
    if (dx != 0 || dy != 0) {
        st->x += dx;
        st->y += dy;
        moveGrabLastWineX_ = rx;
        moveGrabLastWineY_ = ry;
        MarkDesktopRootDirtyLocked();
        OH_LOG_INFO(LOG_APP, "[MW-MOVE] grab move tl=%{public}u dx=%{public}d dy=%{public}d newPos=(%{public}d,%{public}d)",
                    tl, dx, dy, st->x, st->y);
    }
    return true;
}

void WaylandServer::FireToplevelEvent(uint32_t id, const char* event, const char* jsonData) {
    OH_LOG_INFO(LOG_APP, "[MW] FireToplevel id=%{public}u event=%{public}s data=%{public}s", id, event, jsonData);
    if (toplevelCb_) toplevelCb_(id, event, jsonData);
}

void WaylandServer::SetDesktopRootRecognitionEnabled(bool enabled) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    desktopRootRecognitionEnabled_ = enabled;
    OH_LOG_INFO(LOG_APP, "[MW] desktop root recognition %{public}s",
                enabled ? "enabled" : "disabled");
}

void WaylandServer::PromotePendingDesktopRoot() {
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lk(toplevelMutex_);
        id = pendingDesktopRootToplevelId_;
        auto* pst = id ? FindToplevelLocked(id) : nullptr;
        if (id == 0 || !pst || !HasFrame(*pst)) {
            if (id != 0) {
                OH_LOG_WARN(LOG_APP, "[MW] pending desktop root #%{public}u has no pixels, skip", id);
                pendingDesktopRootToplevelId_ = 0;
            }
            return;
        }
        if (desktopRootToplevelId_ == id) {
            pendingDesktopRootToplevelId_ = 0;
            return;
        }
        if (desktopRootToplevelId_ > 0) {
            if (auto* oldRoot = FindToplevelLocked(desktopRootToplevelId_))
                oldRoot->isBackground = true;
        }
        pst->isBackground = false;
        desktopRootToplevelId_ = id;
        pendingDesktopRootToplevelId_ = 0;
        pst->dirty = true;
    }

    OH_LOG_INFO(LOG_APP, "[MW] pending desktop root promoted: #%{public}u", id);
    PluginManager::GetInstance()->MoveRendererToToplevel(0, id);
    FireToplevelEvent(id, "desktop_root", "{}");
}

void WaylandServer::RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl) {
    std::lock_guard<std::mutex> lk(toplevelResMutex_);
    toplevelResources_[toplevelId] = tl;
    OH_LOG_INFO(LOG_APP, "[MW] RegisterToplevelResource id=%{public}u tl=%{public}p", toplevelId, tl);
}

void WaylandServer::UnregisterToplevelResource(uint32_t toplevelId) {
    std::lock_guard<std::mutex> lk(toplevelResMutex_);
    auto it = toplevelResources_.find(toplevelId);
    if (it != toplevelResources_.end()) {
        OH_LOG_INFO(LOG_APP, "[MW] UnregisterToplevelResource id=%{public}u tl=%{public}p (Wine destroyed toplevel)",
                    toplevelId, it->second);
        toplevelResources_.erase(it);
    }
}

void WaylandServer::OnToplevelDestroyed(uint32_t toplevelId) {
    std::vector<uint32_t> cascadePopups;
    {
        std::lock_guard<std::mutex> lk(toplevelMutex_);
        toplevels_.erase(toplevelId);
        if (pendingDesktopRootToplevelId_ == toplevelId)
            pendingDesktopRootToplevelId_ = 0;
        // root 本体被销毁 (xs_destroy / 客户端断连路径同样走到这里): 复位, 等待下一个 explorer
        if (desktopRootToplevelId_ == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW] desktop root toplevel #%{public}u destroyed, clearing root",
                        toplevelId);
            desktopRootToplevelId_ = 0;
        }
        // 被抓取窗口销毁 → 复位 move grab, 防止悬空 grab 吞掉后续 motion
        if (moveGrabToplevelId_ == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW-MOVE] grabbed toplevel #%{public}u destroyed, reset grab",
                        toplevelId);
            moveGrabToplevelId_ = 0;
            moveGrabSerial_ = 0;
            moveGrabLastWineX_ = 0;
            moveGrabLastWineY_ = 0;
        }
        auto zit = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), toplevelId);
        if (zit != toplevelZOrder_.end()) toplevelZOrder_.erase(zit);
        // 级联清理该 toplevel 的全部 PC popup (帧数据 + 映射)
        for (auto& [pid, rec] : popups_) {
            if (rec.parentToplevel == toplevelId) cascadePopups.push_back(pid);
        }
        for (uint32_t pid : cascadePopups) RemovePopupDataLocked(pid);
        MarkDesktopRootDirtyLocked();  // 非 desktop / root 已复位时 root=0, 自然 no-op
        // 对称清理 surface 映射 (popup 路径在 RemovePopupDataLocked 已清, toplevel 路径此前缺失):
        // xs_destroy 时 wl_surface 可能仍存活, 不清会让 GetSurfaceForToplevel(死 id) 命中
        // 已无 toplevel 身份的 surface。嵌套锁序同 RemovePopupDataLocked。
        {
            std::lock_guard<std::mutex> slk(toplevelSurfaceMutex_);
            toplevelSurfaceMap_.erase(toplevelId);
        }
    }
    // 通知 ArkTS 销毁 popup 子窗口 (锁外触发)
    for (uint32_t pid : cascadePopups) {
        char json[64];
        snprintf(json, sizeof(json), "{\"popupId\":%u}", pid);
        FireToplevelEvent(toplevelId, "popup_hide", json);
    }
}

void WaylandServer::RemovePopupDataLocked(uint32_t popupId) {
    // 调用方须已持有 toplevelMutex_
    auto it = popups_.find(popupId);
    if (it == popups_.end()) return;
    popupBySurfaceKey_.erase(it->second.surfaceKey);
    popups_.erase(it);
    toplevels_.erase(popupId);
    {
        std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
        toplevelSurfaceMap_.erase(popupId);
    }
}

uint32_t WaylandServer::RemovePopupBySurfaceKeyLocked(uint64_t surfaceKey, uint32_t& outPopupId) {
    // 调用方须已持有 toplevelMutex_
    outPopupId = 0;
    auto pit = popupBySurfaceKey_.find(surfaceKey);
    if (pit == popupBySurfaceKey_.end()) return 0;
    auto rit = popups_.find(pit->second);
    if (rit == popups_.end()) {
        // 两表不同步 (不应发生): 清理孤儿 key, 避免静默插入空记录
        popupBySurfaceKey_.erase(pit);
        return 0;
    }
    outPopupId = pit->second;
    const uint32_t parent = rit->second.parentToplevel;
    RemovePopupDataLocked(outPopupId);
    return parent;
}

bool WaylandServer::TakeWindowMask(uint32_t id, int& w, int& h, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    auto* st = FindToplevelLocked(id);
    if (!st || !st->mask.dirty) return false;
    w = st->mask.w;
    h = st->mask.h;
    out = st->mask.bits;
    st->mask.dirty = false;
    return true;
}

void WaylandServer::SendToplevelClose(uint32_t toplevelId) {
    wl_resource* tl = nullptr;
    {
        std::lock_guard<std::mutex> lk(toplevelResMutex_);
        auto it = toplevelResources_.find(toplevelId);
        if (it != toplevelResources_.end()) {
            tl = it->second;
            toplevelResources_.erase(it);
        }
    }
    if (tl) {
        OH_LOG_INFO(LOG_APP, "[MW] SendToplevelClose id=%{public}u -> xdg_toplevel_send_close", toplevelId);
        xdg_toplevel_send_close(tl);
    } else {
        OH_LOG_WARN(LOG_APP, "[MW] SendToplevelClose id=%{public}u NOT found", toplevelId);
    }
}

uint32_t WaylandServer::FindToplevelAt(int x, int y) {
    InputTarget target;
    FindInputTargetAt(x, y, target);
    return target.toplevelId;
}

bool WaylandServer::FindInputTargetAt(int x, int y, InputTarget& out) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    uint32_t rootId = desktopRootToplevelId_;

    /*
     * 全屏窗口独占输入: 命中判定走与渲染相同的保比例缩放几何
     * (ComputeFullscreenTransform, 见 TakeToplevelFrame), 只有该窗口及其
     * subsurface 层可交互; 黑边事件归属全屏窗口并标 swallow
     * (调用方只吞 PRESS, MOVE/RELEASE 照常透传)
     */
    {
        const auto* rootSt = FindToplevelLocked(rootId);
        const int rootW = (rootSt && rootSt->w > 0) ? rootSt->w : outputW_;
        const int rootH = (rootSt && rootSt->h > 0) ? rootSt->h : outputH_;
        for (auto zit = toplevelZOrder_.rbegin(); zit != toplevelZOrder_.rend(); ++zit) {
            const auto* zst = FindToplevelLocked(*zit);
            if (!zst || !zst->fullscreen || !IsToplevelVisibleLocked(*zit)) continue;
            FullscreenTransform transform;
            if (!ComputeFullscreenTransform(rootW, rootH, zst->w, zst->h, transform)) break;
            const uint32_t fullscreenId = *zit;
            // 该窗口的 subsurface 层绘制在窗口内容之上, 先命中 (同一变换)
            for (auto it = subsurfaceLayers_.rbegin(); it != subsurfaceLayers_.rend(); ++it) {
                if (zeroCopySurfaceKeys_.count(it->surfaceKey)) continue;
                if (it->parentToplevel != fullscreenId || it->w <= 0 || it->h <= 0) continue;
                int layerX = 0, layerY = 0;
                ResolveSubsurfaceLayerPositionLocked(*it, layerX, layerY);
                const int layerDispW = it->vpDstW > 0 ? std::min(it->vpDstW, it->w) : it->w;
                const int layerDispH = it->vpDstH > 0 ? std::min(it->vpDstH, it->h) : it->h;
                const int layerScrX = transform.offX + static_cast<int>(lround((layerX - zst->x) * transform.scale));
                const int layerScrY = transform.offY + static_cast<int>(lround((layerY - zst->y) * transform.scale));
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
                out.surface = GetSurfaceForToplevel(fullscreenId);
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
            out.surface = GetSurfaceForToplevel(fullscreenId);
            out.originX = transform.offX;
            out.originY = transform.offY;
            out.scale = static_cast<float>(transform.scale);
            out.swallow = true;
            return out.surface != nullptr;
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
    for (auto it = subsurfaceLayers_.rbegin(); it != subsurfaceLayers_.rend(); ++it) {
        // zero-copy GL 层不参与置顶命中: 渲染时它按窗口 z 位被遮挡重绘压回
        // (egl_renderer occluder redraw), 命中同样交给下方 toplevel z-order 循环,
        // 否则被挡住的 GL 窗口仍会收到点击。
        // 查的是实时集合: GPU→CPU fallback 时 key 被移出 zeroCopySurfaceKeys_,
        // 该层自动恢复为普通 subsurface (CPU 合成置顶, 命中也置顶), 无需特判
        if (zeroCopySurfaceKeys_.count(it->surfaceKey)) continue;
        if (it->parentToplevel != rootId && !IsToplevelVisibleLocked(it->parentToplevel)) continue;
        if (it->w <= 0 || it->h <= 0) continue;
        int layerX = 0, layerY = 0;
        ResolveSubsurfaceLayerPositionLocked(*it, layerX, layerY);
        if (x >= layerX && x < layerX + it->w && y >= layerY && y < layerY + it->h) {
            if (it->isExternal) {
                out.toplevelId = rootId;
                out.surface = GetSurfaceForToplevel(rootId);
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

    for (auto it = toplevelZOrder_.rbegin(); it != toplevelZOrder_.rend(); ++it) {
        uint32_t id = *it;
        if (!IsToplevelVisibleLocked(id)) continue;
        const auto* st = FindToplevelLocked(id);
        if (!st) continue;
        if (x >= st->x && x < st->x + st->w && y >= st->y && y < st->y + st->h) {
            wl_resource* surf = GetSurfaceForToplevel(id);
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
    out.surface = GetSurfaceForToplevel(rootId);
    out.originX = 0;
    out.originY = 0;
    return out.surface != nullptr;
}

bool WaylandServer::IsSurfaceAlive(wl_resource* surface) {
    if (!surface) return false;
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    for (auto& [key, res] : surfaceResources_) {
        if (res == surface) return true;
    }
    return false;
}

bool WaylandServer::IsToplevelVisibleLocked(uint32_t id) {
    // 渲染和输入共用的可见性条件 (调用方须已持有 toplevelMutex_):
    // 非 root + 非背景层 + 有像素 + 非最小化
    if (id == desktopRootToplevelId_) return false;
    const auto* st = FindToplevelLocked(id);
    if (!st) return false;
    return !st->isBackground && HasFrame(*st) && !st->minimized;
}

int32_t WaylandServer::GetWorkAreaHeight() {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    int32_t h = outputH_;
    // 找底部对齐的小高度 toplevel (任务栏), 工作区 = 任务栏上方空间
    for (auto& [id, st] : toplevels_) {
        if (st.h > 0 && st.h < 100 && st.y + st.h >= outputH_ && st.y < h) {
            h = st.y;
        }
    }
    return h;
}

void WaylandServer::SetToplevelMinimized(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    // 保留 operator[] 建档语义: pre-commit 最小化同样记录状态
    EnsureToplevelLocked(id).minimized = true;
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::SetToplevelRestored(uint32_t id) {
    // 清除 minimized 状态
    {
        std::lock_guard<std::mutex> lk(toplevelMutex_);
        if (auto* st = FindToplevelLocked(id)) st->minimized = false;
        MarkDesktopRootDirtyLocked();
    }
    // 发 configure 通知 Wine (如果 toplevel resource 存在)
    wl_resource* tl = nullptr;
    {
        std::lock_guard<std::mutex> lk(toplevelResMutex_);
        auto it = toplevelResources_.find(id);
        if (it != toplevelResources_.end()) tl = it->second;
    }
    if (!tl) return;
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (sd) sd->minimized = false;
    wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    // 全屏窗口从最小化还原: 维持 FULLSCREEN 状态 (尺寸 0,0 = Wine 保持当前尺寸)
    if (sd && sd->fullscreen) {
        st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *st = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    xdg_toplevel_send_configure(tl, 0, 0, &states);
    wl_array_release(&states);
    wl_client* client = wl_resource_get_client(tl);
    wl_display* dpy = wl_client_get_display(client);
    xdg_surface_send_configure(xdg->xdgSurface, wl_display_next_serial(dpy));
}

void WaylandServer::SetToplevelMaximized(uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[MW] SetToplevelMaximized id=%{public}u desktop=%{public}s",
                id, IsDesktopMode() ? "yes" : "no");
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    if (auto* st = FindToplevelLocked(id); st && st->hasPosition) {
        st->x = 0;
        st->y = 0;
    }
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::SetToplevelFullscreen(uint32_t id, bool on) {
    OH_LOG_INFO(LOG_APP, "[MW] SetToplevelFullscreen id=%{public}u on=%{public}s",
                id, on ? "yes" : "no");
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    // Ensure 建档语义同 SetToplevelMinimized: pre-commit 全屏同样记录状态
    auto& st = EnsureToplevelLocked(id);
    st.fullscreen = on;
    // 全屏窗口锚定桌面原点: 合成按保比例缩放铺满, 不再使用浮动位置
    if (on && st.hasPosition) {
        st.x = 0;
        st.y = 0;
    }
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::ForceToplevelRedraw(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    if (auto* st = FindToplevelLocked(id)) st->dirty = true;
}

void WaylandServer::NotifyToplevelResize(uint32_t toplevelId, int32_t w, int32_t h) {
    wl_resource* tl = nullptr;
    {
        std::lock_guard<std::mutex> lk(toplevelResMutex_);
        auto it = toplevelResources_.find(toplevelId);
        if (it != toplevelResources_.end()) tl = it->second;
    }
    if (!tl) return;

    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg) return;

    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));

    OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize IN id=%{public}u %{public}dx%{public}d pc=%{public}s max=%{public}s",
                toplevelId, w, h,
                IsDesktopMode() ? "no" : "yes",
                (sd && sd->maximized) ? "yes" : "no");

    wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    if (sd && sd->maximized) {
        st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *st = XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    // 全屏窗口在 OHOS 侧尺寸变化时保持 FULLSCREEN 状态, 否则 Wine 会退出全屏
    if (sd && sd->fullscreen) {
        st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        *st = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    xdg_toplevel_send_configure(tl, w, h, &states);
    wl_array_release(&states);

    wl_client* client = wl_resource_get_client(tl);
    wl_display* dpy = wl_client_get_display(client);
    xdg_surface_send_configure(xdg->xdgSurface, wl_display_next_serial(dpy));

    // 桌面 root 尺寸变化 → 同步更新 output 尺寸, 影响:
    //   - wl_output 上报的物理尺寸
    //   - xdg_toplevel_set_maximized / set_max_size 的基准值
    //   - FindToplevelAt / RaiseToplevel 的边界判断
    if (IsDesktopMode() && toplevelId == desktopRootToplevelId_) {
        SetOutputSize(w, h);
        OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize root=%{public}u → output %{public}dx%{public}d",
                    toplevelId, w, h);
    } else {
        OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize id=%{public}u → %{public}dx%{public}d maximized=%{public}s",
                    toplevelId, w, h, (sd && sd->maximized) ? "yes" : "no");
    }
}

// -- toplevelId -> wl_surface 映射 (供 Seat::InjectPointerEnter 查找) --
wl_resource* WaylandServer::GetSurfaceForToplevel(uint32_t toplevelId) {
    std::lock_guard<std::mutex> lk(toplevelSurfaceMutex_);
    auto it = toplevelSurfaceMap_.find(toplevelId);
    if (it != toplevelSurfaceMap_.end()) return it->second;
    OH_LOG_WARN(LOG_APP, "[MW] GetSurfaceForToplevel #%{public}u NOT FOUND (map size=%{public}zu)",
                toplevelId, toplevelSurfaceMap_.size());
    return nullptr;
}
