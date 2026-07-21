#include "game/native/CoordinatePoolPolicy.h"
#include "test_support.h"

#include <algorithm>
#include <array>

void RunCoordinatePoolPolicyTests() {
    using lengjing::game::native::CoordinatePoolRingRefreshPhase;
    using lengjing::game::native::CoordinatePoolRingSearchBudget;
    using lengjing::game::native::ShouldRefreshCoordinatePoolRing;
    using lengjing::game::native::ShouldRetryCoordinatePoolRing;
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
}
