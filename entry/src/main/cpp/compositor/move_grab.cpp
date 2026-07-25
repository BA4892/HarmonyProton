#include "move_grab.h"
#include "toplevel_manager.h"
#include <hilog/log.h>
#include <wayland-server-core.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

void MoveGrabHandler::StartMoveGrab(ToplevelManager& tmgr, uint32_t toplevelId, uint32_t serial) {
    auto lk = tmgr.Lock();
    toplevelId_ = toplevelId;
    serial_ = serial;
    lastWineX_ = 0;
    lastWineY_ = 0;
    OH_LOG_INFO(LOG_APP, "[MW-MOVE] start interactive move tl=%{public}u serial=%{public}u",
                toplevelId, serial);
}

void MoveGrabHandler::EndMoveGrab(ToplevelManager& tmgr) {
    OH_LOG_INFO(LOG_APP, "[MW-MOVE] end interactive move tl=%{public}u", toplevelId_);
    {
        auto lk = tmgr.Lock();
        toplevelId_ = 0;
        serial_ = 0;
        lastWineX_ = 0;
        lastWineY_ = 0;
    }
}

bool MoveGrabHandler::ProcessMoveGrabMotion(ToplevelManager& tmgr, wl_fixed_t wx, wl_fixed_t wy) {
    auto lk = tmgr.Lock();
    if (toplevelId_ == 0) return false;
    auto* st = tmgr.FindToplevelLocked(toplevelId_);
    if (!st || !st->hasPosition) return false;

    int32_t rx = wl_fixed_to_int(wx) + st->x;
    int32_t ry = wl_fixed_to_int(wy) + st->y;

    if (lastWineX_ == 0 && lastWineY_ == 0) {
        lastWineX_ = rx;
        lastWineY_ = ry;
        return true;
    }

    int32_t dx = rx - lastWineX_;
    int32_t dy = ry - lastWineY_;
    if (dx != 0 || dy != 0) {
        st->x += dx;
        st->y += dy;
        lastWineX_ = rx;
        lastWineY_ = ry;
        OH_LOG_INFO(LOG_APP, "[MW-MOVE] grab move tl=%{public}u dx=%{public}d dy=%{public}d newPos=(%{public}d,%{public}d)",
                    toplevelId_, dx, dy, st->x, st->y);
    }
    return true;
}
