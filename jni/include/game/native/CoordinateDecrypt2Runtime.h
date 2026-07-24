#pragma once

#include "game/native/CoordinatePoolRuntime.h"

namespace lengjing::game::native {

constexpr CoordinatePoolRuntimeLayout MakeCoordinateDecrypt2RuntimeLayout(
    const CoordinatePoolRuntimeLayout& configured) noexcept {
    CoordinatePoolRuntimeLayout layout{
        0x0E738950,
        0x0C,
        -8,
        0xA0,
        0x210,
        0x30,
        0x10,
        configured.ringRefreshFrames,
    };
    return layout;
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
    CoordinatePoolRuntimeProbe Probe() const noexcept;
    void Reset() noexcept;

private:
    CoordinatePoolRuntime runtime_;
};

}  // namespace lengjing::game::native
