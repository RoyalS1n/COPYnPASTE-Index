#include "render/HudRenderer.h"
#include "gfx/Pipeline.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_easy_font.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace df {
namespace {
// stb_easy_font (public domain) builds each glyph from segments on a unit
// grid. They are rasterised once into an atlas at kTexel pixels per unit,
// with a soft outline in the green channel, and drawn as one quad per glyph.
constexpr int kTexel = 4;            // atlas pixels per font unit
constexpr float kPad = 2.0f;         // font units around each glyph: outline and mip margin
constexpr float kOutline = 1.3f;     // outline radius, font units
constexpr float kSpacing = 0.5f;     // extra font units between letters
constexpr uint32_t kCols = 16, kRows = 6;

struct Font {
    uint32_t w = 0, h = 0;
    float cellW = 0, cellH = 0;      // font units, padding included
    float advance[96]{};
    std::vector<uint8_t> rgba;
};

const Font& font() {
    static const Font f = [] {
        Font f;
        struct Box { float x0, y0, x1, y1; };
        std::vector<Box> boxes[95];
        float maxX = 0, maxY = 0;
        for (int c = 32; c < 127; ++c) {
            char txt[2] = {(char)c, 0};
            alignas(16) char buf[64 * 64];
            int quads = stb_easy_font_print(0, 0, txt, nullptr, buf, (int)sizeof(buf));
            for (int q = 0; q < quads; ++q) {
                float v0[2], v2[2];   // quad corners 0 (min) and 2 (max)
                std::memcpy(v0, buf + q * 64, 8);
                std::memcpy(v2, buf + q * 64 + 32, 8);
                boxes[c - 32].push_back({v0[0], v0[1], v2[0], v2[1]});
                maxX = std::max(maxX, v2[0]);
                maxY = std::max(maxY, v2[1]);
            }
            f.advance[c - 32] = (float)(stb_easy_font_charinfo[c - 32].advance & 15);
        }
        f.cellW = std::ceil(maxX) + 2 * kPad;
        f.cellH = std::ceil(maxY) + 2 * kPad;
        uint32_t cw = (uint32_t)f.cellW * kTexel, ch = (uint32_t)f.cellH * kTexel;
        f.w = kCols * cw;
        f.h = kRows * ch;
        std::vector<uint8_t> glyph(f.w * f.h, 0), outline(f.w * f.h, 0);
        for (int i = 0; i < 95; ++i) {
            uint32_t ox = (i % kCols) * cw + (uint32_t)(kPad * kTexel), oy = (i / kCols) * ch + (uint32_t)(kPad * kTexel);
            for (const Box& b : boxes[i])
                for (uint32_t y = oy + (uint32_t)(b.y0 * kTexel); y < oy + (uint32_t)(b.y1 * kTexel); ++y)
                    for (uint32_t x = ox + (uint32_t)(b.x0 * kTexel); x < ox + (uint32_t)(b.x1 * kTexel); ++x) glyph[y * f.w + x] = 255;
        }
        // outline: the glyph dilated by a disc with a one-texel soft edge (stays inside the padded cell)
        const float r = kOutline * kTexel;
        const int ri = (int)std::ceil(r + 0.5f);
        for (int y = 0; y < (int)f.h; ++y)
            for (int x = 0; x < (int)f.w; ++x) {
                if (!glyph[y * f.w + x]) continue;
                for (int dy = -ri; dy <= ri; ++dy)
                    for (int dx = -ri; dx <= ri; ++dx) {
                        int px = x + dx, py = y + dy;
                        if (px < 0 || py < 0 || px >= (int)f.w || py >= (int)f.h) continue;
                        float cov = std::clamp(r + 0.5f - std::sqrt((float)(dx * dx + dy * dy)), 0.0f, 1.0f);
                        uint8_t& o = outline[py * f.w + px];
                        o = std::max(o, (uint8_t)std::lround(cov * 255.0f));
                    }
            }
        f.rgba.resize((size_t)f.w * f.h * 4);
        for (size_t i = 0; i < glyph.size(); ++i) {
            f.rgba[i * 4 + 0] = glyph[i];
            f.rgba[i * 4 + 1] = outline[i];
            f.rgba[i * 4 + 2] = 0;
            f.rgba[i * 4 + 3] = 255;
        }
        return f;
    }();
    return f;
}

// UTF-8 to the font's printable ASCII: quotes, dashes and Latin-1 letters become look-alikes, the rest '?'
std::string toAscii(std::string_view s) {
    static const char* latin1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00C0..U+00FF
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n > 1) cp = c & (0x7F >> n);
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 63);
        i += n;
        if (cp >= 32 && cp < 127) out += (char)cp;
        else if (cp == '\t' || cp == '\n' || cp == 0xA0) out += ' ';
        else if (cp >= 0xC0 && cp <= 0xFF) out += latin1[cp - 0xC0];
        else if (cp == 0x2018 || cp == 0x2019) out += '\'';
        else if (cp == 0x201C || cp == 0x201D) out += '"';
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) out += '-';
        else if (cp == 0x2026) out += "...";
        else out += '?';
    }
    return out;
}
}  // namespace

