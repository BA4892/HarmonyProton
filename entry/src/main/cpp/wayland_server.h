#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// 最小 Wayland Compositor: wl_compositor + wl_surface + wl_shm
class WaylandServer {
public:
    struct ZeroCopyLayerInfo {
        uint64_t surfaceKey = 0;
        uint32_t clientPid = 0;
        uint32_t surfaceId = 0;
        uint32_t parentToplevel = 0;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        uint64_t shmCommitSerial = 0;
        bool desktopCoordinates = false;
        bool protocolOnly = false;
    };

    using StateCb = std::function<void(const char*)>;
    // toplevel 回调: (toplevelId, eventName, jsonData)
    // events: "created", "destroyed", "title", "configure"
    using ToplevelCb = std::function<void(uint32_t, const char*, const char*)>;

    static WaylandServer* GetInstance();

    wl_display* GetDisplay() const { return display_; }

    bool Start(const std::string& socketPath);
    void Stop();

    // EglRenderer 调用: 取最新一帧像素 (deprecated, 用 TakeToplevelFrame)
    bool TakeFrame(std::vector<uint8_t>& outPixels, int& w, int& h);
    // 取指定 toplevel 的最新帧
    bool TakeToplevelFrame(uint32_t toplevelId, std::vector<uint8_t>& outPixels, int& w, int& h);
    bool GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                              int fallbackWidth, int fallbackHeight,
                              ZeroCopyLayerInfo& info);
    void SetSurfaceZeroCopy(uint64_t surfaceKey, bool enabled);

    // 状态回调 (首帧到达 -> 通知 ArkTS)
    void SetStateCallback(StateCb cb) { stateCb_ = std::move(cb); }
    void FireState(const char* s) { if (stateCb_) stateCb_(s); }
    void ResetFirstFrame() { firstFrame_ = false; }

    // toplevel 回调 (xdg_toplevel 生命周期 -> 通知 ArkTS 创建/销毁窗口)
    void SetToplevelCallback(ToplevelCb cb) { toplevelCb_ = std::move(cb); }
    void FireToplevelEvent(uint32_t id, const char* event, const char* jsonData = "{}");

    // 生成唯一 toplevel ID
    uint32_t NextToplevelId() { return nextToplevelId_++; }

    // toplevel resource 映射 (用于 SendToplevelClose -> xdg_toplevel_send_close)
    void RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl);
    void UnregisterToplevelResource(uint32_t toplevelId);
    // 清理 toplevel 像素数据 + 标记 root dirty (desktop mode)
    void OnToplevelDestroyed(uint32_t toplevelId);
    void SendToplevelClose(uint32_t toplevelId);
    // 统一状态转换 (确保 minimize/maximize/restore 涉及的 map 操作原子化)
    void SetToplevelMinimized(uint32_t id);
    void SetToplevelRestored(uint32_t id);
    void SetToplevelMaximized(uint32_t id);
    void SetToplevelUnmaximized(uint32_t id);
    // surface 尺寸变化后强制下次渲染循环取帧重绘 (避免旧 viewport 贴新 surface 导致黑边)
    void ForceToplevelRedraw(uint32_t id);
    // 旧接口 → 转发到新方法
    void NotifyWindowRestored(uint32_t id) { SetToplevelRestored(id); }
    void NotifyToplevelMaximized(uint32_t id) { SetToplevelMaximized(id); }
    void NotifyToplevelMinimized(uint32_t id, int32_t, int32_t) { SetToplevelMinimized(id); }
    // 鸿蒙侧 surface 尺寸变化时调用: 发 configure 通知 Wine 用新尺寸渲染
    void NotifyToplevelResize(uint32_t toplevelId, int32_t w, int32_t h);
    // 设置输出尺寸 (替换硬编码 1280x720)
    void SetOutputSize(int32_t w, int32_t h) { outputW_ = w; outputH_ = h; }
    int32_t outputW_ = 1280;
    int32_t outputH_ = 720;
    int32_t GetWorkAreaHeight();  // 排除任务栏后的可用高度
    // 渲染/输入共用的 toplevel 可见性检查
    bool IsToplevelVisible(uint32_t id);
    // Desktop 模式: 在合成帧中查找包含 (x,y) 的 toplevel (用于输入路由)
    uint32_t FindToplevelAt(int x, int y);
    // Desktop 模式: 提到 Z-order 最顶层
    void RaiseToplevel(uint32_t id);
    // 读取 toplevel 桌面坐标 (InputManager 坐标转换用)
    int GetToplevelX(uint32_t id) { std::lock_guard<std::mutex> lk(toplevelMutex_); return toplevelX_[id]; }
    int GetToplevelY(uint32_t id) { std::lock_guard<std::mutex> lk(toplevelMutex_); return toplevelY_[id]; }
    int GetToplevelW(uint32_t id) { std::lock_guard<std::mutex> lk(toplevelMutex_); return toplevelW_[id]; }
    int GetToplevelH(uint32_t id) { std::lock_guard<std::mutex> lk(toplevelMutex_); return toplevelH_[id]; }
    // Desktop 合成模式 (Tablet): 全部 toplevel 合成到一个 root framebuffer
    void SetDesktopMode(bool on) { desktopMode_ = on; }
    bool IsDesktopMode() const { return desktopMode_; }
    void SetDesktopRootToplevelId(uint32_t id) { desktopRootToplevelId_ = id; }
    uint32_t GetDesktopRootToplevelId() const { return desktopRootToplevelId_; }
    void SetDesktopRootRecognitionEnabled(bool enabled);
    void PromotePendingDesktopRoot();

    // -- wayland 协议实现 --
    static void compositor_bind(wl_client*, void*, uint32_t, uint32_t);
    static void compositor_create_surface(wl_client*, wl_resource*, uint32_t);
    static void compositor_create_region(wl_client*, wl_resource*, uint32_t);

    static void surface_destroy(wl_client*, wl_resource*);
    static void surface_attach(wl_client*, wl_resource*, wl_resource*, int32_t, int32_t);
    static void surface_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t);
    static void surface_frame(wl_client*, wl_resource*, uint32_t);
    static void surface_commit(wl_client*, wl_resource*);
    static void surface_set_opaque_region(wl_client*, wl_resource*, wl_resource*) {}
    static void surface_set_input_region(wl_client*, wl_resource*, wl_resource*);
    static void surface_set_buffer_transform(wl_client*, wl_resource*, int32_t) {}
    static void surface_set_buffer_scale(wl_client*, wl_resource*, int32_t) {}
    static void surface_damage_buffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
    static void surface_offset(wl_client*, wl_resource*, int32_t, int32_t) {}

    static void region_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void region_add(wl_client*, wl_resource* r, int32_t, int32_t, int32_t, int32_t) {
        int* count = static_cast<int*>(wl_resource_get_user_data(r));
        if (count) (*count)++;
    }
    static void region_subtract(wl_client*, wl_resource* r, int32_t, int32_t, int32_t, int32_t) {
        // 追踪计数: Wine 只用空/非空判断
        int* count = static_cast<int*>(wl_resource_get_user_data(r));
        if (count) (*count)++;
    }

    /* wl_subcompositor */
    static void subcompositor_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void subcompositor_get_subsurface(wl_client*, wl_resource*, uint32_t, wl_resource*, wl_resource*);
    /* wl_subsurface */
    static void subsurface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void subsurface_set_position(wl_client*, wl_resource*, int32_t, int32_t);
    static void subsurface_place_above(wl_client*, wl_resource*, wl_resource*);
    static void subsurface_place_below(wl_client*, wl_resource*, wl_resource*);
    static void subsurface_set_sync(wl_client*, wl_resource*) {}
    static void subsurface_set_desync(wl_client*, wl_resource*) {}

    /* wp_viewporter */
    static void viewporter_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void viewporter_get_viewport(wl_client*, wl_resource*, uint32_t, wl_resource*);
    /* wp_viewport */
    static void viewport_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void viewport_set_source(wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {}
    static void viewport_set_destination(wl_client*, wl_resource*, int32_t, int32_t);

    /* Globals bind */
    static void subcompositor_bind(wl_client*, void*, uint32_t, uint32_t);
    static void viewporter_bind(wl_client*, void*, uint32_t, uint32_t);
    static void output_bind(wl_client*, void*, uint32_t, uint32_t);
    /* wl_output */
    static void output_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

    // toplevelId -> wl_surface 映射 (供 Seat::InjectPointerEnter 查找)
    wl_resource* GetSurfaceForToplevel(uint32_t toplevelId);

    // 交互式窗口移动 (xdg_toplevel.move) — 由 xdg_shell 和 InputManager 调用
    bool IsMoveGrabActive() const { return moveGrabToplevelId_ != 0; }
    void StartMoveGrab(uint32_t toplevelId, uint32_t serial);
    void EndMoveGrab();
    bool ProcessMoveGrabMotion(wl_fixed_t wx, wl_fixed_t wy);

private:
    WaylandServer() = default;
    void EventLoop();

    wl_display* display_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // 全局帧缓冲 (deprecated, 保留兼容)
    std::mutex mutex_;
    std::vector<uint8_t> pixels_;
    int width_ = 0, height_ = 0;
    std::atomic<bool> dirty_{false};

    // toplevel 帧缓冲: toplevelId -> pixels
    std::mutex toplevelMutex_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> toplevelPixels_;
    std::unordered_map<uint64_t, wl_resource*> surfaceResources_;
    std::unordered_set<uint64_t> zeroCopySurfaceKeys_;
    std::unordered_set<uint64_t> zeroCopyProtocolGeometryLogged_;
    std::unordered_map<uint32_t, int> toplevelW_, toplevelH_;
    std::unordered_map<uint32_t, int> toplevelX_, toplevelY_;  // compositor 桌面位置 (含 move grab 偏移)
    std::unordered_map<uint32_t, int> toplevelWineX_, toplevelWineY_;  // Wine 坐标系位置 (首次 commit, 不变)
    std::unordered_map<uint32_t, bool> toplevelDirty_;
    std::unordered_map<uint32_t, int> toplevelLastReportedW_, toplevelLastReportedH_;
    std::unordered_map<uint32_t, bool> toplevelMinimized_;  // 桌面合成时跳过最小化窗口
    std::unordered_map<uint32_t, int> toplevelMinimizeCompX_, toplevelMinimizeCompY_;  // 最小化时的 compositor 位置
    std::unordered_map<uint32_t, uint64_t> toplevelMinimizeTimeMs_;  // 最小化时间戳

    StateCb stateCb_;
    ToplevelCb toplevelCb_;
    std::atomic<bool> firstFrame_{false};
    std::atomic<uint32_t> nextToplevelId_{1};
    std::unordered_map<uint32_t, wl_resource*> toplevelResources_;
    std::mutex toplevelResMutex_;

    // toplevelId -> wl_surface 映射 (input focus 查找)
    std::unordered_map<uint32_t, wl_resource*> toplevelSurfaceMap_;
    std::mutex toplevelSurfaceMutex_;

    // Desktop 合成模式
    bool desktopMode_ = false;
    uint32_t desktopRootToplevelId_ = 0;
    uint32_t pendingDesktopRootToplevelId_ = 0;
    bool desktopRootRecognitionEnabled_ = true;
    // 交互式窗口移动 (xdg_toplevel.move)
    uint32_t moveGrabToplevelId_ = 0;
    uint32_t moveGrabSerial_ = 0;
    int32_t moveGrabLastWineX_ = 0, moveGrabLastWineY_ = 0;  // 上一帧 Wine 逻辑坐标
    // subsurface 合成层 (不写入 toplevelPixels_, 避免污染)
    struct SubsurfaceLayer {
        wl_resource* surface = nullptr;
        uint64_t surfaceKey = 0;
        std::vector<uint8_t> pixels;
        int x = 0, y = 0, w = 0, h = 0;
        int localX = 0, localY = 0;
        uint64_t shmCommitSerial = 0;
        uint32_t parentToplevel = 0;
        uint32_t shmFormat = 1;
        bool opaque = false;
        int32_t dmgX = 0, dmgY = 0, dmgW = 0, dmgH = 0;  // damage 包围盒
        int32_t vpDstW = -1, vpDstH = -1;                // viewport destination
        bool isExternal = false;  // 外部菜单 (任务栏等), 输入坐标需用 Wine 基底
    };
    void ResolveSubsurfaceLayerPositionLocked(const SubsurfaceLayer& layer,
                                              int& x, int& y) const;
    std::vector<SubsurfaceLayer> subsurfaceLayers_;
    std::vector<uint32_t> toplevelZOrder_;  // 前景→背景
    std::unordered_set<uint32_t> backgroundLayers_; // 渲染层, 不接收输入 (被切换掉的旧 root)
    uint64_t desktopRootFrameSerial_ = 0;
    uint64_t desktopOutputRootFrameSerial_ = 0;
    uint64_t desktopCompositionSignature_ = 0;
    bool desktopOutputInitialized_ = false;
};

