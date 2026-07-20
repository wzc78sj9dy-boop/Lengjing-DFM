#include "game/native/CoordinatePoolPolicy.h"
#include "test_support.h"

void RunCoordinatePoolPolicyTests() {
    using namespace lengjing::game::native;

    REQUIRE(kCoordinatePoolStableReadAttempts == 4);
    REQUIRE(NormalizeCoordinatePoolPointer(
                UINT64_C(0xABCD123456789ABC)) ==
            UINT64_C(0x0000123456789ABC));
    REQUIRE(ShouldRefreshCoordinatePoolState(1, 0, 60));
    REQUIRE(!ShouldRefreshCoordinatePoolState(59, 1, 60));
    REQUIRE(ShouldRefreshCoordinatePoolState(61, 1, 60));
    REQUIRE(ShouldRefreshCoordinatePoolState(1, 100, 60));
    REQUIRE(ShouldRetryCoordinatePoolRing(true, false));
    REQUIRE(!ShouldRetryCoordinatePoolRing(false, false));
    REQUIRE(!ShouldRetryCoordinatePoolRing(true, true));
}

#if defined(LENGJING_STANDALONE_COORDINATE_POOL_POLICY_TEST)
int main() {
    RunCoordinatePoolPolicyTests();
    return 0;
}
#endif
