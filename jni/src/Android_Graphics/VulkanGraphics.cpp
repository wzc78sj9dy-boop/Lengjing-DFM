#include "VulkanGraphics.h"

#include <android/native_window.h>
#include <vulkan/vulkan_android.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <new>
#include <unistd.h>

namespace {

constexpr uint64_t kFenceWaitTimeoutNs = 2ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr uint64_t kAcquireWaitTimeoutNs = 250ULL * 1000ULL * 1000ULL;
constexpr useconds_t kFailedFrameBackoffUs = 16000;
VkResult g_LastBackendError = VK_SUCCESS;

static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    g_LastBackendError = err;
}

static VkResult TakeBackendError() {
    const VkResult result = g_LastBackendError;
    g_LastBackendError = VK_SUCCESS;
    return result;
}

} // namespace

VkPhysicalDevice VulkanGraphics::SetupVulkan_SelectPhysicalDevice() {
    uint32_t gpu_count = 0;
    VkResult err = vkEnumeratePhysicalDevices(m_Instance, &gpu_count, nullptr);
    if (err != VK_SUCCESS || gpu_count == 0) {
        check_vk_result(err != VK_SUCCESS ? err : VK_ERROR_INITIALIZATION_FAILED);
        return VK_NULL_HANDLE;
    }

    ImVector<VkPhysicalDevice> gpus;
    gpus.resize(gpu_count);
    err = vkEnumeratePhysicalDevices(m_Instance, &gpu_count, gpus.Data);
    if (err != VK_SUCCESS || gpu_count == 0) {
        check_vk_result(err != VK_SUCCESS ? err : VK_ERROR_INITIALIZATION_FAILED);
        return VK_NULL_HANDLE;
    }

    // If a number >1 of GPUs got reported, find discrete GPU if present, or use first one available. This covers
    // most common cases (multi-gpu/integrated+dedicated graphics). Handling more complicated setups (multiple
    // dedicated GPUs) is out of scope of this sample.
    for (VkPhysicalDevice &device: gpus) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            return device;
    }

    // Use first GPU (Integrated) is a Discrete one is not available.
    if (gpu_count > 0)
        return gpus[0];
    return VK_NULL_HANDLE;
}

