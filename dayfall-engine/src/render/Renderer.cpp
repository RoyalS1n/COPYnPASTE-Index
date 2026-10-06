#include "render/Renderer.h"
#include "gfx/Pipeline.h"
#include "render/LightGrid.h"
#include "render/SkyModel.h"
#include <algorithm>
#include <cstring>

namespace df {
namespace {
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkBufferUsageFlags kSsbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr uint32_t kBindingCount = 18;

struct Push { uint32_t batchBase, view, flags, pad; };

// a buffer of at least 16 bytes (empty storage buffers cannot be bound)
template <typename T> Buffer upload(Device& d, const std::vector<T>& v, VkBufferUsageFlags usage) {
    static const uint8_t zero[16]{};
    if (v.empty()) return createBufferWithData(d, zero, sizeof(zero), usage);
    return createBufferWithData(d, v.data(), v.size() * sizeof(T), usage);
}

void uploadImage(Device& d, Image& img, const void* data, VkDeviceSize size) {
    Buffer staging = createBuffer(d, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemUsage::Upload);
    std::memcpy(staging.mapped, data, size);
    vmaFlushAllocation(d.allocator, staging.alloc, 0, size);
    d.immediate([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, img.image, aspectOf(img.format), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {aspectOf(img.format), 0, 0, 1};
        c.imageExtent = {img.extent.width, img.extent.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        imageBarrier(cmd, img.image, aspectOf(img.format), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    destroyBuffer(d, staging);
}

vec4 normalizePlane(vec4 p) {
    float l = glm::length(vec3(p));
    return l < 1e-6f ? vec4(0, 0, 0, 1) : p / l;   // degenerate (infinite far plane): always passes
}
// the six clip planes of a Vulkan (depth 0..1) view-projection, pointing inwards
void extractPlanes(const mat4& m, vec4* out) {
    vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
    vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
    vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
    vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
    out[0] = normalizePlane(r3 + r0);
    out[1] = normalizePlane(r3 - r0);
    out[2] = normalizePlane(r3 + r1);
    out[3] = normalizePlane(r3 - r1);
    out[4] = normalizePlane(r2);
    out[5] = normalizePlane(r3 - r2);
}

VkRenderingAttachmentInfo colorAttachment(VkImageView view, VkAttachmentLoadOp load, VkImageView resolve = VK_NULL_HANDLE) {
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = view;
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = load;
    a.storeOp = resolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    a.clearValue.color = {{0, 0, 0, 1}};
    if (resolve) {
        a.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        a.resolveImageView = resolve;
        a.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    return a;
}

void setViewport(VkCommandBuffer cmd, uint32_t w, uint32_t h, bool flipY) {
    VkViewport vp{0, flipY ? (float)h : 0.0f, (float)w, flipY ? -(float)h : (float)h, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

VkSampler makeSampler(Device& d, VkFilter filter, VkSamplerAddressMode mode, bool mips, bool compare = false) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ci.minFilter = filter;
    ci.mipmapMode = filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = mode;
    ci.maxLod = mips ? VK_LOD_CLAMP_NONE : 0.0f;
    if (mips && d.props.limits.maxSamplerAnisotropy > 1.0f) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(8.0f, d.props.limits.maxSamplerAnisotropy);
    }
    if (compare) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(d.device, &ci, nullptr, &s));
    return s;
}
}  // namespace

void Renderer::init(Device& dev, const RenderSettings& s, const std::string& shaderDir) {
    d_ = &dev;
    s_ = s;
    shaderDir_ = shaderDir;
    uint32_t want = std::max(1u, s.msaa);
    samples_ = VK_SAMPLE_COUNT_1_BIT;
    for (uint32_t c = want; c > 1; c >>= 1)
        if (dev.msaaSupport & c) { samples_ = (VkSampleCountFlagBits)c; break; }
    s_.msaa = (uint32_t)samples_;
    s_.cascades = std::min(s_.cascades, 4u);

    linearRepeat_ = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, true);
    linearClamp_ = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);
    nearestClamp_ = makeSampler(dev, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);
    shadowSampler_ = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, true);
    skySampler_ = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, false);

