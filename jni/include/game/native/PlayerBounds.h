#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace lengjing::game::native {

inline constexpr std::size_t kPlayerBoundsBoneCount = 15;

struct PlayerBoneScreenPoint {
    float x = 0.0f;
    float y = 0.0f;
    bool valid = false;
};

struct PlayerScreenBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

inline bool DoesPlayerScreenBoundsIntersectViewport(
    const PlayerScreenBounds& bounds,
    float screenWidth,
    float screenHeight,
    float margin = 0.0f) noexcept {
    if (!std::isfinite(screenWidth) || !std::isfinite(screenHeight) ||
        !std::isfinite(margin) || screenWidth <= 1.0f ||
        screenHeight <= 1.0f || margin < 0.0f ||
        !std::isfinite(bounds.left) || !std::isfinite(bounds.top) ||
        !std::isfinite(bounds.right) || !std::isfinite(bounds.bottom) ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return false;
    }

    return bounds.right >= -margin &&
        bounds.left <= screenWidth + margin &&
        bounds.bottom >= -margin &&
        bounds.top <= screenHeight + margin;
}

inline bool CalculatePlayerAnchorBounds(
    const PlayerBoneScreenPoint& lowerAnchor,
    const PlayerBoneScreenPoint& upperAnchor,
    PlayerScreenBounds& bounds) noexcept {
    bounds = PlayerScreenBounds{};
    if (!lowerAnchor.valid || !upperAnchor.valid ||
        !std::isfinite(lowerAnchor.x) || !std::isfinite(lowerAnchor.y) ||
        !std::isfinite(upperAnchor.x) || !std::isfinite(upperAnchor.y)) {
        return false;
    }

    const float projectedSpan = lowerAnchor.y - upperAnchor.y;
    if (!std::isfinite(projectedSpan) || projectedSpan < 8.0f) {
        return false;
    }

    const float halfWidth = projectedSpan * 0.25f;
    const float minimumAnchorX = std::min(lowerAnchor.x, upperAnchor.x);
    const float maximumAnchorX = std::max(lowerAnchor.x, upperAnchor.x);
    bounds = PlayerScreenBounds{
        minimumAnchorX - halfWidth,
        upperAnchor.y,
        maximumAnchorX + halfWidth,
        lowerAnchor.y,
    };
    return std::isfinite(bounds.left) && std::isfinite(bounds.top) &&
        std::isfinite(bounds.right) && std::isfinite(bounds.bottom) &&
        bounds.right > bounds.left && bounds.bottom > bounds.top;
}

