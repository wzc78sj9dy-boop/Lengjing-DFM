#include "game/native/CoordinateDecrypt2Runtime.h"

namespace lengjing::game::native {

CoordinateDecrypt2Runtime::CoordinateDecrypt2Runtime(
    CoordinatePoolRuntimeLayout layout)
    : runtime_(MakeCoordinateDecrypt2RuntimeLayout(layout)) {}

bool CoordinateDecrypt2Runtime::Configure(
    const CoordinatePoolRuntimeLayout& layout) noexcept {
    return runtime_.Configure(MakeCoordinateDecrypt2RuntimeLayout(layout));
}

bool CoordinateDecrypt2Runtime::Refresh(
    MemoryTransport& memory,
    pid_t processId,
    std::uintptr_t moduleBase,
    const ProcessExecutionContext& executionContext,
    std::uint64_t frame) noexcept {
    return runtime_.Refresh(
        memory,
        processId,
        moduleBase,
        executionContext,
        frame,
        true);
}

bool CoordinateDecrypt2Runtime::ReadCandidates(
    std::uintptr_t component,
    std::uint32_t decryptIndexOffset,
    CoordinatePoolCandidateSet& candidates,
    bool refresh) noexcept {
    return runtime_.ReadCandidates(
        component,
        decryptIndexOffset,
        candidates,
        refresh);
}

CoordinatePoolRuntimeProbe CoordinateDecrypt2Runtime::Probe() const noexcept {
    return runtime_.Probe();
}

void CoordinateDecrypt2Runtime::Reset() noexcept {
    runtime_.Reset();
}

}  // namespace lengjing::game::native
