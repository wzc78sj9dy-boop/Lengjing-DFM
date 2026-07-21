#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace lengjing::render {

inline constexpr std::uint8_t kSolidAlpha = 255;
inline constexpr std::uint8_t kTextMinimumAlpha = 235;
inline constexpr std::uint8_t kTextOutlineMinimumAlpha = 210;

struct TextOutlineDirection {
    float x = 0.0f;
    float y = 0.0f;
};

inline constexpr std::array<TextOutlineDirection, 4>
    kTextOutlineDirections{{
        {-1.0f, 0.0f},
        {1.0f, 0.0f},
        {0.0f, -1.0f},
        {0.0f, 1.0f},
    }};

inline std::uint32_t WithMinimumAlpha(
    std::uint32_t color,
    std::uint8_t minimumAlpha) noexcept {
    const std::uint32_t alpha = color >> 24U;
    return (color & 0x00ffffffU) |
        (std::max(alpha, static_cast<std::uint32_t>(minimumAlpha)) << 24U);
}

inline std::uint32_t WithExactAlpha(
    std::uint32_t color,
    std::uint8_t alpha) noexcept {
    return (color & 0x00ffffffU) |
        (static_cast<std::uint32_t>(alpha) << 24U);
}

inline float TextOutlineOffset(float fontSize) noexcept {
    const float safeFontSize = std::isfinite(fontSize)
        ? std::max(0.0f, fontSize)
        : 0.0f;
    return std::clamp(safeFontSize * 0.045f, 0.75f, 1.5f);
}

inline float PlayerStrokeWidth(float configuredWidth, float scale) noexcept {
    const float safeScale = std::isfinite(scale)
        ? std::clamp(scale, 0.5f, 2.5f)
        : 1.0f;
    const float safeWidth = std::isfinite(configuredWidth)
        ? std::max(0.0f, configuredWidth)
        : 0.0f;
    return std::max(2.0f, safeWidth * safeScale);
}

inline float PlayerOutlineWidth(
    float strokeWidth,
    float configuredOutlineWidth,
    float scale) noexcept {
    const float safeScale = std::isfinite(scale)
        ? std::clamp(scale, 0.5f, 2.5f)
        : 1.0f;
    const float safeStroke = std::isfinite(strokeWidth)
        ? std::max(2.0f, strokeWidth)
        : 2.0f;
    const float safeOutline = std::isfinite(configuredOutlineWidth)
        ? std::max(0.0f, configuredOutlineWidth)
        : 0.0f;
    return std::max(
        safeStroke + std::max(1.0f, safeScale),
        safeOutline * safeScale);
}

}  // namespace lengjing::render
