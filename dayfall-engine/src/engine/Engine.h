#pragma once
#include "gfx/Swapchain.h"
#include "render/HudRenderer.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include <array>
#include <filesystem>
#include <functional>
#include <string>

struct GLFWwindow;

namespace df {
struct EngineOptions {
    bool headless = false;
    bool validation = false;
    bool vsync = true;
    int gpu = -1;
    uint32_t width = 1600, height = 900;
    RenderSettings render;
    std::filesystem::path shaderDir;     // default: <exe dir>/shaders
    std::string title = "DAYFALL";
};

struct CaptureResult {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// Owns the GPU device, the renderer, the window (unless headless) and the
// frame loop. Game and editor logic sit on top (see app/).
class Engine {
public:
    void init(const EngineOptions& opt);
    void shutdown();

    // Uploads a scene (call again after structural changes).
    void setScene(const Scene& scene) { renderer.setScene(scene); }
    // Renders one frame to the window. Returns false if the window was closed.
    bool renderFrame(const Camera& cam, float time);
    // Renders one frame offscreen at the given size and reads it back (RGBA8, sRGB).
    CaptureResult capture(const Camera& cam, float time, uint32_t width, uint32_t height);
    bool windowOpen() const;
    double lastGpuMs() const { return stats_.totalMs; }
    const FrameStats& stats() const { return stats_; }
    double hudGpuMs() const { return hudMs_; }

    // HUD over the final image of renderFrame and capture: hudBuild fills the canvas for the output size
    // (nothing drawn: no HUD pass). It may call capture() itself (the minimap), which then draws no HUD.
    std::function<void(HudCanvas&)> hudBuild;
    HudRenderer hud;

    Device device;
    Renderer renderer;
    Swapchain swapchain;
    GLFWwindow* window = nullptr;
    EngineOptions options;

private:
    bool buildHud(uint32_t w, uint32_t h);
    void recordFrame(VkCommandBuffer cmd, uint32_t frame, const Camera& cam, float time, OutputTarget out, bool hud);
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        bool submitted = false;
    };
    std::array<Frame, Renderer::kFrames> frames_{};
    uint32_t frameIndex_ = 0;
    FrameStats stats_;
    HudCanvas hudCanvas_;
    bool hudBusy_ = false;
    double hudMs_ = 0;
};
}  // namespace df