bool VulkanGraphics::Create() {
    std::snprintf(RenderName, sizeof(RenderName), "Vulkan");
    m_RenderingDisabled = false;
    m_SkipRender = false;
    m_SwapChainRebuild = false;
    m_ImGuiBackendInitialized = false;
    m_AbandonDevice = false;
    m_UploadInFlight = false;
    m_SurfaceRecoveryRequested.store(false, std::memory_order_release);
    g_LastBackendError = VK_SUCCESS;

    if (m_Window == nullptr)
        return false;
    if (InitVulkan() != 1) {
        fprintf(stderr, "[vulkan] Vulkan loader initialization failed\n");
        return false;
    }

    void *libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (libvulkan == nullptr) {
        fprintf(stderr, "[vulkan] dlopen(libvulkan.so) failed: %s\n", dlerror());
        return false;
    }
    const bool functions_loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, [](const char *function_name, void *handle) -> PFN_vkVoidFunction {
        return reinterpret_cast<PFN_vkVoidFunction>(dlsym(handle, function_name));
    }, libvulkan);
    dlclose(libvulkan);
    if (!functions_loaded || vkCreateAndroidSurfaceKHR == nullptr || vkAcquireNextImageKHR == nullptr ||
        vkQueuePresentKHR == nullptr || vkGetPhysicalDeviceSurfaceSupportKHR == nullptr) {
        fprintf(stderr, "[vulkan] required Vulkan functions are unavailable\n");
        return false;
    }

    wd = std::make_unique<ImGui_ImplVulkanH_Window>();
    auto fail = [&](const char *operation, VkResult result) {
        fprintf(stderr, "[vulkan] %s failed: VkResult = %d\n", operation, result);
        Cleanup();
        return false;
    };

    VkResult err = VK_SUCCESS;
    // Create Vulkan Instance
    {
        const char *instance_extensions[] = {
                "VK_KHR_surface",
                "VK_KHR_android_surface",
        };
        VkApplicationInfo appInfo = {
                .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                .pNext = nullptr,
                .pApplicationName = "lengjing",
                .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                .pEngineName = "native-overlay",
                .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                .apiVersion = VK_MAKE_VERSION(1, 1, 0),
        };
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &appInfo;
        create_info.enabledExtensionCount = sizeof(instance_extensions) / sizeof(instance_extensions[0]);
        create_info.ppEnabledExtensionNames = instance_extensions;
        err = vkCreateInstance(&create_info, m_Allocator, &m_Instance);
        if (err != VK_SUCCESS)
            return fail("vkCreateInstance", err);
    }

    // Select Physical Device (GPU)
    m_PhysicalDevice = SetupVulkan_SelectPhysicalDevice();
    if (m_PhysicalDevice == VK_NULL_HANDLE)
        return fail("vkEnumeratePhysicalDevices", VK_ERROR_INITIALIZATION_FAILED);

    // Select graphics queue family
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &count, nullptr);
        if (count == 0)
            return fail("vkGetPhysicalDeviceQueueFamilyProperties", VK_ERROR_INITIALIZATION_FAILED);
        ImVector<VkQueueFamilyProperties> queues;
        queues.resize((int)count);
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &count, queues.Data);
        for (uint32_t i = 0; i < count; i++)
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                m_QueueFamily = i;
                break;
            }
        if (m_QueueFamily == (uint32_t) -1)
            return fail("select graphics queue", VK_ERROR_INITIALIZATION_FAILED);
    }

    // Create Logical Device (with 1 queue)
    {
        ImVector<const char *> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // Enumerate physical device extension
        uint32_t properties_count = 0;
        ImVector<VkExtensionProperties> properties;
        err = vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &properties_count, nullptr);
        if (err != VK_SUCCESS)
            return fail("vkEnumerateDeviceExtensionProperties", err);
        properties.resize((int)properties_count);
        err = vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &properties_count, properties.Data);
        if (err != VK_SUCCESS)
            return fail("vkEnumerateDeviceExtensionProperties", err);
        bool has_swapchain = false;
        for (const VkExtensionProperties &property : properties)
            if (strcmp(property.extensionName, "VK_KHR_swapchain") == 0) {
                has_swapchain = true;
                break;
            }
        if (!has_swapchain)
            return fail("VK_KHR_swapchain", VK_ERROR_EXTENSION_NOT_PRESENT);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        for (const VkExtensionProperties &property : properties)
            if (strcmp(property.extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) == 0) {
                device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
                break;
            }