uint32_t hudColor(float r, float g, float b, float a) {
    auto u = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return u(r) | u(g) << 8 | u(b) << 16 | u(a) << 24;
}

// ---------------------------------------------------------------------------------------------- canvas
void HudCanvas::reset(uint32_t w, uint32_t h) {
    width = w;
    height = h;
    verts.clear();
    // 1 at 1080p; relatively larger on small outputs, limited by the width on narrow ones
    ui = std::min(std::pow(h / 1080.0f, 0.75f), 1.25f * std::pow(w / 1920.0f, 0.75f));
}

void HudCanvas::quad(const HudVertex (&v)[4]) {
    verts.insert(verts.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

void HudCanvas::rect(vec2 a, vec2 b, uint32_t color) { rectH(a, b, color, color); }

void HudCanvas::rectH(vec2 a, vec2 b, uint32_t left, uint32_t right) {
    quad({{a, vec2(0), vec2(0), left, HudSolid}, {vec2(b.x, a.y), vec2(0), vec2(0), right, HudSolid},
          {b, vec2(0), vec2(0), right, HudSolid}, {vec2(a.x, b.y), vec2(0), vec2(0), left, HudSolid}});
}

void HudCanvas::tri(vec2 a, vec2 b, vec2 c, uint32_t color) {
    verts.insert(verts.end(), {HudVertex{a, vec2(0), vec2(0), color, HudSolid}, HudVertex{b, vec2(0), vec2(0), color, HudSolid},
                               HudVertex{c, vec2(0), vec2(0), color, HudSolid}});
}

void HudCanvas::disc(vec2 c, float r, uint32_t color) {
    quad({{c + vec2(-r, -r), vec2(0), vec2(-1, -1), color, HudDisc}, {c + vec2(r, -r), vec2(0), vec2(1, -1), color, HudDisc},
          {c + vec2(r, r), vec2(0), vec2(1, 1), color, HudDisc}, {c + vec2(-r, r), vec2(0), vec2(-1, 1), color, HudDisc}});
}

void HudCanvas::ring(vec2 c, float r, float w, uint32_t color) {
    vec2 in(std::max(0.0f, (r - w) / r), 0.0f);
    quad({{c + vec2(-r, -r), in, vec2(-1, -1), color, HudRing}, {c + vec2(r, -r), in, vec2(1, -1), color, HudRing},
          {c + vec2(r, r), in, vec2(1, 1), color, HudRing}, {c + vec2(-r, r), in, vec2(-1, 1), color, HudRing}});
}

float HudCanvas::text(vec2 pos, std::string_view s, float scale, uint32_t color, uint32_t outline) {
    const Font& f = font();
    std::string t = toAscii(s);
    vec2 cell(f.cellW, f.cellH), atlas(f.cellW * kCols, f.cellH * kRows);
    float x = pos.x;
    // outlines first, so no outline covers the neighbouring letter
    for (int pass = (outline >> 24) ? 0 : 1; pass < 2; ++pass) {
        x = pos.x;
        for (unsigned char ch : t) {
            int i = ch - 32;
            if (ch != ' ') {
                vec2 p0 = vec2(x, pos.y) - vec2(kPad * scale), p1 = p0 + cell * scale;
                vec2 t0 = vec2((float)(i % kCols), (float)(i / kCols)) * cell / atlas, t1 = t0 + cell / atlas;
                uint32_t col = pass ? color : outline, mode = pass ? HudText : HudOutline;
                quad({{p0, t0, vec2(0), col, mode}, {vec2(p1.x, p0.y), vec2(t1.x, t0.y), vec2(0), col, mode},
                      {p1, t1, vec2(0), col, mode}, {vec2(p0.x, p1.y), vec2(t0.x, t1.y), vec2(0), col, mode}});
            }
            x += (f.advance[i] + kSpacing) * scale;
        }
    }
    return t.empty() ? 0.0f : x - pos.x - kSpacing * scale;
}

float HudCanvas::textWidth(std::string_view s, float scale) {
    const Font& f = font();
    std::string t = toAscii(s);
    float w = 0;
    for (unsigned char ch : t) w += f.advance[ch - 32] + kSpacing;
    return t.empty() ? 0.0f : (w - kSpacing) * scale;
}

// ---------------------------------------------------------------------------------------------- renderer
void HudRenderer::init(Device& d, const std::string& shaderDir) {
    d_ = &d;
    shaderDir_ = shaderDir;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(d.device, &sci, nullptr, &sampler_));

    // the HUD's own set 0: vertices, font atlas, minimap
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3;
    lci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(d.device, &lci, nullptr, &setLayout_));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrames}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kFrames}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kFrames;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(d.device, &pci, nullptr, &pool_));
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(d.device, &plci, nullptr, &layout_));

    for (uint32_t f = 0; f < kFrames; ++f) {
        verts_[f] = createBuffer(d, (VkDeviceSize)kMaxVerts * sizeof(HudVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemUsage::Upload);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        VK_CHECK(vkAllocateDescriptorSets(d.device, &ai, &sets_[f]));
    }
    const Font& fnt = font();
    font_ = createTexture(d, fnt.rgba.data(), fnt.w, fnt.h, false);
    const uint8_t dark[4] = {12, 14, 18, 255};
    minimap_ = createTexture(d, dark, 1, 1, true);
    writeDescriptors();
    if (d.timestamps) {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kFrames * 2;
        VK_CHECK(vkCreateQueryPool(d.device, &qci, nullptr, &queries_));
    }
}

