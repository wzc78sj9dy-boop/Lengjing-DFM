#ifndef LENGJING_VULKAN_GRAPHICS_H
#define LENGJING_VULKAN_GRAPHICS_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "AndroidImgui.h"
#include "imgui_impl_vulkan.h"
#include "vulkan_wrapper.h"

class VulkanGraphics final : public AndroidImgui {
public:
    bool ConsumeSurfaceRecoveryRequest() override;

private:
    struct VulkanTextureData : BaseTexData {
        VkImageView ImageView = VK_NULL_HANDLE;
        VkImage Image = VK_NULL_HANDLE;
        VkDeviceMemory ImageMemory = VK_NULL_HANDLE;
        VkSampler Sampler = VK_NULL_HANDLE;
        VkBuffer UploadBuffer = VK_NULL_HANDLE;
        VkDeviceMemory UploadBufferMemory = VK_NULL_HANDLE;
    };

    bool Create() override;
    bool Setup() override;
    void PrepareFrame(bool resize) override;
    void Render(ImDrawData* drawData) override;
    void PrepareShutdown() override;
    void Cleanup() override;
    BaseTexData* LoadTexture(BaseTexData* texture, void* pixelData) override;
    void RemoveTexture(BaseTexData* texture) override;

    VkPhysicalDevice SetupVulkan_SelectPhysicalDevice();
    std::uint32_t findMemoryType(
        std::uint32_t typeFilter,
        VkMemoryPropertyFlags properties);
    void DisableRendering(const char* operation, VkResult error);
    bool WaitForQueueCompletion(const char* operation);
    void DestroyTextureResources(VulkanTextureData* texture);

    VkAllocationCallbacks* m_Allocator = nullptr;
    VkInstance m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    std::uint32_t m_QueueFamily = UINT32_MAX;
    VkQueue m_Queue = VK_NULL_HANDLE;
    VkPipelineCache m_PipelineCache = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

    // Texture uploads remain independent from in-flight frame command pools.
    VkCommandPool m_UploadPool = VK_NULL_HANDLE;
    VkFence m_UploadFence = VK_NULL_HANDLE;

    std::unique_ptr<ImGui_ImplVulkanH_Window> wd;
    int m_MinImageCount = 2;
    bool m_SwapChainRebuild = false;
    bool m_SkipRender = false;
    bool m_RenderingDisabled = false;
    bool m_ImGuiBackendInitialized = false;
    bool m_AbandonDevice = false;
    bool m_UploadInFlight = false;
    int m_LastWidth = 0;
    int m_LastHeight = 0;
    std::atomic<bool> m_SurfaceRecoveryRequested{false};
};

#endif
