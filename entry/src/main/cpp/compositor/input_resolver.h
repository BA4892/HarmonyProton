#pragma once
#include <wayland-server-core.h>
#include <cstdint>

class ToplevelManager;
class DesktopCompositor;

// 输入命中目标 (原 WaylandServer 嵌套类型, 外部调用方通过 wayland_server.h 的 using 别名继续使用)
struct InputTarget {
    uint32_t toplevelId = 0;         // 事件归属 toplevel (raise/键盘焦点)
    wl_resource* surface = nullptr;  // pointer enter 目标
    int originX = 0, originY = 0;    // surface 的桌面原点 (输入坐标换算基)
    // 桌面坐标 → surface 局部坐标的缩放除数。
    // 全屏窗口保比例缩放显示, 局部坐标 = (桌面坐标 - origin) / scale; 普通窗口为 1
    float scale = 1.0f;
    // true = 该点落在全屏黑边内: 调用方只吞 PRESS (防幻影点击/焦点切换);
    // MOVE/RELEASE 照常按 origin/scale 透传给全屏窗口 (越界坐标由
    // winewayland clamp, 吞掉会导致按键状态卡死)
    bool swallow = false;
};

// 输入命中裁决 (依赖 ToplevelManager + DesktopCompositor)
//
// 不变式: 命中顺序固定为 全屏窗口(含其 subsurface 层) → subsurface 层 →
// toplevel → desktop root 兜底, 与渲染层序一致。zero-copy GL 层不参与
// 置顶命中 (渲染时被遮挡重绘压回, 命中同样下放给 z-order 循环)。
// 全屏黑边命中标 swallow, 调用方只吞 PRESS — MOVE/RELEASE 照常透传,
// 否则按下拖到黑边松手会丢 release, 按键状态永久卡死 (见实现注释)。
class InputResolver {
public:
    InputResolver(ToplevelManager& tmgr, DesktopCompositor& compositor,
                  const uint32_t& desktopRootToplevelId,
                  const int32_t& outputW, const int32_t& outputH);

    // Desktop 模式: (x,y) 处的精确输入目标。
    // 命中 subsurface 菜单层时返回层自己的 wl_surface + 层桌面原点 —
    // 菜单可伸出父窗口边界, 事件必须 enter 菜单 surface 并用菜单相对坐标,
    // 否则经父窗口 surface 的越界坐标会被 winewayland 的 motion clamp
    // (wayland_pointer.c "bring them within bounds") 夹回窗口内, 菜单收不到。
    // 未命中层时回退 toplevel / desktop root。返回 false = surface 不可用。
    bool FindInputTargetAt(int x, int y, InputTarget& out);

    // Desktop 模式: 在合成帧中查找包含 (x,y) 的 toplevel (用于输入路由)
    uint32_t FindToplevelAt(int x, int y);

    // surface 指针是否仍存活 (输入注入前的防御校验, 遍历 surfaceResources_)
    bool IsSurfaceAlive(wl_resource* surface);

private:
    ToplevelManager& tmgr_;
    DesktopCompositor& compositor_;
    const uint32_t& desktopRootToplevelId_;
    const int32_t& outputW_;
    const int32_t& outputH_;
};
