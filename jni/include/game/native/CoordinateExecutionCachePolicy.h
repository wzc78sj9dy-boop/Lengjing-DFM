#pragma once

#include <cstddef>

namespace lengjing::game::native {

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

constexpr bool ShouldAccelerateCoordinateExecutionRecovery(
    bool coordinateExecution,
    bool contextHadSuccess,
    bool agedDecodedFailure) noexcept {
    return coordinateExecution
        ? !contextHadSuccess
        : agedDecodedFailure;
}

}  // namespace lengjing::game::native