    // descriptor set layout: everything in set 0, textures as a variable-size array
    maxTextures_ = std::min<uint32_t>(4096, dev.props.limits.maxPerStageDescriptorSamplers > 64
                                                ? dev.props.limits.maxPerStageDescriptorSamplers - 32 : 32);
    VkDescriptorSetLayoutBinding b[kBindingCount]{};
    VkDescriptorBindingFlags flags[kBindingCount]{};
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_ALL;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i <= 11 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    b[17].descriptorCount = maxTextures_;
    flags[17] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = kBindingCount;
    bf.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf};
    lci.bindingCount = kBindingCount;
    lci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev.device, &lci, nullptr, &setLayout_));

    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFrames},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11 * kFrames},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (5 + maxTextures_) * kFrames}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kFrames;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev.device, &pci, nullptr, &pool_));

    VkPushConstantRange pr{VK_SHADER_STAGE_ALL, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(dev.device, &plci, nullptr, &layout_));

    for (uint32_t f = 0; f < kFrames; ++f)
        ubo_[f] = createBuffer(dev, sizeof(GpuFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemUsage::Upload);

    if (dev.timestamps) {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kFrames * (FrameStats::kPasses + 1);
        VK_CHECK(vkCreateQueryPool(dev.device, &qci, nullptr, &queries_));
    }
    createPipelines();
    createTargets();
}

void Renderer::createPipelines() {
    Device& d = *d_;
    auto sh = [&](const char* n) { return loadShader(d, std::string(shaderDir_) + "/" + n + ".spv"); };
    VkShaderModule cullResetCs = sh("cull_reset.comp"), cullCs = sh("cull.comp");
    VkShaderModule meshVs = sh("mesh.vert"), meshFs = sh("mesh.frag"), shadowVs = sh("shadow.vert"), shadowFs = sh("shadow.frag");
    VkShaderModule fullVs = sh("fullscreen.vert"), skyFs = sh("sky.frag"), waterFs = sh("water.frag");

    cullReset_ = createComputePipeline(d, cullResetCs, layout_);
    cull_ = createComputePipeline(d, cullCs, layout_);

    GraphicsPipelineDesc g;
    g.layout = layout_;
    g.vert = meshVs;
    g.frag = meshFs;
    g.colorFormats = {kHdrFormat};
    g.depthFormat = kDepthFormat;
    g.samples = samples_;
    uint32_t masked = 0;
    g.specEntries = {{0, 0, 4}};
    g.specData.resize(4);
    std::memcpy(g.specData.data(), &masked, 4);
    opaque_ = createGraphicsPipeline(d, g);
    g.cull = VK_CULL_MODE_NONE;
    twoSided_ = createGraphicsPipeline(d, g);
    masked = 1;
    std::memcpy(g.specData.data(), &masked, 4);
    g.alphaToCoverage = samples_ != VK_SAMPLE_COUNT_1_BIT;
    masked_ = createGraphicsPipeline(d, g);

    // transparent passes run single-sampled on the resolved targets
    GraphicsPipelineDesc t = g;
    t.alphaToCoverage = false;
    t.samples = VK_SAMPLE_COUNT_1_BIT;
    t.depthWrite = false;
    masked = 0;
    std::memcpy(t.specData.data(), &masked, 4);
    t.blend = true;
    blend_ = createGraphicsPipeline(d, t);
    t.blend = false;
    t.frag = waterFs;
    t.specEntries.clear();
    t.specData.clear();
    water_ = createGraphicsPipeline(d, t);

    GraphicsPipelineDesc sky;
    sky.layout = layout_;
    sky.vert = fullVs;
    sky.frag = skyFs;
    sky.colorFormats = {kHdrFormat};
    sky.depthFormat = kDepthFormat;
    sky.samples = samples_;
    sky.cull = VK_CULL_MODE_NONE;
    sky.depthWrite = false;
    sky_ = createGraphicsPipeline(d, sky);

    GraphicsPipelineDesc s;
    s.layout = layout_;
    s.vert = shadowVs;
    s.depthFormat = kShadowFormat;
    s.cull = VK_CULL_MODE_NONE;
    s.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    s.depthBias = true;
    shadowOpaque_ = createGraphicsPipeline(d, s);
    s.frag = shadowFs;
    shadowMasked_ = createGraphicsPipeline(d, s);

    for (VkShaderModule m : {cullResetCs, cullCs, meshVs, meshFs, shadowVs, shadowFs, fullVs, skyFs, waterFs})
        vkDestroyShaderModule(d.device, m, nullptr);
}

