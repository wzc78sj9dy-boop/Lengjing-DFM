#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#ifndef LENGJING_ENABLE_PERFORMANCE_TRACE
#define LENGJING_ENABLE_PERFORMANCE_TRACE 0
#endif

namespace lengjing::platform {

enum class PerformancePhase : std::uint8_t {
    DataFrame,
    BackendFrame,
    FrameContext,
    ActorSnapshot,
    ActorLoop,
    PositionRead,
    CoordinateDecode,
    HealthRead,
    DownedRead,
    EquipmentRead,
    BoneRead,
    Projection,
    WorldObjectRefresh,
    WorldObjects,
    RenderFrame,
    DrawGameFrame,
    GraphicsSubmit,
    RemoteRead,
    CoordinateRemoteRead,
    BatchRemoteRead,
    Count,
};

enum class PerformanceCounter : std::uint8_t {
    DataFrames,
    ReadyFrames,
    CoordinateFrames,
    ActorRecords,
    CharacterActors,
    OutputPlayers,
    CoordinateAttempts,
    CoordinateSuccesses,
    RemoteReadCalls,
    RemoteReadBytes,
    RemoteReadFailures,
    CoordinateReadCalls,
    CoordinateReadBytes,
    CoordinateReadFailures,
    CoordinatePathAttempts,
    CoordinateFallbacks,
    BatchReadCalls,
    BatchReadItems,
    BatchReadBytes,
    RenderPlayers,
    DrawCommands,
    DrawVertices,
    DrawIndices,
    Count,
};

enum class PerformanceReadKind : std::uint8_t {
    Standard,
    Coordinate,
    Batch,
};

#if LENGJING_ENABLE_PERFORMANCE_TRACE

extern const bool gPerformanceTraceEnabled;

inline bool PerformanceTraceEnabled() noexcept {
    return gPerformanceTraceEnabled;
}

bool ShouldSamplePerformancePhase(
    PerformancePhase phase,
    std::uint32_t interval) noexcept;
void RecordPerformanceDuration(
    PerformancePhase phase,
    std::chrono::steady_clock::duration duration) noexcept;
void RecordPerformanceCount(
    PerformanceCounter counter,
    std::uint64_t value = 1) noexcept;
void RecordPerformanceRead(
    PerformanceReadKind kind,
    std::size_t bytes,
    bool succeeded,
    std::size_t items = 1) noexcept;
void PublishPerformanceTrace() noexcept;

class PerformanceTraceScope final {
public:
    explicit PerformanceTraceScope(
        PerformancePhase phase,
        std::uint32_t sampleInterval = 1) noexcept
        : phase_(phase),
          active_(PerformanceTraceEnabled() &&
                  (sampleInterval <= 1 ||
                   ShouldSamplePerformancePhase(
                       phase, sampleInterval))) {
        if (active_) startedAt_ = std::chrono::steady_clock::now();
    }
    ~PerformanceTraceScope() {
        Finish();
    }

    PerformanceTraceScope(const PerformanceTraceScope&) = delete;
    PerformanceTraceScope& operator=(const PerformanceTraceScope&) = delete;

    void Finish() noexcept {
        if (!active_) return;
        const auto finishedAt = std::chrono::steady_clock::now();
        active_ = false;
        RecordPerformanceDuration(phase_, finishedAt - startedAt_);
    }

private:
    PerformancePhase phase_;
    std::chrono::steady_clock::time_point startedAt_{};
    bool active_ = false;
};

#else

inline constexpr bool PerformanceTraceEnabled() noexcept {
    return false;
}

inline constexpr bool ShouldSamplePerformancePhase(
    PerformancePhase,
    std::uint32_t) noexcept {
    return false;
}

inline void RecordPerformanceDuration(
    PerformancePhase,
    std::chrono::steady_clock::duration) noexcept {}

inline void RecordPerformanceCount(
    PerformanceCounter,
    std::uint64_t = 1) noexcept {}

inline void RecordPerformanceRead(
    PerformanceReadKind,
    std::size_t,
    bool,
    std::size_t = 1) noexcept {}

inline void PublishPerformanceTrace() noexcept {}

class PerformanceTraceScope final {
public:
    explicit constexpr PerformanceTraceScope(
        PerformancePhase,
        std::uint32_t = 1) noexcept {}

    PerformanceTraceScope(const PerformanceTraceScope&) = delete;
    PerformanceTraceScope& operator=(const PerformanceTraceScope&) = delete;

    constexpr void Finish() noexcept {}
};

#endif

}  // namespace lengjing::platform
