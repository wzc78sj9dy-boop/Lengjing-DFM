#include "test_support.h"

#include "render/PresentationRateTracker.h"

#include <chrono>

void RunPresentationRateTests() {
    using namespace std::chrono_literals;
    using lengjing::render::PresentationRateTracker;

    PresentationRateTracker tracker;
    const auto start = PresentationRateTracker::TimePoint{} + 10s;
    tracker.RecordAt(start);
    for (int frame = 1; frame <= 32; ++frame) {
        tracker.RecordAt(start + frame * 16ms);
    }

    const float activeRate = tracker.ReadAt(start + 512ms);
    REQUIRE(activeRate > 55.0f);
    REQUIRE(activeRate < 70.0f);
    REQUIRE(tracker.ReadAt(start + 2200ms) == 0.0f);

    tracker.Reset();
    REQUIRE(tracker.ReadAt(start + 2300ms) == 0.0f);
}
