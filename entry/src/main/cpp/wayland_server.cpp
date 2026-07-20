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
    OH_LOG_INFO(LOG_APP,
                "[MW-ZC] surface created pid=%{public}u surface=%{public}u "
                "key=%{public}llu resource=%{public}p",
                sd->clientPid, sd->protocolId,
                static_cast<unsigned long long>(sd->surfaceKey), surfRes);
    wl_resource_set_implementation(surfRes, &kSurfaceImpl, sd, [](wl_resource* r) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(r));
        auto* self = GetInstance();
        {
            std::lock_guard<std::mutex> lk(self->toplevelMutex_);
            const uint64_t surfaceKey = sd ? sd->surfaceKey : 0;
            self->surfaceResources_.erase(surfaceKey);
            self->zeroCopySurfaceKeys_.erase(surfaceKey);
            self->zeroCopyProtocolGeometryLogged_.erase(surfaceKey);
            for (auto it = self->subsurfaceLayers_.begin(); it != self->subsurfaceLayers_.end(); ) {
                if (it->surface == r) it = self->subsurfaceLayers_.erase(it);
                else ++it;
            }
            if (self->desktopRootToplevelId_ > 0)
                self->toplevelDirty_[self->desktopRootToplevelId_] = true;
        }
        if (sd && sd->hasToplevel) {
            {
                std::lock_guard<std::mutex> lk(self->toplevelSurfaceMutex_);
                self->toplevelSurfaceMap_.erase(sd->toplevelId);
            }
            InputManager::GetInstance()->OnSurfaceDestroyed(r);
            self->FireToplevelEvent(sd->toplevelId, "destroyed");
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
        if (self->desktopRootToplevelId_ > 0)
            self->toplevelDirty_[self->desktopRootToplevelId_] = true;
    }
    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] set_position: child=%{public}p parent=%{public}p pos=(%{public}d,%{public}d)",
                childSurf, sd->parentSurface, x, y);
}

