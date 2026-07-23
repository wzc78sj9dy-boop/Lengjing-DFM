#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace lengjing::game::native {

enum class GeometrySceneKind : std::uint8_t {
    Static,
    Dynamic,
};

struct GeometrySceneBuildPolicy {
    bool lowBuildQuality = false;
    bool dynamicScene = false;
    bool robust = true;
    bool compact = true;
};

constexpr GeometrySceneBuildPolicy ResolveGeometrySceneBuildPolicy(
    GeometrySceneKind kind) noexcept {
    if (kind == GeometrySceneKind::Dynamic) {
        return GeometrySceneBuildPolicy{true, true, false, true};
    }
    return GeometrySceneBuildPolicy{};
}

constexpr std::chrono::milliseconds kMinimumGeometryPublishInterval{33};

constexpr bool ShouldRequestGeometryRefresh(
    std::uintptr_t previousWorld,
    std::uintptr_t currentWorld) noexcept {
    return previousWorld != 0 && previousWorld != currentWorld;
}

template <typename Clock, typename Duration>
bool ShouldPublishGeometryUpdate(
    bool hasPublished,
    std::chrono::time_point<Clock, Duration> lastPublishedAt,
    std::chrono::time_point<Clock, Duration> now,
    bool criticalChange) noexcept {
    return criticalChange || !hasPublished || now < lastPublishedAt ||
        now - lastPublishedAt >= kMinimumGeometryPublishInterval;
}

template <typename Mesh>
bool CanReuseGeometryScene(
    std::uintptr_t previousInstance,
    const std::vector<std::uintptr_t>& previousScenes,
    const std::vector<std::shared_ptr<const Mesh>>& previousMeshes,
    std::uintptr_t candidateInstance,
    const std::vector<std::uintptr_t>& candidateScenes,
    const std::vector<std::shared_ptr<const Mesh>>& candidateMeshes) noexcept {
    if (previousInstance != candidateInstance ||
        previousScenes != candidateScenes ||
        previousMeshes.size() != candidateMeshes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < previousMeshes.size(); ++index) {
        if (previousMeshes[index].get() != candidateMeshes[index].get()) {
            return false;
        }
    }
    return true;
}

template <typename Mesh, typename Equivalent>
bool ReuseEquivalentGeometryMesh(
    const std::shared_ptr<const Mesh>& previous,
    std::shared_ptr<const Mesh>& candidate,
    Equivalent equivalent) noexcept(noexcept(
        equivalent(*previous, *candidate))) {
    if (!previous || !candidate) {
        return false;
    }
    if (previous.get() == candidate.get()) {
        return true;
    }
    if (!equivalent(*previous, *candidate)) {
        return false;
    }
    candidate = previous;
    return true;
}

}  // namespace lengjing::game::native
