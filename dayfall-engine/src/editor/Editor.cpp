#include "editor/Editor.h"
#include "core/Error.h"
#include "core/Log.h"
#include "world/Area.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <map>

namespace df {
using json = nlohmann::json;
namespace fs = std::filesystem;

const char* categoryName(ToolCategory c) {
    switch (c) {
        case ToolCategory::Read: return "read";
        case ToolCategory::Meta: return "meta";
        case ToolCategory::Terrain: return "terrain";
        case ToolCategory::Lighting: return "lighting";
        case ToolCategory::Character: return "character";
        case ToolCategory::Layout: return "layout";
        case ToolCategory::Foliage: return "foliage";
        case ToolCategory::Gameplay: return "gameplay";
        case ToolCategory::Materials: return "materials";
        case ToolCategory::Assets: return "assets";
    }
    return "?";
}

std::string toolTitle(const std::string& name) {
    static const std::map<std::string, const char*> kTitles = {
        {"project_info", "Project overview"},      {"world_get", "Read the world document"},  {"catalog", "Placeable content"},
        {"terrain_info", "Terrain heights"},       {"ground_query", "Ground at points"},      {"capture", "Capture views"},
        {"stats", "Render statistics"},            {"log", "Engine log"},                     {"batch_begin", "Begin a batch"},
        {"batch_end", "End the batch"},            {"undo", "Undo"},                          {"redo", "Redo"},
        {"save", "Save the map"},                  {"map_new", "New map"},                    {"map_open", "Open a map"},
        {"terrain_generate", "Generate terrain"},  {"terrain_import", "Import a heightmap"},   {"terrain_sculpt", "Sculpt terrain"},
        {"terrain_paint", "Paint terrain"},        {"terrain_settings", "Terrain settings"},  {"path_set", "Add or change a path"},
        {"object_add", "Place objects"},           {"object_update", "Change objects"},       {"delete", "Delete items"},
        {"place_along_path", "Place along a path"}, {"scatter_set", "Scatter rule"},          {"entity_add", "Add gameplay entities"},
        {"hud_set", "In-game HUD"},                {"player_set", "Player character"},        {"environment_set", "Lighting and atmosphere"},
        {"light_set", "Add or change lights"},     {"material_set", "Define a material"},     {"asset_import", "Import a model"},
        {"doc_patch", "Patch the map document"},   {"camera_set", "Save a camera view"},      {"route_set", "Define a test route"},
        {"walk_test", "Walk-test a route"},        {"play_sim", "Simulate play input"},       {"play", "Play mode in the editor"},
        {"world_check", "Check the world for problems"}, {"find_space", "Find building sites"}, {"area_set", "Name an area"},
        {"object_duplicate", "Duplicate objects"}, {"world_diff", "What changed"},           {"editor_state", "The human's selection and view"},
        {"editor_select", "Highlight in the editor"}, {"prefab_save", "Save a prefab"},     {"prefab_place", "Place a prefab"}};
    if (auto it = kTitles.find(name); it != kTitles.end()) return it->second;
    std::string t = name;
    for (char& c : t) if (c == '_') c = ' ';
    if (!t.empty()) t[0] = (char)toupper((unsigned char)t[0]);
    return t;
}

bool Editor::reportProgress(double p, double total, const std::string& message) {
    if (progress) progress(p, total, message);
    return !cancelRequested;
}

static bool exclusive(ToolCategory c) {
    return c == ToolCategory::Terrain || c == ToolCategory::Lighting || c == ToolCategory::Character;
}

json BatchState::toJson() const {
    json t = json::array();
    for (auto c : touched) t.push_back(categoryName(c));
    if (!open) return {{"open", false}, {"batches_completed", number}};
    return {{"open", true}, {"number", number}, {"title", title}, {"implicit", implicit}, {"categories_changed", t},
            {"edits", edits}, {"unverified_edits", editsSinceCapture}, {"captures", captures}};
}

void Editor::init(Engine& eng, const EditorOptions& opt) {
    engine = &eng;
    options = opt;
    builder.init(opt.contentDir);
    physics.init();
    registerTools();
    engine->hudBuild = [this](HudCanvas& c) {
        drawHud(c);
        if ((windowFrame_ || overlayCamera_) && !game.active()) drawEditorOverlay(c);
    };
}

void Editor::shutdown() {
    if (engine) engine->hudBuild = nullptr;
    if (game.active()) game.end(physics, scene);
    physics.shutdown();
}

void Editor::openMap(const fs::path& dir) {
    if (game.active()) stopPlay();
    World w;
    w.load(dir);
    world = std::move(w);
    builtVersion_ = builtTerrainVersion_ = 0;
    terrainUploaded_ = nullptr;
    batch = BatchState();
    rebuildIfNeeded();
    editCamera = overviewCamera();
}

void Editor::newMap(const fs::path& dir, const std::string& name, const json& terrain) {
    if (game.active()) stopPlay();
    world = World();
    world.createNew(dir, name);
    if (terrain.is_object()) {
        world.terrain.generate(terrain);
        world.terrainToDoc();
    }
    builtVersion_ = builtTerrainVersion_ = 0;
    batch = BatchState();
    rebuildIfNeeded();
    editCamera = overviewCamera();
}

bool Editor::rebuildIfNeeded() {
    if (world.version == builtVersion_ && world.terrain.version == builtTerrainVersion_) return false;
    bool wasPlaying = game.active();
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&t0] {
        auto t = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t - t0).count();
        t0 = t;
        return ms;
    };
    builder.build(world, scene, entities, lastBuild);
    double msBuild = lap();
    engine->setScene(scene);
    if (builder.takeTerrainChanged() || terrainUploaded_ != &world.terrain)
        engine->renderer.setTerrain(world.terrain.empty() ? nullptr : &world.terrain, scene.terrainMaterial);
    else if (!world.terrain.empty())
        engine->renderer.setTerrainMaterial(scene.terrainMaterial);
    terrainUploaded_ = &world.terrain;
    double msUpload = lap();
    physics.buildStatic(scene, world.terrain);
    double msPhysics = lap();
    PlayerConfig pc;
    try { pc = PlayerConfig::parse(world.doc.value("player", json::object())); } catch (const std::exception& e) { logWarn("player: {}", e.what()); }
    game.rebind(builder, pc);
    if (wasPlaying) {   // keep playing from the same spot
        for (auto& id : game.collectedIds)
            for (auto& e : entities)
                if (e.id == id && e.instance < scene.instances.size()) {
                    scene.instances[e.instance].cullDistance = -1.0f;
                    if (e.light >= 0) scene.lights[e.light].intensity = 0;
                }
        engine->renderer.updateLights(scene);
    }
    builtVersion_ = world.version;
    builtTerrainVersion_ = world.terrain.version;
    logInfo("scene rebuilt in {:.0f} ms (build {:.0f}, GPU upload {:.0f}, physics {:.0f}): {} instances, {} meshes, {} warnings",
            msBuild + msUpload + msPhysics, msBuild, msUpload, msPhysics, lastBuild.instances, lastBuild.meshes, lastBuild.warnings.size());
    return true;
}

