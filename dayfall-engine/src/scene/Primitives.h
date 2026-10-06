#pragma once
// Parametric procedural meshes for blockout and built-in gameplay pieces.
// Every primitive sits on its origin (z = 0 is the base) unless noted, with
// metre-scale UVs so tiling materials keep their size.
#include "scene/MeshAsset.h"
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace df {
MeshAsset makeBox(vec3 size, const std::string& material);
MeshAsset makePlane(vec2 size, const std::string& material, uint32_t subdivisions = 1);
MeshAsset makeCylinder(float radius, float height, uint32_t segments, const std::string& material, bool caps = true);
MeshAsset makeCone(float radius, float height, uint32_t segments, const std::string& material);
MeshAsset makeSphere(float radius, uint32_t segments, const std::string& material);        // centre at z = radius
MeshAsset makeCapsule(float radius, float height, uint32_t segments, const std::string& material);
MeshAsset makeRamp(vec3 size, const std::string& material);                                 // rises along +Y
MeshAsset makeStairs(vec3 size, uint32_t steps, const std::string& material);               // rises along +Y
MeshAsset makeGem(float size, const std::string& material);                                 // centre at origin

// Builds a primitive from a spec such as {"type": "box", "size": [4, 0.3, 2.5], "material": "plaster"}.
// Returns false if `type` is not a primitive. Throws df::Error on bad parameters.
bool makePrimitive(const nlohmann::json& spec, MeshAsset& out);
// Stable cache key / mesh name for a primitive spec.
std::string primitiveKey(const nlohmann::json& spec);
// Names and parameter docs of every primitive (for the agent API).
nlohmann::json primitiveCatalog();
}  // namespace df
