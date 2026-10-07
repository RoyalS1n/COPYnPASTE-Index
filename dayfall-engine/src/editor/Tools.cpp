// Every editor operation, exposed as an MCP tool. Keep descriptions short and
// exact: they are what an agent reads to decide what to call.
#include "core/Error.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Log.h"
#include "editor/Editor.h"
#include "editor/WorldTools.h"
#include "scene/GltfLoader.h"
#include "scene/Materials.h"
#include "scene/Primitives.h"
#include "world/Area.h"
#include "world/EnvironmentDoc.h"
#include "world/Noise.h"
#include <algorithm>
#include <cmath>
#include <format>

namespace df {
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
// ---------------------------------------------------------------- schema helpers
json S(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }
json num(const char* d) { return S("number", d); }
json integer(const char* d) { return S("integer", d); }
json str(const char* d) { return S("string", d); }
json boolean(const char* d) { return S("boolean", d); }
json anyObj(const char* d) { return S("object", d); }
json arr(json items, const char* d) { return {{"type", "array"}, {"items", items}, {"description", d}}; }
json point(const char* d) { return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 3}, {"description", d}}; }
json points(const char* d) { return arr({{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 3}}, d); }
json object(json props, std::vector<std::string> req = {}) {
    json o = {{"type", "object"}, {"properties", props}};
    if (!req.empty()) o["required"] = req;
    return o;
}
const char* kAreaDoc =
    "Area: {\"circle\": {\"center\": [x,y], \"radius\": r}} | {\"rect\": {\"center\": [x,y], \"size\": [w,h], \"yaw_deg\": a}} | "
    "{\"polygon\": [[x,y],...]} | {\"line\": [[x,y],...], \"width\": w} | {\"union\": [area, ...]} | \"all\"; optional \"falloff\": "
    "metres of soft edge.";
json areaSchema() { return {{"description", kAreaDoc}}; }

vec2 xy(const json& p) {
    if (!p.is_array() || p.size() < 2) throw Error("a position must be [x, y] or [x, y, z]");
    return {p[0].get<float>(), p[1].get<float>()};
}
json r1(vec3 v) { return {std::round(v.x * 10) / 10, std::round(v.y * 10) / 10, std::round(v.z * 10) / 10}; }
float rnd(float v, float s = 10.0f) { return std::round(v * s) / s; }

json mergePatch(json target, const json& patch) {
    if (!patch.is_object()) return patch;
    if (!target.is_object()) target = json::object();
    for (auto& [k, v] : patch.items()) {
        if (v.is_null()) target.erase(k);
        else target[k] = mergePatch(target.value(k, json()), v);
    }
    return target;
}

std::string idPrefixFor(const json& mesh) {
    std::string p = mesh.is_string() ? mesh.get<std::string>() : mesh.is_object() ? mesh.value("type", "obj") : "obj";
    for (char& c : p) if (!isalnum((unsigned char)c) && c != '_') c = '_';
    return p.empty() ? "obj" : p;
}

std::vector<vec2> densify(const std::vector<vec2>& p, float step) {
    std::vector<vec2> out;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        int n = std::max(1, (int)std::ceil(glm::length(p[i + 1] - p[i]) / step));
        for (int k = 0; k < n; ++k) out.push_back(glm::mix(p[i], p[i + 1], (float)k / n));
    }
    if (!p.empty()) out.push_back(p.back());
    return out;
}
}  // namespace