void Editor::startPlay() {
    rebuildIfNeeded();
    if (game.active()) return;
    game.begin(scene, entities, physics, PlayerConfig::parse(world.doc.value("player", json::object())), builder);
}

void Editor::stopPlay() {
    if (!game.active()) return;
    game.end(physics, scene);
    // restore pickups and lights for editing
    builtVersion_ = 0;
    rebuildIfNeeded();
}

void Editor::update(float dt, const PlayerInput& input) {
    time += dt;
    rebuildIfNeeded();
    if (game.active()) {
        game.update(dt, input, physics, scene, entities);
        if (game.lightsChanged) {
            engine->renderer.updateLights(scene);
            game.lightsChanged = false;
        }
    } else {
        Game::animateEntities(time, scene, entities);
    }
    engine->renderer.updateDynamicInstances(scene);
    std::string b = batch.open ? std::format("batch {} '{}' ({} edits, {} unverified)", batch.number, batch.title, batch.edits, batch.editsSinceCapture)
                               : "no open batch";
    status = std::format("{}{} | {} | {}", world.doc.value("name", "map"), world.dirty ? " *" : "", game.active() ? "PLAY" : "EDIT", b);
}

void Editor::render() {
    float aspect = (float)engine->renderer.settings().width / std::max(1u, engine->renderer.settings().height);
    Camera cam = game.active() ? game.camera(physics, aspect) : editCamera;
    windowFrame_ = true;
    engine->renderFrame(cam, time);
    windowFrame_ = false;
}


