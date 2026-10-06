#pragma once
#include "gfx/VkCommon.h"
#include <vk_mem_alloc.h>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace df {
struct DeviceOptions {
    bool validation = false;
    GLFWwindow* window = nullptr;   // null: headless (offscreen only)
    int gpuIndex = -1;              // -1: pick the best discrete GPU
};

// Owns the Vulkan instance, device, the single graphics+compute queue and the
// memory allocator. Requires Vulkan 1.3 with descriptor indexing, draw
// parameters and indirect draw count (every GPU from GTX 10 / RX 400 / Arc up).
class Device {
public:
    void init(const DeviceOptions& opt);
    void shutdown();
    void waitIdle() const { vkDeviceWaitIdle(device); }

    // Records, submits and waits for a one-off command buffer (uploads, setup).
    void immediate(const std::function<void(VkCommandBuffer)>& record);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    VkPhysicalDeviceProperties props{};
    std::string gpuName;
    float timestampPeriodNs = 1.0f;
    bool timestamps = false;
    VkSampleCountFlags msaaSupport = VK_SAMPLE_COUNT_1_BIT;

private:
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkCommandPool immediatePool_ = VK_NULL_HANDLE;
    VkFence immediateFence_ = VK_NULL_HANDLE;
};
}  // namespace df
