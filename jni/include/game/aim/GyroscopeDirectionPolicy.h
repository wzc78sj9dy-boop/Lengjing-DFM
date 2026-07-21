#pragma once

#include <algorithm>
#include <cmath>

namespace lengjing::game::aim {

struct GyroscopeDirection {
    float pitch = 0.0f;
    float yaw = 0.0f;
};

struct KernelGyroscopeCommand {
    float x = 0.0f;
    float y = 0.0f;
};

constexpr int NormalizeGyroscopeOrientation(int orientation) noexcept {
    return ((orientation % 4) + 4) % 4;
}

constexpr GyroscopeDirection ResolveGyroscopeDirection(
    int orientation,
    float pitch,
    float yaw) noexcept {
    switch (NormalizeGyroscopeOrientation(orientation)) {
        case 0:
            pitch = -pitch;
            break;
        case 1:
            break;
        case 2:
            yaw = -yaw;
            break;
        default:
            pitch = -pitch;
            yaw = -yaw;
            break;
    }
    return GyroscopeDirection{pitch, yaw};
}

inline GyroscopeDirection ResolveGyroscopeScreenMotion(
    float offsetX,
    float offsetY,
    float speed,
    float smoothing,
    int orientation) noexcept {
    if (!std::isfinite(offsetX) || !std::isfinite(offsetY) ||
        !std::isfinite(speed) || !std::isfinite(smoothing)) {
        return {};
    }

    float damp = 1.0f;
    if (smoothing > 0.0f) {
        damp = std::max(1.0f / (smoothing + 1.0f), 0.05f);
    }
    const float scale = speed * 0.004f * damp;
    const float sendX = std::clamp(offsetX * scale, -8.0f, 8.0f);
    const float sendY = std::clamp(offsetY * scale, -8.0f, 8.0f);
    float pitch = -sendY;
    float yaw = sendX;
    const int normalized = NormalizeGyroscopeOrientation(orientation);
    if (normalized == 0) pitch = -pitch;
    if (normalized == 2) yaw = -yaw;
    return GyroscopeDirection{pitch, yaw};
}

constexpr KernelGyroscopeCommand ResolveKernelGyroscopeCommand(
    const GyroscopeDirection& direction) noexcept {
    return KernelGyroscopeCommand{-direction.yaw, -direction.pitch};
}

}  // namespace lengjing::game::aim
