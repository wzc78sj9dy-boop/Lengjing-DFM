#include "game/aim/AimGuidePolicy.h"

#include "test_support.h"

#include <limits>

void RunAimGuidePolicyTests() {
    using lengjing::game::aim::ShouldDrawTargetRay;
    using lengjing::game::aim::TargetInsideAimRange;

    REQUIRE(TargetInsideAimRange(149.9f, 150.0f));
    REQUIRE(TargetInsideAimRange(150.0f, 150.0f));
    REQUIRE(!TargetInsideAimRange(150.1f, 150.0f));
    REQUIRE(!TargetInsideAimRange(-1.0f, 150.0f));
    REQUIRE(!TargetInsideAimRange(0.0f, -1.0f));
    REQUIRE(!TargetInsideAimRange(
        std::numeric_limits<float>::quiet_NaN(), 150.0f));
    REQUIRE(!TargetInsideAimRange(
        100.0f, std::numeric_limits<float>::infinity()));

    REQUIRE(ShouldDrawTargetRay(true, true, true, 150.0f, 150.0f));
    REQUIRE(!ShouldDrawTargetRay(true, true, true, 150.1f, 150.0f));
    REQUIRE(ShouldDrawTargetRay(true, true, false, 500.0f, 150.0f));
    REQUIRE(!ShouldDrawTargetRay(
        true,
        true,
        false,
        std::numeric_limits<float>::quiet_NaN(),
        150.0f));
    REQUIRE(!ShouldDrawTargetRay(false, true, true, 100.0f, 150.0f));
    REQUIRE(!ShouldDrawTargetRay(true, false, true, 100.0f, 150.0f));
}
