#pragma once

#include <native_window/external_window.h>

#include <cstdint>
#include <memory>

namespace winehua {

class VenusSurfaceQueueTarget {
public:
    VenusSurfaceQueueTarget();
    ~VenusSurfaceQueueTarget();

    VenusSurfaceQueueTarget(const VenusSurfaceQueueTarget&) = delete;
    VenusSurfaceQueueTarget& operator=(const VenusSurfaceQueueTarget&) = delete;

    int Attach(uint64_t surfaceKey, uint64_t framePeriodNs, OHNativeWindow* window);
    int Detach(uint64_t surfaceKey);
    int SetFramePeriod(uint64_t framePeriodNs);
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
                uint64_t* nextPresentDeadlineNs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace winehua
