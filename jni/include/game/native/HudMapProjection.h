#pragma once

#include "game/native/HudMapReader.h"

#include <algorithm>
#include <cmath>

namespace lengjing::game::native {

struct HudMapPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct HudMapProjection {
    HudMapPoint marker{};
    HudMapPoint directionEnd{};
    bool visible = false;
};

inline HudMapProjection ProjectBigMap(
    const HudMapCache& cache,
    float targetX,
    float targetY,
    float headingRadians,
    float screenWidth,
    float screenHeight,
    float offsetX,
    float offsetY,
    float markerSize) {
    HudMapProjection result{};
    if (!cache.bigMapVisible || cache.bigMapWidget == 0 ||
        cache.bigScaleX <= 10.0f || cache.bigScaleY <= 10.0f ||
        screenWidth <= 1.0f || screenHeight <= 1.0f) {
        return result;
    }

    result.marker.x =
        ((targetX + static_cast<float>(cache.bigWorldOffsetX)) -
         cache.bigOriginX) /
        cache.bigScaleX * screenWidth + offsetX;
    result.marker.y =
        ((targetY + static_cast<float>(cache.bigWorldOffsetY)) -
         cache.bigOriginY) /
        cache.bigScaleY * screenHeight + offsetY;
    const float directionLength = std::max(markerSize, 0.0f) + 18.0f;
    result.directionEnd.x =
        result.marker.x + std::cos(headingRadians) * directionLength;
    result.directionEnd.y =
        result.marker.y + std::sin(headingRadians) * directionLength;
    result.visible = std::isfinite(result.marker.x) &&
        std::isfinite(result.marker.y) &&
        std::isfinite(result.directionEnd.x) &&
        std::isfinite(result.directionEnd.y);
    return result;
}

inline HudMapProjection ProjectMiniMap(
    const HudMapCache& cache,
    float selfX,
    float selfY,
    float selfZ,
    float targetX,
    float targetY,
    float targetZ,
    float headingRadians,
    bool rotatedMap,
    float offsetX,
    float offsetY,
    float markerSize) {
    HudMapProjection result{};
    if (cache.miniMapWidget == 0 || !std::isfinite(cache.miniScale) ||
        cache.miniScale <= 0.0f) {
        return result;
    }

    const float deltaX = targetX - selfX;
    const float deltaY = targetY - selfY;
    const float deltaZ = targetZ - selfZ;
    const float distanceMeters =
        std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ) * 0.01f;
    const float rangeMeters = 80.0f / (cache.miniScale * 0.1f);
    if (!std::isfinite(distanceMeters) || !std::isfinite(rangeMeters) ||
        rangeMeters <= 0.0f || distanceMeters >= rangeMeters) {
        return result;
    }

    const float size = cache.miniSize + 123.5f;
    const float half = size * 0.5f;
    const float centerX = cache.miniPositionX + 96.75f + offsetX;
    const float centerY = cache.miniPositionY + 70.0f + offsetY;
    const float normalizedX = deltaX * 0.01f / rangeMeters;
    const float normalizedY = deltaY * 0.01f / rangeMeters;
    float finalHeading = headingRadians;
    if (rotatedMap) {
        result.marker.x = centerX - half * normalizedY;
        result.marker.y = centerY + half * normalizedX;
        finalHeading += 1.57079632679489661923f;
    } else {
        result.marker.x = centerX + half * normalizedX;
        result.marker.y = centerY + half * normalizedY;
    }

    const float directionLength = std::max(markerSize, 0.0f) + 18.0f;
    result.directionEnd.x =
        result.marker.x + std::cos(finalHeading) * directionLength;
    result.directionEnd.y =
        result.marker.y + std::sin(finalHeading) * directionLength;
    result.visible = std::isfinite(result.marker.x) &&
        std::isfinite(result.marker.y) &&
        std::isfinite(result.directionEnd.x) &&
        std::isfinite(result.directionEnd.y);
    return result;
}

}  // namespace lengjing::game::native
