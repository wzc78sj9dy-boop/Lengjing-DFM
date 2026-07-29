#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace lengjing::game::native {

using ExecutionVeneerReadMemory =
    std::function<bool(std::uintptr_t, void*, std::size_t)>;

enum class ExecutionVeneerMatchPolicy : std::uint8_t {
    Unique,
    OrderedFirst,
};

bool LocateSecondExecutionVeneer(
    std::uintptr_t moduleBase,
    std::uintptr_t firstVeneerRva,
    const ExecutionVeneerReadMemory& readMemory,
    std::uintptr_t& secondVeneerAddress,
    ExecutionVeneerMatchPolicy matchPolicy =
        ExecutionVeneerMatchPolicy::Unique);

}  // namespace lengjing::game::native
