#include "core/Image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <fstream>

namespace df {
std::vector<uint8_t> encodePng(const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out;
    stbi_write_png_compression_level = 6;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* o = static_cast<std::vector<uint8_t>*>(ctx);
            o->insert(o->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &out, (int)w, (int)h, 4, rgba, (int)w * 4);
    return out;
}

std::vector<uint8_t> encodeJpeg(const uint8_t* rgba, uint32_t w, uint32_t h, int quality) {
    std::vector<uint8_t> out;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            auto* o = static_cast<std::vector<uint8_t>*>(ctx);
            o->insert(o->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &out, (int)w, (int)h, 4, rgba, quality);
    return out;
}

bool writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& data) {
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data.data(), (std::streamsize)data.size());
    return (bool)f;
}

std::string base64(const std::vector<uint8_t>& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == d.size()) {
        uint32_t v = d[i] << 16;
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == d.size()) {
        uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
    }
    return o;
}
}  // namespace df
