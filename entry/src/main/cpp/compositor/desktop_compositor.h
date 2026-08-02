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
    bool protocolOnly = false;
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
// - 层序单一数据源 (阶段 1, 行为等价): 一帧桌面的内容来源统一为
//   CompositorLayer 列表 (BuildLayerListLocked), 合成与输入遍历同一个
//   按 zIndex 升序的列表; 各层的合成/命中特判逻辑原样保留 (等价形式),
//   阶段 2 起 ZC 层入列参与层序。

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

    // -- 层序单一数据源 (阶段 1: 行为等价重构) --
    // 一帧桌面的所有内容来源统一为 Layer; 合成与输入遍历同一按 zIndex 升序
    // 的 Layer 列表 (BuildLayerListLocked)。zIndex 分配: root=0 < toplevel
    // (按 toplevelZOrder_ 顺序) < subsurface (原顺序) — 与旧双循环顺序等价。
    // 阶段 1 仅收敛遍历源, 各层合成/命中的特判逻辑保留等价形式 (不动行为);
    // ZC 层阶段 1 仍由合成/输入跳过, 阶段 2 起入列参与层序。
    // 阶段 3: zcActive 为 ZC 层状态单一字段 (合成/输入/遮挡重绘只认它)。
    // sub/st 指针指向调用方持有的容器, 必须在 ToplevelManager 锁内使用。
    struct CompositorLayer {
        enum class Type { Root, Toplevel, Subsurface };
        Type type = Type::Root;
        size_t zIndex = 0;
        bool visible = false;    // 可见性判定结果 (Root 恒 true, 不参与命中)
        // ZC 层状态单一字段: 该层走 GPU 内容 (合成/输入跳过, 内容由
        // egl_renderer GPU 层自绘); false = fallback 到 CPU 内容 (合成/
        // 命中照常)。由 zeroCopySurfaceKeys_ 派生 — 该集合是 compositor
        // 侧唯一权威, broker 的 attached 簿记 / ready marker (guest 选路)
        // 只是它的执行投影, 不参与合成判定。
        bool zcActive = false;
        uint32_t toplevelId = 0; // 归属窗口 (Root 为 0; Subsurface 为 parentToplevel)
        int x = 0, y = 0, w = 0, h = 0;  // 坐标 (桌面合成: 桌面坐标; 窗口内: 窗口局部坐标)
        bool fullscreen = false; // Toplevel: 全屏标记
        const SubsurfaceLayer* sub = nullptr;  // Type==Subsurface 时引用原层
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

    // -- 层序单一数据源 --
    // 构建按 zIndex 升序的 Layer 列表 (调用方须已持有 tmgr mutex)。
    // 合成 (TakeToplevelFrame) 与输入 (InputResolver) 遍历同一列表;
    // rootW/rootH 用于 Root 层几何 (输入侧仅作占位, 不参与命中)。
    std::vector<CompositorLayer> BuildLayerListLocked(int rootW, int rootH);

    // 窗口内 Layer 列表 (阶段 3, PC 模式): 单窗口合成数据源, 与
    // BuildLayerListLocked 对称但用窗口局部坐标:
    //   zIndex: Root(窗口帧) < Subsurface(窗口内局部坐标) < ZC 层(最顶)
    // 窗口间层序不在此管理 (系统合成器)。PC 模式 subsurface 全部转 popup
    // 伪 toplevel (UpdatePopupOnCommit), 窗口内 subsurface 当前恒空 —
    // 层序结构为窗口内内容扩展预留; ZC 层 (zcActive) 在层序最顶, 合成跳过
    // (GPU 自绘覆盖, 与 desktop 模式同语义)。调用方须已持有 tmgr mutex。
    std::vector<CompositorLayer> BuildWindowLayerListLocked(uint32_t toplevelId,
                                                            int winW, int winH);

    // -- Zero-copy layer 管理 --
    bool GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                              int fallbackWidth, int fallbackHeight,
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
    // 返回是否实际改变了合成顺序，调用方据此避免无效的 root 重绘。
    bool ReorderSubsurfaceLayerAbove(wl_resource* child, wl_resource* sibling);
    bool ReorderSubsurfaceLayerBelow(wl_resource* child, wl_resource* sibling);

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
    std::unordered_set<uint64_t> zeroCopyProtocolGeometryLogged_;
    uint64_t desktopCompositionSignature_ = 0;
    uint64_t desktopOutputRootFrameSerial_ = 0;
    bool desktopOutputInitialized_ = false;
    uint64_t desktopRootFrameSerial_ = 0;
};
