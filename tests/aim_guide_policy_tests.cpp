#include "test_support.h"

#include "game/aim/AimGuidePolicy.h"

#include <limits>

void RunAimGuidePolicyTests() {
    using lengjing::game::aim::ShouldDrawAimTargetRay;

    REQUIRE(ShouldDrawAimTargetRay(true, true, 299.0f, 300.0f));
    REQUIRE(ShouldDrawAimTargetRay(true, true, 300.0f, 300.0f));
    REQUIRE(!ShouldDrawAimTargetRay(true, true, 301.0f, 300.0f));
    REQUIRE(!ShouldDrawAimTargetRay(false, true, 100.0f, 300.0f));
    REQUIRE(!ShouldDrawAimTargetRay(true, false, 100.0f, 300.0f));
    REQUIRE(!ShouldDrawAimTargetRay(true, true, -1.0f, 300.0f));
    REQUIRE(!ShouldDrawAimTargetRay(true, true, 100.0f, -1.0f));
    REQUIRE(!ShouldDrawAimTargetRay(
        true,
        true,
        std::numeric_limits<float>::infinity(),
        300.0f));
    REQUIRE(!ShouldDrawAimTargetRay(
        true,
        true,
        100.0f,
        std::numeric_limits<float>::quiet_NaN()));
}
