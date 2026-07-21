#pragma once

#include <cmath>
#include <cstdint>

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

struct ProjectionFovStabilityState {
    std::uintptr_t world = 0;
    std::uintptr_t cameraManager = 0;
    std::uint64_t pendingFrame = 0;
    float acceptedFov = 0.0f;
    float pendingFov = 0.0f;
    bool acceptedValid = false;
    bool pendingValid = false;
};

constexpr bool IsProjectionViewCacheCompatible(
    bool cacheValid,
    std::uintptr_t cachedWorld,
    std::uintptr_t cachedCameraManager,
    std::uintptr_t currentWorld,
    std::uintptr_t currentCameraManager) noexcept {
    return cacheValid && currentWorld != 0 && currentCameraManager != 0 &&
        cachedWorld == currentWorld &&
        cachedCameraManager == currentCameraManager;
}

inline bool ResolveStableProjectionFov(
    std::uintptr_t world,
    std::uintptr_t cameraManager,
    std::uint64_t frameSequence,
    bool firstValid,
    float firstFov,
    bool secondValid,
    float secondFov,
    ProjectionFovStabilityState& state,
    float& resolvedFov) noexcept {
    resolvedFov = 0.0f;
    if (world == 0 || cameraManager == 0) return false;

    if (state.world != world || state.cameraManager != cameraManager) {
        state = ProjectionFovStabilityState{};
        state.world = world;
        state.cameraManager = cameraManager;
    }

    firstValid = firstValid && std::isfinite(firstFov);
    secondValid = secondValid && std::isfinite(secondFov);
    if (!firstValid && !secondValid) {
        if (!state.acceptedValid) return false;
        resolvedFov = state.acceptedFov;
        return true;
    }

    float candidate = secondValid ? secondFov : firstFov;
    const bool inconsistentDoubleRead = firstValid && secondValid &&
        std::fabs(firstFov - secondFov) >= 0.5f;
    if (inconsistentDoubleRead) {
        candidate = state.acceptedValid ? state.acceptedFov : firstFov;
        state.pendingValid = false;
    } else if (state.acceptedValid &&
               std::fabs(candidate - state.acceptedFov) >= 5.5f) {
        const bool confirmedOnLaterFrame = state.pendingValid &&
            frameSequence != state.pendingFrame &&
            std::fabs(candidate - state.pendingFov) < 0.5f;
        if (confirmedOnLaterFrame) {
            state.pendingValid = false;
        } else {
            state.pendingFov = candidate;
            state.pendingFrame = frameSequence;
            state.pendingValid = true;
            candidate = state.acceptedFov;
        }
    } else {
        state.pendingValid = false;
    }

    state.acceptedFov = candidate;
    state.acceptedValid = true;
    resolvedFov = candidate;
    return true;
}

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
