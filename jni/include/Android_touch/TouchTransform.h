#pragma once

namespace lengjing::touch {

struct NormalizedPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct PixelPoint {
    double x = 0.0;
    double y = 0.0;
    int maximumX = 0;
    int maximumY = 0;
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

constexpr PixelPoint DisplayToNaturalPixels(
    double x,
    double y,
    int width,
    int height,
    int orientation) {
    const int maximumWidth = width > 0 ? width : 0;
    const int maximumHeight = height > 0 ? height : 0;
    switch (NormalizeOrientation(orientation)) {
        case 1:
            return {
                static_cast<double>(maximumHeight) - y,
                x,
                maximumHeight,
                maximumWidth,
            };
        case 2:
            return {
                static_cast<double>(maximumWidth) - x,
                static_cast<double>(maximumHeight) - y,
                maximumWidth,
                maximumHeight,
            };
        case 3:
            return {
                y,
                static_cast<double>(maximumWidth) - x,
                maximumHeight,
                maximumWidth,
            };
        default:
            return {x, y, maximumWidth, maximumHeight};
    }
}

}  // namespace lengjing::touch