#endif

        const float queue_priority[] = {1.0f};
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = m_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t) device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(m_PhysicalDevice, &create_info, m_Allocator, &m_Device);
        if (err != VK_SUCCESS)
            return fail("vkCreateDevice", err);
        vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);
        if (m_Queue == VK_NULL_HANDLE)
            return fail("vkGetDeviceQueue", VK_ERROR_INITIALIZATION_FAILED);
    }
    // Create Descriptor Pool
    {
        constexpr uint32_t kDescriptorCount = 2048;
        VkDescriptorPoolSize pool_sizes[] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorCount}
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = kDescriptorCount;
        pool_info.poolSizeCount = (uint32_t) IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(m_Device, &pool_info, m_Allocator, &m_DescriptorPool);
        if (err != VK_SUCCESS)
            return fail("vkCreateDescriptorPool", err);
    }
    {
        // Create Window Surface
        VkSurfaceKHR surface;
        VkAndroidSurfaceCreateInfoKHR createInfo{
                .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                .pNext = nullptr,
                .flags = 0,
                .window = m_Window};

        err = vkCreateAndroidSurfaceKHR(m_Instance, &createInfo, m_Allocator,
                                        &surface);
        if (err != VK_SUCCESS)
            return fail("vkCreateAndroidSurfaceKHR", err);
        wd->Surface = surface;

        // Check for WSI support
        VkBool32 res = VK_FALSE;
        err = vkGetPhysicalDeviceSurfaceSupportKHR(m_PhysicalDevice, m_QueueFamily, wd->Surface, &res);
        if (err != VK_SUCCESS || res != VK_TRUE)
            return fail("vkGetPhysicalDeviceSurfaceSupportKHR",
                        err != VK_SUCCESS ? err : VK_ERROR_INITIALIZATION_FAILED);
        // Select Surface Format
        const VkFormat requestSurfaceImageFormat[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                                      VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
        const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
        wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(m_PhysicalDevice, wd->Surface,
                                                                  requestSurfaceImageFormat,
                                                                  (size_t) IM_ARRAYSIZE(requestSurfaceImageFormat),
                                                                  requestSurfaceColorSpace);
        if (wd->SurfaceFormat.format == VK_FORMAT_UNDEFINED)
            return fail("select surface format", VK_ERROR_FORMAT_NOT_SUPPORTED);

        // Select Present Mode
        VkPresentModeKHR present_modes[] = {
            VK_PRESENT_MODE_FIFO_KHR,
        };
        wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(m_PhysicalDevice, wd->Surface, &present_modes[0],
                                                              IM_ARRAYSIZE(present_modes));
        if (wd->PresentMode == VK_PRESENT_MODE_MAX_ENUM_KHR)
            return fail("select present mode", VK_ERROR_INITIALIZATION_FAILED);

        // Create SwapChain, RenderPass, Framebuffer, etc.
        if (!ImGui_ImplVulkanH_CreateOrResizeWindow(m_Instance, m_PhysicalDevice, m_Device, m_Queue,
                                                    wd.get(), m_QueueFamily, m_Allocator,
                                                    (int) m_Width, (int) m_Height, m_MinImageCount,
                                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                    kFenceWaitTimeoutNs))
            return fail("create swapchain", VK_ERROR_INITIALIZATION_FAILED);
        m_LastWidth = ANativeWindow_getWidth(m_Window);
        m_LastHeight = ANativeWindow_getHeight(m_Window);
    }

    return true;
}

bool VulkanGraphics::Setup() {
    if (m_Instance == VK_NULL_HANDLE || m_PhysicalDevice == VK_NULL_HANDLE ||
        m_Device == VK_NULL_HANDLE || m_Queue == VK_NULL_HANDLE || wd == nullptr ||
        wd->Swapchain == VK_NULL_HANDLE || wd->ImageCount < (uint32_t)m_MinImageCount)
        return false;

    g_LastBackendError = VK_SUCCESS;
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_1;
    init_info.Instance = m_Instance;
    init_info.PhysicalDevice = m_PhysicalDevice;
    init_info.Device = m_Device;
    init_info.QueueFamily = m_QueueFamily;
    init_info.Queue = m_Queue;
    init_info.PipelineCache = m_PipelineCache;
    init_info.DescriptorPool = m_DescriptorPool;
    init_info.MinImageCount = m_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    // Dear ImGui 1.92 stores the render-pass state in PipelineInfoMain.
    init_info.PipelineInfoMain.RenderPass = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = m_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    const bool backend_initialized = ImGui_ImplVulkan_Init(&init_info);
    const VkResult backend_error = TakeBackendError();
    if (!backend_initialized || backend_error != VK_SUCCESS) {
        if (backend_initialized)
            ImGui_ImplVulkan_Shutdown();
        return false;
    }
    m_ImGuiBackendInitialized = true;

    // Keep texture transfers isolated from all frame command pools.
    {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // The complete transient pool is reset before each upload.
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                        | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = m_QueueFamily;
        VkResult err = vkCreateCommandPool(m_Device, &pool_info, m_Allocator, &m_UploadPool);
        if (err != VK_SUCCESS) {
            ImGui_ImplVulkan_Shutdown();
            m_ImGuiBackendInitialized = false;
            return false;
        }

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        err = vkCreateFence(m_Device, &fence_info, m_Allocator, &m_UploadFence);
        if (err != VK_SUCCESS) {
            vkDestroyCommandPool(m_Device, m_UploadPool, m_Allocator);
            m_UploadPool = VK_NULL_HANDLE;
            ImGui_ImplVulkan_Shutdown();
            m_ImGuiBackendInitialized = false;
            return false;
        }
    }
    return true;
}