bool Editor::rayFromWindow(vec2 ndc, vec3& origin, vec3& dir) const {
    float aspect = (float)engine->renderer.settings().width / std::max(1u, engine->renderer.settings().height);
    const Camera& c = editCamera;
    vec3 f = c.forward(), r = glm::normalize(glm::cross(f, vec3(0, 0, 1))), u = glm::cross(r, f);
    float t = std::tan(c.vfov * 0.5f);
    origin = c.position;
    dir = glm::normalize(f + r * (ndc.x * t * aspect) + u * (ndc.y * t));
    return true;
}

void Editor::hover(vec2 ndc) {
    vec3 o, d;
    hasCursorGround = false;
    if (!rayFromWindow(ndc, o, d)) return;
    RayHit h = physics.raycast(o, d, 20000.0f);
    if (h.hit) { hasCursorGround = true; cursorGround = h.position; }
}

void Editor::pick(vec2 ndc, bool add) {
    vec3 o, d;
    if (!rayFromWindow(ndc, o, d)) return;
    RayHit h = physics.raycast(o, d, 20000.0f);
    if (!add) selection.clear();
    if (!h.hit || h.instance == RayHit::kTerrain || h.instance >= scene.instances.size()) {
        status = h.hit ? std::format("ground at ({:.1f}, {:.1f}, {:.1f})", h.position.x, h.position.y, h.position.z) : "nothing there";
        return;
    }
    for (auto& s : scene.sets) {
        if (h.instance < s.first || h.instance >= s.first + s.count) continue;
        if (!world.find(s.name)) break;   // the player, water: not selectable
        Selected sel{s.name, s.count > 1 ? h.instance : UINT32_MAX, h.position};
        auto it = std::find_if(selection.begin(), selection.end(), [&](const Selected& x) { return x.id == sel.id && x.instance == sel.instance; });
        if (it != selection.end()) selection.erase(it);   // shift-click again: deselect
        else selection.push_back(sel);
        status = std::format("selected {}", s.name);
        break;
    }
}

float Editor::groundHeight(vec2 p) const {
    RayHit h = physics.groundBelow(p.x, p.y);
    if (h.hit) return h.position.z;
    return world.terrain.empty() ? 0.0f : world.terrain.heightAt(p.x, p.y);
}

Camera Editor::overviewCamera() const {
    Camera c;
    c.vfov = glm::radians(50.0f);
    vec3 centre, size;
    if (!world.terrain.empty()) {
        const Terrain& t = world.terrain;
        vec2 mid = t.origin + vec2(t.size() * 0.5f);
        centre = vec3(mid, t.heightAt(mid.x, mid.y));
        size = vec3(t.size());
    } else {
        centre = (scene.boundsMin + scene.boundsMax) * 0.5f;
        size = glm::max(scene.boundsMax - scene.boundsMin, vec3(40));
    }
    float s = std::max(size.x, size.y);
    c.position = centre + vec3(0, -0.62f * s, 0.36f * s);
    c.lookAt(centre + vec3(0, 0.08f * s, 0));
    c.nearPlane = 0.5f;
    return c;
}

Camera Editor::playerStartCamera() const {
    Camera c;
    c.vfov = glm::radians(55.0f);
    vec3 p = scene.playerStart.position;
    float yaw = glm::radians(scene.playerStart.yawDeg);
    vec3 fwd(std::cos(yaw), std::sin(yaw), 0);
    c.position = p - fwd * 5.5f + vec3(0, 0, 2.4f);
    c.lookAt(p + fwd * 10.0f + vec3(0, 0, 1.2f));
    c.nearPlane = 0.1f;
    return c;
}

std::vector<uint8_t> Editor::captureRgba(const CaptureView& v, uint32_t w, uint32_t h, float t) {
    rebuildIfNeeded();
    bool showIdle = v.showPlayer && !game.active();
    if (showIdle) game.showIdle(scene, builder, scene.playerStart.position, glm::radians(scene.playerStart.yawDeg - 90.0f), true);
    if (!game.active()) Game::animateEntities(t, scene, entities);
    engine->renderer.updateDynamicInstances(scene);
    overlayCamera_ = v.selection ? &v.camera : nullptr;
    CaptureResult r = engine->capture(v.camera, t, w, h);
    overlayCamera_ = nullptr;
    if (showIdle) {
        game.showIdle(scene, builder, scene.playerStart.position, 0.0f, false);
        engine->renderer.updateDynamicInstances(scene);
    }
    return std::move(r.rgba);
}

