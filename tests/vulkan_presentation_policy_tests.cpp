#include "test_support.h"

#include "render/VulkanPresentationPolicy.h"

void RunVulkanPresentationPolicyTests() {
    using lengjing::render::ResolveVulkanSwapchainImageCount;
    using lengjing::render::VulkanPresentPreference;
    using lengjing::render::kVulkanPresentPreferences;

    REQUIRE(kVulkanPresentPreferences[0] ==
            VulkanPresentPreference::Mailbox);
    REQUIRE(kVulkanPresentPreferences[1] ==
            VulkanPresentPreference::Fifo);

    REQUIRE(ResolveVulkanSwapchainImageCount(2, 0) == 3);
    REQUIRE(ResolveVulkanSwapchainImageCount(3, 0) == 3);
    REQUIRE(ResolveVulkanSwapchainImageCount(4, 0) == 4);
    REQUIRE(ResolveVulkanSwapchainImageCount(2, 2) == 2);
    REQUIRE(ResolveVulkanSwapchainImageCount(2, 4) == 3);
    REQUIRE(ResolveVulkanSwapchainImageCount(4, 4) == 4);

    const auto initial_count =
        ResolveVulkanSwapchainImageCount(2, 0);
    const auto constrained_count =
        ResolveVulkanSwapchainImageCount(2, 2);
    const auto restored_count =
        ResolveVulkanSwapchainImageCount(2, 4);
    REQUIRE(initial_count == 3);
    REQUIRE(constrained_count == 2);
    REQUIRE(restored_count == 3);
}