void VulkanGraphics::PrepareFrame(bool resize) {
    m_SkipRender = false;
    if (m_RenderingDisabled) {
        m_SkipRender = true;
        usleep(kFailedFrameBackoffUs);
        return;
    }

    const int width = ANativeWindow_getWidth(m_Window);
    const int height = ANativeWindow_getHeight(m_Window);
    if (width <= 0 || height <= 0) {
        m_SkipRender = true;
        usleep(kFailedFrameBackoffUs);
        return;
    }

    const bool size_changed = width != m_LastWidth || height != m_LastHeight;
    if (m_SwapChainRebuild || size_changed) {
        g_LastBackendError = VK_SUCCESS;
        if (!ImGui_ImplVulkanH_CreateOrResizeWindow(m_Instance, m_PhysicalDevice, m_Device, m_Queue,
                                                    wd.get(), m_QueueFamily, m_Allocator,
                                                    width, height, m_MinImageCount,
                                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                    kFenceWaitTimeoutNs) ||
            !ImGui_ImplVulkan_SetImageCount(wd->ImageCount)) {
            VkResult result = TakeBackendError();
            if (result == VK_SUCCESS)
                result = VK_ERROR_INITIALIZATION_FAILED;
            DisableRendering("rebuild swapchain", result);
            return;
        }
        wd->FrameIndex = 0;
        wd->SemaphoreIndex = 0;
        m_LastWidth = width;
        m_LastHeight = height;
        m_SwapChainRebuild = false;
    }

    (void) resize;
    g_LastBackendError = VK_SUCCESS;
    ImGui_ImplVulkan_NewFrame();
    const VkResult backendError = TakeBackendError();
    if (backendError != VK_SUCCESS) {
        DisableRendering("ImGui_ImplVulkan_NewFrame", backendError);
    }
}

void VulkanGraphics::Render(ImDrawData *drawData) {
    if (m_SkipRender || m_RenderingDisabled)
        return;
    if (wd == nullptr || wd->Swapchain == VK_NULL_HANDLE || wd->ImageCount == 0 ||
        wd->SemaphoreCount == 0 || wd->Frames.Size < (int)wd->ImageCount ||
        wd->FrameSemaphores.Size < (int)wd->SemaphoreCount ||
        wd->SemaphoreIndex >= wd->SemaphoreCount) {
        DisableRendering("invalid swapchain state", VK_ERROR_INITIALIZATION_FAILED);
        return;
    }

    VkResult err;
    bool rebuild_after_present = false;

    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    err = vkAcquireNextImageKHR(m_Device, wd->Swapchain, kAcquireWaitTimeoutNs,
                                image_acquired_semaphore, VK_NULL_HANDLE,
                                &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR /*|| err == VK_SUBOPTIMAL_KHR*/) {
        m_SwapChainRebuild = true;
        return;
    }
    if (err == VK_TIMEOUT || err == VK_NOT_READY) {
        m_SkipRender = true;
        usleep(kFailedFrameBackoffUs);
        return;
    }
    if (err == VK_SUBOPTIMAL_KHR) {
        rebuild_after_present = true;
    } else if (err != VK_SUCCESS) {
        DisableRendering("vkAcquireNextImageKHR", err);
        return;
    }

    ImGui_ImplVulkanH_Frame *fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(m_Device, 1, &fd->Fence, VK_TRUE,
                              kFenceWaitTimeoutNs);
        if (err != VK_SUCCESS) {
            DisableRendering("vkWaitForFences", err);
            return;
        }

    }
    {
        err = vkResetCommandPool(m_Device, fd->CommandPool, 0);
        if (err != VK_SUCCESS) {
            DisableRendering("vkResetCommandPool", err);
            return;
        }
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        if (err != VK_SUCCESS) {
            DisableRendering("vkBeginCommandBuffer", err);
            return;
        }
    }
    {
        // The helper initializes the clear value to transparent black.
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Record dear imgui primitives into command buffer
    g_LastBackendError = VK_SUCCESS;
    ImGui_ImplVulkan_RenderDrawData(drawData, fd->CommandBuffer);
    const VkResult backend_error = TakeBackendError();
    if (backend_error != VK_SUCCESS) {
        DisableRendering("ImGui_ImplVulkan_RenderDrawData", backend_error);
        return;
    }

    // Submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        if (err != VK_SUCCESS) {
            DisableRendering("vkEndCommandBuffer", err);
            return;
        }
        err = vkResetFences(m_Device, 1, &fd->Fence);
        if (err != VK_SUCCESS) {
            DisableRendering("vkResetFences", err);
            return;
        }
        err = vkQueueSubmit(m_Queue, 1, &info, fd->Fence);
        if (err != VK_SUCCESS) {
            DisableRendering("vkQueueSubmit", err);
            return;
        }
    }

    {
        VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
        VkPresentInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &render_complete_semaphore;
        info.swapchainCount = 1;
        info.pSwapchains = &wd->Swapchain;
        info.pImageIndices = &wd->FrameIndex;
        VkResult err = vkQueuePresentKHR(m_Queue, &info);
        if (err == VK_ERROR_OUT_OF_DATE_KHR /*|| err == VK_SUBOPTIMAL_KHR*/) {
            m_SwapChainRebuild = true;
            return;
        }
        if (err == VK_SUBOPTIMAL_KHR) {
            rebuild_after_present = true;
        } else if (err != VK_SUCCESS) {
            DisableRendering("vkQueuePresentKHR", err);
            return;
        }
        RecordPresentedFrame();
        wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount; // Now we can use the next set of semaphores
        if (rebuild_after_present)
            m_SwapChainRebuild = true;
    }
}

