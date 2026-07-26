#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

using CoordinateExecutionCacheClock = std::chrono::steady_clock;

inline constexpr auto kCoordinateExecutionCacheVerificationMinimum =
    std::chrono::milliseconds(2000);
inline constexpr auto kCoordinateExecutionCacheVerificationMaximum =
    std::chrono::milliseconds(2500);

constexpr std::uint64_t MixCoordinateExecutionCacheActor(
    std::uintptr_t actor) noexcept {
    std::uint64_t value = static_cast<std::uint64_t>(actor);
    value ^= value >> 30U;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31U;
    return value;
}

constexpr std::chrono::milliseconds
CoordinateExecutionCacheVerificationInterval(
    std::uintptr_t actor) noexcept {
    constexpr std::uint64_t span = static_cast<std::uint64_t>(
        (kCoordinateExecutionCacheVerificationMaximum -
         kCoordinateExecutionCacheVerificationMinimum)
            .count());
    return kCoordinateExecutionCacheVerificationMinimum +
        std::chrono::milliseconds(
            MixCoordinateExecutionCacheActor(actor) % (span + 1U));
}

inline bool ShouldVerifyCoordinateExecutionCache(
    std::uintptr_t actor,
    CoordinateExecutionCacheClock::time_point verifiedAt,
    CoordinateExecutionCacheClock::time_point attemptedAt,
    CoordinateExecutionCacheClock::time_point now) noexcept {
    const auto scheduledFrom = attemptedAt > verifiedAt
        ? attemptedAt
        : verifiedAt;
    return actor == 0 || scheduledFrom.time_since_epoch().count() == 0 ||
        now < scheduledFrom ||
        now - scheduledFrom >=
            CoordinateExecutionCacheVerificationInterval(actor);
}

struct CoordinateExecutionHealthSample {
    std::size_t attempts = 0;
    std::size_t successes = 0;

    constexpr bool HasAttempt() const noexcept {
        return attempts != 0;
    }

    constexpr bool IsHealthy() const noexcept {
        return attempts != 0 && successes != 0;
    }
};

constexpr CoordinateExecutionHealthSample
ResolveCoordinateExecutionHealthSample(
    bool coordinateExecution,
    std::size_t outputAttempts,
    std::size_t outputSuccesses,
    std::size_t executionAttempts,
    std::size_t executionSuccesses) noexcept {
    return coordinateExecution
        ? CoordinateExecutionHealthSample{
              executionAttempts, executionSuccesses}
        : CoordinateExecutionHealthSample{outputAttempts, outputSuccesses};
}

}  // namespace lengjing::game::native
