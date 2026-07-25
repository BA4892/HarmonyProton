#include "desktop_root_manager.h"
#include "toplevel_manager.h"
#include "compositor/surface_data.h"
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

DesktopRootManager::DesktopRootManager(ToplevelManager& tmgr,
                                       uint32_t& desktopRootToplevelId,
                                       uint32_t& pendingDesktopRootToplevelId,
                                       bool& recognitionEnabled,
                                       const int32_t& outputW, const int32_t& outputH,
                                       FireEventFn fireEvent)
    : tmgr_(tmgr)
    , desktopRootToplevelId_(desktopRootToplevelId)
    , pendingDesktopRootToplevelId_(pendingDesktopRootToplevelId)
    , recognitionEnabled_(recognitionEnabled)
    , outputW_(outputW)
    , outputH_(outputH)
    , fireEvent_(std::move(fireEvent))
{
}

// -- desktop root 识别特征 --
// 为什么保留 appId 字符串匹配而不是改用纯协议特征 (首个接近全屏的
// toplevel): xdg_toplevel.app_id 本身就是协议提供的身份机制 (Wine 填
// 进程名), 语义明确; 而 "首个全屏窗口" 会把抢在 explorer 桌面之前弹
// 全屏的游戏/应用误判为 root。评估结论: 保留 appId 匹配。
// 识别失败的兜底: root 保持 0, 全部窗口按普通 toplevel 渲染, desktop
// 画面经 deprecated 全局 fb 路径维持可见 (MaintainDeprecatedGlobalFb)。
static bool IsExplorerDesktopShell(const std::string& appId)
{
    return appId.find("explorer") != std::string::npos;
}

// 接近全屏判定: 内容尺寸 ≥ 输出的 80% (explorer 桌面窗口铺满虚拟屏;
// 留 20% 余量吸收整数缩放/边框误差)
static bool IsNearFullOutputSize(int32_t contentW, int32_t contentH,
                                 int32_t outputW, int32_t outputH)
{
    return contentW >= outputW * 8 / 10 && contentH >= outputH * 8 / 10;
}

void DesktopRootManager::SetRecognitionEnabled(bool enabled)
{
    auto lk = tmgr_.Lock();
    recognitionEnabled_ = enabled;
    OH_LOG_INFO(LOG_APP, "[MW] desktop root recognition %{public}s",
                enabled ? "enabled" : "disabled");
}

uint32_t DesktopRootManager::PromotePending()
{
    uint32_t id = 0;
    {
        auto lk = tmgr_.Lock();
        id = pendingDesktopRootToplevelId_;
        auto* pst = id ? tmgr_.FindToplevelLocked(id) : nullptr;
        if (id == 0 || !pst || !ToplevelManager::HasFrame(*pst)) {
            if (id != 0) {
                OH_LOG_WARN(LOG_APP, "[MW] pending desktop root #%{public}u has no pixels, skip", id);
                pendingDesktopRootToplevelId_ = 0;
            }
            return 0;
        }
        if (desktopRootToplevelId_ == id) {
            pendingDesktopRootToplevelId_ = 0;
            return 0;
        }
        if (desktopRootToplevelId_ > 0) {
            if (auto* oldRoot = tmgr_.FindToplevelLocked(desktopRootToplevelId_))
                oldRoot->isBackground = true;
        }
        pst->isBackground = false;
        desktopRootToplevelId_ = id;
        pendingDesktopRootToplevelId_ = 0;
        pst->dirty = true;
    }

    OH_LOG_INFO(LOG_APP, "[MW] pending desktop root promoted: #%{public}u", id);
    if (fireEvent_) fireEvent_(id, "desktop_root", "{}");
    return id;
}

void DesktopRootManager::MarkRootDirtyLocked()
{
    tmgr_.MarkToplevelDirtyLocked(desktopRootToplevelId_);
}

DesktopRootManager::CheckRootResult
DesktopRootManager::CheckRootLocked(SurfaceData* sd, bool isFirstCommit,
                                    int contentW, int contentH)
{
    CheckRootResult result;
    if (!isFirstCommit) return result;

    uint32_t rootId = desktopRootToplevelId_;
    bool isExplorer = IsExplorerDesktopShell(sd->appId);
    bool isFullSize = IsNearFullOutputSize(contentW, contentH, outputW_, outputH_);

    if (!isExplorer || !isFullSize) return result;

    if (!recognitionEnabled_) {
        if (!sd->title.empty()) {
            pendingDesktopRootToplevelId_ = sd->toplevelId;
            tmgr_.EnsureToplevelLocked(sd->toplevelId).isBackground = false;
            OH_LOG_INFO(LOG_APP,
                        "[MW] full-size explorer #%{public}u pending as desktop root while recognition is disabled title=%{public}s",
                        sd->toplevelId, sd->title.c_str());
        } else {
            tmgr_.EnsureToplevelLocked(sd->toplevelId).isBackground = true;
            OH_LOG_INFO(LOG_APP,
                        "[MW] full-size explorer #%{public}u ignored while desktop root recognition is disabled (no title)",
                        sd->toplevelId);
        }
        return result;
    }

    if (rootId == 0) {
        if (pendingDesktopRootToplevelId_ > 0 &&
            pendingDesktopRootToplevelId_ != sd->toplevelId) {
            tmgr_.EnsureToplevelLocked(sd->toplevelId).isBackground = true;
            OH_LOG_INFO(LOG_APP,
                        "[MW] full-size explorer #%{public}u -> background, pending root #%{public}u exists",
                        sd->toplevelId, pendingDesktopRootToplevelId_);
        } else {
            OH_LOG_INFO(LOG_APP, "[MW] desktop root: #%{public}u appId=explorer",
                        sd->toplevelId);
            result.moveRendererTo = sd->toplevelId;
            desktopRootToplevelId_ = sd->toplevelId;
            pendingDesktopRootToplevelId_ = 0;
            result.fireDesktopRoot = true;
        }
        return result;
    }

    if (!sd->title.empty()) {
        wl_resource* oldSurf = tmgr_.GetSurfaceForToplevel(rootId);
        auto* oldSd = oldSurf ? static_cast<SurfaceData*>(wl_resource_get_user_data(oldSurf)) : nullptr;
        if (oldSd && oldSd->title.empty()) {
            OH_LOG_INFO(LOG_APP, "[MW] root switch: #%{public}u (empty) -> #%{public}u (%{public}s)",
                        rootId, sd->toplevelId, sd->title.c_str());
            tmgr_.EnsureToplevelLocked(rootId).isBackground = true;
            result.moveRendererFrom = rootId;
            result.moveRendererTo = sd->toplevelId;
            desktopRootToplevelId_ = sd->toplevelId;
            result.fireDesktopRoot = true;
        } else {
            tmgr_.EnsureToplevelLocked(sd->toplevelId).isBackground = true;
            OH_LOG_INFO(LOG_APP, "[MW] extra full-size explorer #%{public}u -> background",
                        sd->toplevelId);
        }
    } else {
        tmgr_.EnsureToplevelLocked(sd->toplevelId).isBackground = true;
        OH_LOG_INFO(LOG_APP, "[MW] extra full-size explorer #%{public}u (no title) -> background",
                    sd->toplevelId);
    }

    return result;
}
