#pragma once

#include "game/native/GeometryRuntime.h"

#include <cstdint>

namespace lengjing::game::native {

constexpr std::int32_t kSphereShapeGeometry = 0;
constexpr std::int32_t kBoxShapeGeometry = 3;
constexpr std::int32_t kConvexMeshShapeGeometry = 4;
constexpr std::int32_t kTriangleMeshShapeGeometry = 5;
constexpr std::int32_t kHeightFieldShapeGeometry = 6;
constexpr std::uint8_t kSimulationShape = 0x01U;
constexpr std::uint8_t kSceneQueryShape = 0x02U;
constexpr std::uint8_t kTriggerShape = 0x04U;

constexpr bool ShouldIncludeGeometryShape(
    GeometryBodyType bodyType,
    std::int32_t geometryType,
    std::uint8_t shapeFlags,
    std::uint16_t materialIndex) noexcept {
    if (geometryType == kTriangleMeshShapeGeometry) {
        return bodyType == GeometryBodyType::Static
            ? materialIndex != 0
            : materialIndex == 0;
    }

    const bool physicalShape =
        (shapeFlags & kTriggerShape) == 0 &&
        (shapeFlags & (kSimulationShape | kSceneQueryShape)) != 0;
    if (!physicalShape) {
        return false;
    }

    if (bodyType == GeometryBodyType::Dynamic) {
        return geometryType == kSphereShapeGeometry && materialIndex == 0 &&
            (shapeFlags & kSimulationShape) != 0;
    }

    return geometryType == kConvexMeshShapeGeometry ||
        geometryType == kBoxShapeGeometry ||
        geometryType == kHeightFieldShapeGeometry;
}

}  // namespace lengjing::game::native
