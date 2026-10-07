#pragma once
// HUD overlay: screen-space triangles (shapes, text from a built-in bitmap
// font, the minimap texture) alpha-blended over the final image in a pass of
// its own, with its own pipeline, descriptor set layout and push constants.
#include "core/Math.h"
#include "gfx/Resources.h"
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace df {
enum HudMode : uint32_t { HudSolid = 0, HudText = 1, HudOutline = 2, HudMap = 3, HudMapRound = 4, HudDisc = 5, HudRing = 6 };
struct HudVertex {
    vec2 pos;           // output pixels, y down
    vec2 uv;            // atlas / minimap uv; HudRing: uv.x = inner radius (fraction)
    vec2 local;         // -1..1 across discs, rings and the round minimap
    uint32_t color;     // RGBA8, sRGB
    uint32_t mode;      // HudMode
};
static_assert(sizeof(HudVertex) == 32);

// RGBA8 colour from sRGB components 0..1
uint32_t hudColor(float r, float g, float b, float a = 1.0f);
inline uint32_t hudColor(vec3 c, float a = 1.0f) { return hudColor(c.r, c.g, c.b, a); }

// One frame of HUD geometry for an output size. Sizes given for 1080p are
// multiplied by `ui`, which grows a little faster than the height on small
// outputs so 960 x 540 captures stay legible.
struct HudCanvas {
    uint32_t width = 0, height = 0;
    float ui = 1.0f;
    std::vector<HudVertex> verts;

    void reset(uint32_t w, uint32_t h);
    bool empty() const { return verts.empty(); }
    void quad(const HudVertex (&v)[4]);                       // v0 v1 v2 v3 around the quad
    void rect(vec2 a, vec2 b, uint32_t color);                // a = top-left, b = bottom-right
    void rectH(vec2 a, vec2 b, uint32_t left, uint32_t right); // horizontal gradient
    void tri(vec2 a, vec2 b, vec2 c, uint32_t color);
    void disc(vec2 c, float r, uint32_t color);
    void ring(vec2 c, float r, float width, uint32_t color);
    // text with its top-left at `pos`; scale = pixels per font unit (capitals are 7 units tall); returns the width
    float text(vec2 pos, std::string_view s, float scale, uint32_t color, uint32_t outline);
    static float textWidth(std::string_view s, float scale);
    static constexpr float kCapHeight = 7.0f;                 // font units
};

class HudRenderer {
public:
    static constexpr uint32_t kFrames = 2;                    // matches Renderer::kFrames
    void init(Device& d, const std::string& shaderDir);
    void shutdown();
    // the minimap image (RGBA8 sRGB, north at the top); waits for the GPU
    void setMinimap(const uint8_t* rgba, uint32_t w, uint32_t h);
    // Draws `c` into `view`, which is in COLOR_ATTACHMENT_OPTIMAL and was just written by the post pass.
    void record(VkCommandBuffer cmd, uint32_t frame, VkImageView view, VkFormat format, const HudCanvas& c);
    bool readStats(uint32_t frame, double& ms);               // GPU time of the last HUD pass in this frame slot

private:
    VkPipeline pipeline(VkFormat format);
    void writeDescriptors();
    static constexpr uint32_t kMaxVerts = 1u << 15;
    Device* d_ = nullptr;
    std::string shaderDir_;
    std::array<Buffer, kFrames> verts_;
    Image font_, minimap_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFrames> sets_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::vector<std::pair<VkFormat, VkPipeline>> pipes_;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    std::array<bool, kFrames> stamped_{};
};
}  // namespace df
