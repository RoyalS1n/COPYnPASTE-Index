#include "world/SceneBuilder.h"
#include "core/Error.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "game/Mannequin.h"
#include "scene/GltfLoader.h"
#include "scene/Materials.h"
#include "scene/Primitives.h"
#include "scene/TreeGen.h"
#include "world/Area.h"
#include "world/EnvironmentDoc.h"
#include <algorithm>
#include <bit>
#include <unordered_map>
#include <set>
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
// "extends": start from another material (the map's, the content library's or a built-in) and replace the keys given
// here; "triplanar", "wind" and "textures" merge key by key. A map material's inherited library textures become
// absolute paths so they still resolve from the map's folder.
json extendMaterial(const std::string& name, const json& v, const json* mapMats, const json& libMats,
                    const fs::path& contentDir, int depth = 0) {
    if (!v.is_object() || !v.contains("extends")) return v;
    if (!v["extends"].is_string()) throw Error("extends must be a material name");
    if (depth > 8) throw Error("extends: chain too long (a loop?)");
    std::string base = v["extends"].get<std::string>();
    json b;
    if (mapMats && base != name && mapMats->contains(base)) {
        b = extendMaterial(base, (*mapMats)[base], mapMats, libMats, contentDir, depth + 1);
    } else if (libMats.contains(base) && !(mapMats == nullptr && base == name)) {
        b = extendMaterial(base, libMats[base], nullptr, libMats, contentDir, depth + 1);
        if (mapMats && b.contains("textures") && b["textures"].is_object())
            for (auto& [tk, tv] : b["textures"].items())
                if (tv.is_string()) tv = (contentDir / tv.get<std::string>()).generic_string();
    } else if (builtinMaterials().contains(base)) {
        b = builtinMaterials()[base];
    } else {
        throw Error("extends: no material '" + base + "'");
    }
    for (auto& [k, x] : v.items()) {
        if (k == "extends") continue;
        bool merge = (k == "triplanar" || k == "wind" || k == "textures") && x.is_object() && b.contains(k) && b[k].is_object();
        if (merge) b[k].update(x);
        else b[k] = x;
    }
    return b;
}
float maxAxis(vec3 s) { return std::max(std::abs(s.x), std::max(std::abs(s.y), std::abs(s.z))); }
float autoCull(const MeshAsset& a, float scale) {
    float r = a.bounds.w * scale;
    return std::clamp(r * 160.0f, 60.0f, 4000.0f);
}
}  // namespace

Placement objectPlacement(const World& w, const json& o) {
    Placement pl;
    const json& p = o.value("position", json::array({0, 0}));
    if (!p.is_array() || p.size() < 2) throw Error("position must be [x, y] or [x, y, z]");
    vec2 xy(p[0].get<float>(), p[1].get<float>());
    bool onGround = o.value("on_ground", p.size() < 3);
    float ground = w.terrain.empty() ? 0.0f : w.terrain.heightAt(xy.x, xy.y);
    pl.position = vec3(xy, onGround ? ground + o.value("offset_z", 0.0f) : p[2].get<float>());
    if (o.contains("rotation")) {
        const json& r = o["rotation"];
        pl.rotation = glm::normalize(quat(r[3].get<float>(), r[0].get<float>(), r[1].get<float>(), r[2].get<float>()));
    } else {
        pl.rotation = glm::angleAxis(glm::radians(o.value("yaw_deg", 0.0f)), vec3(0, 0, 1)) *
                      glm::angleAxis(glm::radians(o.value("pitch_deg", 0.0f)), vec3(1, 0, 0)) *
                      glm::angleAxis(glm::radians(o.value("roll_deg", 0.0f)), vec3(0, 1, 0));
    }
    if (o.value("align_to_ground", false) && !w.terrain.empty())
        pl.rotation = glm::rotation(vec3(0, 0, 1), w.terrain.normalAt(xy.x, xy.y)) * pl.rotation;
    pl.scale = parseScale(o.value("scale", json(1.0f)));
    return pl;
}

