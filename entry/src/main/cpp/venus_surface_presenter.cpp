#define VK_USE_PLATFORM_OHOS 1

#include "venus_surface_presenter.h"

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "venus-presenter"

namespace winehua {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr uint64_t kDefaultFramePeriodNs = 16666667;
constexpr uint64_t kDispatchLeadNs = 500000;

uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

uint64_t NormalizeFramePeriodNs(uint64_t value)
{
    return value >= 4000000 && value <= 100000000 ? value : kDefaultFramePeriodNs;
}

uint64_t PacingPeriodNs(uint64_t displayPeriodNs)
{
    return displayPeriodNs > kDispatchLeadNs ? displayPeriodNs - kDispatchLeadNs
                                             : displayPeriodNs;
}

bool TraceFrameOrder()
{
    const char* mode = std::getenv("VKR_WINEHUA_SHADOW_FROM_HOST");
    return mode && (!std::strcmp(mode, "none") || !std::strcmp(mode, "precise"));
}

VkPipelineStageFlags SourceStage(VkImageLayout layout)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    default:
        return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

VkAccessFlags SourceAccess(VkImageLayout layout)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_GENERAL:
        return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    default:
        return VK_ACCESS_MEMORY_READ_BIT;
    }
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
{
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> choices = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const auto choice : choices) {
        if (supported & choice) return choice;
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

} // namespace

struct VenusSurfaceQueueTarget::Impl {
    struct Frame {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence complete = VK_NULL_HANDLE;
    };

