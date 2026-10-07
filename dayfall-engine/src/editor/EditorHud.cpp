// HUD glue: game state -> HudState for every frame and capture (Engine::hudBuild),
// and the minimap image: a near-orthographic top-down capture in neutral light
// (high north-west sun, no haze or clouds), made again only when the map's look changes.
#include "core/Log.h"
#include "editor/Editor.h"
#include <algorithm>
#include <chrono>
#include <format>

namespace df {
using json = nlohmann::json;

void Editor::drawHud(HudCanvas& c) {
    if (hudForce == 0 || (hudForce < 0 && !game.active())) return;
    HudConfig cfg;
    try {
        cfg = HudConfig::parse(world.doc.value("hud", json()));
    } catch (const std::exception&) {   // hud_set validates; a bad hand edit falls back to the defaults
    }
    if (!cfg.enabled) return;
    if (cfg.minimap) updateMinimap();
    HudState s;
    if (game.active()) {
        s.player = vec2(game.feet);
        s.facing = game.facing;
        s.viewYaw = game.viewYaw;
        s.time = game.time;
        s.collected = game.collected;
        s.total = game.collectibles;
        s.messages.assign(game.messages.begin(), game.messages.end());
    } else {   // a preview while editing: the player start, nothing collected yet
        s.player = vec2(scene.playerStart.position);
        s.facing = s.viewYaw = glm::radians(scene.playerStart.yawDeg - 90.0f);
        for (auto& e : entities) s.total += e.type == "collectible";
    }
    for (auto& e : entities) {
        bool taken = e.instance < scene.instances.size() && scene.instances[e.instance].cullDistance < 0;
        if (e.type == "collectible" && !taken) s.markers.push_back({vec2(e.position), e.color, 0});
        else if (e.type == "goal") s.markers.push_back({vec2(e.position), e.color, 1});
        else if (e.type == "waypoint") s.markers.push_back({vec2(e.position), vec3(1), 2});
    }
    if (!cfg.route.empty()) {
        const json& routes = world.doc.value("routes", json::object());
        if (routes.contains(cfg.route))
            for (auto& p : routes[cfg.route].value("points", json::array()))
                if (p.is_array() && p.size() >= 2) s.route.push_back({p[0].get<float>(), p[1].get<float>()});
    }
    s.mapOrigin = minimapOrigin_;
    s.mapSize = minimapSize_;
    df::drawHud(c, cfg, s);
}

void Editor::updateMinimap() {
    // only when the scene was rebuilt, and then only if something visible from above changed
    uint64_t built = builtVersion_ * 0x9E3779B97F4A7C15ull ^ builtTerrainVersion_;
    if (built == minimapBuilt_) return;
    minimapBuilt_ = built;
    json look = world.doc;
    for (const char* k : {"hud", "cameras", "routes", "player", "player_start", "entities", "name"}) look.erase(k);
    if (look.contains("environment")) look["environment"] = look["environment"].value("water", json());
    uint64_t key = std::hash<std::string>{}(look.dump()) * 31 + world.terrain.version;
    if (key == minimapKey_) return;
    minimapKey_ = key;
    auto t0 = std::chrono::steady_clock::now();
    // the area: the terrain, else the scene's bounds
    vec2 lo, hi;
    float zMid;
    if (!world.terrain.empty()) {
        lo = vec2(scene.terrainMin);
        hi = vec2(scene.terrainMax);
        zMid = 0.5f * (scene.terrainMin.z + scene.terrainMax.z);
    } else {
        vec2 c = vec2(scene.boundsMin + scene.boundsMax) * 0.5f;
        float half = std::max(scene.boundsMax.x - scene.boundsMin.x, scene.boundsMax.y - scene.boundsMin.y) * 0.55f + 10.0f;
        lo = c - vec2(half);
        hi = c + vec2(half);
        zMid = 0.5f * (scene.boundsMin.z + scene.boundsMax.z);
    }
    float size = std::max(hi.x - lo.x, hi.y - lo.y);
    if (!(size > 1.0f) || (scene.instances.empty() && world.terrain.empty())) { minimapSize_ = 0; return; }
    uint32_t res = (uint32_t)std::clamp(size / 0.5f, 512.0f, 2048.0f);   // 0.5 m per pixel up to 1 km maps
    // a narrow view from far above is close to orthographic (relief shifts by < 0.2 %); the centre ray
    // hits the middle of the map (the pitch is a hair off vertical)
    Camera cam;
    cam.vfov = glm::radians(2.0f);
    float alt = size * 0.5f / std::tan(cam.vfov * 0.5f);
    vec2 centre = (lo + hi) * 0.5f;
    cam.position = vec3(centre, zMid + alt);
    cam.yaw = 0;
    cam.pitch = -1.5703f;   // north at the top
    vec3 fw = cam.forward();
    cam.position -= vec3(vec2(fw) * (alt / -fw.z), 0.0f);
    cam.nearPlane = std::max(1.0f, cam.position.z - scene.boundsMax.z - 5.0f);

    // Instances: the GPU draws those seen from 250 m away (not grass or pebbles), the largest cull
    // distances first, within a triangle budget (small on CPU rasterisers); the rest become dots.
    struct Cand { uint32_t index; float cull; };
    std::vector<Cand> cands;
    std::vector<uint32_t> meshTris(scene.meshes.size(), UINT32_MAX);
    auto tris = [&](uint32_t mi) {   // at the LOD picked from up there
        if (meshTris[mi] == UINT32_MAX) {
            const Mesh& m = scene.meshes[mi];
            meshTris[mi] = 0;
            for (size_t l = 0; l < m.lods.size(); ++l)
                if (m.lods[l].maxDistance * engine->renderer.settings().lodScale >= alt || l + 1 == m.lods.size()) {
                    for (uint32_t sm : m.lods[l].submeshes) meshTris[mi] += scene.submeshes[sm].indexCount / 3;
                    break;
                }
        }
        return meshTris[mi];
    };
    size_t staticEnd = std::min<size_t>(scene.dynamicFirst, scene.instances.size());
    for (uint32_t i = 0; i < staticEnd; ++i)
        if (scene.instances[i].cullDistance >= 250.0f) cands.push_back({i, scene.instances[i].cullDistance});
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cull > b.cull; });
    const double budget = engine->device.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 1.5e6 : 3.0e7;
    double sum = 0;
    float cut = 0;   // > 0: instances with this cull distance or less are not drawn
    for (const Cand& c : cands)
        if ((sum += tris(scene.instances[c.index].mesh)) > budget) { cut = c.cull; break; }
    float distLo = cam.position.z - scene.boundsMax.z;   // no instance is nearer (or farther than distHi)
    float distHi = glm::length(vec3(size, size, cam.position.z - scene.boundsMin.z));
    float cullScale = cut > 0 ? distLo / cut * 0.999f : distHi / 250.0f;
    auto drawn = [&](const GpuInstance& in) {   // the cull shader's test
        const Mesh& m = scene.meshes[in.mesh];
        vec3 c = vec3(in.posScale) + glm::rotate(quat(in.rot.w, in.rot.x, in.rot.y, in.rot.z), vec3(m.bounds) * scene.axisScale(in));
        return std::max(glm::length(c - cam.position) - m.bounds.w * in.posScale.w, 0.0f) <= in.cullDistance * cullScale;
    };

    // neutral, readable light; pickups and the player hidden (the HUD draws markers)
    Environment keepEnv = scene.env;
    Environment& e = scene.env;
    e.sunDir = glm::normalize(vec3(-0.45f, 0.55f, 0.85f));
    e.sunColor = vec3(1.0f, 0.97f, 0.92f);
    e.sunStrength = 6.0f;
    e.hazeAmount = 0;
    e.heightFogDensity = 0;
    e.clouds = false;
    e.exposureEv = 0.15f;
    e.lookPower = 1.08f;
    e.lookSaturation = 1.12f;
    e.vignette = 0;
    e.bloomStrength = 0;
    e.painterly = false;
    e.gain = e.highlightsGain = e.shadowsGain = vec3(1.0f);
    e.whiteTempK = 6500.0f;
    std::vector<GpuInstance> keepDyn(scene.instances.begin() + staticEnd, scene.instances.end());
    for (size_t i = staticEnd; i < scene.instances.size(); ++i) scene.instances[i].cullDistance = -1.0f;
    Renderer& r = engine->renderer;
    float keepShadow = r.shadowDistanceOverride, keepCull = r.cullScaleOverride;
    r.updateDynamicInstances(scene);
    r.shadowDistanceOverride = cam.position.z - scene.boundsMin.z + 10.0f;
    r.cullScaleOverride = cullScale;
    CaptureResult img = engine->capture(cam, time, res, res);
    r.shadowDistanceOverride = keepShadow;
    r.cullScaleOverride = keepCull;
    scene.env = keepEnv;
    std::copy(keepDyn.begin(), keepDyn.end(), scene.instances.begin() + staticEnd);
    r.updateDynamicInstances(scene);

    // paths and water stand out: the terrain's path mask and the ground below the water level tinted in
    const Terrain& T = world.terrain;
    float k = res / size, water = scene.env.waterLevel;
    if (!T.empty() && T.pathMask.size() == (size_t)T.n * T.n && T.height.size() == T.pathMask.size()) {
        auto tint = [](uint8_t* px, vec3 c, float a) { for (int ch = 0; ch < 3; ++ch) px[ch] = (uint8_t)std::lround(px[ch] + (c[ch] - px[ch]) * a); };
        for (uint32_t y = 0; y < res; ++y)
            for (uint32_t x = 0; x < res; ++x) {
                vec2 g = (vec2(lo.x + (x + 0.5f) / k, lo.y + size - (y + 0.5f) / k) - T.origin) / T.spacing;
                g = glm::clamp(g, vec2(0), vec2((float)T.n - 1.001f));
                size_t at = (size_t)g.y * T.n + (uint32_t)g.x;
                vec2 f = glm::fract(g);
                auto bilerp = [&](const std::vector<float>& v) {
                    return glm::mix(glm::mix(v[at], v[at + 1], f.x), glm::mix(v[at + T.n], v[at + T.n + 1], f.x), f.y);
                };
                uint8_t* px = &img.rgba[((size_t)y * res + x) * 4];
                float surf = T.waterSurface.size() == T.height.size() ? std::max(water, bilerp(T.waterSurface)) : water;
                float wet = glm::smoothstep(0.0f, 1.5f, surf - bilerp(T.height)) * 0.7f;
                if (wet > 0) tint(px, vec3(52, 98, 136), wet);
                float path = glm::smoothstep(0.15f, 0.6f, bilerp(T.pathMask)) * 0.75f;
                if (path > 0) tint(px, vec3(214, 190, 140), path);
            }
    }
    // what the budget left out: soft dots with a south-east shadow; foliage dark green, the rest grey
    size_t dots = 0;
    if (cut > 0) {
        std::vector<int8_t> veg(scene.meshes.size(), -1);
        auto splat = [&](vec2 p, float rM, vec3 col, float a) {
            vec2 c((p.x - lo.x) * k, (lo.y + size - p.y) * k);
            float rp = std::max(rM * k, 0.7f);
            int x0 = std::max(0, (int)(c.x - rp - 1)), x1 = std::min((int)res - 1, (int)(c.x + rp + 1));
            int y0 = std::max(0, (int)(c.y - rp - 1)), y1 = std::min((int)res - 1, (int)(c.y + rp + 1));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    float w = std::clamp(rp + 0.5f - glm::length(vec2(x + 0.5f, y + 0.5f) - c), 0.0f, 1.0f) * a;
                    uint8_t* px = &img.rgba[((size_t)y * res + x) * 4];
                    for (int ch = 0; ch < 3; ++ch) px[ch] = (uint8_t)std::lround(px[ch] + (col[ch] * 255.0f - px[ch]) * w);
                }
        };
        for (const Cand& cd : cands) {
            const GpuInstance& in = scene.instances[cd.index];
            if (drawn(in)) continue;
            int8_t& v = veg[in.mesh];
            if (v < 0) {
                v = 0;
                for (uint32_t sm : scene.meshes[in.mesh].lods[0].submeshes) {
                    const GpuMaterial& g = scene.materials[scene.submeshes[sm].material].gpu;
                    if (g.h0.x == ModelFoliage || g.h0.x == ModelGrass || (g.h0.y & MatMasked)) v = 1;
                }
            }
            float rad = scene.meshes[in.mesh].bounds.w * in.posScale.w * (v ? 0.55f : 0.7f), shade = 0.85f + 0.3f * std::fmod(std::abs(in.seed) * 7.13f, 1.0f);
            vec2 p(in.posScale);
            splat(p + vec2(0.35f, -0.35f) * rad, rad, vec3(0), 0.3f);
            splat(p, rad, (v ? vec3(0.16f, 0.25f, 0.11f) : vec3(0.5f, 0.48f, 0.45f)) * shade, 0.95f);
            ++dots;
        }
    }
    engine->hud.setMinimap(img.rgba.data(), img.width, img.height);
    minimapOrigin_ = lo;
    minimapSize_ = size;
    logInfo("minimap: {} px over {:.0f} m in {:.0f} ms ({:.1f} M triangles{})", res, size,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), std::min(sum, budget) / 1e6,
            dots ? std::format(", {} instances over the budget as dots", dots) : "");
}

