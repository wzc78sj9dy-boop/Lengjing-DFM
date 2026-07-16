#include "test_support.h"

#include "game/aim/TrackingCalculator.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

bool NearlyEqual(float left, float right, float tolerance = 0.0001f)
{
    return std::fabs(left - right) <= tolerance;
}

}  // namespace

void RunTrackingCalculatorTests()
{
    using lengjing::game::aim::HitSelectionCache;
    using lengjing::game::aim::TrackingCalculator;
    using lengjing::game::aim::TrackingCommand;
    using lengjing::game::aim::TrackingPoint;
    using lengjing::game::aim::TrackingPrediction;

    static_assert(sizeof(TrackingCommand) == 12);

    const TrackingPoint origin{};
    const TrackingPoint forward{1000.0f, 0.0f, 0.0f};
    const TrackingPoint stopped{};

    const TrackingCommand disabled = TrackingCalculator::Calculate(
        false, origin, forward, stopped, 500.0f, 5.24f);
    REQUIRE(disabled.pitch == 0.0f);
    REQUIRE(disabled.yaw == 0.0f);
    REQUIRE(disabled.flag == 0);

    const TrackingCommand straight = TrackingCalculator::Calculate(
        true, origin, forward, stopped, 500.0f, 0.0f);
    REQUIRE(straight.pitch == 0.0f);
    REQUIRE(straight.yaw == 0.0f);
    REQUIRE(straight.flag == 1);

    const TrackingCommand diagonal = TrackingCalculator::Calculate(
        true,
        origin,
        TrackingPoint{0.0f, 1000.0f, 1000.0f},
        stopped,
        500.0f,
        0.0f);
    REQUIRE(NearlyEqual(diagonal.pitch, 45.0f));
    REQUIRE(NearlyEqual(diagonal.yaw, 90.0f));
    REQUIRE(diagonal.flag == 1);

    const TrackingPoint lateralVelocity{0.0f, 100.0f, 0.0f};
    const TrackingCommand truncatedDistance = TrackingCalculator::Calculate(
        true,
        origin,
        TrackingPoint{199.9f, 0.0f, 0.0f},
        lateralVelocity,
        1.0f,
        0.0f);
    const float expectedYaw = static_cast<float>(
        std::atan2(100.0, 199.9) * 57.29577951308232286465);
    REQUIRE(NearlyEqual(truncatedDistance.yaw, expectedYaw));
    const TrackingPrediction predicted = TrackingCalculator::Predict(
        origin,
        TrackingPoint{199.9f, 0.0f, 0.0f},
        lateralVelocity,
        1.0f,
        0.0f);
    REQUIRE(predicted.valid);
    REQUIRE(predicted.distanceMeters == 1);
    REQUIRE(NearlyEqual(predicted.point.y, 100.0f));

    const TrackingCommand zeroVerticalVelocity = TrackingCalculator::Calculate(
        true,
        origin,
        forward,
        TrackingPoint{0.0f, 0.0f, 0.0f},
        500.0f,
        5.24f);
    const TrackingCommand largeVerticalVelocity = TrackingCalculator::Calculate(
        true,
        origin,
        forward,
        TrackingPoint{0.0f, 0.0f, 1000000.0f},
        500.0f,
        5.24f);
    REQUIRE(zeroVerticalVelocity.pitch == 0.0f);
    REQUIRE(largeVerticalVelocity.pitch > zeroVerticalVelocity.pitch);
    REQUIRE(zeroVerticalVelocity.yaw == largeVerticalVelocity.yaw);

    const float expectedHeight = 0.02f * 0.02f * (5.24f * 22.5f);
    const float expectedPitch = static_cast<float>(
        std::atan2(static_cast<double>(expectedHeight), 1000.0)
        * 57.29577951308232286465);
    REQUIRE(NearlyEqual(largeVerticalVelocity.pitch, expectedPitch));

    const TrackingCommand zeroSpeed = TrackingCalculator::Calculate(
        true, origin, forward, stopped, 0.0f, 5.24f);
    REQUIRE(zeroSpeed.flag == 0);
    const TrackingCommand nonFinite = TrackingCalculator::Calculate(
        true,
        origin,
        TrackingPoint{std::numeric_limits<float>::infinity(), 0.0f, 0.0f},
        stopped,
        500.0f,
        5.24f);
    REQUIRE(nonFinite.flag == 0);

    REQUIRE(HitSelectionCache::SelectedCountForPercentage(40, 5) == 2);

    std::uint32_t randomCalls = 0;
    const auto random = [&randomCalls](std::uint32_t seed) {
        ++randomCalls;
        return seed * 7U + 3U;
    };

    HitSelectionCache cache;
    const auto first = cache.Evaluate(2, 2, 5, random);
    REQUIRE(first.validInput);
    REQUIRE(first.accepted);
    REQUIRE(first.cacheRebuilt);
    REQUIRE(randomCalls == 2);

    const auto cached = cache.Evaluate(2, 0, 5, random);
    REQUIRE(cached.validInput);
    REQUIRE(cached.accepted);
    REQUIRE(!cached.cacheRebuilt);
    REQUIRE(randomCalls == 2);

    const auto rebuilt = cache.Evaluate(1, 0, 5, random);
    REQUIRE(rebuilt.validInput);
    REQUIRE(rebuilt.accepted);
    REQUIRE(rebuilt.cacheRebuilt);
    REQUIRE(randomCalls == 3);

    REQUIRE(!cache.Evaluate(0, 0, -1, random).validInput);
    REQUIRE(!cache.Evaluate(1, 6, 5, random).validInput);

    const auto terminalIndex = cache.Evaluate(6, 5, 5, {});
    REQUIRE(terminalIndex.validInput);
    REQUIRE(terminalIndex.accepted);
    const auto terminalBeforeRange = cache.Evaluate(0, 101, 101, {});
    REQUIRE(terminalBeforeRange.validInput);
    REQUIRE(terminalBeforeRange.accepted);

    const auto tooMany = cache.Evaluate(6, 1, 5, random);
    REQUIRE(tooMany.validInput);
    REQUIRE(!tooMany.accepted);
    REQUIRE(tooMany.selectionUnreachable);

    std::uint32_t duplicateCalls = 0;
    const auto duplicate = [&duplicateCalls](std::uint32_t) {
        ++duplicateCalls;
        return 0U;
    };
    const auto duplicateFailure = cache.Evaluate(2, 0, 5, duplicate);
    REQUIRE(duplicateFailure.validInput);
    REQUIRE(!duplicateFailure.accepted);
    REQUIRE(duplicateFailure.selectionUnreachable);
    REQUIRE(duplicateCalls == 4096);

    cache.Reset();
    const auto afterReset = cache.Evaluate(1, 0, 5, random);
    REQUIRE(afterReset.cacheRebuilt);
    REQUIRE(randomCalls == 4);
}
