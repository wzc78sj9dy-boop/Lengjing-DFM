#include "render/OverlayContrastPolicy.h"

#include "test_support.h"

#include <cmath>
#include <cstdint>
#include <limits>

void RunOverlayContrastPolicyTests() {
    using lengjing::render::PlayerOutlineWidth;
    using lengjing::render::PlayerStrokeWidth;
    using lengjing::render::TextOutlineOffset;
    using lengjing::render::WithExactAlpha;
    using lengjing::render::WithMinimumAlpha;
    using lengjing::render::kTextOutlineDirections;

    REQUIRE((WithMinimumAlpha(0x40112233U, 210U) >> 24U) == 210U);
    REQUIRE((WithMinimumAlpha(0xff112233U, 210U) >> 24U) == 255U);
    REQUIRE((WithExactAlpha(0x40112233U, 255U) >> 24U) == 255U);
    REQUIRE((WithExactAlpha(0x40112233U, 255U) & 0x00ffffffU) ==
            0x00112233U);

    REQUIRE(kTextOutlineDirections.size() == 4U);
    REQUIRE(kTextOutlineDirections[0].x < 0.0f);
    REQUIRE(kTextOutlineDirections[1].x > 0.0f);
    REQUIRE(kTextOutlineDirections[2].y < 0.0f);
    REQUIRE(kTextOutlineDirections[3].y > 0.0f);
    REQUIRE(std::fabs(TextOutlineOffset(24.0f) - 1.08f) < 0.0001f);
    REQUIRE(std::fabs(TextOutlineOffset(0.0f) - 0.75f) < 0.0001f);
    REQUIRE(std::fabs(TextOutlineOffset(
                std::numeric_limits<float>::quiet_NaN()) - 0.75f) <
            0.0001f);

    const float stroke = PlayerStrokeWidth(2.0f, 1.0f);
    REQUIRE(std::fabs(stroke - 2.0f) < 0.0001f);
    REQUIRE(std::fabs(PlayerOutlineWidth(stroke, 4.0f, 1.0f) - 4.0f) <
            0.0001f);
    REQUIRE(std::fabs(PlayerOutlineWidth(5.0f, 0.0f, 1.0f) - 6.0f) <
            0.0001f);
    REQUIRE(std::fabs(PlayerOutlineWidth(2.0f, 4.0f, 2.0f) - 8.0f) <
            0.0001f);
    REQUIRE(std::fabs(PlayerStrokeWidth(1.0f, 1.0f) - 2.0f) < 0.0001f);
    REQUIRE(PlayerStrokeWidth(0.0f, 1.0f) >= 2.0f);
    REQUIRE(PlayerStrokeWidth(
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()) >= 2.0f);
    REQUIRE(PlayerOutlineWidth(
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()) >= 3.0f);
}
