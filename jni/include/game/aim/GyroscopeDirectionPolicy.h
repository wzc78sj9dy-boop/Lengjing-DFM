#pragma once

namespace lengjing::game::aim {

struct GyroscopeDirection {
    float pitch = 0.0f;
    float yaw = 0.0f;
};

constexpr int NormalizeGyroscopeOrientation(int orientation) noexcept {
    return ((orientation % 4) + 4) % 4;
}

constexpr GyroscopeDirection ResolveGyroscopeDirection(
    int orientation,
    float pitch,
    float yaw) noexcept {
    if (NormalizeGyroscopeOrientation(orientation) == 3) {
        pitch = -pitch;
        yaw = -yaw;
    }
    return GyroscopeDirection{pitch, yaw};
}

}  // namespace lengjing::game::aim
