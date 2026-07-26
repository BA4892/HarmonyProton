#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

// InputManager: 统一输入事件管理器
//
// 职责:
//   - 接收 ArkTS 通过 NAPI 发来的标准化事件 (唯一输入源)
//   - 坐标转换 (viewport → wine buffer letterbox)
//   - 状态追踪 (button bitmask, modifier state, pointer/keyboard focus)
//   - 事件入队 (pipe → Wayland 线程)
//   - 事件注入 (wl_pointer_*/wl_keyboard_* send)
//
// 线程模型:
//   - NAPI/JS 线程: SendPointerEvent / SendKeyEvent → 坐标变换 + 状态更新 + Enqueue
//   - Wayland 线程: FlushQueue → 去重 → Inject → wl_display_flush_clients
//
// 所有 wl_*_send_* 调用必须在 Wayland 线程 (通过 pipe 唤醒)

class Seat;  // 前向声明

class InputManager {
public:
    static InputManager* GetInstance();

    // -- 生命周期 (WaylandServer::Start/Stop 调用) --
    void Initialize(wl_display* display);
    void Shutdown();

    // -- NAPI 入口 (JS 线程调用) --
    // action: ArkTS MouseAction (Press=1, Release=2, Move=3)
    // px/py: 已转为物理像素的坐标
    // button: 已由 ArkTS MouseMap 映射的 evdev button code (0x110/0x111/0x112)
    void SendPointerEvent(uint32_t toplevelId, int action, double px, double py, int button);

    // evdevCode: 已由 ArkTS KeyMap 映射的 evdev keycode
    // pressed: true=按下, false=释放
    void SendKeyEvent(uint32_t toplevelId, int evdevCode, bool pressed);

    // axis: 0=垂直(SCROLL_VERTICAL), 1=水平(SCROLL_HORIZONTAL)
    // value: 轴值 (正值=向下/向右, 负值=向上/向左)
    // scrollStep: ArkTS AxisEvent.scrollStep
    // px/py: 鼠标在组件上的物理像素坐标
    void SendScrollEvent(uint32_t toplevelId, int axis, double value, int scrollStep, double px, double py);

    // -- Wayland 线程注入 (由 FlushQueue 调用) --
    void InjectPointerEnter(uint32_t tl, wl_resource* surface, wl_fixed_t sx, wl_fixed_t sy);
    void InjectPointerMotion(wl_fixed_t sx, wl_fixed_t sy);
    void InjectPointerButton(uint32_t button, uint32_t state);
    void InjectPointerAxis(int axis, wl_fixed_t value);
    void InjectPointerLeave();
    void InjectKeyboardEnter(uint32_t tl, wl_resource* surface);
    void InjectKeyboardKey(uint32_t key, uint32_t state);
    void InjectKeyboardLeave();
    void InjectKeyboardModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

    // -- 状态重置 (Seat resource destroy 时调用) --
    void ResetPointerEnter();
    void ResetKeyboardEnter();

    // surface 销毁时重置焦点, 防止后续 Inject*Leave 引用已销毁的 surface
    // 如果不重置, 会导致 Wayland 协议错误 "invalid object" → Wine 断开连接
    void OnSurfaceDestroyed(wl_resource* surface);

    // -- 窗口可见性 (输入抑制) --
    void SetToplevelVisible(uint32_t tl, bool visible);

    // -- Focus 查询 (线程安全) --
    bool HasPointerFocus() const { return pointerFocusedToplevel_.load() != 0; }
    bool NeedsPointerEnter() const;
    uint32_t GetPointerFocusedToplevel() const { return pointerFocusedToplevel_.load(); }

    bool HasKeyboardFocus() const { return keyboardEntered_.load(); }
    uint32_t GetKeyboardFocusedToplevel() const { return keyboardFocusedToplevel_.load(); }

    // -- 辅助: 物理像素 → Wine 逻辑坐标映射 (供 FindToplevelAt 等使用) --
    wl_fixed_t CoordTransform(double px, double py, uint32_t tl, wl_fixed_t* outX, wl_fixed_t* outY);

    // 指针 warp 锚点 (wp_pointer_warp_v1 请求 / lock hint → PointerExtras 调入,
    // Wayland 线程)。sx/sy 是 wine 的 surface 局部坐标。
    void OnPointerWarp(wl_resource* surface, double sx, double sy);

private:
    InputManager() = default;

    // -- 事件队列 (NAPI → Wayland 线程) --
    struct InputEvent {
        enum Type { PTR_ENTER, PTR_LEAVE, PTR_MOTION, PTR_BUTTON, PTR_AXIS,
                    KBD_ENTER, KBD_LEAVE, KBD_KEY, KBD_MODIFIERS } type;
        uint32_t tl = 0;
        wl_resource* surface = nullptr;
        wl_fixed_t x = 0, y = 0;
        uint32_t btn_or_key = 0;
        uint32_t state = 0;
        // axis fields
        int axis = 0;           // 0=vertical, 1=horizontal
        wl_fixed_t axis_value = 0;
        // modifiers fields
        uint32_t mod_depressed = 0, mod_latched = 0, mod_locked = 0, mod_group = 0;
    };

