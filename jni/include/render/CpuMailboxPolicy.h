#pragma once

#include <algorithm>
#include <cstdint>

namespace lengjing::render {

struct CpuMailboxGenerationSnapshot final {
    std::uint64_t presentedGeneration = 0;
    std::uint64_t availableBufferGeneration = 0;

    constexpr std::uint64_t ReusableGeneration() const noexcept {
        return std::min(
            presentedGeneration, availableBufferGeneration);
    }
};

constexpr bool CpuMailboxNeedsCompleteCopy(
    std::uint64_t sampledBufferGeneration,
    std::uint64_t currentBufferGeneration) noexcept {
    return sampledBufferGeneration != currentBufferGeneration;
}

}  // namespace lengjing::render
