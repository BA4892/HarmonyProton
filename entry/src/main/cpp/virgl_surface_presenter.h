#pragma once

#include "virgl_ipc_protocol.h"

#include <native_window/external_window.h>

#include <cstdint>

namespace winehua {

int AttachVirglSurfaceTarget(uint64_t surfaceKey, uint64_t framePeriodNs,
                             OHNativeWindow* window);
int DetachVirglSurfaceTarget(uint64_t surfaceKey);
int SetVirglSurfaceFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs);
int PresentVirglSurface(uint32_t clientPid, uint32_t surfaceId,
                        uint32_t texture, uint32_t width, uint32_t height,
                        uint64_t drawable, uint32_t serial,
                        uint64_t* nextPresentDeadlineNs);
virgl_ipc::SurfaceQueryReply QueryVirglSurfaces();
void ResetVirglSurfaces();

} // namespace winehua
