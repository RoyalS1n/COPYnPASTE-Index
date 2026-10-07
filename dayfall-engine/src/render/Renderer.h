#pragma once
#include "gfx/Resources.h"
#include "render/Camera.h"
#include "scene/Scene.h"

namespace df { class Terrain; }
#include <array>
#include <cstring>
#include <string>

namespace df {
struct RenderSettings {
    uint32_t width = 1280, height = 720;
    uint32_t msaa = 4;             // 1, 2, 4 or 8
    uint32_t shadowSize = 2048;    // per cascade
    uint32_t cascades = 4;         // 0 to 4
    float lodScale = 1.0f;         // multiplies LOD switch distances
    float cullScale = 1.0f;        // multiplies per-instance cull distances
};

struct OutputTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
};

struct FrameStats {
    static constexpr int kPasses = 5;
    const char* names[kPasses] = {"cull", "shadows", "main", "water", "post"};
    double ms[kPasses]{};
    double totalMs = 0.0;
};

// GPU-driven forward renderer. All instances are culled on the GPU each frame
// (camera + shadow cascades, distance cull, LOD pick) into indirect draw
// commands, one multi-draw per pipeline group.
class Renderer {
public:
    static constexpr uint32_t kFrames = 2;
    void init(Device& dev, const RenderSettings& s, const std::string& shaderDir);
    void shutdown();
    void setScene(const Scene& scene);
    void resize(uint32_t w, uint32_t h);
    // Records a whole frame into `out` (same size as the render settings).
    void record(VkCommandBuffer cmd, uint32_t frame, const Camera& cam, float time, const OutputTarget& out);
    // Instances in [scene.dynamicFirst, end) are re-uploaded by the next record() (player, pickups),
    // and the vertices in scene.dynamicVertexFirst/Count (skinned characters) through a staging copy.
    void updateDynamicInstances(const Scene& scene);
    bool readStats(uint32_t frame, FrameStats& st);
    // re-uploads point-light colours / positions (same light count as setScene)
    void updateLights(const Scene& scene);
    float shadowDistanceOverride = 0.0f;   // > 0: overrides the environment's shadow distance (top-down captures)
    float cullScaleOverride = 0.0f;        // > 0: multiplies cull distances instead of RenderSettings::cullScale (minimap)
    // GPU heightmap terrain (final heights, painted layers, paths); nullptr removes it
    void setTerrain(const Terrain* terrain, uint32_t material);
    uint32_t terrainPatches() const { return patchCount_[0]; }
    void setTerrainMaterial(uint32_t material) { float b; std::memcpy(&b, &material, 4); frameData_.terrainC.z = b; terrain_.material = material; }
    const RenderSettings& settings() const { return s_; }

private:
    void createTargets();
    void destroyTargets();
    void createPipelines();
    VkPipeline postPipeline(VkFormat format);
    void writeDescriptors();
    void updateFrame(uint32_t frame, const Camera& cam, float time);
    void drawGroup(VkCommandBuffer cmd, uint32_t view, uint32_t group, VkPipeline pipe);
    void selectTerrain(uint32_t frame, const Camera& cam);
    void drawTerrain(VkCommandBuffer cmd, uint32_t view, VkPipeline pipe);
    void createTerrainDummies();

    Device* d_ = nullptr;
    RenderSettings s_;
    std::string shaderDir_;
    const Scene* scene_ = nullptr;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;

    // scene buffers
    Buffer vertices_, indices_, instances_, instanceScales_, meshInfos_, lods_, batchRefs_, batches_, materials_, lights_, lightGrid_;
    std::array<Buffer, kFrames> ubo_, visible_, cmds_;
    std::vector<Image> textures_;
    Image skyLut_;
    GpuFrame frameData_{};
    uint32_t numBatches_ = 0, numInstances_ = 0, mainTotal_ = 0, shadowTotal_ = 0, views_ = 1;
    std::array<uint32_t, GroupCount> groupStart_{}, groupCount_{};
    vec3 sceneMin_{0}, sceneMax_{0};

    // render targets
    Image hdrMsaa_, depthMsaa_, hdr_, depth_, sceneCopy_, shadowMap_;

    VkSampler linearRepeat_ = VK_NULL_HANDLE, linearClamp_ = VK_NULL_HANDLE, nearestClamp_ = VK_NULL_HANDLE,
              shadowSampler_ = VK_NULL_HANDLE, skySampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFrames> sets_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline cullReset_ = VK_NULL_HANDLE, cull_ = VK_NULL_HANDLE;
    VkPipeline opaque_ = VK_NULL_HANDLE, twoSided_ = VK_NULL_HANDLE, masked_ = VK_NULL_HANDLE, sky_ = VK_NULL_HANDLE;
    VkPipeline shadowOpaque_ = VK_NULL_HANDLE, shadowMasked_ = VK_NULL_HANDLE;
    VkPipeline water_ = VK_NULL_HANDLE, blend_ = VK_NULL_HANDLE;
    std::vector<std::pair<VkFormat, VkPipeline>> post_;
    std::vector<GpuInstance> dynamic_;
    uint32_t dynamicFirst_ = 0;
    std::vector<GpuVertex> dynamicVerts_;
    uint32_t dynamicVertFirst_ = 0, numVertices_ = 0;
    std::array<Buffer, kFrames> vertexStaging_;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    // terrain
    static constexpr uint32_t kMaxPatches = 6144;
    struct TerrainGpu {
        Image height, paint0, paint1, paths;
        uint32_t n = 0;
        float spacing = 1.0f, hMin = 0.0f, hRange = 1.0f;
        vec2 origin{0};
        bool enabled = false;
        uint32_t material = 0;
        std::vector<std::vector<vec2>> minMax;   // per level: (min, max) per node
        std::vector<uint32_t> nodesPerSide;
    } terrain_;
    Buffer terrainIndices_;
    std::array<Buffer, kFrames> patches_;
    std::array<uint32_t, 5> patchFirst_{}, patchCount_{};
    VkPipeline terrainMain_ = VK_NULL_HANDLE, terrainShadow_ = VK_NULL_HANDLE;
    uint32_t maxTextures_ = 4096;
    VkBuffer curCmds_ = VK_NULL_HANDLE;
    bool sceneReady_ = false;
};
}  // namespace df
