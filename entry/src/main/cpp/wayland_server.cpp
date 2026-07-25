#include "wayland_server.h"
#include "seat.h"
#include "input_manager.h"
#include "xdg_shell.h"
#include "fps_counter.h"
#include "compositor/debug_assert.h"
#include "include/xdg-shell-server-protocol.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cerrno>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>


extern "C" void RegisterXdgShell(wl_display* display);
extern "C" void RegisterWlCoreGlobals(wl_display* display);
#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"
#include <hilog/log.h>
#include "plugin_manager.h"

// 核心协议接口表与实现已剥离到 wl_core.cpp (Phase 3 纯搬移)

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

    // 注册 global 对象 (核心协议实现已剥离到 wl_core.cpp)
    RegisterWlCoreGlobals(display_);
    wl_display_init_shm(display_);
    RegisterXdgShell(display_);
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
                        toplevelMgr_.ToplevelResourceCount(), toplevelMgr_.ToplevelSurfaceCount(), renderers);
        }
    }
}



// -- 帧数据接口 --
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

void WaylandServer::RaiseToplevel(uint32_t id) {
    auto lk = toplevelMgr_.Lock();
    toplevelMgr_.RemoveFromZOrder(id);
    toplevelMgr_.AddToZOrder(id);
    // 任务栏始终在顶层 (app_id == "explorer.exe.taskbar");
    // 全屏窗口例外 — 游戏全屏必须压过任务栏
    bool raisedFullscreen = false;
    if (const auto* rst = toplevelMgr_.FindToplevelLocked(id)) raisedFullscreen = rst->fullscreen;
    if (taskbarId_ > 0 && taskbarId_ != id && !raisedFullscreen) {
        toplevelMgr_.RemoveFromZOrder(taskbarId_);
        toplevelMgr_.AddToZOrder(taskbarId_);
    }
    MarkDesktopRootDirtyLocked();
}

// -- 交互式窗口移动 (xdg_toplevel.move) --
void WaylandServer::StartMoveGrab(uint32_t toplevelId, uint32_t serial) {
    moveGrab_.StartMoveGrab(toplevelMgr_, toplevelId, serial);
    if (Policy().OhosWindowPerToplevel()) {
        FireToplevelEvent(toplevelId, "move_start");
    }
}

void WaylandServer::EndMoveGrab() {
    uint32_t tl = moveGrab_.GetToplevelId();
    moveGrab_.EndMoveGrab(toplevelMgr_);
    if (Policy().OhosWindowPerToplevel() && tl != 0) {
        FireToplevelEvent(tl, "move_end");
    }
}

bool WaylandServer::ProcessMoveGrabMotion(wl_fixed_t wx, wl_fixed_t wy) {
    if (!moveGrab_.ProcessMoveGrabMotion(toplevelMgr_, wx, wy)) return false;
    MarkDesktopRootDirtyLocked();
    return true;
}

void WaylandServer::FireToplevelEvent(uint32_t id, const char* event, const char* jsonData) {
    OH_LOG_INFO(LOG_APP, "[MW] FireToplevel id=%{public}u event=%{public}s data=%{public}s", id, event, jsonData);
    if (toplevelCb_) toplevelCb_(id, event, jsonData);
}

void WaylandServer::RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl) {
    toplevelMgr_.RegisterToplevelResource(toplevelId, tl);
    OH_LOG_INFO(LOG_APP, "[MW] RegisterToplevelResource id=%{public}u tl=%{public}p", toplevelId, tl);
}

void WaylandServer::UnregisterToplevelResource(uint32_t toplevelId) {
    auto* tl = toplevelMgr_.FindToplevelResource(toplevelId);
    if (tl) {
        OH_LOG_INFO(LOG_APP, "[MW] UnregisterToplevelResource id=%{public}u tl=%{public}p (Wine destroyed toplevel)",
                    toplevelId, tl);
    }
    toplevelMgr_.UnregisterToplevelResource(toplevelId);
}

void WaylandServer::OnToplevelDestroyed(uint32_t toplevelId) {
    std::vector<uint32_t> cascadePopups;
    {
        auto lk = toplevelMgr_.Lock();
        toplevelMgr_.EraseToplevelLocked(toplevelId);
        if (pendingDesktopRootToplevelId_ == toplevelId)
            pendingDesktopRootToplevelId_ = 0;
        if (taskbarId_ == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW] taskbar toplevel #%{public}u destroyed, clearing cached id",
                        toplevelId);
            taskbarId_ = 0;
        }
        // root 本体被销毁 (xs_destroy / 客户端断连路径同样走到这里): 复位, 等待下一个 explorer
        if (desktopRootToplevelId_ == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW] desktop root toplevel #%{public}u destroyed, clearing root",
                        toplevelId);
            desktopRootToplevelId_ = 0;
        }
        // 被抓取窗口销毁 → 复位 move grab, 防止悬空 grab 吞掉后续 motion
        if (moveGrab_.GetToplevelId() == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW-MOVE] grabbed toplevel #%{public}u destroyed, reset grab",
                        toplevelId);
            moveGrab_.EndMoveGrab(toplevelMgr_);
        }
        toplevelMgr_.RemoveFromZOrder(toplevelId);
        // 级联清理该 toplevel 的全部 PC popup (帧数据 + 映射)
        for (const auto& [pid, rec] : toplevelMgr_.popups()) {
            if (rec.parentToplevel == toplevelId) cascadePopups.push_back(pid);
        }
        for (uint32_t pid : cascadePopups) toplevelMgr_.RemovePopupDataLocked(pid);
        MarkDesktopRootDirtyLocked();  // 非 desktop / root 已复位时 root=0, 自然 no-op
        // 对称清理 surface 映射 (popup 路径在 RemovePopupDataLocked 已清, toplevel 路径此前缺失):
        // xs_destroy 时 wl_surface 可能仍存活, 不清会让 GetSurfaceForToplevel(死 id) 命中
        // 已无 toplevel 身份的 surface。嵌套锁序同 RemovePopupDataLocked。
        toplevelMgr_.UnmapToplevelSurface(toplevelId);
    }
    // 通知 ArkTS 销毁 popup 子窗口 (锁外触发)
    for (uint32_t pid : cascadePopups) {
        char json[64];
        snprintf(json, sizeof(json), "{\"popupId\":%u}", pid);
        FireToplevelEvent(toplevelId, "popup_hide", json);
    }
}

