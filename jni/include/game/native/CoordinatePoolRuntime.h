#pragma once

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
    EntryMappingMissing,
    CodeReadFailed,
    AnalysisFailed,
    EngineSetupFailed,
    ContextMissing,
    ParameterExecutionFailed,
    ParameterReadFailed,
    PoolPointerReadFailed,
    RingSearchFailed,
    PositionReadFailed,
};

struct CoordinatePoolRuntimeProbe {
    CoordinatePoolRuntimeStage stage = CoordinatePoolRuntimeStage::Idle;
    CoordinatePoolRuntimeError error = CoordinatePoolRuntimeError::None;
    std::uintptr_t bridge = 0;
    std::uintptr_t context = 0;
    std::uintptr_t guestEntry = 0;
    std::uintptr_t codeBase = 0;
    std::size_t codeSize = 0;
    std::int32_t poolPointerOffset = 0;
    std::int64_t indexOffset = 0;
    std::uint32_t ringOffset = 0;
    std::int32_t threadId = 0;
    std::uint64_t contextGeneration = 0;
    std::uint64_t attempts = 0;
    std::uint64_t successes = 0;
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
                      CoordinatePoolPosition& position) noexcept;
    CoordinatePoolRuntimeProbe Probe() const noexcept;
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
