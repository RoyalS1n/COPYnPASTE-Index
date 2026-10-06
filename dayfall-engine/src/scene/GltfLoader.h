#pragma once
#include "scene/Scene.h"
#include <filesystem>

namespace df {
// Every mesh node of a glTF file's default scene merged into ONE mesh asset,
// node transforms applied, converted to Z up. Throws df::Error.
MeshAsset loadGltfAsset(const std::filesystem::path& file, const std::string& name);

// A whole glTF scene (e.g. a level exported from Unreal or Blender): each glTF
// mesh becomes a mesh asset, each node a placement (EXT_mesh_gpu_instancing
// supported), KHR_lights_punctual point lights become lights.
struct GltfScene {
    struct Placement { uint32_t mesh; vec3 position; quat rotation; float scale; };
    std::vector<MeshAsset> meshes;
    std::vector<Placement> placements;
    std::vector<LightDef> lights;
};
GltfScene loadGltfScene(const std::filesystem::path& file);

Texture loadTextureFile(const std::filesystem::path& file, bool srgb);   // throws df::Error
uint32_t groupOf(const GpuMaterial& m);                                  // pipeline group of a material
}  // namespace df