inline bool CalculatePlayerScreenBounds(
    const std::array<PlayerBoneScreenPoint, kPlayerBoundsBoneCount>& bones,
    PlayerScreenBounds& bounds) noexcept {
    bounds = PlayerScreenBounds{};
    constexpr std::size_t kPelvis = 2;
    constexpr std::array<std::size_t, 8> kUpperBody{
        0, 1, 3, 4, 5, 6, 7, 8};
    constexpr std::array<std::size_t, 6> kLowerBody{
        9, 10, 11, 12, 13, 14};

    const auto usable = [&bones](std::size_t index) {
        return bones[index].valid && std::isfinite(bones[index].x) &&
            std::isfinite(bones[index].y);
    };
    if (!usable(kPelvis)) return false;

    const auto groupReady = [&usable](const auto& indices) {
        return std::any_of(
            indices.begin(), indices.end(),
            [&usable](std::size_t index) { return usable(index); });
    };
    std::size_t usableCount = 0;
    for (std::size_t index = 0; index < bones.size(); ++index) {
        if (usable(index)) ++usableCount;
    }
    if (usableCount < 5 || !groupReady(kUpperBody) ||
        !groupReady(kLowerBody)) {
        return false;
    }

    const PlayerBoneScreenPoint pelvis = bones[kPelvis];
    std::array<float, kPlayerBoundsBoneCount - 1> pelvisDistances{};
    std::size_t distanceCount = 0;
    for (std::size_t index = 0; index < bones.size(); ++index) {
        if (index == kPelvis || !usable(index)) continue;
        const float distance = std::hypot(
            bones[index].x - pelvis.x,
            bones[index].y - pelvis.y);
        if (!std::isfinite(distance)) continue;
        pelvisDistances[distanceCount++] = distance;
    }
    if (distanceCount == 0) return false;
    std::sort(
        pelvisDistances.begin(),
        pelvisDistances.begin() + static_cast<std::ptrdiff_t>(distanceCount));
    float medianDistance = pelvisDistances[distanceCount / 2];
    if ((distanceCount & 1U) == 0U) {
        medianDistance =
            (pelvisDistances[distanceCount / 2 - 1] + medianDistance) * 0.5f;
    }
    const float maximumPelvisDistance = medianDistance * 3.0f + 8.0f;
    if (!std::isfinite(maximumPelvisDistance)) return false;

    std::array<bool, kPlayerBoundsBoneCount> retained{};
    std::size_t retainedCount = 0;
    for (std::size_t index = 0; index < bones.size(); ++index) {
        if (!usable(index)) continue;
        const float distance = std::hypot(
            bones[index].x - pelvis.x,
            bones[index].y - pelvis.y);
        if (!std::isfinite(distance) || distance > maximumPelvisDistance) {
            continue;
        }
        retained[index] = true;
        ++retainedCount;
    }
    const auto retainedGroupReady = [&retained](const auto& indices) {
        return std::any_of(
            indices.begin(), indices.end(),
            [&retained](std::size_t index) { return retained[index]; });
    };
    if (retainedCount < 5 || !retained[kPelvis] ||
        !retainedGroupReady(kUpperBody) ||
        !retainedGroupReady(kLowerBody)) {
        return false;
    }

    float majorSpan = 0.0f;
    for (std::size_t first = 0; first < bones.size(); ++first) {
        if (!retained[first]) continue;
        for (std::size_t second = first + 1;
             second < bones.size();
             ++second) {
            if (!retained[second]) continue;
            majorSpan = std::max(
                majorSpan,
                std::hypot(
                    bones[first].x - bones[second].x,
                    bones[first].y - bones[second].y));
        }
    }
    if (!std::isfinite(majorSpan) || majorSpan < 8.0f) return false;

    float minimumX = std::numeric_limits<float>::infinity();
    float minimumY = std::numeric_limits<float>::infinity();
    float maximumX = -std::numeric_limits<float>::infinity();
    float maximumY = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < bones.size(); ++index) {
        if (!retained[index]) continue;
        minimumX = std::min(minimumX, bones[index].x);
        minimumY = std::min(minimumY, bones[index].y);
        maximumX = std::max(maximumX, bones[index].x);
        maximumY = std::max(maximumY, bones[index].y);
    }

    const float padding = std::max(3.0f, majorSpan * 0.04f);
    minimumX -= padding;
    minimumY -= padding;
    maximumX += padding;
    maximumY += padding;

    const float minimumShortAxis = majorSpan * 0.16f;
    float width = maximumX - minimumX;
    float height = maximumY - minimumY;
    if (width < minimumShortAxis) {
        const float center = (minimumX + maximumX) * 0.5f;
        minimumX = center - minimumShortAxis * 0.5f;
        maximumX = center + minimumShortAxis * 0.5f;
        width = minimumShortAxis;
    }
    if (height < minimumShortAxis) {
        const float center = (minimumY + maximumY) * 0.5f;
        minimumY = center - minimumShortAxis * 0.5f;
        maximumY = center + minimumShortAxis * 0.5f;
        height = minimumShortAxis;
    }
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width < 4.0f || height < 4.0f) {
        return false;
    }

    bounds = PlayerScreenBounds{
        minimumX,
        minimumY,
        maximumX,
        maximumY,
    };
    return std::isfinite(bounds.left) && std::isfinite(bounds.top) &&
        std::isfinite(bounds.right) && std::isfinite(bounds.bottom) &&
        bounds.right > bounds.left && bounds.bottom > bounds.top;
}

inline bool IsReliablePlayerScreenBounds(
    const PlayerScreenBounds& bounds,
    float screenWidth,
    float screenHeight) noexcept {
    if (!std::isfinite(screenWidth) || !std::isfinite(screenHeight) ||
        screenWidth <= 1.0f || screenHeight <= 1.0f ||
        !std::isfinite(bounds.left) || !std::isfinite(bounds.top) ||
        !std::isfinite(bounds.right) || !std::isfinite(bounds.bottom)) {
        return false;
    }

    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    const float major = std::max(width, height);
    if (!std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(major) || width < 4.0f || height < 4.0f ||
        major > std::max(screenWidth, screenHeight) * 8.0f) {
        return false;
    }

    const float aspectRatio = width / height;
    if (!std::isfinite(aspectRatio) ||
        aspectRatio < 0.05f || aspectRatio > 20.0f) {
        return false;
    }

    return DoesPlayerScreenBoundsIntersectViewport(
        bounds, screenWidth, screenHeight);
}

inline bool SelectPlayerScreenBounds(
    bool boneBoundsReady,
    const PlayerScreenBounds& boneBounds,
    bool anchorBoundsReady,
    const PlayerScreenBounds& anchorBounds,
    float screenWidth,
    float screenHeight,
    PlayerScreenBounds& bounds) noexcept {
    bounds = PlayerScreenBounds{};
    if (boneBoundsReady && IsReliablePlayerScreenBounds(
            boneBounds, screenWidth, screenHeight)) {
        bounds = boneBounds;
        return true;
    }
    if (anchorBoundsReady && IsReliablePlayerScreenBounds(
            anchorBounds, screenWidth, screenHeight)) {
        bounds = anchorBounds;
        return true;
    }
    return false;
}

}  // namespace lengjing::game::native
