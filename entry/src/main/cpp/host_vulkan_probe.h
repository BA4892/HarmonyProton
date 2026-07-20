#pragma once

#include <cstdint>
#include <string>

bool StartHostVulkanProbe(uint64_t surfaceId, const std::string& runId);
void StopHostVulkanProbe();
