#include "test_support.h"

#include "game/native/CoordinateExecutionCachePolicy.h"

#include <array>
#include <chrono>

void RunCoordinateExecutionCachePolicyTests() {
    using namespace std::chrono_literals;
    using lengjing::game::native::
        CoordinateExecutionCacheVerificationInterval;
    using lengjing::game::native::ResolveCoordinateExecutionHealthSample;
    using lengjing::game::native::ShouldVerifyCoordinateExecutionCache;
    using lengjing::game::native::
        kCoordinateExecutionCacheVerificationMaximum;
    using lengjing::game::native::
        kCoordinateExecutionCacheVerificationMinimum;

    constexpr std::array<std::uintptr_t, 6> actors{
        0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000};
    bool staggered = false;
    const auto firstInterval =
        CoordinateExecutionCacheVerificationInterval(actors.front());
    for (const std::uintptr_t actor : actors) {
        const auto interval =
            CoordinateExecutionCacheVerificationInterval(actor);
        REQUIRE(interval >= kCoordinateExecutionCacheVerificationMinimum);
        REQUIRE(interval <= kCoordinateExecutionCacheVerificationMaximum);
        staggered = staggered || interval != firstInterval;
    }
    REQUIRE(staggered);

    using Clock = lengjing::game::native::CoordinateExecutionCacheClock;
    const Clock::time_point start{1s};
    const std::uintptr_t actor = actors.front();
    const auto interval = CoordinateExecutionCacheVerificationInterval(actor);
    REQUIRE(!ShouldVerifyCoordinateExecutionCache(
        actor, start, start, start + interval - 1ms));
    REQUIRE(ShouldVerifyCoordinateExecutionCache(
        actor, start, start, start + interval));
    REQUIRE(!ShouldVerifyCoordinateExecutionCache(
        actor, start, start + 100ms, start + 100ms + interval - 1ms));
    REQUIRE(ShouldVerifyCoordinateExecutionCache(
        actor, start, start + 100ms, start + 100ms + interval));
    REQUIRE(ShouldVerifyCoordinateExecutionCache(
        actor, Clock::time_point{}, Clock::time_point{}, start));
    REQUIRE(ShouldVerifyCoordinateExecutionCache(
        actor, start, start, start - 1ms));

    const auto cachedOutput = ResolveCoordinateExecutionHealthSample(
        true, 10, 10, 0, 0);
    REQUIRE(!cachedOutput.HasAttempt());
    REQUIRE(!cachedOutput.IsHealthy());

    const auto failedExecution = ResolveCoordinateExecutionHealthSample(
        true, 10, 10, 3, 0);
    REQUIRE(failedExecution.HasAttempt());
    REQUIRE(!failedExecution.IsHealthy());

    const auto successfulExecution = ResolveCoordinateExecutionHealthSample(
        true, 10, 10, 3, 1);
    REQUIRE(successfulExecution.HasAttempt());
    REQUIRE(successfulExecution.IsHealthy());

    const auto ordinaryOutput = ResolveCoordinateExecutionHealthSample(
        false, 10, 1, 3, 0);
    REQUIRE(ordinaryOutput.attempts == 10);
    REQUIRE(ordinaryOutput.successes == 1);
    REQUIRE(ordinaryOutput.IsHealthy());
}
