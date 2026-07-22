#include "game/native/CoordinatePoolPolicy.h"
#include "game/native/CoordinatePoolRuntime.h"
#include "test_support.h"

#include <array>

void RunCoordinatePoolPolicyTests() {
    using lengjing::game::native::CoordinatePoolCodeFingerprint;
    using lengjing::game::native::CoordinatePoolCodeIdentityChanged;
    using lengjing::game::native::CoordinatePoolContextIdentityChanged;
    using lengjing::game::native::CoordinatePoolEnvironmentFlagEnabled;
    using lengjing::game::native::CoordinatePoolRingReadEvent;
    using lengjing::game::native::CoordinatePoolRingRecoveryState;
    using lengjing::game::native::CoordinatePoolRingSearchBudget;
    using lengjing::game::native::CoordinatePoolRootSnapshot;
    using lengjing::game::native::CoordinatePoolRootStabilityWindow;
    using lengjing::game::native::CoordinatePoolRootSnapshotsMatch;
    using lengjing::game::native::CoordinatePoolCandidateSet;
    using lengjing::game::native::IsCoordinatePoolReadRangeValid;
    using lengjing::game::native::IsCoordinatePoolSelectedCandidateValid;
    using lengjing::game::native::MapDecodedCoordinatePoolSlot;
    using lengjing::game::native::NextCoordinatePoolCodeValidationFrame;
    using lengjing::game::native::NormalizeCoordinatePoolPointer;
    using lengjing::game::native::
        ShouldClearCoordinatePoolRingsAfterPointerRefresh;
    using lengjing::game::native::ShouldSearchCoordinatePoolRing;
    using lengjing::game::native::
        ShouldRequestCoordinatePoolCodeValidationAfterReadFailure;
    using lengjing::game::native::ShouldRetryCoordinatePoolRing;
    using lengjing::game::native::ShouldRetryCoordinatePoolCompactSnapshot;
    using lengjing::game::native::ShouldValidateCoordinatePoolCode;
    using lengjing::game::native::kCoordinatePoolCodeValidationIdleFrame;
    using lengjing::game::native::kCoordinatePoolCodeValidationRetryFrames;
    using lengjing::game::native::kCoordinatePoolRingRetryFrames;
    using lengjing::game::native::kCoordinatePoolRingReadFailureThreshold;
    using lengjing::game::native::kCoordinatePoolRingSearchesPerFrame;
    using lengjing::game::native::kCoordinatePoolMaximumRemoteAddress;
    using lengjing::game::native::kCoordinatePoolMinimumRemoteAddress;
    REQUIRE(!CoordinatePoolEnvironmentFlagEnabled(nullptr));
    REQUIRE(!CoordinatePoolEnvironmentFlagEnabled(""));
    REQUIRE(!CoordinatePoolEnvironmentFlagEnabled("0"));
    REQUIRE(CoordinatePoolEnvironmentFlagEnabled("1"));
    REQUIRE(CoordinatePoolEnvironmentFlagEnabled("1-full"));

    REQUIRE(!IsCoordinatePoolReadRangeValid(
        kCoordinatePoolMinimumRemoteAddress, 0));
    REQUIRE(!IsCoordinatePoolReadRangeValid(
        kCoordinatePoolMinimumRemoteAddress - 1, 1));
    REQUIRE(IsCoordinatePoolReadRangeValid(
        kCoordinatePoolMinimumRemoteAddress, 1));
    REQUIRE(IsCoordinatePoolReadRangeValid(
        kCoordinatePoolMaximumRemoteAddress - 12, 12));
    REQUIRE(!IsCoordinatePoolReadRangeValid(
        kCoordinatePoolMaximumRemoteAddress - 11, 12));
    REQUIRE(!IsCoordinatePoolReadRangeValid(
        kCoordinatePoolMaximumRemoteAddress, 1));
    REQUIRE(!IsCoordinatePoolReadRangeValid(UINT64_MAX, 1));

    CoordinatePoolCandidateSet candidates{};
    REQUIRE(!IsCoordinatePoolSelectedCandidateValid(candidates));
    candidates.valid[3] = true;
    REQUIRE(!IsCoordinatePoolSelectedCandidateValid(candidates));
    candidates.selectedLogicalSlot = 3;
    REQUIRE(IsCoordinatePoolSelectedCandidateValid(candidates));
    candidates.selectedLogicalSlot =
        static_cast<std::uint8_t>(candidates.valid.size());
    REQUIRE(!IsCoordinatePoolSelectedCandidateValid(candidates));

    using lengjing::game::native::kCoordinatePoolCompactLayout;
    using lengjing::game::native::kCoordinatePoolExtendedLayout;
    REQUIRE(MapDecodedCoordinatePoolSlot(0, kCoordinatePoolCompactLayout) ==
        5);
    REQUIRE(MapDecodedCoordinatePoolSlot(4, kCoordinatePoolCompactLayout) ==
        9);
    REQUIRE(MapDecodedCoordinatePoolSlot(5, kCoordinatePoolCompactLayout) ==
        0);
    REQUIRE(MapDecodedCoordinatePoolSlot(9, kCoordinatePoolCompactLayout) ==
        4);
    REQUIRE(MapDecodedCoordinatePoolSlot(10, kCoordinatePoolCompactLayout) ==
        14);
    REQUIRE(MapDecodedCoordinatePoolSlot(0, kCoordinatePoolExtendedLayout) ==
        7);
    REQUIRE(MapDecodedCoordinatePoolSlot(6, kCoordinatePoolExtendedLayout) ==
        13);
    REQUIRE(MapDecodedCoordinatePoolSlot(7, kCoordinatePoolExtendedLayout) ==
        0);
    REQUIRE(MapDecodedCoordinatePoolSlot(13, kCoordinatePoolExtendedLayout) ==
        6);
    REQUIRE(MapDecodedCoordinatePoolSlot(14, kCoordinatePoolExtendedLayout) ==
        14);

    using lengjing::game::native::CoordinatePoolSlotLayoutCalibration;
    using lengjing::game::native::CoordinatePoolSlotLayoutKind;
    CoordinatePoolSlotLayoutCalibration extendedCalibration;
    REQUIRE(extendedCalibration.ObserveDecodedSlot(10).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(extendedCalibration.CompactPhaseMask() == 0);
    REQUIRE(extendedCalibration.ObserveTransition(
        0x1000, 1, 2, 1, 6, 0x0210).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(extendedCalibration.ObserveTransition(
        0x2000, 3, 4, 12, 3, 0x0042).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(extendedCalibration.ObserveTransition(
        0x1000, 5, 6, 7, 0, 0x0408).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(extendedCalibration.Layout().physicalSlotCount == 14);
    REQUIRE(extendedCalibration.Layout().phase == 3);
    REQUIRE(MapDecodedCoordinatePoolSlot(
        10, extendedCalibration.Layout()) == 13);

    CoordinatePoolSlotLayoutCalibration noisyExtendedCalibration;
    REQUIRE(noisyExtendedCalibration.ObserveDecodedSlot(10).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(noisyExtendedCalibration.ObserveTransition(
        0x3000, 1, 2, 1, 6, 0x0840).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(noisyExtendedCalibration.ObserveTransition(
        0x1000, 3, 4, 1, 6, 0x0210).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(noisyExtendedCalibration.ObserveTransition(
        0x2000, 5, 6, 12, 3, 0x0042).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(noisyExtendedCalibration.ObserveTransition(
        0x1000, 7, 8, 7, 0, 0x0408).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(noisyExtendedCalibration.Layout().phase == 3);

    CoordinatePoolSlotLayoutCalibration singleComponentCalibration;
    REQUIRE(singleComponentCalibration.ObserveDecodedSlot(10).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    for (std::uint64_t index = 0; index < 5; ++index) {
        REQUIRE(singleComponentCalibration.ObserveTransition(
            0x1000,
            index * 2 + 1,
            index * 2 + 2,
            1,
            6,
            0x0210).kind == CoordinatePoolSlotLayoutKind::Unknown);
    }
    REQUIRE(singleComponentCalibration.ObserveTransition(
        0x1000, 11, 12, 12, 3, 0x0042).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(singleComponentCalibration.Layout().phase == 3);

    CoordinatePoolSlotLayoutCalibration legacyExtendedCalibration;
    REQUIRE(legacyExtendedCalibration.ObserveDecodedSlot(10).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(legacyExtendedCalibration.ObserveTransition(
        0x1000, 1, 2, 1, 6, 0x2100).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(legacyExtendedCalibration.ObserveTransition(
        0x2000, 3, 4, 12, 3, 0x0420).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(legacyExtendedCalibration.ObserveTransition(
        0x1000, 5, 6, 7, 0, 0x0081).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(legacyExtendedCalibration.Layout().physicalSlotCount == 14);
    REQUIRE(legacyExtendedCalibration.Layout().phase == 7);
    REQUIRE(MapDecodedCoordinatePoolSlot(
        10, legacyExtendedCalibration.Layout()) == 3);

    CoordinatePoolSlotLayoutCalibration compactCalibration;
    REQUIRE(compactCalibration.ObserveTransition(
        0x1000, 1, 2, 5, 4, 0x0201).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(compactCalibration.ObserveTransition(
        0x2000, 3, 4, 8, 7, 0x000c).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(compactCalibration.ObserveTransition(
        0x1000, 5, 6, 2, 3, 0x0180).kind ==
        CoordinatePoolSlotLayoutKind::Compact);
    REQUIRE(compactCalibration.Layout().physicalSlotCount == 10);
    REQUIRE(compactCalibration.Layout().logicalSlotCount == 5);
    REQUIRE(compactCalibration.Layout().phase == 5);
    REQUIRE(compactCalibration.EvidenceCount(
        CoordinatePoolSlotLayoutKind::Compact) == 3);
    REQUIRE(compactCalibration.ComponentCount(
        CoordinatePoolSlotLayoutKind::Compact) == 2);

    CoordinatePoolSlotLayoutCalibration noisyCompactCalibration;
    REQUIRE(noisyCompactCalibration.ObserveTransition(
        0x1000, 1, 2, 5, 4, 0x0e01).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(noisyCompactCalibration.ObserveTransition(
        0x2000, 3, 4, 8, 7, 0x300c).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(noisyCompactCalibration.ObserveTransition(
        0x1000, 5, 6, 2, 3, 0x0d80).kind ==
        CoordinatePoolSlotLayoutKind::Compact);
    REQUIRE(noisyCompactCalibration.Layout().physicalSlotCount == 10);
    REQUIRE(noisyCompactCalibration.Layout().phase == 5);
    REQUIRE(noisyCompactCalibration.EvidenceCount(
        CoordinatePoolSlotLayoutKind::Compact) == 3);
    REQUIRE(noisyCompactCalibration.EvidenceCount(
        CoordinatePoolSlotLayoutKind::Extended) == 0);
    REQUIRE(noisyCompactCalibration.ComponentCount(
        CoordinatePoolSlotLayoutKind::Compact) == 2);

    REQUIRE(ShouldRetryCoordinatePoolCompactSnapshot(
        {}, true, 9, 14, false));
    REQUIRE(!ShouldRetryCoordinatePoolCompactSnapshot(
        {}, false, 9, 14, false));
    REQUIRE(!ShouldRetryCoordinatePoolCompactSnapshot(
        {}, true, 10, 14, false));
    REQUIRE(!ShouldRetryCoordinatePoolCompactSnapshot(
        {}, true, 9, 10, false));
    REQUIRE(!ShouldRetryCoordinatePoolCompactSnapshot(
        {}, true, 9, 14, true));
    REQUIRE(!ShouldRetryCoordinatePoolCompactSnapshot(
        kCoordinatePoolCompactLayout, true, 9, 14, false));

    REQUIRE(!ShouldRetryCoordinatePoolRing(
        100, 100 + kCoordinatePoolRingRetryFrames - 1));
    REQUIRE(ShouldRetryCoordinatePoolRing(
        100, 100 + kCoordinatePoolRingRetryFrames));
    REQUIRE(ShouldRetryCoordinatePoolRing(100, 99));
    REQUIRE(ShouldSearchCoordinatePoolRing(false, false, 0, 100));
    REQUIRE(!ShouldSearchCoordinatePoolRing(true, true, 1, UINT64_MAX));
    REQUIRE(!ShouldSearchCoordinatePoolRing(
        true, false, 100, 100 + kCoordinatePoolRingRetryFrames - 1));
    REQUIRE(ShouldSearchCoordinatePoolRing(
        true, false, 100, 100 + kCoordinatePoolRingRetryFrames));

    REQUIRE(kCoordinatePoolRingReadFailureThreshold == 2);
    CoordinatePoolRingRecoveryState recovery;
    REQUIRE(recovery.Failures() == 0);
    REQUIRE(!recovery.Observe(CoordinatePoolRingReadEvent::RemoteReadFailure));
    REQUIRE(recovery.Failures() == 1);
    REQUIRE(!recovery.Observe(CoordinatePoolRingReadEvent::OtherFailure));
    REQUIRE(recovery.Failures() == 1);
    REQUIRE(!recovery.Observe(CoordinatePoolRingReadEvent::Success));
    REQUIRE(recovery.Failures() == 0);
    REQUIRE(!recovery.Observe(CoordinatePoolRingReadEvent::RemoteReadFailure));
    REQUIRE(recovery.Observe(CoordinatePoolRingReadEvent::RemoteReadFailure));
    REQUIRE(recovery.Failures() == kCoordinatePoolRingReadFailureThreshold);
    recovery.Reset();
    REQUIRE(recovery.Failures() == 0);

    CoordinatePoolRingSearchBudget budget;
    for (std::size_t index = 0;
         index < kCoordinatePoolRingSearchesPerFrame;
         ++index) {
        REQUIRE(budget.TryConsume(10));
    }
    REQUIRE(!budget.TryConsume(10));
    REQUIRE(budget.TryConsume(11));
    budget.Reset();
    REQUIRE(budget.TryConsume(11));

    constexpr CoordinatePoolRootSnapshot root{
        0x10000000,
        0x20000000,
        0x30000000,
    };
    REQUIRE(CoordinatePoolRootSnapshotsMatch(root, root));
    REQUIRE(!CoordinatePoolRootSnapshotsMatch(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge + 8,
            root.context,
            root.entry,
        }));
    REQUIRE(!CoordinatePoolCodeIdentityChanged(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge + 8,
            root.context + 8,
            root.entry,
        }));
    REQUIRE(CoordinatePoolContextIdentityChanged(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge + 8,
            root.context,
            root.entry,
        }));
    REQUIRE(CoordinatePoolCodeIdentityChanged(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge,
            root.context,
            root.entry + 4,
        }));
    REQUIRE(!CoordinatePoolRootSnapshotsMatch(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge,
            root.context + 8,
            root.entry,
        }));
    REQUIRE(!CoordinatePoolRootSnapshotsMatch(
        root,
        CoordinatePoolRootSnapshot{
            root.bridge,
            root.context,
            root.entry + 4,
        }));

    CoordinatePoolRootStabilityWindow stability;
    REQUIRE(!stability.Observe(root));
    REQUIRE(!stability.Observe(CoordinatePoolRootSnapshot{
        root.bridge,
        root.context,
        root.entry + 4,
    }));
    REQUIRE(stability.Observe(CoordinatePoolRootSnapshot{
        root.bridge,
        root.context,
        root.entry + 4,
    }));
    stability.Reset();
    REQUIRE(!stability.Observe(root));
    REQUIRE(stability.Observe(root));

    std::array<std::uint8_t, 4096> codePage{};
    const std::uint64_t zeroFingerprint =
        CoordinatePoolCodeFingerprint(codePage.data(), codePage.size());
    const std::uint64_t codeRangeFingerprint =
        CoordinatePoolCodeFingerprint(codePage.data() + 128, 64);
    REQUIRE(zeroFingerprint ==
        CoordinatePoolCodeFingerprint(codePage.data(), codePage.size()));
    codePage[2048] = 1;
    REQUIRE(zeroFingerprint !=
        CoordinatePoolCodeFingerprint(codePage.data(), codePage.size()));
    REQUIRE(codeRangeFingerprint ==
        CoordinatePoolCodeFingerprint(codePage.data() + 128, 64));
    codePage[160] = 1;
    REQUIRE(codeRangeFingerprint !=
        CoordinatePoolCodeFingerprint(codePage.data() + 128, 64));

    constexpr std::uint64_t pointer = UINT64_C(0x0000007123456780);
    REQUIRE(NormalizeCoordinatePoolPointer(
        UINT64_C(0xABCD007123456780)) == pointer);
    REQUIRE(NormalizeCoordinatePoolPointer(pointer) == pointer);
    REQUIRE(!ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        true,
        NormalizeCoordinatePoolPointer(UINT64_C(0xABCD007123456780)),
        NormalizeCoordinatePoolPointer(UINT64_C(0x1234007123456780))));

    REQUIRE(ShouldValidateCoordinatePoolCode(100, 100, false));
    REQUIRE(!ShouldValidateCoordinatePoolCode(100, 101, false));
    REQUIRE(ShouldValidateCoordinatePoolCode(100, 101, true));
    REQUIRE(ShouldValidateCoordinatePoolCode(5, 1000, false));
    REQUIRE(!ShouldValidateCoordinatePoolCode(
        100, kCoordinatePoolCodeValidationIdleFrame, false));
    REQUIRE(!ShouldValidateCoordinatePoolCode(
        160, kCoordinatePoolCodeValidationIdleFrame, false));
    REQUIRE(!ShouldValidateCoordinatePoolCode(
        UINT64_MAX - 1, kCoordinatePoolCodeValidationIdleFrame, false));
    REQUIRE(ShouldValidateCoordinatePoolCode(
        100, kCoordinatePoolCodeValidationIdleFrame, true));
    REQUIRE(NextCoordinatePoolCodeValidationFrame(100, true) ==
        kCoordinatePoolCodeValidationIdleFrame);
    REQUIRE(NextCoordinatePoolCodeValidationFrame(100, false) ==
        100 + kCoordinatePoolCodeValidationRetryFrames);
    REQUIRE(!ShouldValidateCoordinatePoolCode(
        100 + kCoordinatePoolCodeValidationRetryFrames - 1,
        100 + kCoordinatePoolCodeValidationRetryFrames,
        false));
    REQUIRE(ShouldValidateCoordinatePoolCode(
        100 + kCoordinatePoolCodeValidationRetryFrames,
        100 + kCoordinatePoolCodeValidationRetryFrames,
        false));
    REQUIRE(NextCoordinatePoolCodeValidationFrame(UINT64_MAX, true) ==
        UINT64_MAX);

    using lengjing::game::CoordinateReadDiagnostic;
    using lengjing::game::CoordinateReadFailure;
    using lengjing::game::CoordinateReadStage;
    using lengjing::game::native::CoordinatePoolRuntimeError;
    using lengjing::game::native::IsCoordinatePoolRingRemoteReadFailure;
    CoordinateReadDiagnostic positionReadFailure{};
    positionReadFailure.stage = CoordinateReadStage::Position;
    positionReadFailure.failure = CoordinateReadFailure::AddressFault;
    REQUIRE(!ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::PositionReadFailed,
        positionReadFailure));
    REQUIRE(IsCoordinatePoolRingRemoteReadFailure(
        CoordinatePoolRuntimeError::PositionReadFailed,
        positionReadFailure));
    positionReadFailure.stage = CoordinateReadStage::RingIndex;
    REQUIRE(!ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::PositionReadFailed,
        positionReadFailure));
    REQUIRE(IsCoordinatePoolRingRemoteReadFailure(
        CoordinatePoolRuntimeError::PositionReadFailed,
        positionReadFailure));
    positionReadFailure.stage = CoordinateReadStage::DynamicPage;
    REQUIRE(ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::PositionReadFailed,
        positionReadFailure));
    REQUIRE(!IsCoordinatePoolRingRemoteReadFailure(
        CoordinatePoolRuntimeError::PositionReadFailed,
        positionReadFailure));
    REQUIRE(ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::PositionReadFailed,
        CoordinateReadDiagnostic{}));
    REQUIRE(!ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::PositionUnstable,
        CoordinateReadDiagnostic{}));
    REQUIRE(!IsCoordinatePoolRingRemoteReadFailure(
        CoordinatePoolRuntimeError::PositionUnstable,
        CoordinateReadDiagnostic{}));
    REQUIRE(!ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::PositionNotFinite,
        CoordinateReadDiagnostic{}));
    REQUIRE(!IsCoordinatePoolRingRemoteReadFailure(
        CoordinatePoolRuntimeError::PositionNotFinite,
        CoordinateReadDiagnostic{}));
    REQUIRE(!ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::RingSearchFailed,
        CoordinateReadDiagnostic{}));
    REQUIRE(ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::RingExecutionFailed,
        CoordinateReadDiagnostic{}));
    REQUIRE(ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
        CoordinatePoolRuntimeError::ParameterExecutionFailed,
        positionReadFailure));

    REQUIRE(!ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        false, 0x10000000, 0));
    REQUIRE(ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        true, 0x10000000, 0x20000000));
    REQUIRE(!ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        true, 0, 0x10000000));
    REQUIRE(!ShouldClearCoordinatePoolRingsAfterPointerRefresh(
        true, 0x10000000, 0x10000000));
}