    std::mutex queueMutex_;
    std::vector<InputEvent> queue_;
    int pipeRead_ = -1, pipeWrite_ = -1;
    struct wl_event_source* pipeSource_ = nullptr;
    wl_display* display_ = nullptr;

    void Enqueue(InputEvent::Type type, uint32_t tl, wl_resource* surface,
                 wl_fixed_t x, wl_fixed_t y, uint32_t btn_or_key, uint32_t state);
    void EnqueueModifiers();  // 入队当前 modifiers_depressed_ 等状态
    void FlushQueue();
    static int OnPipeReadable(int fd, uint32_t mask, void* data);

    // -- 状态追踪 --
    // button bitmask: bit0=left(0x110), bit1=right(0x111), bit2=middle(0x112)
    static constexpr unsigned kBtnBitLeft   = 0;
    static constexpr unsigned kBtnBitRight  = 1;
    static constexpr unsigned kBtnBitMiddle = 2;
    uint32_t pressedButtons_ = 0;

    unsigned ButtonToBit(uint32_t btn);
    uint32_t BitToButton(unsigned bit);

    // modifier state
    uint32_t modifiers_depressed_ = 0;
    uint32_t modifiers_latched_ = 0;
    uint32_t modifiers_locked_ = 0;
    uint32_t modifiers_group_ = 0;
    void UpdateModifiers(int evdevCode, bool pressed);
    bool IsModifierKey(int evdevCode);

    // pointer focus
    std::atomic<uint32_t> pointerFocusedToplevel_{0};
    // atomic: NAPI 线程 (SendPointerEvent) 用它做 surface 级 enter/leave 判定,
    // Wayland 线程 (Inject*) 写入 — desktop 模式菜单层与父窗口同 toplevelId,
    // 仅比较 toplevelId 无法察觉焦点需要在两个 surface 间切换
    std::atomic<wl_resource*> pointerFocusedSurface_{nullptr};
    std::atomic<uint32_t> pointerEnterSerial_{0};
    std::atomic<uint32_t> serial_{1};

    // keyboard focus (独立于 pointer)
    std::atomic<uint32_t> keyboardFocusedToplevel_{0};
    wl_resource* keyboardFocusedSurface_ = nullptr;
    std::atomic<bool> keyboardEntered_{false};

    // 窗口可见性 (鸿蒙侧最小化时抑制输入)
    std::mutex visibleMutex_;
    std::unordered_map<uint32_t, bool> toplevelVisible_;

    /*
     * 指针 warp 状态 (DirectInput 类老游戏: wine dinput warp_check 每 ~10ms
     * SetCursorPos 回窗口中心, 再读两次 GetCursorPos 差值作相对位移 —
     * dlls/dinput/mouse.c)。没有 warp 通道时回中只在 wineserver 内生效,
     * 下一个绝对 motion 又把光标拽回设备位置 → dinput 增量 = 位置-中心
     * → 游戏光标被甩到边缘 (实测: 屏幕中间一小块映射为游戏全屏幕)。
     *
     * warpActive_=true 时用户输入不再是绝对定位, 而是增量源:
     *   逻辑位置 = warp 锚点 + 用户输入增量的累加。
     * 这正是硬件光标在普通 compositor 里的工作方式 (weston: 光标位置是
     * compositor 侧状态, 输入设备只产生 delta); 我们的触屏/鼠标都经 ArkTS
     * 以绝对坐标送达, 所以 warp 后必须自己把绝对流拆成增量。
     * desktop 模式锚点/输入都在桌面坐标空间; PC 模式在窗口局部坐标空间。
     * 仅 ZC 游戏激活 (OnPointerWarp 里门控): SHM 游戏读绝对坐标, 激活反破坏映射。
     */
    std::mutex warpMutex_;
    bool warpActive_ = false;
    wl_resource* warpSurface_ = nullptr;           // 哪个 surface 激活了 warp (NULL=无)
    double warpLogicalX_ = 0, warpLogicalY_ = 0;   // 逻辑指针位置
    double lastUserX_ = 0, lastUserY_ = 0;         // 上一次用户输入 (增量基准)
    bool hasLastUser_ = false;
    // warp 共用的坐标处理 (warpMutex_ 已持有): 输入用户坐标, 输出逻辑坐标
    // (并更新增量基准)。warpSurface_ 非 NULL 时仅对该 surface 生效
    void ApplyWarpLogicLocked(wl_resource* surface, double userX, double userY,
                              bool isPress, double& outX, double& outY);
};
