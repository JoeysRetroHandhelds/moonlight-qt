#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Vrr13 {

// Correct only source-gap error beyond T, by at most another T. Retain the
// prior phase on clean gaps so their intervals stay unchanged, and bound the
// entire retained phase to T so consecutive corrections cannot accumulate.
inline int64_t sourceGapSmoothingUs(uint64_t gapUs, uint64_t referenceUs,
                                  uint64_t toleranceUs, int64_t previousUs)
{
    const auto limit = static_cast<int64_t>(std::min<uint64_t>(
        toleranceUs, uint64_t(std::numeric_limits<int64_t>::max() / 2)));
    previousUs = std::clamp(previousUs, -limit, limit);
    const auto error = gapUs > referenceUs ? gapUs - referenceUs : referenceUs - gapUs;
    const auto excess = static_cast<int64_t>(std::min<uint64_t>(
        uint64_t(limit), error > toleranceUs ? error - toleranceUs : 0));
    return std::clamp(previousUs + (gapUs > referenceUs ? -excess : excess), -limit, limit);
}

// Revision 5. Correct the whole source-gap error beyond T, so the presented
// interval lands within T of the reference. Clean gaps keep the retained
// phase, except that a share of it returns toward zero using only the
// tolerance this interval has left; the presented interval therefore never
// moves beyond T to recentre. The retained phase is bounded by limitUs.
inline int64_t sourceGapPhaseUs(uint64_t gapUs, uint64_t referenceUs,
                                uint64_t toleranceUs, uint64_t limitUs,
                                uint64_t recenterPerMille, int64_t previousUs)
{
    static constexpr uint64_t kMaximumUs = uint64_t(std::numeric_limits<int64_t>::max() / 4);
    const auto bounded = [](uint64_t value) {
        return static_cast<int64_t>(std::min<uint64_t>(value, kMaximumUs));
    };
    const int64_t tolerance = bounded(toleranceUs);
    const int64_t limit = bounded(limitUs);
    previousUs = std::clamp(previousUs, -limit, limit);
    const int64_t error = bounded(gapUs) - bounded(referenceUs);
    const int64_t excess = error > tolerance ? error - tolerance :
        error < -tolerance ? error + tolerance : 0;
    int64_t phase = previousUs - excess;
    const int64_t presentedError = error - excess;
    if (phase != 0 && recenterPerMille != 0) {
        const int64_t magnitude = phase > 0 ? phase : -phase;
        // Lowering a positive phase shortens this interval; raising a negative
        // phase lengthens it.
        const int64_t room = phase > 0 ? tolerance + presentedError : tolerance - presentedError;
        const int64_t share = (magnitude * static_cast<int64_t>(std::min<uint64_t>(recenterPerMille, 1000)) + 999) / 1000;
        const int64_t step = std::min({magnitude, std::max<int64_t>(room, 0), share});
        phase += phase > 0 ? -step : step;
    }
    return std::clamp(phase, -limit, limit);
}

} // namespace Vrr13