    int Attach(uint64_t surfaceKey, uint64_t framePeriodNs, OHNativeWindow* window)
    {
        if (!surfaceKey || !window) return -EINVAL;
        std::lock_guard<std::mutex> lock(mutex_);
        DestroyVulkanLocked(true);
        if (window_) OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = window;
        surfaceKey_ = surfaceKey;
        displayPeriodNs_ = NormalizeFramePeriodNs(framePeriodNs);
        framePeriodNs_ = PacingPeriodNs(displayPeriodNs_);
        lastPresentNs_ = 0;
        framesPresented_ = 0;
        lastSerial_ = 0;
        serialRegressions_ = 0;
        failures_ = 0;
        throttled_ = 0;
        firstPresentedNs_ = 0;
        totalPresentUs_ = 0;
        maxPresentUs_ = 0;
        totalWaitFenceUs_ = 0;
        totalAcquireUs_ = 0;
        totalSubmitUs_ = 0;
        totalQueuePresentUs_ = 0;
        totalReleaseWaitUs_ = 0;
        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] target attached key=%{public}llu "
                    "window=%{public}p display_period_us=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey_), window_,
                    static_cast<unsigned long long>(displayPeriodNs_ / 1000));
        return 0;
    }

    int Detach(uint64_t surfaceKey)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (surfaceKey_ && surfaceKey && surfaceKey_ != surfaceKey) return -EINVAL;
        DestroyVulkanLocked(true);
        if (window_) OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = nullptr;
        surfaceKey_ = 0;
        OH_LOG_INFO(LOG_APP, "[VENUS-PRESENT][NCP] target detached key=%{public}llu",
                    static_cast<unsigned long long>(surfaceKey));
        return 0;
    }

    int SetFramePeriod(uint64_t framePeriodNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        displayPeriodNs_ = NormalizeFramePeriodNs(framePeriodNs);
        framePeriodNs_ = PacingPeriodNs(displayPeriodNs_);
        return 0;
    }

    int Present(uint32_t contextId,
                uintptr_t instance,
                uintptr_t physicalDevice,
                uintptr_t device,
                uintptr_t queue,
                uint64_t image,
                uint32_t queueFamily,
                uint32_t width,
                uint32_t height,
                uint32_t format,
                uint32_t layout,
                uint32_t serial,
                uint64_t* nextPresentDeadlineNs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t presentStartNs = NowNs();
        if (nextPresentDeadlineNs) *nextPresentDeadlineNs = 0;
        if (!window_) {
            OH_LOG_ERROR(LOG_APP, "[VENUS-PRESENT][NCP] present no window");
            return -EAGAIN;
        }

        const uint64_t nowNs = NowNs();
        if (lastPresentNs_ && nowNs - lastPresentNs_ < framePeriodNs_) {
            if (nextPresentDeadlineNs)
                *nextPresentDeadlineNs = lastPresentNs_ + framePeriodNs_;
            ++throttled_;
            return 1;
        }

        const VkInstance hostInstance = reinterpret_cast<VkInstance>(instance);
        const VkPhysicalDevice hostPhysical =
            reinterpret_cast<VkPhysicalDevice>(physicalDevice);
        const VkDevice hostDevice = reinterpret_cast<VkDevice>(device);
        const VkQueue hostQueue = reinterpret_cast<VkQueue>(queue);
        const VkImage sourceImage = reinterpret_cast<VkImage>(static_cast<uintptr_t>(image));
        const VkFormat sourceFormat = static_cast<VkFormat>(format);
        const VkImageLayout sourceLayout = static_cast<VkImageLayout>(layout);
        int initError = 0;
        if (!EnsureVulkanLocked(hostInstance, hostPhysical, hostDevice, hostQueue,
                                queueFamily, width, height, sourceFormat, initError)) {
            ++failures_;
            return initError ? initError : -EIO;
        }
        Frame& frame = frames_[frameIndex_++ % frames_.size()];
        uint64_t stageStartNs = NowNs();
        VkResult result = vkWaitForFences(
            device_, 1, &frame.complete, VK_TRUE, displayPeriodNs_ * 4);
        const uint64_t waitFenceUs = (NowNs() - stageStartNs) / 1000;
        if (result == VK_TIMEOUT) return 1;
        if (result != VK_SUCCESS) return FailLocked("wait fence", result, serial);

        uint32_t imageIndex = 0;
        stageStartNs = NowNs();
        result = vkAcquireNextImageKHR(device_, swapchain_, displayPeriodNs_ * 2,
                                       frame.acquired, VK_NULL_HANDLE, &imageIndex);
        const uint64_t acquireUs = (NowNs() - stageStartNs) / 1000;
        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            ++throttled_;
            return 1;
        }
        /*
         * Some Harmony Vulkan drivers report a transient SurfaceQueue target
         * loss as VK_ERROR_UNKNOWN (rather than VK_ERROR_OUT_OF_DATE_KHR or
         * VK_ERROR_SURFACE_LOST_KHR).  Treat these WSI-only results as a
         * dirty swapchain and let the next present rebuild it.  No image was
         * acquired in this branch, so destroying the target is safe after the
         * per-frame fence wait above.  Do not convert it to device-lost: the
         * Vulkan device and guest Venus context remain usable.
         */
        if (result == VK_ERROR_OUT_OF_DATE_KHR ||
            result == VK_ERROR_SURFACE_LOST_KHR ||
            result == VK_ERROR_UNKNOWN) {
            swapchainDirty_ = true;
            return -EAGAIN;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return FailLocked("acquire", result, serial);

        vkResetFences(device_, 1, &frame.complete);
        vkResetCommandBuffer(frame.command, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(frame.command, &begin);
        if (result != VK_SUCCESS) return FailLocked("begin command", result, serial);

        VkImageMemoryBarrier sourceToTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sourceToTransfer.srcAccessMask = SourceAccess(sourceLayout);
        sourceToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceToTransfer.oldLayout = sourceLayout;
        sourceToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToTransfer.image = sourceImage;
        sourceToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sourceToTransfer.subresourceRange.levelCount = 1;
        sourceToTransfer.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier targetToTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        targetToTransfer.srcAccessMask = targetInitialized_[imageIndex]
            ? VK_ACCESS_MEMORY_READ_BIT : 0;
        targetToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        targetToTransfer.oldLayout = targetInitialized_[imageIndex]
            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        targetToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        targetToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToTransfer.image = swapchainImages_[imageIndex];
        targetToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        targetToTransfer.subresourceRange.levelCount = 1;
        targetToTransfer.subresourceRange.layerCount = 1;

        const std::array<VkImageMemoryBarrier, 2> before = {
            sourceToTransfer, targetToTransfer
        };
        vkCmdPipelineBarrier(frame.command,
                             SourceStage(sourceLayout) |
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(before.size()), before.data());

        if (canBlit_) {
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = {
                static_cast<int32_t>(sourceWidth_),
                static_cast<int32_t>(sourceHeight_), 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = {
                static_cast<int32_t>(extent_.width),
                static_cast<int32_t>(extent_.height), 1};
            vkCmdBlitImage(frame.command,
                           sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           swapchainImages_[imageIndex],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_NEAREST);
        } else {
            VkImageCopy copy{};
            copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.srcSubresource.layerCount = 1;
            copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.dstSubresource.layerCount = 1;
            copy.extent = {sourceWidth_, sourceHeight_, 1};
            vkCmdCopyImage(frame.command,
                           sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           swapchainImages_[imageIndex],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }

        VkImageMemoryBarrier sourceRestore{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sourceRestore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceRestore.dstAccessMask = SourceAccess(sourceLayout);
        sourceRestore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceRestore.newLayout = sourceLayout;
        sourceRestore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceRestore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceRestore.image = sourceImage;
        sourceRestore.subresourceRange = sourceToTransfer.subresourceRange;

        VkImageMemoryBarrier targetToPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        targetToPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        targetToPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        targetToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        targetToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        targetToPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToPresent.image = swapchainImages_[imageIndex];
        targetToPresent.subresourceRange = targetToTransfer.subresourceRange;
        const std::array<VkImageMemoryBarrier, 2> after = {
            sourceRestore, targetToPresent
        };
        vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(after.size()), after.data());

        result = vkEndCommandBuffer(frame.command);
        if (result != VK_SUCCESS) return FailLocked("end command", result, serial);

        const uint64_t timestamp = NowNs();
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_UI_TIMESTAMP, timestamp);
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished_[imageIndex];
        stageStartNs = NowNs();
        result = vkQueueSubmit(queue_, 1, &submit, frame.complete);
        const uint64_t submitUs = (NowNs() - stageStartNs) / 1000;
        if (result != VK_SUCCESS) return FailLocked("queue submit", result, serial);

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinished_[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &imageIndex;
        stageStartNs = NowNs();
        result = vkQueuePresentKHR(queue_, &present);
        const uint64_t queuePresentUs = (NowNs() - stageStartNs) / 1000;

        stageStartNs = NowNs();
        const VkResult fenceResult = vkWaitForFences(
            device_, 1, &frame.complete, VK_TRUE, displayPeriodNs_ * 4);
        const uint64_t releaseWaitUs = (NowNs() - stageStartNs) / 1000;
        if (fenceResult != VK_SUCCESS)
            return FailLocked("source release fence", fenceResult, serial);
        targetInitialized_[imageIndex] = true;

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchainDirty_ = true;
            return -EAGAIN;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return FailLocked("queue present", result, serial);

        lastPresentNs_ = timestamp;
        ++framesPresented_;
        if (lastSerial_ && serial <= lastSerial_) ++serialRegressions_;
        lastSerial_ = serial;
        const uint64_t frameEndNs = NowNs();
        const uint64_t presentUs = (frameEndNs - presentStartNs) / 1000;
        if (!firstPresentedNs_) firstPresentedNs_ = frameEndNs;
        totalPresentUs_ += presentUs;
        maxPresentUs_ = std::max(maxPresentUs_, presentUs);
        totalWaitFenceUs_ += waitFenceUs;
        totalAcquireUs_ += acquireUs;
        totalSubmitUs_ += submitUs;
        totalQueuePresentUs_ += queuePresentUs;
        totalReleaseWaitUs_ += releaseWaitUs;
        if (nextPresentDeadlineNs)
            *nextPresentDeadlineNs = lastPresentNs_ + framePeriodNs_;
        if (TraceFrameOrder() && framesPresented_ <= 600) {
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-ORDER][NCP] frame=%{public}llu serial=%{public}u "
                        "serial_regress=%{public}llu source=0x%{public}llx "
                        "target_index=%{public}u target=0x%{public}llx "
                        "timestamp=%{public}llu",
                        static_cast<unsigned long long>(framesPresented_), serial,
                        static_cast<unsigned long long>(serialRegressions_),
                        static_cast<unsigned long long>(image), imageIndex,
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                            swapchainImages_[imageIndex])),
                        static_cast<unsigned long long>(timestamp));
        }
        if (framesPresented_ == 1 || !(framesPresented_ % 120)) {
            const uint64_t elapsedNs = frameEndNs - firstPresentedNs_;
            const uint64_t fpsX100 = elapsedNs && framesPresented_ > 1
                ? ((framesPresented_ - 1) * 100ULL * 1000000000ULL) / elapsedNs
                : 0;
            OH_LOG_INFO(LOG_APP,
                        "[VENUS-PRESENT][NCP] frames=%{public}llu ctx=%{public}u "
                        "key=%{public}llu serial=%{public}u size=%{public}ux%{public}u "
                        "target=%{public}ux%{public}u format=%{public}u "
                        "fps=%{public}llu.%{public}02llu gpu_copy=1 "
                        "present_us_avg=%{public}llu max=%{public}llu "
                        "wait_fence_avg=%{public}llu acquire_avg=%{public}llu "
                        "submit_avg=%{public}llu queue_present_avg=%{public}llu "
                        "release_wait_avg=%{public}llu failures=%{public}llu "
                        "throttled=%{public}llu",
                        static_cast<unsigned long long>(framesPresented_), contextId,
                        static_cast<unsigned long long>(surfaceKey_), serial,
                        width, height, extent_.width, extent_.height, format,
                        static_cast<unsigned long long>(fpsX100 / 100),
                        static_cast<unsigned long long>(fpsX100 % 100),
                        static_cast<unsigned long long>(totalPresentUs_ / framesPresented_),
                        static_cast<unsigned long long>(maxPresentUs_),
                        static_cast<unsigned long long>(totalWaitFenceUs_ / framesPresented_),
                        static_cast<unsigned long long>(totalAcquireUs_ / framesPresented_),
                        static_cast<unsigned long long>(totalSubmitUs_ / framesPresented_),
                        static_cast<unsigned long long>(totalQueuePresentUs_ / framesPresented_),
                        static_cast<unsigned long long>(totalReleaseWaitUs_ / framesPresented_),
                        static_cast<unsigned long long>(failures_),
                        static_cast<unsigned long long>(throttled_));
        }
        return 0;
    }

    ~Impl()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        DestroyVulkanLocked(true);
        if (window_) OH_NativeWindow_DestroyNativeWindow(window_);
    }

