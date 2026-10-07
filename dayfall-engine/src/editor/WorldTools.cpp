// World tools for agents: checks that find what captures miss (floating, buried, overlapping and path-blocking
// objects, unreachable entities), a search for free building sites, named areas, duplication and a diff of what a
// batch changed. Shared spatial helpers (boxes, relative placement) are declared in WorldTools.h.
#include "editor/WorldTools.h"
#include "core/Error.h"
#include "world/Area.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace df {
using json = nlohmann::json;

namespace {
// ---------------------------------------------------------------- schema helpers (as in Tools.cpp)
json S(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }
json num(const char* d) { return S("number", d); }
json integer(const char* d) { return S("integer", d); }
json str(const char* d) { return S("string", d); }
json boolean(const char* d) { return S("boolean", d); }
json anyObj(const char* d) { return S("object", d); }
json arr(json items, const char* d) { return {{"type", "array"}, {"items", items}, {"description", d}}; }
json point(const char* d) { return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 3}, {"description", d}}; }
json object(json props, std::vector<std::string> req = {}) {
    json o = {{"type", "object"}, {"properties", props}};
    if (!req.empty()) o["required"] = req;
    return o;
}
json areaSchema(const char* what) { return {{"description", std::string(what) + " (an area object, \"all\" or a named area)"}}; }

float rnd(float v, float s = 10.0f) { return std::round(v * s) / s; }
json r2(vec2 v) { return {rnd(v.x), rnd(v.y)}; }
json r3(vec3 v) { return {rnd(v.x), rnd(v.y), rnd(v.z)}; }
vec2 xy(const json& p) {
    if (!p.is_array() || p.size() < 2) throw Error("a position must be [x, y] or [x, y, z]");
    return {p[0].get<float>(), p[1].get<float>()};
}
float yawOf(quat q) {
    vec3 x = q * vec3(1, 0, 0);
    return glm::degrees(std::atan2(x.y, x.x));
}
bool hasTag(const json& o, const std::string& t) {
    for (auto& v : o.value("tags", json::array())) if (v.is_string() && v.get<std::string>() == t) return true;
    return false;
}
// 2D overlap of two boxes (m2) and of the full boxes (m3)
float overlap1(float a0, float a1, float b0, float b1) { return std::max(0.0f, std::min(a1, b1) - std::max(a0, b0)); }
float overlapVolume(const ItemBox& a, const ItemBox& b) {
    return overlap1(a.lo.x, a.hi.x, b.lo.x, b.hi.x) * overlap1(a.lo.y, a.hi.y, b.lo.y, b.hi.y) * overlap1(a.lo.z, a.hi.z, b.lo.z, b.hi.z);
}
float volume(const ItemBox& b) { vec3 s = glm::max(b.hi - b.lo, vec3(0.05f)); return s.x * s.y * s.z; }

// A grid over 2D boxes: which boxes may touch a query rectangle
struct BoxGrid {
    float cell = 16.0f;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    static uint64_t key(int i, int j) { return (uint64_t)(uint32_t)i << 32 | (uint32_t)j; }
    void insert(uint32_t idx, vec2 lo, vec2 hi) {
        for (int i = (int)std::floor(lo.x / cell); i <= (int)std::floor(hi.x / cell); ++i)
            for (int j = (int)std::floor(lo.y / cell); j <= (int)std::floor(hi.y / cell); ++j) cells[key(i, j)].push_back(idx);
    }
    template <class F> void query(vec2 lo, vec2 hi, F&& f) const {
        std::set<uint32_t> seen;
        for (int i = (int)std::floor(lo.x / cell); i <= (int)std::floor(hi.x / cell); ++i)
            for (int j = (int)std::floor(lo.y / cell); j <= (int)std::floor(hi.y / cell); ++j) {
                auto it = cells.find(key(i, j));
                if (it == cells.end()) continue;
                for (uint32_t k : it->second) if (seen.insert(k).second) f(k);
            }
    }
};

struct PathInfo { std::string id; Area corridor; float width; };
std::vector<PathInfo> pathCorridors(const json& doc, float extra) {
    std::vector<PathInfo> out;
    for (auto& p : doc.value("paths", json::array())) {
        if (!p.contains("points") || p["points"].size() < 2) continue;
        float w = p.value("width_m", 3.0f);
        try {
            out.push_back({p.value("id", "?"), Area::parse(json{{"line", p["points"]}, {"width", w + 2 * extra}}), w});
        } catch (const Error&) {}
    }
    return out;
}

// footprint sample points of a box (corners inset a little, edge midpoints, centre)
std::vector<vec2> footprintSamples(const ItemBox& b, float inset = 0.1f) {
    vec2 lo(b.lo), hi(b.hi), d = (hi - lo) * inset;
    lo += d; hi -= d;
    vec2 c = (lo + hi) * 0.5f;
    return {c, lo, {hi.x, lo.y}, hi, {lo.x, hi.y}, {c.x, lo.y}, {c.x, hi.y}, {lo.x, c.y}, {hi.x, c.y}};
}
const char* sideName(vec2 d) {
    if (std::abs(d.x) > std::abs(d.y)) return d.x > 0 ? "east" : "west";
    return d.y > 0 ? "north" : "south";
}
}  // namespace

// ================================================================== shared helpers
ItemBox objectBox(Editor& E, const json& o) {
    ItemBox b;
    b.id = o.value("id", "");
    b.section = "objects";
    if (!o.contains("mesh")) throw Error("object '" + b.id + "' has no mesh");
    auto asset = E.builder.resolveMesh(E.world, o["mesh"]);
    b.placement = objectPlacement(E.world, o);
    placedBounds(*asset, b.placement, b.lo, b.hi);
    b.yawDeg = o.contains("rotation") ? yawOf(b.placement.rotation) : o.value("yaw_deg", 0.0f);
    b.mesh = o["mesh"].is_string() ? o["mesh"].get<std::string>() : "";
    std::string col = o.value("collision", json("auto")).is_string() ? o.value("collision", json("auto")).get<std::string>() : "custom";
    b.collides = col != "none" && E.builder.defaultCollision(E.world, o["mesh"]).kind != CollisionKind::None;
    if (col != "auto" && col != "none") b.collides = true;
    return b;
}

ItemBox itemBox(Editor& E, const std::string& id) {
    std::string sec;
    json* o = E.world.find(id, &sec);
    if (!o) throw Error("no item with id '" + id + "'");
    if (sec == "objects") return objectBox(E, *o);
    if (sec == "entities" || sec == "lights") {
        ItemBox b;
        b.id = id;
        b.section = sec;
        b.collides = false;
        const json& p = o->value("position", json::array({0, 0}));
        vec2 q = xy(p);
        float g = E.world.terrain.empty() ? 0.0f : E.world.terrain.heightAt(q.x, q.y);
        float z = p.size() >= 3 ? p[2].get<float>() : g + (sec == "lights" ? o->value("offset_z", 2.0f) : o->value("hover_m", 0.0f));
        b.placement.position = vec3(q, z);
        b.lo = b.placement.position - vec3(0.25f);
        b.hi = b.placement.position + vec3(0.25f);
        b.yawDeg = o->value("yaw_deg", 0.0f);
        return b;
    }
    throw Error("'" + id + "' is in " + sec + ", not an object or entity");
}

