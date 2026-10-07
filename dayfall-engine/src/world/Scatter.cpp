#include "world/Scatter.h"
#include "core/Error.h"
#include "world/Noise.h"
#include <cmath>
#include <format>
#include <unordered_map>

namespace df {
using json = nlohmann::json;
namespace {
vec2 range(const json& r, const char* k, vec2 def) {
    if (!r.contains(k)) return def;
    const json& v = r[k];
    if (v.is_number()) return vec2(v.get<float>());
    if (!v.is_array() || v.size() != 2) throw Error(std::format("scatter: {} must be [min, max]", k));
    return {v[0].get<float>(), v[1].get<float>()};
}
}  // namespace

json normalizeScatterRule(const json& in) {
    if (!in.is_object()) throw Error("a scatter rule must be an object");
    json r = in;
    if (!r.contains("meshes")) {
        if (!r.contains("mesh")) throw Error("scatter rule needs \"mesh\" or \"meshes\": [{\"mesh\": ..., \"weight\": 1}]");
        r["meshes"] = json::array({{{"mesh", r["mesh"]}, {"weight", 1.0}}});
        r.erase("mesh");
    }
    if (!r["meshes"].is_array() || r["meshes"].empty()) throw Error("scatter: meshes must be a non-empty array");
    for (auto& m : r["meshes"]) {
        if (!m.is_object() || !m.contains("mesh")) throw Error("scatter: each meshes entry needs \"mesh\"");
        if (!m.contains("weight")) m["weight"] = 1.0;
    }
    if (!r.contains("area")) r["area"] = "all";
    Area::parse(r["area"]);   // validates
    float density = r.value("density_per_100m2", 1.0f);
    if (!(density > 0.0f) || density > 5000.0f) throw Error("scatter: density_per_100m2 must be in (0, 5000]");
    r["density_per_100m2"] = density;
    range(r, "scale", {0.8f, 1.2f});
    range(r, "slope_deg", {0.0f, 35.0f});
    range(r, "height_m", {-1e9f, 1e9f});
    if (!r.contains("seed")) r["seed"] = 1;
    if (r.contains("footprint_m") && !(r["footprint_m"].is_number() && r["footprint_m"].get<float>() >= 0.0f))
        throw Error("scatter: footprint_m must be a number >= 0");
    if (r.contains("avoid_rules")) {
        if (!r["avoid_rules"].is_array()) throw Error("scatter: avoid_rules must be an array of scatter rule ids");
        for (auto& v : r["avoid_rules"]) if (!v.is_string()) throw Error("scatter: avoid_rules must be an array of scatter rule ids");
    }
    if (r.contains("companions")) {
        if (!r["companions"].is_array()) throw Error("scatter: companions must be an array");
        for (auto& c : r["companions"]) {
            if (!c.is_object()) throw Error("scatter: each companion must be an object");
            if (!c.contains("meshes")) {
                if (!c.contains("mesh")) throw Error("scatter: each companion needs \"mesh\" or \"meshes\"");
                c["meshes"] = json::array({{{"mesh", c["mesh"]}, {"weight", 1.0}}});
                c.erase("mesh");
            }
            if (!c["meshes"].is_array() || c["meshes"].empty()) throw Error("scatter: companion meshes must be a non-empty array");
            for (auto& m : c["meshes"]) {
                if (!m.is_object() || !m.contains("mesh")) throw Error("scatter: each companion meshes entry needs \"mesh\"");
                if (!m.contains("weight")) m["weight"] = 1.0;
            }
            for (auto [k, def] : {std::pair<const char*, vec2>{"count", {1.0f, 3.0f}}, {"distance_m", {0.6f, 2.0f}}, {"scale", {0.7f, 1.2f}}}) {
                vec2 v = range(c, k, def);
                if (v.x < 0 || v.y < v.x) throw Error(std::format("scatter: companion {} must be [min, max] with 0 <= min <= max", k));
                c[k] = {v.x, v.y};
            }
            if (c["count"][1].get<float>() > 50.0f) throw Error("scatter: companion count is at most 50");
            float chance = c.value("chance", 1.0f);
            if (!(chance >= 0.0f && chance <= 1.0f)) throw Error("scatter: companion chance must be between 0 and 1");
        }
    }
    return r;
}

FootprintIndex::FootprintIndex(const std::vector<Footprint>& fps, float cell) : fps_(fps), cell_(cell) {
    for (uint32_t i = 0; i < fps_.size(); ++i) {
        const Footprint& f = fps_[i];
        int x0 = (int)std::floor((f.center.x - f.radius) / cell_), x1 = (int)std::floor((f.center.x + f.radius) / cell_);
        int y0 = (int)std::floor((f.center.y - f.radius) / cell_), y1 = (int)std::floor((f.center.y + f.radius) / cell_);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) grid_[((uint64_t)(uint32_t)x << 32) | (uint32_t)y].push_back(i);
    }
}

