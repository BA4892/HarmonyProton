#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace winehua {

using PerfClock = std::chrono::steady_clock;

uint64_t PerfNowUs();

struct RendererPerfWindow {
    static constexpr size_t kSamples = 120;

    std::array<uint64_t, kSamples> takeUs{};
    std::array<uint64_t, kSamples> uploadUs{};
    std::array<uint64_t, kSamples> swapUs{};
    std::array<uint64_t, kSamples> totalUs{};
    size_t count = 0;
    uint64_t displayed = 0;
    uint64_t windowDisplayed = 0;
    uint64_t failedSwaps = 0;
    uint64_t uploadBytes = 0;
    uint64_t startedUs = PerfNowUs();
    uint64_t publishStartedUs = startedUs;
    uint64_t publishFrames = 0;
    uint64_t publishSequence = 0;

    void PublishDisplayedFps(uint32_t toplevelId, uint64_t nowUs);

    static uint64_t Percentile(std::array<uint64_t, kSamples> values, size_t count,
                               unsigned int percentile);

    void Add(uint32_t toplevelId, uint64_t take, uint64_t upload, uint64_t swap,
             uint64_t total, size_t bytes, bool swapOk);
};

} // namespace winehua
