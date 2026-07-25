#pragma once
#include <wayland-server-core.h>
#include <cstdint>
#include <mutex>

class ToplevelManager;

// 交互式窗口移动 (xdg_toplevel.move)。
// 状态由自身持有，读写 toplevel 位置需通过 ToplevelManager 引用。
class MoveGrabHandler {
public:
    // 开始抓取。serial 是 xdg_toplevel.move 携带的序列号。
    void StartMoveGrab(ToplevelManager& tmgr, uint32_t toplevelId, uint32_t serial);

    // 结束抓取。返回被结束的 toplevelId（已为 0 表示未在抓取）。
    void EndMoveGrab(ToplevelManager& tmgr);

    // 处理移动。返回 true 表示事件被 grab 消费。
    // wx/wy 为 wl_fixed_t 类型的窗口内绝对坐标 (InputManager 注入的 motion 坐标),
    // 内部与 lastWineX_/lastWineY_ 求差得到位移。
    bool ProcessMoveGrabMotion(ToplevelManager& tmgr, wl_fixed_t wx, wl_fixed_t wy);

    bool IsActive() const { return toplevelId_ != 0; }
    uint32_t GetToplevelId() const { return toplevelId_; }
    uint32_t GetSerial() const { return serial_; }

private:
    uint32_t toplevelId_ = 0;
    uint32_t serial_ = 0;
    int32_t lastWineX_ = 0;
    int32_t lastWineY_ = 0;
};