private:
    int FailLocked(const char* operation, VkResult result, uint32_t serial)
    {
        ++failures_;
        if (failures_ == 1 || !(failures_ % 60)) {
            OH_LOG_ERROR(LOG_APP,
                         "[VENUS-PRESENT][NCP] %{public}s failed result=%{public}d "
                         "serial=%{public}u failures=%{public}llu",
                         operation, static_cast<int32_t>(result), serial,
                         static_cast<unsigned long long>(failures_));
        }
        return result == VK_ERROR_DEVICE_LOST ? -ENODEV : -EIO;
    }

    bool EnsureVulkanLocked(VkInstance instance,
                            VkPhysicalDevice physicalDevice,
                            VkDevice device,
                            VkQueue queue,
                            uint32_t queueFamily,
                            uint32_t width,
                            uint32_t height,
                            VkFormat sourceFormat,
                            int& error)
    {
        const bool sameSource = instance_ == instance && physicalDevice_ == physicalDevice &&
            device_ == device && queue_ == queue && queueFamily_ == queueFamily &&
            sourceWidth_ == width && sourceHeight_ == height &&
            sourceFormat_ == sourceFormat;
        if (swapchain_ && sameSource && !swapchainDirty_) return true;

        /* A WSI error can leave the platform present queue waiting forever.
         * The dirty path has already waited for the per-frame fence and has
         * no newly acquired image, so it must not perform an unbounded
         * vkQueueWaitIdle during recovery. */
        DestroyVulkanLocked(!swapchainDirty_);
        instance_ = instance;
        physicalDevice_ = physicalDevice;
        device_ = device;
        queue_ = queue;
        queueFamily_ = queueFamily;
        sourceWidth_ = width;
        sourceHeight_ = height;
        sourceFormat_ = sourceFormat;
        swapchainDirty_ = false;

        if (!instance_ || !physicalDevice_ || !device_ || !queue_) {
            error = -EINVAL;
            return false;
        }

        OH_NativeWindow_NativeWindowHandleOpt(
            window_, SET_BUFFER_GEOMETRY,
            static_cast<int32_t>(width), static_cast<int32_t>(height));
        OH_NativeWindow_NativeWindowHandleOpt(
            window_, SET_USAGE,
            static_cast<uint64_t>(NATIVEBUFFER_USAGE_HW_RENDER |
                                  NATIVEBUFFER_USAGE_HW_TEXTURE));
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_TIMEOUT, 0);

        VkSurfaceCreateInfoOHOS surfaceInfo{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};
        surfaceInfo.window = window_;
        VkResult result = vkCreateSurfaceOHOS(instance_, &surfaceInfo, nullptr, &surface_);
        if (result != VK_SUCCESS) {
            error = -ENOTSUP;
            return false;
        }

        VkBool32 presentSupported = VK_FALSE;
        result = vkGetPhysicalDeviceSurfaceSupportKHR(
            physicalDevice_, queueFamily_, surface_, &presentSupported);
        if (result != VK_SUCCESS || !presentSupported) {
            error = -ENOTSUP;
            return false;
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice_, surface_, &capabilities);
        if (result != VK_SUCCESS ||
            !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            error = -ENOTSUP;
            return false;
        }

        uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice_, surface_, &formatCount, nullptr);
        if (result != VK_SUCCESS || !formatCount) {
            error = -ENOTSUP;
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice_, surface_, &formatCount, formats.data());
        if (result != VK_SUCCESS) {
            error = -EIO;
            return false;
        }
        VkSurfaceFormatKHR chosen = formats.front();
        for (const auto& candidate : formats) {
            if (candidate.format == sourceFormat_) {
                chosen = candidate;
                break;
            }
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
                candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
                chosen = candidate;
        }
        targetFormat_ = chosen.format;

        extent_ = capabilities.currentExtent;
        if (extent_.width == UINT32_MAX) {
            extent_.width = std::clamp(width, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent_.height = std::clamp(height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        uint32_t imageCount = std::max(2u, capabilities.minImageCount);
        if (capabilities.maxImageCount)
            imageCount = std::min(imageCount, capabilities.maxImageCount);

        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface_;
        create.minImageCount = imageCount;
        create.imageFormat = chosen.format;
        create.imageColorSpace = chosen.colorSpace;
        create.imageExtent = extent_;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = capabilities.currentTransform;
        create.compositeAlpha = ChooseCompositeAlpha(capabilities.supportedCompositeAlpha);
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        create.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            error = -ENOTSUP;
            return false;
        }

        result = vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        if (result != VK_SUCCESS || !imageCount) {
            error = -EIO;
            return false;
        }
        swapchainImages_.resize(imageCount);
        result = vkGetSwapchainImagesKHR(
            device_, swapchain_, &imageCount, swapchainImages_.data());
        if (result != VK_SUCCESS) {
            error = -EIO;
            return false;
        }
        targetInitialized_.assign(imageCount, false);
        renderFinished_.resize(imageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& semaphore : renderFinished_) {
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
                error = -ENOMEM;
                return false;
            }
        }

        VkFormatProperties sourceProperties{};
        VkFormatProperties targetProperties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, sourceFormat_, &sourceProperties);
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, targetFormat_, &targetProperties);
        canBlit_ =
            (sourceProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
            (targetProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        if (!canBlit_ &&
            (sourceFormat_ != targetFormat_ || width != extent_.width ||
             height != extent_.height)) {
            error = -ENOTSUP;
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            error = -EIO;
            return false;
        }
        frames_.resize(std::min<size_t>(3, swapchainImages_.size()));
        std::vector<VkCommandBuffer> commands(frames_.size());
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = static_cast<uint32_t>(commands.size());
        result = vkAllocateCommandBuffers(device_, &allocate, commands.data());
        if (result != VK_SUCCESS) {
            error = -ENOMEM;
            return false;
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (size_t i = 0; i < frames_.size(); ++i) {
            frames_[i].command = commands[i];
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                                  &frames_[i].acquired) != VK_SUCCESS ||
                vkCreateFence(device_, &fenceInfo, nullptr,
                              &frames_[i].complete) != VK_SUCCESS) {
                error = -ENOMEM;
                return false;
            }
        }

        OH_LOG_INFO(LOG_APP,
                    "[VENUS-PRESENT][NCP] swapchain ready key=%{public}llu "
                    "source=%{public}ux%{public}u target=%{public}ux%{public}u "
                    "source_format=%{public}u target_format=%{public}u images=%{public}u "
                    "blit=%{public}d queue_family=%{public}u",
                    static_cast<unsigned long long>(surfaceKey_),
                    width, height, extent_.width, extent_.height,
                    static_cast<uint32_t>(sourceFormat_),
                    static_cast<uint32_t>(targetFormat_), imageCount,
                    canBlit_, queueFamily_);
        return true;
    }

    void DestroyVulkanLocked(bool waitQueue)
    {
        if (waitQueue && device_ && queue_) vkQueueWaitIdle(queue_);
        if (device_) {
            for (auto& frame : frames_) {
                if (frame.complete) vkDestroyFence(device_, frame.complete, nullptr);
                if (frame.acquired) vkDestroySemaphore(device_, frame.acquired, nullptr);
            }
            for (const VkSemaphore semaphore : renderFinished_) {
                if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
            }
            if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
            if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        }
        if (instance_ && surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        frames_.clear();
        renderFinished_.clear();
        targetInitialized_.clear();
        swapchainImages_.clear();
        commandPool_ = VK_NULL_HANDLE;
        swapchain_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        instance_ = VK_NULL_HANDLE;
        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        queueFamily_ = 0;
        sourceWidth_ = 0;
        sourceHeight_ = 0;
        sourceFormat_ = VK_FORMAT_UNDEFINED;
        targetFormat_ = VK_FORMAT_UNDEFINED;
        extent_ = {};
        frameIndex_ = 0;
        canBlit_ = false;
    }

    std::mutex mutex_;
    OHNativeWindow* window_ = nullptr;
    uint64_t surfaceKey_ = 0;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    std::vector<bool> targetInitialized_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<Frame> frames_;
    size_t frameIndex_ = 0;
    VkExtent2D extent_{};
    VkFormat sourceFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat targetFormat_ = VK_FORMAT_UNDEFINED;
    uint32_t sourceWidth_ = 0;
    uint32_t sourceHeight_ = 0;
    bool canBlit_ = false;
    bool swapchainDirty_ = false;
    uint64_t displayPeriodNs_ = kDefaultFramePeriodNs;
    uint64_t framePeriodNs_ = kDefaultFramePeriodNs;
    uint64_t lastPresentNs_ = 0;
    uint64_t framesPresented_ = 0;
    uint32_t lastSerial_ = 0;
    uint64_t serialRegressions_ = 0;
    uint64_t failures_ = 0;
    uint64_t throttled_ = 0;
    uint64_t firstPresentedNs_ = 0;
    uint64_t totalPresentUs_ = 0;
    uint64_t maxPresentUs_ = 0;
    uint64_t totalWaitFenceUs_ = 0;
    uint64_t totalAcquireUs_ = 0;
    uint64_t totalSubmitUs_ = 0;
    uint64_t totalQueuePresentUs_ = 0;
    uint64_t totalReleaseWaitUs_ = 0;
};

