#pragma once

#if 0

#include <cstddef>
#include <cstdint>
#include <functional>

namespace lengjing::game::native {

using ExecutionVeneerReadMemory =
    std::function<bool(std::uintptr_t, void*, std::size_t)>;

bool LocateSecondExecutionVeneer(
    std::uintptr_t moduleBase,
    std::uintptr_t firstVeneerRva,
    const ExecutionVeneerReadMemory& readMemory,
    std::uintptr_t& secondVeneerAddress);

}  // namespace lengjing::game::native

#endif
