#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace df {
namespace fs = std::filesystem;
std::vector<uint8_t> readBinary(const fs::path& p);
std::string readText(const fs::path& p);
bool writeText(const fs::path& p, const std::string& s);
fs::path executableDir();
}  // namespace df