void VulkanGraphics::DisableRendering(const char *operation, VkResult error) {
    fprintf(stderr, "[vulkan] %s failed: VkResult = %d; rendering disabled\n",
            operation, error);
    if (error == VK_ERROR_DEVICE_LOST || error == VK_TIMEOUT)
        m_AbandonDevice = true;
    m_SurfaceRecoveryRequested.store(true, std::memory_order_release);
    m_RenderingDisabled = true;
    m_SkipRender = true;
}

bool VulkanGraphics::ConsumeSurfaceRecoveryRequest() {
    if (!m_SurfaceRecoveryRequested.load(std::memory_order_acquire))
        return false;
    return m_SurfaceRecoveryRequested.exchange(
        false, std::memory_order_acq_rel);
}

bool VulkanGraphics::WaitForQueueCompletion(const char *operation) {
    if (m_AbandonDevice)
        return false;
    if (m_Device == VK_NULL_HANDLE || m_Queue == VK_NULL_HANDLE || wd == nullptr)
        return true;

    g_LastBackendError = VK_SUCCESS;
    if (ImGui_ImplVulkanH_WaitForQueue(m_Device, m_Queue, wd.get(), m_Allocator,
                                       kFenceWaitTimeoutNs)) {
        m_UploadInFlight = false;
        return true;
    }

    VkResult result = TakeBackendError();
    if (result == VK_SUCCESS)
        result = VK_TIMEOUT;
    fprintf(stderr, "[vulkan] %s failed while waiting for the queue: VkResult = %d\n",
            operation, result);
    m_SurfaceRecoveryRequested.store(true, std::memory_order_release);
    if (result == VK_TIMEOUT || result == VK_ERROR_DEVICE_LOST)
        m_AbandonDevice = true;
    return false;
}

void VulkanGraphics::PrepareShutdown() {
    if (!m_ImGuiBackendInitialized)
        return;
    if (!m_AbandonDevice && !WaitForQueueCompletion("shutdown"))
        m_AbandonDevice = true;
    ImGui_ImplVulkan_Shutdown(!m_AbandonDevice);
    m_ImGuiBackendInitialized = false;
}

