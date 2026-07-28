#include "game/native/CoordinateStabilityPolicy.h"
#include "test_support.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>

namespace {

using Decision =
    lengjing::game::native::CoordinateStabilityDecision;
using Policy =
    lengjing::game::native::CoordinateStabilityPolicy;
using State =
    lengjing::game::native::CoordinateStabilityState;
using Coordinate =
    lengjing::game::native::StableCoordinate;

constexpr std::uintptr_t kComponent = 0x12340000;

Coordinate Point(float x, float y, float z = 0.0f) {
    return Coordinate{x, y, z};
}

State MakeHistory(
    const Coordinate& current,
    const Coordinate& latest,
    const Coordinate& older,
    std::uint32_t count = 6) {
    State state{};
    state.current = current;
    state.componentGeneration = kComponent;
    state.count = count;
    state.ringIndex = count;
    state.initialized = true;
    for (std::uint32_t index = 0; index + 1 < count; ++index) {
        state.history[index] = older;
    }
    state.history[count - 1] = latest;
    return state;
}

void TestValidityBoundaries() {
    REQUIRE(!Policy::IsValid(Point(0.0f, 0.0f, 0.0f)));
    REQUIRE(!Policy::IsValid(Point(1.0f, 0.0f, 1001.0f)));
    REQUIRE(!Policy::IsValid(Point(10.0f, 0.0f, 0.0f)));
    REQUIRE(!Policy::IsValid(Point(2.0f, 0.0f, 1000.0f)));
    REQUIRE(Policy::IsValid(Point(2.0f, 0.0f, 1001.0f)));
    REQUIRE(Policy::IsValid(Point(11.0f, 0.0f, 0.0f)));
    REQUIRE(Policy::IsValid(Point(1000000.0f, 0.0f, 0.0f)));
    REQUIRE(!Policy::IsValid(Point(1000001.0f, 0.0f, 0.0f)));
    REQUIRE(!Policy::IsValid(Point(
        std::numeric_limits<float>::infinity(),
        1.0f)));
    REQUIRE(!Policy::IsValid(Point(
        std::numeric_limits<float>::quiet_NaN(),
        1.0f)));
}

void TestAppendAndRingCapacity() {
    State state{};
    for (std::uint32_t index = 0; index < 55; ++index) {
        const Coordinate candidate =
            Point(100.0f + static_cast<float>(index), 100.0f);
        REQUIRE(
            Policy::Submit(state, kComponent, candidate) ==
            Decision::Accepted);
    }

    REQUIRE(state.initialized);
    REQUIRE(state.componentGeneration == kComponent);
    REQUIRE(state.count == Policy::kHistoryCapacity);
    REQUIRE(state.ringIndex == 5);
    REQUIRE(state.invalidStreak == 0);
    REQUIRE(state.current == Point(154.0f, 100.0f));
    REQUIRE(state.history[4] == Point(154.0f, 100.0f));
    REQUIRE(state.history[5] == Point(105.0f, 100.0f));
}

void TestComponentGenerationReset() {
    State state{};
    REQUIRE(
        Policy::Submit(state, kComponent, Point(100.0f, 100.0f)) ==
        Decision::Accepted);
    REQUIRE(
        Policy::Submit(state, kComponent, Point(110.0f, 100.0f)) ==
        Decision::Accepted);

    const std::uintptr_t nextComponent = kComponent + 0x1000;
    REQUIRE(
        Policy::Submit(state, nextComponent, Point(300.0f, 300.0f)) ==
        Decision::Accepted);
    REQUIRE(state.componentGeneration == nextComponent);
    REQUIRE(state.count == 3);
    REQUIRE(state.ringIndex == 3);
    REQUIRE(state.history[0] == Point(100.0f, 100.0f));
    REQUIRE(state.history[1] == Point(110.0f, 100.0f));
    REQUIRE(state.history[2] == Point(300.0f, 300.0f));
}

void TestImmediateJumpThreshold() {
    State state{};
    REQUIRE(
        Policy::Submit(state, kComponent, Point(100.0f, 100.0f)) ==
        Decision::Accepted);

    REQUIRE(!Policy::ShouldReject(
        state,
        kComponent,
        Point(1600.0f, 100.0f)));
    REQUIRE(Policy::ShouldReject(
        state,
        kComponent,
        Point(1601.0f, 100.0f)));

    state.invalidStreak = Policy::kInvalidStreakLimit;
    REQUIRE(!Policy::ShouldReject(
        state,
        kComponent,
        Point(1601.0f, 100.0f)));
}

void TestHistoryGateAndWeighting() {
    State weightedReject = MakeHistory(
        Point(1000.0f, 100.0f),
        Point(1000.0f, 100.0f),
        Point(-1000.0f, 100.0f));
    const Coordinate candidate = Point(1500.0f, 100.0f);
    REQUIRE(Policy::ShouldReject(
        weightedReject,
        kComponent,
        candidate));

    State tooShort = weightedReject;
    tooShort.count = 5;
    REQUIRE(!Policy::ShouldReject(
        tooShort,
        kComponent,
        candidate));

    REQUIRE(!Policy::ShouldReject(
        weightedReject,
        kComponent + 8,
        candidate));

    State strictLatest = MakeHistory(
        Point(1000.0f, 100.0f),
        Point(1000.0f, 100.0f),
        Point(-1000.0f, 100.0f));
    REQUIRE(!Policy::ShouldReject(
        strictLatest,
        kComponent,
        Point(1350.0f, 100.0f)));

    State harmonicMean = MakeHistory(
        Point(500.0f, 100.0f),
        Point(500.0f, 100.0f),
        Point(-700.0f, 100.0f));
    REQUIRE(!Policy::ShouldReject(
        harmonicMean,
        kComponent,
        Point(1000.0f, 100.0f)));
}

void TestInvalidHistoryStopsScan() {
    State state = MakeHistory(
        Point(1000.0f, 100.0f),
        Point(1000.0f, 100.0f),
        Point(-1000.0f, 100.0f));
    state.history[4] = Coordinate{};
    REQUIRE(!Policy::ShouldReject(
        state,
        kComponent,
        Point(1500.0f, 100.0f)));
}

void TestRejectedJumpLimit() {
    State state{};
    const Coordinate baseline = Point(100.0f, 100.0f);
    const Coordinate jump = Point(2000.0f, 100.0f);
    REQUIRE(
        Policy::Submit(state, kComponent, baseline) ==
        Decision::Accepted);

    for (std::uint32_t streak = 1;
         streak < Policy::kInvalidStreakLimit;
         ++streak) {
        REQUIRE(
            Policy::Submit(state, kComponent, jump) ==
            Decision::Rejected);
        REQUIRE(state.invalidStreak == streak);
        REQUIRE(state.current == baseline);
        REQUIRE(state.count == 1);
    }

    REQUIRE(
        Policy::Submit(state, kComponent, jump) ==
        Decision::InvalidCleared);
    REQUIRE(!state.initialized);
    REQUIRE(state.invalidStreak == 0);
    REQUIRE(state.count == 0);

    REQUIRE(
        Policy::Submit(state, kComponent, jump) ==
        Decision::Accepted);
    REQUIRE(state.invalidStreak == 0);
    REQUIRE(state.current == jump);
    REQUIRE(state.count == 1);
    REQUIRE(state.ringIndex == 1);
}

void TestInvalidLimit() {
    State state{};
    const Coordinate baseline = Point(100.0f, 100.0f);
    REQUIRE(
        Policy::Submit(state, kComponent, baseline) ==
        Decision::Accepted);

    for (std::uint32_t streak = 1;
         streak < Policy::kInvalidStreakLimit;
         ++streak) {
        REQUIRE(
            Policy::RecordInvalid(state, kComponent) ==
            Decision::InvalidRetained);
        REQUIRE(state.invalidStreak == streak);
        REQUIRE(state.initialized);
        REQUIRE(state.current == baseline);
    }

    REQUIRE(
        Policy::RecordInvalid(state, kComponent) ==
        Decision::InvalidCleared);
    REQUIRE(!state.initialized);
    REQUIRE(state.componentGeneration == kComponent);
    REQUIRE(state.invalidStreak == 0);
    REQUIRE(state.count == 0);
    REQUIRE(state.current == Coordinate{});
}

void TestInvalidComponentChange() {
    State state{};
    REQUIRE(
        Policy::Submit(state, kComponent, Point(100.0f, 100.0f)) ==
        Decision::Accepted);

    const std::uintptr_t nextComponent = kComponent + 0x1000;
    REQUIRE(
        Policy::RecordInvalid(state, nextComponent) ==
        Decision::InvalidRetained);
    REQUIRE(state.initialized);
    REQUIRE(state.componentGeneration == nextComponent);
    REQUIRE(state.invalidStreak == 1);
    REQUIRE(state.count == 1);
    REQUIRE(state.current == Point(100.0f, 100.0f));
}

void RunTests() {
    TestValidityBoundaries();
    TestAppendAndRingCapacity();
    TestComponentGenerationReset();
    TestImmediateJumpThreshold();
    TestHistoryGateAndWeighting();
    TestInvalidHistoryStopsScan();
    TestRejectedJumpLimit();
    TestInvalidLimit();
    TestInvalidComponentChange();
}

}

int main() {
    try {
        RunTests();
        std::cout << "coordinate stability policy tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
