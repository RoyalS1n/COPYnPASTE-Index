#pragma once
#include "scene/MeshAsset.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>

namespace df {
// Loads a texture referenced by a material, returns its index (or kNoTexture).
using TextureResolver = std::function<uint32_t(const std::string& relPath, bool srgb)>;

// Material from its JSON definition (see docs/MAP_FORMAT.md, "materials").
// Throws df::Error on an unknown shading model.
MaterialDef parseMaterial(const std::string& name, const nlohmann::json& j, const TextureResolver& textures);

// The engine's built-in material library: always available, overridable by a
// map material of the same name. Returns {name: definition}.
const nlohmann::json& builtinMaterials();
}  // namespace df