VkPipeline Renderer::postPipeline(VkFormat format) {
    for (auto& [f, p] : post_) if (f == format) return p;
    Device& d = *d_;
    VkShaderModule vs = loadShader(d, shaderDir_ + "/fullscreen.vert.spv"), fs = loadShader(d, shaderDir_ + "/post.frag.spv");
    GraphicsPipelineDesc p;
    p.layout = layout_;
    p.vert = vs;
    p.frag = fs;
    p.colorFormats = {format};
    p.cull = VK_CULL_MODE_NONE;
    p.depthTest = p.depthWrite = false;
    VkPipeline pipe = createGraphicsPipeline(d, p);
    vkDestroyShaderModule(d.device, vs, nullptr);
    vkDestroyShaderModule(d.device, fs, nullptr);
    post_.push_back({format, pipe});
    return pipe;
}

void Renderer::updateDynamicInstances(const Scene& scene) {
    dynamicFirst_ = scene.dynamicFirst;
    dynamic_.assign(scene.instances.begin() + std::min<size_t>(scene.dynamicFirst, scene.instances.size()), scene.instances.end());
}

void Renderer::createTargets() {
    Device& d = *d_;
    uint32_t w = s_.width, h = s_.height;
    ImageDesc c{kHdrFormat, w, h};
    c.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    hdr_ = createImage(d, c);
    c.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sceneCopy_ = createImage(d, c);
    ImageDesc z{kDepthFormat, w, h};
    z.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depth_ = createImage(d, z);
    if (samples_ != VK_SAMPLE_COUNT_1_BIT) {
        ImageDesc mc{kHdrFormat, w, h};
        mc.samples = samples_;
        mc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        hdrMsaa_ = createImage(d, mc);
        ImageDesc mz{kDepthFormat, w, h};
        mz.samples = samples_;
        mz.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depthMsaa_ = createImage(d, mz);
    }
    ImageDesc sd{kShadowFormat, s_.shadowSize, s_.shadowSize};
    sd.layers = std::max(1u, s_.cascades);
    sd.arrayView = true;
    sd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    shadowMap_ = createImage(d, sd);
    // the shadow map must be readable even when no cascade renders into it
    d.immediate([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearDepthStencilValue one{1.0f, 0};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cmd, shadowMap_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &r);
        imageBarrier(cmd, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        for (Image* img : {&hdr_, &sceneCopy_})
            imageBarrier(cmd, img->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imageBarrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
}

void Renderer::destroyTargets() {
    for (Image* i : {&hdrMsaa_, &depthMsaa_, &hdr_, &depth_, &sceneCopy_, &shadowMap_})
        if (i->image) destroyImage(*d_, *i);
}

void Renderer::resize(uint32_t w, uint32_t h) {
    if (w == s_.width && h == s_.height) return;
    d_->waitIdle();
    destroyTargets();
    s_.width = std::max(1u, w);
    s_.height = std::max(1u, h);
    createTargets();
    if (sceneReady_) writeDescriptors();
}

void Renderer::setScene(const Scene& scene) {
    Device& d = *d_;
    d.waitIdle();
    // release the previous scene
    for (Buffer* b : {&vertices_, &indices_, &instances_, &meshInfos_, &lods_, &batchRefs_, &batches_, &materials_, &lights_, &lightGrid_})
        if (b->buffer) destroyBuffer(d, *b);
    for (uint32_t f = 0; f < kFrames; ++f) {
        if (visible_[f].buffer) destroyBuffer(d, visible_[f]);
        if (cmds_[f].buffer) destroyBuffer(d, cmds_[f]);
    }
    for (auto& t : textures_) destroyImage(d, t);
    textures_.clear();
    if (skyLut_.image) destroyImage(d, skyLut_);
    scene_ = &scene;

    // batches: one per (mesh, lod, submesh), sorted by pipeline group
    struct Pending { uint32_t mesh, lod, submesh, group; };
    std::vector<Pending> pending;
    for (uint32_t m = 0; m < scene.meshes.size(); ++m)
        for (uint32_t l = 0; l < scene.meshes[m].lods.size(); ++l)
            for (uint32_t sm : scene.meshes[m].lods[l].submeshes)
                pending.push_back({m, l, sm, scene.materials[scene.submeshes[sm].material].group});
    std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.group < b.group; });

    std::vector<uint32_t> perMesh(scene.meshes.size(), 0);
    for (const GpuInstance& i : scene.instances) perMesh[i.mesh]++;

    std::vector<GpuBatch> batches(pending.size());
    std::vector<std::vector<std::vector<uint32_t>>> refs(scene.meshes.size());
    for (uint32_t m = 0; m < scene.meshes.size(); ++m) refs[m].resize(scene.meshes[m].lods.size());
    groupStart_.fill(0);
    groupCount_.fill(0);
    mainTotal_ = shadowTotal_ = 0;
    for (uint32_t i = 0; i < pending.size(); ++i) {
        const Pending& p = pending[i];
        const Submesh& sm = scene.submeshes[p.submesh];
        GpuBatch& b = batches[i];
        b.indexCount = sm.indexCount;
        b.firstIndex = sm.firstIndex;
        b.vertexOffset = sm.vertexOffset;
        b.material = sm.material;
        b.group = p.group;
        b.mainOffset = mainTotal_;
        mainTotal_ += perMesh[p.mesh];
        b.shadowOffset = shadowTotal_;
        if (p.group < GroupWater) shadowTotal_ += perMesh[p.mesh];
        if (groupCount_[p.group]++ == 0) groupStart_[p.group] = i;
        refs[p.mesh][p.lod].push_back(i);
    }
    std::vector<uint32_t> batchRefs;
    std::vector<GpuMeshLod> lods;
    std::vector<GpuMeshInfo> infos(scene.meshes.size());
    for (uint32_t m = 0; m < scene.meshes.size(); ++m) {
        infos[m].bounds = scene.meshes[m].bounds;
        infos[m].firstLod = (uint32_t)lods.size();
        infos[m].lodCount = (uint32_t)scene.meshes[m].lods.size();
        for (uint32_t l = 0; l < scene.meshes[m].lods.size(); ++l) {
            GpuMeshLod gl{};
            gl.firstRef = (uint32_t)batchRefs.size();
            gl.refCount = (uint32_t)refs[m][l].size();
            gl.maxDistance = scene.meshes[m].lods[l].maxDistance;
            batchRefs.insert(batchRefs.end(), refs[m][l].begin(), refs[m][l].end());
            lods.push_back(gl);
        }
    }
    std::vector<GpuMaterial> mats;
    for (const MaterialDef& m : scene.materials) mats.push_back(m.gpu);

    numBatches_ = (uint32_t)batches.size();
    numInstances_ = (uint32_t)scene.instances.size();
    views_ = 1 + s_.cascades;
    vertices_ = upload(d, scene.vertices, kSsbo);
    indices_ = upload(d, scene.indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    instances_ = upload(d, scene.instances, kSsbo);
    meshInfos_ = upload(d, infos, kSsbo);
    lods_ = upload(d, lods, kSsbo);
    batchRefs_ = upload(d, batchRefs, kSsbo);
    batches_ = upload(d, batches, kSsbo);
    materials_ = upload(d, mats, kSsbo);
    VkDeviceSize visibleBytes = std::max<VkDeviceSize>(16, ((VkDeviceSize)mainTotal_ + (VkDeviceSize)shadowTotal_ * s_.cascades) * 4);
    VkDeviceSize cmdBytes = std::max<VkDeviceSize>(32, (VkDeviceSize)numBatches_ * views_ * sizeof(VkDrawIndexedIndirectCommand));
    for (uint32_t f = 0; f < kFrames; ++f) {
        visible_[f] = createBuffer(d, visibleBytes, kSsbo, MemUsage::GpuOnly);
        cmds_[f] = createBuffer(d, cmdBytes, kSsbo | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, MemUsage::GpuOnly);
    }

    LightGrid grid = buildLightGrid(scene.lights);
    lights_ = upload(d, grid.lights, kSsbo);
    lightGrid_ = upload(d, grid.data, kSsbo);
    frameData_.lightGrid = vec4(grid.origin, grid.cell);
    frameData_.lightGridDims = grid.dims;

    SkyData sky = computeSky(scene.env);
    ImageDesc sd{VK_FORMAT_R16G16B16A16_SFLOAT, sky.width, sky.height};
    sd.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    skyLut_ = createImage(d, sd);
    uploadImage(d, skyLut_, sky.lut.data(), sky.lut.size() * sizeof(uint16_t));
    for (int i = 0; i < 9; ++i) frameData_.sh[i] = sky.sh[i];

    if (scene.textures.size() > maxTextures_)
        logWarn("{} textures, the device allows {}; the rest are not bound", scene.textures.size(), maxTextures_);
    for (const Texture& t : scene.textures) textures_.push_back(createTexture(d, t.rgba.data(), t.width, t.height, t.srgb));
    if (textures_.empty()) {
        const uint8_t white[4] = {255, 255, 255, 255};
        textures_.push_back(createTexture(d, white, 1, 1, false));
    }

    sceneMin_ = scene.boundsMin;
    sceneMax_ = scene.boundsMax;
    if (sceneMin_.x > sceneMax_.x) sceneMin_ = sceneMax_ = vec3(0);
    sceneReady_ = true;
    writeDescriptors();
    double mb = (double)(vertices_.size + indices_.size + instances_.size + visibleBytes * kFrames) / (1 << 20);
    logInfo("renderer: {} batches, {} instances, visible list {} + {} x {}, {:.0f} MB geometry and lists", numBatches_,
            numInstances_, mainTotal_, s_.cascades, shadowTotal_, mb);
}

void Renderer::updateLights(const Scene& scene) {
    Device& d = *d_;
    d.waitIdle();
    if (lights_.buffer) destroyBuffer(d, lights_);
    if (lightGrid_.buffer) destroyBuffer(d, lightGrid_);
    LightGrid grid = buildLightGrid(scene.lights);
    lights_ = upload(d, grid.lights, kSsbo);
    lightGrid_ = upload(d, grid.data, kSsbo);
    frameData_.lightGrid = vec4(grid.origin, grid.cell);
    frameData_.lightGridDims = grid.dims;
    writeDescriptors();
}

void Renderer::writeDescriptors() {
    Device& d = *d_;
    vkResetDescriptorPool(d.device, pool_, 0);
    uint32_t texCount = std::min<uint32_t>((uint32_t)textures_.size(), maxTextures_);
    for (uint32_t f = 0; f < kFrames; ++f) {
        VkDescriptorSetVariableDescriptorCountAllocateInfo vc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        vc.descriptorSetCount = 1;
        vc.pDescriptorCounts = &texCount;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vc};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        VK_CHECK(vkAllocateDescriptorSets(d.device, &ai, &sets_[f]));

        const Buffer* bufs[12] = {&ubo_[f], &vertices_, &instances_, &meshInfos_, &lods_, &batchRefs_, &batches_, &materials_,
                                  &visible_[f], &cmds_[f], &lights_, &lightGrid_};
        VkDescriptorBufferInfo bi[12];
        VkDescriptorImageInfo ii[5];
        std::vector<VkDescriptorImageInfo> ti(texCount);
        std::vector<VkWriteDescriptorSet> w;
        for (uint32_t i = 0; i < 12; ++i) {
            bi[i] = {bufs[i]->buffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstSet = sets_[f];
            x.dstBinding = i;
            x.descriptorCount = 1;
            x.descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            x.pBufferInfo = &bi[i];
            w.push_back(x);
        }
        ii[0] = {shadowSampler_, shadowMap_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[1] = {skySampler_, skyLut_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[2] = {linearClamp_, sceneCopy_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[3] = {nearestClamp_, depth_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        ii[4] = {linearClamp_, hdr_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (uint32_t i = 0; i < 5; ++i) {
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstSet = sets_[f];
            x.dstBinding = 12 + i;
            x.descriptorCount = 1;
            x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            x.pImageInfo = &ii[i];
            w.push_back(x);
        }
        for (uint32_t i = 0; i < texCount; ++i) ti[i] = {linearRepeat_, textures_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = sets_[f];
        x.dstBinding = 17;
        x.descriptorCount = texCount;
        x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        x.pImageInfo = ti.data();
        w.push_back(x);
        vkUpdateDescriptorSets(d.device, (uint32_t)w.size(), w.data(), 0, nullptr);
    }
}

void Renderer::updateFrame(uint32_t frame, const Camera& cam, float time) {
    const Environment& env = scene_->env;
    GpuFrame& f = frameData_;
    float aspect = (float)s_.width / (float)s_.height;
    f.view = cam.view();
    f.proj = cam.projection(aspect);
    f.viewProj = f.proj * f.view;
    f.invViewProj = glm::inverse(f.viewProj);
    f.cameraPos = vec4(cam.position, time);
    f.sunDir = vec4(glm::normalize(env.sunDir), env.sunStrength);
    f.sunColor = vec4(env.sunColor, glm::radians(env.sunAngleDeg) * 0.5f);
    f.fogNear = vec4(env.hazeNear, env.hazeAmount);
    f.fogFar = vec4(env.hazeFar, env.mistStart);
    f.fogParams = vec4(std::max(env.mistDepth, 1.0f), env.heightFogDensity, env.heightFogFalloff, env.heightFogBase);
    f.viewport = vec4((float)s_.width, (float)s_.height, 1.0f / s_.width, 1.0f / s_.height);
    f.tonemap = vec4(std::exp2(env.exposureEv), env.lookPower, env.lookSaturation, env.lookSlope);
    f.wind = vec4(env.windDir, env.windStrength, 0.3f);
    f.clouds = vec4(env.cloudCoverage, env.cloudHeight, env.cloudScale, env.clouds ? 1.0f : 0.0f);
    f.cloudColor = vec4(env.cloudColor, 1.0f);
    f.water = vec4(env.waterLevel, 0.0f, cam.nearPlane, 0.0f);
    f.lodBias = vec4(s_.lodScale, s_.cullScale, 0, 0);
    f.post = vec4(env.vignette, 1.0f, 0, 0);
    f.totals = uvec4(mainTotal_, shadowTotal_, 0, 0);
    f.counts = uvec4(numInstances_, numBatches_, views_, 0);
    extractPlanes(f.viewProj, &f.viewPlanes[0]);

    // cascaded shadow maps: practical split scheme, a bounding sphere per
    // slice (stable under rotation), snapped to shadow texels (stable under
    // translation); each light frustum reaches back to cover the whole scene
    uint32_t n = s_.cascades;
    f.shadowParams = vec4(1.0f / s_.shadowSize, 0.0f, 1.0f, (float)n);
    f.cascadeSplits = vec4(0.0f);
    if (n > 0) {
        float nearD = cam.nearPlane, farD = std::max(shadowDistanceOverride > 0 ? shadowDistanceOverride : env.shadowDistance, nearD * 2.0f);
        vec3 fwd = cam.forward();
        vec3 right = glm::normalize(glm::cross(fwd, vec3(0, 0, 1)));
        if (!std::isfinite(right.x) || glm::length(glm::cross(fwd, vec3(0, 0, 1))) < 1e-4f) right = vec3(1, 0, 0);
        vec3 up = glm::cross(right, fwd);
        float tv = std::tan(cam.vfov * 0.5f), th = tv * aspect;
        vec3 L = glm::normalize(env.sunDir);
        vec3 lup = std::abs(L.z) > 0.99f ? vec3(0, 1, 0) : vec3(0, 0, 1);
        float prev = nearD;
        for (uint32_t c = 0; c < n; ++c) {
            float t = (float)(c + 1) / n;
            float split = 0.8f * nearD * std::pow(farD / nearD, t) + 0.2f * (nearD + (farD - nearD) * t);
            vec3 corners[8];
            int k = 0;
            for (float dist : {prev, split})
                for (float sx : {-1.0f, 1.0f})
                    for (float sy : {-1.0f, 1.0f})
                        corners[k++] = cam.position + fwd * dist + right * (sx * th * dist) + up * (sy * tv * dist);
            vec3 center(0);
            for (auto& p : corners) center += p / 8.0f;
            float r = 0;
            for (auto& p : corners) r = std::max(r, glm::length(p - center));
            r = std::ceil(r * 16.0f) / 16.0f;
            float back = r;
            for (int i = 0; i < 8; ++i) {
                vec3 corner((i & 1) ? sceneMax_.x : sceneMin_.x, (i & 2) ? sceneMax_.y : sceneMin_.y, (i & 4) ? sceneMax_.z : sceneMin_.z);
                back = std::max(back, glm::dot(corner - center, L));
            }
            back = std::min(back, 4000.0f) + 5.0f;
            mat4 lv = glm::lookAt(center + L * back, center, lup);
            mat4 lp = glm::ortho(-r, r, -r, r, 0.0f, back + r + 5.0f);
            mat4 m = lp * lv;
            vec4 o = m * vec4(0, 0, 0, 1);
            float half = s_.shadowSize * 0.5f;
            vec2 oo = vec2(o) * half;
            vec2 off = (glm::round(oo) - oo) / half;
            lp[3][0] += off.x;
            lp[3][1] += off.y;
            m = lp * lv;
            f.cascadeViewProj[c] = m;
            f.cascadeSplits[c] = split;
            extractPlanes(m, &f.viewPlanes[(c + 1) * 6]);
            if (c == 0) f.shadowParams.y = 2.0f * r / s_.shadowSize * 1.5f;
            prev = split;
        }
    }
    std::memcpy(ubo_[frame].mapped, &f, sizeof(GpuFrame));
    vmaFlushAllocation(d_->allocator, ubo_[frame].alloc, 0, sizeof(GpuFrame));
}

void Renderer::drawGroup(VkCommandBuffer cmd, uint32_t view, uint32_t group, VkPipeline pipe) {
    if (groupCount_[group] == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    Push p{groupStart_[group], view, samples_ == VK_SAMPLE_COUNT_1_BIT ? 1u : 0u, 0};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
    VkDeviceSize off = ((VkDeviceSize)view * numBatches_ + groupStart_[group]) * sizeof(VkDrawIndexedIndirectCommand);
    vkCmdDrawIndexedIndirect(cmd, curCmds_, off, groupCount_[group],
                             sizeof(VkDrawIndexedIndirectCommand));
}

void Renderer::record(VkCommandBuffer cmd, uint32_t frame, const Camera& cam, float time, const OutputTarget& out) {
    updateFrame(frame, cam, time);
    curCmds_ = cmds_[frame].buffer;
    uint32_t q0 = frame * (FrameStats::kPasses + 1);
    auto stamp = [&](uint32_t i, VkPipelineStageFlags2 stage) {
        if (queries_) vkCmdWriteTimestamp2(cmd, stage, queries_, q0 + i);
    };
    if (queries_) vkCmdResetQueryPool(cmd, queries_, q0, FrameStats::kPasses + 1);
    stamp(0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets_[frame], 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[frame], 0, nullptr);
    vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);

    // 0. moving instances
    if (!dynamic_.empty() && dynamicFirst_ + dynamic_.size() <= numInstances_) {
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const size_t chunk = 65536 / sizeof(GpuInstance);
        for (size_t i = 0; i < dynamic_.size(); i += chunk) {
            size_t n = std::min(chunk, dynamic_.size() - i);
            vkCmdUpdateBuffer(cmd, instances_.buffer, (dynamicFirst_ + i) * sizeof(GpuInstance), n * sizeof(GpuInstance), &dynamic_[i]);
        }
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    // 1. GPU culling into indirect draws
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    Push p{0, 0, 0, 0};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
    if (numBatches_ > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullReset_);
        vkCmdDispatch(cmd, (numBatches_ * views_ + 63) / 64, 1, 1);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (numInstances_ > 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_);
            vkCmdDispatch(cmd, (numInstances_ + 63) / 64, 1, 1);
        }
    }
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    stamp(1, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    // 2. shadow cascades
    if (s_.cascades > 0) {
        imageBarrier(cmd, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        for (uint32_t c = 0; c < s_.cascades; ++c) {
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = shadowMap_.layerViews[c];
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {s_.shadowSize, s_.shadowSize}};
            ri.layerCount = 1;
            ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cmd, &ri);
            setViewport(cmd, s_.shadowSize, s_.shadowSize, true);
            vkCmdSetDepthBias(cmd, 1.5f, 0.0f, 2.0f);
            drawGroup(cmd, c + 1, GroupOpaque, shadowOpaque_);
            drawGroup(cmd, c + 1, GroupTwoSided, shadowOpaque_);
            drawGroup(cmd, c + 1, GroupMasked, shadowMasked_);
            vkCmdEndRendering(cmd);
        }
        imageBarrier(cmd, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    stamp(2, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);

    // 3. main pass: opaque, two-sided, masked, sky (MSAA, resolved into hdr_ / depth_)
    bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    constexpr VkPipelineStageFlags2 kAtt = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    imageBarrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, kAtt,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // (depth resolves write in the colour-attachment stage, so both stages are covered)
    imageBarrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kDepthStages, 0, kAtt | kDepthStages,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    if (msaa) {
        imageBarrier(cmd, hdrMsaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAtt, 0, kAtt, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        imageBarrier(cmd, depthMsaa_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthStages, 0, kDepthStages,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    }
    {
        VkRenderingAttachmentInfo ca = msaa ? colorAttachment(hdrMsaa_.view, VK_ATTACHMENT_LOAD_OP_CLEAR, hdr_.view)
                                            : colorAttachment(hdr_.view, VK_ATTACHMENT_LOAD_OP_CLEAR);
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = msaa ? depthMsaa_.view : depth_.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil = {0.0f, 0};   // reversed Z: 0 is infinitely far
        if (msaa) {
            da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            da.resolveImageView = depth_.view;
            da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {s_.width, s_.height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        setViewport(cmd, s_.width, s_.height, true);
        drawGroup(cmd, 0, GroupOpaque, opaque_);
        drawGroup(cmd, 0, GroupTwoSided, twoSided_);
        drawGroup(cmd, 0, GroupMasked, masked_);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
    stamp(3, kAtt);

    // 4. water and blended surfaces over a copy of the opaque scene
    imageBarrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, kAtt | kDepthStages,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kDepthStages,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    bool transparent = groupCount_[GroupWater] + groupCount_[GroupBlend] > 0;
    if (transparent) {
        imageBarrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kAtt, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        imageBarrier(cmd, sceneCopy_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy ic{};
        ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        ic.extent = {s_.width, s_.height, 1};
        vkCmdCopyImage(cmd, hdr_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sceneCopy_.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        imageBarrier(cmd, sceneCopy_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imageBarrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, kAtt,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
        VkRenderingAttachmentInfo ca = colorAttachment(hdr_.view, VK_ATTACHMENT_LOAD_OP_LOAD);
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth_.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {s_.width, s_.height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        setViewport(cmd, s_.width, s_.height, true);
        drawGroup(cmd, 0, GroupWater, water_);
        drawGroup(cmd, 0, GroupBlend, blend_);
        vkCmdEndRendering(cmd);
    }
    imageBarrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kAtt, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    stamp(4, kAtt);

    // 5. tone mapping into the output image
    imageBarrier(cmd, out.image, VK_IMAGE_ASPECT_COLOR_BIT, out.initialLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 kAtt | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, kAtt, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    {
        VkRenderingAttachmentInfo ca = colorAttachment(out.view, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {s_.width, s_.height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cmd, &ri);
        setViewport(cmd, s_.width, s_.height, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeline(out.format));
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
    if (out.finalLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        imageBarrier(cmd, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, out.finalLayout, kAtt,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT);
    stamp(5, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
}

bool Renderer::readStats(uint32_t frame, FrameStats& st) {
    if (!queries_) return false;
    uint64_t t[FrameStats::kPasses + 1];
    VkResult r = vkGetQueryPoolResults(d_->device, queries_, frame * (FrameStats::kPasses + 1), FrameStats::kPasses + 1,
                                       sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return false;
    double toMs = d_->timestampPeriodNs * 1e-6;
    for (int i = 0; i < FrameStats::kPasses; ++i) st.ms[i] = (double)(t[i + 1] - t[i]) * toMs;
    st.totalMs = (double)(t[FrameStats::kPasses] - t[0]) * toMs;
    return true;
}

void Renderer::shutdown() {
    if (!d_) return;
    Device& d = *d_;
    d.waitIdle();
    destroyTargets();
    for (Buffer* b : {&vertices_, &indices_, &instances_, &meshInfos_, &lods_, &batchRefs_, &batches_, &materials_, &lights_, &lightGrid_})
        if (b->buffer) destroyBuffer(d, *b);
    for (uint32_t f = 0; f < kFrames; ++f)
        for (Buffer* b : {&ubo_[f], &visible_[f], &cmds_[f]})
            if (b->buffer) destroyBuffer(d, *b);
    for (auto& t : textures_) destroyImage(d, t);
    if (skyLut_.image) destroyImage(d, skyLut_);
    for (VkPipeline p : {cullReset_, cull_, opaque_, twoSided_, masked_, sky_, shadowOpaque_, shadowMasked_, water_, blend_})
        if (p) vkDestroyPipeline(d.device, p, nullptr);
    for (auto& [f, p] : post_) vkDestroyPipeline(d.device, p, nullptr);
    post_.clear();
    for (VkSampler s : {linearRepeat_, linearClamp_, nearestClamp_, shadowSampler_, skySampler_}) vkDestroySampler(d.device, s, nullptr);
    if (queries_) vkDestroyQueryPool(d.device, queries_, nullptr);
    vkDestroyPipelineLayout(d.device, layout_, nullptr);
    vkDestroyDescriptorPool(d.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(d.device, setLayout_, nullptr);
    d_ = nullptr;
}
}  // namespace df
