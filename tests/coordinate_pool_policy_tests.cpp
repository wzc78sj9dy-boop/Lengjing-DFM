#include "game/native/CoordinatePoolPolicy.h"
#include "test_support.h"

#include <algorithm>
#include <array>

void RunCoordinatePoolPolicyTests() {
    using lengjing::game::native::CoordinatePoolCodeFingerprint;
    using lengjing::game::native::CoordinatePoolCodeIdentityChanged;
    using lengjing::game::native::CoordinatePoolContextIdentityChanged;
    using lengjing::game::native::CoordinatePoolRingRefreshPhase;
    using lengjing::game::native::CoordinatePoolRingSearchBudget;
    using lengjing::game::native::CoordinatePoolRootSnapshot;
    using lengjing::game::native::CoordinatePoolRootStabilityWindow;
    using lengjing::game::native::CoordinatePoolRootSnapshotsMatch;
    using lengjing::game::native::NextCoordinatePoolCodeValidationFrame;
    using lengjing::game::native::NormalizeCoordinatePoolPointer;
    using lengjing::game::native::
        ShouldClearCoordinatePoolRingsAfterPointerRefresh;
    using lengjing::game::native::ShouldRefreshCoordinatePoolRing;
    using lengjing::game::native::ShouldRetryCoordinatePoolRing;
    using lengjing::game::native::ShouldValidateCoordinatePoolCode;
    using lengjing::game::native::kCoordinatePoolCodeValidationFrames;
    using lengjing::game::native::kCoordinatePoolCodeValidationRetryFrames;
    using lengjing::game::native::kCoordinatePoolRingRetryFrames;
    using lengjing::game::native::kCoordinatePoolRingSearchesPerFrame;

    constexpr std::uint32_t interval = 60;
    constexpr std::uintptr_t component = 0x101230;
    constexpr std::uint64_t stamp = 1;
    const std::uint64_t phase =
        CoordinatePoolRingRefreshPhase(component, interval);
    std::uint64_t scheduled = stamp + interval;
    while (scheduled % interval != phase) ++scheduled;

    REQUIRE(!ShouldRefreshCoordinatePoolRing(
        component, stamp, stamp + interval - 1, interval));
    REQUIRE(ShouldRefreshCoordinatePoolRing(
        component, stamp, scheduled, interval));
    REQUIRE(!ShouldRefreshCoordinatePoolRing(
        component, stamp, scheduled + 1, interval));
    REQUIRE(!ShouldRefreshCoordinatePoolRing(
        component, stamp, scheduled, 0));

    std::array<std::size_t, interval> phases{};
    for (std::uintptr_t index = 0; index < interval; ++index) {
        ++phases[CoordinatePoolRingRefreshPhase(
            0x100000 + index * 0x10, interval)];
    }
    REQUIRE(*std::max_element(phases.begin(), phases.end()) <= 2);

    REQUIRE(!ShouldRetryCoordinatePoolRing(
        100, 100 + kCoordinatePoolRingRetryFrames - 1));
    REQUIRE(ShouldRetryCoordinatePoolRing(
        100, 100 + kCoordinatePoolRingRetryFrames));
    REQUIRE(ShouldRetryCoordinatePoolRing(100, 99));

    CoordinatePoolRingSearchBudget budget;
    for (std::size_t index = 0;
         index < kCoordinatePoolRingSearchesPerFrame;
         ++index) {
        REQUIRE(budget.TryConsume(10));
    }
    REQUIRE(!budget.TryConsume(10));
    REQUIRE(budget.TryConsume(11));
    budget.Reset();
    REQUIRE(budget.TryConsume(11));

    constexpr CoordinatePoolRootSnapshot root{
        0x10000000,
        0x20000000,
        0x30000000,
    };
    REQUIRE(CoordinatePoolRootSnapshotsMatch(root, root));
    REQUIRE(!CoordinatePoolRootSnapshotsMatch(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge + 8,
            root.context,
            root.entry,
        }));
    REQUIRE(!CoordinatePoolCodeIdentityChanged(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge + 8,
            root.context + 8,
            root.entry,
        }));
    REQUIRE(CoordinatePoolContextIdentityChanged(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge + 8,
            root.context,
            root.entry,
        }));
    REQUIRE(CoordinatePoolCodeIdentityChanged(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge,
            root.context,
            root.entry + 4,
        }));
    REQUIRE(!CoordinatePoolRootSnapshotsMatch(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge,
            root.context + 8,
            root.entry,
        }));
    REQUIRE(!CoordinatePoolRootSnapshotsMatch(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge,
            root.context,
            root.entry + 4,
        }));

    CoordinatePoolRootStabilityWindow stability;
    REQUIRE(!stability.Observe(root));
    REQUIRE(!stability.Observe(CoordinatePoolRootSnapshot{
        root.bridge,
        root.context,
        root.entry + 4,
    }));
    REQUIRE(stability.Observe(CoordinatePoolRootSnapshot{
        root.bridge,
        root.context,
        root.entry + 4,
    }));
    stability.Reset();
    REQUIRE(!stability.Observe(root));
    REQUIRE(stability.Observe(root));

    std::array<std::uint8_t, 4096> codePage{};
    const std::uint64_t zeroFingerprint =
        CoordinatePoolCodeFingerprint(codePage.data(), codePage.size());
    const std::uint64_t codeRangeFingerprint =
        CoordinatePoolCodeFingerprint(codePage.data() + 128, 64);
    REQUIRE(zeroFingerprint ==
        CoordinatePoolCodeFingerprint(codePage.data(), codePage.size()));
    codePage[2048] = 1;
    REQUIRE(zeroFingerprint !=
        CoordinatePoolCodeFingerprint(codePage.data(), codePage.size()));
    REQUIRE(codeRangeFingerprint ==
        CoordinatePoolCodeFingerprint(codePage.data() + 128, 64));
    codePage[160] = 1;
    REQUIRE(codeRangeFingerprint !=
        CoordinatePoolCodeFingerprint(codePage.data() + 128, 64));

    constexpr std::uint64_t pointer = UINT64_C(0x0000007123456780);
    REQUIRE(NormalizeCoordinatePoolPointer(
        UINT64_C(0xABCD007123456780)) == pointer);
    REQUIRE(NormalizeCoordinatePoolPointer(pointer) == pointer);
    REQUIRE(!ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        true,
        NormalizeCoordinatePoolPointer(UINT64_C(0xABCD007123456780)),
        NormalizeCoordinatePoolPointer(UINT64_C(0x1234007123456780))));

    REQUIRE(ShouldValidateCoordinatePoolCode(100, 100, false));
    REQUIRE(!ShouldValidateCoordinatePoolCode(100, 101, false));
    REQUIRE(ShouldValidateCoordinatePoolCode(100, 101, true));
    REQUIRE(ShouldValidateCoordinatePoolCode(5, 1000, false));
    REQUIRE(NextCoordinatePoolCodeValidationFrame(100, true) ==
        100 + kCoordinatePoolCodeValidationFrames);
    REQUIRE(NextCoordinatePoolCodeValidationFrame(100, false) ==
        100 + kCoordinatePoolCodeValidationRetryFrames);
    REQUIRE(NextCoordinatePoolCodeValidationFrame(UINT64_MAX, true) ==
        UINT64_MAX);

    REQUIRE(!ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        false, 0x10000000, 0));
    REQUIRE(ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        true, 0x10000000, 0x20000000));
    REQUIRE(!ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        true, 0, 0x10000000));
    REQUIRE(!ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        true, 0x10000000, 0x10000000));
}
