#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace lengjing::render {

enum class VulkanPresentPreference : std::uint8_t {
    Mailbox,
    Fifo,
};

enum class VulkanSwapchainStatus : std::uint8_t {
    Ready,
    Suboptimal,
    OutOfDate,
    Failure,
};

inline constexpr std::array<VulkanPresentPreference, 2>
    kVulkanPresentPreferences{
        VulkanPresentPreference::Mailbox,
        VulkanPresentPreference::Fifo,
    };

constexpr std::uint32_t ResolveVulkanSwapchainImageCount(
    std::uint32_t surfaceMinimum,
    std::uint32_t surfaceMaximum) noexcept {
    constexpr std::uint32_t kPreferredImageCount = 3;
    std::uint32_t count = std::max(surfaceMinimum, kPreferredImageCount);
    if (surfaceMaximum != 0) {
        count = std::min(count, surfaceMaximum);
    }
    return count;
}

constexpr bool IsVulkanSwapchainUsable(
    VulkanSwapchainStatus status) noexcept {
    return status == VulkanSwapchainStatus::Ready ||
        status == VulkanSwapchainStatus::Suboptimal;
}

constexpr bool RequiresVulkanSwapchainRebuild(
    VulkanSwapchainStatus status) noexcept {
    return status == VulkanSwapchainStatus::OutOfDate;
}

}  // namespace lengjing::render
