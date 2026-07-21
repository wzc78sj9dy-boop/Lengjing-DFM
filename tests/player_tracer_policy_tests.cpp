#include "render/PlayerTracerPolicy.h"

#include "test_support.h"

#include <cmath>
#include <limits>

namespace {

bool Near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

}  // namespace

void RunPlayerTracerPolicyTests() {
    const ImVec2 origin = lengjing::render::TopTracerOrigin(2376.0f);
    REQUIRE(Near(origin.x, 1188.0f));
    REQUIRE(Near(origin.y, 10.0f));

    const lengjing::ScreenRect bounds{400.0f, 120.0f, 520.0f, 640.0f};
    const ImVec2 target = lengjing::render::TopTracerTarget(bounds);
    REQUIRE(Near(target.x, 460.0f));
    REQUIRE(Near(target.y, 120.0f));
    REQUIRE(target.y == bounds.top);
    REQUIRE(target.y != bounds.bottom);

    const ImVec2 sanitized = lengjing::render::TopTracerOrigin(
        std::numeric_limits<float>::quiet_NaN(), -5.0f);
    REQUIRE(Near(sanitized.x, 0.0f));
    REQUIRE(Near(sanitized.y, 0.0f));
}
