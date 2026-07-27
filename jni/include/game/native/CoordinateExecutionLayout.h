#pragma once

#include <cstdint>

namespace lengjing::game::native {

struct CoordinateExecutionDiscoveryLayout {
    std::uint64_t rootOffset = 0;
    std::uint64_t pointerOffset = 0;
    std::uint64_t entryOffset = 0;
    std::uint64_t returnStubMagic = 0;

    constexpr bool IsValid() const noexcept {
        return rootOffset != 0 && pointerOffset != 0 && entryOffset != 0 &&
            returnStubMagic != 0;
    }

    friend constexpr bool operator==(
        const CoordinateExecutionDiscoveryLayout& left,
        const CoordinateExecutionDiscoveryLayout& right) noexcept {
        return left.rootOffset == right.rootOffset &&
            left.pointerOffset == right.pointerOffset &&
            left.entryOffset == right.entryOffset &&
            left.returnStubMagic == right.returnStubMagic;
    }
};

struct CoordinateExecutionResultLayout {
    std::uint64_t resultSlotOffset = 0;
    std::uint64_t positionOffset = 0;

    constexpr bool IsValid() const noexcept {
        return resultSlotOffset != 0 && positionOffset != 0;
    }

    friend constexpr bool operator==(
        const CoordinateExecutionResultLayout& left,
        const CoordinateExecutionResultLayout& right) noexcept {
        return left.resultSlotOffset == right.resultSlotOffset &&
            left.positionOffset == right.positionOffset;
    }
};

struct CoordinateExecutionHookLayout {
    std::uint64_t subjectLoadInstruction = 0;
    std::uint64_t callbackInstruction = 0;
    std::uint64_t callbackReturn = 0;
    std::uint64_t callbackIndex = 0;
    std::uint64_t callbackCopyPrepare = 0;
    std::uint64_t callbackCopyAfter = 0;
    std::uint64_t callbackTablePointerCode = 0;
    std::uint64_t callbackTableValueCode = 0;
    std::uint64_t callbackLockCode = 0;
    std::uint64_t callbackLockReturnCode = 0;
    std::uint64_t callbackFirstCallCode = 0;
    std::uint64_t callbackFirstReturnCode = 0;
    std::uint64_t callbackExternalCallCode = 0;
    std::uint64_t callbackExternalReturnCode = 0;
    std::uint64_t callbackPrimaryGateWriteCode = 0;
    std::uint64_t callbackAlternateGateWriteCode = 0;
    std::uint64_t callbackGateCode = 0;
    std::uint64_t callbackRecordCountCode = 0;
    std::uint64_t callbackTargetKeyCode = 0;
    std::uint64_t callbackRingSetupCode = 0;
    std::uint64_t callbackRingProbeCode = 0;
    std::uint64_t callbackRingHitCode = 0;
    std::uint64_t callbackDispatchCode = 0;
    std::uint64_t callbackDispatchReturnCode = 0;
    std::uint64_t callbackResultPrepareCode = 0;
    std::uint64_t callbackResultCode = 0;

    constexpr bool IsValid() const noexcept {
        return subjectLoadInstruction != 0 && callbackInstruction != 0 &&
            callbackReturn != 0 && callbackIndex != 0 &&
            callbackCopyPrepare != 0 && callbackCopyAfter != 0 &&
            callbackTablePointerCode != 0 &&
            callbackTableValueCode != 0 && callbackLockCode != 0 &&
            callbackLockReturnCode != 0 && callbackFirstCallCode != 0 &&
            callbackFirstReturnCode != 0 && callbackExternalCallCode != 0 &&
            callbackExternalReturnCode != 0 &&
            callbackPrimaryGateWriteCode != 0 &&
            callbackAlternateGateWriteCode != 0 && callbackGateCode != 0 &&
            callbackRecordCountCode != 0 && callbackTargetKeyCode != 0 &&
            callbackRingSetupCode != 0 && callbackRingProbeCode != 0 &&
            callbackRingHitCode != 0 && callbackDispatchCode != 0 &&
            callbackDispatchReturnCode != 0 &&
            callbackResultPrepareCode != 0 && callbackResultCode != 0;
    }