std::vector<ItemBox> allObjectBoxes(Editor& E) {
    std::vector<ItemBox> out;
    json lib = E.builder.catalog(E.world)["meshes"];
    for (const json& o : E.world.doc.value("objects", json::array())) {
        if (o.value("hidden", false)) continue;
        try {
            ItemBox b = objectBox(E, o);
            for (auto& t : o.value("tags", json::array())) if (t.is_string()) b.tags.insert(t.get<std::string>());
            if (!b.mesh.empty() && lib.contains(b.mesh)) b.category = lib[b.mesh].value("category", "");
            else if (b.mesh.empty()) b.category = "primitive";
            out.push_back(std::move(b));
        } catch (const Error&) {}
    }
    return out;
}

void applyRelativePlacement(Editor& E, json& o) {
    if (!o.contains("place")) return;
    json pl = o["place"];
    o.erase("place");
    if (!pl.is_object()) throw Error("place must be an object: {on}, {next_to, side} or {relative_to, offset}");
    auto ground = [&](vec2 p) { return E.world.terrain.empty() ? 0.0f : E.world.terrain.heightAt(p.x, p.y); };
    if (pl.contains("on")) {
        ItemBox t = itemBox(E, pl["on"].get<std::string>());
        vec2 at = pl.contains("at") ? xy(pl["at"]) : vec2(t.center());
        float z = t.hi.z;
        // the real top surface under that point, if the target is already built
        RayHit h = E.physics.raycast(vec3(at, t.hi.z + 1.0f), vec3(0, 0, -1), t.hi.z - t.lo.z + 2.0f);
        if (h.hit && h.instance != RayHit::kTerrain)
            for (auto& s : E.scene.sets)
                if (s.name == t.id && h.instance >= s.first && h.instance < s.first + s.count) z = h.position.z;
        o["position"] = {rnd(at.x, 100), rnd(at.y, 100), rnd(z + pl.value("gap_m", 0.0f), 100)};
        return;
    }
    if (pl.contains("next_to")) {
        ItemBox t = itemBox(E, pl["next_to"].get<std::string>());
        std::string side = pl.value("side", "east");
        vec2 dir = side == "north" || side == "n" ? vec2(0, 1) : side == "south" || side == "s" ? vec2(0, -1)
                 : side == "west" || side == "w" ? vec2(-1, 0) : side == "east" || side == "e" ? vec2(1, 0) : vec2(0);
        if (dir == vec2(0)) throw Error("place.side must be north, south, east or west");
        // the new object's own extent around its origin, with its rotation and scale
        json probe = o;
        probe["position"] = {0.0f, 0.0f, 0.0f};
        ItemBox me = objectBox(E, probe);
        float gap = pl.value("gap_m", 0.3f), along = pl.value("offset_m", 0.0f);
        vec2 c(t.center());
        vec2 p;
        if (dir.x != 0) {
            float edge = dir.x > 0 ? t.hi.x : t.lo.x;
            float half = dir.x > 0 ? -me.lo.x : me.hi.x;     // distance from the new object's origin to its facing side
            p = vec2(edge + dir.x * (gap + half), c.y + along);
        } else {
            float edge = dir.y > 0 ? t.hi.y : t.lo.y;
            float half = dir.y > 0 ? -me.lo.y : me.hi.y;
            p = vec2(c.x + along, edge + dir.y * (gap + half));
        }
        o["position"] = {rnd(p.x, 100), rnd(p.y, 100)};
        return;
    }
    if (pl.contains("relative_to")) {
        ItemBox t = itemBox(E, pl["relative_to"].get<std::string>());
        const json& off = pl.value("offset", json::array({0, 0}));
        vec2 d = xy(off);
        float a = glm::radians(t.yawDeg);
        vec2 w(d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a));
        vec2 p = vec2(t.placement.position) + w;
        if (off.size() >= 3) o["position"] = {rnd(p.x, 100), rnd(p.y, 100), rnd(t.placement.position.z + off[2].get<float>(), 100)};
        else o["position"] = {rnd(p.x, 100), rnd(p.y, 100)};
        if (pl.value("match_yaw", true) && !o.contains("rotation")) o["yaw_deg"] = rnd(o.value("yaw_deg", 0.0f) + t.yawDeg, 100);
        (void)ground;
        return;
    }
    throw Error("place needs on, next_to or relative_to");
}

void rotateItemAbout(json& o, vec2 pivot, float degrees) {
    float a = glm::radians(degrees);
    if (o.contains("position")) {
        json& p = o["position"];
        vec2 d = xy(p) - pivot;
        vec2 q = pivot + vec2(d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a));
        p[0] = rnd(q.x, 1000);
        p[1] = rnd(q.y, 1000);
    }
    if (o.contains("rotation")) {
        const json& r = o["rotation"];
        quat q = glm::angleAxis(a, vec3(0, 0, 1)) * glm::normalize(quat(r[3].get<float>(), r[0].get<float>(), r[1].get<float>(), r[2].get<float>()));
        o["rotation"] = {q.x, q.y, q.z, q.w};
    } else {
        float y = std::fmod(o.value("yaw_deg", 0.0f) + degrees, 360.0f);
        o["yaw_deg"] = rnd(y > 180.0f ? y - 360.0f : y < -180.0f ? y + 360.0f : y, 100);
    }
}

namespace {
void instanceBox(const Scene& sc, uint32_t k, vec3& lo, vec3& hi) {
    const GpuInstance& in = sc.instances[k];
    const Mesh& m = sc.meshes[in.mesh];
    MeshAsset probe;
    probe.aabbMin = m.aabbMin;
    probe.aabbMax = m.aabbMax;
    placedBounds(probe, Placement{vec3(in.posScale), quat(in.rot.w, in.rot.x, in.rot.y, in.rot.z), sc.axisScale(in)}, lo, hi);
}
}  // namespace