void WaylandServer::subsurface_place_above(wl_client*, wl_resource* ssRes, wl_resource* sibling) {
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
    if (self->IsDesktopMode() && self->desktopRootToplevelId_ > 0)
        self->toplevelDirty_[self->desktopRootToplevelId_] = true;
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
    if (self->IsDesktopMode() && self->desktopRootToplevelId_ > 0)
        self->toplevelDirty_[self->desktopRootToplevelId_] = true;
    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] place_below child=%{public}p below sibling=%{public}p",
                childSurf, sibling);
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
        self->OnToplevelDestroyed(sd->toplevelId);
        // 重置 InputManager 焦点: 防止后续 Inject*Leave 引用已销毁的 surface
        // (否则 Wine 收到 invalid object 协议错误 → 断开连接)
        InputManager::GetInstance()->OnSurfaceDestroyed(r);
        // 如果是 desktop root 被销毁，重置 root ID，等待下一个 explorer toplevel
        if (sd->toplevelId == self->GetDesktopRootToplevelId()) {
            OH_LOG_INFO(LOG_APP, "[MW] desktop root toplevel #%{public}u destroyed, clearing root",
                        sd->toplevelId);
            self->SetDesktopRootToplevelId(0);
        }
        self->FireToplevelEvent(sd->toplevelId, "destroyed");
    }
    // subsurface 销毁: 清除 layer + 标记 root dirty 触发重绘 (移除残留像素)
    if (sd && sd->isSubsurface) {
        auto* self = GetInstance();
        std::lock_guard<std::mutex> lk(self->toplevelMutex_);
        for (auto it = self->subsurfaceLayers_.begin(); it != self->subsurfaceLayers_.end(); ) {
            if (it->surface == r) it = self->subsurfaceLayers_.erase(it);
            else ++it;
        }
        if (self->IsDesktopMode() && self->desktopRootToplevelId_ > 0)
            self->toplevelDirty_[self->desktopRootToplevelId_] = true;
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
    // NULL buffer → surface 无内容: 清除对应 subsurface layer
    if (!sd->pendingBuffer) {
        if (sd->isSubsurface) {
            auto* self = GetInstance();
            std::lock_guard<std::mutex> lk(self->toplevelMutex_);
            size_t before = self->subsurfaceLayers_.size();
            for (auto it = self->subsurfaceLayers_.begin(); it != self->subsurfaceLayers_.end(); ) {
                if (it->surface == surfRes) it = self->subsurfaceLayers_.erase(it);
                else ++it;
            }
            if (self->subsurfaceLayers_.size() != before) {
                OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] NULL buffer commit → removed layer, layers %{public}zu→%{public}zu",
                            before, self->subsurfaceLayers_.size());
                if (self->IsDesktopMode() && self->desktopRootToplevelId_ > 0)
                    self->toplevelDirty_[self->desktopRootToplevelId_] = true;
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
            OH_LOG_INFO(LOG_APP, "[MW-GEO] using window_geometry: src=%{public}dx%{public}d geo=(%{public}d,%{public}d %{public}dx%{public}d) screen=(%{public}d,%{public}d)",
                        w, h, contentOffX, contentOffY, contentW, contentH, screenX, screenY);
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
            copyTight(self->toplevelPixels_[sd->toplevelId]);
            self->toplevelW_[sd->toplevelId] = contentW;
            self->toplevelH_[sd->toplevelId] = contentH;
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
                self->toplevelMinimized_.erase(sd->toplevelId);
                self->toplevelMinimizeCompX_.erase(sd->toplevelId);
                self->toplevelMinimizeCompY_.erase(sd->toplevelId);
                self->toplevelMinimizeTimeMs_.erase(sd->toplevelId);
                OH_LOG_INFO(LOG_APP, "[MW] auto-restore tl=%{public}u size=%{public}dx%{public}d",
                            sd->toplevelId, contentW, contentH);
            }
            isFirstCommit = (self->toplevelX_.count(sd->toplevelId) == 0);
            if (isFirstCommit) {
                self->toplevelX_[sd->toplevelId] = screenX;
                self->toplevelY_[sd->toplevelId] = screenY;
                self->toplevelWineX_[sd->toplevelId] = screenX;
                self->toplevelWineY_[sd->toplevelId] = screenY;
                OH_LOG_INFO(LOG_APP, "[MW-MOVE] initial pos tl=%{public}u (%{public}d,%{public}d)",
                    sd->toplevelId, screenX, screenY);
            }
            // 后续 commit: 忽略 geoX/geoY, compositor 位置为权威
            self->toplevelDirty_[sd->toplevelId] = true;
            // 新 toplevel 加到 Z-order 顶层
            if (self->IsDesktopMode() && sd->toplevelId != self->desktopRootToplevelId_) {
                auto zit = std::find(self->toplevelZOrder_.begin(), self->toplevelZOrder_.end(), sd->toplevelId);
                if (zit == self->toplevelZOrder_.end()) self->toplevelZOrder_.push_back(sd->toplevelId);
            }
            OH_LOG_INFO(LOG_APP, "[MW-COMMIT] toplevel #%{public}u frame %{public}dx%{public}d stride=%{public}d stored=%{public}zu",
                        sd->toplevelId, contentW, contentH, stride, self->toplevelPixels_[sd->toplevelId].size());

            // 检测尺寸变化 -> 通知 ArkTS 调整子窗口
            int lastW = self->toplevelLastReportedW_[sd->toplevelId];
            int lastH = self->toplevelLastReportedH_[sd->toplevelId];
            if (contentW != lastW || contentH != lastH) {
                self->toplevelLastReportedW_[sd->toplevelId] = contentW;
                self->toplevelLastReportedH_[sd->toplevelId] = contentH;
                char json[64];
                snprintf(json, sizeof(json), "{\"w\":%d,\"h\":%d}", contentW, contentH);
                OH_LOG_INFO(LOG_APP, "[MW] toplevel #%{public}u size changed: %{public}dx%{public}d max=%{public}s -> ArkTS",
                            sd->toplevelId, contentW, contentH,
                            sd->maximized ? "yes" : "no");
                self->FireToplevelEvent(sd->toplevelId, "resize", json);
            }
        }
        // Desktop 模式子窗口 commit → 标记 root dirty
        if (self->IsDesktopMode() && sd->hasToplevel &&
            sd->toplevelId != self->GetDesktopRootToplevelId()) {
            uint32_t rootId = self->GetDesktopRootToplevelId();
            // 仅首次 commit 识别 explorer 桌面 (防止最大化误切 root)
            if (isFirstCommit) {
                bool isExplorer = (sd->appId.find("explorer") != std::string::npos);
                bool isFullSize = (contentW >= self->outputW_ * 8 / 10 &&
                                   contentH >= self->outputH_ * 8 / 10);

                if (isExplorer && isFullSize) {
                    if (!self->desktopRootRecognitionEnabled_) {
                        if (!sd->title.empty()) {
                            self->pendingDesktopRootToplevelId_ = sd->toplevelId;
                            self->backgroundLayers_.erase(sd->toplevelId);
                            OH_LOG_INFO(LOG_APP,
                                        "[MW] full-size explorer #%{public}u pending as desktop root while recognition is disabled title=%{public}s",
                                        sd->toplevelId, sd->title.c_str());
                        } else {
                            self->backgroundLayers_.insert(sd->toplevelId);
                            OH_LOG_INFO(LOG_APP,
                                        "[MW] full-size explorer #%{public}u ignored while desktop root recognition is disabled (no title)",
                                        sd->toplevelId);
                        }
                    } else if (rootId == 0) {
                        if (self->pendingDesktopRootToplevelId_ > 0 &&
                            self->pendingDesktopRootToplevelId_ != sd->toplevelId) {
                            self->backgroundLayers_.insert(sd->toplevelId);
                            OH_LOG_INFO(LOG_APP,
                                        "[MW] full-size explorer #%{public}u -> background, pending root #%{public}u exists",
                                        sd->toplevelId, self->pendingDesktopRootToplevelId_);
                        } else {
                            OH_LOG_INFO(LOG_APP, "[MW] desktop root: #%{public}u appId=explorer",
                                        sd->toplevelId);
                            PluginManager::GetInstance()->MoveRendererToToplevel(0, sd->toplevelId);
                            self->SetDesktopRootToplevelId(sd->toplevelId);
                            self->pendingDesktopRootToplevelId_ = 0;
                            self->FireToplevelEvent(sd->toplevelId, "desktop_root", "{}");
                            rootId = sd->toplevelId;
                        }
                    } else if (!sd->title.empty()) {
                        wl_resource* oldSurf = self->GetSurfaceForToplevel(rootId);
                        auto* oldSd = oldSurf ? static_cast<SurfaceData*>(wl_resource_get_user_data(oldSurf)) : nullptr;
                        if (oldSd && oldSd->title.empty()) {
                            OH_LOG_INFO(LOG_APP, "[MW] root switch: #%{public}u (empty) -> #%{public}u (%{public}s)",
                                        rootId, sd->toplevelId, sd->title.c_str());
                            self->backgroundLayers_.insert(rootId);
                            PluginManager::GetInstance()->MoveRendererToToplevel(rootId, sd->toplevelId);
                            self->SetDesktopRootToplevelId(sd->toplevelId);
                            self->FireToplevelEvent(sd->toplevelId, "desktop_root", "{}");
                            rootId = sd->toplevelId;
                        } else {
                            self->backgroundLayers_.insert(sd->toplevelId);
                            OH_LOG_INFO(LOG_APP, "[MW] extra full-size explorer #%{public}u -> background",
                                        sd->toplevelId);
                        }
                    } else {
                        self->backgroundLayers_.insert(sd->toplevelId);
                        OH_LOG_INFO(LOG_APP, "[MW] extra full-size explorer #%{public}u (no title) -> background",
                                    sd->toplevelId);
                    }
                }
            }
            // 每次子窗口 commit 都标记 root dirty (包括 resize)
            if (rootId > 0) {
                std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                self->toplevelDirty_[rootId] = true;
            }
        }
        // subsurface: Desktop 模式存 layer 信息, 多窗口模式合成到父 toplevel
        if (sd->isSubsurface && sd->parentSurface && sd->pixels.size() > 0) {
            auto* parentSd = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
            if (parentSd && parentSd->hasToplevel) {
                uint32_t parentId = parentSd->toplevelId;
                if (self->IsDesktopMode()) {
                    const uint32_t shmFormat = wl_shm_buffer_get_format(shm);
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
                    // Desktop 模式: 存 layer, 在 TakeToplevelFrame 中合成 (不污染 toplevelPixels_)
                    std::lock_guard<std::mutex> lk(self->toplevelMutex_);
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
                     * 再用 toplevelX_ (compositor 位置) 累加 → 菜单跑出屏幕。
                     * 补偿: 检测 offset > 16000 时减去 32000 还原真实偏移。
                     */
                    if (self->toplevelMinimized_.count(parentId)) {
                        if (sx > 16000) sx -= 32000;
                        if (sy > 16000) sy -= 32000;
                    }
                    /*
                     * Wine 和 compositor 有两套坐标系:
                     * - toplevelWineX_/Y_: Wine 认为的窗口位置 (首次 commit 后不变)
                     * - toplevelX_/Y_:     compositor 管理的桌面位置 (move grab 后会变)
                     *
                     * 窗口内菜单: subsurface offset 是相对于窗口内部的 → 跟 compositor 位置
                     * 外部菜单 (如任务栏右击): subsurface offset 是 Wine 虚拟屏幕坐标
                     *   → 用 Wine 原始位置 (不含 move grab 偏移) 避免双重偏移
                     *
                     * 判断方法: offset 是否在窗口内容范围内
                     */
                    int wineX = (self->toplevelWineX_.count(parentId) ? self->toplevelWineX_[parentId] : 0);
                    int wineY = (self->toplevelWineY_.count(parentId) ? self->toplevelWineY_[parentId] : 0);
                    int compX = self->toplevelX_[parentId];
                    int compY = self->toplevelY_[parentId];
                    int compW = self->toplevelW_[parentId];
                    int compH = self->toplevelH_[parentId];
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
                    self->toplevelDirty_[self->desktopRootToplevelId_] = true;
                    OH_LOG_INFO(LOG_APP, "[MW-SUBSURF] stored layer %{public}dx%{public}d at (%{public}d,%{public}d) parent=#%{public}u",
                                layer.w, layer.h, layer.x, layer.y, parentId);
                } else {
                    // 多窗口模式: 直接合成到父 toplevel
                    std::lock_guard<std::mutex> lk(self->toplevelMutex_);
                    auto it = self->toplevelPixels_.find(parentId);
                    if (it != self->toplevelPixels_.end()) {
                        int parentW = self->toplevelW_[parentId];
                        int parentH = self->toplevelH_[parentId];
                        int popupX = sd->subsurfaceX - parentSd->geoX;
                        int popupY = sd->subsurfaceY - parentSd->geoY;
                        int srcX = (popupX < 0) ? -popupX : 0;
                        int srcY = (popupY < 0) ? -popupY : 0;
                        int dstX = (popupX > 0) ? popupX : 0;
                        int dstY = (popupY > 0) ? popupY : 0;
                        // wp_viewport: destination 指定实际显示尺寸
                        int copyW = sd->w - srcX;
                        int copyH = sd->h - srcY;
                        if (sd->vpDstW > 0 && sd->vpDstW < copyW) copyW = sd->vpDstW;
                        if (sd->vpDstH > 0 && sd->vpDstH < copyH) copyH = sd->vpDstH;
                        if (dstX + copyW > parentW) copyW = parentW - dstX;
                        if (dstY + copyH > parentH) copyH = parentH - dstY;
                        if (copyW > 0 && copyH > 0) {
                            auto& targetBuf = it->second;
                            for (int y = 0; y < copyH; y++) {
                                int srcRowStart = ((srcY + y) * sd->w + srcX) * 4;
                                int dstRowStart = ((dstY + y) * parentW + dstX) * 4;
                                std::memcpy(&targetBuf[dstRowStart], &sd->pixels[srcRowStart], copyW * 4);
                            }
                            self->toplevelDirty_[parentId] = true;
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

    const auto xIt = toplevelX_.find(layer.parentToplevel);
    const auto yIt = toplevelY_.find(layer.parentToplevel);
    if (xIt != toplevelX_.end()) x = xIt->second + layer.localX;
    if (yIt != toplevelY_.end()) y = yIt->second + layer.localY;
}

bool WaylandServer::GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                                         int fallbackWidth, int fallbackHeight,
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
                 !IsToplevelVisible(info.parentToplevel)))
                return false;
            for (const auto& layer : subsurfaceLayers_)
            {
                if (layer.surface != resourceIt->second) continue;
                ResolveSubsurfaceLayerPositionLocked(layer, info.x, info.y);
                info.width = layer.vpDstW > 0 ? layer.vpDstW : layer.w;
                info.height = layer.vpDstH > 0 ? layer.vpDstH : layer.h;
                info.shmCommitSerial = layer.shmCommitSerial;
                info.desktopCoordinates = true;
                info.protocolOnly = false;
                return info.width > 0 && info.height > 0;
            }

            /*
             * A Vulkan private-present surface can be a Wayland subsurface
             * without ever committing a wl_shm buffer. The present protocol
             * supplies the image dimensions, while Wayland still supplies
             * the parent and offset.
             */
            int sx = sd->subsurfaceX;
            int sy = sd->subsurfaceY;
            if (toplevelMinimized_.count(info.parentToplevel)) {
                if (sx > 16000) sx -= 32000;
                if (sy > 16000) sy -= 32000;
            }
            const int compX = toplevelX_.count(info.parentToplevel)
                ? toplevelX_.at(info.parentToplevel) : 0;
            const int compY = toplevelY_.count(info.parentToplevel)
                ? toplevelY_.at(info.parentToplevel) : 0;
            const int wineX = toplevelWineX_.count(info.parentToplevel)
                ? toplevelWineX_.at(info.parentToplevel) : 0;
            const int wineY = toplevelWineY_.count(info.parentToplevel)
                ? toplevelWineY_.at(info.parentToplevel) : 0;
            const int compW = toplevelW_.count(info.parentToplevel)
                ? toplevelW_.at(info.parentToplevel) : 0;
            const int compH = toplevelH_.count(info.parentToplevel)
                ? toplevelH_.at(info.parentToplevel) : 0;
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
            if (zeroCopyProtocolGeometryLogged_.insert(surfaceKey).second) {
                OH_LOG_INFO(LOG_APP,
                            "[MW-ZC] protocol-only geometry key=%{public}llu "
                            "pid=%{public}u surface=%{public}u parent=%{public}u "
                            "offset=%{public}d,%{public}d layer=%{public}dx%{public}d "
                            "fallback=%{public}dx%{public}d",
                            static_cast<unsigned long long>(surfaceKey),
                            info.clientPid, info.surfaceId, info.parentToplevel,
                            sx, sy, info.width, info.height,
                            fallbackWidth, fallbackHeight);
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
    if (desktopMode_)
    {
        if (rendererToplevelId != desktopRootToplevelId_ ||
            (sd->toplevelId != desktopRootToplevelId_ && !IsToplevelVisible(sd->toplevelId)))
            return false;
        info.x = toplevelX_[sd->toplevelId];
        info.y = toplevelY_[sd->toplevelId];
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
    if (desktopRootToplevelId_ > 0)
        toplevelDirty_[desktopRootToplevelId_] = true;
    desktopCompositionSignature_ = 0;
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
        auto rit = toplevelPixels_.find(id);
        if (rit == toplevelPixels_.end()) return false;

        auto dit = toplevelDirty_.find(id);
        if (dit == toplevelDirty_.end() || !dit->second) return false;

        int rootW = toplevelW_[id];
        int rootH = toplevelH_[id];

        // 快速路径: 无子窗口且无 subsurface 层 → 零拷贝直接输出 root pixels
        // 节省每帧 3.7MB 的 vector 分配+拷贝 (1280×725×4 × 60fps ≈ 222MB/s)
        bool hasChildren = false;
        for (uint32_t cid : toplevelZOrder_) {
            if (cid != id && toplevelPixels_.count(cid)) {
                hasChildren = true;
                break;
            }
        }
        if (!hasChildren && subsurfaceLayers_.empty()) {
            // 拷贝而非 move: 子窗口后续 commit 会设 dirty=true 但不填充 root pixels,
            // move 后 root pixels 为空 → 下次合成时越界 SIGSEGV
            out = rit->second;
            w = rootW;
            h = rootH;
            toplevelDirty_[id] = false;
            return true;
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
            const bool visible = IsToplevelVisible(childId);
            mixSignature(visible ? 1 : 0);
            if (!visible) continue;
            mixSignature(static_cast<uint32_t>(toplevelX_[childId]));
            mixSignature(static_cast<uint32_t>(toplevelY_[childId]));
            mixSignature(static_cast<uint32_t>(toplevelW_[childId]));
            mixSignature(static_cast<uint32_t>(toplevelH_[childId]));
        }
        for (const auto& layer : subsurfaceLayers_) {
            int layerX = 0, layerY = 0;
            ResolveSubsurfaceLayerPositionLocked(layer, layerX, layerY);
            mixSignature(reinterpret_cast<uintptr_t>(layer.surface));
            mixSignature(zeroCopySurfaceKeys_.count(layer.surfaceKey) ? 1 : 0);
            mixSignature(layer.parentToplevel);
            mixSignature(layer.parentToplevel == id || IsToplevelVisible(layer.parentToplevel));
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
            out = rit->second;
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
            if (!IsToplevelVisible(childId)) continue;
            int cw = toplevelW_[childId], ch = toplevelH_[childId];
            auto& childPx = toplevelPixels_[childId];
            int childW = toplevelW_[childId];
            int childH = toplevelH_[childId];
            int posX = toplevelX_[childId];
            int posY = toplevelY_[childId];
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
            for (int y = 0; y < copyH; y++) {
                auto* srcRow = &childPx[(srcY + y) * childW * 4];
                auto* dstRow = &composited[(dstY + y) * rootW * 4];
                // toplevel 窗口使用 XRGB8888 (alpha 字节=0)，不加 alpha 过滤
                memcpy(&dstRow[dstX * 4], &srcRow[srcX * 4], copyW * 4);
            }
        }
        const auto childrenComposited = TakeClock::now();

        // 合成 subsurface 弹出层 (菜单/popup)
        for (auto& layer : subsurfaceLayers_) {
            if (zeroCopySurfaceKeys_.count(layer.surfaceKey)) continue;
            if (layer.parentToplevel != id && !IsToplevelVisible(layer.parentToplevel)) continue;
            if (layer.w <= 0 || layer.h <= 0) continue;
            int layerX = 0, layerY = 0;
            ResolveSubsurfaceLayerPositionLocked(layer, layerX, layerY);
            size_t expectSz = (size_t)layer.w * layer.h * 4;
            if (layer.pixels.size() < expectSz) {
                OH_LOG_WARN(LOG_APP, "[MW-SUBSURF] layer size mismatch: w=%{public}d h=%{public}d px=%{public}zu expected=%{public}zu",
                            layer.w, layer.h, layer.pixels.size(), expectSz);
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
        toplevelDirty_[id] = false;
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] root #%{public}u %{public}dx%{public}d children=%{public}zu subsurfaces=%{public}zu",
                    id, w, h, toplevelZOrder_.size(), subsurfaceLayers_.size());
        return true;
    }

    auto it = toplevelDirty_.find(id);
    if (it == toplevelDirty_.end() || !it->second) return false;
    out = toplevelPixels_[id];
    w = toplevelW_[id];
    h = toplevelH_[id];
    toplevelDirty_[id] = false;
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu", id, w, h, out.size());
    return true;
}

void WaylandServer::RaiseToplevel(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    auto it = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), id);
    if (it != toplevelZOrder_.end()) toplevelZOrder_.erase(it);
    toplevelZOrder_.push_back(id);
    // 任务栏始终在顶层: 底部对齐 + 高度 <100 的 toplevel
    uint32_t taskbarId = 0;
    for (auto& [tid, th] : toplevelH_) {
        if (th > 0 && th < 100 && toplevelY_[tid] + th >= outputH_) {
            taskbarId = tid;
            break;
        }
    }
    if (taskbarId > 0 && taskbarId != id) {
        auto tit = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), taskbarId);
        if (tit != toplevelZOrder_.end()) toplevelZOrder_.erase(tit);
        toplevelZOrder_.push_back(taskbarId);
    }
    toplevelDirty_[desktopRootToplevelId_] = true;
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
    auto xit = toplevelX_.find(tl);
    if (xit == toplevelX_.end()) return false;

    int32_t rx = wl_fixed_to_int(wx) + xit->second;
    int32_t ry = wl_fixed_to_int(wy) + toplevelY_[tl];

    if (moveGrabLastWineX_ == 0 && moveGrabLastWineY_ == 0) {
        // 首帧: 只记录位置, 不移动
        moveGrabLastWineX_ = rx;
        moveGrabLastWineY_ = ry;
        return true;
    }

    int32_t dx = rx - moveGrabLastWineX_;
    int32_t dy = ry - moveGrabLastWineY_;
    if (dx != 0 || dy != 0) {
        xit->second += dx;
        toplevelY_[tl] += dy;
        moveGrabLastWineX_ = rx;
        moveGrabLastWineY_ = ry;
        toplevelDirty_[desktopRootToplevelId_] = true;
        OH_LOG_INFO(LOG_APP, "[MW-MOVE] grab move tl=%{public}u dx=%{public}d dy=%{public}d newPos=(%{public}d,%{public}d)",
                    tl, dx, dy, xit->second, toplevelY_[tl]);
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
        if (id == 0 || toplevelPixels_.find(id) == toplevelPixels_.end()) {
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
        if (desktopRootToplevelId_ > 0)
            backgroundLayers_.insert(desktopRootToplevelId_);
        backgroundLayers_.erase(id);
        desktopRootToplevelId_ = id;
        pendingDesktopRootToplevelId_ = 0;
        toplevelDirty_[id] = true;
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
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    toplevelPixels_.erase(toplevelId);
    toplevelW_.erase(toplevelId);
    toplevelH_.erase(toplevelId);
    toplevelX_.erase(toplevelId);
    toplevelY_.erase(toplevelId);
    toplevelWineX_.erase(toplevelId);
    toplevelWineY_.erase(toplevelId);
    toplevelDirty_.erase(toplevelId);
    toplevelMinimized_.erase(toplevelId);
    toplevelMinimizeCompX_.erase(toplevelId);
    toplevelMinimizeCompY_.erase(toplevelId);
    toplevelMinimizeTimeMs_.erase(toplevelId);
    backgroundLayers_.erase(toplevelId);
    if (pendingDesktopRootToplevelId_ == toplevelId)
        pendingDesktopRootToplevelId_ = 0;
    auto zit = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), toplevelId);
    if (zit != toplevelZOrder_.end()) toplevelZOrder_.erase(zit);
    if (IsDesktopMode() && toplevelId != desktopRootToplevelId_) {
        if (desktopRootToplevelId_ > 0) toplevelDirty_[desktopRootToplevelId_] = true;
    }
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
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    uint32_t rootId = desktopRootToplevelId_;

    /*
     * subsurface 命中优先于 toplevel (渲染在上层):
     * - 内部菜单: 在父窗口范围内 → 走父 toplevel
     * - 外部菜单 (isExternal): 任务栏弹出等, 超出父窗口范围
     *   → 走 root, Wine explorer 内部处理点击分发
     *   (直接发给父 toplevel 会导致 Wine 收到错误的窗口相对坐标)
     */
    for (auto it = subsurfaceLayers_.rbegin(); it != subsurfaceLayers_.rend(); ++it) {
        if (it->parentToplevel != rootId && !IsToplevelVisible(it->parentToplevel)) continue;
        if (it->w <= 0 || it->h <= 0) continue;
        int layerX = 0, layerY = 0;
        ResolveSubsurfaceLayerPositionLocked(*it, layerX, layerY);
        if (x >= layerX && x < layerX + it->w && y >= layerY && y < layerY + it->h) {
            return it->isExternal ? rootId : it->parentToplevel;
        }
    }

    for (auto it = toplevelZOrder_.rbegin(); it != toplevelZOrder_.rend(); ++it) {
        uint32_t id = *it;
        if (!IsToplevelVisible(id)) continue;
        int tx = toplevelX_[id];
        int ty = toplevelY_[id];
        int tw = toplevelW_[id];
        int th = toplevelH_[id];
        if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
            wl_resource* surf = GetSurfaceForToplevel(id);
            if (surf) {
                auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surf));
                if (sd && sd->inputRegionEmpty) continue;
            }
            return id;
        }
    }
    return rootId;
}

