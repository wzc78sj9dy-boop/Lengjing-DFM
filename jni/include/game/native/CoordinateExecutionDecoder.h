#pragma once

#include "game/native/CoordinateExecutionRuntime.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace lengjing::game::native {

class MemoryTransport;
struct ProcessExecutionContext;

inline constexpr std::size_t kCoordinateExecutionAttemptsPerDecode = 64;
inline constexpr std::uint64_t kCoordinateExecutionTraversalLimitMs = 1000;

enum class CoordinateExecutionDecoderStage : std::uint8_t {
    Idle,
    Configured,
    Discovering,
    Ready,
    Executing,
    Completed,
    Failed,
};

enum class CoordinateExecutionDecoderError : std::uint8_t {
    None,
    InvalidMode,
    InvalidRefresh,
    CandidateDiscoveryFailed,
    NotReady,
    BackendUnavailable,
    ExecutionFailed,
};

struct CoordinateExecutionDecoderProbe {
    CoordinateExecutionDecoderStage stage =
        CoordinateExecutionDecoderStage::Idle;
    CoordinateExecutionDecoderError error =
        CoordinateExecutionDecoderError::None;
    CoordinateExecutionStatus status = CoordinateExecutionStatus::Idle;
    CoordinateExecutionMode mode = CoordinateExecutionMode::Emulate;
    std::int32_t processId = 0;
    std::uintptr_t moduleBase = 0;
    std::size_t moduleSize = 0;
    std::uintptr_t codeBase = 0;
    std::size_t codeSize = 0;
    std::uint64_t contextGeneration = 0;
    std::size_t candidateCount = 0;
    std::size_t cursor = 0;
    std::size_t attemptsThisDecode = 0;
    std::uint64_t totalAttempts = 0;
    std::size_t lastCandidateIndex = 0;
    std::uint64_t lastObject = 0;
    bool configured = false;
    bool refreshed = false;
    bool candidatesTruncated = false;
    bool knownCandidate = false;
    bool candidateLimitReached = false;
    bool traversalTimeLimitReached = false;
    CoordinateExecutionCandidate cachedCandidate{};
    CoordinateExecutionCandidate lastCandidate{};
    CoordinateExecutionRuntimeProbe runtime{};
};

using CoordinateExecutionDecoderExecuteCallback = std::function<
    CoordinateExecutionResult(
        std::uintptr_t subject,
        const CoordinateExecutionRequest& request)>;
using CoordinateExecutionDecoderProbeCallback =
    std::function<CoordinateExecutionRuntimeProbe()>;
using CoordinateExecutionDecoderResetCallback = std::function<void()>;
using CoordinateExecutionDecoderClockCallback =
    std::function<std::uint64_t()>;

struct CoordinateExecutionDecoderHooks {
    CoordinateExecutionDecoderExecuteCallback execute;
    CoordinateExecutionDecoderProbeCallback probe;
    CoordinateExecutionDecoderResetCallback reset;
    CoordinateExecutionDecoderClockCallback nowMilliseconds;
};

class CoordinateExecutionDecoder final {
public:
    CoordinateExecutionDecoder();
    explicit CoordinateExecutionDecoder(
        CoordinateExecutionDecoderHooks hooks);
    ~CoordinateExecutionDecoder();

    CoordinateExecutionDecoder(const CoordinateExecutionDecoder&) = delete;
    CoordinateExecutionDecoder& operator=(
        const CoordinateExecutionDecoder&) = delete;

    bool Configure(CoordinateExecutionMode mode,
                   const CoordinateExecutionLayout& layout) noexcept;

    bool Refresh(MemoryTransport& memory,
                 std::int32_t processId,
                 std::uintptr_t moduleBase,
                 std::size_t moduleSize,
                 std::uintptr_t codeBase,
                 std::size_t codeSize,
                 const ProcessExecutionContext& executionContext) noexcept;

    bool Refresh(const CoordinateExecutionReadCallback& read,
                 std::int32_t processId,
                 std::uintptr_t moduleBase,
                 std::size_t moduleSize,
                 std::uintptr_t codeBase,
                 std::size_t codeSize,
                 const ProcessExecutionContext& executionContext) noexcept;

    bool Decode(std::uintptr_t subject,
                CoordinateExecutionPosition& position) noexcept;

    CoordinateExecutionDecoderProbe Probe() const noexcept;
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
