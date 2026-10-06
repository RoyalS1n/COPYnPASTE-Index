#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace df {
std::vector<uint8_t> encodePng(const uint8_t* rgba, uint32_t w, uint32_t h);
std::vector<uint8_t> encodeJpeg(const uint8_t* rgba, uint32_t w, uint32_t h, int quality = 88);
bool writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& data);
std::string base64(const std::vector<uint8_t>& data);
}  // namespace df
