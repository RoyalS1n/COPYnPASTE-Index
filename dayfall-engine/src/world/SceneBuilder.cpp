#include "world/SceneBuilder.h"
#include "core/Error.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "game/Mannequin.h"
#include "scene/GltfLoader.h"
#include "scene/Materials.h"
#include "scene/Primitives.h"
#include "world/EnvironmentDoc.h"
#include <chrono>
#include <cstring>
#include <format>

namespace df {
using json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
vec3 v3(const json& j, vec3 def) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
std::string hexColor(vec3 c) {
    auto b = [](float v) { return (int)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return std::format("{:02x}{:02x}{:02x}", b(c.r), b(c.g), b(c.b));
}
// "scale": a number or per-axis [x, y, z]
vec3 parseScale(const json& j) {
    vec3 s = j.is_array() ? vec3(j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()) : vec3(j.get<float>());
    if (!(glm::abs(s.x) > 0 && glm::abs(s.y) > 0 && glm::abs(s.z) > 0)) throw Error("scale must not be 0");
    return s;
}
float maxAxis(vec3 s) { return std::max(std::abs(s.x), std::max(std::abs(s.y), std::abs(s.z))); }
float autoCull(const MeshAsset& a, float scale) {
    float r = a.bounds.w * scale;
    return std::clamp(r * 160.0f, 60.0f, 4000.0f);
}
}  // namespace

json BuildInfo::toJson() const {
    json w = json::array();
    for (auto& s : warnings) w.push_back(s);
    return {{"build_ms", std::round(ms * 10) / 10}, {"instances", instances}, {"meshes", meshes}, {"triangles_lod0", triangles}, {"warnings", w}};
}

void SceneBuilder::init(const fs::path& dir) {
    contentDir = dir;
    fs::path libFile = dir / "library.json";
    if (fs::exists(libFile)) {
        try {
            lib_ = json::parse(readText(libFile));
            logInfo("content library: {} meshes in {}", lib_.value("meshes", json::object()).size(), dir.string());
        } catch (const std::exception& e) {
            logWarn("content library {}: {}", libFile.string(), e.what());
            lib_ = json::object();
        }
    } else {
        logWarn("no content library at {} (built-in primitives only)", libFile.string());
    }
}

CollisionDesc SceneBuilder::parseCollision(const json& j, CollisionDesc def) {
    if (j.is_null()) return def;
    std::string kind = j.is_string() ? j.get<std::string>() : j.is_object() ? j.value("type", "auto") : "auto";
    CollisionDesc c = def;
    if (kind == "auto") return def;
    if (kind == "none") c.kind = CollisionKind::None;
    else if (kind == "mesh") c.kind = CollisionKind::Mesh;
    else if (kind == "convex") c.kind = CollisionKind::Convex;
    else if (kind == "cylinder") c.kind = CollisionKind::Cylinder;
    else if (kind == "box") c.kind = CollisionKind::Box;
    else if (kind == "sphere") c.kind = CollisionKind::Sphere;
    else throw Error("collision must be auto, none, mesh, convex, box, cylinder or sphere");
    if (j.is_object()) { c.radius = j.value("radius", c.radius); c.height = j.value("height", c.height); }
    return c;
}

const json* SceneBuilder::meshEntry(const World& w, const std::string& name, fs::path& baseDir) const {
    if (w.doc.contains("meshes") && w.doc["meshes"].contains(name)) { baseDir = w.dir; return &w.doc["meshes"][name]; }
    if (lib_.contains("meshes") && lib_["meshes"].contains(name)) { baseDir = contentDir; return &lib_["meshes"][name]; }
    return nullptr;
}

CollisionDesc SceneBuilder::defaultCollision(const World& w, const json& ref) const {
    CollisionDesc mesh{CollisionKind::Mesh};
    if (ref.is_object()) {
        std::string t = ref.value("type", "");
        if (t == "box") return {CollisionKind::Box};
        if (t == "gem") return {CollisionKind::None};
        return mesh;
    }
    if (!ref.is_string()) return mesh;
    fs::path base;
    if (const json* e = meshEntry(w, ref.get<std::string>(), base)) return parseCollision(e->value("collision", json()), mesh);
    return mesh;
}

std::shared_ptr<const MeshAsset> SceneBuilder::loadFileAsset(const fs::path& file, const json& entry, const std::string& name) {
    std::string key = "file:" + file.string() + "|" + entry.dump();
    std::error_code ec;
    auto mtime = fs::last_write_time(file, ec);
    if (ec) throw Error(std::format("mesh '{}': cannot read {}", name, file.string()));
    if (auto it = assets_.find(key); it != assets_.end() && assetTimes_[key] == mtime) return it->second;
    MeshAsset a = loadGltfAsset(file, name);
    float s = entry.value("import_scale", 1.0f);
    float rotZ = glm::radians(entry.value("import_yaw_deg", 0.0f));
    if (s != 1.0f || rotZ != 0.0f) {
        quat q = glm::angleAxis(rotZ, vec3(0, 0, 1));
        for (auto& v : a.vertices) {
            vec3 p = q * vec3(v.p0) * s, n = q * vec3(v.p1), t = q * vec3(v.tangent);
            v.p0 = vec4(p, v.p0.w); v.p1 = vec4(n, v.p1.w); v.tangent = vec4(t, v.tangent.w);
        }
        a.computeBounds();
    }
    if (entry.value("ground_origin", false)) {   // move the origin to the bottom centre
        vec3 off(-(a.aabbMin.x + a.aabbMax.x) * 0.5f, -(a.aabbMin.y + a.aabbMax.y) * 0.5f, -a.aabbMin.z);
        for (auto& v : a.vertices) { v.p0.x += off.x; v.p0.y += off.y; v.p0.z += off.z; }
        a.computeBounds();
    }
    if (entry.contains("lods")) {
        std::vector<LodSpec> specs;
        for (auto& l : entry["lods"])
            specs.push_back({l.value("ratio", 0.25f), l.value("distance_m", 1e9f), l.value("sloppy", false), l.value("foliage", "") == "drop"});
        // materials shaded as foliage or grass: their pieces are cards / blades
        std::vector<std::string> foliage;
        auto collect = [&](const json& mats) {
            if (!mats.is_object()) return;
            for (auto& [k, v] : mats.items()) {
                std::string model = v.value("model", "lit");
                if (model == "foliage" || model == "grass") foliage.push_back(k);
            }
        };
        collect(builtinMaterials());
        collect(lib_.value("materials", json::object()));
        generateLods(a, entry.value("lod0_distance_m", 1e9f), specs, foliage);
    }
    auto ptr = std::make_shared<const MeshAsset>(std::move(a));
    assets_[key] = ptr;
    assetTimes_[key] = mtime;
    return ptr;
}

std::shared_ptr<const MeshAsset> SceneBuilder::resolveMesh(const World& w, const json& ref) {
    if (ref.is_object()) {
        std::string key = primitiveKey(ref);
        if (auto it = assets_.find(key); it != assets_.end()) return it->second;
        MeshAsset a;
        if (!makePrimitive(ref, a)) throw Error(std::format("unknown primitive type '{}' (see catalog: primitives)", ref.value("type", "")));
        auto ptr = std::make_shared<const MeshAsset>(std::move(a));
        assets_[key] = ptr;
        return ptr;
    }
    if (!ref.is_string()) throw Error("a mesh reference must be a name or a primitive spec object");
    std::string name = ref.get<std::string>();
    fs::path base;
    if (const json* e = meshEntry(w, name, base)) {
        if (e->contains("primitive")) return resolveMesh(w, (*e)["primitive"]);
        if (!e->contains("file")) throw Error(std::format("mesh '{}' has no \"file\"", name));
        return loadFileAsset(base / (*e)["file"].get<std::string>(), *e, name);
    }
    // built-ins: bare primitive names with defaults
    for (const char* p : {"box", "plane", "cylinder", "cone", "sphere", "capsule", "ramp", "stairs", "gem"})
        if (name == p) return resolveMesh(w, json{{"type", name}, {"material", "blockout"}});
    std::vector<std::string> some;
    if (w.doc.contains("meshes")) for (auto& [k, v] : w.doc["meshes"].items()) if (some.size() < 12) some.push_back(k);
    if (lib_.contains("meshes")) for (auto& [k, v] : lib_["meshes"].items()) if (some.size() < 24) some.push_back(k);
    std::string list;
    for (auto& s : some) list += (list.empty() ? "" : ", ") + s;
    throw Error(std::format("unknown mesh '{}'. Use a primitive spec like {{\"type\": \"box\", \"size\": [2,2,2]}} or one of: {}{}", name,
                            list, some.size() >= 24 ? ", ... (see catalog)" : ""));
}

std::shared_ptr<const MeshAsset> SceneBuilder::withMaterials(const std::shared_ptr<const MeshAsset>& a, const json& overrides) {
    if (!overrides.is_object() || overrides.empty()) return a;
    std::string key = std::format("mat:{}|{}", (const void*)a.get(), overrides.dump());
    if (auto it = assets_.find(key); it != assets_.end()) return it->second;
    MeshAsset m = *a;
    m.name = std::format("{}|{:08x}", a->name, (uint32_t)std::hash<std::string>{}(overrides.dump()));
    for (auto& lod : m.lods)
        for (auto& part : lod)
            if (auto it = overrides.find(part.material); it != overrides.end() && it->is_string()) part.material = it->get<std::string>();
    auto ptr = std::make_shared<const MeshAsset>(std::move(m));
    assets_[key] = ptr;
    return ptr;
}

json SceneBuilder::catalog(const World& w) const {
    json meshes = json::object();
    auto add = [&](const json& src, const char* origin) {
        if (!src.contains("meshes")) return;
        for (auto& [k, v] : src["meshes"].items()) {
            json e = {{"source", origin}};
            for (const char* f : {"category", "description", "size_m", "collision", "footprint_m"})
                if (v.contains(f)) e[f] = v[f];
            meshes[k] = e;
        }
    };
    add(lib_, "content library");
    add(w.doc, "map");
    json mats = json::array();
    for (auto& [k, v] : builtinMaterials().items()) mats.push_back(k);
    if (lib_.contains("materials")) for (auto& [k, v] : lib_["materials"].items()) mats.push_back(k);
    if (w.doc.contains("materials")) for (auto& [k, v] : w.doc["materials"].items()) mats.push_back(k);
    return {{"primitives", primitiveCatalog()}, {"meshes", meshes}, {"materials", mats}};
}

void SceneBuilder::build(World& w, Scene& s, std::vector<EntityState>& entities, BuildInfo& info) {
    auto t0 = std::chrono::steady_clock::now();
    s = Scene();
    entities.clear();
    info = BuildInfo();
    const json& doc = w.doc;
    s.name = doc.value("name", "map");
    auto warn = [&](std::string m) {
        if (info.warnings.size() < 50) info.warnings.push_back(m);
        logWarn("{}", m);
    };

    // environment
    try {
        parseEnvironment(doc.value("environment", json::object()), s.env);
    } catch (const Error& e) {
        warn(std::string("environment: ") + e.what());
    }
    w.docToTerrain();

    // materials: built-ins < content library < map
    auto texResolver = [&](const fs::path& base) {
        return [&, base](const std::string& rel, bool srgb) -> uint32_t {
            fs::path p = base / rel;
            std::string key = p.string() + (srgb ? "#s" : "#l");
            auto it = textureCache_.find(key);
            if (it == textureCache_.end()) {
                try {
                    it = textureCache_.emplace(key, loadTextureFile(p, srgb)).first;
                } catch (const Error& e) {
                    warn(e.what());
                    return kNoTexture;
                }
            }
            return s.addTexture(it->second);
        };
    };
    for (auto& [k, v] : builtinMaterials().items()) s.addMaterial(parseMaterial(k, v, nullptr));
    if (lib_.contains("materials"))
        for (auto& [k, v] : lib_["materials"].items()) {
            try { s.addMaterial(parseMaterial(k, v, texResolver(contentDir))); } catch (const Error& e) { warn(std::format("material '{}': {}", k, e.what())); }
        }
    if (doc.contains("materials") && doc["materials"].is_object())
        for (auto& [k, v] : doc["materials"].items()) {
            try { s.addMaterial(parseMaterial(k, v, texResolver(w.dir))); } catch (const Error& e) { warn(std::format("material '{}': {}", k, e.what())); }
        }
    if (auto it = s.materialByName.find("terrain"); it != s.materialByName.end()) s.materials[it->second].gpu.p[0].x = s.env.waterLevel;

    auto meshCull = [&](const json& ref, float def) {
        if (!ref.is_string()) return def;
        fs::path base;
        const json* e = meshEntry(w, ref.get<std::string>(), base);
        return e ? e->value("cull_distance_m", def) : def;
    };
    std::unordered_map<const MeshAsset*, uint32_t> meshIds;
    auto sceneMesh = [&](const std::shared_ptr<const MeshAsset>& a) {
        if (auto it = meshIds.find(a.get()); it != meshIds.end()) return it->second;
        uint32_t id = s.addMeshAsset(*a, a->name);
        meshIds[a.get()] = id;
        return id;
    };
    std::vector<std::shared_ptr<const MeshAsset>> keepAlive;
    auto groundZ = [&](vec2 p) { return w.terrain.empty() ? 0.0f : w.terrain.heightAt(p.x, p.y); };

    // terrain: rendered from heightmap textures (Renderer::setTerrain); paths are applied here
    Terrain& T = w.terrain;
    if (!T.empty()) {
        std::string pathsKey = doc.value("paths", json::array()).dump();
        if (chunkTerrainVersion_ != T.version || chunkPathsKey_ != pathsKey) {
            T.applyPaths(doc.value("paths", json::array()));
            chunkTerrainVersion_ = T.version;
            chunkPathsKey_ = pathsKey;
            terrainChanged_ = true;
        }
        std::string tmat = doc.value("terrain", json::object()).value("material", "terrain");
        s.terrainMaterial = s.findOrAddMaterial(tmat);
        // distant land
        const json& hz = doc.value("terrain", json::object()).value("horizon", json::object());
        if (hz.value("enabled", true)) {
            std::string key = hz.dump() + std::format("|{}|", T.version) + chunkPathsKey_;
            if (horizonKey_ != key || !horizon_) {
                horizon_ = std::make_shared<MeshAsset>(T.horizonMesh(hz));
                horizonKey_ = key;
            }
            for (auto& lod : horizon_->lods) for (auto& p : lod) p.material = tmat;
            uint32_t m = s.addMeshAsset(*horizon_, horizon_->name);
            s.addInstance(m, vec3(0), quat(1, 0, 0, 0), 1.0f, true, 1e9f);
        }
        float lo = 1e30f, hi = -1e30f;
        for (float h : T.height) { lo = std::min(lo, h); hi = std::max(hi, h); }
        s.terrainMin = vec3(T.origin, lo);
        s.terrainMax = vec3(T.maxCorner(), hi);
        s.hasTerrain = true;
    } else if (chunkTerrainVersion_ != 0) {
        chunkTerrainVersion_ = 0;
        terrainChanged_ = true;
    }

    // water plane
    const json& env = doc.value("environment", json::object());
    if (env.contains("water") && env["water"].value("enabled", false) && env["water"].value("plane", true)) {
        const json& wt = env["water"];
        float size = wt.value("size_m", T.empty() ? 2000.0f : T.size() + 400.0f);
        vec2 c = T.empty() ? vec2(0) : T.origin + vec2(T.size() * 0.5f);
        auto a = resolveMesh(w, json{{"type", "plane"}, {"size", {size, size}}, {"subdivisions", 16}, {"material", wt.value("material", "water")}});
        keepAlive.push_back(a);
        uint32_t m = sceneMesh(a);
        uint32_t first = s.addInstance(m, vec3(c, s.env.waterLevel), quat(1, 0, 0, 0), 1.0f, false, 1e9f);
        s.sets.push_back({"water", m, first, 1, {CollisionKind::None}});
    }

    // objects
    std::vector<Footprint> footprints;
    for (const json& o : doc.value("objects", json::array())) {
        std::string id = o.value("id", "?");
        if (o.value("hidden", false)) continue;
        try {
            if (!o.contains("mesh")) throw Error("no \"mesh\"");
            auto a = withMaterials(resolveMesh(w, o["mesh"]), o.value("materials", json()));
            keepAlive.push_back(a);
            uint32_t m = sceneMesh(a);
            const json& p = o.value("position", json::array({0, 0}));
            if (!p.is_array() || p.size() < 2) throw Error("position must be [x, y] or [x, y, z]");
            vec2 xy(p[0].get<float>(), p[1].get<float>());
            bool onGround = o.value("on_ground", p.size() < 3);
            float z = onGround ? groundZ(xy) + o.value("offset_z", 0.0f) : p[2].get<float>();
            quat q;
            if (o.contains("rotation")) {
                const json& r = o["rotation"];
                q = glm::normalize(quat(r[3].get<float>(), r[0].get<float>(), r[1].get<float>(), r[2].get<float>()));
            } else {
                q = glm::angleAxis(glm::radians(o.value("yaw_deg", 0.0f)), vec3(0, 0, 1)) *
                    glm::angleAxis(glm::radians(o.value("pitch_deg", 0.0f)), vec3(1, 0, 0)) *
                    glm::angleAxis(glm::radians(o.value("roll_deg", 0.0f)), vec3(0, 1, 0));
            }
            if (o.value("align_to_ground", false) && !T.empty())
                q = glm::rotation(vec3(0, 0, 1), T.normalAt(xy.x, xy.y)) * q;
            vec3 scale = parseScale(o.value("scale", json(1.0f)));
            float cull = o.value("cull_distance_m", meshCull(o["mesh"], autoCull(*a, maxAxis(scale))));
            uint32_t first = s.addInstance(m, vec3(xy, z), q, scale, o.value("shadow", true), cull);
            InstanceSet set{id, m, first, 1, parseCollision(o.value("collision", json()), defaultCollision(w, o["mesh"]))};
            s.sets.push_back(set);
            // keep-out circle for scatter: footprint_m, or automatic for objects under 50 m (merged
            // structures like a whole castle or a meadow's ruins must not clear the map)
            vec2 ext = glm::max(glm::abs(vec2(a->aabbMin) * vec2(scale)), glm::abs(vec2(a->aabbMax) * vec2(scale)));
            const json& fp = o.value("footprint_m", json());
            if (fp.is_number()) { if (fp.get<float>() > 0) footprints.push_back({xy, fp.get<float>()}); }
            else if (!fp.is_boolean() && glm::length(ext) < 50.0f) footprints.push_back({xy, glm::length(ext) * 0.85f});
            info.triangles += a->triangleCount();
        } catch (const Error& e) {
            warn(std::format("object '{}': {}", id, e.what()));
        }
    }
    for (const json& e : doc.value("entities", json::array()))
        if (e.contains("position") && e["position"].is_array() && e["position"].size() >= 2)
            footprints.push_back({vec2(e["position"][0].get<float>(), e["position"][1].get<float>()), e.value("radius_m", 1.5f)});

    // scatter rules
    ScatterContext ctx{T.empty() ? nullptr : &T, s.env.waterLevel, footprints};
    std::string fpKey;
    for (auto& f : footprints) fpKey += std::format("{:.1f},{:.1f},{:.1f};", f.center.x, f.center.y, f.radius);
    for (const json& raw : doc.value("scatter", json::array())) {
        std::string id = raw.value("id", "?");
        if (raw.value("hidden", false)) continue;
        try {
            json rule = normalizeScatterRule(raw);
            std::string key = rule.dump() + std::format("|t{}|", T.version) + chunkPathsKey_ + "|" + fpKey;
            auto& cache = scatter_[id];
            if (cache.key != key) {
                cache.points = evaluateScatter(rule, ctx, rule.value("max_instances", 2000000u));
                cache.key = key;
            }
            const json& variants = rule["meshes"];
            std::vector<std::shared_ptr<const MeshAsset>> assets;
            std::vector<uint32_t> ids;
            for (auto& v : variants) {
                auto a = resolveMesh(w, v["mesh"]);
                keepAlive.push_back(a);
                assets.push_back(a);
                ids.push_back(sceneMesh(a));
            }
            CollisionDesc col = parseCollision(rule.value("collision", json("none")), {CollisionKind::None});
            bool shadow = rule.value("shadow", true);
            // one instance set per variant so collision and picking stay per mesh
            for (uint32_t vi = 0; vi < ids.size(); ++vi) {
                InstanceSet set{id, ids[vi], (uint32_t)s.instances.size(), 0, col};
                float cull = rule.value("cull_distance_m", meshCull(variants[vi]["mesh"], autoCull(*assets[vi], 1.0f)));
                for (const ScatterPoint& p : cache.points) {
                    if (p.variant != vi) continue;
                    s.addInstance(ids[vi], p.position, p.rotation, p.scale, shadow, cull);
                    ++set.count;
                }
                if (set.count) s.sets.push_back(set);
            }
            if (cache.points.empty()) warn(std::format("scatter '{}' placed nothing (check area, slope_deg, height_m, density)", id));
        } catch (const Error& e) {
            warn(std::format("scatter '{}': {}", id, e.what()));
        }
    }

    // binary instance files (ported maps), little-endian float32:
    //   layout "pos_scale_quat" (default): 32 bytes per instance: position xyz, scale, quaternion xyzw
    //   layout "pos_quat_scale3":          40 bytes per instance: position xyz, quaternion xyzw, scale xyz
    for (const json& f : doc.value("instance_files", json::array())) {
        std::string id = f.value("id", f.value("file", "?"));
        try {
            auto a = withMaterials(resolveMesh(w, f.at("mesh")), f.value("materials", json()));
            keepAlive.push_back(a);
            uint32_t m = sceneMesh(a);
            std::string layout = f.value("layout", "pos_scale_quat");
            if (layout != "pos_scale_quat" && layout != "pos_quat_scale3") throw Error("layout must be pos_scale_quat or pos_quat_scale3");
            bool axis = layout == "pos_quat_scale3";
            size_t stride = axis ? 10 : 8;
            auto raw = readBinary(w.dir / f["file"].get<std::string>());
            size_t n = raw.size() / (stride * 4);
            std::vector<float> fp(n * stride);
            std::memcpy(fp.data(), raw.data(), n * stride * 4);
            InstanceSet set{id, m, (uint32_t)s.instances.size(), (uint32_t)n,
                            parseCollision(f.value("collision", json("none")), {CollisionKind::None})};
            bool shadow = f.value("shadow", true);
            float cull = f.value("cull_distance_m", autoCull(*a, 1.0f));
            for (size_t i = 0; i < n; ++i) {
                const float* r = &fp[i * stride];
                if (axis) s.addInstance(m, vec3(r[0], r[1], r[2]), quat(r[6], r[3], r[4], r[5]), vec3(r[7], r[8], r[9]), shadow, cull);
                else s.addInstance(m, vec3(r[0], r[1], r[2]), quat(r[7], r[4], r[5], r[6]), r[3], shadow, cull);
            }
            s.sets.push_back(set);
        } catch (const std::exception& e) {
            warn(std::format("instance file '{}': {}", id, e.what()));
        }
    }

    // whole glTF scenes (levels exported from Unreal or Blender)
    for (const json& g : doc.value("gltf_scenes", json::array())) {
        std::string file = g.value("file", "");
        try {
            GltfScene gs = loadGltfScene(w.dir / file);
            std::vector<uint32_t> ids;
            for (auto& m : gs.meshes) {
                auto a = std::make_shared<const MeshAsset>(std::move(m));
                keepAlive.push_back(a);
                ids.push_back(a->vertices.empty() ? UINT32_MAX : sceneMesh(a));
            }
            CollisionDesc col = parseCollision(g.value("collision", json("mesh")), {CollisionKind::Mesh});
            for (auto& p : gs.placements) {
                if (ids[p.mesh] == UINT32_MAX) continue;
                uint32_t first = s.addInstance(ids[p.mesh], p.position, p.rotation, p.scale, g.value("shadow", true), g.value("cull_distance_m", 1e9f));
                s.sets.push_back({g.value("id", file), ids[p.mesh], first, 1, col});
            }
            for (auto& l : gs.lights) s.lights.push_back(l);
        } catch (const std::exception& e) {
            warn(std::format("gltf scene '{}': {}", file, e.what()));
        }
    }

    // static lights
    for (const json& l : doc.value("lights", json::array())) {
        try {
            LightDef d;
            d.id = l.value("id", "");
            const json& p = l.at("position");
            vec2 xy(p[0].get<float>(), p[1].get<float>());
            d.position = vec3(xy, p.size() >= 3 && !l.value("on_ground", false) ? p[2].get<float>() : groundZ(xy) + l.value("offset_z", 2.0f));
            d.color = v3(l.value("color", json()), vec3(1.0f, 0.8f, 0.6f));
            d.intensity = l.value("intensity", 10.0f);
            d.range = l.value("range_m", 12.0f);
            s.lights.push_back(d);
        } catch (const std::exception& e) {
            warn(std::format("light '{}': {}", l.value("id", "?"), e.what()));
        }
    }

    // ---- dynamic instances from here on: entities and the player
    s.dynamicFirst = (uint32_t)s.instances.size();
    for (const json& e : doc.value("entities", json::array())) {
        EntityState es;
        es.id = e.value("id", "?");
        es.type = e.value("type", "");
        es.props = e;
        try {
            const json& p = e.at("position");
            vec2 xy(p[0].get<float>(), p[1].get<float>());
            es.color = v3(e.value("color", json()), es.type == "goal" ? vec3(0.4f, 0.75f, 1.0f) : vec3(1.0f, 0.72f, 0.28f));
            es.message = e.value("message", "");
            if (es.type == "collectible") {
                es.position = vec3(xy, p.size() >= 3 ? p[2].get<float>() : groundZ(xy) + e.value("hover_m", 1.1f));
                es.radius = e.value("radius_m", 1.3f);
                std::string mat = "glow_" + hexColor(es.color);
                if (!s.materialByName.count(mat)) {
                    json def = {{"model", "emissive"}, {"base_color", {es.color.r, es.color.g, es.color.b}},
                                {"emissive", {es.color.r, es.color.g, es.color.b}}, {"emissive_strength", e.value("glow", 25.0f)}};
                    s.addMaterial(parseMaterial(mat, def, nullptr));
                }
                auto a = resolveMesh(w, json{{"type", "gem"}, {"size", e.value("size_m", 0.6f)}, {"material", mat}});
                keepAlive.push_back(a);
                es.instance = s.addInstance(sceneMesh(a), es.position, quat(1, 0, 0, 0), 1.0f, false, 400.0f);
                if (e.value("light", true)) {
                    LightDef l;
                    l.id = es.id;
                    l.position = es.position;
                    l.color = es.color;
                    l.intensity = e.value("light_intensity", 6.0f);
                    l.range = e.value("light_range_m", 7.0f);
                    es.light = (int)s.lights.size();
                    s.lights.push_back(l);
                }
            } else if (es.type == "goal" || es.type == "trigger" || es.type == "spawn" || es.type == "waypoint") {
                es.position = vec3(xy, p.size() >= 3 ? p[2].get<float>() : groundZ(xy));
                es.radius = e.value("radius_m", es.type == "goal" ? 3.0f : 2.0f);
                if (es.type == "goal") {
                    std::string mat = "beacon_" + hexColor(es.color);
                    if (!s.materialByName.count(mat)) {
                        json def = {{"model", "emissive"}, {"base_color", {es.color.r, es.color.g, es.color.b}},
                                    {"emissive", {es.color.r, es.color.g, es.color.b}}, {"emissive_strength", 8.0f}};
                        s.addMaterial(parseMaterial(mat, def, nullptr));
                    }
                    auto a = resolveMesh(w, json{{"type", "cylinder"}, {"radius", es.radius}, {"height", 0.06f}, {"segments", 48}, {"material", mat}});
                    keepAlive.push_back(a);
                    es.instance = s.addInstance(sceneMesh(a), es.position + vec3(0, 0, 0.02f), quat(1, 0, 0, 0), 1.0f, false, 600.0f);
                }
            } else {
                throw Error("unknown entity type '" + es.type + "' (collectible, goal, trigger, spawn, waypoint)");
            }
            entities.push_back(es);
        } catch (const std::exception& ex) {
            warn(std::format("entity '{}': {}", es.id, ex.what()));
        }
    }
    // the default mannequin (hidden until play)
    playerFirst = (uint32_t)s.instances.size();
    for (const MannequinPart& part : mannequinParts()) {
        uint32_t m = s.addMeshAsset(part.mesh, part.mesh.name);
        uint32_t i = s.addInstance(m, vec3(0), quat(1, 0, 0, 0), 1.0f, true, -1.0f);   // negative cull distance: hidden
        (void)i;
    }
    playerCount = (uint32_t)mannequinParts().size();

    // cameras and player start
    if (doc.contains("cameras") && doc["cameras"].is_object())
        for (auto& [name, c] : doc["cameras"].items()) {
            CameraDef d;
            d.position = v3(c.value("position", json()), vec3(0, -10, 5));
            d.target = v3(c.value("target", json()), vec3(0));
            d.vfovDeg = c.value("vfov_deg", 50.0f);
            s.cameras[name] = d;
        }
    if (doc.contains("player_start")) {
        const json& ps = doc["player_start"];
        const json& p = ps.value("position", json::array({0, 0}));
        vec2 xy(p[0].get<float>(), p[1].get<float>());
        s.playerStart.position = vec3(xy, p.size() >= 3 ? p[2].get<float>() : groundZ(xy));
        s.playerStart.yawDeg = ps.value("yaw_deg", 90.0f);
    }

    s.computeBounds();
    if (s.hasTerrain) {
        s.boundsMin = glm::min(s.boundsMin, s.terrainMin);
        s.boundsMax = glm::max(s.boundsMax, s.terrainMax);
    }
    info.instances = s.instances.size();
    info.meshes = s.meshes.size();
    info.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace df
