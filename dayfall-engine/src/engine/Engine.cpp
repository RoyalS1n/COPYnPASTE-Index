#include "engine/Engine.h"
#include "core/FileSystem.h"
#include <GLFW/glfw3.h>

namespace df {
void Engine::init(const EngineOptions& opt) {
    options = opt;
    if (options.shaderDir.empty()) options.shaderDir = executableDir() / "shaders";
    DeviceOptions dopt;
    dopt.validation = opt.validation;
    dopt.gpuIndex = opt.gpu;
    RenderSettings rs = opt.render;
    if (!opt.headless) {
        if (!glfwInit()) fatal("glfwInit failed");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow((int)opt.width, (int)opt.height, opt.title.c_str(), nullptr, nullptr);
        if (!window) fatal("cannot create a window (use --headless on machines without a display)");
        dopt.window = window;
    }
    device.init(dopt);
    if (window) {
        swapchain.init(device, window, opt.vsync);
        rs.width = swapchain.extent.width;
        rs.height = swapchain.extent.height;
    } else {
        rs.width = opt.width;
        rs.height = opt.height;
    }
    renderer.init(device, rs, options.shaderDir.string());
    hud.init(device, options.shaderDir.string());
    static_assert(HudRenderer::kFrames == Renderer::kFrames);
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = device.queueFamily;
        VK_CHECK(vkCreateCommandPool(device.device, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device.device, &ai, &f.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device.device, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device.device, &sci, nullptr, &f.acquired));
    }
}

bool Engine::windowOpen() const { return window && !glfwWindowShouldClose(window); }

bool Engine::renderFrame(const Camera& cam, float time) {
    if (!window) return false;
    if (glfwWindowShouldClose(window)) return false;
    Frame& f = frames_[frameIndex_];
    VK_CHECK(vkWaitForFences(device.device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    if (f.submitted) renderer.readStats(frameIndex_, stats_);
    if (f.submitted) hud.readStats(frameIndex_, hudMs_);
    uint32_t image = 0;
    if (!swapchain.acquire(f.acquired, image)) {
        swapchain.recreate();
        return true;
    }
    if (swapchain.extent.width != renderer.settings().width || swapchain.extent.height != renderer.settings().height)
        renderer.resize(swapchain.extent.width, swapchain.extent.height);
    bool drawHud = buildHud(swapchain.extent.width, swapchain.extent.height);
    VK_CHECK(vkResetFences(device.device, 1, &f.fence));
    VK_CHECK(vkResetCommandBuffer(f.cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    OutputTarget out{swapchain.images[image], swapchain.views[image], swapchain.format, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    recordFrame(f.cmd, frameIndex_, cam, time, out, drawHud);
    VK_CHECK(vkEndCommandBuffer(f.cmd));
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = swapchain.renderDone[image];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(device.queue, 1, &si, f.fence));
    f.submitted = true;
    if (!swapchain.present(image)) swapchain.recreate();
    frameIndex_ = (frameIndex_ + 1) % Renderer::kFrames;
    return true;
}

CaptureResult Engine::capture(const Camera& cam, float time, uint32_t width, uint32_t height) {
    width = std::clamp(width, 16u, 7680u);
    height = std::clamp(height, 16u, 4320u);
    bool drawHud = buildHud(width, height);   // first: it may render the minimap with a capture of its own
    device.waitIdle();
    uint32_t oldW = renderer.settings().width, oldH = renderer.settings().height;
    renderer.resize(width, height);
    ImageDesc id{VK_FORMAT_R8G8B8A8_SRGB, width, height};
    id.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Image img = createImage(device, id);
    Buffer rb = createBuffer(device, (VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemUsage::Readback);
    device.immediate([&](VkCommandBuffer cmd) {
        OutputTarget out{img.image, img.view, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        recordFrame(cmd, 0, cam, time, out, drawHud);
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &bc);
    });
    renderer.readStats(0, stats_);
    if (drawHud) hud.readStats(0, hudMs_);
    vmaInvalidateAllocation(device.allocator, rb.alloc, 0, VK_WHOLE_SIZE);
    CaptureResult r;
    r.width = width;
    r.height = height;
    r.rgba.assign((uint8_t*)rb.mapped, (uint8_t*)rb.mapped + (size_t)width * height * 4);
    destroyBuffer(device, rb);
    destroyImage(device, img);
    if (window) renderer.resize(oldW, oldH);
    return r;
}

bool Engine::buildHud(uint32_t w, uint32_t h) {
    if (hudBusy_) return false;   // a capture made while building the HUD (the minimap) draws none
    hudCanvas_.reset(w, h);
    if (!hudBuild) return false;
    hudBusy_ = true;
    try {
        hudBuild(hudCanvas_);
    } catch (...) {
        hudBusy_ = false;
        throw;
    }
    hudBusy_ = false;
    return !hudCanvas_.empty();
}

void Engine::recordFrame(VkCommandBuffer cmd, uint32_t frame, const Camera& cam, float time, OutputTarget out, bool drawHud) {
    if (!drawHud) return renderer.record(cmd, frame, cam, time, out);
    VkImageLayout finalLayout = out.finalLayout;
    out.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // the renderer leaves the image to the HUD pass
    renderer.record(cmd, frame, cam, time, out);
    hud.record(cmd, frame, out.view, out.format, hudCanvas_);
    imageBarrier(cmd, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, finalLayout,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT);
}

void Engine::shutdown() {
    if (!device.device) return;
    device.waitIdle();
    hudBuild = nullptr;
    hud.shutdown();
    for (Frame& f : frames_) {
        vkDestroyFence(device.device, f.fence, nullptr);
        vkDestroySemaphore(device.device, f.acquired, nullptr);
        vkDestroyCommandPool(device.device, f.pool, nullptr);
    }
    renderer.shutdown();
    swapchain.shutdown();
    device.shutdown();
    if (window) {
        glfwDestroyWindow(window);
        glfwTerminate();
        window = nullptr;
    }
}
}  // namespace df
