#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace lengjing::game::native {

struct GeometryPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class GeometryBodyType : std::uint8_t {
    Static,
    Dynamic,
};

enum class GeometryVisibility : std::uint8_t {
    Unavailable,
    Visible,
    Occluded,
};

struct GeometryMesh {
    GeometryBodyType bodyType = GeometryBodyType::Static;
    std::uintptr_t actorAddress = 0;
    std::uintptr_t shapeAddress = 0;
    GeometryPoint center{};
    float boundsRadius = 0.0f;
    std::vector<GeometryPoint> vertices;
    std::vector<std::uint32_t> indices;
};

struct GeometrySnapshot {
    bool available = false;
    std::uintptr_t instanceAddress = 0;
    std::uint64_t generation = 0;
    std::uint64_t refreshEpoch = 0;
    std::size_t triangleCount = 0;
    std::vector<std::shared_ptr<const GeometryMesh>> staticMeshes;
    std::vector<std::shared_ptr<const GeometryMesh>> dynamicMeshes;
};

struct GeometryRaycastHit {
    std::shared_ptr<const GeometryMesh> mesh;
    GeometryPoint position{};
    float distance = 0.0f;
    std::uint32_t triangleIndex = 0xFFFFFFFFU;

    explicit operator bool() const noexcept {
        return mesh != nullptr;
    }
};

struct GeometryRuntimeConfig {
    // A validated instance can be supplied directly. Otherwise, each slot is
    // read as a pointer and the first instance with a valid scene array is used.
    std::uintptr_t instanceAddress = 0;
    std::vector<std::uintptr_t> instancePointerSlots;

    std::chrono::milliseconds dynamicRefresh{750};
    std::chrono::milliseconds staticRefresh{10000};
    std::chrono::milliseconds lastGoodTtl{5000};
    std::size_t maxConsecutiveFailures = 4;

    std::size_t maxActors = 131072;
    std::size_t maxShapes = 131072;
    std::size_t maxMeshes = 12000;
    std::size_t maxVerticesPerMesh = 1000000;
    std::size_t maxTrianglesPerMesh = 1000000;
    std::size_t maxTotalVertices = 4000000;
    std::size_t maxTotalTriangles = 4000000;
};

class GeometryRuntime final {
public:
    using ReadCallback =
        std::function<bool(std::uintptr_t, void*, std::size_t)>;

    GeometryRuntime();
    ~GeometryRuntime();

    GeometryRuntime(const GeometryRuntime&) = delete;
    GeometryRuntime& operator=(const GeometryRuntime&) = delete;
    GeometryRuntime(GeometryRuntime&&) = delete;
    GeometryRuntime& operator=(GeometryRuntime&&) = delete;

    bool Start(ReadCallback read, GeometryRuntimeConfig config);
    void Stop() noexcept;
    bool IsRunning() const noexcept;
    std::uint64_t RequestRefresh() noexcept;

    std::shared_ptr<const GeometrySnapshot> GetSnapshot() const noexcept;
    GeometryVisibility Trace(const GeometryPoint& origin,
                             const GeometryPoint& target) const noexcept;
    GeometryRaycastHit Raycast(const GeometryPoint& origin,
                               const GeometryPoint& target) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
