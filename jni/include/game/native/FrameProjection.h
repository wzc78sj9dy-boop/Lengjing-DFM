#pragma once

#include <cmath>

namespace lengjing::game::native {

struct ProjectionPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ProjectionRotation {
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
};

struct ProjectionView {
    ProjectionPoint location{};
    ProjectionRotation rotation{};
    float fieldOfView = 0.0f;
};

struct CameraSpacePoint {
    float side = 0.0f;
    float vertical = 0.0f;
    float forward = 0.0f;
};

struct ScreenProjection {
    float x = 0.0f;
    float y = 0.0f;
    CameraSpacePoint camera{};
    bool valid = false;
};

inline bool IsFinite(const ProjectionPoint& point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y) &&
        std::isfinite(point.z);
}

inline bool IsFinite(const ProjectionView& view) noexcept {
    return IsFinite(view.location) &&
        std::isfinite(view.rotation.pitch) &&
        std::isfinite(view.rotation.yaw) &&
        std::isfinite(view.rotation.roll) &&
        std::isfinite(view.fieldOfView);
}

inline CameraSpacePoint ToCameraSpace(
    const ProjectionPoint& world,
    const ProjectionView& view) noexcept {
    constexpr float kDegreesToRadians =
        3.14159265358979323846f / 180.0f;
    const float pitch = view.rotation.pitch * kDegreesToRadians;
    const float yaw = view.rotation.yaw * kDegreesToRadians;
    const float roll = view.rotation.roll * kDegreesToRadians;
    const float sinPitch = std::sin(pitch);
    const float cosPitch = std::cos(pitch);
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const float sinRoll = std::sin(roll);
    const float cosRoll = std::cos(roll);

    const ProjectionPoint forward{
        cosPitch * cosYaw,
        cosPitch * sinYaw,
        sinPitch,
    };
    const ProjectionPoint right{
        sinRoll * sinPitch * cosYaw - cosRoll * sinYaw,
        sinRoll * sinPitch * sinYaw + cosRoll * cosYaw,
        -sinRoll * cosPitch,
    };
    const ProjectionPoint up{
        -(cosRoll * sinPitch * cosYaw + sinRoll * sinYaw),
        cosYaw * sinRoll - cosRoll * sinPitch * sinYaw,
        cosRoll * cosPitch,
    };
    const ProjectionPoint delta{
        world.x - view.location.x,
        world.y - view.location.y,
        world.z - view.location.z,
    };
    const auto dot = [](const ProjectionPoint& left,
                        const ProjectionPoint& rightPoint) {
        return left.x * rightPoint.x + left.y * rightPoint.y +
            left.z * rightPoint.z;
    };
    return CameraSpacePoint{
        dot(delta, right),
        dot(delta, up),
        dot(delta, forward),
    };
}

inline ScreenProjection ProjectWorldPoint(
    const ProjectionPoint& world,
    const ProjectionView& view,
    int screenWidth,
    int screenHeight) noexcept {
    ScreenProjection result{};
    if (!IsFinite(world) || !IsFinite(view) ||
        screenWidth <= 1 || screenHeight <= 1) {
        return result;
    }
    result.camera = ToCameraSpace(world, view);
    if (!std::isfinite(result.camera.side) ||
        !std::isfinite(result.camera.vertical) ||
        !std::isfinite(result.camera.forward) ||
        result.camera.forward <= 0.01f) {
        return result;
    }

    constexpr float kHalfDegreesToRadians =
        3.14159265358979323846f / 360.0f;
    const float tangent = std::tan(view.fieldOfView * kHalfDegreesToRadians);
    if (!std::isfinite(tangent) || tangent <= 0.001f) return result;
    const float halfWidth = static_cast<float>(screenWidth) * 0.5f;
    const float halfHeight = static_cast<float>(screenHeight) * 0.5f;
    const float scale = halfWidth / tangent;
    result.x = halfWidth + result.camera.side * scale / result.camera.forward;
    result.y = halfHeight - result.camera.vertical * scale / result.camera.forward;
    result.valid = std::isfinite(result.x) && std::isfinite(result.y);
    return result;
}

}  // namespace lengjing::game::native
