#pragma once
#include <wayland-server-core.h>
#include <string>
#include <mutex>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// WaylandServer 中 toplevel/popup 聚合状态的集中存储。
// 所有字段由 mutex() 保护，调用方在访问任何成员前必须先加锁。
//
// 不变式 (违反即 bug):
// - toplevelZOrder_ 是 z-order 唯一存放处 (置顶/Raise 都经它)。
//   desktop root 可能因识别时序已在列 (先 AddToZOrder 后 CheckRoot),
//   由 IsToplevelVisibleLocked 对 root 恒 false 兜底 — root 永不作为
//   可见 toplevel 参与合成/命中 (桌面"仅剩背景"回归的根因, 见 cpp 注释)。
// - minimized/fullscreen 的唯一权威字段在 ToplevelState; 变更只经
//   WaylandServer::SetToplevel* (本类故意不提供 setter)。
// - fullscreen toplevel 锚定 (0,0): SetToplevelFullscreen 维护,
//   合成按保比例缩放铺满, 不使用浮动位置。

class ToplevelManager {
public:
    // -- 公共类型 --

    struct WindowMask {
        int w = 0, h = 0;
        uint64_t hash = 0;
        std::vector<uint8_t> bits;  // w*h, 每像素 0/1
        bool dirty = false;
    };

    struct ToplevelState {
        // -- 帧数据 (toplevel 与 PC popup 共用) --
        std::vector<uint8_t> pixels;   // empty() = 尚无帧
        int w = 0, h = 0;              // content 尺寸 (popup 为显示尺寸)
        bool dirty = false;
        uint32_t shmFormat = 1;        // wl_shm format (0=ARGB8888, 1=XRGB8888)
        // -- 桌面坐标 (仅 toplevel) --
        bool hasPosition = false;      // 首次 commit 置位 (isFirstCommit 判定 / 移动守卫)
        int x = 0, y = 0;              // compositor 桌面位置 (含 move grab 偏移)
        int wineX = 0, wineY = 0;      // Wine 坐标系位置 (首次 commit, 不变)
        // -- 尺寸上报去重 --
        int lastReportedW = 0, lastReportedH = 0;
        // -- 状态标记 --
        bool minimized = false;        // 桌面合成时跳过最小化窗口
        bool isBackground = false;     // 渲染层, 不接收输入 (被切换掉的旧 root)
        bool fullscreen = false;
        // -- 全屏辅助 --
        // ZC 游戏 (画面在 zero-copy GL 层) 全屏后 buffer 被 Wine 扩到输出尺寸,
        // 但游戏内部分辨率不变, 输入逆映射须用全屏前尺寸; SHM 游戏 buffer 即
        // 画面, 直接用 w/h。选择逻辑是 geometry.h 的纯函数, 两侧共用。
        int32_t preFsW = 0, preFsH = 0;  // 全屏前内容尺寸, SetToplevelFullscreen 快照
        // -- 全屏优先级序号 --
        // 多窗口可同时处于 fullscreen: 游戏 ChangeDisplaySettings 缩虚拟屏后,
        // Wine 按"窗口矩形覆盖整个屏幕即全屏"把所有足够大的旧窗口 (notepad、
        // explorer 伴随窗口等) 连带标成 fullscreen, 且 set_fullscreen 请求
        // 到达顺序不定 — 靠 z-order/到达顺序选"全屏前台"都会被旧窗口压顶,
        // 第一下点击路由错 → Wine 前台切换 → 游戏掉出全屏 (2026-07 实测,
        // 曾用 app_id 前缀 "explorer.exe" 特判, 但 notepad 等非 explorer
        // 窗口同样触发, 特判只是 mask 最常见实例, 故改一般规则)。
        // 取号规则 (渲染/输入两侧全屏扫描统一取序号最大者):
        // - 首次入 z-order 时按先后取号 (AddToZOrder 内部完成): 游戏窗口
        //   必定最晚入列 (map 或 set_fullscreen 的 raise, 以先到者为准),
        //   天然最大, 连带标记的旧窗口都比它旧;
        // - 用户显式 raise 一个已 fullscreen 的窗口时重新取号
        //   (RaiseToplevel 用户路径): 两个全屏窗口经任务栏互相切换靠它。
        //   tl_set_fullscreen 批处理里的 raise 不重新取号 (否则退回到达
        //   顺序); 窗口化窗口不重新取号 (点过 notepad 不该让它日后盖过游戏)。
        // 已知局限:
        // - 游戏窗口 map 之后、模式切换之前新建的其他窗口 (launcher 弹窗
        //   等) 会盖过游戏 — 概率低, 发生时 fs-pick 日志可诊断;
        // - 本字段是容错锚点, 根因在 Wine 连带标记; 根治需 wine 侧只对
        //   前台窗口发 xdg set_fullscreen。
        uint64_t fsPriority = 0;
        // -- ARGB 窗口剪影掩码 --
        WindowMask mask;               // mask.w==0 = 从未生成
    };

