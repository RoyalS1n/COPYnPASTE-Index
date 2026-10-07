#include "gfx/Pipeline.h"
#include "core/FileSystem.h"

namespace df {
VkShaderModule loadShader(Device& d, const std::filesystem::path& spvPath) {
    auto code = readBinary(spvPath);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(d.device, &ci, nullptr, &m));
    return m;
}

VkPipeline createGraphicsPipeline(Device& d, const GraphicsPipelineDesc& desc) {
    VkSpecializationInfo spec{};
    spec.mapEntryCount = (uint32_t)desc.specEntries.size();
    spec.pMapEntries = desc.specEntries.data();
    spec.dataSize = desc.specData.size();
    spec.pData = desc.specData.data();
    VkPipelineShaderStageCreateInfo stages[2]{};
    uint32_t stageCount = 0;
    stages[stageCount++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                            desc.vert, "main", desc.specEntries.empty() ? nullptr : &spec};
    if (desc.frag)
        stages[stageCount++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                                desc.frag, "main", desc.specEntries.empty() ? nullptr : &spec};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};  // vertex pulling
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = desc.cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = desc.depthBias;
    rs.depthClampEnable = desc.depthClamp;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = desc.samples;
    ms.alphaToCoverageEnable = desc.alphaToCoverage;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = desc.depthTest;
    ds.depthWriteEnable = desc.depthWrite;
    ds.depthCompareOp = desc.depthCompare;
    std::vector<VkPipelineColorBlendAttachmentState> blends(desc.colorFormats.size());
    for (auto& b : blends) {
        b.colorWriteMask = 0xF;
        if (desc.blend || desc.additive) {
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = desc.additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
            b.dstColorBlendFactor = desc.additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.colorBlendOp = VK_BLEND_OP_ADD;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = (uint32_t)blends.size();
    cb.pAttachments = blends.data();
    std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    if (desc.depthBias) dyn.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = (uint32_t)dyn.size();
    dy.pDynamicStates = dyn.data();
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = (uint32_t)desc.colorFormats.size();
    ri.pColorAttachmentFormats = desc.colorFormats.data();
    ri.depthAttachmentFormat = desc.depthFormat;
    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    gci.stageCount = stageCount;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &dy;
    gci.layout = desc.layout;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &gci, nullptr, &p));
    return p;
}

VkPipeline createComputePipeline(Device& d, VkShaderModule cs, VkPipelineLayout layout) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main"};
    ci.layout = layout;
    VkPipeline p;
    VK_CHECK(vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    return p;
}
}  // namespace df
