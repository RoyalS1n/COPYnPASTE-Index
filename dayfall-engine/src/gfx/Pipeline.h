#pragma once
#include "gfx/Device.h"
#include <filesystem>
#include <vector>

namespace df {
VkShaderModule loadShader(Device& d, const std::filesystem::path& spvPath);

struct GraphicsPipelineDesc {
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT;
    bool depthTest = true, depthWrite = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reversed Z
    bool blend = false;              // premultiplied-free standard alpha blend
    bool alphaToCoverage = false;
    bool depthBias = false;          // dynamic depth bias (shadows)
    bool depthClamp = false;
    std::vector<VkSpecializationMapEntry> specEntries;
    std::vector<uint8_t> specData;
};
VkPipeline createGraphicsPipeline(Device& d, const GraphicsPipelineDesc& desc);
VkPipeline createComputePipeline(Device& d, VkShaderModule cs, VkPipelineLayout layout);
}  // namespace df