bool FootprintIndex::blocked(vec2 p, float margin) const {
    if (fps_.empty()) return false;
    int r = (int)std::ceil(margin / cell_);
    int cx = (int)std::floor(p.x / cell_), cy = (int)std::floor(p.y / cell_);
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) {
            auto it = grid_.find(((uint64_t)(uint32_t)x << 32) | (uint32_t)y);
            if (it == grid_.end()) continue;
            for (uint32_t i : it->second)
                if (glm::length(p - fps_[i].center) < fps_[i].radius + margin) return true;
        }
    return false;
}

std::vector<ScatterPoint> evaluateScatter(const json& rule, const ScatterContext& ctx, size_t maxPoints) {
    std::vector<ScatterPoint> out;
    const Terrain* t = ctx.terrain;
    Area area = Area::parse(rule.value("area", json("all")));
    float density = rule.value("density_per_100m2", 1.0f);
    float cell = std::sqrt(100.0f / density);
    float minSpacing = rule.value("min_spacing_m", 0.0f);
    float clumping = std::clamp(rule.value("clumping", 0.3f), 0.0f, 1.0f);
    float clumpScale = std::max(rule.value("clump_scale_m", 30.0f), 1.0f);
    vec2 scale = range(rule, "scale", {0.8f, 1.2f});
    vec2 slope = range(rule, "slope_deg", {0.0f, 35.0f});
    vec2 hr = range(rule, "height_m", {-1e9f, 1e9f});
    float tilt = glm::radians(rule.value("tilt_deg", 0.0f));
    float align = std::clamp(rule.value("align_to_slope", 0.0f), 0.0f, 1.0f);
    float avoidPaths = rule.value("avoid_paths_m", 1.0f);
    float avoidObjects = rule.value("avoid_objects_m", 1.0f);
    bool avoidWater = rule.value("avoid_water", true);
    bool avoidRuts = rule.value("avoid_ruts", false);
    float sink = rule.value("sink_m", 0.05f);
    float maxRock = rule.value("max_rock", 1.0f), maxPath = rule.value("max_path", 0.15f);
    bool yawRandom = !rule.contains("yaw_deg");
    float yawFixed = glm::radians(rule.value("yaw_deg", 0.0f));
    uint32_t seed = rule.value("seed", 1u);
    const json& meshes = rule["meshes"];
    std::vector<float> cdf;
    float wsum = 0;
    for (auto& m : meshes) { wsum += std::max(m.value("weight", 1.0f), 0.0f); cdf.push_back(wsum); }
    if (wsum <= 0) return out;
    Perlin2D clump(seed * 7919u + 13u);

    vec2 lo, hi;
    area.bounds(lo, hi);
    if (t && !t->empty()) { lo = glm::max(lo, t->origin); hi = glm::min(hi, t->maxCorner()); }
    else if (area.kind == Area::Kind::All) { lo = vec2(-100); hi = vec2(100); }
    if (lo.x >= hi.x || lo.y >= hi.y) return out;
    int nx = (int)std::ceil((hi.x - lo.x) / cell), ny = (int)std::ceil((hi.y - lo.y) / cell);
    if ((double)nx * ny > 4.0e7) throw Error(std::format("scatter rule '{}' would test {:.0f} M cells; lower the density or the area",
                                                          rule.value("id", "?"), (double)nx * ny / 1e6));
    // spatial hash for min spacing
    std::unordered_map<uint64_t, std::vector<vec2>> grid;
    float gcell = std::max(minSpacing, 0.01f);
    auto key = [&](int x, int y) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y; };
    float pathProbe = avoidPaths;
    FootprintIndex fpIndex(ctx.footprints);
    for (int gy = 0; gy < ny; ++gy)
        for (int gx = 0; gx < nx; ++gx) {
            uint32_t h0 = hash32((uint32_t)gx * 73856093u ^ (uint32_t)gy * 19349663u ^ seed * 83492791u);
            float rx = (h0 & 0xFFFF) / 65536.0f, ry = (h0 >> 16) / 65536.0f;
            vec2 p = lo + (vec2(gx, gy) + vec2(rx, ry)) * cell;
            float keep = area.weight(p);
            if (keep <= 0) continue;
            if (clumping > 0) {
                float c = 0.5f + 0.5f * fbm(clump, p.x / clumpScale, p.y / clumpScale, 3) * 1.6f;
                keep *= glm::mix(1.0f, std::clamp(c * 1.6f - 0.3f, 0.0f, 1.0f), clumping);
            }
            if (hash01(h0, 1) >= keep) continue;
            float z = 0;
            if (t && !t->empty()) {
                if (!t->inside(p)) continue;
                z = t->heightAt(p.x, p.y);
                float s = t->slopeDegAt(p.x, p.y);
                if (s < slope.x || s > slope.y) continue;
                if (z < hr.x || z > hr.y) continue;
                if (avoidWater && (z < ctx.waterLevel + 0.15f || t->waterSurfaceAt(p.x, p.y) > z - 0.15f)) continue;
                vec4 layers = t->layersAt(p.x, p.y);
                if (layers.x > maxRock) continue;
                if (layers.w > maxPath) continue;
                if (avoidRuts && t->rutAt(p.x, p.y)) continue;
                if (pathProbe > 0) {
                    bool near = false;
                    for (int k = 0; k < 8 && !near; ++k) {
                        float a = k * 0.785398f;
                        if (t->pathAt(p.x + std::cos(a) * pathProbe, p.y + std::sin(a) * pathProbe) > 0.3f) near = true;
                    }
                    if (near) continue;
                }
            }
            bool blocked = false;
            if (minSpacing > 0) {
                int cx = (int)std::floor(p.x / gcell), cy = (int)std::floor(p.y / gcell);
                for (int dy = -1; dy <= 1 && !blocked; ++dy)
                    for (int dx = -1; dx <= 1 && !blocked; ++dx) {
                        auto it = grid.find(key(cx + dx, cy + dy));
                        if (it == grid.end()) continue;
                        for (vec2 q : it->second) if (glm::length(q - p) < minSpacing) { blocked = true; break; }
                    }
                if (blocked) continue;
                grid[key(cx, cy)].push_back(p);
            }
            if (fpIndex.blocked(p, avoidObjects)) continue;   // after spacing: see Scatter.h
            ScatterPoint sp;
            float pick = hash01(h0, 2) * wsum;
            sp.variant = 0;
            while (sp.variant + 1 < cdf.size() && pick > cdf[sp.variant]) ++sp.variant;
            sp.scale = glm::mix(scale.x, scale.y, hash01(h0, 3));
            float yaw = yawRandom ? hash01(h0, 4) * 6.2831853f : yawFixed;
            quat q = glm::angleAxis(yaw, vec3(0, 0, 1));
            if (tilt > 0) {
                float ta = hash01(h0, 5) * 6.2831853f;
                q = glm::angleAxis(hash01(h0, 6) * tilt, vec3(std::cos(ta), std::sin(ta), 0)) * q;
            }
            if (align > 0 && t && !t->empty()) {
                vec3 nn = t->normalAt(p.x, p.y);
                quat toN = glm::rotation(vec3(0, 0, 1), glm::normalize(glm::mix(vec3(0, 0, 1), nn, align)));
                q = toN * q;
            }
            sp.rotation = q;
            sp.position = vec3(p, z - sink * sp.scale);
            out.push_back(sp);
            if (out.size() >= maxPoints) return out;
        }
    return out;
}
}  // namespace df
