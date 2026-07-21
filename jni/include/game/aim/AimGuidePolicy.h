#pragma once

#include <cmath>

namespace lengjing::game::aim {

inline bool ShouldDrawAimTargetRay(
    bool requested,
    bool targetAvailable,
    float targetScreenDistancePixels,
    float rangePixels) noexcept {
    return requested && targetAvailable &&
        std::isfinite(targetScreenDistancePixels) &&
        std::isfinite(rangePixels) &&
        targetScreenDistancePixels >= 0.0f &&
        rangePixels >= 0.0f &&
        targetScreenDistancePixels <= rangePixels;
}

}  // namespace lengjing::game::aim
