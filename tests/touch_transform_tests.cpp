#include "test_support.h"

#include "Android_touch/TouchTransform.h"

#include <array>
#include <cmath>

namespace {

bool Near(double left, double right) {
    return std::fabs(left - right) < 0.0001;
}

void RequirePoint(
    const lengjing::touch::NormalizedPoint& actual,
    float expectedX,
    float expectedY) {
    REQUIRE(Near(actual.x, expectedX));
    REQUIRE(Near(actual.y, expectedY));
}

void RequirePixelPoint(
    const lengjing::touch::PixelPoint& actual,
    double expectedX,
    double expectedY,
    int expectedMaximumX,
    int expectedMaximumY) {
    REQUIRE(Near(actual.x, expectedX));
    REQUIRE(Near(actual.y, expectedY));
    REQUIRE(actual.maximumX == expectedMaximumX);
    REQUIRE(actual.maximumY == expectedMaximumY);
}

}  // namespace

void RunTouchTransformTests() {
    using lengjing::touch::DisplayToNatural;
    using lengjing::touch::DisplayToNaturalPixels;
    using lengjing::touch::NaturalToDisplay;
    using lengjing::touch::NormalizedPoint;

    const NormalizedPoint display{0.2f, 0.3f};
    RequirePoint(DisplayToNatural(display, 0), 0.2f, 0.3f);
    RequirePoint(DisplayToNatural(display, 1), 0.7f, 0.2f);
    RequirePoint(DisplayToNatural(display, 2), 0.8f, 0.7f);
    RequirePoint(DisplayToNatural(display, 3), 0.3f, 0.8f);

    const NormalizedPoint natural{0.2f, 0.3f};
    RequirePoint(NaturalToDisplay(natural, 0), 0.2f, 0.3f);
    RequirePoint(NaturalToDisplay(natural, 1), 0.3f, 0.8f);
    RequirePoint(NaturalToDisplay(natural, 2), 0.8f, 0.7f);
    RequirePoint(NaturalToDisplay(natural, 3), 0.7f, 0.2f);

    const NormalizedPoint devicePoint{
        10112.0f / 20224.0f,
        33360.0f / 44480.0f,
    };
    const NormalizedPoint landscape =
        NaturalToDisplay(devicePoint, 1);
    RequirePoint(landscape, 0.75f, 0.5f);

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

    RequirePixelPoint(
        DisplayToNaturalPixels(1450.0, 380.0, 2400, 1080, 0),
        1450.0, 380.0, 2400, 1080);
    RequirePixelPoint(
        DisplayToNaturalPixels(1450.0, 380.0, 2400, 1080, 1),
        700.0, 1450.0, 1080, 2400);
    RequirePixelPoint(
        DisplayToNaturalPixels(1450.0, 380.0, 2400, 1080, 2),
        950.0, 700.0, 2400, 1080);
    RequirePixelPoint(
        DisplayToNaturalPixels(1450.0, 380.0, 2400, 1080, 3),
        380.0, 950.0, 1080, 2400);

    RequirePixelPoint(
        DisplayToNaturalPixels(0.0, 0.0, 2400, 1080, 1),
        1080.0, 0.0, 1080, 2400);
    RequirePixelPoint(
        DisplayToNaturalPixels(2400.0, 1080.0, 2400, 1080, 3),
        1080.0, 0.0, 1080, 2400);
    RequirePixelPoint(
        DisplayToNaturalPixels(1450.0, 380.0, 2400, 1080, 5),
        700.0, 1450.0, 1080, 2400);
}