// RemovePopupDataLocked 已移至 ToplevelManager (compositor/toplevel_manager.cpp)

// RemovePopupBySurfaceKeyLocked 已移至 ToplevelManager (compositor/toplevel_manager.cpp)

bool WaylandServer::TakeWindowMask(uint32_t id, int& w, int& h, std::vector<uint8_t>& out) {
    auto lk = toplevelMgr_.Lock();
    auto* st = toplevelMgr_.FindToplevelLocked(id);
    if (!st || !st->mask.dirty) return false;
    w = st->mask.w;
    h = st->mask.h;
    out = st->mask.bits;
    st->mask.dirty = false;
    return true;
}

void WaylandServer::SendToplevelClose(uint32_t toplevelId) {
    wl_resource* tl = toplevelMgr_.FindToplevelResource(toplevelId);
    if (tl) {
        toplevelMgr_.UnregisterToplevelResource(toplevelId);
        OH_LOG_INFO(LOG_APP, "[MW] SendToplevelClose id=%{public}u -> xdg_toplevel_send_close", toplevelId);
        xdg_toplevel_send_close(tl);
    } else {
        OH_LOG_WARN(LOG_APP, "[MW] SendToplevelClose id=%{public}u NOT found", toplevelId);
    }
}

// IsToplevelVisibleLocked 已移至 ToplevelManager (compositor/toplevel_manager.cpp)

int32_t WaylandServer::GetWorkAreaHeight() {
    auto lk = toplevelMgr_.Lock();
    if (taskbarId_ == 0) return outputH_;
    const auto* st = toplevelMgr_.FindToplevelLocked(taskbarId_);
    if (!st) return outputH_;
    return st->y;  // 工作区 = 任务栏上方空间
}

void WaylandServer::SetToplevelMinimized(uint32_t id) {
    auto lk = toplevelMgr_.Lock();
    // 保留 operator[] 建档语义: pre-commit 最小化同样记录状态
    toplevelMgr_.EnsureToplevelLocked(id).minimized = true;
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::SetToplevelRestored(uint32_t id) {
    // 清除 minimized 状态
    {
        auto lk = toplevelMgr_.Lock();
        if (auto* st = toplevelMgr_.FindToplevelLocked(id)) st->minimized = false;
        MarkDesktopRootDirtyLocked();
    }
    // 发 configure 通知 Wine (如果 toplevel resource 存在)
    wl_resource* tl = toplevelMgr_.FindToplevelResource(id);
    if (!tl) return;
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    wl_array states;
    wl_array_init(&states);
    uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    // 全屏窗口从最小化还原: 维持 FULLSCREEN 状态 (尺寸 0,0 = Wine 保持当前尺寸)
    if (IsToplevelFullscreen(id)) {
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
    auto lk = toplevelMgr_.Lock();
    if (auto* st = toplevelMgr_.FindToplevelLocked(id); st && st->hasPosition) {
        st->x = 0;
        st->y = 0;
    }
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::SetToplevelFullscreen(uint32_t id, bool on) {
    OH_LOG_INFO(LOG_APP, "[MW] SetToplevelFullscreen id=%{public}u on=%{public}s",
                id, on ? "yes" : "no");
    auto lk = toplevelMgr_.Lock();
    // Ensure 建档语义同 SetToplevelMinimized: pre-commit 全屏同样记录状态
    auto& st = toplevelMgr_.EnsureToplevelLocked(id);
    st.fullscreen = on;
    // 全屏窗口锚定桌面原点: 合成按保比例缩放铺满, 不再使用浮动位置
    if (on && st.hasPosition) {
        st.x = 0;
        st.y = 0;
    }
    // 不变式守卫 (ToplevelManager 头注释): 全屏 toplevel 锚定 (0,0)
    MW_ASSERT(!on || !st.hasPosition || (st.x == 0 && st.y == 0),
              "fullscreen toplevel must be anchored at (0,0)");
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::ForceToplevelRedraw(uint32_t id) {
    auto lk = toplevelMgr_.Lock();
    if (auto* st = toplevelMgr_.FindToplevelLocked(id)) st->dirty = true;
}

void WaylandServer::NotifyToplevelResize(uint32_t toplevelId, int32_t w, int32_t h) {
    wl_resource* tl = toplevelMgr_.FindToplevelResource(toplevelId);
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
    if (IsToplevelFullscreen(toplevelId)) {
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
    if (Policy().RootCompositing() && toplevelId == desktopRootToplevelId_) {
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
    return toplevelMgr_.GetSurfaceForToplevel(toplevelId);
}
