#pragma once
#include "gfx/Device.h"
#include <vector>

struct GLFWwindow;

namespace df {
class Swapchain {
public:
    void init(Device& d, GLFWwindow* window, bool vsync);
    void shutdown();
    // false: out of date (resized / minimised); call recreate() and try again
    bool acquire(VkSemaphore signal, uint32_t& index);
    bool present(uint32_t index);
    void recreate();

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> renderDone;   // one per image: signalled by the frame's submit, waited by present
private:
    void create();
    void destroyViews();
    Device* d_ = nullptr;
    GLFWwindow* window_ = nullptr;
    bool vsync_ = true;
};
}  // namespace df
