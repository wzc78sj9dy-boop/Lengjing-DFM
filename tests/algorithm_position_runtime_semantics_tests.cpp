#include "game/native/AlgorithmPositionPolicy.h"
#include "game/native/AlgorithmPositionRuntime.h"
#include "test_support.h"

#include <cstddef>
#include <cstdint>

void RunAlgorithmPositionRuntimeSemanticsTests() {
    using namespace lengjing::game::native;

    static_assert(kAlgorithmPositionPointerPayloadMask ==
                  UINT64_C(0x0000FFFFFFFFFFFF));
    static_assert(NormalizeAlgorithmPositionRemoteAddress(
                      UINT64_C(0xABCD123456789ABC)) ==
                  UINT64_C(0x0000123456789ABC));

    REQUIRE(ParseAlgorithmPositionDecryptRva("0x1234").decryptRva ==
            0x1234U);
    REQUIRE(!ParseAlgorithmPositionDecryptRva("0x1235").IsConfigured());
    REQUIRE(ParseAlgorithmPositionGuestPc("0x7000123400").absoluteGuestPc ==
            UINT64_C(0x7000123400));
    REQUIRE(!ParseAlgorithmPositionGuestPc(
                 "0xabcd7000123400").IsConfigured());
    std::uintptr_t resolvedGuestPc = 0;
    REQUIRE(ResolveAlgorithmPositionGuestPc(
        AlgorithmPositionRuntimeConfig{0x1234, 0},
        UINT64_C(0x7000000000),
        resolvedGuestPc));
    REQUIRE(resolvedGuestPc == UINT64_C(0x7000001234));
    REQUIRE(ResolveAlgorithmPositionGuestPc(
        AlgorithmPositionRuntimeConfig{0, UINT64_C(0x7100001234)},
        UINT64_C(0x7000000000),
        resolvedGuestPc));
    REQUIRE(resolvedGuestPc == UINT64_C(0x7100001234));
    REQUIRE(!ResolveAlgorithmPositionGuestPc(
        AlgorithmPositionRuntimeConfig{0x1234, UINT64_C(0x7100001234)},
        UINT64_C(0x7000000000),
        resolvedGuestPc));

    constexpr AlgorithmPositionRefreshPlan normalFirst =
        MakeAlgorithmPositionRefreshPlan(true);
    REQUIRE(normalFirst.first);
    REQUIRE(normalFirst.discarded);
    REQUIRE(!normalFirst.candidate);

    constexpr AlgorithmPositionRefreshPlan normalLater =
        MakeAlgorithmPositionRefreshPlan(false);
    REQUIRE(!normalLater.first);
    REQUIRE(normalLater.discarded);
    REQUIRE(!normalLater.candidate);

    REQUIRE(!ShouldClearAlgorithmPositionCoordinatePages(200));
    REQUIRE(ShouldClearAlgorithmPositionCoordinatePages(201));
    REQUIRE(kAlgorithmPositionMaximumCachedPages == 4096);
    REQUIRE(kAlgorithmPositionRetainedCachedPages == 3072);
    REQUIRE(kAlgorithmPositionRefreshBatchSize == 1024);

    constexpr std::uint64_t supportedSvc[] = {
        25, 29, 62, 98, 172, 178, 278,
    };
    for (const std::uint64_t number : supportedSvc) {
        REQUIRE(IsSupportedAlgorithmPositionSvc(number));
    }
    REQUIRE(!IsSupportedAlgorithmPositionSvc(0));
    REQUIRE(!IsSupportedAlgorithmPositionSvc(999));
}

#if defined(LENGJING_STANDALONE_RUNTIME_SEMANTICS_TEST)
int main() {
    RunAlgorithmPositionRuntimeSemanticsTests();
    return 0;
}
#endif
