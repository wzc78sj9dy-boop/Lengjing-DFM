#include "platform/PerformanceTrace.h"

#if LENGJING_ENABLE_PERFORMANCE_TRACE

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace lengjing::platform {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<std::uint64_t, 18> kHistogramUpperMicroseconds{
    25,
    50,
    100,
    200,
    500,
    1'000,
    2'000,
    4'000,
    8'000,
    12'000,
    16'667,
    25'000,
    33'333,
    50'000,
    75'000,
    100'000,
    250'000,
    std::numeric_limits<std::uint64_t>::max(),
};

constexpr std::array<const char*,
                     static_cast<std::size_t>(PerformancePhase::Count)>
    kPhaseNames{
        "data_frame",
        "backend_frame",
        "frame_context",
        "actor_snapshot",
        "actor_loop",
        "position_read",
        "coordinate_decode",
        "health_read",
        "bone_read",
        "projection",
        "world_objects",
        "render_frame",
        "draw_game_frame",
        "graphics_submit",
        "remote_read",
        "coordinate_remote_read",
        "batch_remote_read",
    };

struct PhaseAccumulator {
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> totalNanoseconds{0};
    std::atomic<std::uint64_t> maximumNanoseconds{0};
    std::array<std::atomic<std::uint64_t>, kHistogramUpperMicroseconds.size()>
        histogram{};
};

struct PhaseSnapshot {
    std::uint64_t samples = 0;
    std::uint64_t totalNanoseconds = 0;
    std::uint64_t maximumNanoseconds = 0;
    std::array<std::uint64_t, kHistogramUpperMicroseconds.size()>
        histogram{};
};

std::array<PhaseAccumulator,
           static_cast<std::size_t>(PerformancePhase::Count)>
    gPhases{};
std::array<std::atomic<std::uint64_t>,
           static_cast<std::size_t>(PerformanceCounter::Count)>
    gCounters{};
std::atomic<std::uint64_t> gLastPublishNanoseconds{0};

std::uint64_t NowNanoseconds() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

std::size_t HistogramIndex(std::uint64_t microseconds) noexcept {
    for (std::size_t index = 0;
         index < kHistogramUpperMicroseconds.size();
         ++index) {
        if (microseconds <= kHistogramUpperMicroseconds[index]) return index;
    }
    return kHistogramUpperMicroseconds.size() - 1;
}

void UpdateMaximum(
    std::atomic<std::uint64_t>& maximum,
    std::uint64_t candidate) noexcept {
    std::uint64_t observed = maximum.load(std::memory_order_relaxed);
    while (candidate > observed &&
           !maximum.compare_exchange_weak(
               observed,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

std::uint64_t QuantileUpperBound(
    const std::array<std::uint64_t, kHistogramUpperMicroseconds.size()>&
        histogram,
    std::uint64_t samples,
    std::uint64_t numerator,
    std::uint64_t denominator) noexcept {
    if (samples == 0) return 0;
    const std::uint64_t target =
        (samples * numerator + denominator - 1) / denominator;
    std::uint64_t accumulated = 0;
    for (std::size_t index = 0; index < histogram.size(); ++index) {
        accumulated += histogram[index];
        if (accumulated >= target) {
            const std::uint64_t upper = kHistogramUpperMicroseconds[index];
            return upper == std::numeric_limits<std::uint64_t>::max()
                ? 250'001
                : upper;
        }
    }
    return 250'001;
}

void ClearPerformanceAccumulators() noexcept {
    for (PhaseAccumulator& accumulator : gPhases) {
        accumulator.samples.exchange(0, std::memory_order_relaxed);
        accumulator.totalNanoseconds.exchange(0, std::memory_order_relaxed);
        accumulator.maximumNanoseconds.exchange(0, std::memory_order_relaxed);
        for (auto& bucket : accumulator.histogram) {
            bucket.exchange(0, std::memory_order_relaxed);
        }
    }
    for (auto& counter : gCounters) {
        counter.exchange(0, std::memory_order_relaxed);
    }
}

}  // namespace

extern const bool gPerformanceTraceEnabled = [] {
    const char* value = std::getenv("LENGJING_PERFORMANCE_TRACE");
    return value != nullptr && value[0] != '\0' &&
        value[0] != '0' && std::string_view(value) != "false";
}();

bool ShouldSamplePerformancePhase(PerformancePhase phase,
                                  std::uint32_t interval) noexcept {
    if (interval <= 1) return true;
    thread_local std::array<
        std::uint32_t,
        static_cast<std::size_t>(PerformancePhase::Count)> sequences{};
    const std::size_t index = static_cast<std::size_t>(phase);
    if (index >= sequences.size()) return false;
    const std::uint32_t sequence = ++sequences[index];
    return sequence == 1 || sequence % interval == 0;
}

void RecordPerformanceDuration(
    PerformancePhase phase,
    Clock::duration duration) noexcept {
    if (!PerformanceTraceEnabled()) return;
    const std::size_t phaseIndex = static_cast<std::size_t>(phase);
    if (phaseIndex >= gPhases.size()) return;
    const auto signedNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    if (signedNanoseconds < 0) return;
    const std::uint64_t nanoseconds =
        static_cast<std::uint64_t>(signedNanoseconds);
    PhaseAccumulator& accumulator = gPhases[phaseIndex];
    accumulator.samples.fetch_add(1, std::memory_order_relaxed);
    accumulator.totalNanoseconds.fetch_add(
        nanoseconds, std::memory_order_relaxed);
    UpdateMaximum(accumulator.maximumNanoseconds, nanoseconds);
    const std::uint64_t microseconds = (nanoseconds + 999) / 1'000;
    accumulator.histogram[HistogramIndex(microseconds)].fetch_add(
        1, std::memory_order_relaxed);
}

void RecordPerformanceCount(
    PerformanceCounter counter,
    std::uint64_t value) noexcept {
    if (!PerformanceTraceEnabled() || value == 0) return;
    const std::size_t index = static_cast<std::size_t>(counter);
    if (index >= gCounters.size()) return;
    gCounters[index].fetch_add(value, std::memory_order_relaxed);
}

void RecordPerformanceRead(PerformanceReadKind kind,
                           std::size_t bytes,
                           bool succeeded,
                           std::size_t items) noexcept {
    if (!PerformanceTraceEnabled()) return;
    const auto add = [](PerformanceCounter counter, std::uint64_t value) {
        if (value == 0) return;
        gCounters[static_cast<std::size_t>(counter)].fetch_add(
            value, std::memory_order_relaxed);
    };
    switch (kind) {
        case PerformanceReadKind::Standard:
            add(PerformanceCounter::RemoteReadCalls, 1);
            add(PerformanceCounter::RemoteReadBytes, bytes);
            if (!succeeded) {
                add(PerformanceCounter::RemoteReadFailures, 1);
            }
            break;
        case PerformanceReadKind::Coordinate:
            add(PerformanceCounter::CoordinateReadCalls, 1);
            add(PerformanceCounter::CoordinateReadBytes, bytes);
            if (!succeeded) {
                add(PerformanceCounter::CoordinateReadFailures, 1);
            }
            break;
        case PerformanceReadKind::Batch:
            add(PerformanceCounter::BatchReadCalls, 1);
            add(PerformanceCounter::BatchReadItems, items);
            add(PerformanceCounter::BatchReadBytes, bytes);
            break;
    }
}

void PublishPerformanceTrace() noexcept {
    if (!PerformanceTraceEnabled()) return;
    constexpr std::uint64_t kPublishIntervalNanoseconds = 1'000'000'000;
    const std::uint64_t now = NowNanoseconds();
    std::uint64_t previous =
        gLastPublishNanoseconds.load(std::memory_order_relaxed);
    if (previous == 0) {
        if (gLastPublishNanoseconds.compare_exchange_strong(
                previous, now, std::memory_order_relaxed)) {
            ClearPerformanceAccumulators();
        }
        return;
    }
    if (now - previous < kPublishIntervalNanoseconds ||
        !gLastPublishNanoseconds.compare_exchange_strong(
            previous, now, std::memory_order_relaxed)) {
        return;
    }
    const std::uint64_t windowMilliseconds = (now - previous) / 1'000'000;

    std::array<PhaseSnapshot,
               static_cast<std::size_t>(PerformancePhase::Count)>
        phaseSnapshots{};
    for (std::size_t index = 0; index < gPhases.size(); ++index) {
        PhaseAccumulator& accumulator = gPhases[index];
        PhaseSnapshot& snapshot = phaseSnapshots[index];
        snapshot.samples =
            accumulator.samples.exchange(0, std::memory_order_relaxed);
        snapshot.totalNanoseconds =
            accumulator.totalNanoseconds.exchange(
                0, std::memory_order_relaxed);
        snapshot.maximumNanoseconds =
            accumulator.maximumNanoseconds.exchange(
                0, std::memory_order_relaxed);
        for (std::size_t bucket = 0;
             bucket < snapshot.histogram.size();
             ++bucket) {
            snapshot.histogram[bucket] =
                accumulator.histogram[bucket].exchange(
                0, std::memory_order_relaxed);
        }
    }

    std::array<std::uint64_t,
               static_cast<std::size_t>(PerformanceCounter::Count)>
        counters{};
    for (std::size_t index = 0; index < counters.size(); ++index) {
        counters[index] =
            gCounters[index].exchange(0, std::memory_order_relaxed);
    }

    for (std::size_t index = 0; index < phaseSnapshots.size(); ++index) {
        const PhaseSnapshot& snapshot = phaseSnapshots[index];
        if (snapshot.samples == 0) continue;
        std::uint64_t histogramSamples = 0;
        for (const std::uint64_t count : snapshot.histogram) {
            histogramSamples += count;
        }
        const double averageMicroseconds =
            static_cast<double>(snapshot.totalNanoseconds) /
            static_cast<double>(snapshot.samples) / 1'000.0;
        std::fprintf(
            stderr,
            "[perf] schema=2 window_ms=%llu phase=%s samples=%llu "
            "avg_us=%.2f p50_us_le=%llu p95_us_le=%llu max_us=%.2f\n",
            static_cast<unsigned long long>(windowMilliseconds),
            kPhaseNames[index],
            static_cast<unsigned long long>(snapshot.samples),
            averageMicroseconds,
            static_cast<unsigned long long>(QuantileUpperBound(
                snapshot.histogram, histogramSamples, 50, 100)),
            static_cast<unsigned long long>(QuantileUpperBound(
                snapshot.histogram, histogramSamples, 95, 100)),
            static_cast<double>(snapshot.maximumNanoseconds) / 1'000.0);
    }
    const auto value = [&counters](PerformanceCounter counter) {
        return counters[static_cast<std::size_t>(counter)];
    };
    std::fprintf(
        stderr,
        "[perf-counts] schema=2 window_ms=%llu frames=%llu ready=%llu "
        "coordinate_frames=%llu actor_records=%llu characters=%llu "
        "players=%llu coordinate_attempts=%llu coordinate_successes=%llu "
        "reads=%llu read_bytes=%llu read_failures=%llu "
        "coordinate_reads=%llu coordinate_bytes=%llu "
        "coordinate_failures=%llu coordinate_path_attempts=%llu "
        "coordinate_fallbacks=%llu batches=%llu batch_items=%llu "
        "batch_bytes=%llu render_players=%llu draw_commands=%llu "
        "draw_vertices=%llu draw_indices=%llu\n",
        static_cast<unsigned long long>(windowMilliseconds),
        static_cast<unsigned long long>(value(PerformanceCounter::DataFrames)),
        static_cast<unsigned long long>(value(PerformanceCounter::ReadyFrames)),
        static_cast<unsigned long long>(value(PerformanceCounter::CoordinateFrames)),
        static_cast<unsigned long long>(value(PerformanceCounter::ActorRecords)),
        static_cast<unsigned long long>(value(PerformanceCounter::CharacterActors)),
        static_cast<unsigned long long>(value(PerformanceCounter::OutputPlayers)),
        static_cast<unsigned long long>(value(PerformanceCounter::CoordinateAttempts)),
        static_cast<unsigned long long>(value(PerformanceCounter::CoordinateSuccesses)),
        static_cast<unsigned long long>(value(PerformanceCounter::RemoteReadCalls)),
        static_cast<unsigned long long>(value(PerformanceCounter::RemoteReadBytes)),
        static_cast<unsigned long long>(value(PerformanceCounter::RemoteReadFailures)),
        static_cast<unsigned long long>(value(PerformanceCounter::CoordinateReadCalls)),
        static_cast<unsigned long long>(value(PerformanceCounter::CoordinateReadBytes)),
        static_cast<unsigned long long>(value(PerformanceCounter::CoordinateReadFailures)),
        static_cast<unsigned long long>(value(PerformanceCounter::CoordinatePathAttempts)),
        static_cast<unsigned long long>(value(PerformanceCounter::CoordinateFallbacks)),
        static_cast<unsigned long long>(value(PerformanceCounter::BatchReadCalls)),
        static_cast<unsigned long long>(value(PerformanceCounter::BatchReadItems)),
        static_cast<unsigned long long>(value(PerformanceCounter::BatchReadBytes)),
        static_cast<unsigned long long>(value(PerformanceCounter::RenderPlayers)),
        static_cast<unsigned long long>(value(PerformanceCounter::DrawCommands)),
        static_cast<unsigned long long>(value(PerformanceCounter::DrawVertices)),
        static_cast<unsigned long long>(value(PerformanceCounter::DrawIndices)));
    std::fflush(stderr);
}

}  // namespace lengjing::platform

#endif
