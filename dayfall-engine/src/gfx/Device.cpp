#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "gfx/Device.h"
#include <GLFW/glfw3.h>
#include <cstring>

namespace df {
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                    void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) logError("vulkan: {}", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) logWarn("vulkan: {}", data->pMessage);
    return VK_FALSE;
}

static bool hasLayer(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

void Device::init(const DeviceOptions& opt) {
    VK_CHECK(volkInitialize());
    uint32_t apiVersion = 0;
    vkEnumerateInstanceVersion(&apiVersion);
    if (apiVersion < VK_API_VERSION_1_3) fatal("Vulkan 1.3 is required; update the GPU driver");

    std::vector<const char*> exts, layers;
    if (opt.window) {
        uint32_t n = 0;
        const char** e = glfwGetRequiredInstanceExtensions(&n);
        exts.assign(e, e + n);
    }
    bool validation = opt.validation && hasLayer("VK_LAYER_KHRONOS_validation");
    if (opt.validation && !validation) logWarn("validation requested but VK_LAYER_KHRONOS_validation is not installed");
    if (validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DAYFALL";
    app.pEngineName = "DAYFALL engine";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
    volkLoadInstanceOnly(instance);

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debugCallback;
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance, &mci, nullptr, &messenger_));
        logInfo("vulkan validation on");
    }
    if (opt.window) VK_CHECK(glfwCreateWindowSurface(instance, opt.window, nullptr, &surface));

    // pick a GPU: requested index, else the first discrete one, else the first
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (!count) fatal("no Vulkan GPU found");
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    gpu = gpus[0];
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpus[i], &p);
        logInfo("gpu {}: {} (Vulkan {}.{})", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
        if (opt.gpuIndex == (int)i) { gpu = gpus[i]; break; }
        if (opt.gpuIndex < 0 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu = gpus[i]; break; }
    }
    vkGetPhysicalDeviceProperties(gpu, &props);
    gpuName = props.deviceName;
    timestampPeriodNs = props.limits.timestampPeriod;
    msaaSupport = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    if (props.apiVersion < VK_API_VERSION_1_3) fatal(std::format("{} does not support Vulkan 1.3; update the driver", gpuName));

    // one queue family for graphics + compute (+ present)
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qf.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i) {
        VkBool32 present = VK_TRUE;
        if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            queueFamily = i;
            timestamps = qf[i].timestampValidBits > 0;
            found = true;
            break;
        }
    }
    if (!found) fatal("no graphics+compute queue");

    // features (all core in 1.2 / 1.3)
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    auto need = [&](VkBool32 v, const char* what) { if (!v) fatal(std::format("{} lacks required feature {}", gpuName, what)); };
    need(f13.dynamicRendering, "dynamicRendering");
    need(f13.synchronization2, "synchronization2");
    need(f12.descriptorIndexing, "descriptorIndexing");
    need(f12.runtimeDescriptorArray, "runtimeDescriptorArray");
    need(f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    need(f12.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount");
    need(f12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing");
    need(f12.drawIndirectCount, "drawIndirectCount");
    need(f11.shaderDrawParameters, "shaderDrawParameters");
    need(f2.features.multiDrawIndirect, "multiDrawIndirect");
    need(f2.features.drawIndirectFirstInstance, "drawIndirectFirstInstance");

    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    e13.maintenance4 = f13.maintenance4;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
    e12.descriptorIndexing = VK_TRUE;
    e12.runtimeDescriptorArray = VK_TRUE;
    e12.descriptorBindingPartiallyBound = VK_TRUE;
    e12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    e12.drawIndirectCount = VK_TRUE;
    e12.scalarBlockLayout = f12.scalarBlockLayout;
    e12.hostQueryReset = f12.hostQueryReset;
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &e12};
    e11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e11};
    e2.features.multiDrawIndirect = VK_TRUE;
    e2.features.drawIndirectFirstInstance = VK_TRUE;
    e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    e2.features.fillModeNonSolid = f2.features.fillModeNonSolid;
    e2.features.depthClamp = f2.features.depthClamp;
    e2.features.shaderInt16 = f2.features.shaderInt16;
    e2.features.textureCompressionBC = f2.features.textureCompressionBC;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<const char*> devExts;
    if (surface) devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.physicalDevice = gpu;
    aci.device = device;
    aci.instance = instance;
    aci.pVulkanFunctions = &vf;
    VK_CHECK(vmaCreateAllocator(&aci, &allocator));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &immediatePool_));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &immediateFence_));
    logInfo("device: {}", gpuName);
}

void Device::immediate(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = immediatePool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    record(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, immediateFence_));
    VK_CHECK(vkWaitForFences(device, 1, &immediateFence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(device, 1, &immediateFence_));
    vkFreeCommandBuffers(device, immediatePool_, 1, &cmd);
}

void Device::shutdown() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    vkDestroyFence(device, immediateFence_, nullptr);
    vkDestroyCommandPool(device, immediatePool_, nullptr);
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance, messenger_, nullptr);
    vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
}
}  // namespace df