// wl_surface 的每个实例携带的数据
struct SurfaceData {
    wl_resource* surface = nullptr;
    uint64_t surfaceKey = 0;
    uint32_t clientPid = 0;
    uint32_t protocolId = 0;
    wl_resource* pendingBuffer = nullptr;
    std::vector<wl_resource*> frameCallbacks;

    // per-surface pixel buffer
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    bool dirty = false;
    std::atomic<uint64_t> shmCommitSerial{0};

    // toplevel identity
    uint32_t toplevelId = 0;
    bool hasToplevel = false;
    std::string title;
    int x = 0, y = 0, winW = 640, winH = 480;

    // xdg_surface window geometry (content area within buffer), 默认全 buffer
    bool hasWindowGeometry = false;
    int geoX = 0, geoY = 0, geoW = 0, geoH = 0;

    // subsurface 父子追踪 (用于 popup 菜单合成到父 toplevel)
    wl_resource* parentSurface = nullptr;     // 父 wl_surface (仅 subsurface)
    int32_t subsurfaceX = 0, subsurfaceY = 0; // wl_subsurface.set_position
    bool isSubsurface = false;

    // surface_damage 累积包围盒 (buffer 坐标), 用于裁剪 subsurface 渲染
    int32_t damageX = 0, damageY = 0, damageW = 0, damageH = 0;

    // wp_viewport destination (实际显示尺寸, -1=未设置/使用 buffer 尺寸)
    int32_t vpDstW = -1, vpDstH = -1;

    // window states
    // app_id (xdg_toplevel.set_app_id), 用于识别 explorer 桌面
    std::string appId;
    bool minimized = false;
    bool maximized = false;
    int32_t preMaxW = 0, preMaxH = 0;  // 最大化前尺寸, restore 用

    // xdg_toplevel resize 约束 (0 = 无限制)
    bool hasSizeLimits = false;
    int32_t minWidth = 0, minHeight = 0;
    int32_t maxWidth = 0, maxHeight = 0;

    // wl_surface.set_input_region: true = 空区域 (不接受输入, 穿透点击)
    bool inputRegionEmpty = false;
};