bool Editor::selectionBox(const Selected& s, vec3& lo, vec3& hi) {
    if (s.instance != UINT32_MAX && s.instance < scene.instances.size()) {
        instanceBox(scene, s.instance, lo, hi);
        return true;
    }
    try {
        ItemBox b = itemBox(*this, s.id);
        lo = b.lo;
        hi = b.hi;
        return true;
    } catch (const Error&) {}
    // scatter rules, instance files, glTF scenes: the union of their instances
    lo = vec3(1e30f);
    hi = vec3(-1e30f);
    for (auto& set : scene.sets) {
        if (set.name != s.id) continue;
        for (uint32_t k = set.first; k < set.first + std::min(set.count, 4000u) && k < scene.instances.size(); ++k) {
            vec3 a, b;
            instanceBox(scene, k, a, b);
            lo = glm::min(lo, a);
            hi = glm::max(hi, b);
        }
    }
    return lo.x <= hi.x;
}

// ================================================================== tools
void Editor::registerWorldTools() {
    Editor& E = *this;

    // ------------------------------------------------------------------ shared selection
    addTool({"editor_state",
             "What the human sees and has selected in the editor window: when they say \"this\", \"that one\" or \"here\", "
             "call this. selection: items they clicked (id, section, mesh, box, the point clicked; one instance of a scatter "
             "rule or instance file), camera (position, the point looked at), cursor_ground (the surface under the mouse), playing.",
             ToolCategory::Read, object({}),
             [&E](const json&) {
                 json sel = json::array();
                 for (auto& s : E.selection) {
                     json j = {{"id", s.id}, {"picked_point", r3(s.point)}};
                     std::string sec;
                     if (json* o = E.world.find(s.id, &sec)) {
                         j["section"] = sec;
                         if (o->contains("mesh")) j["mesh"] = (*o)["mesh"];
                         if (o->contains("position")) j["position"] = (*o)["position"];
                         if (o->contains("tags")) j["tags"] = (*o)["tags"];
                     }
                     if (s.instance != UINT32_MAX) j["instance"] = s.instance;
                     vec3 lo, hi;
                     if (E.selectionBox(s, lo, hi)) j["box"] = {{"min", r3(lo)}, {"max", r3(hi)}, {"size_m", r3(hi - lo)}};
                     sel.push_back(j);
                 }
                 const Camera& c = E.editCamera;
                 RayHit h = E.physics.raycast(c.position, c.forward(), 20000.0f);
                 vec3 look = h.hit ? h.position : c.position + c.forward() * 50.0f;
                 ToolResult r;
                 r.data = {{"selection", sel}, {"playing", E.playing()},
                           {"camera", {{"position", r3(c.position)}, {"looking_at", r3(look)}, {"vfov_deg", rnd(glm::degrees(c.vfov))}}},
                           {"cursor_ground", E.hasCursorGround ? r3(E.cursorGround) : json()}};
                 if (sel.empty()) r.data["hint"] = "nothing selected: the human selects by clicking in the editor window (shift adds)";
                 return r;
             }});

    addTool({"editor_select",
             "Show the human what you mean: highlight items in the editor window (ids; clear: true removes the highlight) and "
             "with frame: true move the editor camera to look at them. click: [u, v] (0..1 across a capture of {editor: true}, "
             "from the top-left) selects what is there, as a mouse click would. Ask \"this one?\" before big changes to something "
             "they pointed at.",
             ToolCategory::Meta,
             object({{"ids", arr(str(""), "items to highlight")}, {"clear", boolean("remove the highlight")},
                     {"frame", boolean("move the editor camera to them")}, {"click", point("[u, v] in the editor view, 0..1")}}),
             [&E](const json& a) {
                 if (a.value("clear", false)) E.selection.clear();
                 std::vector<Editor::Selected> add;
                 for (auto& i : a.value("ids", json::array())) {
                     std::string id = i.get<std::string>();
                     bool known = E.world.find(id) != nullptr;
                     for (auto& s : E.scene.sets) known = known || s.name == id;
                     if (!known) throw Error("no item with id '" + id + "'");
                     add.push_back({id, UINT32_MAX, vec3(0)});
                 }
                 if (!add.empty()) E.selection = add;
                 if (a.contains("click")) {
                     vec2 uv = xy(a["click"]);
                     E.pick(vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f), !E.selection.empty() && !a.value("clear", false));
                 }
                 vec3 lo(1e30f), hi(-1e30f);
                 for (auto& s : E.selection) {
                     vec3 a0, b0;
                     if (!E.selectionBox(s, a0, b0)) continue;
                     lo = glm::min(lo, a0);
                     hi = glm::max(hi, b0);
                     if (s.point == vec3(0)) s.point = (a0 + b0) * 0.5f;
                 }
                 ToolResult r;
                 if (a.value("frame", false) && lo.x <= hi.x) {
                     vec3 c = (lo + hi) * 0.5f;
                     float radius = std::max(2.0f, glm::length(hi - lo) * 0.5f);
                     float dist = radius / std::tan(E.editCamera.vfov * 0.5f) * 1.15f;
                     E.editCamera.position = c + glm::normalize(vec3(-0.55f, -0.75f, 0.5f)) * dist;
                     E.editCamera.lookAt(c);
                     r.data["camera"] = {{"position", r3(E.editCamera.position)}, {"target", r3(c)}};
                 }
                 json ids = json::array();
                 for (auto& s : E.selection) ids.push_back(s.id);
                 r.data["selected"] = ids;
                 return r;
             }});

    // ------------------------------------------------------------------ world_check
    addTool({"world_check",
             "Find problems captures miss, with ids and fixes: floating or buried objects, bases hanging over slopes, "
             "duplicates and heavy overlaps, objects blocking paths, entities and the player start inside objects or underground, "
             "objects outside the terrain or under water, scatter rules that placed nothing, missing meshes / materials. "
             "area (only there), min_severity error|warning|info (default warning), limit (default 60). Objects tagged floating "
             "may float (sky islands, birds); tagged no_check are skipped.",
             ToolCategory::Read,
             object({{"area", areaSchema("only check here")}, {"min_severity", str("error|warning|info (default warning)")},
                     {"limit", integer("max issues (default 60)")}}),
             [&E](const json& a) {
                 Area area = a.contains("area") ? Area::parse(a["area"]) : Area();
                 std::string minSev = a.value("min_severity", "warning");
                 int minRank = minSev == "error" ? 2 : minSev == "info" ? 0 : 1;
                 size_t limit = a.value("limit", 60u);
                 const json& doc = E.world.doc;
                 const Terrain& T = E.world.terrain;
                 auto ground = [&](vec2 p) { return T.empty() ? 0.0f : T.heightAt(p.x, p.y); };
                 float water = E.scene.env.waterLevel;
                 std::vector<json> issues;
                 std::map<std::string, int> counts;
                 auto add = [&](const char* sev, const char* kind, json ids, vec3 at, std::string msg, std::string fix) {
                     int rank = std::string(sev) == "error" ? 2 : std::string(sev) == "warning" ? 1 : 0;
                     counts[sev]++;
                     if (rank < minRank) return;
                     json j = {{"severity", sev}, {"kind", kind}, {"ids", ids}, {"position", r3(at)}, {"message", msg}};
                     if (!fix.empty()) j["fix"] = fix;
                     issues.push_back(j);
                 };
                 auto inArea = [&](vec2 p) { return area.kind == Area::Kind::All || area.contains(p); };

                 std::vector<ItemBox> boxes = allObjectBoxes(E);
                 BoxGrid grid;
                 for (uint32_t i = 0; i < boxes.size(); ++i) grid.insert(i, vec2(boxes[i].lo), vec2(boxes[i].hi));
                 auto paths = pathCorridors(doc, 0.0f);
                 static const std::set<std::string> loose = {"trees", "rocks", "grass", "bushes", "ferns", "marsh", "characters"};

                 for (uint32_t i = 0; i < boxes.size(); ++i) {
                     const ItemBox& b = boxes[i];
                     vec2 c(b.center());
                     if (!inArea(c) || b.tags.count("no_check")) continue;
                     vec3 at(c, b.lo.z);
                     vec3 size = b.hi - b.lo;
                     if (!T.empty() && !T.inside(c)) {
                         add("warning", "outside_terrain", {b.id}, at, std::format("'{}' stands outside the terrain", b.id),
                             "move it inside the terrain or delete it");
                         continue;
                     }
                     // ground contact: the bottom against the top surface under each footprint sample
                     auto samples = footprintSamples(b);
                     float gMin = 1e30f, gMax = -1e30f, worstGap = 0, minGap = 1e30f;
                     vec2 worstAt = c;
                     for (vec2 s : samples) {
                         float g = ground(s);
                         RayHit h = E.physics.groundBelow(s.x, s.y, b.lo.z + 0.05f);
                         float surf = g;
                         if (h.hit) {
                             bool self = false;
                             for (auto& st : E.scene.sets)
                                 if (st.name == b.id && h.instance >= st.first && h.instance < st.first + st.count) self = true;
                             if (!self) surf = std::max(g, h.position.z);
                         }
                         gMin = std::min(gMin, g);
                         gMax = std::max(gMax, g);
                         float gap = b.lo.z - surf;
                         minGap = std::min(minGap, gap);
                         if (gap > worstGap) { worstGap = gap; worstAt = s; }
                     }
                     bool groundObject = !loose.count(b.category) || b.category.empty();
                     // resting on another object (a bridge on two towers, a crate on a table): its bottom meets the other's
                     // top or sits inside its height range where their footprints overlap
                     bool supported = false;
                     grid.query(vec2(b.lo), vec2(b.hi), [&](uint32_t k) {
                         const ItemBox& o = boxes[k];
                         if (k == i || supported || !o.collides) return;
                         if (overlap1(b.lo.x, b.hi.x, o.lo.x, o.hi.x) <= 0 || overlap1(b.lo.y, b.hi.y, o.lo.y, o.hi.y) <= 0) return;
                         if (b.lo.z >= o.lo.z - 0.05f && b.lo.z <= o.hi.z + 0.35f) supported = true;
                     });
                     bool huge = size.x > 40.0f || size.y > 40.0f;   // terrain-like structures (cliffs, a whole castle)
                     if (b.tags.count("floating") || supported) {
                     } else if (minGap > 0.3f) {
                         add(minGap > 1.5f ? "error" : "warning", "floating", {b.id}, at,
                             std::format("'{}' floats {:.1f} m above what is below it", b.id, minGap),
                             "put it on the ground (position [x, y]) or on another object (object_update set place {on: id})");
                     } else if (groundObject && !huge && worstGap > 0.5f && size.x * size.y > 4.0f) {
                         add("warning", "base_over_slope", {b.id}, vec3(worstAt, b.lo.z),
                             std::format("'{}': its base hangs {:.1f} m above the ground on the {} side ({:.1f} m height change under it)",
                                         b.id, worstGap, sideName(worstAt - c), gMax - gMin),
                             std::format("terrain_sculpt flatten {{rect: {{center: [{:.0f}, {:.0f}], size: [{:.0f}, {:.0f}]}}, falloff: 4}}, "
                                         "or sink it with offset_z", c.x, c.y, size.x + 2, size.y + 2));
                     }
                     float gMean = 0;
                     for (vec2 s : samples) gMean += ground(s);
                     gMean /= (float)samples.size();
                     if (b.hi.z < gMin - 0.05f) {
                         add("error", "buried", {b.id}, at, std::format("'{}' is completely under the ground", b.id), "raise it or delete it");
                     } else if (size.z > 0.5f && gMax - gMin < size.z * 0.5f && (gMean - b.lo.z) / size.z > 0.6f && groundObject && !huge) {
                         add("warning", "mostly_buried", {b.id}, at,
                             std::format("'{}' is {:.0f}% below the ground", b.id, 100.0f * (gMean - b.lo.z) / size.z), "raise it (offset_z)");
                     }
                     if (water > -999.0f && b.hi.z < water) add("info", "under_water", {b.id}, at, std::format("'{}' is under water", b.id), "");
                     // overlaps with later boxes (each pair once)
                     grid.query(vec2(b.lo), vec2(b.hi), [&](uint32_t k) {
                         if (k <= i) return;
                         const ItemBox& o = boxes[k];
                         if (!b.collides || !o.collides) return;
                         float v = overlapVolume(b, o);
                         if (v <= 0) return;
                         float frac = v / std::min(volume(b), volume(o));
                         bool same = b.mesh == o.mesh && glm::length(b.placement.position - o.placement.position) < 0.15f;
                         if (same) {
                             add("error", "duplicate", {b.id, o.id}, at, std::format("'{}' and '{}' are the same mesh in the same place", b.id, o.id),
                                 "delete one of them");
                             return;
                         }
                         // the smaller one entirely inside the other's box with its centre in the other's solid geometry
                         const ItemBox& in = volume(b) <= volume(o) ? b : o;
                         const ItemBox& out = &in == &b ? o : b;
                         bool contained = glm::all(glm::greaterThanEqual(in.lo, out.lo - vec3(0.05f))) &&
                                          glm::all(glm::lessThanEqual(in.hi, out.hi + vec3(0.05f)));
                         uint32_t hitInst = UINT32_MAX;
                         bool solid = contained && E.physics.pointInside(in.center(), &hitInst);
                         std::string hitId;
                         for (auto& st : E.scene.sets) if (hitInst >= st.first && hitInst < st.first + st.count) hitId = st.name;
                         if (solid && hitId == out.id)
                             add("warning", "embedded", {in.id, out.id}, in.center(),
                                 std::format("'{}' is sunk inside the solid geometry of '{}'", in.id, out.id), "move it out or delete it");
                         else if (frac > 0.2f)
                             add("info", "overlap", {b.id, o.id}, at,
                                 std::format("'{}' and '{}' overlap by {:.0f}% of the smaller box (fine if joined on purpose)", b.id, o.id, frac * 100), "");
                     });
                     // paths: a solid object whose footprint reaches into the path's driving width
                     if (b.collides && size.z > 0.4f)
                         for (auto& p : paths) {
                             int hits = 0;
                             for (vec2 s : footprintSamples(b, 0.15f)) if (p.corridor.distance(s) < -0.3f) ++hits;
                             if (hits)
                                 add("warning", "blocks_path", {b.id, p.id}, at, std::format("'{}' stands on path '{}'", b.id, p.id),
                                     "move it off the path (object_update move_by) or reroute the path");
                         }
                 }
                 // entities, lights, player start
                 auto insideObject = [&](vec3 p, std::string& who) {
                     uint32_t inst = UINT32_MAX;
                     if (!E.physics.pointInside(p, &inst)) return false;
                     who = "?";
                     for (auto& s : E.scene.sets)
                         if (inst >= s.first && inst < s.first + s.count) { who = s.name; break; }
                     return true;
                 };
                 for (const json& e : doc.value("entities", json::array())) {
                     std::string id = e.value("id", "?");
                     ItemBox b;
                     try { b = itemBox(E, id); } catch (const Error&) { continue; }
                     vec3 p = b.placement.position;
                     if (!inArea(vec2(p))) continue;
                     std::string who;
                     float g = ground(vec2(p));
                     if (p.z < g - 0.2f) add("error", "entity_underground", {id}, p, std::format("entity '{}' is under the ground", id),
                                             "use a 2D position [x, y]");
                     else if (insideObject(p + vec3(0, 0, 0.3f), who))
                         add("warning", "entity_inside_object", {id, who}, p, std::format("entity '{}' is inside '{}'", id, who),
                             "move it out (object_update move_by) or onto it");
                     RayHit h = E.physics.groundBelow(p.x, p.y, p.z);
                     float drop = p.z - (h.hit ? h.position.z : g);
                     if (e.value("type", "") == "collectible" && drop > 3.5f)
                         add("warning", "entity_out_of_reach", {id}, p, std::format("collectible '{}' hovers {:.1f} m above the surface", id, drop),
                             "lower it (hover_m) or check the jump with play_sim");
                 }
                 for (const json& l : doc.value("lights", json::array())) {
                     std::string id = l.value("id", "?");
                     ItemBox b;
                     try { b = itemBox(E, id); } catch (const Error&) { continue; }
                     vec3 p = b.placement.position;
                     if (inArea(vec2(p)) && p.z < ground(vec2(p)) - 0.05f)
                         add("warning", "light_underground", {id}, p, std::format("light '{}' is under the ground", id), "raise it (offset_z)");
                 }
                 if (doc.contains("player_start")) {
                     vec3 p = E.scene.playerStart.position;
                     std::string who;
                     if (inArea(vec2(p))) {
                         if (!T.empty() && !T.inside(vec2(p))) add("error", "player_start", json::array(), p, "the player start is outside the terrain", "player_set start");
                         if (insideObject(p + vec3(0, 0, 0.9f), who))
                             add("error", "player_start", {who}, p, std::format("the player start is inside '{}'", who), "player_set start {position}");
                         if (water > -999.0f && ground(vec2(p)) < water - 0.5f)
                             add("warning", "player_start", json::array(), p, "the player start is in deep water", "player_set start {position}");
                         if (!T.empty() && T.slopeDegAt(p.x, p.y) > 40.0f)
                             add("warning", "player_start", json::array(), p, "the player start is on a slope too steep to stand on", "player_set start");
                     }
                 }
                 // scatter rules that place nothing
                 for (const json& sr : doc.value("scatter", json::array())) {
                     std::string id = sr.value("id", "?");
                     if (sr.value("hidden", false)) continue;
                     size_t n = 0;
                     for (auto& s : E.scene.sets) if (s.name == id) n += s.count;
                     if (n == 0) add("warning", "scatter_empty", {id}, vec3(0), std::format("scatter rule '{}' placed nothing", id),
                                     "loosen slope_deg / height_m / avoid_* or check its area");
                     else if (sr.contains("max_instances") && n >= sr["max_instances"].get<size_t>())
                         add("info", "scatter_capped", {id}, vec3(0), std::format("scatter rule '{}' hit max_instances ({})", id, n),
                             "raise max_instances or lower the density");
                 }
                 for (auto& w : E.lastBuild.warnings) add("error", "build", json::array(), vec3(0), w, "");

                 std::stable_sort(issues.begin(), issues.end(), [](const json& x, const json& y) {
                     auto rank = [](const json& j) { std::string s = j["severity"]; return s == "error" ? 0 : s == "warning" ? 1 : 2; };
                     return rank(x) < rank(y);
                 });
                 size_t total = issues.size();
                 if (issues.size() > limit) issues.resize(limit);
                 ToolResult r;
                 r.data = {{"issues", issues}, {"shown", issues.size()}, {"matching", total},
                           {"counts", {{"error", counts["error"]}, {"warning", counts["warning"]}, {"info", counts["info"]}}},
                           {"checked", {{"objects", boxes.size()}, {"paths", paths.size()},
                                        {"entities", doc.value("entities", json::array()).size()}}}};
                 if (total == 0) r.data["summary"] = "no problems found at this severity; still look at captures";
                 return r;
             }});

    // ------------------------------------------------------------------ find_space
    addTool({"find_space",
             "Find free, level building sites: size [w, d] footprint (m), optional yaw_deg; inside area, or near [x,y] within "
             "radius_m (default 60). Filters: max_slope_deg (10), max_height_diff_m (1.0 across the footprint), clearance_m from "
             "objects and trees (2), path_clearance_m (1.5), near_path {id (any), max_m}, avoid_water (true). Returns up to count "
             "(5) sites, best first, with ground height, flatness, the nearest object and path and a yaw that faces the path.",
             ToolCategory::Read,
             object({{"size", point("[w, d] footprint in metres")}, {"area", areaSchema("search here")}, {"near", point("[x, y]")},
                     {"radius_m", num("search radius around near (default 60)")}, {"yaw_deg", num("footprint rotation (default 0)")},
                     {"max_slope_deg", num("default 10")}, {"max_height_diff_m", num("default 1.0")}, {"clearance_m", num("default 2")},
                     {"path_clearance_m", num("default 1.5")}, {"near_path", anyObj("{id, max_m}: within max_m of a path")},
                     {"avoid_water", boolean("default true")}, {"count", integer("default 5")}},
                    {"size"}),
             [&E](const json& a) {
                 const Terrain& T = E.world.terrain;
                 if (T.empty()) throw Error("the map has no terrain yet");
                 vec2 size = xy(a.at("size"));
                 if (size.x <= 0 || size.y <= 0) throw Error("size must be positive [w, d]");
                 float yaw = glm::radians(a.value("yaw_deg", 0.0f));
                 vec2 ax(std::cos(yaw), std::sin(yaw)), ay(-std::sin(yaw), std::cos(yaw));
                 Area area;
                 bool hasNear = a.contains("near");
                 vec2 nearP = hasNear ? xy(a["near"]) : vec2(0);
                 float radius = a.value("radius_m", 60.0f);
                 if (a.contains("area")) area = Area::parse(a["area"]);
                 else if (hasNear) area = Area::parse(json{{"circle", {{"center", {nearP.x, nearP.y}}, {"radius", radius}}}});
                 float maxSlope = a.value("max_slope_deg", 10.0f), maxDiff = a.value("max_height_diff_m", 1.0f);
                 float clearance = a.value("clearance_m", 2.0f), pathClear = a.value("path_clearance_m", 1.5f);
                 bool avoidWater = a.value("avoid_water", true);
                 size_t count = std::clamp<size_t>(a.value("count", 5u), 1, 30);
                 float water = E.scene.env.waterLevel;

                 // obstacles: boxes of colliding objects, of every colliding instance placed by scatter rules and instance
                 // files (trees, rocks), and for huge merged meshes (all of a meadow's props in one object, whose box
                 // covers everything) the real surface under the footprint, found with rays
                 std::vector<ItemBox> boxes = allObjectBoxes(E);
                 struct Ob { vec2 lo, hi; std::string id; };
                 std::vector<Ob> obs;
                 std::set<std::string> objectIds, hugeIds;
                 for (auto& b : boxes) {
                     objectIds.insert(b.id);
                     vec3 sz = b.hi - b.lo;
                     if (!b.collides) continue;                          // grass clumps, backdrops (a distant mountain ring)
                     if (sz.z < 0.5f && sz.x * sz.y > 25.0f) continue;   // paving, platforms: ground covers, not obstacles
                     if (sz.x > 40.0f || sz.y > 40.0f) { hugeIds.insert(b.id); continue; }
                     obs.push_back({vec2(b.lo), vec2(b.hi), b.id});
                 }
                 for (auto& s : E.scene.sets) {
                     if (s.collision.kind == CollisionKind::None || objectIds.count(s.name)) continue;
                     for (uint32_t k = s.first; k < s.first + s.count && k < E.scene.instances.size(); ++k) {
                         const GpuInstance& in = E.scene.instances[k];
                         vec2 p(in.posScale);
                         if (s.collision.kind == CollisionKind::Cylinder || s.collision.kind == CollisionKind::Sphere) {
                             float r = std::max(0.5f, s.collision.radius * in.posScale.w);
                             obs.push_back({p - vec2(r), p + vec2(r), s.name});
                         } else {
                             const Mesh& m = E.scene.meshes[in.mesh];
                             MeshAsset probe;
                             probe.aabbMin = m.aabbMin;
                             probe.aabbMax = m.aabbMax;
                             Placement pl{vec3(in.posScale), quat(in.rot.w, in.rot.x, in.rot.y, in.rot.z), E.scene.axisScale(in)};
                             vec3 lo, hi;
                             placedBounds(probe, pl, lo, hi);
                             if (hi.x - lo.x > 40.0f || hi.y - lo.y > 40.0f) { hugeIds.insert(s.name); continue; }
                             obs.push_back({vec2(lo), vec2(hi), s.name});
                         }
                     }
                 }
                 std::set<uint32_t> hugeInstances;   // instances of huge meshes, recognised in ray hits
                 for (auto& s : E.scene.sets)
                     if (hugeIds.count(s.name)) for (uint32_t k = s.first; k < s.first + s.count; ++k) hugeInstances.insert(k);
                 BoxGrid grid;
                 for (uint32_t i = 0; i < obs.size(); ++i) grid.insert(i, obs[i].lo, obs[i].hi);
                 auto paths = pathCorridors(E.world.doc, pathClear);
                 auto rawPaths = pathCorridors(E.world.doc, 0.0f);
                 const json np = a.value("near_path", json());
                 std::string npId = np.is_object() ? np.value("id", "") : "";
                 float npMax = np.is_object() ? np.value("max_m", 20.0f) : 0.0f;

                 vec2 lo, hi;
                 area.bounds(lo, hi);
                 lo = glm::max(lo, T.origin);
                 hi = glm::min(hi, T.maxCorner());
                 float step = std::max(1.0f, std::min(size.x, size.y) * 0.5f);
                 while (((hi.x - lo.x) / step) * ((hi.y - lo.y) / step) > 60000.0f) step *= 1.5f;
                 vec2 half = size * 0.5f;
                 // footprint sample offsets: corners, edge points every ~2 m, centre
                 std::vector<vec2> offs = {{0, 0}};
                 int nx = std::max(2, (int)std::ceil(size.x / 2.0f)), ny = std::max(2, (int)std::ceil(size.y / 2.0f));
                 for (int i = 0; i <= nx; ++i) for (int j = 0; j <= ny; ++j)
                     if (i == 0 || j == 0 || i == nx || j == ny) offs.push_back(vec2(-half.x + size.x * i / nx, -half.y + size.y * j / ny));
                 std::map<std::string, int> rejected;
                 struct Cand { vec2 p; float score, diff, slope, z; };
                 std::vector<Cand> cands;
                 size_t tested = 0;
                 for (float y = lo.y + half.y; y <= hi.y - half.y + 1e-3f; y += step)
                     for (float x = lo.x + half.x; x <= hi.x - half.x + 1e-3f; x += step) {
                         vec2 c(x, y);
                         ++tested;
                         float zMin = 1e30f, zMax = -1e30f, zSum = 0;
                         bool ok = true;
                         for (vec2 o : offs) {
                             vec2 w = c + ax * o.x + ay * o.y;
                             if (!T.inside(w) || (area.kind != Area::Kind::All && !area.contains(w))) { ok = false; break; }
                             float z = T.heightAt(w.x, w.y);
                             zMin = std::min(zMin, z); zMax = std::max(zMax, z); zSum += z;
                         }
                         if (!ok) { rejected["outside the area"]++; continue; }
                         float diff = zMax - zMin, slope = T.slopeDegAt(c.x, c.y);
                         if (diff > maxDiff) { rejected["uneven"]++; continue; }
                         if (slope > maxSlope) { rejected["too steep"]++; continue; }
                         if (avoidWater && water > -999.0f && zMin < water + 0.3f) { rejected["water"]++; continue; }
                         // obstacles: the footprint's bounding rectangle grown by the clearance
                         vec2 ext = glm::abs(ax) * half.x + glm::abs(ay) * half.y + vec2(clearance);
                         bool blocked = false;
                         grid.query(c - ext, c + ext, [&](uint32_t k) {
                             if (blocked) return;
                             const Ob& o = obs[k];
                             if (o.hi.x > c.x - ext.x && o.lo.x < c.x + ext.x && o.hi.y > c.y - ext.y && o.lo.y < c.y + ext.y) blocked = true;
                         });
                         if (!blocked && !hugeInstances.empty())
                             for (float u = -ext.x; u <= ext.x + 1e-3f && !blocked; u += 1.0f)
                                 for (float v = -ext.y; v <= ext.y + 1e-3f && !blocked; v += 1.0f) {
                                     RayHit h = E.physics.groundBelow(c.x + u, c.y + v);
                                     if (h.hit && hugeInstances.count(h.instance) && h.position.z > T.heightAt(c.x + u, c.y + v) + 0.3f) blocked = true;
                                 }
                         if (blocked) { rejected["objects or trees"]++; continue; }
                         bool onPath = false;
                         for (auto& p : paths) {
                             for (vec2 o : offs) if (p.corridor.contains(c + ax * o.x + ay * o.y)) { onPath = true; break; }
                             if (onPath) break;
                         }
                         if (onPath) { rejected["path"]++; continue; }
                         float pathDist = 1e30f;
                         for (auto& p : rawPaths) if (npId.empty() || p.id == npId) pathDist = std::min(pathDist, p.corridor.distance(c));
                         if (np.is_object() && pathDist > npMax) { rejected["too far from the path"]++; continue; }
                         float score = diff * 2.0f + slope * 0.05f;
                         if (hasNear) score += glm::length(c - nearP) / std::max(radius, 1.0f);
                         if (np.is_object()) score += pathDist / std::max(npMax, 1.0f);
                         cands.push_back({c, score, diff, slope, zSum / offs.size()});
                     }
                 std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score < y.score; });
                 float sep = std::max(size.x, size.y) + clearance;
                 std::vector<Cand> picked;
                 for (auto& c : cands) {
                     bool far = true;
                     for (auto& p : picked) if (glm::length(p.p - c.p) < sep) { far = false; break; }
                     if (far) picked.push_back(c);
                     if (picked.size() >= count) break;
                 }
                 json sites = json::array();
                 for (auto& c : picked) {
                     json s = {{"position", r2(c.p)}, {"ground_z", rnd(c.z, 100)}, {"height_diff_m", rnd(c.diff, 100)}, {"slope_deg", rnd(c.slope)}};
                     float best = 1e30f;
                     std::string bestId;
                     for (auto& b : boxes) {
                         if (!b.collides || hugeIds.count(b.id)) continue;
                         vec2 d = glm::max(glm::max(vec2(b.lo) - c.p, c.p - vec2(b.hi)), vec2(0));
                         if (glm::length(d) < best) { best = glm::length(d); bestId = b.id; }
                     }
                     if (!bestId.empty()) s["nearest_object"] = {{"id", bestId}, {"distance_m", rnd(best)}};
                     float pd = 1e30f;
                     const PathInfo* pp = nullptr;
                     for (auto& p : rawPaths) { float d = p.corridor.distance(c.p); if (d < pd) { pd = d; pp = &p; } }
                     if (pp) {
                         // the nearest point on the path: step towards decreasing distance
                         vec2 q = c.p;
                         for (int it = 0; it < 40; ++it) {
                             float e = 0.5f, d0 = pp->corridor.distance(q);
                             vec2 g((pp->corridor.distance(q + vec2(e, 0)) - d0) / e, (pp->corridor.distance(q + vec2(0, e)) - d0) / e);
                             if (glm::length(g) < 1e-4f || d0 <= 0) break;
                             q -= glm::normalize(g) * std::min(d0, 4.0f);
                         }
                         vec2 dir = q - c.p;
                         s["nearest_path"] = {{"id", pp->id}, {"distance_m", rnd(std::max(pd, 0.0f))}};
                         if (glm::length(dir) > 0.1f)   // a mesh's front is -y: yaw so that -y points at the path
                             s["yaw_facing_path_deg"] = rnd(glm::degrees(std::atan2(dir.y, dir.x)) + 90.0f);
                     }
                     if (c.diff > 0.3f)
                         s["level_with"] = std::format("terrain_sculpt flatten {{rect: {{center: [{:.0f}, {:.0f}], size: [{:.0f}, {:.0f}], yaw_deg: {:.0f}}}, "
                                                       "falloff: 4}}", c.p.x, c.p.y, size.x + 2, size.y + 2, glm::degrees(yaw));
                     sites.push_back(s);
                 }
                 ToolResult r;
                 r.data = {{"sites", sites}, {"tested", tested}, {"rejected", rejected}, {"step_m", rnd(step)}};
                 if (sites.empty()) r.data["hint"] = "nothing fits: see rejected for the strictest filter and loosen it, widen the area or level a site";
                 return r;
             }});

    // ------------------------------------------------------------------ named areas
    addTool({"area_set",
             "Name an area (town, forest_north, arena) so any tool argument \"area\" can use the name, and scatter rules follow later "
             "changes to it: name, area (an area object); delete: true removes it. world_get section areas lists them.",
             ToolCategory::Meta,
             object({{"name", str("area name")}, {"area", areaSchema("the region")}, {"delete", boolean("remove the name")}}, {"name"}),
             [&E](const json& a) {
                 std::string name = a.at("name").get<std::string>();
                 if (name.empty() || name == "all") throw Error("pick another name");
                 json& areas = E.world.doc["areas"];
                 if (!areas.is_object()) areas = json::object();
                 ToolResult r;
                 if (a.value("delete", false)) {
                     if (!areas.contains(name)) throw Error("no named area '" + name + "'");
                     for (auto& sr : E.world.doc.value("scatter", json::array()))
                         if (sr.value("area", json()) == json(name)) throw Error("scatter rule '" + sr.value("id", "?") + "' uses this area");
                     E.world.beginEdit("area_set", false);
                     areas.erase(name);
                     E.world.endEdit();
                     r.data = {{"deleted", name}};
                     return r;
                 }
                 if (!a.contains("area")) throw Error("give area (or delete: true)");
                 json def = a["area"];
                 json tmp = areas;
                 tmp[name] = def;
                 Area parsed = Area::parse(Area::expandNamed(def, tmp));   // validates (and catches loops)
                 E.world.beginEdit("area_set", false);
                 areas[name] = def;
                 E.world.endEdit();
                 vec2 lo, hi;
                 parsed.bounds(lo, hi);
                 r.data = {{"name", name}, {"area_m2", std::round(parsed.areaM2())}, {"bounds", {r2(lo), r2(hi)}}};
                 return r;
             }});

    // ------------------------------------------------------------------ duplicate
    addTool({"object_duplicate",
             "Copy objects or entities (ids or tag) count times (default 1): each copy moves by offset [dx,dy(,dz)] and turns by "
             "rotate_by_deg about pivot ([x,y], default the sources' centre) from the previous one: rows, rings, mirrored pairs. "
             "Copies get new ids (id_prefix), the sources' tags plus tags.",
             ToolCategory::Layout,
             object({{"ids", arr(str(""), "sources")}, {"tag", str("sources with this tag")}, {"count", integer("copies (default 1)")},
                     {"offset", point("[dx, dy(, dz)] per copy")}, {"rotate_by_deg", num("per copy, about pivot")}, {"pivot", point("[x, y]")},
                     {"id_prefix", str("")}, {"tags", arr(str(""), "added to the copies")}}),
             [&E](const json& a) {
                 std::vector<json> src;
                 std::vector<std::string> secs;
                 auto take = [&](const json& o, const std::string& sec) { src.push_back(o); secs.push_back(sec); };
                 for (auto& i : a.value("ids", json::array())) {
                     std::string sec;
                     json* o = E.world.find(i.get<std::string>(), &sec);
                     if (!o) throw Error("no item with id '" + i.get<std::string>() + "'");
                     if (sec != "objects" && sec != "entities") throw Error("'" + i.get<std::string>() + "' is in " + sec);
                     take(*o, sec);
                 }
                 if (a.contains("tag"))
                     for (const char* s : {"objects", "entities"})
                         for (json& o : E.world.list(s)) if (hasTag(o, a["tag"].get<std::string>())) take(o, s);
                 if (src.empty()) throw Error("nothing to copy (give ids or tag)");
                 int count = a.value("count", 1);
                 if (count < 1 || count > 500) throw Error("count must be 1..500");
                 vec3 off(0);
                 if (a.contains("offset")) { vec2 d = xy(a["offset"]); off = vec3(d, a["offset"].size() >= 3 ? a["offset"][2].get<float>() : 0.0f); }
                 float rot = a.value("rotate_by_deg", 0.0f);
                 vec2 pivot(0);
                 if (a.contains("pivot")) pivot = xy(a["pivot"]);
                 else { for (auto& o : src) pivot += xy(o.value("position", json::array({0, 0}))); pivot /= (float)src.size(); }
                 E.world.beginEdit("object_duplicate", false);
                 json made = json::array();
                 try {
                     std::vector<json> prev = src;
                     for (int c = 0; c < count; ++c) {
                         vec2 pv = pivot + vec2(off) * (float)c;
                         for (size_t k = 0; k < prev.size(); ++k) {
                             json o = prev[k];
                             json& p = o["position"];
                             p[0] = rnd(p[0].get<float>() + off.x, 1000);
                             p[1] = rnd(p[1].get<float>() + off.y, 1000);
                             if (off.z != 0) {
                                 if (p.size() >= 3) p[2] = p[2].get<float>() + off.z;
                                 else o["offset_z"] = o.value("offset_z", 0.0f) + off.z;
                             }
                             if (rot != 0) rotateItemAbout(o, pv + vec2(off), rot);
                             std::string base = a.value("id_prefix", src[k].value("id", "copy"));
                             o["id"] = E.world.newId(base);
                             for (auto& t : a.value("tags", json::array())) {
                                 if (!o.contains("tags")) o["tags"] = json::array();
                                 if (!hasTag(o, t.get<std::string>())) o["tags"].push_back(t);
                             }
                             E.world.list(secs[k]).push_back(o);
                             made.push_back(o["id"]);
                             prev[k] = o;
                         }
                     }
                 } catch (...) { E.world.cancelEdit(); throw; }
                 E.world.endEdit();
                 ToolResult r;
                 r.data = {{"created", made}};
                 return r;
             }});

    // ------------------------------------------------------------------ world_diff
    addTool({"world_diff",
             "What changed: since batch (default while a batch is open: since batch_begin) or since save (the last save / load). "
             "Lists added, removed and changed ids per section (with the changed fields), changed settings sections, and whether "
             "the terrain was edited. Use it to review a batch before batch_end and to write its notes.",
             ToolCategory::Read, object({{"since", str("batch|save")}}),
             [&E](const json& a) {
                 std::string since = a.value("since", E.batch.open ? "batch" : "save");
                 const json* base = nullptr;
                 uint64_t terrainBase = 0;
                 if (since == "batch") {
                     if (!E.batch.open || E.batch.docAtStart.is_null()) throw Error("no batch is open: use since save");
                     base = &E.batch.docAtStart;
                     terrainBase = E.batch.terrainVersionAtStart;
                 } else if (since == "save") {
                     base = &E.world.savedDoc;
                     terrainBase = E.world.savedTerrainVersion;
                 } else {
                     throw Error("since must be batch or save");
                 }
                 const json& now = E.world.doc;
                 json sections = json::object();
                 size_t total = 0;
                 for (auto& sec : World::idSections()) {
                     const json wasList = base->value(sec, json::array()), isList = now.value(sec, json::array());   // kept alive
                     std::map<std::string, const json*> was, is;
                     for (auto& o : wasList) was[o.value("id", "")] = &o;
                     for (auto& o : isList) is[o.value("id", "")] = &o;
                     json added = json::array(), removed = json::array(), changed = json::array();
                     for (auto& [id, o] : is) {
                         auto it = was.find(id);
                         if (it == was.end()) { added.push_back(id); continue; }
                         if (*it->second == *o) continue;
                         json fields = json::array();
                         std::set<std::string> keys;
                         for (auto& [k, v] : o->items()) keys.insert(k);
                         for (auto& [k, v] : it->second->items()) keys.insert(k);
                         for (auto& k : keys) if (o->value(k, json()) != it->second->value(k, json())) fields.push_back(k);
                         changed.push_back({{"id", id}, {"fields", fields}});
                     }
                     for (auto& [id, o] : was) if (!is.count(id)) removed.push_back(id);
                     if (added.empty() && removed.empty() && changed.empty()) continue;
                     total += added.size() + removed.size() + changed.size();
                     json s = json::object();
                     if (!added.empty()) s["added"] = added;
                     if (!removed.empty()) s["removed"] = removed;
                     if (!changed.empty()) s["changed"] = changed;
                     sections[sec] = s;
                 }
                 json settings = json::array();
                 std::set<std::string> keys;
                 for (auto& [k, v] : now.items()) keys.insert(k);
                 for (auto& [k, v] : base->items()) keys.insert(k);
                 const auto& idSecs = World::idSections();
                 for (auto& k : keys) {
                     if (std::find(idSecs.begin(), idSecs.end(), k) != idSecs.end()) continue;
                     if (now.value(k, json()) != base->value(k, json())) settings.push_back(k);
                 }
                 ToolResult r;
                 r.data = {{"since", since}, {"sections", sections}, {"settings_changed", settings},
                           {"terrain_edited", E.world.terrain.version != terrainBase}, {"items_changed", total}};
                 return r;
             }});
}
}  // namespace df
