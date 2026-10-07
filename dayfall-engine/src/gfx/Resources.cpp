#include "gfx/Resources.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace df {
Buffer createBuffer(Device& d, VkDeviceSize size, VkBufferUsageFlags usage, MemUsage mem) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<VkDeviceSize>(size, 16);
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (mem == MemUsage::Upload)
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else if (mem == MemUsage::Readback)
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(d.allocator, &bci, &aci, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    return b;
}

void destroyBuffer(Device& d, Buffer& b) {
    if (b.buffer) vmaDestroyBuffer(d.allocator, b.buffer, b.alloc);
    b = {};
}

Buffer createBufferWithData(Device& d, const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer dst = createBuffer(d, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemUsage::GpuOnly);
    if (!size) return dst;
    // stage in chunks so huge buffers (instances) never need one giant staging buffer
    const VkDeviceSize chunk = 64ull << 20;
    Buffer staging = createBuffer(d, std::min(size, chunk), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemUsage::Upload);
    for (VkDeviceSize off = 0; off < size; off += chunk) {
        VkDeviceSize n = std::min(chunk, size - off);
        std::memcpy(staging.mapped, static_cast<const uint8_t*>(data) + off, n);
        vmaFlushAllocation(d.allocator, staging.alloc, 0, n);
        d.immediate([&](VkCommandBuffer cmd) {
            VkBufferCopy c{0, off, n};
            vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &c);
        });
    }
    destroyBuffer(d, staging);
    return dst;
}

VkImageAspectFlags aspectOf(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

uint32_t mipCount(uint32_t w, uint32_t h) { return 1 + (uint32_t)std::floor(std::log2((double)std::max(w, h))); }

Image createImage(Device& d, const ImageDesc& desc) {
    Image img;
    img.format = desc.format;
    img.extent = {desc.width, desc.height};
    img.mips = desc.mips;
    img.layers = desc.layers;
    img.samples = desc.samples;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = desc.format;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = desc.mips;
    ici.arrayLayers = desc.layers;
    ici.samples = desc.samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = desc.usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(d.allocator, &ici, &aci, &img.image, &img.alloc, nullptr));
    const VkImageUsageFlags viewable = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!(desc.usage & viewable)) return img;   // transfer-only images have no views
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = (desc.layers > 1 || desc.arrayView) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = desc.format;
    vci.subresourceRange = {aspectOf(desc.format), 0, desc.mips, 0, desc.layers};
    VK_CHECK(vkCreateImageView(d.device, &vci, nullptr, &img.view));
    if (desc.layers > 1 || desc.arrayView) {
        for (uint32_t l = 0; l < desc.layers; ++l) {
            VkImageViewCreateInfo lv = vci;
            lv.viewType = VK_IMAGE_VIEW_TYPE_2D;
            lv.subresourceRange = {aspectOf(desc.format), 0, 1, l, 1};
            VkImageView v;
            VK_CHECK(vkCreateImageView(d.device, &lv, nullptr, &v));
            img.layerViews.push_back(v);
        }
    }
    return img;
}

void destroyImage(Device& d, Image& img) {
    for (auto v : img.layerViews) vkDestroyImageView(d.device, v, nullptr);
    if (img.view) vkDestroyImageView(d.device, img.view, nullptr);
    if (img.image) vmaDestroyImage(d.allocator, img.image, img.alloc);
    img = {};
}

Image createTexture(Device& d, const uint8_t* rgba, uint32_t w, uint32_t h, bool srgb) {
    ImageDesc desc;
    desc.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    desc.width = w;
    desc.height = h;
    desc.mips = mipCount(w, h);
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Image img = createImage(d, desc);
    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    Buffer staging = createBuffer(d, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemUsage::Upload);
    std::memcpy(staging.mapped, rgba, size);
    vmaFlushAllocation(d.allocator, staging.alloc, 0, size);
    d.immediate([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        int32_t mw = (int32_t)w, mh = (int32_t)h;
        for (uint32_t m = 1; m < img.mips; ++m) {
            imageBarrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, m - 1, 1);
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
            b.srcOffsets[1] = {mw, mh, 1};
            mw = std::max(1, mw / 2);
            mh = std::max(1, mh / 2);
            b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            b.dstOffsets[1] = {mw, mh, 1};
            vkCmdBlitImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            imageBarrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, m - 1, 1);
        }
        imageBarrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, img.mips - 1, 1);
    });
    destroyBuffer(d, staging);
    return img;
}

void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess, uint32_t baseMip, uint32_t mips, uint32_t baseLayer, uint32_t layers) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, baseMip, mips, baseLayer, layers};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}
}  // namespace df