void placedBounds(const MeshAsset& a, const Placement& p, vec3& lo, vec3& hi) {
    lo = vec3(1e30f);
    hi = vec3(-1e30f);
    for (int k = 0; k < 8; ++k) {
        vec3 c((k & 1) ? a.aabbMax.x : a.aabbMin.x, (k & 2) ? a.aabbMax.y : a.aabbMin.y, (k & 4) ? a.aabbMax.z : a.aabbMin.z);
        vec3 wpos = p.position + p.rotation * (c * p.scale);
        lo = glm::min(lo, wpos);
        hi = glm::max(hi, wpos);
    }
}

json BuildInfo::toJson() const {
    json w = json::array();
    for (auto& s : warnings) w.push_back(s);
    json j = {{"build_ms", std::round(ms * 10) / 10}, {"instances", instances}, {"meshes", meshes}, {"triangles_lod0", triangles}, {"warnings", w}};
    if (!skyOcclusion.is_null()) j["sky_occlusion"] = skyOcclusion;
    return j;
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
    // sub-libraries: content/<folder>/library.json, each written by its own tool (fortress kit, texture set);
    // their paths are relative to their folder. Names already defined keep their first definition.
    std::vector<fs::path> subs;
    if (fs::is_directory(dir))
        for (auto& e : fs::directory_iterator(dir))
            if (e.is_directory() && fs::exists(e.path() / "library.json")) subs.push_back(e.path());
    std::sort(subs.begin(), subs.end());
    for (auto& sub : subs) {
        std::string prefix = sub.filename().generic_string() + "/";
        try {
            json part = json::parse(readText(sub / "library.json"));
            size_t added = 0;
            for (const char* kind : {"meshes", "materials", "prefabs"}) {
                if (!part.contains(kind)) continue;
                json& into = lib_[kind];
                if (!into.is_object()) into = json::object();
                for (auto& [k, v] : part[kind].items()) {
                    if (into.contains(k)) { logWarn("content library {}: '{}' is already defined, skipped", prefix, k); continue; }
                    json e = v;
                    if (e.contains("file") && e["file"].is_string()) e["file"] = prefix + e["file"].get<std::string>();
                    if (e.contains("textures") && e["textures"].is_object())
                        for (auto& [tk, tv] : e["textures"].items())
                            if (tv.is_string()) tv = prefix + tv.get<std::string>();
                    into[k] = std::move(e);
                    ++added;
                }
            }
            logInfo("content library {}: {} entries", prefix, added);
        } catch (const std::exception& e) {
            logWarn("content library {}: {}", (sub / "library.json").string(), e.what());
        }
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

const json* SceneBuilder::prefab(const World& w, const std::string& name) const {
    if (w.doc.contains("prefabs") && w.doc["prefabs"].is_object() && w.doc["prefabs"].contains(name)) return &w.doc["prefabs"][name];
    if (lib_.contains("prefabs") && lib_["prefabs"].is_object() && lib_["prefabs"].contains(name)) return &lib_["prefabs"][name];
    return nullptr;
}

CollisionDesc SceneBuilder::defaultCollision(const World& w, const json& ref) const {
    CollisionDesc mesh{CollisionKind::Mesh};
    if (ref.is_object()) {
        std::string t = ref.value("type", "");
        if (t == "box") return {CollisionKind::Box};
        if (t == "gem") return {CollisionKind::None};
        if (t == "tree") {   // the trunk, up to the crown; bushes are walked through
            TreeShape s = treeShape(ref);
            if (ref.value("species", "oak") == "bush") return {CollisionKind::None};
            return {CollisionKind::Cylinder, s.trunkRadius * 1.1f, std::max(s.crownBase, 1.8f)};
        }
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
        a.computeBounds();   // thinned foliage LODs grow their cards a little past LOD0
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

std::shared_ptr<const CharacterAsset> SceneBuilder::resolveCharacter(const World& w, const json& player) {
    std::string name = player.value("character", "mannequin");
    fs::path base;
    const json* e = meshEntry(w, name, base);
    if (!e || !e->contains("file"))
        throw Error(std::format("player.character '{}' is not a mesh with a file: use \"mannequin\" or a rigged mesh (catalog category "
                                "characters; asset_import a GLB first)", name));
    std::vector<fs::path> files{base / (*e)["file"].get<std::string>()};
    for (auto& f : e->value("animation_files", json::array())) files.push_back(base / f.get<std::string>());
    for (auto& f : player.value("animation_files", json::array())) files.push_back(w.dir / f.get<std::string>());
    std::string key = e->dump();
    for (auto& f : files) {
        std::error_code ec;
        auto t = fs::last_write_time(f, ec);
        if (ec) throw Error(std::format("player.character '{}': cannot read {}", name, f.string()));
        key += std::format("|{}@{}", f.string(), (long long)t.time_since_epoch().count());
    }
    if (characterCache_ && characterKey_ == key) return characterCache_;
    // the model's front (mesh "front"; glTF characters face -y after conversion) turns to +Y, the engine's facing
    std::string front = e->value("front", "-y");
    float frontYaw = front == "+y" || front == "y" ? 0.0f : front == "+x" || front == "x" ? 90.0f : front == "-x" ? -90.0f : 180.0f;
    CharacterImport imp{e->value("import_scale", 1.0f), frontYaw + e->value("import_yaw_deg", 0.0f), e->value("ground_origin", false)};
    auto c = std::make_shared<CharacterAsset>(loadGltfCharacter(files[0], name, imp));
    for (size_t i = 1; i < files.size(); ++i) loadGltfClips(files[i], *c);
    characterKey_ = key;
    characterCache_ = c;
    return c;
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
    auto add = [&](const json& src, const char* origin, const fs::path& dir) {
        if (!src.contains("meshes")) return;
        for (auto& [k, v] : src["meshes"].items()) {
            json e = {{"source", origin}};
            for (const char* f : {"category", "description", "size_m", "collision", "footprint_m"})
                if (v.contains(f)) e[f] = v[f];
            if (v.value("category", "") == "characters" && v.contains("file")) {   // rigged: the clips player_set maps
                json clips = json::array(), files = v.value("animation_files", json::array());
                size_t joints = 0;
                files.insert(files.begin(), v["file"]);
                for (auto& f : files) {
                    try {
                        GltfRigInfo r = inspectGltfRig(dir / f.get<std::string>());
                        joints += r.joints;
                        for (auto& [n, sec] : r.clips) clips.push_back({{"name", n}, {"seconds", std::round(sec * 100.0) / 100.0}});
                    } catch (const std::exception&) {}
                }
                e["rigged"] = joints > 0;
                e["clips"] = clips;
            }
            meshes[k] = e;
        }
    };
    add(lib_, "content library", contentDir);
    add(w.doc, "map", w.dir);
    json mats = json::array();
    for (auto& [k, v] : builtinMaterials().items()) mats.push_back(k);
    if (lib_.contains("materials")) for (auto& [k, v] : lib_["materials"].items()) mats.push_back(k);
    if (w.doc.contains("materials")) for (auto& [k, v] : w.doc["materials"].items()) mats.push_back(k);
    // prefabs: groups of objects saved with prefab_save (map) or shipped in a content library
    json prefabs = json::object();
    auto addPrefabs = [&](const json& src, const char* origin) {
        if (!src.contains("prefabs") || !src["prefabs"].is_object()) return;
        for (auto& [k, v] : src["prefabs"].items()) {
            json e = {{"source", origin}, {"items", v.value("objects", json::array()).size() + v.value("entities", json::array()).size() +
                                                    v.value("lights", json::array()).size()}};
            for (const char* f : {"description", "size_m"}) if (v.contains(f)) e[f] = v[f];
            prefabs[k] = e;
        }
    };
    addPrefabs(lib_, "content library");
    addPrefabs(w.doc, "map");
    return {{"primitives", primitiveCatalog()}, {"meshes", meshes}, {"materials", mats}, {"prefabs", prefabs}};
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
    // library materials with textures load on first use (the library holds far more textures than a map uses)
    static const json noMaterials = json::object();
    const json& libMats = lib_.contains("materials") && lib_["materials"].is_object() ? lib_["materials"] : noMaterials;
    auto addLibMaterial = [&, res = texResolver(contentDir)](const std::string& k, const json& v) {
        try {
            s.addMaterial(parseMaterial(k, extendMaterial(k, v, nullptr, libMats, contentDir), res));
        } catch (const Error& e) {
            warn(std::format("material '{}': {}", k, e.what()));
        }
    };
    for (auto& [k, v] : libMats.items()) {
        bool textured = v.contains("textures");
        try { textured = extendMaterial(k, v, nullptr, libMats, contentDir).contains("textures"); } catch (const Error&) {}
        if (!textured || s.materialByName.count(k)) addLibMaterial(k, v);
    }
    struct SourceGuard { Scene& s; ~SourceGuard() { s.materialSource = nullptr; } } sourceGuard{s};
    s.materialSource = [&](Scene&, const std::string& k) {
        if (doc.contains("materials") && doc["materials"].contains(k)) return;   // the map's own definition follows
        if (lib_.contains("materials") && lib_["materials"].contains(k)) addLibMaterial(k, lib_["materials"][k]);
    };
    if (doc.contains("materials") && doc["materials"].is_object())
        for (auto& [k, v] : doc["materials"].items()) {
            try {
                s.addMaterial(parseMaterial(k, extendMaterial(k, v, &doc["materials"], libMats, contentDir), texResolver(w.dir)));
            } catch (const Error& e) {
                warn(std::format("material '{}': {}", k, e.what()));
            }
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
        // paths and water bodies carve the final heights; both are in the key every terrain-derived cache uses
        json waterDoc = Area::expandArgs(doc.value("water", json::array()), doc.value("areas", json::object()));
        std::string pathsKey = doc.value("paths", json::array()).dump() + "|water|" + waterDoc.dump();
        if (chunkTerrainVersion_ != T.version || chunkPathsKey_ != pathsKey) {
            T.applyPaths(doc.value("paths", json::array()));
            waterWarnings_.clear();
            waterBodies_ = T.applyWater(waterDoc, waterWarnings_);
            waterMeshes_.clear();
            for (auto& b : waterBodies_) waterMeshes_.push_back(std::make_shared<MeshAsset>(T.waterMesh(b)));
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

    // lakes and rivers (meshes made with the carving above)
    for (auto& wmsg : waterWarnings_) warn(wmsg);
    for (size_t k = 0; k < waterBodies_.size() && k < waterMeshes_.size(); ++k) {
        if (waterMeshes_[k]->vertices.empty()) continue;
        uint32_t m = sceneMesh(waterMeshes_[k]);
        uint32_t first = s.addInstance(m, vec3(0), quat(1, 0, 0, 0), 1.0f, false, 1e9f);
        s.sets.push_back({waterBodies_[k].id, m, first, 1, {CollisionKind::None}});
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
            Placement pl = objectPlacement(w, o);
            vec2 xy(pl.position);
            vec3 scale = pl.scale;
            float cull = o.value("cull_distance_m", meshCull(o["mesh"], autoCull(*a, maxAxis(scale))));
            uint32_t first = s.addInstance(m, pl.position, pl.rotation, scale, o.value("shadow", true), cull);
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
    // evaluated without footprints and cached, then filtered: moving an object does not re-run the rules
    ScatterContext ctx{T.empty() ? nullptr : &T, s.env.waterLevel, {}};
    FootprintIndex fpIndex(footprints);
    // rules that later rules keep clear of (avoid_rules): their instances become keep-out circles of footprint_m
    std::set<std::string> avoided;
    for (const json& raw : doc.value("scatter", json::array()))
        for (auto& v : raw.value("avoid_rules", json::array())) if (v.is_string()) avoided.insert(v.get<std::string>());
    std::unordered_map<std::string, std::vector<Footprint>> ruleFootprints;
    for (const json& raw : doc.value("scatter", json::array())) {
        std::string id = raw.value("id", "?");
        if (raw.value("hidden", false)) continue;
        try {
            json rule = normalizeScatterRule(Area::expandArgs(raw, doc.value("areas", json::object())));
            std::string key = rule.dump() + std::format("|t{}|", T.version) + chunkPathsKey_;
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
            float avoid = rule.value("avoid_objects_m", 1.0f);
            // keep clear of the instances of earlier rules named in avoid_rules
            std::vector<Footprint> others;
            for (auto& v : rule.value("avoid_rules", json::array())) {
                auto it = ruleFootprints.find(v.get<std::string>());
                if (it == ruleFootprints.end()) warn(std::format("scatter '{}': avoid_rules names '{}', which is not an earlier scatter rule", id, v.get<std::string>()));
                else others.insert(others.end(), it->second.begin(), it->second.end());
            }
            FootprintIndex otherIndex(others, 8.0f);
            float avoidRules = rule.value("avoid_rules_m", 0.3f);
            std::vector<char> kept(cache.points.size(), 0);
            for (size_t k = 0; k < cache.points.size(); ++k) {
                vec2 q(cache.points[k].position);
                kept[k] = !fpIndex.blocked(q, avoid) && !otherIndex.blocked(q, avoidRules);
            }
            // one instance set per variant so collision and picking stay per mesh
            for (uint32_t vi = 0; vi < ids.size(); ++vi) {
                InstanceSet set{id, ids[vi], (uint32_t)s.instances.size(), 0, col};
                float cull = rule.value("cull_distance_m", meshCull(variants[vi]["mesh"], autoCull(*assets[vi], 1.0f)));
                for (size_t k = 0; k < cache.points.size(); ++k) {
                    const ScatterPoint& p = cache.points[k];
                    if (p.variant != vi || !kept[k]) continue;
                    s.addInstance(ids[vi], p.position, p.rotation, p.scale, shadow, cull);
                    ++set.count;
                }
                if (set.count) s.sets.push_back(set);
            }
            if (avoided.count(id)) {
                float fp = rule.value("footprint_m", 0.5f);
                auto& out = ruleFootprints[id];
                for (size_t k = 0; k < cache.points.size(); ++k)
                    if (kept[k]) out.push_back({vec2(cache.points[k].position), fp * cache.points[k].scale});
            }
            // companions: smaller things around each instance (stones and ferns at tree feet), placed from the
            // instance's position alone so they stay put when other rules change
            for (const json& c : rule.value("companions", json::array())) {
                const json& cv = c["meshes"];
                std::vector<uint32_t> cids;
                std::vector<float> cdf;
                float wsum = 0;
                for (auto& v : cv) {
                    auto a = resolveMesh(w, v["mesh"]);
                    keepAlive.push_back(a);
                    cids.push_back(sceneMesh(a));
                    wsum += std::max(v.value("weight", 1.0f), 0.0f);
                    cdf.push_back(wsum);
                }
                vec2 count = vec2(c["count"][0].get<float>(), c["count"][1].get<float>());
                vec2 dist = vec2(c["distance_m"][0].get<float>(), c["distance_m"][1].get<float>());
                vec2 sc = vec2(c["scale"][0].get<float>(), c["scale"][1].get<float>());
                float chance = c.value("chance", 1.0f), sink = c.value("sink_m", 0.05f);
                std::vector<std::vector<std::pair<vec3, std::pair<quat, float>>>> byVariant(cids.size());
                uint64_t salt = std::hash<std::string>{}(c.dump()) ^ (uint64_t)rule.value("seed", 1);
                for (size_t k = 0; k < cache.points.size(); ++k) {
                    if (!kept[k]) continue;
                    const ScatterPoint& p = cache.points[k];
                    uint64_t hsh = salt ^ ((uint64_t)std::bit_cast<uint32_t>(p.position.x) * 0x9E3779B97F4A7C15ull) ^
                                   ((uint64_t)std::bit_cast<uint32_t>(p.position.y) * 0xC2B2AE3D27D4EB4Full);
                    auto rnd01 = [&hsh]() {
                        hsh += 0x9E3779B97F4A7C15ull;
                        uint64_t z = hsh;
                        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                        return (float)(((z ^ (z >> 31)) >> 40) * (1.0 / 16777216.0));
                    };
                    if (rnd01() >= chance) continue;
                    int nc = (int)std::floor(glm::mix(count.x, count.y + 0.999f, rnd01()));
                    for (int m = 0; m < nc; ++m) {
                        float ang = rnd01() * 6.2831853f, r = glm::mix(dist.x, dist.y, rnd01()) * p.scale;
                        vec2 q = vec2(p.position) + vec2(std::cos(ang), std::sin(ang)) * r;
                        if (!T.empty() && (!T.inside(q) || T.waterSurfaceAt(q.x, q.y) != Terrain::kNoWater)) continue;
                        if (fpIndex.blocked(q, 0.2f)) continue;
                        float pick = rnd01() * wsum;
                        uint32_t vi = 0;
                        while (vi + 1 < cdf.size() && pick > cdf[vi]) ++vi;
                        float z = T.empty() ? p.position.z : T.heightAt(q.x, q.y);
                        quat rot = glm::angleAxis(rnd01() * 6.2831853f, vec3(0, 0, 1));
                        byVariant[vi].push_back({vec3(q, z - sink), {rot, glm::mix(sc.x, sc.y, rnd01())}});
                    }
                }
                for (uint32_t vi = 0; vi < cids.size(); ++vi) {
                    if (byVariant[vi].empty()) continue;
                    InstanceSet set{id, cids[vi], (uint32_t)s.instances.size(), 0, {CollisionKind::None}};
                    float cull = meshCull(cv[vi]["mesh"], 120.0f);
                    for (auto& [pos, rs] : byVariant[vi]) {
                        s.addInstance(cids[vi], pos, rs.first, rs.second, shadow, cull);
                        ++set.count;
                    }
                    s.sets.push_back(set);
                }
            }
            if (cache.points.empty()) warn(std::format("scatter '{}' placed nothing (check area, slope_deg, height_m, density)", id));
        } catch (const std::exception& e) {   // a bad rule is a warning, never a failed build
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
    // the player (hidden until play): a rigged character (player.character names a skinned mesh) or the default mannequin
    playerFirst = (uint32_t)s.instances.size();
    character.reset();
    const json& player = doc.value("player", json::object());
    try {
        if (player.is_object() && player.value("character", "mannequin") != "mannequin") {
            character = resolveCharacter(w, player);
            uint32_t m = s.addMeshAsset(character->mesh, character->mesh.name + "#player");   // own vertices, skinned every frame
            s.meshes[m].bounds.w *= 1.5f;                                                     // room for the limbs (GPU culling)
            s.dynamicVertexFirst = s.meshes[m].vertexStart;
            s.dynamicVertexCount = s.meshes[m].vertexCount;
            s.addInstance(m, vec3(0), quat(1, 0, 0, 0), 1.0f, true, -1.0f);
            playerCount = 1;
            for (auto& msg : character->warnings) warn("player.character: " + msg);
        }
    } catch (const std::exception& e) {
        warn(std::format("player.character: {} (using the mannequin)", e.what()));
        character.reset();
    }
    if (!character) {
        for (const MannequinPart& part : mannequinParts()) {
            uint32_t m = s.addMeshAsset(part.mesh, part.mesh.name);
            s.addInstance(m, vec3(0), quat(1, 0, 0, 0), 1.0f, true, -1.0f);   // negative cull distance: hidden
        }
        playerCount = (uint32_t)mannequinParts().size();
    }

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
    // sky occlusion around static structures (cached: retraced only when they or the ground under them change)
    s.skyVolume = skyOcclusion_.build(s, T);
    if (s.skyVolume) {
        json cells = json::array();
        for (auto& r : s.skyVolume->regions) cells.push_back(std::round(r->cell * 100) / 100);
        info.skyOcclusion = {{"volumes", s.skyVolume->regions.size()}, {"cells", s.skyVolume->cells}, {"cell_m", cells},
                             {"rays", s.skyVolume->rays}, {"trace_ms", std::round(s.skyVolume->ms)}};
    }
    info.instances = s.instances.size();
    info.meshes = s.meshes.size();
    info.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace df
