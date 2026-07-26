#pragma once
#include <wayland-server-core.h>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "display_policy.h"

class ToplevelManager;

// -- 公共类型 (原 WaylandServer 嵌套类型, 外部调用方通过 wayland_server.h 的 using 别名继续使用) --

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
    bool fullscreen = false;  // 所属 toplevel 全屏: GL 层保比例缩放铺满视口 (ZC 游戏)
};

struct ZeroCopyOccluderRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// -- 帧合成 + 零拷贝 layer 管理 --
// 依赖 ToplevelManager (只读), 通过构造时注入的引用访问。
// 所有读写 subsurface/zero-copy 状态的方法由自身持有数据, 加锁约定同 WaylandServer。
//
// 不变式:
// - desktop root 帧是合成基底: TakeToplevelFrame(rootId) 输出整屏合成
//   结果, 其它 toplevel 帧只在其上叠加, 永不替代。
// - zero-copy GL 层与 CPU 合成按帧互斥 (非并存): 走 GL overlay 的帧,
//   被上层窗口遮挡的区域用桌面纹理重绘恢复层序 (egl_renderer occluder
//   redraw); GPU→CPU fallback 时 key 移出 zeroCopySurfaceKeys_, 该层
//   自动回归普通 CPU 合成与置顶命中, 无需特判。
// - desktop root 不参与可见性判定 (契约在 ToplevelManager::IsToplevelVisibleLocked)。

class DesktopCompositor {
public:
    // subsurface 合成层 (独立于 per-toplevel 帧缓冲, 避免污染)
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

    // 构造: 注入 ToplevelManager + 桌面合成配置 (由 WaylandServer 持有,
    // policy 为引用 — SetDesktopMode 后随动)
    DesktopCompositor(ToplevelManager& tmgr,
                      const DisplayPolicy& policy,
                      const uint32_t& desktopRootToplevelId,
                      const int32_t& outputW,
                      const int32_t& outputH);

    // -- 帧输出 --
    // 取指定 toplevel 的最新帧 (桌面模式合成到 root framebuffer)
    bool TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, int& w, int& h);

    // -- Zero-copy layer 管理 --
    bool GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                              ZeroCopyLayerInfo& info);
    void SetSurfaceZeroCopy(uint64_t surfaceKey, bool enabled);
    int GetZeroCopyOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                             ZeroCopyOccluderRect* out, int maxOut);

    // -- Subsurface layer 位置解析 (InputResolver 调用) --
    void ResolveSubsurfaceLayerPositionLocked(const SubsurfaceLayer& layer,
                                              int& x, int& y) const;

    // -- 桌面 root dirty 标记 --
    void MarkDesktopRootDirtyLocked();

    // -- Subsurface layer 生命周期 (替代直接操作 subsurfaceLayers_) --

    // 更新 subsurface layer 的本地偏移 (subsurface_set_position 调用)
    void UpdateSubsurfaceLayerLocalPosition(wl_resource* surface, int32_t x, int32_t y);

    // 移除指定 surface 对应的 layer。返回是否实际移除 (调用方据此决定是否 mark dirty)。
    bool RemoveSubsurfaceLayer(wl_resource* surface);

    // 插入或替换 layer (按 surface 匹配)。`layer` 应已填充除 pixels 外的所有字段。
    // `newPixels` 是 sd->pixels 中刚提交的帧数据, 被移入 layer。
    // 返回旧 layer 的 pixels (新插入时为空), 供调用方归还给 sd->pixels 做双缓冲轮转。
    std::vector<uint8_t> UpsertSubsurfaceLayer(SubsurfaceLayer&& layer,
                                               std::vector<uint8_t>&& newPixels);

    // 在 sibling 之上/下移动 child layer。child 和 sibling 必须已存在。
    void ReorderSubsurfaceLayerAbove(wl_resource* child, wl_resource* sibling);
    void ReorderSubsurfaceLayerBelow(wl_resource* child, wl_resource* sibling);

    // 移除 zero-copy key (调用方须已持有 mutex)
    void RemoveZeroCopyKeyLocked(uint64_t surfaceKey);

    // Increment root frame serial (called from surface_commit when root commits)
    void IncrementDesktopRootFrameSerial() { ++desktopRootFrameSerial_; }

    // -- 只读 accessors --
    const std::vector<SubsurfaceLayer>& subsurfaceLayers() const { return subsurfaceLayers_; }
    const std::unordered_set<uint64_t>& zeroCopySurfaceKeys() const { return zeroCopySurfaceKeys_; }

    // toplevel 是否有 zero-copy GL 层 (ZC 游戏判定: 全屏渲染/输入映射分流用,
    // 调用方须已持有 tmgr mutex)
    bool HasZeroCopyLayerForToplevelLocked(uint32_t id) const;

private:
    ToplevelManager& tmgr_;
    const DisplayPolicy& policy_;
    const uint32_t& desktopRootToplevelId_;
    const int32_t& outputW_;
    const int32_t& outputH_;

    std::vector<SubsurfaceLayer> subsurfaceLayers_;
    std::unordered_set<uint64_t> zeroCopySurfaceKeys_;
    uint64_t desktopCompositionSignature_ = 0;
    uint64_t desktopOutputRootFrameSerial_ = 0;
    bool desktopOutputInitialized_ = false;
    uint64_t desktopRootFrameSerial_ = 0;
};