    struct PopupRecord {
        uint32_t popupId = 0;
        uint32_t parentToplevel = 0;
        wl_resource* surface = nullptr;  // popup 的 wl_surface (pointer enter 目标)
        uint64_t surfaceKey = 0;
        int32_t offX = 0, offY = 0;      // 相对父窗口内容原点
        int w = 0, h = 0;
    };

    // -- 访问器 --

    // RAII lock guard。用 auto lk = tmgr.Lock(); 替代 std::lock_guard<std::mutex>
    [[nodiscard]] std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(toplevelMutex_); }

    // 读路径: find 语义, miss 返回 nullptr
    ToplevelState* FindToplevelLocked(uint32_t id) {
        auto it = toplevels_.find(id);
        return it != toplevels_.end() ? &it->second : nullptr;
    }

    // 写路径显式建档 (首次 commit / 状态转换等合法创建点)
    ToplevelState& EnsureToplevelLocked(uint32_t id) { return toplevels_[id]; }

    static bool HasFrame(const ToplevelState& st) { return !st.pixels.empty(); }

    void EraseToplevelLocked(uint32_t id) { toplevels_.erase(id); }

    // -- Z-order 管理 --
    const std::vector<uint32_t>& toplevelZOrder() const { return toplevelZOrder_; }
    void AddToZOrder(uint32_t id) {
        toplevelZOrder_.push_back(id);
        // 首次入列时取全屏优先级号 (fsPriority==0 即从未取过; Remove 后再
        // Add 的置顶重排不重复取)。必须在 AddToZOrder 内部做: 取号点若只
        // 挂在首帧 commit 处, RaiseToplevel 会抢先把未 commit 的窗口放进
        // z-order (红警2 的 set_fullscreen 先于首帧 commit, 2026-07 实测),
        // map 点的"不在列才取号"判定被绕过, 窗口永远拿不到号 → 全屏扫描
        // 输给任何旧窗口
        auto& st = EnsureToplevelLocked(id);
        if (st.fsPriority == 0) st.fsPriority = nextFsPriority_++;
    }
    void RemoveFromZOrder(uint32_t id) {
        auto it = std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), id);
        if (it != toplevelZOrder_.end()) toplevelZOrder_.erase(it);
    }
    bool IsInZOrder(uint32_t id) const {
        return std::find(toplevelZOrder_.begin(), toplevelZOrder_.end(), id) != toplevelZOrder_.end();
    }

    // -- 全屏优先级取号 (调用方须已持有 mutex; 规则与局限见 ToplevelState::fsPriority) --
    // 首次入 z-order 的取号在 AddToZOrder 内部完成; 此处仅"用户显式 raise
    // 已 fullscreen 窗口"的重新取号 (RaiseToplevel 用户路径)
    void BumpFsPriorityLocked(uint32_t id) { EnsureToplevelLocked(id).fsPriority = nextFsPriority_++; }

    // -- 只读遍历 --
    const std::unordered_map<uint32_t, ToplevelState>& toplevels() const { return toplevels_; }

    // -- 方法 --

    // toplevel 可见性: 隐藏/显示 toplevel, 控制渲染和输入是否包含该窗口。
    // 调用方须已持有 mutex。
    void HideToplevelLocked(uint32_t id) { EnsureToplevelLocked(id).isBackground = true; }
    void ShowToplevelLocked(uint32_t id) { EnsureToplevelLocked(id).isBackground = false; }

    // 查询隐藏/可见状态 (桌面合成时会排除已隐藏 and 未 hidden 的窗口)。
    bool IsToplevelVisibleLocked(uint32_t id, uint32_t desktopRootId);

    // popup 管理 (调用方须已持有 mutex, 除非另行说明)
    uint32_t FindPopupBySurfaceKey(uint64_t key) {
        auto it = popupBySurfaceKey_.find(key);
        return it != popupBySurfaceKey_.end() ? it->second : 0;
    }
    PopupRecord* FindPopup(uint32_t popupId) {
        auto it = popups_.find(popupId);
        return it != popups_.end() ? &it->second : nullptr;
    }
    void RegisterPopup(uint32_t popupId, const PopupRecord& rec) {
        popups_[popupId] = rec;
        popupBySurfaceKey_[rec.surfaceKey] = popupId;
    }
    void RemovePopupDataLocked(uint32_t popupId);
    uint32_t RemovePopupBySurfaceKeyLocked(uint64_t surfaceKey, uint32_t& outPopupId);
    // 遍历 popup 找属于某 parent 的所有 popup (OnToplevelDestroyed 级联清理)
    const std::unordered_map<uint32_t, PopupRecord>& popups() const { return popups_; }

    // 状态查询 (内部加锁)。minimized/fullscreen 的唯一权威字段在 ToplevelState;
    // 变更只经 WaylandServer::SetToplevel* (Ensure 建档 + dirty + 协议反应),
    // 本类不提供 setter — 历史上这里有一套无调用方且语义不等价的 setter, 已删除。
    bool IsToplevelMinimized(uint32_t id);
    bool IsToplevelFullscreen(uint32_t id);

    // resource 映射 (SendToplevelClose / xdg_toplevel 销毁)
    void RegisterToplevelResource(uint32_t id, wl_resource* tl);
    void UnregisterToplevelResource(uint32_t id);
    wl_resource* FindToplevelResource(uint32_t id);
    size_t ToplevelResourceCount() const { return toplevelResources_.size(); }

    // toplevelId → wl_surface 映射 (input focus / 渲染)
    void MapToplevelSurface(uint32_t id, wl_resource* surf);
    void UnmapToplevelSurface(uint32_t id);
    wl_resource* GetSurfaceForToplevel(uint32_t id);
    // 反查 (warp 门控/锚点换算: wine 侧请求只带 wl_surface)。
    // 线性扫描, 表很小且只在低频路径用 (warp 请求/约束析构)
    uint32_t FindToplevelBySurface(wl_resource* surf);
    size_t ToplevelSurfaceCount() const { return toplevelSurfaceMap_.size(); }

    // 异型窗口掩码
    bool TakeWindowMask(uint32_t id, int& w, int& h, std::vector<uint8_t>& out);

    // 标记 toplevel dirty (调用方须已持有 mutex)
    void MarkToplevelDirtyLocked(uint32_t id);

    // ID 分配
    uint32_t AllocateToplevelId() { return nextToplevelId_++; }

    // surface resource 管理 (surface key ↔ wl_resource 映射)
    wl_resource* FindSurfaceResource(uint64_t key) {
        auto it = surfaceResources_.find(key);
        return it != surfaceResources_.end() ? it->second : nullptr;
    }
    void RegisterSurfaceResource(uint64_t key, wl_resource* res) { surfaceResources_[key] = res; }
    void UnregisterSurfaceResource(uint64_t key) { surfaceResources_.erase(key); }
    bool ContainsSurfaceResource(wl_resource* res) {
        for (auto& [k, r] : surfaceResources_) if (r == res) return true;
        return false;
    }

    // 坐标/尺寸查询
    int GetToplevelX(uint32_t id);
    int GetToplevelY(uint32_t id);
    int GetToplevelW(uint32_t id);
    int GetToplevelH(uint32_t id);
    uint32_t GetToplevelShmFormat(uint32_t id);

private:
    std::mutex toplevelMutex_;
    std::atomic<uint32_t> nextToplevelId_{1};
    std::unordered_map<uint64_t, wl_resource*> surfaceResources_;
    std::unordered_map<uint32_t, ToplevelState> toplevels_;
    std::vector<uint32_t> toplevelZOrder_;
    uint64_t nextFsPriority_ = 1;  // 全屏优先级取号器 (toplevelMutex_ 保护)
    // 以下成员由自己的 mutex 保护 (非 toplevelMutex_)
    std::unordered_map<uint32_t, wl_resource*> toplevelSurfaceMap_;
    std::mutex toplevelSurfaceMutex_;
    std::unordered_map<uint32_t, wl_resource*> toplevelResources_;
    std::mutex toplevelResMutex_;
    std::unordered_map<uint32_t, PopupRecord> popups_;
    std::unordered_map<uint64_t, uint32_t> popupBySurfaceKey_;
};
