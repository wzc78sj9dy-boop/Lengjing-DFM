#include "game/native/WorldObjectRefreshPolicy.h"
#include "test_support.h"

#include <chrono>

void RunWorldObjectRefreshPolicyTests() {
    using Policy = lengjing::game::native::WorldObjectRefreshPolicy;
    using namespace std::chrono_literals;

    Policy policy(500ms);
    const Policy::TimePoint start{};
    REQUIRE(!policy.ShouldRefresh(0, 1, start));
    REQUIRE(policy.ShouldRefresh(0x1000, 1, start));

    policy.MarkRefreshed(0x1000, 1, start);
    REQUIRE(!policy.ShouldRefresh(0x1000, 1, start + 499ms));
    REQUIRE(policy.ShouldRefresh(0x1000, 1, start + 500ms));
    REQUIRE(policy.ShouldRefresh(0x2000, 1, start + 1ms));
    REQUIRE(policy.ShouldRefresh(0x1000, 2, start + 1ms));

    policy.Invalidate();
    REQUIRE(policy.ShouldRefresh(0x1000, 1, start + 1ms));
}
