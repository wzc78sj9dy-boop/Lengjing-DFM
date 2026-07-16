#pragma once

namespace lengjing::touch {

struct NormalizedPoint {
    float x = 0.0f;
    float y = 0.0f;
};

constexpr int NormalizeOrientation(int orientation) {
    return ((orientation % 4) + 4) % 4;
}

constexpr NormalizedPoint NaturalToDisplay(
    NormalizedPoint point,
    int orientation) {
    switch (NormalizeOrientation(orientation)) {
        case 1:
            return {point.y, 1.0f - point.x};
        case 2:
            return {1.0f - point.x, 1.0f - point.y};
        case 3:
            return {1.0f - point.y, point.x};
        default:
            return point;
    }
}

constexpr NormalizedPoint DisplayToNatural(
    NormalizedPoint point,
    int orientation) {
    switch (NormalizeOrientation(orientation)) {
        case 1:
            return {1.0f - point.y, point.x};
        case 2:
            return {1.0f - point.x, 1.0f - point.y};
        case 3:
            return {point.y, 1.0f - point.x};
        default:
            return point;
    }
}

}  // namespace lengjing::touch
