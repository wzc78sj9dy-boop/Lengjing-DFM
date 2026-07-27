#include "test_support.h"

#include "game/native/CoordinateExecutionCachePolicy.h"

void RunCoordinateExecutionCachePolicyTests() {
    using lengjing::game::native::ResolveCoordinateExecutionHealthSample;
    using lengjing::game::native::
        ShouldAccelerateCoordinateExecutionRecovery;

    const auto noExecutionAttempt = ResolveCoordinateExecutionHealthSample(
        true, 10, 10, 0, 0);
    REQUIRE(!noExecutionAttempt.HasAttempt());
    REQUIRE(!noExecutionAttempt.IsHealthy());

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

    REQUIRE(ShouldAccelerateCoordinateExecutionRecovery(
        true, false, false));
    REQUIRE(!ShouldAccelerateCoordinateExecutionRecovery(
        true, true, false));
    REQUIRE(!ShouldAccelerateCoordinateExecutionRecovery(
        true, true, true));
    REQUIRE(!ShouldAccelerateCoordinateExecutionRecovery(
        false, false, false));
    REQUIRE(ShouldAccelerateCoordinateExecutionRecovery(
        false, true, true));
}
