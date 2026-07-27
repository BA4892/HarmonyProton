#pragma once

#include <wayland-server-core.h>
#include <mutex>
#include <vector>

/*
 * DirectInput 类老游戏 (PAL2 等) 依赖的指针扩展协议 compositor 端实现:
 *
 * - wp_pointer_warp_v1: SetCursorPos 光标回中。老游戏 (经 wine dinput
 *   warp_check, 非独占 ~10ms/次) 把光标拉回窗口中心再读差值作相对位移;
 *   compositor 不支持时回中只在 wineserver 内 "虚拟成功", 下一个绝对
 *   motion 又把光标拽回设备位置 → dinput 增量 = 位置-中心 → 游戏光标
 *   被甩到边缘钉死 (实测症状: 屏幕中间一小块映射为游戏全屏幕)。
 * - zwp_pointer_constraints_v1: lock/confine 对象承载。只注册全局 +
 *   应答 locked/confined 即可满足 wine 的约束状态机; 锁销毁时若游戏
 *   给过 cursor_position_hint, 按协议把逻辑指针移到 hint。
 *
 * 故意不注册 zwp_relative_pointer_manager_v1 (也不实现该协议):
 *   relative_pointer 对象一旦存在, wine 的 pointer_handle_motion 会丢弃
 *   绝对 motion (wayland_pointer.c:176), wineserver 光标只能靠相对增量
 *   驱动; 我们的输入设备 (触屏/鼠标) 经 ArkTS 全是绝对坐标, 走绝对路径
 *   才能让 wineserver 光标始终 = 设备位置 (点击命中对齐), dinput 增量
 *   由 wine 自己从绝对位置差分得出 (dlls/dinput/mouse.c)。
 *
 * confine 的坐标钳制不在 compositor 侧做: ClipCursor 在 wineserver 内
 * 同样钳住光标 (与驱动无关), 两侧钳制结果一致, 无需重复实现。
 *
 * 参照 weston pointer-constraints.c。
 */
class PointerExtras {
public:
    static PointerExtras* GetInstance();

    // 注册 constraints + warp global (relative 故意不注册, 见头注释)
    void Register(wl_display* display);

    enum class ConstraintType { None, Lock, Confine };

    // 某 surface 当前生效的约束 (无 = None)。
    // surface 已销毁的约束条目视为不存在 (惰性失效)
    ConstraintType ConstraintFor(wl_resource* surface);

    // -- 协议接口实现 (public: wl 接口表在类外初始化, 与 wayland_server.h 同例) --
    // zwp_pointer_constraints_v1
    static void constr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void constr_lock_pointer(wl_client*, wl_resource*, uint32_t id,
                                    wl_resource* surface, wl_resource* pointer,
                                    wl_resource* region, uint32_t lifetime);
    static void constr_confine_pointer(wl_client*, wl_resource*, uint32_t id,
                                       wl_resource* surface, wl_resource* pointer,
                                       wl_resource* region, uint32_t lifetime);
    static void locked_destroy(wl_client*, wl_resource* r);
    static void locked_set_cursor_position_hint(wl_client*, wl_resource* r,
                                                wl_fixed_t sx, wl_fixed_t sy);
    static void confined_destroy(wl_client*, wl_resource* r);
    static void confined_set_region(wl_client*, wl_resource*, wl_resource*) {}
    static void locked_set_region(wl_client*, wl_resource*, wl_resource*) {}
    // wp_pointer_warp_v1
    static void warp_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void warp_warp_pointer(wl_client*, wl_resource*, wl_resource* surface,
                                  wl_resource* pointer, wl_fixed_t x, wl_fixed_t y,
                                  uint32_t serial);

private:
    struct Constraint {
        ConstraintType type = ConstraintType::None;
        wl_resource* surface = nullptr;   // 约束目标 surface
        wl_resource* res = nullptr;       // locked/confined 对象
        bool hasHint = false;             // locked 的 cursor_position_hint
        double hintX = 0, hintY = 0;
    };

    std::mutex mutex_;
    std::vector<Constraint> constraints_;

    // 约束资源析构共通处理: 摘掉条目, 如有 hint 则把逻辑指针移到 hint
    static void OnConstraintResourceDestroyed(wl_resource* r);
    static void constraints_bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void warp_bind(wl_client* client, void* data, uint32_t version, uint32_t id);
};
