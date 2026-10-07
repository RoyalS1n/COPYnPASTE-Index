#include "gfx/Swapchain.h"
#include <GLFW/glfw3.h>
#include <algorithm>

namespace df {
void Swapchain::init(Device& d, GLFWwindow* window, bool vsync) {
    d_ = &d;
    window_ = window;
    vsync_ = vsync;
    create();
}

void Swapchain::create() {
    Device& d = *d_;
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.gpu, d.surface, &caps));
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, d.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, d.surface, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    format = chosen.format;
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.gpu, d.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(d.gpu, d.surface, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
        for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    }
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        int w, h;
        glfwGetFramebufferSize(window_, &w, &h);
        extent = {std::clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width),
                  std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    uint32_t count = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = d.surface;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR sc;
    VK_CHECK(vkCreateSwapchainKHR(d.device, &ci, nullptr, &sc));
    if (swapchain) vkDestroySwapchainKHR(d.device, swapchain, nullptr);
    swapchain = sc;
    vkGetSwapchainImagesKHR(d.device, swapchain, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(d.device, swapchain, &n, images.data());
    views.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(d.device, &vci, nullptr, &views[i]));
    }
    renderDone.resize(n);
    for (auto& s : renderDone) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(d.device, &si, nullptr, &s));
    }
}

void Swapchain::destroyViews() {
    for (auto v : views) vkDestroyImageView(d_->device, v, nullptr);
    for (auto s : renderDone) vkDestroySemaphore(d_->device, s, nullptr);
    views.clear();
    renderDone.clear();
}

void Swapchain::recreate() {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0) {   // minimised
        glfwWaitEvents();
        glfwGetFramebufferSize(window_, &w, &h);
    }
    d_->waitIdle();
    destroyViews();
    create();
}

bool Swapchain::acquire(VkSemaphore signal, uint32_t& index) {
    VkResult r = vkAcquireNextImageKHR(d_->device, swapchain, UINT64_MAX, signal, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return false;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_CHECK(r);
    return true;
}

bool Swapchain::present(uint32_t index) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(d_->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return false;
    VK_CHECK(r);
    return true;
}

void Swapchain::shutdown() {
    if (!d_) return;
    destroyViews();
    if (swapchain) vkDestroySwapchainKHR(d_->device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
    d_ = nullptr;
}
}  // namespace df
