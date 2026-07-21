#pragma once

#include <algorithm>
#include <cmath>

namespace lengjing::game::aim {

struct TouchMotionStep {
    float x = 0.0f;
    float y = 0.0f;
};

inline TouchMotionStep ResolveTouchScreenStep(
    float offsetX,
    float offsetY,
    float speed,
    float screenWidth,
    float screenHeight) noexcept {
    if (!std::isfinite(offsetX) || !std::isfinite(offsetY) ||
        !std::isfinite(speed) || !std::isfinite(screenWidth) ||
        !std::isfinite(screenHeight) || speed <= 0.0f ||
        screenWidth <= 0.0f || screenHeight <= 0.0f) {
        return {};
    }
    TouchMotionStep step{offsetX / speed, offsetY / speed};
    const float halfWidth = screenWidth * 0.5f;
    const float halfHeight = screenHeight * 0.5f;
    if (step.x + halfWidth < 0.0f ||
        step.x + halfWidth > screenWidth) {
        step.x = 0.0f;
    }
    if (step.y + halfHeight < 0.0f ||
        step.y + halfHeight > screenHeight) {
        step.y = 0.0f;
    }
    return step;
}

}  // namespace lengjing::game::aim