    friend constexpr bool operator==(
        const CoordinateExecutionHookLayout& left,
        const CoordinateExecutionHookLayout& right) noexcept {
        return left.subjectLoadInstruction == right.subjectLoadInstruction &&
            left.callbackInstruction == right.callbackInstruction &&
            left.callbackReturn == right.callbackReturn &&
            left.callbackIndex == right.callbackIndex &&
            left.callbackCopyPrepare == right.callbackCopyPrepare &&
            left.callbackCopyAfter == right.callbackCopyAfter &&
            left.callbackTablePointerCode == right.callbackTablePointerCode &&
            left.callbackTableValueCode == right.callbackTableValueCode &&
            left.callbackLockCode == right.callbackLockCode &&
            left.callbackLockReturnCode == right.callbackLockReturnCode &&
            left.callbackFirstCallCode == right.callbackFirstCallCode &&
            left.callbackFirstReturnCode == right.callbackFirstReturnCode &&
            left.callbackExternalCallCode == right.callbackExternalCallCode &&
            left.callbackExternalReturnCode ==
                right.callbackExternalReturnCode &&
            left.callbackPrimaryGateWriteCode ==
                right.callbackPrimaryGateWriteCode &&
            left.callbackAlternateGateWriteCode ==
                right.callbackAlternateGateWriteCode &&
            left.callbackGateCode == right.callbackGateCode &&
            left.callbackRecordCountCode == right.callbackRecordCountCode &&
            left.callbackTargetKeyCode == right.callbackTargetKeyCode &&
            left.callbackRingSetupCode == right.callbackRingSetupCode &&
            left.callbackRingProbeCode == right.callbackRingProbeCode &&
            left.callbackRingHitCode == right.callbackRingHitCode &&
            left.callbackDispatchCode == right.callbackDispatchCode &&
            left.callbackDispatchReturnCode ==
                right.callbackDispatchReturnCode &&
            left.callbackResultPrepareCode ==
                right.callbackResultPrepareCode &&
            left.callbackResultCode == right.callbackResultCode;
    }
};

struct CoordinateExecutionFieldLayout {
    std::uint64_t externalExpected = 0;
    std::uint64_t externalPriorGate = 0;
    std::uint64_t primaryGateSource = 0;
    std::uint64_t gateFlag = 0;
    std::uint64_t gateSnapshotA = 0;
    std::uint64_t gateSnapshotB = 0;
    std::uint64_t ringMid = 0;
    std::uint64_t resultPosition = 0;
    std::uint64_t capturedLocal0 = 0;
    std::uint64_t capturedLocal1 = 0;
    std::uint64_t capturedLocal2 = 0;
    std::uint64_t capturedLocal3 = 0;
    std::uint64_t capturedLocalField = 0;
    std::uint64_t poolSelector = 0;
    std::uint64_t poolTable = 0;

    constexpr bool IsValid() const noexcept {
        return externalExpected != 0 && externalPriorGate != 0 &&
            primaryGateSource != 0 && gateFlag != 0 &&
            gateSnapshotA != 0 && gateSnapshotB != 0 && ringMid != 0 &&
            resultPosition != 0 && capturedLocal0 != 0 &&
            capturedLocal1 != 0 && capturedLocal2 != 0 &&
            capturedLocal3 != 0 && capturedLocalField != 0 &&
            poolSelector != 0 && poolTable != 0;
    }

    friend constexpr bool operator==(
        const CoordinateExecutionFieldLayout& left,
        const CoordinateExecutionFieldLayout& right) noexcept {
        return left.externalExpected == right.externalExpected &&
            left.externalPriorGate == right.externalPriorGate &&
            left.primaryGateSource == right.primaryGateSource &&
            left.gateFlag == right.gateFlag &&
            left.gateSnapshotA == right.gateSnapshotA &&
            left.gateSnapshotB == right.gateSnapshotB &&
            left.ringMid == right.ringMid &&
            left.resultPosition == right.resultPosition &&
            left.capturedLocal0 == right.capturedLocal0 &&
            left.capturedLocal1 == right.capturedLocal1 &&
            left.capturedLocal2 == right.capturedLocal2 &&
            left.capturedLocal3 == right.capturedLocal3 &&
            left.capturedLocalField == right.capturedLocalField &&
            left.poolSelector == right.poolSelector &&
            left.poolTable == right.poolTable;
    }
};

struct CoordinateExecutionLayout {
    CoordinateExecutionDiscoveryLayout discovery{};
    CoordinateExecutionResultLayout result{};
    CoordinateExecutionHookLayout hooks{};
    CoordinateExecutionFieldLayout fields{};

    constexpr bool IsValid() const noexcept {
        return discovery.IsValid() && result.IsValid();
    }

    constexpr bool HasCompleteDiagnostics() const noexcept {
        return hooks.IsValid() && fields.IsValid();
    }

    friend constexpr bool operator==(
        const CoordinateExecutionLayout& left,
        const CoordinateExecutionLayout& right) noexcept {
        return left.discovery == right.discovery &&
            left.result == right.result && left.hooks == right.hooks &&
            left.fields == right.fields;
    }
};

}  // namespace lengjing::game::native
