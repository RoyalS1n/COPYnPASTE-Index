#pragma once
#include <functional>
// Render-ready scene: everything the renderer and physics consume, assembled
// by the scene builder from the editable world document. World space is
// right-handed, Z up, metres.
#include "core/Math.h"
#include "render/GpuTypes.h"
#include "scene/MeshAsset.h"
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace df {
struct Submesh {
    uint32_t firstIndex = 0, indexCount = 0;
    int32_t vertexOffset = 0;
    uint32_t material = 0;
};
struct MeshLodDesc { std::vector<uint32_t> submeshes; float maxDistance = 1e9f; };
struct Mesh {
    std::string name;
    vec4 bounds{0, 0, 0, 1};                 // local bounding sphere
    vec3 aabbMin{0}, aabbMax{0};
    std::vector<MeshLodDesc> lods;           // lods[0] is full detail
    uint32_t vertexStart = 0, vertexCount = 0;
    uint32_t firstIndex = 0, indexCount = 0; // all LODs
};

enum class CollisionKind { None, Mesh, Convex, Cylinder, Box, Sphere };
struct CollisionDesc {
    CollisionKind kind = CollisionKind::None;
    float radius = 0.4f, height = 4.0f;      // cylinder (trunks): radius and height from the instance origin
};
// A group of instances that share collision and an owner in the world document
struct InstanceSet {
    std::string name;                        // world id (object id, scatter id, ...)
    uint32_t mesh = 0;
    uint32_t first = 0, count = 0;           // range in Scene::instances
    CollisionDesc collision;
};
struct LightDef { vec3 position{0}; vec3 color{1}; float intensity = 10.0f; float range = 12.0f; std::string id; };
struct CameraDef { vec3 position{0}; vec3 target{0, 1, 0}; float vfovDeg = 50.0f; };

struct Environment {
    vec3 sunDir = glm::normalize(vec3(0.3f, -0.6f, 0.5f));   // towards the sun
    vec3 sunColor{1.0f, 0.95f, 0.9f};
    float sunStrength = 4.5f;
    float sunAngleDeg = 0.6f;
    float skyStrength = 0.1f;
    float airDensity = 1.0f, aerosolDensity = 1.6f, ozoneDensity = 1.0f;
    bool skyMultipleScattering = true;   // false: the older single-scattering sky
    float groundAlbedo = 0.3f;
    vec3 hazeNear{0.62f, 0.52f, 0.42f}, hazeFar{0.48f, 0.47f, 0.5f};
    float hazeAmount = 0.6f, mistStart = 30.0f, mistDepth = 6500.0f;
    float heightFogDensity = 0.0f, heightFogFalloff = 0.3f, heightFogBase = 0.0f;
    bool clouds = true;
    float cloudCoverage = 0.5f, cloudHeight = 1500.0f, cloudScale = 0.00032f;   // the Blender worlds' noise scale
    vec3 cloudColor{1.0f, 0.75f, 0.55f};
    float exposureEv = 0.85f;
    float lookPower = 1.15f, lookSaturation = 1.05f, lookSlope = 1.0f;   // AgX look
    float vignette = 0.25f;
    // environment.post: bloom (Blender's Glare node: threshold before exposure, size = the glow's extent relative
    // to the image), grade gains in scene-linear light, white balance, the painterly filter
    float bloomStrength = 0.0f, bloomThreshold = 0.8f, bloomSize = 0.7f;
    vec3 gain{1.0f}, highlightsGain{1.0f}, shadowsGain{1.0f};
    float whiteTempK = 6500.0f;
    bool painterly = false;
    float kuwaharaRadius = 2.0f, painterlyBlend = 0.6f, inkStrength = 0.25f, inkDepthK = 14.0f, inkNormalK = 1.2f, painterlyChroma = 1.08f;
    vec2 windDir{0.6f, 0.8f};
    float windStrength = 1.0f;
    float waterLevel = -1000.0f;
    float shadowDistance = 250.0f;
    bool skyOcclusion = true;                // environment.sky_occlusion (world/SkyOcclusion.h)
    float skyOccCell = 0.5f, skyOccStrength = 1.0f;
    uint32_t skyOccRays = 48;
};
struct SkyVolume;
struct PlayerStart { vec3 position{0, 0, 2}; float yawDeg = 90.0f; };

struct Scene {
    std::string name;
    std::vector<GpuVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Submesh> submeshes;
    std::vector<Mesh> meshes;
    std::vector<MaterialDef> materials;
    std::vector<Texture> textures;
    std::vector<GpuInstance> instances;
    std::vector<vec4> instanceScales;        // per-axis scales of stretched instances (InstAxisScale)
    std::vector<InstanceSet> sets;
    std::vector<LightDef> lights;
    std::map<std::string, CameraDef> cameras;
    PlayerStart playerStart;
    Environment env;
    std::unordered_map<std::string, uint32_t> meshByName, materialByName;
    vec3 boundsMin{1e30f}, boundsMax{-1e30f};
    uint32_t dynamicFirst = 0;               // instances from here on move every frame (player, pickups)
    uint32_t dynamicVertexFirst = 0, dynamicVertexCount = 0;   // vertices rewritten every frame (CPU-skinned characters)
    bool hasTerrain = false;                 // the heightfield itself is drawn by the renderer's terrain pass
    uint32_t terrainMaterial = 0;
    vec3 terrainMin{0}, terrainMax{0};
    std::shared_ptr<const SkyVolume> skyVolume;   // sky occlusion around static structures (null: none)

    // Defines a material on first use by name (content-library materials with textures load only when a mesh,
    // the terrain or an override names them). Set by the scene builder for the length of a build.
    std::function<void(Scene&, const std::string&)> materialSource;

    uint32_t addMaterial(MaterialDef m);
    uint32_t findOrAddMaterial(const std::string& name);
    bool hasMaterial(const std::string& name);   // asks materialSource for a missing one
    uint32_t addTexture(const Texture& t);   // deduplicated by name
    // Appends a mesh asset's geometry. Its parts' materials resolve by name:
    // a material already in the scene wins (borrowing the asset's base-colour
    // texture if it has none), otherwise the asset's own definition is added.
    uint32_t addMeshAsset(const MeshAsset& a, const std::string& name);
    uint32_t addInstance(uint32_t mesh, vec3 pos, quat rot, float scale, bool shadow = true, float cullDistance = 1e9f);
    uint32_t addInstance(uint32_t mesh, vec3 pos, quat rot, vec3 scale, bool shadow = true, float cullDistance = 1e9f);
    vec3 axisScale(const GpuInstance& inst) const;
    void computeBounds();
};
}  // namespace df
