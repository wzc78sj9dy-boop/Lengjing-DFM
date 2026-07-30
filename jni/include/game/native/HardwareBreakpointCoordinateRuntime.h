#pragma once

#include "game/native/MemoryTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace lengjing::game::native {

struct HardwareBreakpointCoordinate {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class HardwareBreakpointCandidateSampleResult : std::uint8_t {
    Retry,
    Accepted,
    Reset,
};

struct HardwareBreakpointCoordinateCallbacks {
    std::function<bool(std::uintptr_t)> configureBreakpoint;
    std::function<bool(ExecutionBreakpointRecord*,
                       std::size_t,
                       std::size_t&,
                       std::uintptr_t&,
                       std::size_t&)>
        readRecords;
    std::function<bool(std::uintptr_t, void*, std::size_t)> readMemory;
    std::function<bool()> removeBreakpoints;
    std::function<HardwareBreakpointCandidateSampleResult(
        std::uintptr_t&)>
        readCandidate;

    explicit operator bool() const noexcept {
        return configureBreakpoint &&
            (readRecords || readCandidate) &&
            readMemory && removeBreakpoints;
    }
};

enum class HardwareBreakpointCoordinateProfile : std::uint8_t {
    Existing,
    OrderedRecordTable,
    MeshStream,
    StableRecordTable,
};

class HardwareBreakpointCoordinateRuntime final {
public:
    HardwareBreakpointCoordinateRuntime() = default;
    ~HardwareBreakpointCoordinateRuntime();

    HardwareBreakpointCoordinateRuntime(
        const HardwareBreakpointCoordinateRuntime&) = delete;
    HardwareBreakpointCoordinateRuntime& operator=(
        const HardwareBreakpointCoordinateRuntime&) = delete;

    bool Start(
        std::uintptr_t breakpointAddress,
        HardwareBreakpointCoordinateCallbacks callbacks,
        HardwareBreakpointCoordinateProfile profile =
            HardwareBreakpointCoordinateProfile::Existing) noexcept;
    bool Reconfigure(
        std::uintptr_t breakpointAddress,
        HardwareBreakpointCoordinateCallbacks callbacks,
        HardwareBreakpointCoordinateProfile profile) noexcept;
    bool Stop() noexcept;
    bool Poll(std::uintptr_t world) noexcept;
    bool Poll(std::uintptr_t world,
              std::uintptr_t manager) noexcept;
    bool Poll(std::uintptr_t world,
              std::uintptr_t targetRoot,
              std::uintptr_t manager) noexcept;
    bool Lookup(std::uint32_t id,
                std::uintptr_t world,
                HardwareBreakpointCoordinate& coordinate) noexcept;
    bool Lookup(std::uint32_t id,
                std::uintptr_t mesh,
                std::uintptr_t world,
                HardwareBreakpointCoordinate& coordinate) noexcept;
    void ResetWorld(std::uintptr_t world) noexcept;

    bool IsActive() const noexcept;
    std::uintptr_t BreakpointAddress() const noexcept;
    std::uintptr_t RecordsBase() const noexcept;
    std::size_t PublishedCoordinateCount() const noexcept;
    std::uint64_t PollCount() const noexcept;
    std::uint64_t AcceptedSampleCount() const noexcept;
    bool NeedsReconfigure() const noexcept;

private:
    struct SeenRecord {
        pid_t tid = -1;
        std::uint64_t hitCount = 0;
        std::uintptr_t pc = 0;
        std::uintptr_t x20 = 0;
        std::uintptr_t x21 = 0;
        bool valid = false;
    };

    struct MeshStreamCoordinate {
        std::uintptr_t mesh = 0;
        HardwareBreakpointCoordinate value{};
        std::uint64_t lastSeenPoll = 0;
    };

    struct CandidateSource {
        std::uintptr_t coordinateBase = 0;
        std::uintptr_t mesh = 0;
        std::uint64_t lastSeenPoll = 0;
    };

    struct CandidateObservation {
        std::uintptr_t value = 0;
        std::uint64_t lastSeenPoll = 0;
        std::uint64_t lastEvaluatedPoll = 0;
        std::uint64_t occurrences = 0;
        std::uint64_t distantOccurrences = 0;
        std::array<CandidateSource, 8> sources{};
        std::size_t sourceWriteIndex = 0;
        std::size_t sourceCount = 0;
        std::uint8_t qualifiedStreak = 0;
        std::uint8_t failureStreak = 0;
    };