bool WaylandServer::IsToplevelVisible(uint32_t id) {
    // 渲染和输入共用的可见性条件:
    // 非 root + 非背景层 + 有像素 + 非最小化
    if (id == desktopRootToplevelId_) return false;
    if (backgroundLayers_.count(id)) return false;
    if (toplevelPixels_.find(id) == toplevelPixels_.end()) return false;
    if (toplevelMinimized_.count(id)) return false;
    return true;
}

int32_t WaylandServer::GetWorkAreaHeight() {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    int32_t h = outputH_;
    // 找底部对齐的小高度 toplevel (任务栏), 工作区 = 任务栏上方空间
    for (auto& [id, y] : toplevelY_) {
        if (toplevelH_.count(id) && toplevelH_[id] > 0 && toplevelH_[id] < 100 &&
            y + toplevelH_[id] >= outputH_ && y < h) {
            h = y;
        }
    }
    return h;
}

void WaylandServer::SetToplevelMinimized(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    toplevelMinimized_[id] = true;
    if (toplevelX_.count(id)) {
        toplevelMinimizeCompX_[id] = toplevelX_[id];
        toplevelMinimizeCompY_[id] = toplevelY_[id];
    }
    toplevelMinimizeTimeMs_[id] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (desktopRootToplevelId_ > 0)
        toplevelDirty_[desktopRootToplevelId_] = true;
}