VenusSurfaceQueueTarget::VenusSurfaceQueueTarget()
    : impl_(std::make_unique<Impl>())
{
}

VenusSurfaceQueueTarget::~VenusSurfaceQueueTarget() = default;

int VenusSurfaceQueueTarget::Attach(uint64_t surfaceKey, uint64_t framePeriodNs,
                                    OHNativeWindow* window)
{
    return impl_->Attach(surfaceKey, framePeriodNs, window);
}

int VenusSurfaceQueueTarget::Detach(uint64_t surfaceKey)
{
    return impl_->Detach(surfaceKey);
}

int VenusSurfaceQueueTarget::SetFramePeriod(uint64_t framePeriodNs)
{
    return impl_->SetFramePeriod(framePeriodNs);
}

int VenusSurfaceQueueTarget::Present(uint32_t contextId,
                                     uintptr_t instance,
                                     uintptr_t physicalDevice,
                                     uintptr_t device,
                                     uintptr_t queue,
                                     uint64_t image,
                                     uint32_t queueFamily,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t format,
                                     uint32_t layout,
                                     uint32_t serial,
                                     uint64_t* nextPresentDeadlineNs)
{
    return impl_->Present(contextId, instance, physicalDevice, device, queue,
                          image, queueFamily, width, height, format, layout,
                          serial, nextPresentDeadlineNs);
}

} // namespace winehua