// ------------------------------------------------------------------------------------- editor overlay
// The selection (a click in the window, or an agent's editor_select): each item's box as lines and a label, drawn
// over window frames only (never in captures). Projected with the same basis as the click's ray.
namespace {
struct Projector {
    vec3 pos, f, r, u;
    float t, aspect, w, h;
    bool project(vec3 p, vec2& s) const {
        vec3 d = p - pos;
        float z = glm::dot(d, f);
        if (z < 0.05f) return false;
        s = vec2((glm::dot(d, r) / (z * t * aspect) + 1.0f) * 0.5f * w, (1.0f - glm::dot(d, u) / (z * t)) * 0.5f * h);
        return true;
    }
    // a segment, clipped to the space in front of the camera
    bool segment(vec3 a, vec3 b, vec2& sa, vec2& sb) const {
        float za = glm::dot(a - pos, f), zb = glm::dot(b - pos, f);
        const float zn = 0.05f;
        if (za < zn && zb < zn) return false;
        if (za < zn) a = glm::mix(a, b, (zn - za) / (zb - za));
        if (zb < zn) b = glm::mix(b, a, (zn - zb) / (za - zb));
        return project(a, sa) && project(b, sb);
    }
};
void line(HudCanvas& c, vec2 a, vec2 b, float width, uint32_t color) {
    vec2 d = b - a;
    float len = glm::length(d);
    if (len < 0.5f) return;
    vec2 n = vec2(-d.y, d.x) / len * (width * 0.5f);
    c.tri(a + n, b + n, b - n, color);
    c.tri(a + n, b - n, a - n, color);
}
}  // namespace

