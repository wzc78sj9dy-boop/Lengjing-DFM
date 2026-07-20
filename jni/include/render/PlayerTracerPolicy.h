#pragma once

#include "render/render_types.h"

#include <algorithm>
#include <cmath>

namespace lengjing::render {

inline ImVec2 TopTracerOrigin(float screenWidth,
                              float topMargin = 10.0f) noexcept {
    const float width = std::isfinite(screenWidth)
        ? std::max(0.0f, screenWidth)
        : 0.0f;
    const float margin = std::isfinite(topMargin)
        ? std::max(0.0f, topMargin)
        : 0.0f;
    return ImVec2(width * 0.5f, margin);
}

inline ImVec2 TopTracerTarget(const ScreenRect& bounds) noexcept {
    return ImVec2((bounds.left + bounds.right) * 0.5f, bounds.top);
}

}  // namespace lengjing::render
