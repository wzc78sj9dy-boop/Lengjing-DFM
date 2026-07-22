#include "game/native/FrameProjection.h"
#include "test_support.h"

#include <array>
#include <cmath>
#include <limits>

void RunFrameProjectionTests() {
    using namespace lengjing::game::native;

    REQUIRE(IsProjectionViewCacheCompatible(
        true, 0x1000, 0x2000, 0x1000, 0x2000));
    REQUIRE(!IsProjectionViewCacheCompatible(
        false, 0x1000, 0x2000, 0x1000, 0x2000));
    REQUIRE(!IsProjectionViewCacheCompatible(
        true, 0x1000, 0x2000, 0x3000, 0x2000));
    REQUIRE(!IsProjectionViewCacheCompatible(
        true, 0x1000, 0x2000, 0x1000, 0x4000));

    ProjectionFovStabilityState fovState{};
    float resolvedFov = 0.0f;
    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 1, true, 90.0f, true, 90.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 90.0f) < 0.001f);

    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 2, true, 60.0f, true, 60.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 90.0f) < 0.001f);
    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 2, true, 60.0f, true, 60.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 90.0f) < 0.001f);
    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 3, true, 60.0f, true, 60.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 60.0f) < 0.001f);

    REQUIRE(ResolveStableProjectionFov(
        0x1000, 0x2000, 4, true, 60.0f, true, 75.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 60.0f) < 0.001f);
    REQUIRE(ResolveStableProjectionFov(
        0x3000, 0x4000, 4, true, 100.0f, true, 100.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 100.0f) < 0.001f);
    REQUIRE(ResolveStableProjectionFov(
        0x3000, 0x4000, 5, false, 0.0f, false, 0.0f,
        fovState, resolvedFov));
    REQUIRE(std::fabs(resolvedFov - 100.0f) < 0.001f);

    ProjectionView view{};
    view.fieldOfView = 90.0f;
    const ProjectionPoint target{100.0f, 0.0f, 0.0f};
    const ScreenProjection centered =
        ProjectWorldPoint(target, view, 2400, 1080);
    REQUIRE(centered.valid);
    REQUIRE(std::fabs(centered.x - 1200.0f) < 0.01f);
    REQUIRE(std::fabs(centered.y - 540.0f) < 0.01f);

    ProjectionView latest = view;
    latest.location.y = 10.0f;
    const ScreenProjection refreshed =
        ProjectWorldPoint(target, latest, 2400, 1080);
    REQUIRE(refreshed.valid);
    REQUIRE(refreshed.x < centered.x);

    latest.rotation.yaw = 180.0f;
    REQUIRE(!ProjectWorldPoint(target, latest, 2400, 1080).valid);

    ProjectionView rolled = view;
    rolled.rotation.roll = 90.0f;
    const ScreenProjection rollRotated = ProjectWorldPoint(
        {100.0f, 0.0f, -10.0f}, rolled, 2400, 1080);
    REQUIRE(rollRotated.valid);
    REQUIRE(rollRotated.x > 1200.0f);
    REQUIRE(std::fabs(rollRotated.y - 540.0f) < 0.01f);

    const auto completeRing = ProjectWorldHorizontalRing(
        {100.0f, 0.0f, 0.0f}, 10.0f, view, 2400, 1080);
    REQUIRE(completeRing.size() == 36);
    for (std::size_t index = 0; index < completeRing.size(); ++index) {
        REQUIRE(completeRing[index].start.valid);
        REQUIRE(completeRing[index].end.valid);
        const ScreenProjection& nextStart =
            completeRing[(index + 1) % completeRing.size()].start;
        REQUIRE(std::fabs(completeRing[index].end.x - nextStart.x) < 0.01f);
        REQUIRE(std::fabs(completeRing[index].end.y - nextStart.y) < 0.01f);
    }

    constexpr std::size_t kRingSegmentCount = 36;
    constexpr float kTwoPi = 6.28318530717958647692f;
    std::array<ScreenProjection, kRingSegmentCount> partialPoints{};
    for (std::size_t index = 0; index < partialPoints.size(); ++index) {
        const float angle = kTwoPi * static_cast<float>(index) /
            static_cast<float>(partialPoints.size());
        partialPoints[index] = ProjectWorldPoint(
            {10.0f * std::cos(angle), 10.0f * std::sin(angle), 0.0f},
            view,
            2400,
            1080);
    }
    std::size_t expectedPartialSegments = 0;
    for (std::size_t index = 0; index < partialPoints.size(); ++index) {
        if (partialPoints[index].valid &&
            partialPoints[(index + 1) % partialPoints.size()].valid) {
            ++expectedPartialSegments;
        }
    }
    const auto partialRing = ProjectWorldHorizontalRing(
        {0.0f, 0.0f, 0.0f}, 10.0f, view, 2400, 1080);
    REQUIRE(expectedPartialSegments > 0);
    REQUIRE(expectedPartialSegments < kRingSegmentCount);
    REQUIRE(partialRing.size() == expectedPartialSegments);
    for (const ScreenProjectionSegment& segment : partialRing) {
        bool matchesAdjacentPoints = false;
        for (std::size_t index = 0; index < partialPoints.size(); ++index) {
            const ScreenProjection& start = partialPoints[index];
            const ScreenProjection& end =
                partialPoints[(index + 1) % partialPoints.size()];
            if (!start.valid || !end.valid) continue;
            if (std::fabs(segment.start.x - start.x) < 0.01f &&
                std::fabs(segment.start.y - start.y) < 0.01f &&
                std::fabs(segment.end.x - end.x) < 0.01f &&
                std::fabs(segment.end.y - end.y) < 0.01f) {
                matchesAdjacentPoints = true;
                break;
            }
        }
        REQUIRE(matchesAdjacentPoints);
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    REQUIRE(ProjectWorldHorizontalRing(
        {100.0f, 0.0f, 0.0f}, 0.0f, view, 2400, 1080).empty());
    REQUIRE(ProjectWorldHorizontalRing(
        {100.0f, 0.0f, 0.0f}, -1.0f, view, 2400, 1080).empty());
    REQUIRE(ProjectWorldHorizontalRing(
        {100.0f, 0.0f, 0.0f}, nan, view, 2400, 1080).empty());
    REQUIRE(ProjectWorldHorizontalRing(
        {100.0f, 0.0f, 0.0f}, infinity, view, 2400, 1080).empty());
    REQUIRE(ProjectWorldHorizontalRing(
        {nan, 0.0f, 0.0f}, 10.0f, view, 2400, 1080).empty());
}
