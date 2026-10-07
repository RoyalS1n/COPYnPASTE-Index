#pragma once
#include "scene/Scene.h"
#include "scene/Skin.h"
#include <filesystem>

namespace df {
// Every mesh node of a glTF file's default scene merged into ONE mesh asset,
// node transforms applied, converted to Z up. Throws df::Error.
MeshAsset loadGltfAsset(const std::filesystem::path& file, const std::string& name);

// A whole glTF scene (e.g. a level exported from Unreal or Blender): each glTF
// mesh becomes a mesh asset, each node a placement (EXT_mesh_gpu_instancing
// supported), KHR_lights_punctual point lights become lights.
struct GltfScene {
    struct Placement { uint32_t mesh; vec3 position; quat rotation; vec3 scale; };   // per-axis scale in world axes
    std::vector<MeshAsset> meshes;
    std::vector<Placement> placements;
    std::vector<LightDef> lights;
};
GltfScene loadGltfScene(const std::filesystem::path& file);

// A rigged character (glTF skins: JOINTS_n / WEIGHTS_n, inverse bind matrices,
// the joint hierarchy) with the file's animations. The mesh is in model space:
// Z up, scaled, turned by yawDeg about Z (the caller turns the front to +Y).
// Unskinned meshes in the file follow their nearest joint ancestor. Throws df::Error.
struct CharacterImport { float scale = 1.0f, yawDeg = 0.0f; bool groundOrigin = false; };
CharacterAsset loadGltfCharacter(const std::filesystem::path& file, const std::string& name, const CharacterImport& imp);
// Adds the animations of another file with the same skeleton (joints matched by name, Mixamo style).
void loadGltfClips(const std::filesystem::path& file, CharacterAsset& c);
// Skin and clip facts from the JSON alone (no buffers): catalog, asset_import.
struct GltfRigInfo { size_t meshes = 0, joints = 0; std::vector<std::pair<std::string, float>> clips; };   // clip name, seconds
GltfRigInfo inspectGltfRig(const std::filesystem::path& file);

Texture loadTextureFile(const std::filesystem::path& file, bool srgb);   // throws df::Error
uint32_t groupOf(const GpuMaterial& m);                                  // pipeline group of a material
}  // namespace df
