#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace df {
// Pretty JSON that stays compact: numbers, points and small objects stay on
// one line. Map files are read by agents, so lines are tokens.
std::string dumpReadable(const nlohmann::json& j, int indent = 2, size_t inlineWidth = 100);
}  // namespace df