void HudRenderer::writeDescriptors() {
    for (uint32_t f = 0; f < kFrames; ++f) {
        VkDescriptorBufferInfo bi{verts_[f].buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo ii[2] = {{sampler_, font_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {sampler_, minimap_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = sets_[f];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i == 0) w[i].pBufferInfo = &bi;
            else w[i].pImageInfo = &ii[i - 1];
        }
        vkUpdateDescriptorSets(d_->device, 3, w, 0, nullptr);
    }
}

void HudRenderer::setMinimap(const uint8_t* rgba, uint32_t w, uint32_t h) {
    d_->waitIdle();
    destroyImage(*d_, minimap_);
    minimap_ = createTexture(*d_, rgba, w, h, true);
    writeDescriptors();
}

VkPipeline HudRenderer::pipeline(VkFormat format) {
    for (auto& [f, p] : pipes_) if (f == format) return p;
    VkShaderModule vs = loadShader(*d_, shaderDir_ + "/hud.vert.spv"), fs = loadShader(*d_, shaderDir_ + "/hud.frag.spv");
    GraphicsPipelineDesc g;
    g.layout = layout_;
    g.vert = vs;
    g.frag = fs;
    g.colorFormats = {format};
    g.cull = VK_CULL_MODE_NONE;
    g.depthTest = g.depthWrite = false;
    g.blend = true;
    VkPipeline p = createGraphicsPipeline(*d_, g);
    vkDestroyShaderModule(d_->device, vs, nullptr);
    vkDestroyShaderModule(d_->device, fs, nullptr);
    pipes_.push_back({format, p});
    return p;
}

void HudRenderer::record(VkCommandBuffer cmd, uint32_t frame, VkImageView view, VkFormat format, const HudCanvas& c) {
    uint32_t n = (uint32_t)std::min<size_t>(c.verts.size(), kMaxVerts) / 3 * 3;
    if (n == 0 || c.width == 0 || c.height == 0) return;
    std::memcpy(verts_[frame].mapped, c.verts.data(), n * sizeof(HudVertex));
    vmaFlushAllocation(d_->allocator, verts_[frame].alloc, 0, n * sizeof(HudVertex));
    constexpr VkPipelineStageFlags2 kAtt = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (queries_) {
        vkCmdResetQueryPool(cmd, queries_, frame * 2, 2);
        vkCmdWriteTimestamp2(cmd, kAtt, queries_, frame * 2);
    }
    // blend over what the post pass just wrote
    memoryBarrier(cmd, kAtt, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kAtt,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {c.width, c.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, (float)c.width, (float)c.height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(format));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[frame], 0, nullptr);
    float push[4] = {1.0f / c.width, 1.0f / c.height, 0, 0};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, n, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (queries_) {
        vkCmdWriteTimestamp2(cmd, kAtt, queries_, frame * 2 + 1);
        stamped_[frame] = true;
    }
}

bool HudRenderer::readStats(uint32_t frame, double& ms) {
    if (!queries_ || !stamped_[frame]) return false;
    uint64_t t[2];
    if (vkGetQueryPoolResults(d_->device, queries_, frame * 2, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return false;
    stamped_[frame] = false;
    ms = (double)(t[1] - t[0]) * d_->timestampPeriodNs * 1e-6;
    return true;
}

void HudRenderer::shutdown() {
    if (!d_) return;
    Device& d = *d_;
    d.waitIdle();
    for (auto& b : verts_) if (b.buffer) destroyBuffer(d, b);
    for (Image* i : {&font_, &minimap_}) if (i->image) destroyImage(d, *i);
    for (auto& [f, p] : pipes_) vkDestroyPipeline(d.device, p, nullptr);
    pipes_.clear();
    if (queries_) vkDestroyQueryPool(d.device, queries_, nullptr);
    vkDestroySampler(d.device, sampler_, nullptr);
    vkDestroyPipelineLayout(d.device, layout_, nullptr);
    vkDestroyDescriptorPool(d.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(d.device, setLayout_, nullptr);
    d_ = nullptr;
}
}  // namespace df
