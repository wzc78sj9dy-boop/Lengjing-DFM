#pragma once

#include <cstddef>
#include <cstdint>

namespace lengjing::game::native {

struct PacgaOperands {
    std::uint64_t data = 0;
    std::uint64_t modifier = 0;
};

bool ResolvePacgaOperandsFromImmediateBlock(
    const std::uint32_t* instructions,
    std::size_t instructionCount,
    std::size_t pacgaIndex,
    PacgaOperands& operands) noexcept;

}  // namespace lengjing::game::native
