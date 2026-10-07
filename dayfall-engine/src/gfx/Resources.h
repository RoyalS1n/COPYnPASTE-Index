#pragma once
#include "gfx/Device.h"

namespace df {
enum class MemUsage { GpuOnly, Upload, Readback };

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkDeviceSize size = 0;
    void* mapped = nullptr;   // persistent mapping for Upload / Readback buffers
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;           // whole image (all mips, all layers)
    std::vector<VkImageView> layerViews;         // one 2D view per layer (render targets)
    VmaAllocation alloc = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t mips = 1, layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

Buffer createBuffer(Device& d, VkDeviceSize size, VkBufferUsageFlags usage, MemUsage mem);
void destroyBuffer(Device& d, Buffer& b);
// GPU-only buffer filled through a staging copy
Buffer createBufferWithData(Device& d, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

struct ImageDesc {
    VkFormat format;
    uint32_t width, height;
    uint32_t mips = 1, layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage;
    bool arrayView = false;    // view as 2D array even with one layer
};
Image createImage(Device& d, const ImageDesc& desc);
void destroyImage(Device& d, Image& img);
// RGBA8 texture with a full mip chain generated on the GPU
Image createTexture(Device& d, const uint8_t* rgba, uint32_t w, uint32_t h, bool srgb);

VkImageAspectFlags aspectOf(VkFormat f);
uint32_t mipCount(uint32_t w, uint32_t h);

// synchronization2 helpers
void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess, uint32_t baseMip = 0, uint32_t mips = VK_REMAINING_MIP_LEVELS,
                  uint32_t baseLayer = 0, uint32_t layers = VK_REMAINING_ARRAY_LAYERS);
void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
}  // namespace df
