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

namespace {
uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t k = i;
            for (int j = 0; j < 8; ++j) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void be32(std::vector<uint8_t>& o, uint32_t v) { o.insert(o.end(), {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}); }
void chunk(std::vector<uint8_t>& o, const char* type, const uint8_t* data, size_t n) {
    be32(o, (uint32_t)n);
    size_t start = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data, data + n);
    be32(o, crc32(&o[start], n + 4) ^ 0xFFFFFFFFu);
}
}  // namespace

std::vector<uint8_t> encodePng16(const uint16_t* gray, uint32_t w, uint32_t h) {
    // rows filtered with "up" (type 2): smooth heightmaps compress well
    size_t stride = (size_t)w * 2;
    std::vector<uint8_t> raw((stride + 1) * h);
    std::vector<uint8_t> prev(stride, 0), cur(stride);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t v = gray[(size_t)y * w + x];
            cur[x * 2] = (uint8_t)(v >> 8);
            cur[x * 2 + 1] = (uint8_t)v;
        }
        uint8_t* row = &raw[y * (stride + 1)];
        row[0] = 2;
        for (size_t i = 0; i < stride; ++i) row[1 + i] = (uint8_t)(cur[i] - prev[i]);
        prev.swap(cur);
    }
    int zlen = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), (int)raw.size(), &zlen, 8);
    std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {16, 0, 0, 0, 0});   // 16-bit, grayscale, deflate, adaptive filtering, no interlace
    chunk(o, "IHDR", ihdr.data(), ihdr.size());
    chunk(o, "IDAT", z, (size_t)zlen);
    chunk(o, "IEND", nullptr, 0);
    STBIW_FREE(z);
    return o;
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
