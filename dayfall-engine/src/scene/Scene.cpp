#include "scene/Scene.h"
#include "scene/GltfLoader.h"

namespace df {
uint32_t Scene::addMaterial(MaterialDef m) {
    auto it = materialByName.find(m.name);
    if (it != materialByName.end()) { materials[it->second] = std::move(m); return it->second; }
    uint32_t id = (uint32_t)materials.size();
    materialByName[m.name] = id;
    materials.push_back(std::move(m));
    return id;
}

uint32_t Scene::findOrAddMaterial(const std::string& name) {
    auto it = materialByName.find(name);
    if (it != materialByName.end()) return it->second;
    MaterialDef m;
    m.name = name;
    m.gpu.c[0] = vec4(0.6f, 0.6f, 0.6f, 1.0f);
    m.gpu.p[0] = vec4(0.8f, 0.0f, 0.5f, 1.0f);
    m.gpu.p[1].w = 1.0f;
    return addMaterial(m);
}

uint32_t Scene::addTexture(const Texture& t) {
    for (uint32_t i = 0; i < textures.size(); ++i)
        if (!t.name.empty() && textures[i].name == t.name && textures[i].srgb == t.srgb) return i;
    textures.push_back(t);
    return (uint32_t)textures.size() - 1;
}

uint32_t Scene::addMeshAsset(const MeshAsset& a, const std::string& name) {
    // materials
    auto remapTex = [&](uint32_t local) { return local == kNoTexture || local >= a.textures.size() ? kNoTexture : addTexture(a.textures[local]); };
    std::unordered_map<std::string, uint32_t> matIds;
    for (const auto& lod : a.lods)
        for (const MeshPart& p : lod) {
            if (matIds.count(p.material)) continue;
            const MaterialDef* own = nullptr;
            for (auto& m : a.materials) if (m.name == p.material) own = &m;
            auto it = materialByName.find(p.material);
            if (it != materialByName.end()) {
                MaterialDef& def = materials[it->second];
                if (own && def.gpu.h0.z == kNoTexture && own->gpu.h0.z != kNoTexture) def.gpu.h0.z = remapTex(own->gpu.h0.z);
                matIds[p.material] = it->second;
            } else if (own) {
                MaterialDef d = *own;
                d.gpu.h0.z = remapTex(d.gpu.h0.z);
                d.gpu.h0.w = remapTex(d.gpu.h0.w);
                d.gpu.h1.x = remapTex(d.gpu.h1.x);
                d.gpu.h1.y = remapTex(d.gpu.h1.y);
                d.group = groupOf(d.gpu);
                matIds[p.material] = addMaterial(d);
            } else {
                matIds[p.material] = findOrAddMaterial(p.material);
            }
        }
    Mesh mesh;
    mesh.name = name;
    mesh.bounds = a.bounds;
    mesh.aabbMin = a.aabbMin;
    mesh.aabbMax = a.aabbMax;
    mesh.vertexStart = (uint32_t)vertices.size();
    mesh.vertexCount = (uint32_t)a.vertices.size();
    mesh.firstIndex = (uint32_t)indices.size();
    mesh.indexCount = (uint32_t)a.indices.size();
    vertices.insert(vertices.end(), a.vertices.begin(), a.vertices.end());
    indices.insert(indices.end(), a.indices.begin(), a.indices.end());
    for (size_t l = 0; l < a.lods.size(); ++l) {
        MeshLodDesc lod;
        lod.maxDistance = l < a.lodDistances.size() ? a.lodDistances[l] : 1e9f;
        for (const MeshPart& p : a.lods[l]) {
            Submesh sm;
            sm.firstIndex = mesh.firstIndex + p.firstIndex;
            sm.indexCount = p.indexCount;
            sm.vertexOffset = (int32_t)mesh.vertexStart;
            sm.material = matIds[p.material];
            lod.submeshes.push_back((uint32_t)submeshes.size());
            submeshes.push_back(sm);
        }
        mesh.lods.push_back(std::move(lod));
    }
    uint32_t id = (uint32_t)meshes.size();
    meshByName[name] = id;
    meshes.push_back(std::move(mesh));
    return id;
}

uint32_t Scene::addInstance(uint32_t mesh, vec3 pos, quat rot, float scale, bool shadow, float cullDistance) {
    GpuInstance inst{};
    inst.posScale = vec4(pos, scale);
    inst.rot = vec4(rot.x, rot.y, rot.z, rot.w);
    inst.mesh = mesh;
    inst.flags = shadow ? InstShadow : 0;
    inst.cullDistance = cullDistance;
    inst.seed = (float)((instances.size() * 2654435761ull) % 10007ull) / 10007.0f;
    instances.push_back(inst);
    return (uint32_t)instances.size() - 1;
}

uint32_t Scene::addInstance(uint32_t mesh, vec3 pos, quat rot, vec3 scale, bool shadow, float cullDistance) {
    vec3 a = glm::abs(scale);
    float big = std::max(a.x, std::max(a.y, a.z));
    bool uniform = scale.x > 0 && std::abs(scale.x - scale.y) <= 1e-5f * big && std::abs(scale.x - scale.z) <= 1e-5f * big;
    if (uniform) return addInstance(mesh, pos, rot, scale.x, shadow, cullDistance);
    uint32_t id = addInstance(mesh, pos, rot, big, shadow, cullDistance);
    instances[id].flags |= InstAxisScale | ((uint32_t)instanceScales.size() << 8);
    instanceScales.push_back(vec4(scale, 0.0f));
    return id;
}

vec3 Scene::axisScale(const GpuInstance& inst) const {
    return (inst.flags & InstAxisScale) ? vec3(instanceScales[inst.flags >> 8]) : vec3(inst.posScale.w);
}

void Scene::computeBounds() {
    boundsMin = vec3(1e30f);
    boundsMax = vec3(-1e30f);
    for (auto& inst : instances) {
        const Mesh& m = meshes[inst.mesh];
        vec3 c = vec3(inst.posScale) + glm::rotate(quat(inst.rot.w, inst.rot.x, inst.rot.y, inst.rot.z), vec3(m.bounds) * axisScale(inst));
        float r = m.bounds.w * inst.posScale.w;
        boundsMin = glm::min(boundsMin, c - r);
        boundsMax = glm::max(boundsMax, c + r);
    }
    if (instances.empty()) boundsMin = boundsMax = vec3(0);
}
}  // namespace df