void WaylandServer::SetToplevelRestored(uint32_t id) {
    // 清除 minimized 状态
    {
        std::lock_guard<std::mutex> lk(toplevelMutex_);
        toplevelMinimized_.erase(id);
        toplevelMinimizeCompX_.erase(id);
        toplevelMinimizeCompY_.erase(id);
        toplevelMinimizeTimeMs_.erase(id);
        if (desktopRootToplevelId_ > 0)
        toplevelDirty_[desktopRootToplevelId_] = true;
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
    if (toplevelX_.count(id)) {
        toplevelX_[id] = 0;
        toplevelY_[id] = 0;
    }
    if (desktopRootToplevelId_ > 0)
        toplevelDirty_[desktopRootToplevelId_] = true;
}

void WaylandServer::SetToplevelUnmaximized(uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[MW] SetToplevelUnmaximized id=%{public}u desktop=%{public}s",
                id, IsDesktopMode() ? "yes" : "no");
    // 仅清除标记, 位置/尺寸由 surface_commit 恢复
    if (desktopRootToplevelId_ > 0)
        toplevelDirty_[desktopRootToplevelId_] = true;
}

void WaylandServer::ForceToplevelRedraw(uint32_t id) {
    std::lock_guard<std::mutex> lk(toplevelMutex_);
    toplevelDirty_[id] = true;
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
