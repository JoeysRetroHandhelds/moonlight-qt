#pragma once

#include "ffmpeg-renderers/pacer/vrr/cadencedetection.h"
#include "ffmpeg-renderers/pacer/vrr/inputpercentilebuffer.h"
#include <algorithm>
#include <cstdio>
#include <string>

// Presentation only: these values never become controller feedback.
namespace PerformanceStats {
struct Snapshot {
    int width = 0, height = 0;
    const char* codec = "UNKNOWN";
    double receivedFps = 0, decodedFps = 0, renderedFps = 0;
    unsigned rttMs = 0;
    double decodeMs = 0, queueMs = 0;
    double networkDropPercent = 0, clientDropPercent = 0;
    bool vrr = false, adaptive = false;
    uint64_t bufferUs = 0, bufferCapUs = 0;
    uint64_t targetPpm = 0, historyUs = 0, toleranceUs = 0;
    uint64_t nowUs = 0;
    Vrr13::InputPercentileBuffer::Stats input;
    Vrr13::CadenceDetection::Stats display;
};

inline std::string format(const Snapshot& s)
{
    char text[2048], network[24];
    if (s.rttMs) std::snprintf(network, sizeof(network), "%u ms", s.rttMs);
    else std::snprintf(network, sizeof(network), "N/A");
    std::snprintf(text, sizeof(text),
        "%dx%d | %s\n"
        "FPS: in %.1f | decode %.1f | render %.1f\n"
        "Latency: network %s | decode %.2f ms | queue %.2f ms\n"
        "Drops: network %.2f%% | client %.2f%%\n",
        s.width, s.height, s.codec,
        s.receivedFps, s.decodedFps, s.renderedFps,
        network, s.decodeMs, s.queueMs,
        s.networkDropPercent, s.clientDropPercent);
    std::string result(text);
    if (!s.vrr) return result;

    char learned[48], coverage[160], display[192];
    if (s.adaptive && s.input.samples)
        std::snprintf(learned, sizeof(learned), "%.2f ms%s", s.input.targetUs / 1000.0,
                      s.input.tailUncertain ? " (learning)" : "");
    else std::snprintf(learned, sizeof(learned), "N/A");
    // Coverage includes clean inputs plus absorbed inputs; absorbed alone is
    // only the subset that needed protection. Outcomes use each frame's
    // applied buffer, tolerance and VRR headroom, within the selected window.
    if (s.adaptive && s.input.recentEligible) {
        const auto covered = s.input.recentEligible - std::min(s.input.recentMissed, s.input.recentEligible);
        std::snprintf(coverage, sizeof(coverage), "%.2f%% (%llu/%llu frames)",
            100.0 * double(covered) / double(s.input.recentEligible),
            static_cast<unsigned long long>(covered),
            static_cast<unsigned long long>(s.input.recentEligible));
    }
    else std::snprintf(coverage, sizeof(coverage), "N/A");
    const bool freshDisplay = s.display.lastObservedUs && s.nowUs >= s.display.lastObservedUs &&
        s.nowUs - s.display.lastObservedUs <= 1000000;
    if (s.display.intervals && freshDisplay)
        std::snprintf(display, sizeof(display), "%llu/%llu (%.2f%%) | uncertain %llu",
            static_cast<unsigned long long>(s.display.deviations),
            static_cast<unsigned long long>(s.display.intervals),
            100.0 * double(s.display.deviations) / double(s.display.intervals),
            static_cast<unsigned long long>(s.display.uncertain));
    else std::snprintf(display, sizeof(display), "N/A (no recent matched feedback)");
    std::snprintf(text, sizeof(text),
        "Buffer: %.2f ms / %.2f ms cap | learned %s\n"
        "Target: %.2f%% | history %llus | tolerance %.2f ms\n"
        "Buffer coverage (%llus): %s\n"
        "DISPLAY >%.2f ms (2s matched): %s\n",
        s.bufferUs / 1000.0, s.bufferCapUs / 1000.0, learned,
        s.targetPpm / 10000.0, static_cast<unsigned long long>(s.historyUs / 1000000),
        s.toleranceUs / 1000.0,
        static_cast<unsigned long long>(s.input.windowUs / 1000000), coverage,
        s.toleranceUs / 1000.0, display);
    return result + text;
}
}
