#pragma once
// Compiles the World (map document + terrain) into a render-ready Scene.
// Mesh assets, terrain chunks and scatter results are cached, so rebuilding
// after an edit only redoes what the edit touched.
#include "scene/Scene.h"
#include "scene/Skin.h"
#include "world/Scatter.h"
#include "world/World.h"
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace df {
struct EntityState {
    std::string id, type;
    vec3 position{0};            // resting position (world)
    vec3 color{1};
    float radius = 1.2f;         // pickup / trigger radius
    uint32_t instance = UINT32_MAX;
    int light = -1;
    std::string message;
    nlohmann::json props;
};

struct BuildInfo {
    double ms = 0;
    size_t instances = 0, meshes = 0, triangles = 0;
    std::vector<std::string> warnings;
    nlohmann::json toJson() const;
};

class SceneBuilder {
public:
    void init(const std::filesystem::path& contentDir);
    // Rebuilds `out` from the world. Never throws for bad content: problems become warnings.
    void build(World& w, Scene& out, std::vector<EntityState>& entities, BuildInfo& info);
    // A mesh reference: a name (map "meshes", the content library, built-ins) or a primitive spec object.
    std::shared_ptr<const MeshAsset> resolveMesh(const World& w, const nlohmann::json& ref);   // throws df::Error
    // the same mesh with some materials swapped: {"old material": "new material"}; cached
    std::shared_ptr<const MeshAsset> withMaterials(const std::shared_ptr<const MeshAsset>& a, const nlohmann::json& overrides);
    // Default collision of a mesh reference (from the library entry; primitives: mesh)
    CollisionDesc defaultCollision(const World& w, const nlohmann::json& ref) const;
    // Everything an agent can place: built-in primitives, content library, map meshes, materials.
    nlohmann::json catalog(const World& w) const;
    static CollisionDesc parseCollision(const nlohmann::json& j, CollisionDesc def);
    // The rigged character a map "player" section names (a skinned mesh entry, plus its animation_files); cached
    std::shared_ptr<const CharacterAsset> resolveCharacter(const World& w, const nlohmann::json& player);   // throws df::Error

    std::filesystem::path contentDir;
    uint32_t playerFirst = 0, playerCount = 0;   // mannequin part instances (or the rigged character's one) in the last built scene
    std::shared_ptr<const CharacterAsset> character;   // the player's rigged character in the last built scene (null: mannequin)
    // true once after the terrain heights / paint / paths changed (upload them to the GPU)
    bool takeTerrainChanged() { bool c = terrainChanged_; terrainChanged_ = false; return c; }

private:
    std::shared_ptr<const MeshAsset> loadFileAsset(const std::filesystem::path& file, const nlohmann::json& entry, const std::string& name);
    const nlohmann::json* meshEntry(const World& w, const std::string& name, std::filesystem::path& baseDir) const;
    nlohmann::json lib_ = nlohmann::json::object();
    std::unordered_map<std::string, std::shared_ptr<const MeshAsset>> assets_;
    std::unordered_map<std::string, std::filesystem::file_time_type> assetTimes_;
    // terrain chunk cache
    uint64_t chunkTerrainVersion_ = 0;
    std::string chunkPathsKey_;
    bool terrainChanged_ = false;
    std::shared_ptr<MeshAsset> horizon_;
    std::string horizonKey_;
    // scatter cache: rule id -> (key, points)
    struct ScatterCache { std::string key; std::vector<ScatterPoint> points; };
    std::unordered_map<std::string, ScatterCache> scatter_;
    std::unordered_map<std::string, Texture> textureCache_;
    std::string characterKey_;
    std::shared_ptr<const CharacterAsset> characterCache_;
};
}  // namespace df
