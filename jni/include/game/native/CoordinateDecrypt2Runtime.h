#pragma once

#include "game/native/CoordinatePoolRuntime.h"

namespace lengjing::game::native {

constexpr CoordinatePoolRuntimeLayout MakeCoordinateDecrypt2RuntimeLayout(
    const CoordinatePoolRuntimeLayout& configured) noexcept {
    return configured;
}

class CoordinateDecrypt2Runtime final {
public:
    explicit CoordinateDecrypt2Runtime(
        CoordinatePoolRuntimeLayout layout = {});

    bool Configure(const CoordinatePoolRuntimeLayout& layout) noexcept;
    bool Refresh(MemoryTransport& memory,
                 pid_t processId,
                 std::uintptr_t moduleBase,
                 const ProcessExecutionContext& executionContext,
                 std::uint64_t frame) noexcept;
    bool ReadCandidates(std::uintptr_t component,
                        std::uint32_t decryptIndexOffset,
                        CoordinatePoolCandidateSet& candidates,
                        bool refresh = false) noexcept;
    bool ObserveOutputStability(
        std::uintptr_t component,
        std::uint32_t decryptIndexOffset,
        std::uint8_t blockCount,
        bool flicker) noexcept;
    CoordinatePoolRuntimeProbe Probe() const noexcept;
    void Reset() noexcept;

private:
    CoordinatePoolRuntime runtime_;
};

}  // namespace lengjing::game::native
