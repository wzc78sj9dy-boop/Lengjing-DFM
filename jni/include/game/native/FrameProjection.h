#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

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

struct PreparedProjection {
    ProjectionPoint location{};
    ProjectionPoint forward{};
    ProjectionPoint right{};
    ProjectionPoint up{};
    float halfWidth = 0.0f;
    float halfHeight = 0.0f;
    float scale = 0.0f;
    bool basisValid = false;
    bool valid = false;
};

struct ScreenProjection {
    float x = 0.0f;
    float y = 0.0f;
    CameraSpacePoint camera{};
    bool valid = false;
};

struct ScreenProjectionSegment {
    ScreenProjection start{};
    ScreenProjection end{};
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

namespace projection_detail {

inline const std::array<ProjectionPoint, 36>&
HorizontalRingUnitPoints() noexcept {
    static const std::array<ProjectionPoint, 36> points = [] {
        constexpr std::size_t kSegmentCount = 36;
        constexpr float kTwoPi = 6.28318530717958647692f;
        std::array<ProjectionPoint, kSegmentCount> result{};
        for (std::size_t index = 0; index < kSegmentCount; ++index) {
            const float angle = kTwoPi * static_cast<float>(index) /
                static_cast<float>(kSegmentCount);
            result[index] = ProjectionPoint{
                std::cos(angle),
                std::sin(angle),
                0.0f,
            };
        }
        return result;
    }();
    return points;
}

inline PreparedProjection PrepareBasis(
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

    PreparedProjection result{};
    result.location = view.location;
    result.forward = ProjectionPoint{
        cosPitch * cosYaw,
        cosPitch * sinYaw,
        sinPitch,
    };
    result.right = ProjectionPoint{
        sinRoll * sinPitch * cosYaw - cosRoll * sinYaw,
        sinRoll * sinPitch * sinYaw + cosRoll * cosYaw,
        -sinRoll * cosPitch,
    };
    result.up = ProjectionPoint{
        -(cosRoll * sinPitch * cosYaw + sinRoll * sinYaw),
        cosYaw * sinRoll - cosRoll * sinPitch * sinYaw,
        cosRoll * cosPitch,
    };
    result.basisValid = true;
    return result;
}

}  // namespace projection_detail

inline PreparedProjection PrepareProjection(
    const ProjectionView& view,
    int screenWidth,
    int screenHeight) noexcept {
    if (!IsFinite(view) || screenWidth <= 1 || screenHeight <= 1) {
        return {};
    }

    PreparedProjection result = projection_detail::PrepareBasis(view);
    constexpr float kHalfDegreesToRadians =
        3.14159265358979323846f / 360.0f;
    const float tangent = std::tan(
        view.fieldOfView * kHalfDegreesToRadians);
    if (!std::isfinite(tangent) || tangent <= 0.001f) {
        return result;
    }

    result.halfWidth = static_cast<float>(screenWidth) * 0.5f;
    result.halfHeight = static_cast<float>(screenHeight) * 0.5f;
    result.scale = result.halfWidth / tangent;
    result.valid = true;
    return result;
}

inline CameraSpacePoint ToCameraSpace(
    const ProjectionPoint& world,
    const PreparedProjection& prepared) noexcept {
    if (!prepared.basisValid) return {};
    const ProjectionPoint delta{
        world.x - prepared.location.x,
        world.y - prepared.location.y,
        world.z - prepared.location.z,
    };
    const auto dot = [](const ProjectionPoint& left,
                        const ProjectionPoint& rightPoint) {
        return left.x * rightPoint.x + left.y * rightPoint.y +
            left.z * rightPoint.z;
    };
    return CameraSpacePoint{
        dot(delta, prepared.right),
        dot(delta, prepared.up),
        dot(delta, prepared.forward),
    };
}

inline CameraSpacePoint ToCameraSpace(
    const ProjectionPoint& world,
    const ProjectionView& view) noexcept {
    return ToCameraSpace(
        world, projection_detail::PrepareBasis(view));
}

inline ScreenProjection ProjectWorldPoint(
    const ProjectionPoint& world,
    const PreparedProjection& prepared) noexcept {
    ScreenProjection result{};
    if (!IsFinite(world) || !prepared.basisValid) return result;

    result.camera = ToCameraSpace(world, prepared);
    if (!std::isfinite(result.camera.side) ||
        !std::isfinite(result.camera.vertical) ||
        !std::isfinite(result.camera.forward) ||
        result.camera.forward <= 0.01f || !prepared.valid) {
        return result;
    }

    result.x = prepared.halfWidth +
        result.camera.side * prepared.scale / result.camera.forward;
    result.y = prepared.halfHeight -
        result.camera.vertical * prepared.scale / result.camera.forward;
    result.valid = std::isfinite(result.x) && std::isfinite(result.y);
    return result;
}

inline ScreenProjection ProjectWorldPoint(
    const ProjectionPoint& world,
    const ProjectionView& view,
    int screenWidth,
    int screenHeight) noexcept {
    if (!IsFinite(world)) return {};
    return ProjectWorldPoint(
        world, PrepareProjection(view, screenWidth, screenHeight));
}

inline std::vector<ScreenProjectionSegment> ProjectWorldHorizontalRing(
    const ProjectionPoint& center,
    float radius,
    const PreparedProjection& prepared) {
    constexpr std::size_t kSegmentCount = 36;
    std::vector<ScreenProjectionSegment> result;
    if (!IsFinite(center) || !prepared.valid ||
        !std::isfinite(radius) || radius <= 0.0f) {
        return result;
    }

    std::array<ScreenProjection, kSegmentCount> points{};
    const auto& unitPoints =
        projection_detail::HorizontalRingUnitPoints();
    for (std::size_t index = 0; index < kSegmentCount; ++index) {
        const ProjectionPoint& unitPoint = unitPoints[index];
        points[index] = ProjectWorldPoint(
            ProjectionPoint{
                center.x + radius * unitPoint.x,
                center.y + radius * unitPoint.y,
                center.z,
            },
            prepared);
    }

    result.reserve(kSegmentCount);
    for (std::size_t index = 0; index < kSegmentCount; ++index) {
        const ScreenProjection& start = points[index];
        const ScreenProjection& end = points[(index + 1) % kSegmentCount];
        if (!start.valid || !end.valid) continue;
        result.push_back(ScreenProjectionSegment{start, end});
    }
    return result;
}

inline std::vector<ScreenProjectionSegment> ProjectWorldHorizontalRing(
    const ProjectionPoint& center,
    float radius,
    const ProjectionView& view,
    int screenWidth,
    int screenHeight) {
    if (!IsFinite(center) || !IsFinite(view) ||
        !std::isfinite(radius) || radius <= 0.0f ||
        screenWidth <= 1 || screenHeight <= 1) {
        return {};
    }
    return ProjectWorldHorizontalRing(
        center,
        radius,
        PrepareProjection(view, screenWidth, screenHeight));
}

}  // namespace lengjing::game::native