void Editor::registerTools() {
    Editor& E = *this;
    auto requireTerrain = [&E]() {
        if (E.world.terrain.empty()) throw Error("the map has no terrain yet: call terrain_generate first");
    };
    auto groundAt = [&E](vec2 p) { return E.world.terrain.empty() ? 0.0f : E.world.terrain.heightAt(p.x, p.y); };
    auto ensureUnique = [&E](const std::string& id) {
        if (id.empty()) throw Error("ids must not be empty");
        if (E.world.find(id)) throw Error("id '" + id + "' already exists; pick another or update it instead");
    };

    // =================================================================== read
    addTool({"project_info",
             "Start here. Map, rules, coordinate conventions, world summary (terrain, counts, environment, player), the open batch, "
             "undo history and build warnings.",
             ToolCategory::Read, object({}),
             [&E](const json&) {
                 ToolResult r;
                 const json& d = E.world.doc;
                 json counts = json::object();
                 for (auto& s : World::idSections()) counts[s] = d.contains(s) && d[s].is_array() ? d[s].size() : 0;
                 json cams = json::array(), routes = json::array();
                 const json camsDoc = d.value("cameras", json::object()), routesDoc = d.value("routes", json::object());   // named: items() on a temporary dangles
                 for (auto& [k, v] : camsDoc.items()) cams.push_back(k);
                 for (auto& [k, v] : routesDoc.items()) routes.push_back(k);
                 json lib = E.builder.catalog(E.world)["meshes"];
                 std::map<std::string, int> cats;
                 for (auto& [k, v] : lib.items()) cats[v.value("category", "uncategorised")]++;
                 r.data = {
                     {"engine", "DAYFALL engine 0.1 (Vulkan, GPU-driven, Jolt physics)"},
                     {"map", {{"name", d.value("name", "")}, {"dir", E.world.dir.string()}, {"unsaved_changes", E.world.dirty}}},
                     {"coordinates", "metres; Z up, X east, Y north; yaw_deg counter-clockwise from +X (0 east, 90 north); "
                                     "a 2-component position [x, y] means on the ground"},
                     {"rules", {"Read before changing (this, world_get, catalog, terrain_info).",
                                "Small batches: batch_begin -> edits -> capture -> inspect -> fix -> batch_end.",
                                "A successful tool call does not mean the level is right: capture after every batch (batch_end enforces it).",
                                "Never change terrain, lighting and character in one batch (enforced).",
                                "Block out first, walk_test the route with the default mannequin, then add art."}},
                     {"enforced", E.options.enforceRules},
                     {"terrain", E.world.terrain.summary()},
                     {"counts", counts},
                     {"cameras", cams},
                     {"routes", routes},
                     {"areas", [&] {
                          json n = json::array();
                          const json areas = d.value("areas", json::object());   // named: items() on a temporary dangles
                          for (auto& [k, v] : areas.items()) n.push_back(k);
                          return n;
                      }()},
                     {"environment", d.value("environment", json::object())},
                     {"player", PlayerConfig::parse(d.value("player", json::object())).toJson()},
                     {"character", E.game.animator().info()},
                     {"player_start", d.value("player_start", json::object())},
                     {"content_library", {{"meshes", lib.size()}, {"categories", cats}}},
                     {"batch", E.batch.toJson()},
                     {"history", E.world.history(10)},
                     {"build", E.lastBuild.toJson()},
                     {"playing", E.playing()}};
                 return r;
             }});

    // an object's world box and the ground under it (derived, not stored: object_update ignores "resolved")
    auto resolvedOf = [&E](const json& o) -> json {
        try {
            ItemBox b = objectBox(E, o);
            vec2 c(b.center());
            json j = {{"min", r1(b.lo)}, {"max", r1(b.hi)}, {"size_m", r1(b.hi - b.lo)}};
            if (!E.world.terrain.empty()) j["ground_z"] = rnd(E.world.terrain.heightAt(c.x, c.y), 100);
            return j;
        } catch (const Error& e) {
            return {{"error", e.what()}};
        }
    };
    addTool({"world_get",
             "Read the world document. No arguments: a summary of every section. section: objects|scatter|paths|entities|lights|"
             "environment|player|player_start|cameras|routes|areas|terrain|materials|meshes|hud. Filter lists with id, ids, tag, near "
             "{position [x,y], radius_m} and limit (default 50). Objects come with resolved: their world box (min, max, size_m) "
             "and the ground height under them.",
             ToolCategory::Read,
             object({{"section", str("section name")}, {"id", str("one item by id")}, {"ids", arr(str(""), "items by id")},
                     {"tag", str("items with this tag")}, {"near", anyObj("{\"position\": [x,y], \"radius_m\": r}")},
                     {"limit", integer("max items (default 50)")}}),
             [&E, resolvedOf](const json& a) {
                 ToolResult r;
                 const json& d = E.world.doc;
                 if (a.contains("id")) {
                     std::string sec;
                     json* o = E.world.find(a["id"].get<std::string>(), &sec);
                     if (!o) return ToolResult::fail("no item with id '" + a["id"].get<std::string>() + "'");
                     r.data = {{"section", sec}, {"item", *o}};
                     if (sec == "objects") r.data["item"]["resolved"] = resolvedOf(*o);
                     return r;
                 }
                 if (!a.contains("section")) {
                     json heads = json::object();
                     for (auto& s : World::idSections()) {
                         json ids = json::array();
                         for (auto& o : d.value(s, json::array())) if (ids.size() < 30) ids.push_back(o.value("id", "?"));
                         heads[s] = {{"count", d.value(s, json::array()).size()}, {"first_ids", ids}};
                     }
                     r.data = {{"name", d.value("name", "")}, {"sections", heads}, {"environment", d.value("environment", json::object())},
                               {"player", d.value("player", json::object())}, {"player_start", d.value("player_start", json::object())},
                               {"cameras", d.value("cameras", json::object())}, {"routes", d.value("routes", json::object())},
                               {"terrain", d.value("terrain", json())}};
                     return r;
                 }
                 std::string sec = a["section"].get<std::string>();
                 if (!d.contains(sec)) { r.data = {{"section", sec}, {"value", nullptr}}; return r; }
                 const json& v = d[sec];
                 if (!v.is_array()) { r.data = {{"section", sec}, {"value", v}}; return r; }
                 size_t limit = a.value("limit", 50u);
                 std::set<std::string> ids;
                 if (a.contains("ids")) for (auto& i : a["ids"]) ids.insert(i.get<std::string>());
                 json out = json::array();
                 size_t total = 0;
                 for (auto& o : v) {
                     if (!ids.empty() && !ids.count(o.value("id", ""))) continue;
                     if (a.contains("tag")) {
                         auto tags = o.value("tags", json::array());
                         if (std::find(tags.begin(), tags.end(), a["tag"]) == tags.end()) continue;
                     }
                     if (a.contains("near")) {
                         vec2 c = xy(a["near"].at("position"));
                         float rad = a["near"].value("radius_m", 20.0f);
                         if (!o.contains("position")) continue;
                         if (glm::length(xy(o["position"]) - c) > rad) continue;
                     }
                     ++total;
                     if (out.size() < limit) {
                         out.push_back(o);
                         if (sec == "objects") out.back()["resolved"] = resolvedOf(o);
                     }
                 }
                 r.data = {{"section", sec}, {"total_matching", total}, {"items", out}};
                 return r;
             }});

    addTool({"catalog",
             "What can be placed: built-in primitives (parametric blockout shapes), content-library and map meshes (with category, "
             "size, collision), materials, environment presets, entity types, terrain presets, sculpt ops and paint layers.",
             ToolCategory::Read, object({{"category", str("only library meshes of this category (e.g. trees, rocks, buildings)")}}),
             [&E](const json& a) {
                 ToolResult r;
                 json c = E.builder.catalog(E.world);
                 if (a.contains("category")) {
                     json f = json::object();
                     for (auto& [k, v] : c["meshes"].items()) if (v.value("category", "") == a["category"].get<std::string>()) f[k] = v;
                     c["meshes"] = f;
                 }
                 json presets = json::array();
                 for (auto& [k, v] : environmentPresets().items()) presets.push_back(k);
                 c["environment_presets"] = presets;
                 c["entity_types"] = {{"collectible", "glowing pickup; color, size_m, hover_m, light"},
                                      {"goal", "ring on the ground; radius_m, message shown on arrival"},
                                      {"trigger", "invisible; radius_m, message"},
                                      {"spawn", "spawn point marker"},
                                      {"waypoint", "route marker"}};
                 c["terrain_presets"] = {"flat", "hills", "valley", "mountains", "meadow", "island"};
                 c["sculpt_ops"] = {"raise", "lower", "flatten", "set", "smooth", "noise", "ramp"};
                 c["paint_layers"] = {"dirt", "rock", "snow", "wet", "dry", "grass"};
                 r.data = c;
                 return r;
             }});

    addTool({"terrain_info",
             "Terrain size, height range and a coarse height grid (rows south to north) to plan valleys, paths and building sites.",
             ToolCategory::Read,
             object({{"grid_cells", integer("grid resolution per side, 2..64 (default 16)")}, {"area", areaSchema()}}),
             [&E, requireTerrain](const json& a) {
                 requireTerrain();
                 ToolResult r;
                 Area area = a.contains("area") ? Area::parse(a["area"]) : Area();
                 r.data = {{"summary", E.world.terrain.summary()}, {"grid", E.world.terrain.heightGrid(a.value("grid_cells", 16u), area)}};
                 return r;
             }});

    addTool({"ground_query",
             "Ground facts at points: height (top surface, including objects), terrain height, slope, path / rock / snow / wet "
             "weights, water depth, and which object is underneath.",
             ToolCategory::Read, object({{"points", points("[[x, y], ...] up to 200")}}, {"points"}),
             [&E](const json& a) {
                 ToolResult r;
                 json out = json::array();
                 for (auto& p : a.at("points")) {
                     if (out.size() >= 200) break;
                     vec2 q = xy(p);
                     RayHit h = E.physics.groundBelow(q.x, q.y);
                     float th = E.world.terrain.empty() ? 0.0f : E.world.terrain.heightAt(q.x, q.y);
                     json o = {{"x", q.x}, {"y", q.y}, {"height", rnd(h.hit ? h.position.z : th, 100)}, {"terrain_height", rnd(th, 100)}};
                     if (!E.world.terrain.empty()) {
                         vec4 l = E.world.terrain.layersAt(q.x, q.y);
                         o["slope_deg"] = rnd(E.world.terrain.slopeDegAt(q.x, q.y));
                         o["rock"] = rnd(l.x, 100); o["snow"] = rnd(l.y, 100); o["wet"] = rnd(l.z, 100); o["path"] = rnd(l.w, 100);
                         o["inside_terrain"] = E.world.terrain.inside(q);
                     }
                     if (E.scene.env.waterLevel > -999 && th < E.scene.env.waterLevel) o["water_depth"] = rnd(E.scene.env.waterLevel - th, 100);
                     if (h.hit && h.instance != RayHit::kTerrain && h.instance < E.scene.instances.size())
                         for (auto& s : E.scene.sets)
                             if (h.instance >= s.first && h.instance < s.first + s.count) { o["on"] = s.name; break; }
                     out.push_back(o);
                 }
                 r.data = {{"points", out}};
                 return r;
             }});

    addTool({"capture",
             "Render views of the live world and return them as images (also saved under <map>/captures/). ALWAYS look at "
             "captures after edits: this is how you verify. views (max 6): {\"overview\": true} | {\"camera\": name} | "
             "{\"position\": [x,y,z], \"target\": [x,y,z] or [x,y], \"vfov_deg\"} | {\"player\": true} (third-person view at the "
             "player start, mannequin shown for scale) | {\"top_down\": {\"center\": [x,y], \"size_m\": s}} | {\"orbit\": "
             "{\"target\": [x,y(,z)], \"distance_m\", \"yaw_deg\", \"pitch_deg\"}} | {\"editor\": true}. Default: overview + player. "
             "The in-game HUD is drawn while playing; hud: true also previews it while editing (at the player start), false hides it. "
             "selection: true draws the editor selection's boxes (what the human clicked, or editor_select) as the human sees them.",
             ToolCategory::Read,
             object({{"views", arr(anyObj("a view"), "views to render")}, {"width", integer("pixels (default 1024)")},
                     {"height", integer("pixels (default 576)")}, {"format", str("jpeg (default) or png")},
                     {"save", boolean("save to <map>/captures (default true)")},
                     {"hud", boolean("draw the HUD: default only while playing; true also while editing; false never")},
                     {"selection", boolean("draw the selection's boxes")}}),
             [&E](const json& a) {
                 ToolResult r;
                 struct HudScope { Editor& e; int keep; ~HudScope() { e.hudForce = keep; } } hudScope{E, E.hudForce};
                 if (a.contains("hud")) E.hudForce = a["hud"].get<bool>() ? 1 : 0;
                 uint32_t w = std::clamp(a.value("width", E.options.captureWidth), 256u, 1920u);
                 uint32_t h = std::clamp(a.value("height", E.options.captureHeight), 144u, 1080u);
                 bool png = a.value("format", "jpeg") == "png";
                 json views = a.value("views", json::array({{{"overview", true}}, {{"player", true}}}));
                 if (!views.is_array() || views.empty()) views = json::array({{{"overview", true}}});
                 json info = json::array();
                 float aspect = (float)w / h;
                 for (size_t vi = 0; vi < views.size() && vi < 6; ++vi) {
                     const json& v = views[vi];
                     CaptureView cv;
                     float shadowOverride = 0;
                     if (v.contains("camera")) {
                         std::string n = v["camera"].get<std::string>();
                         auto it = E.scene.cameras.find(n);
                         if (it == E.scene.cameras.end()) {
                             std::string names;
                             for (auto& [k, c] : E.scene.cameras) names += (names.empty() ? "" : ", ") + k;
                             throw Error(std::format("no camera '{}' (cameras: {})", n, names.empty() ? "none; add with camera_set" : names));
                         }
                         cv.label = n;
                         cv.camera.position = it->second.position;
                         cv.camera.vfov = glm::radians(it->second.vfovDeg);
                         cv.camera.lookAt(it->second.target);
                     } else if (v.contains("position")) {
                         const json& p = v["position"];
                         vec2 q = xy(p);
                         cv.camera.position = vec3(q, p.size() >= 3 ? p[2].get<float>() : E.groundHeight(q) + 1.7f);
                         const json& t = v.value("target", json::array({q.x, q.y + 10.0f}));
                         vec2 tq = xy(t);
                         cv.camera.lookAt(vec3(tq, t.size() >= 3 ? t[2].get<float>() : E.groundHeight(tq) + 1.0f));
                         cv.camera.vfov = glm::radians(v.value("vfov_deg", 55.0f));
                         cv.label = "view";
                         cv.showPlayer = v.value("show_mannequin", false);
                     } else if (v.contains("top_down")) {
                         const json& td = v["top_down"];
                         vec2 c = td.contains("center") ? xy(td["center"])
                                                        : (E.world.terrain.empty() ? vec2(0) : E.world.terrain.origin + vec2(E.world.terrain.size() * 0.5f));
                         float size = td.value("size_m", E.world.terrain.empty() ? 200.0f : E.world.terrain.size());
                         cv.camera.vfov = glm::radians(30.0f);
                         float alt = size * 0.5f / std::tan(cv.camera.vfov * 0.5f) / std::min(aspect, 1.0f);
                         cv.camera.position = vec3(c, E.groundHeight(c) + alt);
                         cv.camera.yaw = 0;
                         cv.camera.pitch = -1.5703f;
                         cv.camera.nearPlane = std::max(0.5f, alt * 0.01f);
                         cv.label = "top_down";
                         shadowOverride = alt + size;
                     } else if (v.contains("orbit")) {
                         const json& o = v["orbit"];
                         const json& t = o.at("target");
                         vec2 tq = xy(t);
                         vec3 target(tq, t.size() >= 3 ? t[2].get<float>() : E.groundHeight(tq) + 1.0f);
                         float d = o.value("distance_m", 25.0f), yaw = glm::radians(o.value("yaw_deg", -90.0f)),
                               pitch = glm::radians(o.value("pitch_deg", 25.0f));
                         cv.camera.position = target + vec3(std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)) * d;
                         cv.camera.lookAt(target);
                         cv.camera.vfov = glm::radians(o.value("vfov_deg", 50.0f));
                         cv.label = "orbit";
                         cv.showPlayer = o.value("show_mannequin", false);
                     } else if (v.value("player", false)) {
                         cv.camera = E.playing() ? E.game.camera(E.physics, aspect) : E.playerStartCamera();
                         cv.showPlayer = !E.playing();
                         cv.label = "player";
                     } else if (v.value("editor", false)) {
                         cv.camera = E.editCamera;
                         cv.label = "editor";
                     } else {
                         cv.camera = E.overviewCamera();
                         cv.label = "overview";
                     }
                     E.engine->renderer.shadowDistanceOverride = shadowOverride;
                     cv.selection = a.value("selection", false);
                     std::vector<uint8_t> rgba = E.captureRgba(cv, w, h, E.time);
                     E.engine->renderer.shadowDistanceOverride = 0;
                     std::vector<uint8_t> enc = png ? encodePng(rgba.data(), w, h) : encodeJpeg(rgba.data(), w, h, 85);
                     ToolImage img{png ? "image/png" : "image/jpeg", base64(enc), cv.label, ""};
                     if (a.value("save", true) && !E.world.dir.empty()) {
                         fs::path p = E.world.dir / "captures" / std::format("{:04d}_{}.{}", ++E.captureCounter_, cv.label, png ? "png" : "jpg");
                         if (writeFile(p, enc)) img.path = p.string();
                     }
                     vec3 fwd = cv.camera.forward();
                     info.push_back({{"label", cv.label}, {"position", r1(cv.camera.position)}, {"looking", r1(fwd)}, {"saved", img.path}});
                     r.images.push_back(img);
                 }
                 E.batch.editsSinceCapture = 0;
                 ++E.batch.captures;
                 r.data = {{"views", info},
                           {"gpu_ms", rnd((float)E.engine->lastGpuMs())},
                           {"check", "Look for: floating or buried objects, gaps and intersections, scale against the 1.8 m "
                                     "mannequin, path continuity, empty or cluttered areas, lighting and exposure. Fix before batch_end."}};
                 return r;
             },
             nullptr, true});

    addTool({"stats", "Render and world statistics: GPU time per pass, instances, triangles, physics bodies, build time.",
             ToolCategory::Read, object({}),
             [&E](const json&) {
                 ToolResult r;
                 const FrameStats& st = E.engine->stats();
                 json passes = json::object();
                 for (int i = 0; i < FrameStats::kPasses; ++i) passes[st.names[i]] = rnd((float)st.ms[i], 100);
                 r.data = {{"gpu", E.engine->device.gpuName}, {"gpu_ms_last_frame", rnd((float)st.totalMs, 100)}, {"passes_ms", passes},
                           {"build", E.lastBuild.toJson()}, {"physics_static_bodies", E.physics.staticBodies()},
                           {"lights", E.scene.lights.size()}, {"textures", E.scene.textures.size()},
                           {"hud_ms_last", rnd((float)E.engine->hudGpuMs(), 100)}};
                 return r;
             }});

    addTool({"log", "Recent engine log lines (warnings, errors, timings).", ToolCategory::Read,
             object({{"lines", integer("how many (default 40, max 300)")}}),
             [](const json& a) {
                 ToolResult r;
                 r.data = {{"lines", recentLog(std::min(a.value("lines", 40u), 300u))}};
                 return r;
             }});

    // =================================================================== batches, history, files
    addTool({"batch_begin",
             "Open a batch of related edits (one goal, e.g. 'valley base', 'town layout', 'evening light'). Terrain, lighting and "
             "character changes must be in separate batches.",
             ToolCategory::Meta, object({{"title", str("what this batch does")}, {"intent", str("optional: what done looks like")}}, {"title"}),
             [&E](const json& a) {
                 if (E.batch.open && E.batch.edits > 0 && E.options.enforceRules)
                     return ToolResult::fail(std::format("batch {} ('{}') is still open with {} edit(s){}. Capture, inspect, then batch_end first.",
                                                         E.batch.number, E.batch.title, E.batch.edits,
                                                         E.batch.editsSinceCapture ? std::format(", {} unverified", E.batch.editsSinceCapture) : ""));
                 int n = E.batch.open && E.batch.edits == 0 ? E.batch.number : E.batch.number + 1;
                 E.batch = BatchState{true, false, n, a.at("title").get<std::string>(), a.value("intent", ""), {}, 0, 0, 0, {}};
                 E.batch.docAtStart = E.world.doc;
                 E.batch.terrainVersionAtStart = E.world.terrain.version;
                 ToolResult r;
                 r.data = {{"batch", E.batch.toJson()}};
                 return r;
             }});

    addTool({"batch_end",
             "Close the open batch and save the map. Refused while edits are unverified (no capture since the last edit) unless "
             "force is true. Summarise what changed in notes.",
             ToolCategory::Meta,
             object({{"notes", str("what changed and what you saw in the captures")}, {"save", boolean("save the map (default true)")},
                     {"force", boolean("close even with unverified edits (avoid)")}}),
             [&E](const json& a) {
                 if (!E.batch.open) return ToolResult::fail("no batch is open");
                 if (E.batch.editsSinceCapture > 0 && E.options.enforceRules && !a.value("force", false))
                     return ToolResult::fail(std::format("{} edit(s) since the last capture. A successful tool call doesn't mean the level is "
                                                         "right: call capture, inspect the images, fix problems, then batch_end.",
                                                         E.batch.editsSinceCapture));
                 ToolResult r;
                 json summary = E.batch.toJson();
                 summary["notes"] = a.value("notes", "");
                 summary["calls"] = E.batch.calls;
                 bool saved = false;
                 if (a.value("save", true) && E.world.dirty && !E.world.dir.empty()) { E.world.save(); saved = true; }
                 E.batch.open = false;
                 E.batch.touched.clear();
                 r.data = {{"closed", summary}, {"saved", saved}};
                 return r;
             }});

    addTool({"undo", "Undo the last edit(s). Counts as an edit that needs a capture.", ToolCategory::Meta,
             object({{"steps", integer("how many edits (default 1)")}}),
             [&E](const json& a) {
                 json undone = json::array();
                 for (int i = 0; i < std::clamp(a.value("steps", 1), 1, 100); ++i) {
                     std::string label;
                     if (!E.world.undo(&label)) break;
                     undone.push_back(label);
                 }
                 if (undone.empty()) return ToolResult::fail("nothing to undo");
                 E.rebuildIfNeeded();
                 if (E.batch.open) ++E.batch.editsSinceCapture;
                 ToolResult r;
                 r.data = {{"undone", undone}, {"history", E.world.history(5)}};
                 return r;
             }});

    addTool({"redo", "Redo edits undone with undo.", ToolCategory::Meta, object({{"steps", integer("default 1")}}),
             [&E](const json& a) {
                 json redone = json::array();
                 for (int i = 0; i < std::clamp(a.value("steps", 1), 1, 100); ++i) {
                     std::string label;
                     if (!E.world.redo(&label)) break;
                     redone.push_back(label);
                 }
                 if (redone.empty()) return ToolResult::fail("nothing to redo");
                 E.rebuildIfNeeded();
                 if (E.batch.open) ++E.batch.editsSinceCapture;
                 ToolResult r;
                 r.data = {{"redone", redone}};
                 return r;
             }});

    addTool({"save", "Save the map (map.json and terrain files). batch_end also saves.", ToolCategory::Meta, object({}),
             [&E](const json&) {
                 E.world.save();
                 ToolResult r;
                 r.data = {{"saved", E.world.dir.string()}};
                 return r;
             }});

    addTool({"map_new",
             "Create and open a new map directory (unsaved changes in the current map are kept on disk only if saved). terrain: "
             "optional terrain_generate arguments.",
             ToolCategory::Meta,
             object({{"dir", str("map directory, e.g. maps/my_level")}, {"name", str("display name")}, {"terrain", anyObj("terrain_generate arguments")}},
                    {"dir"}),
             [&E](const json& a) {
                 fs::path dir = a.at("dir").get<std::string>();
                 if (fs::exists(dir / "map.json")) return ToolResult::fail(dir.string() + " already has a map; use map_open");
                 E.newMap(dir, a.value("name", dir.filename().string()), a.value("terrain", json()));
                 E.world.save();
                 ToolResult r;
                 r.data = {{"created", dir.string()}, {"terrain", E.world.terrain.summary()}};
                 return r;
             }});

    addTool({"map_open", "Open another map directory (saves nothing; call save first if needed).", ToolCategory::Meta,
             object({{"dir", str("map directory containing map.json")}}, {"dir"}),
             [&E](const json& a) {
                 E.openMap(a.at("dir").get<std::string>());
                 ToolResult r;
                 r.data = {{"opened", E.world.dir.string()}, {"build", E.lastBuild.toJson()}};
                 return r;
             }});

    // =================================================================== terrain
    addTool({"terrain_generate",
             "Replace the terrain with a procedural base. preset: flat|hills|valley|mountains|meadow|island; size_m (default 512), "
             "spacing_m (default 1), seed. valley: valley_width_m, valley_slope_m, mountain_height_m, backdrop_height_m, "
             "meander_amp_m, axis_deg (valley runs along +Y rotated by axis_deg), closed_end. hills/mountains: height_m, feature_m. "
             "meadow: radius_m, rim_height_m. island: radius_m, height_m, water_level_m. Also snowline_m, micro_relief.",
             ToolCategory::Terrain, object({{"preset", str("terrain preset")}, {"size_m", num("side length")}, {"spacing_m", num("sample spacing")},
                                            {"seed", integer("random seed")}}, {"preset"}),
             [&E](const json& a) {
                 E.world.beginEdit("terrain_generate " + a.value("preset", ""), true);
                 try {
                     E.world.terrain.generate(a);
                     E.world.terrainToDoc();
                     if (a.contains("water_level_m") && a.value("preset", "") == "island") {
                         E.world.doc["environment"]["water"] = {{"enabled", true}, {"level_m", a["water_level_m"]}};
                     }
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"terrain", E.world.terrain.summary()}};
                 return r;
             }});

    addTool({"terrain_import",
             "Replace the terrain with a heightmap image (porting a level from Unreal, World Machine, Gaea...): file (.png 16-bit, "
             ".r16 / .raw), size_m (world size of the image), height_range_m [lo, hi] (black..white), origin [x, y] (default: "
             "centred), flip_x, flip_y. Unreal landscape exports: height_range_m = [-2.56 * z_scale, 2.56 * z_scale], flip_y: true.",
             ToolCategory::Terrain,
             object({{"file", str("heightmap path")}, {"size_m", num("")}, {"height_range_m", arr(num(""), "[lo, hi]")}, {"origin", point("")},
                     {"flip_x", boolean("")}, {"flip_y", boolean("")}}, {"file"}),
             [&E](const json& a) {
                 E.world.beginEdit("terrain_import", true);
                 json summary;
                 try {
                     summary = E.world.terrain.importHeightmap(a.at("file").get<std::string>(), a);
                     E.world.terrainToDoc();
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"terrain", summary}};
                 return r;
             }});

    addTool({"terrain_sculpt",
             "Brush edits of the base terrain. op: raise|lower (amount_m) | flatten (height_m, default: average in the area) | set "
             "(height_m) | smooth (iterations, kernel_m) | noise (amplitude_m, scale_m) | ramp (from [x,y,z], to [x,y,z]: straight "
             "grade, e.g. a road up a hill). area is required; strength 0..1. Several brush ops at once: ops: [{op, area, ...}].",
             ToolCategory::Terrain, object({{"op", str("operation")}, {"area", areaSchema()}, {"strength", num("0..1, default 1")},
                                            {"ops", arr(anyObj("a brush op"), "several ops applied in order")}}),
             [&E, requireTerrain](const json& a) {
                 requireTerrain();
                 json ops = a.contains("ops") ? a["ops"] : json::array({a});
                 E.world.beginEdit("terrain_sculpt", true);
                 json results = json::array();
                 try {
                     for (auto& op : ops) results.push_back(E.world.terrain.sculpt(op));
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"results", results}};
                 return r;
             }});

    addTool({"terrain_paint",
             "Paint surface layers: layer dirt|rock|snow|wet|dry|grass (grass removes automatic rock / dry), area, strength 0..1, "
             "mode add|set|erase. Several strokes: strokes: [{layer, area, ...}].",
             ToolCategory::Terrain, object({{"layer", str("layer")}, {"area", areaSchema()}, {"strength", num("0..1")}, {"mode", str("add|set|erase")},
                                            {"strokes", arr(anyObj("a stroke"), "several strokes")}}),
             [&E, requireTerrain](const json& a) {
                 requireTerrain();
                 json strokes = a.contains("strokes") ? a["strokes"] : json::array({a});
                 E.world.beginEdit("terrain_paint", true);
                 json results = json::array();
                 try {
                     for (auto& s : strokes) results.push_back(E.world.terrain.paintLayer(s));
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"results", results}};
                 return r;
             }});

    addTool({"terrain_settings",
             "Automatic layer rules: snowline_m, rock_slope_deg (slopes steeper than this show rock), dry_amount (0..1 dry grass "
             "patches), material (terrain material name).",
             ToolCategory::Terrain, object({{"snowline_m", num("")}, {"rock_slope_deg", num("")}, {"dry_amount", num("")}, {"material", str("")}}),
             [&E, requireTerrain](const json& a) {
                 requireTerrain();
                 E.world.beginEdit("terrain_settings", false);
                 json& t = E.world.doc["terrain"];
                 for (const char* k : {"snowline_m", "rock_slope_deg", "dry_amount", "material"})
                     if (a.contains(k)) t[k] = a[k];
                 E.world.docToTerrain();
                 E.world.terrain.touch();
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"terrain", t}};
                 return r;
             }});

    addTool({"path_set",
             "Create or replace a path (dirt road, trail, lane) along points; it flattens and carves the terrain under it "
             "non-destructively and paints it. Fields: id (replace if it exists), points [[x,y],...] (smoothed as a spline), "
             "width_m (3), style dirt|lane (lane: two wheel ruts), carve_m (0.12), flatten 0..1 (0.85), falloff_m, smooth_m (16: "
             "length over which the height profile is smoothed), max_grade (e.g. 0.12 for 12%; 0 = off).",
             ToolCategory::Terrain,
             object({{"id", str("path id")}, {"points", points("centre line")}, {"width_m", num("")}, {"style", str("dirt|lane")},
                     {"carve_m", num("")}, {"flatten", num("")}, {"falloff_m", num("")}, {"smooth_m", num("")}, {"max_grade", num("")}},
                    {"points"}),
             [&E, requireTerrain](const json& a) {
                 requireTerrain();
                 if (!a.at("points").is_array() || a["points"].size() < 2) throw Error("a path needs at least 2 points");
                 json p = a;
                 for (auto& q : p["points"]) xy(q);
                 std::string id = a.value("id", "");
                 E.world.beginEdit("path_set", true);
                 std::string sec;
                 json* existing = id.empty() ? nullptr : E.world.find(id, &sec);
                 if (existing && sec != "paths") { E.world.cancelEdit(); throw Error("id '" + id + "' belongs to " + sec); }
                 if (id.empty()) { id = E.world.newId("path"); p["id"] = id; }
                 if (existing) *existing = p;
                 else E.world.list("paths").push_back(p);
                 E.world.terrain.touch();
                 E.world.endEdit();
                 E.rebuildIfNeeded();
                 // report the walkable grade along the result
                 std::vector<vec2> pts;
                 for (auto& q : p["points"]) pts.push_back(xy(q));
                 std::vector<vec2> d = densify(pts, 4.0f);
                 float len = 0, maxGrade = 0;
                 for (size_t i = 1; i < d.size(); ++i) {
                     float dl = glm::length(d[i] - d[i - 1]);
                     len += dl;
                     if (dl > 0.01f) maxGrade = std::max(maxGrade, std::abs(E.world.terrain.heightAt(d[i].x, d[i].y) - E.world.terrain.heightAt(d[i - 1].x, d[i - 1].y)) / dl);
                 }
                 ToolResult r;
                 r.data = {{"id", id}, {"length_m", std::round(len)}, {"max_grade_percent", std::round(maxGrade * 100)},
                           {"note", maxGrade > 0.45f ? "steeper than the mannequin can climb in places; add max_grade or ramp the terrain" : "ok"}};
                 return r;
             }});

    // =================================================================== layout
    auto addObjects = [&E, groundAt, ensureUnique](const json& list, std::vector<json>& added) {
        for (const json& in : list) {
            json o = in;
            if (!o.contains("mesh")) throw Error("each object needs \"mesh\" (a mesh name or a primitive spec)");
            auto asset = E.builder.resolveMesh(E.world, o["mesh"]);   // validates
            applyRelativePlacement(E, o);   // "place": on / next_to / relative_to another item
            if (!o.contains("position")) throw Error("each object needs \"position\": [x, y] (on the ground) or [x, y, z], or \"place\"");
            vec2 p = xy(o["position"]);
            if (o.contains("face_towards")) {
                vec2 t = xy(o["face_towards"]);
                std::string front = "-y";
                if (o["mesh"].is_string()) {
                    json cat = E.builder.catalog(E.world)["meshes"];
                    if (cat.contains(o["mesh"].get<std::string>())) front = cat[o["mesh"].get<std::string>()].value("front", "-y");
                }
                vec2 f0 = front == "+y" ? vec2(0, 1) : front == "+x" ? vec2(1, 0) : front == "-x" ? vec2(-1, 0) : vec2(0, -1);
                vec2 dir = t - p;
                if (glm::length(dir) > 1e-3f) o["yaw_deg"] = rnd(glm::degrees(std::atan2(dir.y, dir.x) - std::atan2(f0.y, f0.x)));
                o.erase("face_towards");
            }
            std::string id = o.value("id", "");
            if (id.empty()) { id = E.world.newId(idPrefixFor(o["mesh"])); o["id"] = id; }
            else ensureUnique(id);
            E.world.list("objects").push_back(o);
            const json& sj = o.value("scale", json(1.0f));
            vec3 sc = sj.is_array() && sj.size() == 3 ? vec3(sj[0].get<float>(), sj[1].get<float>(), sj[2].get<float>()) : vec3(sj.get<float>());
            vec3 size = glm::abs((asset->aabbMax - asset->aabbMin) * sc);
            bool onGround = o.value("on_ground", o["position"].size() < 3);
            float z = onGround ? groundAt(p) + o.value("offset_z", 0.0f) : o["position"][2].get<float>();
            ItemBox box = objectBox(E, o);
            added.push_back({{"id", id}, {"position", r1(vec3(p, z))}, {"size_m", r1(size)}, {"min", r1(box.lo)}, {"max", r1(box.hi)}});
        }
    };

    addTool({"object_add",
             "Place objects. Each: mesh (name from catalog, or a primitive spec like {\"type\": \"box\", \"size\": [4,6,3], "
             "\"material\": \"plaster\"}), position [x,y] (on the ground) or [x,y,z], yaw_deg, pitch_deg, roll_deg (or "
             "rotation [qx,qy,qz,qw]), scale (number or per-axis [x,y,z]), materials {\"mesh material\": \"replacement\"}, "
             "face_towards [x,y], offset_z, align_to_ground, collision auto|none|mesh|convex|box|cylinder, shadow, "
             "cull_distance_m, tags [..], id. Instead of position, place relative to another item (also one added earlier in "
             "the same call): place {on: id, at [x,y]} (on its top surface) | {next_to: id, side: north|south|east|west, gap_m "
             "(0.3), offset_m (along the side)} | {relative_to: id, offset [dx,dy(,dz)] in its own frame, match_yaw (true)}. "
             "Pass one object's fields or objects: [...] (any number).",
             ToolCategory::Layout, object({{"objects", arr(anyObj("an object"), "objects to add")}, {"mesh", {{"description", "mesh name or primitive spec"}}},
                                           {"position", point("[x,y] or [x,y,z]")}}),
             [&E, addObjects](const json& a) {
                 json list = a.contains("objects") ? a["objects"] : json::array({a});
                 E.world.beginEdit("object_add", false);
                 std::vector<json> added;
                 try { addObjects(list, added); } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"added", added}};
                 return r;
             }});

    addTool({"object_update",
             "Change objects or entities: ids [...] or id or tag; set {field: value} (merged; null removes a field; set {place: "
             "{...}} moves it relative to another item as in object_add), move_by [dx,dy(,dz)], rotate_by_deg (each in place, or "
             "all together about pivot [x,y] or \"center\": turns a group such as a house with its fence), scale_by.",
             ToolCategory::Layout,
             object({{"ids", arr(str(""), "")}, {"id", str("")}, {"tag", str("")}, {"set", anyObj("fields to merge")},
                     {"move_by", point("")}, {"rotate_by_deg", num("")}, {"pivot", {{"description", "[x, y] or \"center\""}}},
                     {"scale_by", num("")}}),
             [&E](const json& a) {
                 std::vector<json*> targets;
                 E.world.beginEdit("object_update", false);
                 auto collect = [&](const std::string& id) {
                     std::string sec;
                     json* o = E.world.find(id, &sec);
                     if (!o) throw Error("no item with id '" + id + "'");
                     if (sec != "objects" && sec != "entities") throw Error("'" + id + "' is in " + sec + "; use the matching tool");
                     targets.push_back(o);
                 };
                 try {
                     if (a.contains("id")) collect(a["id"].get<std::string>());
                     if (a.contains("ids")) for (auto& i : a["ids"]) collect(i.get<std::string>());
                     if (a.contains("tag"))
                         for (const char* s : {"objects", "entities"})
                             for (json& o : E.world.list(s)) {
                                 auto tags = o.value("tags", json::array());
                                 if (std::find(tags.begin(), tags.end(), a["tag"]) != tags.end()) targets.push_back(&o);
                             }
                     if (targets.empty()) throw Error("nothing matched (give id, ids or tag)");
                     bool hasPivot = a.contains("pivot") && a.contains("rotate_by_deg");
                     vec2 pivot(0);
                     if (hasPivot) {
                         if (a["pivot"].is_string()) {
                             if (a["pivot"].get<std::string>() != "center") throw Error("pivot must be [x, y] or \"center\"");
                             for (json* o : targets) pivot += xy(o->value("position", json::array({0, 0})));
                             pivot /= (float)targets.size();
                         } else {
                             pivot = xy(a["pivot"]);
                         }
                     }
                     for (json* o : targets) {
                         if (a.contains("set")) {
                             json patch = a["set"];
                             patch.erase("id");
                             patch.erase("resolved");
                             json place = patch.contains("place") ? patch["place"] : json();
                             patch.erase("place");
                             *o = mergePatch(*o, patch);
                             if (!place.is_null()) {
                                 (*o)["place"] = place;
                                 applyRelativePlacement(E, *o);
                             }
                             if (o->contains("mesh")) E.builder.resolveMesh(E.world, (*o)["mesh"]);
                         }
                         if (a.contains("move_by")) {
                             const json& d = a["move_by"];
                             json& p = (*o)["position"];
                             p[0] = p[0].get<float>() + d[0].get<float>();
                             p[1] = p[1].get<float>() + d[1].get<float>();
                             if (d.size() >= 3) {
                                 if (p.size() >= 3) p[2] = p[2].get<float>() + d[2].get<float>();
                                 else (*o)["offset_z"] = o->value("offset_z", 0.0f) + d[2].get<float>();
                             }
                         }
                         if (hasPivot) rotateItemAbout(*o, pivot, a["rotate_by_deg"].get<float>());
                         else if (a.contains("rotate_by_deg")) (*o)["yaw_deg"] = o->value("yaw_deg", 0.0f) + a["rotate_by_deg"].get<float>();
                         if (a.contains("scale_by")) {
                             float k = a["scale_by"].get<float>();
                             json& sj = (*o)["scale"];
                             if (sj.is_array()) for (auto& c : sj) c = c.get<float>() * k;
                             else sj = (sj.is_number() ? sj.get<float>() : 1.0f) * k;
                         }
                     }
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 json ids = json::array();
                 for (json* o : targets) ids.push_back(o->value("id", "?"));
                 ToolResult r;
                 r.data = {{"updated", ids}};
                 return r;
             }});

    addTool({"delete",
             "Delete items by ids [...], by tag, or every item of a section inside an area ({section, area}). Works for objects, "
             "entities, scatter rules, paths and lights.",
             ToolCategory::Layout,
             object({{"ids", arr(str(""), "")}, {"tag", str("")}, {"section", str("objects|entities|scatter|paths|lights")}, {"area", areaSchema()}}),
             [&E](const json& a) {
                 E.world.beginEdit("delete", true);
                 json removed = json::array();
                 try {
                     if (a.contains("ids"))
                         for (auto& i : a["ids"]) {
                             if (!E.world.erase(i.get<std::string>())) throw Error("no item with id '" + i.get<std::string>() + "'");
                             removed.push_back(i);
                         }
                     if (a.contains("tag") || (a.contains("section") && a.contains("area"))) {
                         std::vector<std::string> secs = a.contains("section") ? std::vector<std::string>{a["section"].get<std::string>()} : World::idSections();
                         Area area = a.contains("area") ? Area::parse(a["area"]) : Area();
                         for (auto& s : secs) {
                             json& arr = E.world.list(s);
                             for (size_t k = arr.size(); k-- > 0;) {
                                 const json& o = arr[k];
                                 if (a.contains("tag")) {
                                     auto tags = o.value("tags", json::array());
                                     if (std::find(tags.begin(), tags.end(), a["tag"]) == tags.end()) continue;
                                 }
                                 if (a.contains("area")) {
                                     if (!o.contains("position")) continue;
                                     if (!area.contains(xy(o["position"]))) continue;
                                 }
                                 removed.push_back(o.value("id", "?"));
                                 arr.erase(k);
                             }
                         }
                     }
                     if (removed.empty()) throw Error("nothing matched");
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.terrain.touch();   // paths may have changed
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"deleted", removed}};
                 return r;
             },
             [&E](const json& a) {
                 auto secOf = [&](const std::string& id) { std::string s; E.world.find(id, &s); return s; };
                 std::set<std::string> secs;
                 if (a.contains("section")) secs.insert(a["section"].get<std::string>());
                 if (a.contains("ids")) for (auto& i : a["ids"]) secs.insert(secOf(i.get<std::string>()));
                 if (secs.count("paths")) return ToolCategory::Terrain;
                 if (secs.count("lights")) return ToolCategory::Lighting;
                 if (secs.count("scatter")) return ToolCategory::Foliage;
                 if (secs.count("entities")) return ToolCategory::Gameplay;
                 return ToolCategory::Layout;
             }});

    addTool({"place_along_path",
             "Place a row of objects along a path or polyline: fences, lamp posts, hedges, market stalls. mesh, path_id or points, "
             "spacing_m, offset_m (sideways from the centre line; positive = left), both_sides, yaw: along|across|<degrees>, "
             "yaw_offset_deg, jitter_m, start_m/end_m (distance range along the line), scale, tags, id_prefix, collision.",
             ToolCategory::Layout,
             object({{"mesh", {{"description", "mesh name or primitive spec"}}}, {"path_id", str("")}, {"points", points("")},
                     {"spacing_m", num("")}, {"offset_m", num("")}, {"both_sides", boolean("")}, {"yaw", {{"description", "along|across|degrees"}}},
                     {"yaw_offset_deg", num("")}, {"jitter_m", num("")}, {"start_m", num("")}, {"end_m", num("")}, {"scale", num("")},
                     {"tags", arr(str(""), "")}, {"id_prefix", str("")}},
                    {"mesh"}),
             [&E, addObjects](const json& a) {
                 std::vector<vec2> pts;
                 if (a.contains("path_id")) {
                     std::string sec;
                     json* p = E.world.find(a["path_id"].get<std::string>(), &sec);
                     if (!p || sec != "paths") throw Error("no path '" + a["path_id"].get<std::string>() + "'");
                     for (auto& q : (*p)["points"]) pts.push_back(xy(q));
                 } else {
                     for (auto& q : a.at("points")) pts.push_back(xy(q));
                 }
                 if (pts.size() < 2) throw Error("need at least 2 points");
                 std::vector<vec2> d = densify(pts, 0.25f);
                 float spacing = std::max(a.value("spacing_m", 3.0f), 0.2f), offset = a.value("offset_m", 0.0f), jitter = a.value("jitter_m", 0.0f);
                 float start = a.value("start_m", 0.0f), endM = a.value("end_m", 1e9f);
                 std::string yawMode = a.contains("yaw") && a["yaw"].is_string() ? a["yaw"].get<std::string>() : "along";
                 float yawFixed = a.contains("yaw") && a["yaw"].is_number() ? a["yaw"].get<float>() : 0.0f;
                 bool fixedYaw = a.contains("yaw") && a["yaw"].is_number();
                 std::string prefix = a.value("id_prefix", idPrefixFor(a["mesh"]));
                 json list = json::array();
                 float acc = 0, next = start;
                 uint32_t count = 0;
                 for (size_t i = 1; i < d.size() && list.size() < 2000; ++i) {
                     float seg = glm::length(d[i] - d[i - 1]);
                     while (next <= acc + seg && next <= endM && list.size() < 2000) {
                         float t = seg > 0 ? (next - acc) / seg : 0;
                         vec2 p = glm::mix(d[i - 1], d[i], t);
                         vec2 dir = glm::normalize(d[i] - d[i - 1]);
                         vec2 left(-dir.y, dir.x);
                         float heading = glm::degrees(std::atan2(dir.y, dir.x));
                         for (int side : (a.value("both_sides", false) ? std::vector<int>{1, -1} : std::vector<int>{1})) {
                             vec2 q = p + left * (offset * side);
                             if (jitter > 0) q += vec2(hash01(count, 1) - 0.5f, hash01(count, 2) - 0.5f) * 2.0f * jitter;
                             float yaw = fixedYaw ? yawFixed : yawMode == "across" ? heading + 90.0f : heading;
                             yaw += a.value("yaw_offset_deg", 0.0f) + (side < 0 && !fixedYaw ? 180.0f : 0.0f);
                             json o = {{"mesh", a["mesh"]}, {"position", {rnd(q.x, 100), rnd(q.y, 100)}}, {"yaw_deg", rnd(yaw)},
                                       {"id", E.world.newId(prefix) + std::format("_{}", count)}};
                             for (const char* k : {"scale", "tags", "collision", "shadow", "cull_distance_m", "offset_z"})
                                 if (a.contains(k)) o[k] = a[k];
                             list.push_back(o);
                             ++count;
                         }
                         next += spacing;
                     }
                     acc += seg;
                 }
                 if (list.empty()) throw Error("the line is shorter than start_m / spacing_m allow");
                 // ids above were generated before insertion; make them unique and sequential
                 std::string base = E.world.newId(prefix);
                 for (size_t k = 0; k < list.size(); ++k) list[k]["id"] = std::format("{}_{}", base, k + 1);
                 E.world.beginEdit("place_along_path", false);
                 std::vector<json> added;
                 try { addObjects(list, added); } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"placed", added.size()}, {"first_id", added.front()["id"]}, {"last_id", added.back()["id"]}};
                 return r;
             }});

    // =================================================================== foliage
    addTool({"scatter_set",
             "Create or replace a procedural scatter rule (forests, grass, rocks, flowers). id (replace if exists), mesh or meshes "
             "[{mesh, weight}], area, density_per_100m2, min_spacing_m, clumping 0..1, clump_scale_m, scale [min,max], "
             "slope_deg [min,max], height_m [min,max], tilt_deg, align_to_slope 0..1, avoid_paths_m, avoid_objects_m, avoid_water, avoid_ruts, "
             "max_rock, max_path, sink_m, seed, shadow, collision (none|cylinder {radius, height}), cull_distance_m, max_instances.",
             ToolCategory::Foliage,
             object({{"id", str("rule id")}, {"mesh", {{"description", "mesh name or primitive spec"}}}, {"meshes", arr(anyObj(""), "variants")},
                     {"area", areaSchema()}, {"density_per_100m2", num("instances per 100 m2")}}),
             [&E](const json& a) {
                 json rule = normalizeScatterRule(Area::expandArgs(a, E.world.doc.value("areas", json::object())));
                 if (a.contains("area")) rule["area"] = a["area"];   // a named area stays a name: the rule follows it
                 for (auto& v : rule["meshes"]) E.builder.resolveMesh(E.world, v["mesh"]);
                 std::string id = a.value("id", "");
                 E.world.beginEdit("scatter_set", false);
                 std::string sec;
                 json* existing = id.empty() ? nullptr : E.world.find(id, &sec);
                 if (existing && sec != "scatter") { E.world.cancelEdit(); throw Error("id '" + id + "' belongs to " + sec); }
                 if (id.empty()) { id = E.world.newId("scatter"); rule["id"] = id; }
                 if (existing) *existing = rule;
                 else E.world.list("scatter").push_back(rule);
                 E.world.endEdit();
                 E.rebuildIfNeeded();
                 size_t placed = 0;
                 for (auto& s : E.scene.sets) if (s.name == id) placed += s.count;
                 ToolResult r;
                 r.data = {{"id", id}, {"instances", placed}};
                 return r;
             }});

    // =================================================================== gameplay
    addTool({"entity_add",
             "Add gameplay entities. type: collectible (glowing pickup: color [r,g,b], size_m, hover_m, light, radius_m) | goal "
             "(glowing ring: radius_m, message) | trigger (radius_m, message) | spawn | waypoint. position [x,y] (on the ground) "
             "or [x,y,z]; id. One entity's fields or entities: [...].",
             ToolCategory::Gameplay, object({{"entities", arr(anyObj("an entity"), "")}, {"type", str("")}, {"position", point("")}}),
             [&E, ensureUnique](const json& a) {
                 json list = a.contains("entities") ? a["entities"] : json::array({a});
                 E.world.beginEdit("entity_add", false);
                 json added = json::array();
                 try {
                     for (json e : list) {
                         std::string type = e.value("type", "");
                         if (type != "collectible" && type != "goal" && type != "trigger" && type != "spawn" && type != "waypoint")
                             throw Error("entity type must be collectible, goal, trigger, spawn or waypoint");
                         xy(e.at("position"));
                         std::string id = e.value("id", "");
                         if (id.empty()) { id = E.world.newId(type); e["id"] = id; }
                         else ensureUnique(id);
                         E.world.list("entities").push_back(e);
                         added.push_back(id);
                     }
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"added", added}};
                 return r;
             }});

    addTool({"hud_set",
             "In-game HUD drawn while playing and in play captures (a gameplay edit, merged into the map's hud section): enabled, "
             "scale, minimap {enabled, corner top_right|top_left|bottom_right|bottom_left, size_px (diameter at 1080p), range_m "
             "(player to edge), north_up (false: the view direction points up), shape round|square, route (a route name drawn as "
             "a trail)}, counters (collectibles found / total), timer, messages (toasts and goal banners); reset: true starts "
             "from the defaults. Verify with capture {views: [{player: true}], hud: true} or play_sim.",
             ToolCategory::Gameplay,
             object({{"enabled", boolean("")}, {"scale", num("multiplies every size")},
                     {"minimap", anyObj("{enabled, corner, size_px, range_m, north_up, shape, route}, or false")},
                     {"counters", boolean("")}, {"timer", boolean("")}, {"messages", boolean("")}, {"reset", boolean("")}}),
             [&E](const json& a) {
                 json patch = a;
                 patch.erase("reset");
                 json next = a.value("reset", false) ? patch : mergePatch(E.world.doc.value("hud", json::object()), patch);
                 HudConfig cfg = HudConfig::parse(next);   // validates
                 if (!cfg.route.empty() && !E.world.doc.value("routes", json::object()).contains(cfg.route))
                     throw Error("no route '" + cfg.route + "' (define it with route_set)");
                 E.world.beginEdit("hud_set", false);
                 E.world.doc["hud"] = next;
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"hud", cfg.toJson()}};
                 return r;
             }});

    addTool({"player_set",
             "Character settings (a character batch): character (\"mannequin\" or a rigged mesh, catalog category characters), "
             "camera third_person|first_person, walk_speed, run_speed (m/s), jump_height_m, height_m, radius_m, max_slope_deg, "
             "step_height_m, camera_distance_m. Rigged characters: animations {idle, walk, run, jump, fall, land: clip name or "
             "{clip, speed_mps}} (unmapped states take the clip whose name contains the state), animation_files [GLBs with more "
             "clips for the same skeleton, map-relative, e.g. from asset_import], character_scale, character_yaw_offset_deg, "
             "root_motion keep|strip, animation_blend_s. start: {position [x,y(,z)], yaw_deg} moves the player start. The result "
             "lists the character's clips and the clip each state plays.",
             ToolCategory::Character,
             object({{"character", str("mannequin or a rigged mesh name")}, {"camera", str("")}, {"walk_speed", num("")}, {"run_speed", num("")},
                     {"jump_height_m", num("")}, {"animations", anyObj("state -> clip name or {clip, speed_mps}")},
                     {"animation_files", arr(str(""), "GLB files with more clips (relative to the map)")},
                     {"character_scale", num("model scale (default 1)")}, {"character_yaw_offset_deg", num("turns the model (default 0)")},
                     {"root_motion", str("keep (default) or strip: remove the root bone's horizontal travel")},
                     {"animation_blend_s", num("crossfade seconds (default 0.2)")}, {"start", anyObj("{position, yaw_deg}")}}),
             [&E](const json& a) {
                 json p = a, start = a.value("start", json());
                 p.erase("start");
                 if (!start.is_null()) xy(start.at("position"));
                 json next = mergePatch(E.world.doc.value("player", json::object()), p);
                 PlayerConfig::parse(next);   // validates the value types
                 if (next.contains("root_motion") && next["root_motion"] != "keep" && next["root_motion"] != "strip")
                     throw Error("root_motion must be keep or strip");
                 if (next.contains("animations") && !next["animations"].is_object()) throw Error("animations must be {\"state\": \"clip\"}");
                 const json anims = next.value("animations", json::object());   // named: items() on a temporary dangles
                 for (auto& [k, v] : anims.items())
                     if (k != "idle" && k != "walk" && k != "run" && k != "jump" && k != "fall" && k != "land")
                         throw Error("animations: unknown state '" + k + "' (idle, walk, run, jump, fall, land)");
                 if (next.value("character", "mannequin") != "mannequin") {   // a rigged character must load and its clips exist
                     CharacterAnimator test;
                     auto c = E.builder.resolveCharacter(E.world, next);
                     test.bind(c, PlayerConfig::parse(next));
                     if (!test.problems.empty()) {
                         std::string clips;
                         for (auto& clip : c->clips) clips += (clips.empty() ? "" : ", ") + clip.name;
                         throw Error(std::format("{}; clips: {}", test.problems[0], clips.empty() ? "none" : clips));
                     }
                 }
                 E.world.beginEdit("player_set", false);
                 if (!start.is_null()) E.world.doc["player_start"] = mergePatch(E.world.doc.value("player_start", json::object()), start);
                 if (!p.empty()) E.world.doc["player"] = next;
                 E.world.endEdit();
                 E.rebuildIfNeeded();
                 ToolResult r;
                 r.data = {{"player", PlayerConfig::parse(E.world.doc.value("player", json::object())).toJson()},
                           {"player_start", E.world.doc.value("player_start", json())}, {"character", E.game.animator().info()}};
                 return r;
             }});

    // =================================================================== lighting
    addTool({"environment_set",
             "Lighting and atmosphere (a lighting batch), merged into the current settings (replace: true starts from defaults): "
             "preset golden_hour|serene|noon|misty_morning|dusk|overcast, time_of_day (hours), sun {elevation_deg, azimuth_deg, "
             "strength, color, angle_deg}, sky {strength, aerosol_density}, haze {color_near, color_far, amount, start_m, depth_m, "
             "height_fog_density, height_fog_falloff, height_fog_base_m}, clouds {enabled, coverage, height_m, color}, tonemap "
             "{exposure_ev, contrast, saturation, vignette}, post {bloom {strength, threshold, size}, gain [r,g,b], highlights_gain, "
             "shadows_gain, white_temp_k, painterly (true or {enabled, radius, blend, edge_strength, depth_k, normal_k, chroma})}, "
             "wind {direction [x,y], strength}, water {enabled, level_m}, "
             "shadow_distance_m, sky_occlusion {enabled, cell_m, rays, strength} (sky light hidden by roofs and walls; false = off).",
             ToolCategory::Lighting, object({{"preset", str("")}, {"time_of_day", num("")}, {"sun", anyObj("")}, {"replace", boolean("")}}),
             [&E](const json& a) {
                 json patch = a;
                 patch.erase("replace");
                 Environment test;
                 json next = a.value("replace", false) ? patch : mergePatch(E.world.doc.value("environment", json::object()), patch);
                 if (patch.contains("preset") || patch.contains("time_of_day")) {   // a preset / time replaces explicit sun angles
                     if (patch.contains("preset") && !patch.contains("sun") && next.contains("sun")) next["sun"].erase("elevation_deg"), next["sun"].erase("azimuth_deg");
                 }
                 parseEnvironment(next, test);   // validates
                 E.world.beginEdit("environment_set", true);
                 E.world.doc["environment"] = next;
                 E.world.docToTerrain();
                 E.world.terrain.touch();      // water level changes the terrain's wet layer
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"environment", next}};
                 return r;
             }});

    addTool({"light_set",
             "Create or update a point light (a lighting batch): id, position [x,y] (offset_z above ground, default 2) or [x,y,z], "
             "color [r,g,b], intensity (default 10), range_m (default 12). Lanterns, torches, windows.",
             ToolCategory::Lighting,
             object({{"id", str("")}, {"position", point("")}, {"color", arr(num(""), "")}, {"intensity", num("")}, {"range_m", num("")}}),
             [&E](const json& a) {
                 std::string id = a.value("id", "");
                 E.world.beginEdit("light_set", false);
                 std::string sec;
                 json* existing = id.empty() ? nullptr : E.world.find(id, &sec);
                 try {
                     if (existing && sec != "lights") throw Error("id '" + id + "' belongs to " + sec);
                     if (existing) *existing = mergePatch(*existing, a);
                     else {
                         xy(a.at("position"));
                         json l = a;
                         if (id.empty()) { id = E.world.newId("light"); l["id"] = id; }
                         E.world.list("lights").push_back(l);
                     }
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"id", id}};
                 return r;
             }});

    // =================================================================== materials and assets
    addTool({"material_set",
             "Create or change a material. definition: {model: lit|courses|foliage|grass|terrain|water|emissive|unlit, ...}; "
             "lit: base_color, roughness, metallic, emissive, emissive_strength, textures {base, normal, orm}; courses (stone, "
             "bricks, tiles, planks): color_a, color_b, mortar_color, block_width, block_height, mortar_width, grime; terrain: "
             "grass, grass_dry, soil, rock, rock_dark, moss, snow. merge (default true) keeps unspecified fields.",
             ToolCategory::Materials, object({{"name", str("")}, {"definition", anyObj("")}, {"merge", boolean("")}}, {"name", "definition"}),
             [&E](const json& a) {
                 std::string name = a.at("name").get<std::string>();
                 json base = json::object();
                 if (a.value("merge", true)) {
                     if (E.world.doc.contains("materials") && E.world.doc["materials"].contains(name)) base = E.world.doc["materials"][name];
                     else if (builtinMaterials().contains(name)) base = builtinMaterials()[name];
                 }
                 json def = mergePatch(base, a.at("definition"));
                 parseMaterial(name, def, nullptr);   // validates
                 E.world.beginEdit("material_set " + name, false);
                 E.world.doc["materials"][name] = def;
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"material", name}, {"definition", def}};
                 return r;
             }});

    addTool({"asset_import",
             "Import a glTF / GLB model into the map's asset folder and register it as a mesh name. file (absolute or relative "
             "path), name, category, description, front (-y default: Blender front), import_scale, import_yaw_deg, "
             "ground_origin (move the origin to the bottom centre), lods [{ratio, distance_m}], lod0_distance_m, collision. "
             "A rigged model (a skin) becomes category characters and the result lists its clips (player_set character uses it); "
             "a GLB with animations but no mesh is only copied: pass its path to player_set animation_files.",
             ToolCategory::Assets,
             object({{"file", str("path to .glb / .gltf")}, {"name", str("mesh name")}, {"category", str("")}, {"collision", {{"description", "auto|none|mesh|convex|box|cylinder"}}}},
                    {"file", "name"}),
             [&E](const json& a) {
                 fs::path src = a.at("file").get<std::string>();
                 if (!fs::exists(src)) throw Error("no such file: " + src.string());
                 std::string name = a.at("name").get<std::string>();
                 fs::path dstDir = E.world.dir / "assets";
                 fs::create_directories(dstDir);
                 fs::path dst = dstDir / src.filename();
                 if (fs::absolute(src) != fs::absolute(dst)) {
                     fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
                     if (src.extension() == ".gltf") {   // copy referenced buffers and images
                         json g = json::parse(readText(src));
                         for (const char* key : {"buffers", "images"})
                             for (auto& b : g.value(key, json::array()))
                                 if (b.contains("uri") && b["uri"].get<std::string>().rfind("data:", 0) != 0) {
                                     fs::path f = src.parent_path() / b["uri"].get<std::string>();
                                     fs::create_directories((dstDir / b["uri"].get<std::string>()).parent_path());
                                     if (fs::exists(f)) fs::copy_file(f, dstDir / b["uri"].get<std::string>(), fs::copy_options::overwrite_existing);
                                 }
                     }
                 }
                 json entry = a;
                 entry.erase("file");
                 entry.erase("name");
                 entry["file"] = (fs::path("assets") / src.filename()).generic_string();
                 GltfRigInfo rig = inspectGltfRig(dst);
                 json clips = json::array();
                 for (auto& [n, sec] : rig.clips) clips.push_back({{"name", n}, {"seconds", rnd(sec, 100)}});
                 if (rig.meshes == 0 && !rig.clips.empty()) {   // clips only (Mixamo "without skin"): nothing to register
                     ToolResult r;
                     r.data = {{"animation_file", entry["file"]}, {"clips", clips},
                               {"next", "add this path to player_set animation_files (the character's skeleton must use the same joint names)"}};
                     return r;
                 }
                 if (rig.joints > 0 && !entry.contains("category")) entry["category"] = "characters";
                 MeshAsset test = loadGltfAsset(dst, name);   // validates
                 E.world.beginEdit("asset_import " + name, false);
                 E.world.doc["meshes"][name] = entry;
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"name", name}, {"triangles", test.triangleCount()}, {"size_m", r1(test.aabbMax - test.aabbMin)},
                           {"materials", test.materials.size()}, {"origin_note", test.aabbMin.z < -0.05f || test.aabbMin.z > 0.05f
                                                                                  ? "the model's base is not at z = 0; consider ground_origin: true"
                                                                                  : "base at z = 0"}};
                 if (rig.joints > 0) {
                     r.data["rigged"] = {{"joints", rig.joints}, {"clips", clips}};
                     r.data["next"] = "player_set {character: \"" + name + "\", animations: {idle, walk, run, jump, fall, land: clip names}}";
                 }
                 return r;
             }});

    addTool({"doc_patch",
             "Escape hatch: apply RFC 6902 JSON Patch operations to the map document (e.g. [{\"op\": \"replace\", \"path\": "
             "\"/objects/3/scale\", \"value\": 2}]). Prefer the specific tools; use world_get to read paths first.",
             ToolCategory::Layout, object({{"patch", arr(anyObj("an RFC 6902 operation"), "")}}, {"patch"}),
             [&E](const json& a) {
                 json next = E.world.doc.patch(a.at("patch"));
                 if (next.value("format", "") != "dayfall-map") throw Error("the patch must not change \"format\"");
                 E.world.beginEdit("doc_patch", true);
                 E.world.doc = next;
                 E.world.docToTerrain();
                 E.world.terrain.touch();
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"applied", a["patch"].size()}};
                 return r;
             },
             [](const json& a) {
                 std::set<std::string> top;
                 for (auto& op : a.value("patch", json::array())) {
                     std::string p = op.value("path", "/");
                     top.insert(p.substr(1, p.find('/', 1) - 1));
                 }
                 if (top.count("terrain") || top.count("paths")) return ToolCategory::Terrain;
                 if (top.count("environment") || top.count("lights")) return ToolCategory::Lighting;
                 if (top.count("player") || top.count("player_start")) return ToolCategory::Character;
                 if (top.count("scatter")) return ToolCategory::Foliage;
                 if (top.count("entities") || top.count("hud")) return ToolCategory::Gameplay;
                 return ToolCategory::Layout;
             }});

    // =================================================================== viewpoints, routes, testing
    addTool({"camera_set",
             "Save a named viewpoint for captures: name, position [x,y,z] and target [x,y,z], vfov_deg; or from_editor: true to "
             "save the human's current editor view; delete: true removes it.",
             ToolCategory::Meta,
             object({{"name", str("")}, {"position", point("")}, {"target", point("")}, {"vfov_deg", num("")}, {"from_editor", boolean("")},
                     {"delete", boolean("")}}, {"name"}),
             [&E](const json& a) {
                 std::string n = a.at("name").get<std::string>();
                 E.world.beginEdit("camera_set " + n, false);
                 if (a.value("delete", false)) E.world.doc["cameras"].erase(n);
                 else if (a.value("from_editor", false)) {
                     const Camera& c = E.editCamera;
                     E.world.doc["cameras"][n] = {{"position", r1(c.position)}, {"target", r1(c.position + c.forward() * 20.0f)},
                                                 {"vfov_deg", rnd(glm::degrees(c.vfov))}};
                 } else {
                     const json& p = a.at("position");
                     const json& t = a.at("target");
                     if (p.size() < 3 || t.size() < 3) { E.world.cancelEdit(); throw Error("camera position and target need [x, y, z]"); }
                     E.world.doc["cameras"][n] = {{"position", p}, {"target", t}, {"vfov_deg", a.value("vfov_deg", 50.0f)}};
                 }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"cameras", E.world.doc["cameras"]}};
                 return r;
             }});

    addTool({"route_set",
             "Define a named test route for walk_test: name, points [[x,y],...] (waypoints in order) or path_id (follow a path), "
             "include_start (prepend the player start, default true).",
             ToolCategory::Meta, object({{"name", str("")}, {"points", points("")}, {"path_id", str("")}, {"include_start", boolean("")}}, {"name"}),
             [&E](const json& a) {
                 json pts = json::array();
                 if (a.value("include_start", true)) {
                     vec3 s = E.scene.playerStart.position;
                     pts.push_back({rnd(s.x), rnd(s.y)});
                 }
                 if (a.contains("path_id")) {
                     std::string sec;
                     json* p = E.world.find(a["path_id"].get<std::string>(), &sec);
                     if (!p || sec != "paths") throw Error("no path '" + a["path_id"].get<std::string>() + "'");
                     std::vector<vec2> raw;
                     for (auto& q : (*p)["points"]) raw.push_back(xy(q));
                     for (vec2 q : densify(raw, 6.0f)) pts.push_back({rnd(q.x), rnd(q.y)});
                 }
                 for (auto& q : a.value("points", json::array())) { vec2 v = xy(q); pts.push_back({v.x, v.y}); }
                 if (pts.size() < 2) throw Error("a route needs at least 2 points");
                 E.world.beginEdit("route_set", false);
                 E.world.doc["routes"][a.at("name").get<std::string>()] = {{"points", pts}};
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"route", a["name"]}, {"waypoints", pts.size()}};
                 return r;
             }});

    addTool({"walk_test",
             "Walk the default mannequin along a route with real physics (as fast as possible) and report problems: stuck, fall, "
             "steep, water, timeout, plus pickups. route (name), points or path_id; run (default false); max_seconds (240); "
             "captures (default true: images at the first problem spots). passed = reached the end with no problems.",
             ToolCategory::Read,
             object({{"route", str("route name")}, {"points", points("")}, {"path_id", str("")}, {"run", boolean("")}, {"max_seconds", num("")},
                     {"captures", boolean("")}}),
             [&E](const json& a) {
                 WalkTestOptions opt;
                 if (a.contains("route")) {
                     std::string n = a["route"].get<std::string>();
                     const json& routes = E.world.doc.value("routes", json::object());
                     if (!routes.contains(n)) throw Error("no route '" + n + "' (define it with route_set)");
                     for (auto& q : routes[n]["points"]) opt.points.push_back(xy(q));
                 } else if (a.contains("path_id")) {
                     std::string sec;
                     json* p = E.world.find(a["path_id"].get<std::string>(), &sec);
                     if (!p || sec != "paths") throw Error("no path '" + a["path_id"].get<std::string>() + "'");
                     std::vector<vec2> raw;
                     for (auto& q : (*p)["points"]) raw.push_back(xy(q));
                     opt.points = densify(raw, 6.0f);
                 } else {
                     for (auto& q : a.at("points")) opt.points.push_back(xy(q));
                 }
                 opt.run = a.value("run", false);
                 opt.maxSeconds = std::clamp(a.value("max_seconds", 240.0f), 5.0f, 1200.0f);
                 if (E.playing()) E.stopPlay();
                 E.rebuildIfNeeded();
                 std::vector<vec3> problems;
                 ToolResult r;
                 r.data = runWalkTest(opt, E.scene, E.entities, E.physics, PlayerConfig::parse(E.world.doc.value("player", json::object())),
                                      E.scene.env.waterLevel, &problems);
                 if (a.value("captures", true)) {
                     for (size_t i = 0; i < problems.size() && i < 3; ++i) {
                         vec3 p = problems[i];
                         CaptureView cv;
                         cv.label = std::format("walk_problem_{}", i + 1);
                         cv.camera.position = p + vec3(-5.0f, -5.0f, 4.0f);
                         cv.camera.lookAt(p + vec3(0, 0, 0.9f));
                         cv.camera.vfov = glm::radians(60.0f);
                         E.game.showIdle(E.scene, E.builder, p, 0.0f, true);
                         E.engine->renderer.updateDynamicInstances(E.scene);
                         CaptureResult c = E.engine->capture(cv.camera, E.time, E.options.captureWidth, E.options.captureHeight);
                         auto enc = encodeJpeg(c.rgba.data(), c.width, c.height, 85);
                         ToolImage img{"image/jpeg", base64(enc), cv.label, ""};
                         if (!E.world.dir.empty()) {
                             fs::path path = E.world.dir / "captures" / std::format("{:04d}_{}.jpg", ++E.captureCounter_, cv.label);
                             if (writeFile(path, enc)) img.path = path.string();
                         }
                         r.images.push_back(img);
                     }
                     E.game.showIdle(E.scene, E.builder, vec3(0), 0.0f, false);
                     E.engine->renderer.updateDynamicInstances(E.scene);
                 }
                 return r;
             }});

    addTool({"play_sim",
             "Play the level with scripted input through the real player controller and camera, as fast as possible: test "
             "jumps, ledges, stairs and pickups. start: {position [x,y(,z)], yaw_deg} (default: the player start); inputs: "
             "[{seconds, move [x right, y forward], run, jump (pressed at the step start), turn_deg (spread over the step), "
             "expect_animation (idle|walk|run|jump|fall|land: the call fails if a rigged character is in another state at the "
             "step end)}]; "
             "captures: end (default) | each | none (with the HUD unless hud: false); camera: player (default) | side | front "
             "(4 m from the character, to check its animation).",
             ToolCategory::Read,
             object({{"start", anyObj("{position, yaw_deg}")}, {"inputs", arr(anyObj("an input step"), "input steps in order")},
                     {"captures", str("end|each|none")}, {"camera", str("player|side|front")},
                     {"hud", boolean("draw the HUD in the captures (default true)")}}, {"inputs"}),
             [&E](const json& a) {
                 struct HudScope { Editor& e; int keep; ~HudScope() { e.hudForce = keep; } } hudScope{E, E.hudForce};
                 E.hudForce = a.value("hud", true) ? -1 : 0;
                 if (E.playing()) E.stopPlay();
                 E.startPlay();
                 if (a.contains("start")) {
                     const json& st = a["start"];
                     vec2 p = xy(st.at("position"));
                     float z = st["position"].size() >= 3 ? st["position"][2].get<float>() : E.groundHeight(p) + 0.05f;
                     E.game.teleport(E.physics, vec3(p, z), st.value("yaw_deg", 90.0f));
                 }
                 std::string capMode = a.value("captures", "end");
                 ToolResult r;
                 json timeline = json::array();
                 const float dt = 1.0f / 60.0f;
                 float t = 0;
                 auto snap = [&](const std::string& label) {
                     float aspect = (float)E.options.captureWidth / E.options.captureHeight;
                     CaptureView cv;
                     cv.camera = E.game.camera(E.physics, aspect);
                     std::string cam = a.value("camera", "player");
                     if (cam == "side" || cam == "front") {   // looking at the character from its right side or its front
                         vec3 f(-std::sin(E.game.facing), std::cos(E.game.facing), 0.0f), dir = cam == "side" ? vec3(f.y, -f.x, 0.0f) : f;
                         vec3 at = E.game.feet + vec3(0, 0, E.game.config.height * 0.5f);
                         cv.camera.position = at + dir * 4.0f + vec3(0, 0, 0.4f);
                         cv.camera.lookAt(at);
                         cv.camera.vfov = glm::radians(50.0f);
                         cv.camera.nearPlane = 0.1f;
                     }
                     cv.label = label;
                     E.engine->renderer.updateDynamicInstances(E.scene);
                     CaptureResult c = E.engine->capture(cv.camera, E.time + t, E.options.captureWidth, E.options.captureHeight);
                     auto enc = encodeJpeg(c.rgba.data(), c.width, c.height, 85);
                     ToolImage img{"image/jpeg", base64(enc), label, ""};
                     if (!E.world.dir.empty()) {
                         fs::path path = E.world.dir / "captures" / std::format("{:04d}_{}.jpg", ++E.captureCounter_, label);
                         if (writeFile(path, enc)) img.path = path.string();
                     }
                     r.images.push_back(img);
                 };
                 int step = 0;
                 std::vector<std::string> unmet;
                 for (const json& in : a.at("inputs")) {
                     float secs = std::clamp(in.value("seconds", 1.0f), 0.0f, 60.0f);
                     int frames = std::max(1, (int)std::round(secs / dt));
                     PlayerInput pin;
                     if (in.contains("move")) pin.move = xy(in["move"]);
                     pin.run = in.value("run", false);
                     float turn = glm::radians(in.value("turn_deg", 0.0f)) / frames;
                     float maxRise = 0, startZ = E.game.feet.z;
                     for (int f = 0; f < frames && t < 300.0f; ++f) {
                         pin.jump = f == 0 && in.value("jump", false);
                         pin.lookYaw = turn;
                         E.game.update(dt, pin, E.physics, E.scene, E.entities);
                         maxRise = std::max(maxRise, E.game.feet.z - startZ);
                         t += dt;
                     }
                     ++step;
                     timeline.push_back({{"step", step}, {"t", rnd(t)}, {"position", r1(E.game.feet)}, {"on_ground", E.game.onGround},
                                         {"max_rise_m", rnd(maxRise, 100)}, {"speed", rnd(glm::length(vec2(E.game.velocity)))}});
                     if (E.game.animator().character()) timeline.back()["animation"] = CharacterAnimator::name(E.game.animator().state());
                     if (in.contains("expect_animation")) {
                         std::string want = in["expect_animation"].get<std::string>();
                         std::string got = E.game.animator().character() ? CharacterAnimator::name(E.game.animator().state()) : "mannequin";
                         if (got != want) unmet.push_back(std::format("step {}: animation {}, expected {}", step, got, want));
                     }
                     if (capMode == "each" && r.images.size() < 6) snap(std::format("sim_step_{}", step));
                 }
                 if (capMode == "end") snap("sim_end");
                 json msgs = json::array();
                 for (auto& m : E.game.messages) msgs.push_back(m.text);
                 r.data = {{"timeline", timeline},
                           {"final", {{"position", r1(E.game.feet)}, {"on_ground", E.game.onGround},
                                      {"facing_deg", rnd(glm::degrees(E.game.facing) + 90.0f)}}},
                           {"collected", E.game.collectedIds}, {"messages", msgs}};
                 if (E.game.animator().character()) r.data["character"] = E.game.animator().info();
                 if (!unmet.empty()) {
                     r.error = true;
                     r.data["error"] = std::format("{} expectation(s) not met", unmet.size());
                     r.data["unmet"] = unmet;
                 }
                 E.stopPlay();
                 return r;
             }});

    addTool({"play", "Start or stop play mode in the live editor window (for the human to try the level): action start|stop.",
             ToolCategory::Meta, object({{"action", str("start|stop")}}, {"action"}),
             [&E](const json& a) {
                 std::string act = a.at("action").get<std::string>();
                 if (act == "start") E.startPlay();
                 else if (act == "stop") E.stopPlay();
                 else throw Error("action must be start or stop");
                 ToolResult r;
                 r.data = {{"playing", E.playing()}};
                 return r;
             }});

    registerWorldTools();   // world_check, find_space, area_set, object_duplicate, world_diff (WorldTools.cpp)
}
}  // namespace df
