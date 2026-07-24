#pragma once

#if 0

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

    explicit operator bool() const noexcept {
        return configureBreakpoint && readRecords && readMemory &&
            removeBreakpoints;
    }
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
        HardwareBreakpointCoordinateCallbacks callbacks) noexcept;
    bool Stop() noexcept;
    bool Poll(std::uintptr_t world) noexcept;
    bool Poll(std::uintptr_t world,
              std::uintptr_t manager) noexcept;
    bool Lookup(std::uint32_t id,
                std::uintptr_t world,
                HardwareBreakpointCoordinate& coordinate) noexcept;
    void ResetWorld(std::uintptr_t world) noexcept;

    bool IsActive() const noexcept;
    std::uintptr_t BreakpointAddress() const noexcept;
    std::uintptr_t RecordsBase() const noexcept;
    std::size_t PublishedCoordinateCount() const noexcept;
    std::uint64_t PollCount() const noexcept;
    std::uint64_t AcceptedSampleCount() const noexcept;

private:
    struct SeenRecord {
        pid_t tid = -1;
        std::uint64_t hitCount = 0;
        std::uintptr_t pc = 0;
        bool valid = false;
    };

    bool SampleRecordsBase() noexcept;
    bool RefreshCoordinateTable(
        std::uintptr_t world,
        std::uintptr_t manager) noexcept;
    void ClearWorldState() noexcept;
    void ClearSamplingState() noexcept;

    HardwareBreakpointCoordinateCallbacks callbacks_{};
    std::array<ExecutionBreakpointRecord, kExecutionBreakpointRecordLimit>
        records_{};
    std::array<SeenRecord, kExecutionBreakpointRecordLimit> seenRecords_{};
    std::array<std::uintptr_t, 10> candidateRing_{};
    std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
        coordinates_;
    std::uintptr_t breakpointAddress_ = 0;
    std::uintptr_t recordsBase_ = 0;
    std::uintptr_t world_ = 0;
    std::size_t candidateWriteIndex_ = 0;
    std::size_t candidateCount_ = 0;
    std::size_t lastTotalRecords_ = 0;
    std::uintptr_t lastHitAddress_ = 0;
    std::uint64_t pollCount_ = 0;
    std::uint64_t acceptedSampleCount_ = 0;
    bool active_ = false;
};

}  // namespace lengjing::game::native

#endif