void VulkanGraphics::Cleanup() {
    if (m_AbandonDevice) {
        fprintf(stderr, "[vulkan] abandoning an unresponsive device without blocking cleanup\n");
        wd.reset();
        m_UploadFence = VK_NULL_HANDLE;
        m_UploadPool = VK_NULL_HANDLE;
        m_DescriptorPool = VK_NULL_HANDLE;
        m_Queue = VK_NULL_HANDLE;
        m_Device = VK_NULL_HANDLE;
        m_PhysicalDevice = VK_NULL_HANDLE;
        m_Instance = VK_NULL_HANDLE;
        m_QueueFamily = UINT32_MAX;
        m_LastWidth = 0;
        m_LastHeight = 0;
        return;
    }

    // Upload synchronization objects must be gone before the device.
    if (m_Device != VK_NULL_HANDLE && m_UploadFence != VK_NULL_HANDLE) {
        vkDestroyFence(m_Device, m_UploadFence, m_Allocator);
        m_UploadFence = VK_NULL_HANDLE;
    }
    if (m_Device != VK_NULL_HANDLE && m_UploadPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_Device, m_UploadPool, m_Allocator);
        m_UploadPool = VK_NULL_HANDLE;
    }

    if (wd != nullptr)
        ImGui_ImplVulkanH_DestroyWindow(m_Instance, m_Device, wd.get(), m_Allocator);
    wd.reset();
    if (m_Device != VK_NULL_HANDLE && m_DescriptorPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, m_Allocator);
    m_DescriptorPool = VK_NULL_HANDLE;
    if (m_Device != VK_NULL_HANDLE)
        vkDestroyDevice(m_Device, m_Allocator);
    if (m_Instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_Instance, m_Allocator);
    m_Queue = VK_NULL_HANDLE;
    m_Device = VK_NULL_HANDLE;
    m_PhysicalDevice = VK_NULL_HANDLE;
    m_Instance = VK_NULL_HANDLE;
    m_QueueFamily = UINT32_MAX;
    m_LastWidth = 0;
    m_LastHeight = 0;
}

// Helper function to find Vulkan memory type bits. See ImGui_ImplVulkan_MemoryType() in imgui_impl_vulkan.cpp
uint32_t VulkanGraphics::findMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++)
        if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;

    return 0xFFFFFFFF; // Unable to find memoryType
}

