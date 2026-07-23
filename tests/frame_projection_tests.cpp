#include "game/native/FrameProjection.h"
#include "test_support.h"

#include <array>
#include <cmath>
#include <limits>

void RunFrameProjectionTests() {
    using namespace lengjing::game::native;

    const auto requireProjectionEquivalent = [](
        const ScreenProjection& left,
        const ScreenProjection& right) {
        REQUIRE(left.valid == right.valid);
        REQUIRE(std::fabs(left.x - right.x) < 0.001f);
        REQUIRE(std::fabs(left.y - right.y) < 0.001f);
        REQUIRE(std::fabs(left.camera.side - right.camera.side) < 0.001f);
        REQUIRE(std::fabs(
            left.camera.vertical - right.camera.vertical) < 0.001f);
        REQUIRE(std::fabs(
            left.camera.forward - right.camera.forward) < 0.001f);
    };

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
    const PreparedProjection prepared =
        PrepareProjection(view, 2400, 1080);
    REQUIRE(prepared.basisValid);
    REQUIRE(prepared.valid);
    const ProjectionPoint target{100.0f, 0.0f, 0.0f};
    const ScreenProjection centered =
        ProjectWorldPoint(target, view, 2400, 1080);
    requireProjectionEquivalent(
        centered, ProjectWorldPoint(target, prepared));
    REQUIRE(centered.valid);
    REQUIRE(std::fabs(centered.x - 1200.0f) < 0.01f);
    REQUIRE(std::fabs(centered.y - 540.0f) < 0.01f);

    const ScreenProjection offset = ProjectWorldPoint(
        {100.0f, 50.0f, 25.0f}, prepared);
    REQUIRE(offset.valid);
    REQUIRE(std::fabs(offset.camera.side - 50.0f) < 0.01f);
    REQUIRE(std::fabs(offset.camera.vertical - 25.0f) < 0.01f);
    REQUIRE(std::fabs(offset.camera.forward - 100.0f) < 0.01f);
    REQUIRE(std::fabs(offset.x - 1800.0f) < 0.01f);
    REQUIRE(std::fabs(offset.y - 240.0f) < 0.01f);

    const CameraSpacePoint legacyCamera = ToCameraSpace(target, view);
    const CameraSpacePoint preparedCamera = ToCameraSpace(target, prepared);
    REQUIRE(std::fabs(legacyCamera.side - preparedCamera.side) < 0.001f);
    REQUIRE(std::fabs(
        legacyCamera.vertical - preparedCamera.vertical) < 0.001f);
    REQUIRE(std::fabs(
        legacyCamera.forward - preparedCamera.forward) < 0.001f);

    const std::array<ProjectionView, 4> equivalenceViews{{
        view,
        ProjectionView{
            {12.0f, -25.0f, 8.0f},
            {15.0f, 42.0f, 0.0f},
            70.0f},
        ProjectionView{
            {-300.0f, 120.0f, 45.0f},
            {-20.0f, 135.0f, 18.0f},
            110.0f},
        ProjectionView{
            {8.0f, 9.0f, 10.0f},
            {0.0f, 270.0f, -45.0f},
            55.0f},
    }};
    const std::array<ProjectionPoint, 5> equivalencePoints{{
        {100.0f, 0.0f, 0.0f},
        {-100.0f, 0.0f, 0.0f},
        {500.0f, 120.0f, -80.0f},
        {-350.0f, 800.0f, 240.0f},
        {12.5f, -27.25f, 9.75f},
    }};
    for (const ProjectionView& candidateView : equivalenceViews) {
        const PreparedProjection candidatePrepared =
            PrepareProjection(candidateView, 2400, 1080);
        REQUIRE(candidatePrepared.basisValid);
        REQUIRE(candidatePrepared.valid);
        for (const ProjectionPoint& point : equivalencePoints) {
            requireProjectionEquivalent(
                ProjectWorldPoint(point, candidateView, 2400, 1080),
                ProjectWorldPoint(point, candidatePrepared));
        }
    }

    ProjectionView latest = view;
    latest.location.y = 10.0f;
    const ScreenProjection refreshed =
        ProjectWorldPoint(target, latest, 2400, 1080);
    const PreparedProjection refreshedPrepared =
        PrepareProjection(latest, 2400, 1080);
    requireProjectionEquivalent(
        refreshed, ProjectWorldPoint(target, refreshedPrepared));
    REQUIRE(refreshed.valid);
    REQUIRE(refreshed.x < centered.x);
    REQUIRE(std::fabs(
        ProjectWorldPoint(target, prepared).x -
        ProjectWorldPoint(target, refreshedPrepared).x) > 0.001f);

    latest.rotation.yaw = 180.0f;
    const ScreenProjection refreshedBehind =
        ProjectWorldPoint(target, latest, 2400, 1080);
    const PreparedProjection refreshedBehindPrepared =
        PrepareProjection(latest, 2400, 1080);
    requireProjectionEquivalent(
        refreshedBehind,
        ProjectWorldPoint(target, refreshedBehindPrepared));
    REQUIRE(!refreshedBehind.valid);

    ProjectionView zeroFov = view;
    zeroFov.fieldOfView = 0.0f;
    const PreparedProjection zeroFovPrepared =
        PrepareProjection(zeroFov, 2400, 1080);
    REQUIRE(zeroFovPrepared.basisValid);
    REQUIRE(!zeroFovPrepared.valid);
    const ScreenProjection zeroFovLegacy =
        ProjectWorldPoint(target, zeroFov, 2400, 1080);
    const ScreenProjection zeroFovProjected =
        ProjectWorldPoint(target, zeroFovPrepared);
    requireProjectionEquivalent(zeroFovLegacy, zeroFovProjected);
    REQUIRE(!zeroFovLegacy.valid);
    REQUIRE(zeroFovLegacy.camera.forward > 0.01f);

    REQUIRE(!PrepareProjection(view, 1, 1080).basisValid);
    REQUIRE(!PrepareProjection(view, 2400, 1).basisValid);
    ProjectionView invalidView = view;
    invalidView.rotation.pitch =
        std::numeric_limits<float>::quiet_NaN();
    REQUIRE(!PrepareProjection(invalidView, 2400, 1080).basisValid);

    ProjectionView rolled = view;
    rolled.rotation.roll = 90.0f;
    const ScreenProjection rollRotated = ProjectWorldPoint(
        {100.0f, 0.0f, -10.0f}, rolled, 2400, 1080);
    REQUIRE(rollRotated.valid);
    REQUIRE(rollRotated.x > 1200.0f);
    REQUIRE(std::fabs(rollRotated.y - 540.0f) < 0.01f);

    const auto completeRing = ProjectWorldHorizontalRing(
        {100.0f, 0.0f, 0.0f}, 10.0f, view, 2400, 1080);
    const auto preparedCompleteRing = ProjectWorldHorizontalRing(
        {100.0f, 0.0f, 0.0f}, 10.0f, prepared);
    REQUIRE(preparedCompleteRing.size() == completeRing.size());
    REQUIRE(completeRing.size() == 36);
    for (std::size_t index = 0; index < completeRing.size(); ++index) {
        requireProjectionEquivalent(
            completeRing[index].start,
            preparedCompleteRing[index].start);
        requireProjectionEquivalent(
            completeRing[index].end,
            preparedCompleteRing[index].end);
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
