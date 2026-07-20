#pragma once

#include <cmath>

namespace lengjing::game::aim {

inline bool TargetInsideAimRange(float screenDistancePixels,
                                 float rangePixels) {
    return std::isfinite(screenDistancePixels) &&
        std::isfinite(rangePixels) &&
        screenDistancePixels >= 0.0f && rangePixels >= 0.0f &&
        screenDistancePixels <= rangePixels;
}

inline bool ShouldDrawTargetRay(bool enabled,
                                bool targetAvailable,
                                bool drawRange,
                                float screenDistancePixels,
                                float rangePixels) {
    if (!enabled || !targetAvailable ||
        !std::isfinite(screenDistancePixels) ||
        screenDistancePixels < 0.0f) {
        return false;
    }
    return !drawRange || TargetInsideAimRange(
        screenDistancePixels, rangePixels);
}

}  // namespace lengjing::game::aim
