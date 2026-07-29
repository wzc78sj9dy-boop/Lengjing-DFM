#pragma once

#include <cstdint>

namespace lengjing::game::native {

struct Arm64PacgaKey {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

constexpr std::uint64_t FormatArm64PacgaResult(
    std::uint64_t computedPac) noexcept {
    return computedPac & UINT64_C(0xFFFFFFFF00000000);
}

std::uint64_t ComputeArm64Pacga(
    std::uint64_t data,
    std::uint64_t modifier,
    const Arm64PacgaKey& key) noexcept;

}  // namespace lengjing::game::native
