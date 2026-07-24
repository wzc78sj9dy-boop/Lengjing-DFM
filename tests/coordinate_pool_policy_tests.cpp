#include "game/native/CoordinateDecrypt2Runtime.h"
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
    using lengjing::game::native::IsCoordinatePoolRootSnapshotInitialized;
    using lengjing::game::native::CoordinatePoolCandidateSet;
    using lengjing::game::native::CoordinatePoolDecryptMode;
    using lengjing::game::native::CoordinatePoolStablePositionCache;
    using lengjing::game::native::IsCoordinatePoolReadRangeValid;
    using lengjing::game::native::IsCoordinatePoolBlockTerminator;
    using lengjing::game::native::IsCoordinatePoolDecryptIndexOffsetValid;
    using lengjing::game::native::IsCoordinatePoolSelectedCandidateValid;
    using lengjing::game::native::MapDecodedCoordinatePoolSlot;
    using lengjing::game::native::MakeCoordinateDecrypt2RuntimeLayout;
    using lengjing::game::native::NextCoordinatePoolCodeValidationFrame;
    using lengjing::game::native::NormalizeCoordinatePoolIndexedPointer;
    using lengjing::game::native::NormalizeCoordinatePoolPointer;
    using lengjing::game::native::
        ResolveCoordinatePoolIndexedPointerAddress;
    using lengjing::game::native::
        ResolveCoordinatePoolIndexedRootAddresses;
    using lengjing::game::native::ResolveCoordinatePoolDecryptMode;
    using lengjing::game::native::IsCoordinatePoolDecryptRequested;
    using lengjing::game::native::IsCoordinatePoolIndexedDecrypt;
    using lengjing::game::native::
        ShouldClearCoordinatePoolRingsAfterPointerRefresh;
    using lengjing::game::native::ShouldSearchCoordinatePoolRing;
    using lengjing::game::native::
        ShouldRequestCoordinatePoolCodeValidationAfterReadFailure;
    using lengjing::game::native::ShouldRetryCoordinatePoolRing;
    using lengjing::game::native::ShouldRetryCoordinatePoolCompactSnapshot;
    using lengjing::game::native::ShouldValidateCoordinatePoolCode;
    using lengjing::game::native::SelectCoordinatePoolIndexedSlot;
    using lengjing::game::native::PredictCoordinatePoolBlockCount;
    using lengjing::game::native::kCoordinatePoolCodeValidationIdleFrame;
    using lengjing::game::native::kCoordinatePoolCodeValidationRetryFrames;
    using lengjing::game::native::kCoordinatePoolRingRetryFrames;
    using lengjing::game::native::kCoordinatePoolRingReadFailureThreshold;
    using lengjing::game::native::kCoordinatePoolRingSearchesPerFrame;
    using lengjing::game::native::kCoordinatePoolMaximumRemoteAddress;
    using lengjing::game::native::kCoordinatePoolBlockProbeCount;
    using lengjing::game::native::kCoordinatePoolMaximumBlockCount;
    using lengjing::game::native::
        kCoordinatePoolMaximumDecryptIndexOffset;
    using lengjing::game::native::kCoordinatePoolMinimumRemoteAddress;
    REQUIRE(!CoordinatePoolEnvironmentFlagEnabled(nullptr));
    REQUIRE(!CoordinatePoolEnvironmentFlagEnabled(""));
    REQUIRE(!CoordinatePoolEnvironmentFlagEnabled("0"));
    REQUIRE(CoordinatePoolEnvironmentFlagEnabled("1"));
    REQUIRE(CoordinatePoolEnvironmentFlagEnabled("1-full"));
    REQUIRE(
        ResolveCoordinatePoolDecryptMode(false, false) ==
        CoordinatePoolDecryptMode::None);
    REQUIRE(
        ResolveCoordinatePoolDecryptMode(true, false) ==
        CoordinatePoolDecryptMode::Legacy);
    REQUIRE(
        ResolveCoordinatePoolDecryptMode(false, true) ==
        CoordinatePoolDecryptMode::Indexed);
    REQUIRE(
        ResolveCoordinatePoolDecryptMode(true, true) ==
        CoordinatePoolDecryptMode::Indexed);
    REQUIRE(IsCoordinatePoolDecryptRequested(
        CoordinatePoolDecryptMode::Legacy));
    REQUIRE(IsCoordinatePoolDecryptRequested(
        CoordinatePoolDecryptMode::Indexed));
    REQUIRE(!IsCoordinatePoolDecryptRequested(
        CoordinatePoolDecryptMode::None));
    REQUIRE(!IsCoordinatePoolIndexedDecrypt(
        CoordinatePoolDecryptMode::Legacy));
    REQUIRE(IsCoordinatePoolIndexedDecrypt(
        CoordinatePoolDecryptMode::Indexed));

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

    REQUIRE(IsCoordinatePoolDecryptIndexOffsetValid(0));
    REQUIRE(IsCoordinatePoolDecryptIndexOffsetValid(
        kCoordinatePoolMaximumDecryptIndexOffset));
    REQUIRE(!IsCoordinatePoolDecryptIndexOffsetValid(
        kCoordinatePoolMaximumDecryptIndexOffset + 1));
    REQUIRE(SelectCoordinatePoolIndexedSlot(0, 0, 10) == 0);
    REQUIRE(SelectCoordinatePoolIndexedSlot(9, 1, 10) == 0);
    REQUIRE(SelectCoordinatePoolIndexedSlot(13, 10, 14) == 9);
    REQUIRE(SelectCoordinatePoolIndexedSlot(UINT64_MAX, 10, 19) == 9);
    REQUIRE(SelectCoordinatePoolIndexedSlot(1, 0, 0) ==
        kCoordinatePoolBlockProbeCount);
    REQUIRE(SelectCoordinatePoolIndexedSlot(
        1, 0, kCoordinatePoolMaximumBlockCount + 1) ==
        kCoordinatePoolBlockProbeCount);
    REQUIRE(SelectCoordinatePoolIndexedSlot(
        1, kCoordinatePoolMaximumDecryptIndexOffset + 1, 10) ==
        kCoordinatePoolBlockProbeCount);
    REQUIRE(IsCoordinatePoolBlockTerminator({}));
    REQUIRE(IsCoordinatePoolBlockTerminator({-0.0f, 0.0f, -0.0f}));
    REQUIRE(!IsCoordinatePoolBlockTerminator({1.0f, 0.0f, 0.0f}));
    std::array<lengjing::game::native::CoordinatePoolPosition,
               kCoordinatePoolBlockProbeCount> poolBlocks{};
    REQUIRE(PredictCoordinatePoolBlockCount(
        poolBlocks.data(), poolBlocks.size()) == 0);
    for (std::size_t index = 0; index < poolBlocks.size(); ++index) {
        poolBlocks[index] = {
            static_cast<float>(index + 1),
            index == 3 ? 0.0f : 1.0f,
            1.0f,
        };
    }
    REQUIRE(PredictCoordinatePoolBlockCount(
        poolBlocks.data(), poolBlocks.size()) == 0);
    poolBlocks[5] = {};
    REQUIRE(PredictCoordinatePoolBlockCount(
        poolBlocks.data(), poolBlocks.size()) == 5);
    poolBlocks[2] = {1.0f, 0.0f, 0.0f};
    REQUIRE(PredictCoordinatePoolBlockCount(
        poolBlocks.data(), poolBlocks.size()) == 5);
    poolBlocks[19] = {};
    poolBlocks[5] = {6.0f, 1.0f, 1.0f};
    REQUIRE(PredictCoordinatePoolBlockCount(
        poolBlocks.data(), poolBlocks.size()) == 19);
    poolBlocks[1] = {};
    poolBlocks[7] = {};
    REQUIRE(PredictCoordinatePoolBlockCount(
        poolBlocks.data(), poolBlocks.size()) == 1);
    poolBlocks[0] = {};
    REQUIRE(PredictCoordinatePoolBlockCount(
        poolBlocks.data(), poolBlocks.size()) == 0);
    REQUIRE(PredictCoordinatePoolBlockCount(
        poolBlocks.data(), poolBlocks.size() - 1) == 0);

    CoordinatePoolStablePositionCache stablePosition;
    std::uint8_t resolvedStableSlot = 0;
    const auto initialConflict = stablePosition.Resolve(
        1, 2, {11.0f, 12.0f, 13.0f}, 4, resolvedStableSlot);
    REQUIRE(IsCoordinatePoolBlockTerminator(initialConflict));
    REQUIRE(resolvedStableSlot == UINT8_MAX);
    const auto firstStable = stablePosition.Resolve(
        3, 3, {21.0f, 22.0f, 23.0f}, 6, resolvedStableSlot);
    REQUIRE(firstStable.x == 21.0f);
    REQUIRE(resolvedStableSlot == 6);
    const auto retained = stablePosition.Resolve(
        4, 5, {31.0f, 32.0f, 33.0f}, 9, resolvedStableSlot);
    REQUIRE(retained.x == 21.0f);
    REQUIRE(retained.y == 22.0f);
    REQUIRE(retained.z == 23.0f);
    REQUIRE(resolvedStableSlot == 6);
    const auto secondStable = stablePosition.Resolve(
        8, 8, {41.0f, 42.0f, 43.0f}, 12, resolvedStableSlot);
    REQUIRE(secondStable.x == 41.0f);
    REQUIRE(resolvedStableSlot == 12);
    const auto secondRetained = stablePosition.Resolve(
        9, 10, {51.0f, 52.0f, 53.0f}, 14, resolvedStableSlot);
    REQUIRE(secondRetained.x == 41.0f);
    REQUIRE(secondRetained.y == 42.0f);
    REQUIRE(secondRetained.z == 43.0f);
    REQUIRE(resolvedStableSlot == 12);
    stablePosition.Reset();
    REQUIRE(IsCoordinatePoolBlockTerminator(
        stablePosition.Resolve(
            6,
            7,
            {41.0f, 42.0f, 43.0f},
            12,
            resolvedStableSlot)));
    REQUIRE(resolvedStableSlot == UINT8_MAX);

    CoordinatePoolCandidateSet candidates{};
    REQUIRE(!IsCoordinatePoolSelectedCandidateValid(candidates));
    candidates.valid[3] = true;
    REQUIRE(!IsCoordinatePoolSelectedCandidateValid(candidates));
    candidates.selectedLogicalSlot = 3;
    REQUIRE(IsCoordinatePoolSelectedCandidateValid(candidates));
    candidates.selectedLogicalSlot =
        static_cast<std::uint8_t>(candidates.valid.size());
    REQUIRE(!IsCoordinatePoolSelectedCandidateValid(candidates));
    candidates.resolvedPosition = {1.0f, 2.0f, 3.0f};
    candidates.resolvedPoolSlot = 18;
    candidates.resolvedValid = true;
    REQUIRE(!IsCoordinatePoolSelectedCandidateValid(candidates));
    REQUIRE(candidates.resolvedValid);

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

    CoordinatePoolSlotLayoutCalibration compactUpgradeCalibration;
    REQUIRE(compactUpgradeCalibration.ObserveTransition(
        0x1000, 1, 2, 5, 4, 0x0201).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(compactUpgradeCalibration.ObserveTransition(
        0x2000, 3, 4, 8, 7, 0x000c).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(compactUpgradeCalibration.ObserveTransition(
        0x1000, 5, 6, 2, 3, 0x0180).kind ==
        CoordinatePoolSlotLayoutKind::Compact);
    REQUIRE(compactUpgradeCalibration.ObserveDecodedSlot(10).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(!compactUpgradeCalibration.CompactPossible());
    REQUIRE(compactUpgradeCalibration.ObserveTransition(
        0x1000, 7, 8, 1, 6, 0x0210).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(compactUpgradeCalibration.ObserveTransition(
        0x2000, 9, 10, 12, 3, 0x0042).kind ==
        CoordinatePoolSlotLayoutKind::Unknown);
    REQUIRE(compactUpgradeCalibration.ObserveTransition(
        0x1000, 11, 12, 7, 0, 0x0408).kind ==
        CoordinatePoolSlotLayoutKind::Extended);

    CoordinatePoolSlotLayoutCalibration invalidDecodedCalibration;
    invalidDecodedCalibration.ObserveDecodedSlot(10);
    invalidDecodedCalibration.ObserveTransition(
        0x1000, 1, 2, 1, 6, 0x0210);
    invalidDecodedCalibration.ObserveTransition(
        0x2000, 3, 4, 12, 3, 0x0042);
    REQUIRE(invalidDecodedCalibration.ObserveTransition(
        0x1000, 5, 6, 7, 0, 0x0408).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(invalidDecodedCalibration.ObserveDecodedSlot(14).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(invalidDecodedCalibration.ObserveDecodedSlot(6).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(invalidDecodedCalibration.ObserveDecodedSlot(14).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(invalidDecodedCalibration.ObserveDecodedSlot(15).kind ==
        CoordinatePoolSlotLayoutKind::Extended);
    REQUIRE(invalidDecodedCalibration.ObserveDecodedSlot(16).kind ==
        CoordinatePoolSlotLayoutKind::Conflict);

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
    REQUIRE(IsCoordinatePoolRootSnapshotInitialized(root));
    REQUIRE(!IsCoordinatePoolRootSnapshotInitialized({}));
    REQUIRE(CoordinatePoolGuardedRootSnapshotMatches(
        root, root, root.bridge));
    REQUIRE(!CoordinatePoolGuardedRootSnapshotMatches(
        root, root, root.bridge + 4));
    REQUIRE(!CoordinatePoolGuardedRootSnapshotMatches(
        root,
        CoordinatePoolRootSnapshot{root.bridge, root.context + 8, root.entry},
        root.bridge));
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
    REQUIRE(NormalizeCoordinatePoolIndexedPointer(
        UINT64_C(0xAB00007123456780)) == pointer);
    REQUIRE(NormalizeCoordinatePoolIndexedPointer(
        UINT64_C(0xAB01007123456780)) ==
        UINT64_C(0x0001007123456780));
    std::uint64_t indexedEntryAddress = 0;
    REQUIRE(ResolveCoordinatePoolIndexedPointerAddress(
        UINT64_C(0xAB00007123456000),
        0xA0,
        indexedEntryAddress));
    REQUIRE(indexedEntryAddress == UINT64_C(0x00000071234560A0));
    std::uint64_t indexedContextAddress = 0;
    REQUIRE(ResolveCoordinatePoolIndexedPointerAddress(
        UINT64_C(0xCD00007123457000),
        -8,
        indexedContextAddress));
    REQUIRE(indexedContextAddress == UINT64_C(0x0000007123456FF8));
    REQUIRE(!ResolveCoordinatePoolIndexedPointerAddress(
        4, -8, indexedContextAddress));
    REQUIRE(ResolveCoordinatePoolIndexedRootAddresses(
        UINT64_C(0xAB00007123456000),
        -8,
        0xA0,
        indexedContextAddress,
        indexedEntryAddress));
    REQUIRE(indexedContextAddress == UINT64_C(0x0000007123455FF8));
    REQUIRE(indexedEntryAddress == UINT64_C(0x00000071234560A0));

    const lengjing::game::native::CoordinatePoolRuntimeLayout cloudLayout{
        0x1A009000,
        0x14,
        -16,
        0xB0,
        0x220,
        64,
        24,
        90,
    };
    REQUIRE(cloudLayout.IsValid());
    const auto decrypt2Layout =
        MakeCoordinateDecrypt2RuntimeLayout(cloudLayout);
    REQUIRE(decrypt2Layout.rootRva == 0x0E738950);
    REQUIRE(decrypt2Layout.bridgeOffset == 0x0C);
    REQUIRE(decrypt2Layout.entryOffset == 0xA0);
    REQUIRE(decrypt2Layout.contextOffset == -8);
    REQUIRE(decrypt2Layout.componentKeyOffset == 0x210);
    REQUIRE(decrypt2Layout.entryStride == 0x30);
    REQUIRE(decrypt2Layout.poolHeadSkip == 0x10);
    REQUIRE(decrypt2Layout.ringRefreshFrames == 90);
    REQUIRE(kCoordinatePoolBlockProbeCount * decrypt2Layout.entryStride ==
        0x3C0);
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
