#include "test_support.h"

#include "game/native/CoordinateOutputPolicy.h"

#include <chrono>

void RunCoordinateOutputPolicyTests() {
    using namespace std::chrono_literals;
    using lengjing::game::native::CanRetainDecodedPosition;
    using lengjing::game::native::CanUseDecodedPositionHistory;
    using lengjing::game::native::CharacterCoordinateDisposition;
    using lengjing::game::native::ClassifyDecodedPositionCacheIdentity;
    using lengjing::game::native::CoordinateFailureRecoveryDelay;
    using lengjing::game::native::DecodedPositionCacheIdentity;
    using lengjing::game::native::DecodedPositionCacheIdentityState;
    using lengjing::game::native::DecodedPositionClock;
    using lengjing::game::native::HasCoordinateFailureRecoveryElapsed;
    using lengjing::game::native::IsDecodedPositionCacheOwnerMatch;
    using lengjing::game::native::IsCoordinateFrameHealthy;
    using lengjing::game::native::ShouldDiscardDecodedPositionCache;
    using lengjing::game::native::ShouldBlockStandardCoordinateFallback;
    using lengjing::game::native::ShouldBlockStandardCoordinateFallbackForBuild;
    using lengjing::game::native::ShouldReadAlgorithmCoordinate;
    using lengjing::game::native::ShouldReadAlgorithmCoordinateForBuild;
    using lengjing::game::native::ShouldEscalateDecodedPositionFailure;
    using lengjing::game::native::ShouldKeepDecodedPositionSource;
    using lengjing::game::native::ShouldReportCoordinateOutputError;
    using lengjing::game::native::ResolveCharacterCoordinateDisposition;

    REQUIRE(!ShouldKeepDecodedPositionSource(false, true));
    REQUIRE(!ShouldKeepDecodedPositionSource(true, false));
    REQUIRE(ShouldKeepDecodedPositionSource(true, true));

    REQUIRE(!IsCoordinateFrameHealthy(0, 0));
    REQUIRE(IsCoordinateFrameHealthy(20, 1));
    REQUIRE(IsCoordinateFrameHealthy(5, 2));
    REQUIRE(IsCoordinateFrameHealthy(5, 4));
    REQUIRE(IsCoordinateFrameHealthy(20, 17));
    REQUIRE(IsCoordinateFrameHealthy(20, 18));
    REQUIRE(IsCoordinateFrameHealthy(20, 19));
    REQUIRE(IsCoordinateFrameHealthy(20, 20));
    REQUIRE(IsCoordinateFrameHealthy(20, 19, true));
    REQUIRE(ShouldReportCoordinateOutputError(false, false));
    REQUIRE(!ShouldReportCoordinateOutputError(true, false));
    REQUIRE(!ShouldReportCoordinateOutputError(false, true));
    REQUIRE(!ShouldReportCoordinateOutputError(true, true));

    constexpr std::uintptr_t world = 0x1000;
    constexpr std::uintptr_t actor = 0x2000;
    constexpr std::uintptr_t coordinateIdentity = 0x3000;
    const DecodedPositionClock::time_point capturedAt{};
    constexpr DecodedPositionCacheIdentity cacheIdentity{
        world,
        actor,
        coordinateIdentity,
    };
    REQUIRE(ClassifyDecodedPositionCacheIdentity(
        cacheIdentity, cacheIdentity) ==
        DecodedPositionCacheIdentityState::Match);
    REQUIRE(ClassifyDecodedPositionCacheIdentity(
        cacheIdentity,
        DecodedPositionCacheIdentity{world, 0x2100, coordinateIdentity}) ==
        DecodedPositionCacheIdentityState::Mismatch);
    REQUIRE(ClassifyDecodedPositionCacheIdentity(
        cacheIdentity,
        DecodedPositionCacheIdentity{world, actor, 0x4000}) ==
        DecodedPositionCacheIdentityState::Mismatch);
    REQUIRE(ClassifyDecodedPositionCacheIdentity(
        cacheIdentity,
        DecodedPositionCacheIdentity{0x1100, actor, coordinateIdentity}) ==
        DecodedPositionCacheIdentityState::Mismatch);
    REQUIRE(ClassifyDecodedPositionCacheIdentity(
        cacheIdentity,
        DecodedPositionCacheIdentity{world, actor, 0}) ==
        DecodedPositionCacheIdentityState::Unknown);
    REQUIRE(IsDecodedPositionCacheOwnerMatch(
        cacheIdentity,
        DecodedPositionCacheIdentity{world, actor, 0}));
    REQUIRE(!IsDecodedPositionCacheOwnerMatch(
        cacheIdentity,
        DecodedPositionCacheIdentity{world, 0x2100, 0}));
    REQUIRE(ClassifyDecodedPositionCacheIdentity(
        cacheIdentity,
        DecodedPositionCacheIdentity{world, 0x2100, 0}) ==
        DecodedPositionCacheIdentityState::Mismatch);
    REQUIRE(!ShouldDiscardDecodedPositionCache(
        DecodedPositionCacheIdentityState::Unknown));
    REQUIRE(!ShouldDiscardDecodedPositionCache(
        DecodedPositionCacheIdentityState::Match));
    REQUIRE(ShouldDiscardDecodedPositionCache(
        DecodedPositionCacheIdentityState::Mismatch));
    REQUIRE(CanUseDecodedPositionHistory(
        DecodedPositionCacheIdentityState::Match,
        capturedAt,
        capturedAt + 2s));
    REQUIRE(!CanUseDecodedPositionHistory(
        DecodedPositionCacheIdentityState::Match,
        capturedAt,
        capturedAt + 24h));
    REQUIRE(!CanUseDecodedPositionHistory(
        DecodedPositionCacheIdentityState::Mismatch,
        capturedAt,
        capturedAt + 24h));
    REQUIRE(!CanUseDecodedPositionHistory(
        DecodedPositionCacheIdentityState::Match,
        capturedAt + 1s,
        capturedAt));
    REQUIRE(!ShouldReadAlgorithmCoordinate(false, true));
    REQUIRE(!ShouldReadAlgorithmCoordinate(true, true));
    REQUIRE(!ShouldReadAlgorithmCoordinate(true, false));
    REQUIRE(!ShouldReadAlgorithmCoordinate(false, false));
    REQUIRE(!ShouldBlockStandardCoordinateFallback(false, true, false));
    REQUIRE(!ShouldBlockStandardCoordinateFallback(true, true, false));
    REQUIRE(!ShouldBlockStandardCoordinateFallback(false, true, true));
    REQUIRE(!ShouldBlockStandardCoordinateFallback(true, false, false));
    REQUIRE(!ShouldReadAlgorithmCoordinateForBuild(false, false, true));
    REQUIRE(!ShouldReadAlgorithmCoordinateForBuild(false, true, true));
    REQUIRE(ShouldReadAlgorithmCoordinateForBuild(true, false, true));
    REQUIRE(ShouldReadAlgorithmCoordinateForBuild(true, true, true));
    REQUIRE(!ShouldReadAlgorithmCoordinateForBuild(true, true, false));
    REQUIRE(ShouldBlockStandardCoordinateFallbackForBuild(
        true, false, true, false));
    REQUIRE(ShouldBlockStandardCoordinateFallbackForBuild(
        true, true, true, false));
    REQUIRE(!ShouldBlockStandardCoordinateFallbackForBuild(
        true, true, true, true));
    REQUIRE(!ShouldBlockStandardCoordinateFallbackForBuild(
        false, true, true, false));
    REQUIRE(ResolveCharacterCoordinateDisposition(false, false) ==
        CharacterCoordinateDisposition::ContinueExisting);
    REQUIRE(ResolveCharacterCoordinateDisposition(false, true) ==
        CharacterCoordinateDisposition::ContinueExisting);
    REQUIRE(ResolveCharacterCoordinateDisposition(true, false) ==
        CharacterCoordinateDisposition::ReturnUnavailable);
    REQUIRE(ResolveCharacterCoordinateDisposition(true, true) ==
        CharacterCoordinateDisposition::ReturnAlgorithm);
    REQUIRE(CanRetainDecodedPosition(
        true,
        cacheIdentity,
        cacheIdentity,
        capturedAt,
        capturedAt + 3s));
    REQUIRE(!CanRetainDecodedPosition(
        true,
        cacheIdentity,
        cacheIdentity,
        capturedAt,
        capturedAt + 3001ms));
    REQUIRE(!CanRetainDecodedPosition(
        false,
        cacheIdentity,
        cacheIdentity,
        capturedAt,
        capturedAt + 1s));
    REQUIRE(!CanRetainDecodedPosition(
        true,
        cacheIdentity,
        DecodedPositionCacheIdentity{0x1100, actor, coordinateIdentity},
        capturedAt,
        capturedAt + 1s));
    REQUIRE(!CanRetainDecodedPosition(
        true,
        cacheIdentity,
        DecodedPositionCacheIdentity{world, actor, 0x4000},
        capturedAt,
        capturedAt + 1s));
    REQUIRE(!CanRetainDecodedPosition(
        true,
        cacheIdentity,
        cacheIdentity,
        capturedAt + 1s,
        capturedAt));

    REQUIRE(!ShouldEscalateDecodedPositionFailure(
        capturedAt, capturedAt + 2499ms));
    REQUIRE(ShouldEscalateDecodedPositionFailure(
        capturedAt, capturedAt + 2500ms));
    REQUIRE(IsCoordinateFrameHealthy(20, 18, true));
    REQUIRE(ShouldEscalateDecodedPositionFailure(
        capturedAt, capturedAt + 3s));
    REQUIRE(!ShouldEscalateDecodedPositionFailure(
        capturedAt + 1s, capturedAt));
    REQUIRE(CoordinateFailureRecoveryDelay(false) == 2s);
    REQUIRE(CoordinateFailureRecoveryDelay(true) == 250ms);
    const auto agedFailureSince = capturedAt + 2500ms;
    REQUIRE(!HasCoordinateFailureRecoveryElapsed(
        DecodedPositionClock::time_point{}, capturedAt + 3s, true));
    REQUIRE(!HasCoordinateFailureRecoveryElapsed(
        agedFailureSince, capturedAt + 2749ms, true));
    REQUIRE(HasCoordinateFailureRecoveryElapsed(
        agedFailureSince, capturedAt + 2750ms, true));
    REQUIRE(capturedAt + 2750ms < capturedAt + 3s);
}