BaseTexData *VulkanGraphics::LoadTexture(BaseTexData *tex, void *pixel_data) {
    if (tex == nullptr || pixel_data == nullptr || m_RenderingDisabled || m_AbandonDevice ||
        !m_ImGuiBackendInitialized || m_Device == VK_NULL_HANDLE || m_UploadPool == VK_NULL_HANDLE ||
        m_UploadFence == VK_NULL_HANDLE || tex->Width <= 0 || tex->Height <= 0 || tex->Channels != 4)
        return nullptr;

    const size_t width = (size_t)tex->Width;
    const size_t height = (size_t)tex->Height;
    const size_t channels = (size_t)tex->Channels;
    if (width > SIZE_MAX / height || width * height > SIZE_MAX / channels)
        return nullptr;
    const size_t image_size = width * height * channels;
    if (image_size == 0)
        return nullptr;

    auto *tex_data = new (std::nothrow) VulkanTextureData();
    if (tex_data == nullptr)
        return nullptr;
    tex_data->Width = tex->Width;
    tex_data->Height = tex->Height;
    tex_data->Channels = tex->Channels;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    auto fail = [&](const char *operation, VkResult result) -> BaseTexData * {
        fprintf(stderr, "[vulkan] texture upload %s failed: VkResult = %d\n", operation, result);
        if (m_UploadInFlight) {
            // The queue accepted this upload, but the fence never confirmed
            // completion. No Vulkan object referenced by it is safe to free.
            m_AbandonDevice = true;
            DisableRendering(operation, result);
        } else if (result == VK_ERROR_DEVICE_LOST || result == VK_TIMEOUT) {
            DisableRendering(operation, result);
        }
        if (command_buffer != VK_NULL_HANDLE && !m_AbandonDevice && !m_UploadInFlight)
            vkFreeCommandBuffers(m_Device, m_UploadPool, 1, &command_buffer);
        DestroyTextureResources(tex_data);
        return nullptr;
    };

    VkResult err = VK_SUCCESS;
    // Create the Vulkan image.
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent.width = tex_data->Width;
        info.extent.height = tex_data->Height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        err = vkCreateImage(m_Device, &info, m_Allocator, &tex_data->Image);
        if (err != VK_SUCCESS)
            return fail("vkCreateImage", err);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_Device, tex_data->Image, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc_info.memoryTypeIndex == 0xFFFFFFFF)
            return fail("find image memory type", VK_ERROR_FEATURE_NOT_PRESENT);
        err = vkAllocateMemory(m_Device, &alloc_info, m_Allocator, &tex_data->ImageMemory);
        if (err != VK_SUCCESS)
            return fail("vkAllocateMemory(image)", err);
        err = vkBindImageMemory(m_Device, tex_data->Image, tex_data->ImageMemory, 0);
        if (err != VK_SUCCESS)
            return fail("vkBindImageMemory", err);
    }

    // Create the Image View
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = tex_data->Image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        err = vkCreateImageView(m_Device, &info, m_Allocator, &tex_data->ImageView);
        if (err != VK_SUCCESS)
            return fail("vkCreateImageView", err);
    }

    // Create Sampler
    {
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.minLod = -1000;
        sampler_info.maxLod = 1000;
        sampler_info.maxAnisotropy = 1.0f;
        err = vkCreateSampler(m_Device, &sampler_info, m_Allocator, &tex_data->Sampler);
        if (err != VK_SUCCESS)
            return fail("vkCreateSampler", err);
    }

    // Create Descriptor Set using ImGUI's implementation
    g_LastBackendError = VK_SUCCESS;
    tex_data->DS = (void *) ImGui_ImplVulkan_AddTexture(tex_data->Sampler, tex_data->ImageView,
                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (tex_data->DS == nullptr) {
        VkResult result = TakeBackendError();
        if (result == VK_SUCCESS)
            result = VK_ERROR_OUT_OF_POOL_MEMORY;
        return fail("ImGui_ImplVulkan_AddTexture", result);
    }

    // Create Upload Buffer
    {
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = image_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        err = vkCreateBuffer(m_Device, &buffer_info, m_Allocator, &tex_data->UploadBuffer);
        if (err != VK_SUCCESS)
            return fail("vkCreateBuffer", err);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_Device, tex_data->UploadBuffer, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (alloc_info.memoryTypeIndex == 0xFFFFFFFF)
            return fail("find upload memory type", VK_ERROR_FEATURE_NOT_PRESENT);
        err = vkAllocateMemory(m_Device, &alloc_info, m_Allocator, &tex_data->UploadBufferMemory);
        if (err != VK_SUCCESS)
            return fail("vkAllocateMemory(upload)", err);
        err = vkBindBufferMemory(m_Device, tex_data->UploadBuffer, tex_data->UploadBufferMemory, 0);
        if (err != VK_SUCCESS)
            return fail("vkBindBufferMemory", err);
    }

    // Upload to Buffer:
    {
        void *map = nullptr;
        err = vkMapMemory(m_Device, tex_data->UploadBufferMemory, 0, VK_WHOLE_SIZE, 0, &map);
        if (err != VK_SUCCESS || map == nullptr)
            return fail("vkMapMemory", err != VK_SUCCESS ? err : VK_ERROR_MEMORY_MAP_FAILED);
        memcpy(map, pixel_data, image_size);
        VkMappedMemoryRange range[1] = {};
        range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[0].memory = tex_data->UploadBufferMemory;
        range[0].size = VK_WHOLE_SIZE;
        err = vkFlushMappedMemoryRanges(m_Device, 1, range);
        vkUnmapMemory(m_Device, tex_data->UploadBufferMemory);
        if (err != VK_SUCCESS)
            return fail("vkFlushMappedMemoryRanges", err);
    }

    // Reuse only the dedicated texture-upload command pool.
    VkCommandPool command_pool = m_UploadPool;
    {
        // Begin each upload from an empty transient pool.
        err = vkResetCommandPool(m_Device, command_pool, 0);
        if (err != VK_SUCCESS)
            return fail("vkResetCommandPool", err);

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandPool = command_pool;
        alloc_info.commandBufferCount = 1;

        err = vkAllocateCommandBuffers(m_Device, &alloc_info, &command_buffer);
        if (err != VK_SUCCESS)
            return fail("vkAllocateCommandBuffers", err);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(command_buffer, &begin_info);
        if (err != VK_SUCCESS)
            return fail("vkBeginCommandBuffer", err);
    }

    // Copy to Image
    {
        VkImageMemoryBarrier copy_barrier[1] = {};
        copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].image = tex_data->Image;
        copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_barrier[0].subresourceRange.levelCount = 1;
        copy_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                             0,
                             NULL, 1, copy_barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = tex_data->Width;
        region.imageExtent.height = tex_data->Height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(command_buffer, tex_data->UploadBuffer, tex_data->Image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier use_barrier[1] = {};
        use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].image = tex_data->Image;
        use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        use_barrier[0].subresourceRange.levelCount = 1;
        use_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0, NULL, 0, NULL, 1, use_barrier);
    }

    // End command buffer
    {
        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &command_buffer;
        err = vkEndCommandBuffer(command_buffer);
        if (err != VK_SUCCESS)
            return fail("vkEndCommandBuffer", err);
        // Wait only for this transfer, never for every in-flight frame.
        err = vkResetFences(m_Device, 1, &m_UploadFence);
        if (err != VK_SUCCESS)
            return fail("vkResetFences", err);
        err = vkQueueSubmit(m_Queue, 1, &end_info, m_UploadFence);
        if (err != VK_SUCCESS)
            return fail("vkQueueSubmit", err);
        m_UploadInFlight = true;
        err = vkWaitForFences(m_Device, 1, &m_UploadFence, VK_TRUE,
                              kFenceWaitTimeoutNs);
        if (err != VK_SUCCESS) {
            return fail("vkWaitForFences", err);
        }
        m_UploadInFlight = false;
    }

    vkFreeCommandBuffers(m_Device, command_pool, 1, &command_buffer);
    vkDestroyBuffer(m_Device, tex_data->UploadBuffer, m_Allocator);
    tex_data->UploadBuffer = VK_NULL_HANDLE;
    vkFreeMemory(m_Device, tex_data->UploadBufferMemory, m_Allocator);
    tex_data->UploadBufferMemory = VK_NULL_HANDLE;

    return tex_data;
}