void Editor::drawEditorOverlay(HudCanvas& c) {
    if (selection.empty() || c.width == 0) return;
    const Camera& cam = overlayCamera_ ? *overlayCamera_ : editCamera;
    Projector P;
    P.pos = cam.position;
    P.f = cam.forward();
    P.r = glm::normalize(glm::cross(P.f, vec3(0, 0, 1)));
    P.u = glm::cross(P.r, P.f);
    P.t = std::tan(cam.vfov * 0.5f);
    P.w = (float)c.width;
    P.h = (float)c.height;
    P.aspect = P.w / std::max(1.0f, P.h);
    uint32_t col = hudColor(1.0f, 0.78f, 0.2f, 0.95f), shadow = hudColor(0.0f, 0.0f, 0.0f, 0.55f);
    std::string label;
    for (const Selected& s : selection) {
        vec3 lo, hi;
        if (!selectionBox(s, lo, hi)) continue;
        vec3 v[8];
        for (int k = 0; k < 8; ++k) v[k] = vec3((k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z);
        static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& e : edges) {
            vec2 a, b;
            if (!P.segment(v[e[0]], v[e[1]], a, b)) continue;
            line(c, a + vec2(1), b + vec2(1), 3.0f * c.ui, shadow);
            line(c, a, b, 2.0f * c.ui, col);
        }
        vec3 size = hi - lo;
        label += std::format("{}{} ({:.1f} x {:.1f} x {:.1f} m)", label.empty() ? "" : ",  ", s.id, size.x, size.y, size.z);
    }
    if (label.empty()) return;
    label = "Selected: " + label + "   -  agents see this (editor_state)";
    float scale = 2.0f * c.ui;
    float wText = HudCanvas::textWidth(label, scale);
    c.rect(vec2(12, 10) * c.ui, vec2(12 * c.ui + wText + 16 * c.ui, 10 * c.ui + HudCanvas::kCapHeight * scale + 14 * c.ui), hudColor(0, 0, 0, 0.45f));
    c.text(vec2(20, 17) * c.ui, label, scale, hudColor(1.0f, 0.92f, 0.7f), hudColor(0, 0, 0, 0.8f));
}
}  // namespace df
