#ifndef LENGJING_CPU_RASTER_RULES_H
#define LENGJING_CPU_RASTER_RULES_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace lengjing::render::cpu {

struct PixelRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    [[nodiscard]] bool valid() const {
        return x1 > x0 && y1 > y0;
    }
};

inline PixelRect UnionRect(const PixelRect& a, const PixelRect& b) {
    if (!a.valid())
        return b;
    if (!b.valid())
        return a;
    return {
        std::min(a.x0, b.x0),
        std::min(a.y0, b.y0),
        std::max(a.x1, b.x1),
        std::max(a.y1, b.y1),
    };
}

inline PixelRect IntersectRect(const PixelRect& a, const PixelRect& b) {
    return {
        std::max(a.x0, b.x0),
        std::max(a.y0, b.y0),
        std::min(a.x1, b.x1),
        std::min(a.y1, b.y1),
    };
}

inline PixelRect ClampRect(const PixelRect& rect, int width, int height) {
    return {
        std::clamp(rect.x0, 0, width),
        std::clamp(rect.y0, 0, height),
        std::clamp(rect.x1, 0, width),
        std::clamp(rect.y1, 0, height),
    };
}

inline int SaturatingInt(double value) {
    if (value <= static_cast<double>(std::numeric_limits<int>::min()))
        return std::numeric_limits<int>::min();
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(value);
}

// The first pixel whose center is on or after an edge.
inline int FirstPixelAtOrAfter(float edge) {
    return SaturatingInt(std::ceil(static_cast<double>(edge) - 0.5));
}

// The exclusive end for a half-open edge. A center exactly on the edge is out.
inline int EndPixelBefore(float edge) {
    return SaturatingInt(std::ceil(static_cast<double>(edge) - 0.5));
}

// A conservative exclusive end. Coverage testing decides equality.
inline int EndPixelAtOrBefore(float edge) {
    return SaturatingInt(std::floor(static_cast<double>(edge) - 0.5) + 1.0);
}

constexpr int kSubpixelBits = 8;
constexpr int64_t kSubpixelScale = int64_t{1} << kSubpixelBits;
constexpr int64_t kSubpixelHalf = kSubpixelScale / 2;

struct SubpixelPoint {
    int64_t x = 0;
    int64_t y = 0;
};

inline bool ToSubpixel(float x, float y, SubpixelPoint* output) {
    if (output == nullptr || !std::isfinite(x) || !std::isfinite(y))
        return false;
    // Keeps the largest edge product comfortably inside int64_t.
    constexpr double kLimit = 1000000.0;
    if (std::fabs(static_cast<double>(x)) > kLimit
        || std::fabs(static_cast<double>(y)) > kLimit)
        return false;
    output->x = static_cast<int64_t>(std::llround(
        static_cast<double>(x) * kSubpixelScale));
    output->y = static_cast<int64_t>(std::llround(
        static_cast<double>(y) * kSubpixelScale));
    return true;
}

inline int64_t EdgeValue(const SubpixelPoint& a,
                         const SubpixelPoint& b,
                         const SubpixelPoint& p) {
    return (b.x - a.x) * (p.y - a.y)
         - (b.y - a.y) * (p.x - a.x);
}

inline bool IsTopLeftEdge(const SubpixelPoint& a,
                          const SubpixelPoint& b) {
    const int64_t dx = b.x - a.x;
    const int64_t dy = b.y - a.y;
    return dy < 0 || (dy == 0 && dx > 0);
}

inline bool EdgeCovers(int64_t value, bool top_left) {
    return value > 0 || (value == 0 && top_left);
}

inline SubpixelPoint PixelCenter(int x, int y) {
    return {
        static_cast<int64_t>(x) * kSubpixelScale + kSubpixelHalf,
        static_cast<int64_t>(y) * kSubpixelScale + kSubpixelHalf,
    };
}

} // namespace lengjing::render::cpu

#endif