    struct CoordinateTableSnapshot {
        std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
            coordinates;
        std::uintptr_t rawIdArray = 0;
        std::uintptr_t idArray = 0;
        std::uint32_t rawCount = 0;
        std::int64_t logicalCount = -1;
        std::size_t nonzeroIdCount = 0;
        std::size_t exactValidCount = 0;
        std::size_t plausibleCount = 0;
        std::size_t evidenceCount = 0;
        std::size_t distinctCount = 0;
        std::size_t requiredCount = 0;
        bool qualified = false;
    };

    struct CandidateSourceValidation {
        std::size_t storedCount = 0;
        std::size_t freshCount = 0;
        std::size_t expiredCount = 0;
        std::size_t addressFailureCount = 0;
        std::size_t readFailureCount = 0;
        std::size_t invalidCount = 0;
        std::size_t usableCount = 0;
        std::size_t idMissCount = 0;
        std::size_t presentCount = 0;
        std::size_t distanceFailureCount = 0;
        std::size_t matchedCount = 0;
        std::size_t distinctIdCount = 0;
        double closestDistance = 0.0;
    };

    bool SampleRecordsBase() noexcept;
    bool SampleStableRecordsBase() noexcept;
    bool SampleMeshStream() noexcept;
    void ExpireMeshStream() noexcept;
    bool SampleCandidate() noexcept;
    void ObserveCandidate(
        std::uintptr_t candidate,
        std::uintptr_t x20,
        std::uintptr_t x21) noexcept;
    bool ValidateCandidateSources(
        const CandidateObservation& observation,
        const CoordinateTableSnapshot& snapshot,
        CandidateSourceValidation& validation) noexcept;
    bool ReadObservedManager(
        std::uintptr_t targetRoot,
        std::uintptr_t& manager) noexcept;
    bool RefreshCoordinateTable(
        std::uintptr_t world,
        std::uintptr_t targetRoot,
        std::uintptr_t manager) noexcept;
    bool RefreshOrderedFallbackCoordinateTable(
        std::uintptr_t world,
        std::uintptr_t targetRoot,
        std::uintptr_t manager) noexcept;
    bool ReadCoordinateTable(
        std::uintptr_t world,
        std::uintptr_t targetRoot,
        std::uintptr_t manager,
        std::uintptr_t recordsBase,
        bool requireFallbackConfidence,
        CoordinateTableSnapshot& snapshot) noexcept;
    bool PublishCoordinateTable(
        std::uintptr_t world,
        std::uintptr_t targetRoot,
        std::uintptr_t manager,
        std::uintptr_t recordsBase,
        CoordinateTableSnapshot snapshot) noexcept;
    void ClearPublishedState() noexcept;
    void ClearConfigurationState() noexcept;
    void ClearWorldState() noexcept;
    void ClearSamplingState() noexcept;

    HardwareBreakpointCoordinateCallbacks callbacks_{};
    std::array<ExecutionBreakpointRecord, kExecutionBreakpointRecordLimit>
        records_{};
    std::array<SeenRecord, kExecutionBreakpointRecordLimit> seenRecords_{};
    std::array<std::uintptr_t, 10> candidateRing_{};
    std::array<
        CandidateObservation,
        kExecutionBreakpointRecordLimit> candidateObservations_{};
    std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
        coordinates_;
    std::unordered_map<std::uint32_t, MeshStreamCoordinate>
        meshStreamCoordinates_;
    std::unordered_map<std::uintptr_t, std::uint32_t>
        meshStreamIds_;
    std::uintptr_t breakpointAddress_ = 0;
    std::uintptr_t recordsBase_ = 0;
    std::uintptr_t pendingRecordsBase_ = 0;
    std::uintptr_t publishedRecordsBase_ = 0;
    std::uintptr_t publishedTargetRoot_ = 0;
    std::uintptr_t publishedManager_ = 0;
    std::uintptr_t publishedIdArray_ = 0;
    std::uintptr_t samplingTargetRoot_ = 0;
    std::uintptr_t samplingManager_ = 0;
    std::uintptr_t samplingIdArray_ = 0;
    std::uintptr_t world_ = 0;
    std::size_t publishedEvidenceCount_ = 0;
    std::size_t publishedDistinctCount_ = 0;
    std::size_t candidateWriteIndex_ = 0;
    std::size_t candidateCount_ = 0;
    std::uint8_t orderedRefreshFailureCount_ = 0;
    std::size_t lastTotalRecords_ = 0;
    std::uintptr_t lastHitAddress_ = 0;
    std::uint64_t pollCount_ = 0;
    std::uint64_t acceptedSampleCount_ = 0;
    HardwareBreakpointCoordinateProfile profile_ =
        HardwareBreakpointCoordinateProfile::Existing;
    bool needsReconfigure_ = false;
    bool active_ = false;
};

}  // namespace lengjing::game::native
