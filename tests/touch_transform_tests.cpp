#include "test_support.h"

#include "Android_touch/TouchTransform.h"

#include <array>
#include <cmath>

namespace {

bool Near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

void RequirePoint(
    const lengjing::touch::NormalizedPoint& actual,
    float expectedX,
    float expectedY) {
    REQUIRE(Near(actual.x, expectedX));
    REQUIRE(Near(actual.y, expectedY));
}

}  // namespace

void RunTouchTransformTests() {
    using lengjing::touch::DisplayToNatural;
    using lengjing::touch::NaturalToDisplay;
    using lengjing::touch::NormalizedPoint;

    const NormalizedPoint display{0.2f, 0.3f};
    RequirePoint(DisplayToNatural(display, 0), 0.2f, 0.3f);
    RequirePoint(DisplayToNatural(display, 1), 0.7f, 0.2f);
    RequirePoint(DisplayToNatural(display, 2), 0.8f, 0.7f);
    RequirePoint(DisplayToNatural(display, 3), 0.3f, 0.8f);

    constexpr std::array<NormalizedPoint, 5> points{{
        {0.0f, 0.0f},
        {0.2f, 0.3f},
        {0.5f, 0.5f},
        {0.75f, 0.1f},
        {1.0f, 1.0f},
    }};
    for (int orientation = 0; orientation < 4; ++orientation) {
        for (const NormalizedPoint point : points) {
            const NormalizedPoint natural =
                DisplayToNatural(point, orientation);
            const NormalizedPoint roundTrip =
                NaturalToDisplay(natural, orientation);
            RequirePoint(roundTrip, point.x, point.y);
        }
    }

    REQUIRE(lengjing::touch::NormalizeOrientation(-1) == 3);
    REQUIRE(lengjing::touch::NormalizeOrientation(5) == 1);
}