ToolResult Editor::runEdit(const Tool& t, const json& args) {
    // batch rules from the agent workflow: small batches, verified with a
    // capture, never terrain + lighting + character together
    if (!batch.open) {
        batch = BatchState{true, true, batch.number + 1, "(implicit) " + t.name, "", {}, 0, 0, 0, {}};
        batch.docAtStart = world.doc;
        batch.terrainVersionAtStart = world.terrain.version;
    }
    ToolCategory cat = t.categoryFor ? t.categoryFor(args) : t.category;
    if (options.enforceRules && exclusive(cat)) {
        for (ToolCategory c : batch.touched)
            if (exclusive(c) && c != cat)
                return ToolResult::fail(std::format(
                    "Rule: never change terrain, lighting and character in one batch. Batch {} ('{}') already changed {}; '{}' changes {}. "
                    "Capture and inspect the current result, call batch_end, then batch_begin a new batch for the {} work.",
                    batch.number, batch.title, categoryName(c), t.name, categoryName(cat), categoryName(cat)));
    }
    ToolResult r = t.run(args);
    if (r.error) return r;
    batch.touched.insert(cat);
    ++batch.edits;
    ++batch.editsSinceCapture;
    batch.calls.push_back(t.name);
    rebuildIfNeeded();
    r.data["batch"] = batch.toJson();
    if (!lastBuild.warnings.empty()) r.data["build_warnings"] = lastBuild.warnings;
    r.data["next"] = "Not verified yet: call capture and look at the result before batch_end.";
    return r;
}

ToolResult Editor::call(const std::string& name, const json& argsIn) {
    const Tool* t = nullptr;
    for (auto& x : tools_) if (x.name == name) t = &x;
    if (!t) return ToolResult::fail("unknown tool '" + name + "'");
    json args = argsIn.is_object() ? argsIn : json::object();
    try {
        // named areas: "area": "town" means the map's areas.town (scatter_set keeps the name so the rule follows the area)
        static const std::set<std::string> keepNames = {"scatter_set", "area_set", "doc_patch"};
        if (!keepNames.count(name) && world.doc.contains("areas")) args = Area::expandArgs(args, world.doc["areas"]);
        if (t->edits()) return runEdit(*t, args);
        return t->run(args);
    } catch (const Error& e) {
        return ToolResult::fail(e.what());
    } catch (const json::exception& e) {
        return ToolResult::fail(std::string("bad arguments: ") + e.what());
    } catch (const std::exception& e) {
        logError("tool {} failed: {}", name, e.what());
        return ToolResult::fail(std::string("internal error: ") + e.what());
    }
}

std::string Editor::instructions() const {
    return R"(DAYFALL engine: a live, agent-first game editor. The world is a JSON document (map.json) plus a terrain heightfield; every tool edits it live and the editor window updates immediately.

Coordinates: metres, Z up, X east, Y north. yaw_deg is counter-clockwise from +X (0 = east, 90 = north). Objects with a 2-component position [x, y] sit on the ground and follow terrain edits.

Workflow (follow it):
1. Read before you change anything: project_info, then world_get / catalog / terrain_info as needed.
2. Work in small batches: batch_begin {title} -> a few related edits -> capture -> look at the images -> fix -> batch_end.
3. A successful tool call does not mean the level is right. Capture and inspect after every batch; batch_end refuses unverified edits.
4. Never change terrain, lighting and character in one batch (enforced). Layout, foliage and gameplay edits can go in any batch.
5. Block out with primitives and test the whole route with walk_test (the default mannequin, real physics) before adding art. Fix every stuck / fall / water event it reports.
   Before batch_end, run world_check (floating / buried / duplicate objects, objects on paths, entities inside objects) and fix what it finds.
   Place precisely instead of guessing: find_space for building sites, object_add place {on / next_to / relative_to}, area_set to name regions.
   When the human says "this", "that one" or "here", call editor_state: it returns what they clicked in the editor window.
6. Then import or place library assets, then refine in small batches: vegetation, lighting, details.
Use undo if a batch went wrong. Captures are saved under <map>/captures/ for the human to review.)";
}
}  // namespace df
