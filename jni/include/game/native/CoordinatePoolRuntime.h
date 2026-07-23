#pragma once

#include "game/CoordinateDecryptDiagnostics.h"
#include "game/native/CoordinatePoolPolicy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <sys/types.h>

namespace lengjing::game::native {

class MemoryTransport;
struct ProcessExecutionContext;

struct CoordinatePoolRuntimeLayout {
    std::uintptr_t rootRva = 0x0E738950;
    std::uintptr_t bridgeOffset = 12;
    std::int32_t contextOffset = -8;
    std::uintptr_t entryOffset = 0xA0;
    std::uintptr_t componentKeyOffset = 0x210;
    std::uint32_t entryStride = 48;
    std::uint32_t poolHeadSkip = 16;
    std::uint32_t ringRefreshFrames = 60;

    constexpr bool IsValid() const noexcept {
        return rootRva >= 4 && rootRva <= 0xffffffffULL &&
            (rootRva & 3U) == 0 &&
            bridgeOffset <= 0x10000 && (bridgeOffset & 3U) == 0 &&
            bridgeOffset <= 0xffffffffULL - rootRva &&
            contextOffset != 0 && contextOffset >= -0x10000 &&
            contextOffset <= 0x10000 && (contextOffset % 8) == 0 &&
            entryOffset >= 8 && entryOffset <= 0x10000 &&
            (entryOffset & 7U) == 0 &&
            componentKeyOffset >= 8 && componentKeyOffset <= 0xffff &&
            (componentKeyOffset & 7U) == 0 &&
            entryStride >= 12 && entryStride <= 4096 &&
            (entryStride & 3U) == 0 && poolHeadSkip <= 4084 &&
            poolHeadSkip + 12 <= entryStride &&
            ringRefreshFrames != 0 && ringRefreshFrames <= 3600;
    }
};

struct CoordinatePoolPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct CoordinatePoolCandidateSet {
    std::array<CoordinatePoolPosition,
               kCoordinatePoolLogicalCandidateCount> positions{};
    std::array<bool, kCoordinatePoolLogicalCandidateCount> valid{};
    std::array<CoordinatePoolPosition,
               kCoordinatePoolPhysicalSlotCount> physicalPositions{};
    std::array<bool, kCoordinatePoolPhysicalSlotCount> physicalValid{};
    std::uintptr_t ring = 0;
    std::uint64_t index = 0;
    std::uint16_t changedPhysicalMask = 0;
    std::uint16_t decodedSlotMask = 0;
    std::uint8_t activeBank = 0;
    std::uint8_t logicalSlotCount = 0;
    std::uint8_t physicalSlotCount = 0;
    std::uint8_t slotPhase = 0;
    std::uint8_t selectedLogicalSlot = 0;
    std::uint8_t selectedPhysicalSlot = 0;
    std::uint8_t decodedPhysicalSlot = 0;
    std::uint8_t changedPhysicalCount = 0;
    std::uint8_t newestPhysicalSlot = UINT8_MAX;
};

constexpr bool IsCoordinatePoolSelectedCandidateValid(
    const CoordinatePoolCandidateSet& candidates) noexcept {
    return candidates.selectedLogicalSlot < candidates.valid.size() &&
        candidates.valid[candidates.selectedLogicalSlot];
}

enum class CoordinatePoolRuntimeStage : std::uint8_t {
    Idle,
    RootResolved,
    CodeAnalyzed,
    ContextReady,
    Active,
    Faulted,
};

enum class CoordinatePoolRuntimeError : std::uint8_t {
    None,
    InvalidInput,
    RootReadFailed,
    EntryResolveFailed,
    EntryMappingMissing,
    EntryMappingFragmented,
    EntryMappingChanged,
    EntryPageReadFailed,
    EntryCodeReadFailed,
    CodeReadFailed,
    AnalysisFailed,
    EngineSetupFailed,
    ContextMissing,
    ParameterExecutionFailed,
    ParameterReadFailed,
    PoolPointerReadFailed,
    RingSearchFailed,
    RingPreparationFailed,
    RingExecutionFailed,
    RingRegisterReadFailed,
    RingValueInvalid,
    PositionReadFailed,
    PositionNotFinite,
    PositionUnstable,
    SlotLayoutPending,
    SlotLayoutConflict,
    SlotLayoutEvidenceMissing,
};

enum class CoordinatePoolAnalysisFailure : std::uint8_t {
    None,
    MappingInvalid,
    EntryScan,
    FinderSetup,
    FinderPattern,
    PassLimit,
    FingerprintMissing,
    SnapshotChanged,
    RequiredPoints,
};

constexpr bool ShouldRequestCoordinatePoolCodeValidationAfterReadFailure(
    CoordinatePoolRuntimeError error,
    const CoordinateReadDiagnostic& read) noexcept {
    switch (error) {
        case CoordinatePoolRuntimeError::None:
        case CoordinatePoolRuntimeError::InvalidInput:
        case CoordinatePoolRuntimeError::PoolPointerReadFailed:
        case CoordinatePoolRuntimeError::RingSearchFailed:
        case CoordinatePoolRuntimeError::PositionNotFinite:
        case CoordinatePoolRuntimeError::PositionUnstable:
        case CoordinatePoolRuntimeError::SlotLayoutPending:
        case CoordinatePoolRuntimeError::SlotLayoutConflict:
        case CoordinatePoolRuntimeError::SlotLayoutEvidenceMissing:
            return false;
        case CoordinatePoolRuntimeError::PositionReadFailed:
            return !read.HasFailure() ||
                (read.stage != CoordinateReadStage::RingIndex &&
                 read.stage != CoordinateReadStage::Position);
        default:
            return true;
    }
}

constexpr bool IsCoordinatePoolRingRemoteReadFailure(
    CoordinatePoolRuntimeError error,
    const CoordinateReadDiagnostic& read) noexcept {
    return error == CoordinatePoolRuntimeError::PositionReadFailed &&
        read.HasFailure() &&
        (read.stage == CoordinateReadStage::RingIndex ||
         read.stage == CoordinateReadStage::Position);
}

struct CoordinatePoolRuntimeProbe {
    CoordinatePoolRuntimeStage stage = CoordinatePoolRuntimeStage::Idle;
    CoordinatePoolRuntimeError error = CoordinatePoolRuntimeError::None;
    std::uintptr_t bridge = 0;
    std::uintptr_t context = 0;
    std::uintptr_t guestEntry = 0;
    std::uint32_t entryInstruction = 0;
    std::uintptr_t codeBase = 0;
    std::size_t codeSize = 0;
    std::uint32_t executableMappingFragments = 0;
    std::uintptr_t executableMappingStart = 0;
    std::uintptr_t executableMappingEnd = 0;
    std::uintptr_t failedMethod = 0;
    CoordinatePoolAnalysisFailure analysisFailure =
        CoordinatePoolAnalysisFailure::None;
    std::uint8_t analysisFindStage = 0;
    std::uint8_t analysisMode = 0;
    std::uint16_t analysisPasses = 0;
    std::uint32_t analysisLoadedPages = 0;
    std::uint32_t analysisRequestedMethods = 0;
    std::uint32_t analysisSkippedPages = 0;
    std::uint8_t analysisSkippedFailure = 0;
    int analysisSkippedSystemError = 0;
    std::int32_t poolPointerOffset = 0;
    std::int64_t indexOffset = 0;
    std::uint32_t ringOffset = 0;
    std::uint16_t decodedSlotMask = 0;
    std::uint16_t compactPhaseMask = 0;
    std::uint16_t extendedPhaseMask = 0;
    std::uint8_t logicalSlotCount = 0;
    std::uint8_t physicalSlotCount = 0;
    std::uint8_t slotPhase = 0;
    std::uint8_t slotLayoutKind = 0;
    std::uint8_t compactLayoutEvidence = 0;
    std::uint8_t extendedLayoutEvidence = 0;
    std::int32_t threadId = 0;
    std::uint64_t contextGeneration = 0;
    std::uint64_t attempts = 0;
    std::uint64_t successes = 0;
    int systemError = 0;
    CoordinateReadDiagnostic read{};
    CoordinatePoolPointerDiagnostic poolPointer{};
};

class CoordinatePoolRuntime final {
public:
    explicit CoordinatePoolRuntime(
        CoordinatePoolRuntimeLayout layout = {});
    ~CoordinatePoolRuntime();

    CoordinatePoolRuntime(const CoordinatePoolRuntime&) = delete;
    CoordinatePoolRuntime& operator=(const CoordinatePoolRuntime&) = delete;

    bool Configure(const CoordinatePoolRuntimeLayout& layout) noexcept;

    bool Refresh(MemoryTransport& memory,
                 pid_t processId,
                 std::uintptr_t moduleBase,
                 const ProcessExecutionContext& executionContext,
                 std::uint64_t frame) noexcept;
    bool ReadPosition(std::uintptr_t component,
                      CoordinatePoolPosition& position,
                      bool refresh = false) noexcept;
    bool ReadCandidates(std::uintptr_t component,
                        CoordinatePoolCandidateSet& candidates,
                        bool refresh = false) noexcept;
    CoordinatePoolRuntimeProbe Probe() const noexcept;
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
