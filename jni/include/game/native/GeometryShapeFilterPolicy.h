#pragma once

#include "game/native/GeometryRuntime.h"

#include <cstdint>

namespace lengjing::game::native {

constexpr std::int32_t kSphereShapeGeometry = 0;
constexpr std::int32_t kTriangleMeshShapeGeometry = 5;
constexpr std::int32_t kHeightFieldShapeGeometry = 6;
constexpr std::uint8_t kSimulationShape = 0x01U;
constexpr std::uint8_t kTriggerShape = 0x04U;
constexpr float kOcclusionHeightFieldRowScale = 200.0f;

constexpr bool ShouldIncludeGeometryShape(
    GeometryBodyType bodyType,
    std::int32_t geometryType,
    std::uint8_t shapeFlags,
    std::uint16_t materialIndex,
    float heightFieldRowScale) noexcept {
    if (bodyType == GeometryBodyType::Static) {
        if (geometryType == kTriangleMeshShapeGeometry) {
            return materialIndex != 0;
        }
        return geometryType == kHeightFieldShapeGeometry &&
            heightFieldRowScale == kOcclusionHeightFieldRowScale;
    }

    if (materialIndex != 0) return false;
    if (geometryType == kTriangleMeshShapeGeometry) return true;
    return geometryType == kSphereShapeGeometry &&
        (shapeFlags & kSimulationShape) != 0 &&
        (shapeFlags & kTriggerShape) == 0;
}

}  // namespace lengjing::game::native