void VulkanGraphics::DestroyTextureResources(VulkanTextureData *tex_data) {
    if (tex_data == nullptr)
        return;
    if (!m_AbandonDevice && m_Device != VK_NULL_HANDLE) {
        if (tex_data->DS != nullptr && m_ImGuiBackendInitialized)
            ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet) tex_data->DS);
        if (tex_data->UploadBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_Device, tex_data->UploadBuffer, m_Allocator);
        if (tex_data->UploadBufferMemory != VK_NULL_HANDLE)
            vkFreeMemory(m_Device, tex_data->UploadBufferMemory, m_Allocator);
        if (tex_data->Sampler != VK_NULL_HANDLE)
            vkDestroySampler(m_Device, tex_data->Sampler, m_Allocator);
        if (tex_data->ImageView != VK_NULL_HANDLE)
            vkDestroyImageView(m_Device, tex_data->ImageView, m_Allocator);
        if (tex_data->Image != VK_NULL_HANDLE)
            vkDestroyImage(m_Device, tex_data->Image, m_Allocator);
        if (tex_data->ImageMemory != VK_NULL_HANDLE)
            vkFreeMemory(m_Device, tex_data->ImageMemory, m_Allocator);
    }
    delete tex_data;
}

void VulkanGraphics::RemoveTexture(BaseTexData *tex) {
    auto *tex_data = static_cast<VulkanTextureData *>(tex);
    if (tex_data == nullptr)
        return;
    if (!m_AbandonDevice && !WaitForQueueCompletion("texture removal"))
        m_AbandonDevice = true;
    DestroyTextureResources(tex_data);
}
