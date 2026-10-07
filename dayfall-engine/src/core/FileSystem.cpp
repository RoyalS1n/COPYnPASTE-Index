#include "core/FileSystem.h"
#include "core/Error.h"
#include "core/Log.h"
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace df {
std::vector<uint8_t> readBinary(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw Error(std::format("cannot open {}", p.string()));
    auto size = static_cast<size_t>(f.tellg());
    std::vector<uint8_t> data(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

std::string readText(const fs::path& p) {
    auto b = readBinary(p);
    return std::string(b.begin(), b.end());
}

bool writeText(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << s;
    return true;
}

fs::path executableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
}
}  // namespace df
