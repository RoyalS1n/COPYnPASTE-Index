#include "world/Terrain.h"
#include "core/Error.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include <stb_image.h>
#include "world/Noise.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

namespace df {
using json = nlohmann::json;
namespace {
constexpr float kPi = 3.14159265358979f;
float num(const json& j, const char* k, float def) { return j.contains(k) && j[k].is_number() ? j[k].get<float>() : def; }

std::vector<vec2> catmullRom(const std::vector<vec2>& p, float step) {
    std::vector<vec2> out;
    if (p.size() < 2) return p;
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        vec2 p0 = p[i == 0 ? 0 : i - 1], p1 = p[i], p2 = p[i + 1], p3 = p[std::min(i + 2, p.size() - 1)];
        int steps = std::max(1, (int)std::ceil(glm::length(p2 - p1) / step));
        for (int s = 0; s < steps; ++s) {
            float t = (float)s / steps, t2 = t * t, t3 = t2 * t;
            out.push_back(0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                                  (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3));
        }
    }
    out.push_back(p.back());
    return out;
}
}  // namespace

uint64_t Terrain::nextVersion() {
    static std::atomic<uint64_t> counter{1};
    return ++counter;
}

const char* Terrain::paintName(uint32_t c) {
    static const char* names[] = {"dirt", "rock", "snow", "wet", "dry", "grass", "reserved6", "reserved7"};
    return c < kPaintChannels ? names[c] : "?";
}
int Terrain::paintIndex(const std::string& name) {
    for (uint32_t i = 0; i < 6; ++i) if (name == paintName(i)) return (int)i;
    return -1;
}

void Terrain::create(uint32_t samples, float sp, vec2 org, float h) {
    if (samples < 3 || samples > 8193) throw Error("terrain samples per side must be between 3 and 8193");
    if (!(sp > 0.05f && sp <= 64.0f)) throw Error("terrain spacing must be between 0.05 and 64 m");
    n = samples;
    spacing = sp;
    origin = org;
    base.assign((size_t)n * n, h);
    paint.assign((size_t)n * n * kPaintChannels, 0);
    height = base;
    pathMask.assign(base.size(), 0.0f);
    laneMask.assign(base.size(), 0.0f);
    laneDist.assign(base.size(), 0.0f);
    touch();
}

void Terrain::thermalErosion(int passes, float talus, float rate) {
    float lim = talus * spacing;
    std::vector<float> moved(base.size());
    const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
    for (int p = 0; p < passes; ++p) {
        std::fill(moved.begin(), moved.end(), 0.0f);
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) {
                float h = at(base, i, j);
                for (int k = 0; k < 4; ++k) {
                    int ni = (int)i + di[k], nj = (int)j + dj[k];
                    if (ni < 0 || nj < 0 || ni >= (int)n || nj >= (int)n) continue;
                    float diff = h - at(base, ni, nj);
                    if (diff > lim) {
                        float f = (diff - lim) * rate * 0.25f;
                        moved[(size_t)j * n + i] -= f;
                        moved[(size_t)nj * n + ni] += f;
                    }
                }
            }
        for (size_t i = 0; i < base.size(); ++i) base[i] += moved[i];
    }
}

void Terrain::generate(const json& p) {
    std::string preset = p.value("preset", "hills");
    float size = num(p, "size_m", 512.0f);
    float sp = num(p, "spacing_m", 1.0f);
    if (size < 16 || size > 16384) throw Error("terrain size_m must be between 16 and 16384");
    uint32_t samples = (uint32_t)std::round(size / sp) + 1;
    if (samples > 4097) throw Error(std::format("terrain too dense: {} samples per side (max 4097); raise spacing_m", samples));
    seed = (uint32_t)num(p, "seed", (float)seed);
    vec2 org = p.contains("origin") ? vec2(p["origin"][0].get<float>(), p["origin"][1].get<float>()) : vec2(-size * 0.5f);
    create(samples, sp, org, 0.0f);
    std::fill(paint.begin(), paint.end(), 0);
    Perlin2D n1(seed), n2(seed + 1), n3(seed + 2), n4(seed + 3);
    float half = size * 0.5f;
    float sc = std::clamp(size / 2000.0f, 0.25f, 4.0f);   // feature-size scale for large shapes
    vec2 centre = org + vec2(half);
    float axis = glm::radians(num(p, "axis_deg", 0.0f));
    float ca = std::cos(axis), sa = std::sin(axis);
    auto micro = [&](float X, float Y) {
        return num(p, "micro_relief", 1.0f) * (0.35f * fbm(n1, X / 7.0f + 11, Y / 7.0f, 3) + 0.15f * std::abs(fbm(n2, X / 3.2f, Y / 3.2f, 2)));
    };
    if (preset == "flat") {
        float h0 = num(p, "base_height_m", 0.0f);
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) { vec2 w = pos(i, j); at(base, i, j) = h0 + 0.4f * micro(w.x, w.y); }
    } else if (preset == "hills") {
        float H = num(p, "height_m", 0.05f * size), feat = num(p, "feature_m", 0.4f * size);
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) {
                vec2 w = pos(i, j);
                float h = H * (0.5f + 0.5f * fbm(n1, w.x / feat, w.y / feat, 5)) + 0.25f * H * fbm(n2, w.x / (feat * 0.3f), w.y / (feat * 0.3f), 4);
                at(base, i, j) = h + micro(w.x, w.y);
            }
        thermalErosion(6, 1.0f, 0.22f);
    } else if (preset == "mountains") {
        float H = num(p, "height_m", 0.25f * size), feat = num(p, "feature_m", 0.45f * size);
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) {
                vec2 w = pos(i, j);
                float wx = w.x + 0.2f * feat * fbm(n3, w.x / feat, w.y / feat, 4), wy = w.y + 0.2f * feat * fbm(n4, w.x / feat + 7.1f, w.y / feat, 4);
                float r = smoothstep(0.08f, 0.95f, ridged(n1, wx / feat, wy / feat, 8));
                at(base, i, j) = H * r + 0.08f * H * ridged(n2, wx / (feat * 0.25f), wy / (feat * 0.25f), 5) + micro(w.x, w.y);
            }
        thermalErosion(10, 1.15f, 0.22f);
    } else if (preset == "island") {
        float H = num(p, "height_m", 0.08f * size), depth = num(p, "sea_depth_m", 12.0f);
        float rad = num(p, "radius_m", 0.38f * size);
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) {
                vec2 w = pos(i, j), d = w - centre;
                float r = glm::length(d) + 0.12f * rad * fbm(n3, w.x / (rad * 0.6f), w.y / (rad * 0.6f), 4);
                float land = 1.0f - smoothstep(rad * 0.75f, rad * 1.1f, r);
                float hills = H * (0.4f + 0.6f * fbm(n1, w.x / (rad * 0.5f), w.y / (rad * 0.5f), 5)) * smoothstep(0.0f, rad * 0.6f, rad - r);
                at(base, i, j) = glm::mix(-depth, 1.5f + hills, land) + micro(w.x, w.y);
            }
        thermalErosion(8, 1.0f, 0.22f);
        waterLevel = num(p, "water_level_m", 0.0f);
    } else if (preset == "meadow") {
        // rolling grassland ringed by low forested hills (reference-world serene_meadow)
        float core = num(p, "radius_m", 0.35f * size), rimH = num(p, "rim_height_m", 0.05f * size);
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) {
                vec2 w = pos(i, j), d = w - centre;
                float r = std::hypot(d.x, d.y / 1.15f) + 70.0f * sc * fbm(n3, w.x / (420.0f * sc), w.y / (420.0f * sc), 3);
                float rise = smoothstep(core * 0.85f, core * 1.6f, r);
                float rolling = 5.0f * fbm(n1, w.x / 260.0f, w.y / 260.0f, 4) + 2.2f * fbm(n2, w.x / 95.0f, w.y / 95.0f, 4);
                float rim = rimH * rise * (0.55f + 0.45f * ridged(n4, w.x / (600.0f * sc), w.y / (600.0f * sc), 6));
                at(base, i, j) = 5.0f + rolling * (1.0f - 0.5f * rise) + micro(w.x, w.y) + rim;
            }
        thermalErosion(12, 1.0f, 0.22f);
    } else if (preset == "valley") {
        // a meandering valley floor between ridged mountain walls, closing into
        // a massif at the far (+Y) end (reference-world golden_valley)
        float vw = num(p, "valley_width_m", std::max(40.0f, 0.14f * size)), vs = num(p, "valley_slope_m", 0.25f * size);
        float mh = num(p, "mountain_height_m", 0.16f * size), bh = num(p, "backdrop_height_m", 0.2f * size);
        float hh = num(p, "hill_height_m", 0.03f * size), amp = num(p, "meander_amp_m", 0.05f * size);
        float lam = num(p, "meander_wavelength_m", 1.2f * size), warp = num(p, "warp_strength", 1.0f);
        bool closed = p.value("closed_end", true);
        Rng r(seed + 11);
        float ph1 = r.uniform(0, 2 * kPi), ph2 = r.uniform(0, 2 * kPi);
        std::vector<float> rise((size_t)n * n);
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) {
                vec2 w = pos(i, j), d = w - centre;
                float X = d.x * ca + d.y * sa, Y = -d.x * sa + d.y * ca;   // valley runs along local +Y
                float ws = warp * 260.0f * sc;
                float wx = X + ws * fbm(n3, X / (900.0f * sc), Y / (900.0f * sc), 4);
                float wy = Y + ws * fbm(n4, X / (900.0f * sc) + 7.1f, Y / (900.0f * sc) - 3.3f, 4);
                float centreX = amp * std::sin(2 * kPi * Y / lam + ph1) + 0.32f * amp * std::sin(2 * kPi * Y / (0.41f * lam) + ph2);
                float dist = std::abs(X - centreX + 18.0f * sc * fbm(n2, X / 160.0f, Y / 160.0f, 3));
                float valley = smoothstep(vw * 0.5f, vw * 0.5f + vs, dist);
                float far = closed ? smoothstep(0.05f * half, 0.95f * half, Y) : 0.0f;
                float mtn = smoothstep(0.08f, 0.95f, ridged(n1, wx / (700.0f * sc), wy / (700.0f * sc), 8));
                float mount = mh * mtn * (0.25f + 0.75f * valley) + bh * far * (0.45f + 0.55f * ridged(n2, wx / (520.0f * sc), wy / (520.0f * sc), 7));
                float hills = hh * (0.5f + 0.5f * fbm(n2, X / (260.0f * sc), Y / (260.0f * sc), 5));
                float floorH = 1.4f + 1.4f * fbm(n3, X / 70.0f, Y / 70.0f, 4) + 3.0f * sc * fbm(n4, X / 420.0f, Y / 420.0f, 3) + micro(X, Y);
                float rs = std::max(valley, far * 0.85f);
                rise[(size_t)j * n + i] = rs;
                at(base, i, j) = floorH + hills * smoothstep(0.0f, 0.6f, rs) + mount * rs;
            }
        // erosion-flavoured detail: gullies damped on steep slopes, sharper crests
        std::vector<float> h0 = base;
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) {
                vec2 w = pos(i, j), d = w - centre;
                float X = d.x * ca + d.y * sa, Y = -d.x * sa + d.y * ca;
                uint32_t i0 = i ? i - 1 : i, i1 = std::min(i + 1, n - 1), j0 = j ? j - 1 : j, j1 = std::min(j + 1, n - 1);
                float gx = (at(h0, i1, j) - at(h0, i0, j)) / ((i1 - i0) * spacing), gy = (at(h0, i, j1) - at(h0, i, j0)) / ((j1 - j0) * spacing);
                float slope = std::sqrt(gx * gx + gy * gy), rs = rise[(size_t)j * n + i];
                float h = at(base, i, j) + 9.0f * sc * fbm(n1, X / 38.0f, Y / 38.0f, 5) * rs / (1.0f + 2.5f * slope * slope);
                h += rs * (0.13f * mh * ridged(n4, X / (230.0f * sc), Y / (230.0f * sc), 6) + 0.07f * mh * ridged(n3, X / (75.0f * sc) + 3.3f, Y / (75.0f * sc), 4) +
                           0.018f * mh * ridged(n2, X / 26.0f - 1.7f, Y / 26.0f, 3));
                at(base, i, j) = h;
            }
        thermalErosion((int)num(p, "erosion_passes", 10), 1.15f, 0.22f);
    } else {
        throw Error("unknown terrain preset '" + preset + "' (flat, hills, valley, mountains, meadow, island)");
    }
    if (p.contains("snowline_m")) snowline = num(p, "snowline_m", snowline);
    if (p.contains("water_level_m")) waterLevel = num(p, "water_level_m", waterLevel);
    height = base;
    std::fill(pathMask.begin(), pathMask.end(), 0.0f);
    std::fill(laneMask.begin(), laneMask.end(), 0.0f);
    std::fill(laneDist.begin(), laneDist.end(), 0.0f);
    touch();
}

json Terrain::sculpt(const json& p) {
    if (empty()) throw Error("the map has no terrain; create one with terrain_generate first");
    std::string op = p.value("op", "");
    if (!p.contains("area")) throw Error("terrain sculpt needs an \"area\"");
    Area area = Area::parse(p["area"]);
    float strength = std::clamp(num(p, "strength", 1.0f), 0.0f, 1.0f);
    vec2 lo, hi;
    area.bounds(lo, hi);
    int i0 = std::max(0, (int)std::floor((lo.x - origin.x) / spacing)), i1 = std::min((int)n - 1, (int)std::ceil((hi.x - origin.x) / spacing));
    int j0 = std::max(0, (int)std::floor((lo.y - origin.y) / spacing)), j1 = std::min((int)n - 1, (int)std::ceil((hi.y - origin.y) / spacing));
    if (i0 > i1 || j0 > j1) throw Error("the area does not overlap the terrain");
    std::vector<float> before;
    float minD = 1e30f, maxD = -1e30f;
    size_t changed = 0;
    auto apply = [&](auto fn) {
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                vec2 w = pos(i, j);
                float wt = area.weight(w) * strength;
                if (wt <= 0) continue;
                float& h = at(base, i, j);
                float old = h;
                h = std::clamp(fn(h, w, wt, i, j), -2000.0f, 9000.0f);
                float d = h - old;
                if (d != 0) { ++changed; minD = std::min(minD, d); maxD = std::max(maxD, d); }
            }
    };
    if (op == "raise" || op == "lower") {
        float amount = num(p, "amount_m", 2.0f) * (op == "lower" ? -1.0f : 1.0f);
        apply([&](float h, vec2, float wt, int, int) { return h + amount * wt; });
    } else if (op == "flatten") {
        float target;
        if (p.contains("height_m")) target = num(p, "height_m", 0);
        else {   // the average base height inside the area
            double sum = 0; size_t cnt = 0;
            for (int j = j0; j <= j1; ++j) for (int i = i0; i <= i1; ++i) if (area.contains(pos(i, j))) { sum += at(base, i, j); ++cnt; }
            target = cnt ? (float)(sum / cnt) : 0.0f;
        }
        apply([&](float h, vec2, float wt, int, int) { return glm::mix(h, target, wt); });
    } else if (op == "set") {
        if (!p.contains("height_m")) throw Error("sculpt op 'set' needs height_m");
        float target = num(p, "height_m", 0);
        apply([&](float h, vec2, float wt, int, int) { return glm::mix(h, target, wt); });
    } else if (op == "smooth") {
        int iters = std::clamp((int)num(p, "iterations", 3.0f), 1, 50);
        int rad = std::clamp((int)std::round(num(p, "kernel_m", 2.0f * spacing) / spacing), 1, 16);
        for (int it = 0; it < iters; ++it) {
            std::vector<float> src = base;
            apply([&](float h, vec2, float wt, int i, int j) {
                double s = 0; int c = 0;
                for (int dj = -rad; dj <= rad; ++dj)
                    for (int di = -rad; di <= rad; ++di) {
                        int x = std::clamp(i + di, 0, (int)n - 1), y = std::clamp(j + dj, 0, (int)n - 1);
                        s += src[(size_t)y * n + x]; ++c;
                    }
                return glm::mix(h, (float)(s / c), wt);
            });
        }
    } else if (op == "noise") {
        float amp = num(p, "amplitude_m", 1.5f), scale = std::max(num(p, "scale_m", 20.0f), spacing);
        Perlin2D nz(seed + 97 + (uint32_t)num(p, "seed", 0));
        apply([&](float h, vec2 w, float wt, int, int) { return h + wt * amp * fbm(nz, w.x / scale, w.y / scale, 4); });
    } else if (op == "ramp") {
        // a straight grade from `from` to `to` (each [x, y, height]); good for paths up hills
        if (!p.contains("from") || !p.contains("to")) throw Error("sculpt op 'ramp' needs from [x, y, z] and to [x, y, z]");
        vec3 a(p["from"][0].get<float>(), p["from"][1].get<float>(), p["from"][2].get<float>());
        vec3 b(p["to"][0].get<float>(), p["to"][1].get<float>(), p["to"][2].get<float>());
        vec2 ab = vec2(b - a);
        float len2 = std::max(glm::dot(ab, ab), 1e-6f);
        apply([&](float h, vec2 w, float wt, int, int) {
            float t = std::clamp(glm::dot(w - vec2(a), ab) / len2, 0.0f, 1.0f);
            return glm::mix(h, glm::mix(a.z, b.z, t), wt);
        });
    } else {
        throw Error("unknown sculpt op '" + op + "' (raise, lower, flatten, set, smooth, noise, ramp)");
    }
    touch();
    return {{"op", op}, {"samples_changed", changed}, {"min_change_m", changed ? minD : 0.0f}, {"max_change_m", changed ? maxD : 0.0f}};
}

json Terrain::paintLayer(const json& p) {
    if (empty()) throw Error("the map has no terrain");
    std::string layer = p.value("layer", "");
    int ch = paintIndex(layer);
    if (ch < 0) throw Error("unknown layer '" + layer + "' (dirt, rock, snow, wet, dry, grass)");
    if (!p.contains("area")) throw Error("terrain paint needs an \"area\"");
    Area area = Area::parse(p["area"]);
    float strength = std::clamp(num(p, "strength", 1.0f), 0.0f, 1.0f);
    std::string mode = p.value("mode", "add");
    if (mode != "add" && mode != "set" && mode != "erase") throw Error("paint mode must be add, set or erase");
    vec2 lo, hi;
    area.bounds(lo, hi);
    int i0 = std::max(0, (int)std::floor((lo.x - origin.x) / spacing)), i1 = std::min((int)n - 1, (int)std::ceil((hi.x - origin.x) / spacing));
    int j0 = std::max(0, (int)std::floor((lo.y - origin.y) / spacing)), j1 = std::min((int)n - 1, (int)std::ceil((hi.y - origin.y) / spacing));
    size_t changed = 0;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            float w = area.weight(pos(i, j));
            if (w <= 0) continue;
            uint8_t& v = paint[((size_t)j * n + i) * kPaintChannels + ch];
            float f = v / 255.0f, nf = f;
            if (mode == "add") nf = std::max(f, strength * w);
            else if (mode == "set") nf = glm::mix(f, strength, w);
            else nf = f * (1.0f - w * strength);
            uint8_t nv = (uint8_t)std::lround(std::clamp(nf, 0.0f, 1.0f) * 255.0f);
            if (nv != v) { v = nv; ++changed; }
        }
    touch();
    return {{"layer", layer}, {"mode", mode}, {"samples_changed", changed}};
}

void Terrain::applyPaths(const json& paths) {
    height = base;
    std::fill(pathMask.begin(), pathMask.end(), 0.0f);
    std::fill(laneMask.begin(), laneMask.end(), 0.0f);
    std::fill(laneDist.begin(), laneDist.end(), 0.0f);
    if (empty() || !paths.is_array()) return;
    std::vector<float> best, lat, hcen;
    for (const json& path : paths) {
        if (!path.contains("points") || !path["points"].is_array() || path["points"].size() < 2) continue;
        std::vector<vec2> pts;
        for (auto& q : path["points"]) pts.push_back({q[0].get<float>(), q[1].get<float>()});
        float width = std::max(num(path, "width_m", 3.0f), 0.3f), hw = width * 0.5f;
        float carve = num(path, "carve_m", 0.12f), flatten = std::clamp(num(path, "flatten", 0.85f), 0.0f, 1.0f);
        float falloff = std::max(num(path, "falloff_m", width * 0.75f + 1.0f), 0.1f);
        float smoothM = std::max(num(path, "smooth_m", 16.0f), 0.0f);
        float maxGrade = num(path, "max_grade", 0.0f);
        bool lane = path.value("style", "dirt") == "lane";
        float step = std::min(1.0f, spacing);
        std::vector<vec2> d = catmullRom(pts, step);
        if (d.size() < 2) continue;
        std::vector<float> hc(d.size());
        for (size_t k = 0; k < d.size(); ++k) hc[k] = sampleArr(height, d[k].x, d[k].y);
        // smooth the profile along the path, then cap its grade
        int win = std::max(1, (int)std::round(smoothM / step * 0.5f));
        for (int pass = 0; pass < 2 && smoothM > 0; ++pass) {
            std::vector<float> src = hc;
            for (size_t k = 0; k < d.size(); ++k) {
                double s = 0; int c = 0;
                for (int o = -win; o <= win; ++o) { int q = std::clamp((int)k + o, 0, (int)d.size() - 1); s += src[q]; ++c; }
                hc[k] = (float)(s / c);
            }
        }
        if (maxGrade > 0)
            for (int pass = 0; pass < 2; ++pass) {
                for (size_t k = 1; k < d.size(); ++k) {
                    float lim = maxGrade * glm::length(d[k] - d[k - 1]);
                    hc[k] = std::clamp(hc[k], hc[k - 1] - lim, hc[k - 1] + lim);
                }
                for (size_t k = d.size() - 1; k-- > 0;) {
                    float lim = maxGrade * glm::length(d[k + 1] - d[k]);
                    hc[k] = std::clamp(hc[k], hc[k + 1] - lim, hc[k + 1] + lim);
                }
            }
        best.assign(base.size(), 1e30f);
        lat.assign(base.size(), 0.0f);
        hcen.assign(base.size(), 0.0f);
        float reach = hw + falloff;
        int bi0 = (int)n, bi1 = -1, bj0 = (int)n, bj1 = -1;
        for (size_t k = 0; k + 1 < d.size(); ++k) {
            vec2 a = d[k], b = d[k + 1], ab = b - a;
            float len2 = glm::dot(ab, ab);
            if (len2 < 1e-10f) continue;
            vec2 lo = glm::min(a, b) - reach, hi = glm::max(a, b) + reach;
            int i0 = std::max(0, (int)std::floor((lo.x - origin.x) / spacing)), i1 = std::min((int)n - 1, (int)std::ceil((hi.x - origin.x) / spacing));
            int j0 = std::max(0, (int)std::floor((lo.y - origin.y) / spacing)), j1 = std::min((int)n - 1, (int)std::ceil((hi.y - origin.y) / spacing));
            bi0 = std::min(bi0, i0); bi1 = std::max(bi1, i1); bj0 = std::min(bj0, j0); bj1 = std::max(bj1, j1);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    vec2 w = pos(i, j);
                    float t = std::clamp(glm::dot(w - a, ab) / len2, 0.0f, 1.0f);
                    vec2 c = a + ab * t;
                    float dist = glm::length(w - c);
                    size_t idx = (size_t)j * n + i;
                    if (dist < best[idx]) {
                        best[idx] = dist;
                        lat[idx] = (ab.x * (w.y - a.y) - ab.y * (w.x - a.x)) > 0 ? -dist : dist;
                        hcen[idx] = glm::mix(hc[k], hc[k + 1], t);
                    }
                }
        }
        for (int j = bj0; j <= bj1; ++j)
            for (int i = bi0; i <= bi1; ++i) {
                size_t idx = (size_t)j * n + i;
                float dist = best[idx];
                if (dist >= reach) continue;
                float w = 1.0f - smoothstep(hw, reach, dist);
                float core = 1.0f - smoothstep(hw * 0.5f, hw, dist);
                height[idx] = glm::mix(height[idx], hcen[idx] - carve * core, w * flatten);
                float pm = 1.0f - smoothstep(hw - 0.4f, hw + 0.4f, dist);
                if (lane) {
                    if (pm > laneMask[idx]) laneDist[idx] = lat[idx];
                    laneMask[idx] = std::max(laneMask[idx], pm > 0.01f ? 1.0f : 0.0f);
                }
                pathMask[idx] = std::max(pathMask[idx], pm);
            }
    }
}

namespace {
float smoothRange(float e0, float e1, float x) { float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); }
}  // namespace

std::vector<Terrain::WaterBody> Terrain::applyWater(const json& bodies, std::vector<std::string>& warnings) {
    std::vector<WaterBody> out;
    waterSurface.assign(height.size(), kNoWater);
    shoreLevel.assign(height.size(), kNoWater);
    if (empty() || !bodies.is_array()) return out;
    const float shore = 4.0f;   // metres of wet bank around the water
    for (const json& j : bodies) {
        WaterBody b;
        b.id = j.value("id", "?");
        b.type = j.value("type", "lake");
        b.material = j.value("material", "water");
        try {
            if (b.type == "lake") {
                b.area = Area::parse(j.at("area"));
                if (b.area.kind == Area::Kind::All) throw Error("a lake needs a bounded area");
                vec2 lo, hi;
                b.area.bounds(lo, hi);
                float pad = shore + spacing * 2.0f + 1.0f;   // bounds() already includes the falloff
                int i0 = std::max(0, (int)std::floor((lo.x - pad - origin.x) / spacing)), i1 = std::min((int)n - 1, (int)std::ceil((hi.x + pad - origin.x) / spacing));
                int j0 = std::max(0, (int)std::floor((lo.y - pad - origin.y) / spacing)), j1 = std::min((int)n - 1, (int)std::ceil((hi.y + pad - origin.y) / spacing));
                if (i0 > i1 || j0 > j1) throw Error("the lake's area is outside the terrain");
                // the level: given, or "auto": just below the lowest ground on the rim (outside the area, before any
                // carving), so the water stays in
                const json& lv = j.value("level_m", json("auto"));
                if (lv.is_number()) {
                    b.level = lv.get<float>();
                } else {
                    float rim = 1e30f, r1 = std::max(spacing * 1.5f, 1.0f);
                    for (int jj = j0; jj <= j1; ++jj)
                        for (int ii = i0; ii <= i1; ++ii) {
                            float d = b.area.distance(pos(ii, jj));
                            if (d > 0 && d <= r1) rim = std::min(rim, height[(size_t)jj * n + ii]);
                        }
                    if (rim > 1e29f) throw Error("could not find the lake's rim (is the area larger than the terrain?)");
                    b.level = rim - j.value("freeboard_m", 0.1f);
                }
                // carve_m: dig the area down to that depth below the level (deepening over a few metres from the shore),
                // with a low beach outside it that blends back to the ground over the area's falloff
                float carve = j.value("carve_m", 0.0f);
                if (carve > 0) {
                    float ramp = std::max(2.0f, carve * 3.0f), bank = std::max(b.area.falloff, spacing);
                    for (int jj = j0; jj <= j1; ++jj)
                        for (int ii = i0; ii <= i1; ++ii) {
                            size_t idx = (size_t)jj * n + ii;
                            float d = b.area.distance(pos(ii, jj));
                            if (d <= 0) height[idx] = std::min(height[idx], b.level - carve * smoothRange(0.0f, ramp, -d));
                            else if (d < bank) height[idx] = glm::mix(std::min(height[idx], b.level + 0.25f), height[idx], smoothRange(0.0f, bank, d));
                        }
                }
                int wet = 0;
                for (int jj = j0; jj <= j1; ++jj)
                    for (int ii = i0; ii <= i1; ++ii) {
                        size_t idx = (size_t)jj * n + ii;
                        float d = b.area.distance(pos(ii, jj));
                        if (d <= 0 && height[idx] < b.level) { waterSurface[idx] = std::max(waterSurface[idx], b.level); ++wet; }
                        if (d <= shore) shoreLevel[idx] = std::max(shoreLevel[idx], b.level);
                    }
                if (wet == 0) warnings.push_back(std::format("water '{}': the ground is above level {:.2f} everywhere in its area (carve_m digs a basin)", b.id, b.level));
            } else if (b.type == "river") {
                const json& pj = j.at("points");
                if (!pj.is_array() || pj.size() < 2) throw Error("a river needs at least 2 points");
                std::vector<vec2> pts;
                for (auto& q : pj) pts.push_back({q.at(0).get<float>(), q.at(1).get<float>()});
                float width = std::max(j.value("width_m", 6.0f), 0.5f), depth = std::max(j.value("depth_m", 1.2f), 0.1f);
                b.halfWidth = width * 0.5f;
                float bank = std::max(j.value("bank_m", width * 0.5f + 1.5f), 0.5f);
                float step = std::min(1.0f, spacing);
                std::vector<vec2> d = catmullRom(pts, step);
                if (d.size() < 2) throw Error("the river is too short");
                std::vector<float> hb(d.size());
                for (size_t k = 0; k < d.size(); ++k) hb[k] = sampleArr(height, d[k].x, d[k].y);
                // bank profile: smoothed, then never rising downstream (the river cuts through rises)
                int win = std::max(1, (int)std::round(12.0f / step));
                for (int pass = 0; pass < 2; ++pass) {
                    std::vector<float> src = hb;
                    for (size_t k = 0; k < d.size(); ++k) {
                        double s = 0; int c = 0;
                        for (int o = -win; o <= win; ++o) { int q = std::clamp((int)k + o, 0, (int)d.size() - 1); s += src[q]; ++c; }
                        hb[k] = (float)(s / c);
                    }
                }
                for (size_t k = 1; k < d.size(); ++k) hb[k] = std::min(hb[k], hb[k - 1]);
                b.line = d;
                b.surface.resize(d.size());
                for (size_t k = 0; k < d.size(); ++k) b.surface[k] = hb[k] - depth * 0.25f;
                for (size_t k = 1; k < d.size(); ++k) b.length += glm::length(d[k] - d[k - 1]);
                float drop = b.surface.front() - b.surface.back();
                b.flowSpeed = std::clamp(0.35f + drop / std::max(b.length, 1.0f) * 60.0f, 0.35f, 3.0f);
                // carve: a U-shaped bed (depth at the centre line, bank height at the half width), banks lowered to
                // the bank profile and blended back to the ground over bank_m
                float reach = b.halfWidth + bank;
                std::vector<float> best(height.size(), 1e30f), at(height.size(), 0.0f);
                int bi0 = (int)n, bi1 = -1, bj0 = (int)n, bj1 = -1;
                for (size_t k = 0; k + 1 < d.size(); ++k) {
                    vec2 a = d[k], c = d[k + 1], ac = c - a;
                    float len2 = glm::dot(ac, ac);
                    if (len2 < 1e-10f) continue;
                    vec2 lo = glm::min(a, c) - reach, hi = glm::max(a, c) + std::max(reach, shore);
                    lo -= std::max(0.0f, shore - reach);
                    int i0 = std::max(0, (int)std::floor((lo.x - origin.x) / spacing)), i1 = std::min((int)n - 1, (int)std::ceil((hi.x - origin.x) / spacing));
                    int j0 = std::max(0, (int)std::floor((lo.y - origin.y) / spacing)), j1 = std::min((int)n - 1, (int)std::ceil((hi.y - origin.y) / spacing));
                    bi0 = std::min(bi0, i0); bi1 = std::max(bi1, i1); bj0 = std::min(bj0, j0); bj1 = std::max(bj1, j1);
                    for (int jj = j0; jj <= j1; ++jj)
                        for (int ii = i0; ii <= i1; ++ii) {
                            vec2 w = pos(ii, jj);
                            float t = std::clamp(glm::dot(w - a, ac) / len2, 0.0f, 1.0f);
                            float dist = glm::length(w - (a + ac * t));
                            size_t idx = (size_t)jj * n + ii;
                            if (dist < best[idx]) { best[idx] = dist; at[idx] = (float)k + t; }
                        }
                }
                int wet = 0;
                for (int jj = std::max(bj0, 0); jj <= bj1; ++jj)
                    for (int ii = std::max(bi0, 0); ii <= bi1; ++ii) {
                        size_t idx = (size_t)jj * n + ii;
                        float dist = best[idx];
                        if (dist >= std::max(reach, shore)) continue;
                        size_t k = std::min((size_t)at[idx], d.size() - 2);
                        float f = at[idx] - (float)k;
                        float bankH = glm::mix(hb[k], hb[k + 1], f), surf = glm::mix(b.surface[k], b.surface[k + 1], f);
                        float bed = bankH - depth;
                        if (dist < b.halfWidth) {
                            float u = dist / b.halfWidth;
                            height[idx] = std::min(height[idx], bed + (bankH - bed) * u * u);
                        } else if (dist < reach) {
                            float t = smoothRange(b.halfWidth, reach, dist);
                            height[idx] = glm::mix(std::min(height[idx], bankH), height[idx], t);
                        }
                        if (dist < b.halfWidth && height[idx] < surf) { waterSurface[idx] = std::max(waterSurface[idx], surf); ++wet; }
                        if (dist < b.halfWidth + shore) shoreLevel[idx] = std::max(shoreLevel[idx], surf);
                    }
                if (wet == 0) warnings.push_back(std::format("water '{}': the river is outside the terrain", b.id));
            } else {
                throw Error("type must be lake or river");
            }
            out.push_back(std::move(b));
        } catch (const std::exception& e) {
            warnings.push_back(std::format("water '{}': {}", b.id, e.what()));
        }
    }
    return out;
}

float Terrain::waterSurfaceAt(float x, float y) const {
    if (empty()) return kNoWater;
    float best = kNoWater;
    if (!waterSurface.empty() && inside(vec2(x, y))) {
        uint32_t i = (uint32_t)std::clamp((int)std::lround((x - origin.x) / spacing), 0, (int)n - 1);
        uint32_t j = (uint32_t)std::clamp((int)std::lround((y - origin.y) / spacing), 0, (int)n - 1);
        best = waterSurface[(size_t)j * n + i];
        if (best != kNoWater && heightAt(x, y) >= best) best = kNoWater;   // the shore between samples
    }
    if (waterLevel > -999.0f && heightAt(x, y) < waterLevel) best = std::max(best, waterLevel);
    return best;
}

MeshAsset Terrain::waterMesh(const WaterBody& b) const {
    MeshBuilder mb;
    if (b.type == "lake") {
        vec2 lo, hi;
        b.area.bounds(lo, hi);
        lo = glm::max(lo, origin);
        hi = glm::min(hi, maxCorner());
        float cell = std::max(spacing, 1.0f);
        while (((hi.x - lo.x) / cell) * ((hi.y - lo.y) / cell) > 250000.0f) cell *= 1.5f;
        int nx = std::max(1, (int)std::ceil((hi.x - lo.x) / cell)), ny = std::max(1, (int)std::ceil((hi.y - lo.y) / cell));
        std::vector<int> idx((size_t)(nx + 1) * (ny + 1), -1);
        vec2 alo, ahi;
        b.area.bounds(alo, ahi);
        vec4 fetch(0, std::max(ahi.x - alo.x, ahi.y - alo.y), 0, 0);   // colour1.g: how far the wind blows over it
        auto vert = [&](int i, int j) {
            int& v = idx[(size_t)j * (nx + 1) + i];
            if (v < 0) {
                vec2 p = lo + vec2(i * cell, j * cell);
                float depth = std::max(b.level - heightAt(p.x, p.y), 0.0f);
                v = (int)mb.vertex(makeVertex(vec3(p, b.level), vec3(0, 0, 1), p * 0.1f, vec4(1, 0, 0, 1), vec4(depth * 0.5f, 0, 0, 1), fetch));
            }
            return (uint32_t)v;
        };
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                vec2 c = lo + vec2((i + 0.5f) * cell, (j + 0.5f) * cell);
                if (b.area.distance(c) > cell) continue;
                // only cells where the ground dips below the surface somewhere (the terrain hides the rest)
                float g = std::min({heightAt(c.x - cell * 0.5f, c.y - cell * 0.5f), heightAt(c.x + cell * 0.5f, c.y - cell * 0.5f),
                                    heightAt(c.x + cell * 0.5f, c.y + cell * 0.5f), heightAt(c.x - cell * 0.5f, c.y + cell * 0.5f), heightAt(c.x, c.y)});
                if (g > b.level + 0.05f) continue;
                mb.quad(b.material, vert(i, j), vert(i + 1, j), vert(i + 1, j + 1), vert(i, j + 1));
            }
    } else {
        // a ribbon a little narrower than the bed at the banks (its edges sink under the ground)
        float hw = b.halfWidth * 0.95f, along = 0;
        uint32_t prevL = 0, prevR = 0;
        for (size_t k = 0; k < b.line.size(); ++k) {
            vec2 dir = glm::normalize(b.line[std::min(k + 1, b.line.size() - 1)] - b.line[k > 0 ? k - 1 : 0]);
            vec2 side(-dir.y, dir.x);
            if (k > 0) along += glm::length(b.line[k] - b.line[k - 1]);
            float z = b.surface[k];
            vec4 tan(dir, 0, 1), flow(b.flowSpeed, b.halfWidth * 6.0f, 0, 0);   // speed, fetch
            float dl = std::max(z - heightAt(b.line[k].x, b.line[k].y), 0.0f);
            uint32_t l = mb.vertex(makeVertex(vec3(b.line[k] + side * hw, z), vec3(0, 0, 1), vec2(0, along * 0.1f), tan, vec4(dl * 0.5f, 0, 0, 1), flow));
            uint32_t r = mb.vertex(makeVertex(vec3(b.line[k] - side * hw, z), vec3(0, 0, 1), vec2(1, along * 0.1f), tan, vec4(dl * 0.5f, 0, 0, 1), flow));
            if (k > 0) mb.quad(b.material, prevR, r, l, prevL);
            prevL = l;
            prevR = r;
        }
    }
    return mb.build("water_" + b.id);
}

float Terrain::sampleArr(const std::vector<float>& a, float x, float y) const {
    if (empty()) return 0.0f;
    float fx = std::clamp((x - origin.x) / spacing, 0.0f, (float)(n - 1)), fy = std::clamp((y - origin.y) / spacing, 0.0f, (float)(n - 1));
    uint32_t i = std::min((uint32_t)fx, n - 2), j = std::min((uint32_t)fy, n - 2);
    float tx = fx - i, ty = fy - j;
    float a0 = glm::mix(at(a, i, j), at(a, i + 1, j), tx), a1 = glm::mix(at(a, i, j + 1), at(a, i + 1, j + 1), tx);
    return glm::mix(a0, a1, ty);
}

float Terrain::heightAt(float x, float y) const { return sampleArr(height, x, y); }
float Terrain::baseAt(float x, float y) const { return sampleArr(base, x, y); }
float Terrain::pathAt(float x, float y) const { return sampleArr(pathMask, x, y); }

bool Terrain::rutAt(float x, float y) const {
    if (empty() || laneMask.empty()) return false;
    uint32_t i = (uint32_t)std::clamp((int)std::lround((x - origin.x) / spacing), 0, (int)n - 1);
    uint32_t j = (uint32_t)std::clamp((int)std::lround((y - origin.y) / spacing), 0, (int)n - 1);
    size_t k = (size_t)j * n + i;
    if (laneMask[k] < 0.5f) return false;
    // the sample's own lane distance, moved to the exact point across the lane
    float d = std::abs(sampleArr(laneDist, x, y));
    return std::abs(d - 0.78f) < 0.3f;
}

vec3 Terrain::normalAt(float x, float y) const {
    if (empty()) return vec3(0, 0, 1);
    float e = spacing;
    float dx = heightAt(x + e, y) - heightAt(x - e, y), dy = heightAt(x, y + e) - heightAt(x, y - e);
    return glm::normalize(vec3(-dx, -dy, 2.0f * e));
}

float Terrain::paintAt(uint32_t ch, float x, float y) const {
    if (empty() || ch >= kPaintChannels) return 0.0f;
    uint32_t i = (uint32_t)std::clamp((int)std::lround((x - origin.x) / spacing), 0, (int)n - 1);
    uint32_t j = (uint32_t)std::clamp((int)std::lround((y - origin.y) / spacing), 0, (int)n - 1);
    return paint[((size_t)j * n + i) * kPaintChannels + ch] / 255.0f;
}

void Terrain::sampleMasks(uint32_t i, uint32_t j, float slope, vec4& c0, vec4& c1) const {
    static thread_local uint32_t cachedSeed = ~0u;
    static thread_local Perlin2D* nm = nullptr;
    if (cachedSeed != seed) { delete nm; nm = new Perlin2D(seed + 50); cachedSeed = seed; }
    vec2 w = pos(i, j);
    size_t idx = (size_t)j * n + i;
    float h = height[idx];
    float brk = fbm(*nm, w.x / 55.0f, w.y / 55.0f, 4), big = fbm(*nm, w.x / 400.0f + 9, w.y / 400.0f - 4, 4);
    const uint8_t* pt = &paint[idx * kPaintChannels];
    auto P = [&](int c) { return pt[c] / 255.0f; };
    float snowLine = snowline + 45.0f * big;
    float snow = smoothstep(snowLine - 25, snowLine + 25, h + 15 * brk) * (1 - smoothstep(40, 56, slope + 8 * brk));
    snow = std::max(snow, P(PaintSnow));
    float rock = smoothstep(rockSlopeDeg - 4, rockSlopeDeg + 10, slope + 9 * brk);
    rock = std::max(rock, P(PaintRock)) * (1 - snow) * (1 - P(PaintGrass));
    float wet = std::max(1 - smoothstep(waterLevel + 0.2f, waterLevel + 1.6f, h), P(PaintWet));
    if (!shoreLevel.empty() && shoreLevel[idx] != kNoWater) wet = std::max(wet, 1 - smoothstep(shoreLevel[idx] + 0.15f, shoreLevel[idx] + 1.2f, h));
    float dry = std::max(smoothstep(-0.2f, 0.6f, big + 0.4f * brk) * dryAmount, P(PaintDry)) * (1 - P(PaintGrass) * 0.7f);
    float path = std::max(pathMask[idx], P(PaintDirt));
    // the bed of a lake or river is silt, not turf
    if (!waterSurface.empty() && waterSurface[idx] != kNoWater) path = std::max(path, smoothstep(0.05f, 0.5f, waterSurface[idx] - h));
    rock *= 1 - path;
    dry *= 1 - path;
    float grass = std::clamp(1 - rock - snow, 0.0f, 1.0f);
    c0 = vec4(grass, rock, snow, wet);
    c1 = vec4(path, dry, laneMask[idx], 0);
}

vec4 Terrain::layersAt(float x, float y) const {
    if (empty()) return vec4(0);
    uint32_t i = (uint32_t)std::clamp((int)std::lround((x - origin.x) / spacing), 0, (int)n - 1);
    uint32_t j = (uint32_t)std::clamp((int)std::lround((y - origin.y) / spacing), 0, (int)n - 1);
    vec4 c0, c1;
    sampleMasks(i, j, slopeDegAt(x, y), c0, c1);
    return vec4(c0.y, c0.z, c0.w, c1.x);   // rock, snow, wet, path
}

MeshAsset Terrain::horizonMesh(const json& p) const {
    MeshBuilder b;
    float S = size();
    vec2 c = origin + vec2(S * 0.5f);
    float lo = 1e30f, hi = -1e30f;
    for (float h : height) { lo = std::min(lo, h); hi = std::max(hi, h); }
    float R = std::max(num(p, "radius_m", std::max(5000.0f, S * 10.0f)), S);
    float H = num(p, "height_m", std::max(80.0f, (hi - lo) * 1.4f));
    float rough = std::clamp(num(p, "roughness", 0.6f), 0.0f, 1.0f);
    Perlin2D n1(seed + 31), n2(seed + 32);
    const int around = 4 * 96;   // perimeter samples (square -> circle)
    const int rings = 48;
    auto square = [&](float a) {   // point on the terrain boundary in direction a
        vec2 d(std::cos(a), std::sin(a));
        float k = 0.5f * S / std::max(std::abs(d.x), std::abs(d.y));
        return c + d * k;
    };
    std::vector<vec3> pos((size_t)(rings + 1) * around);
    for (int r = 0; r <= rings; ++r) {
        float t = (float)r / rings;
        float tt = t * t;   // denser near the playable edge
        for (int i = 0; i < around; ++i) {
            float a = 6.2831853f * i / around;
            vec2 sq = square(a), far = c + vec2(std::cos(a), std::sin(a)) * R;
            vec2 q = glm::mix(sq, far, tt);
            float edgeH = heightAt(sq.x, sq.y) - (r == 0 ? 0.08f : 0.0f);
            float d = glm::length(q - sq);
            float ridge = ridged(n1, q.x / 1400.0f, q.y / 1400.0f, 6);
            float hills = 0.5f + 0.5f * fbm(n2, q.x / 600.0f, q.y / 600.0f, 5);
            float farH = lo + H * (rough * smoothstep(0.1f, 0.9f, ridge) + (1.0f - rough) * hills) * smoothstep(0.0f, R * 0.25f, d);
            float w = smoothstep(0.0f, std::max(S * 0.6f, 300.0f), d);
            pos[(size_t)r * around + i] = vec3(q, glm::mix(edgeH, std::max(farH, edgeH * 0.5f + farH * 0.5f), w));
        }
    }
    auto P = [&](int r, int i) { return pos[(size_t)r * around + ((i + around) % around)]; };
    for (int r = 0; r <= rings; ++r)
        for (int i = 0; i < around; ++i) {
            vec3 dr = r < rings ? P(r + 1, i) - P(r, i) : P(r, i) - P(r - 1, i);
            vec3 di = P(r, i + 1) - P(r, i - 1);
            vec3 nn = glm::normalize(glm::cross(di, dr));
            if (nn.z < 0) nn = -nn;
            float slope = glm::degrees(std::acos(std::clamp(nn.z, -1.0f, 1.0f)));
            vec3 q = P(r, i);
            float rock = smoothstep(rockSlopeDeg - 6, rockSlopeDeg + 8, slope);
            float snow = smoothstep(snowline - 20, snowline + 30, q.z) * (1 - smoothstep(40, 55, slope));
            vec4 c0(1 - rock, rock, snow, 0), c1(0, dryAmount * 0.6f, 0, 0);
            b.vertex(makeVertex(q, nn, vec2(q), vec4(1, 0, 0, 1), c0, c1));
        }
    for (int r = 0; r < rings; ++r)
        for (int i = 0; i < around; ++i) {
            uint32_t a = r * around + i, bb = r * around + (i + 1) % around, cc = (r + 1) * around + (i + 1) % around, d = (r + 1) * around + i;
            b.quad("terrain", a, d, cc, bb);
        }
    MeshAsset m = b.build("terrain_horizon");
    return m;
}

void Terrain::load(const std::filesystem::path& hf, const std::vector<std::filesystem::path>& pfs, uint32_t samples, float sp,
                   vec2 org, vec2 range, bool northFirst) {
    create(samples, sp, org);
    if (hf.extension() == ".png") {
        int w = 0, h = 0, c = 0;
        stbi_us* px = stbi_load_16(hf.string().c_str(), &w, &h, &c, 1);
        if (!px) throw Error("cannot read " + hf.string());
        if ((uint32_t)w != n || (uint32_t)h != n) { stbi_image_free(px); throw Error(std::format("{} is {}x{}, the map says {}x{}", hf.string(), w, h, n, n)); }
        for (uint32_t j = 0; j < n; ++j) {
            uint32_t row = northFirst ? n - 1 - j : j;
            for (uint32_t i = 0; i < n; ++i) base[(size_t)j * n + i] = range.x + (range.y - range.x) * (px[(size_t)row * n + i] / 65535.0f);
        }
        stbi_image_free(px);
    } else {
        auto raw = readBinary(hf);
        if (raw.size() != base.size() * 4)
            throw Error(std::format("{}: expected {} bytes for {}x{} float32 heights, found {}", hf.string(), base.size() * 4, n, n, raw.size()));
        std::memcpy(base.data(), raw.data(), raw.size());
    }
    if (pfs.size() == 2 && pfs[0].extension() == ".png") {
        for (int k = 0; k < 2; ++k) {
            if (!std::filesystem::exists(pfs[k])) continue;
            int w = 0, h = 0, c = 0;
            stbi_uc* px = stbi_load(pfs[k].string().c_str(), &w, &h, &c, 4);
            if (!px || (uint32_t)w != n || (uint32_t)h != n) { if (px) stbi_image_free(px); throw Error("bad paint image " + pfs[k].string()); }
            for (uint32_t j = 0; j < n; ++j) {
                uint32_t row = northFirst ? n - 1 - j : j;
                for (uint32_t i = 0; i < n; ++i)
                    std::memcpy(&paint[((size_t)j * n + i) * kPaintChannels + k * 4], &px[((size_t)row * n + i) * 4], 4);
            }
            stbi_image_free(px);
        }
    } else if (pfs.size() == 1 && std::filesystem::exists(pfs[0])) {
        auto p = readBinary(pfs[0]);
        if (p.size() != paint.size()) throw Error(std::format("{}: expected {} bytes of paint layers, found {}", pfs[0].string(), paint.size(), p.size()));
        std::memcpy(paint.data(), p.data(), p.size());
    }
    height = base;
    touch();
}

void Terrain::flip(bool fx, bool fy) {
    if (!fx && !fy) return;
    auto flipArr = [&](auto& a, size_t stride) {
        auto src = a;
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t si = fx ? n - 1 - i : i, sj = fy ? n - 1 - j : j;
                std::memcpy(&a[((size_t)j * n + i) * stride], &src[((size_t)sj * n + si) * stride], sizeof(a[0]) * stride);
            }
    };
    flipArr(base, 1);
    flipArr(paint, kPaintChannels);
    height = base;
    touch();
}

json Terrain::importHeightmap(const std::filesystem::path& file, const json& p) {
    if (!std::filesystem::exists(file)) throw Error("no such file: " + file.string());
    std::vector<float> v;   // 0..1
    uint32_t w = 0, h = 0;
    std::string ext = file.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".png") {
        int iw, ih, ic;
        stbi_us* px = stbi_load_16(file.string().c_str(), &iw, &ih, &ic, 1);
        if (!px) throw Error("cannot read " + file.string());
        w = (uint32_t)iw; h = (uint32_t)ih;
        v.resize((size_t)w * h);
        for (size_t i = 0; i < v.size(); ++i) v[i] = px[i] / 65535.0f;
        stbi_image_free(px);
    } else if (ext == ".r16" || ext == ".raw") {
        auto raw = readBinary(file);
        size_t count = raw.size() / 2;
        w = h = (uint32_t)std::lround(std::sqrt((double)count));
        if ((size_t)w * h != count) throw Error("raw 16-bit heightmaps must be square");
        v.resize(count);
        for (size_t i = 0; i < count; ++i) v[i] = (raw[i * 2] | (raw[i * 2 + 1] << 8)) / 65535.0f;
    } else {
        throw Error("heightmaps must be .png (16-bit) or .r16 / .raw");
    }
    if (w != h) throw Error(std::format("the heightmap is {}x{}: crop or pad it to a square first", w, h));
    float size = num(p, "size_m", (float)(w - 1));
    vec2 range(0, 100);
    if (p.contains("height_range_m")) range = vec2(p["height_range_m"][0].get<float>(), p["height_range_m"][1].get<float>());
    vec2 org = p.contains("origin") ? vec2(p["origin"][0].get<float>(), p["origin"][1].get<float>()) : vec2(-size * 0.5f);
    create(w, size / (w - 1), org);
    for (size_t i = 0; i < v.size(); ++i) base[i] = range.x + (range.y - range.x) * v[i];
    // images run top row first; the engine's rows run from the south (low y)
    flip(p.value("flip_x", false), !p.value("flip_y", false));
    return summary();
}

vec2 Terrain::save(const std::filesystem::path& hf, const std::vector<std::filesystem::path>& pfs) const {
    std::filesystem::create_directories(hf.parent_path());
    vec2 range(0, 1);
    if (hf.extension() == ".png") {
        float lo = 1e30f, hi = -1e30f;
        for (float h : base) { lo = std::min(lo, h); hi = std::max(hi, h); }
        hi = std::max(hi, lo + 0.01f);
        range = vec2(lo, hi);
        std::vector<uint16_t> q(base.size());
        for (uint32_t j = 0; j < n; ++j)   // north (top row) first
            for (uint32_t i = 0; i < n; ++i)
                q[(size_t)(n - 1 - j) * n + i] = (uint16_t)std::lround((base[(size_t)j * n + i] - lo) / (hi - lo) * 65535.0f);
        if (!writeFile(hf, encodePng16(q.data(), n, n))) throw Error("cannot write " + hf.string());
    } else {
        std::ofstream h(hf, std::ios::binary);
        h.write((const char*)base.data(), (std::streamsize)(base.size() * 4));
        if (!h) throw Error("cannot write " + hf.string());
    }
    if (pfs.size() == 2) {
        for (int k = 0; k < 2; ++k) {
            std::vector<uint8_t> img(base.size() * 4);
            for (uint32_t j = 0; j < n; ++j)
                for (uint32_t i = 0; i < n; ++i)
                    std::memcpy(&img[((size_t)(n - 1 - j) * n + i) * 4], &paint[((size_t)j * n + i) * kPaintChannels + k * 4], 4);
            if (!writeFile(pfs[k], encodePng(img.data(), n, n))) throw Error("cannot write " + pfs[k].string());
        }
    } else if (pfs.size() == 1) {
        std::ofstream p(pfs[0], std::ios::binary);
        p.write((const char*)paint.data(), (std::streamsize)paint.size());
    }
    return range;
}

json Terrain::summary() const {
    if (empty()) return nullptr;
    float lo = 1e30f, hi = -1e30f;
    double sum = 0;
    for (float h : height) { lo = std::min(lo, h); hi = std::max(hi, h); sum += h; }
    auto r1 = [](float v) { return std::round(v * 10.0f) / 10.0f; };
    return {{"size_m", size()}, {"spacing_m", spacing}, {"samples_per_side", n},
            {"min_corner", {origin.x, origin.y}}, {"max_corner", {maxCorner().x, maxCorner().y}},
            {"height_min_m", r1(lo)}, {"height_max_m", r1(hi)}, {"height_mean_m", r1((float)(sum / height.size()))},
            {"water_level_m", waterLevel}, {"snowline_m", snowline}, {"rock_slope_deg", rockSlopeDeg}};
}

json Terrain::heightGrid(uint32_t cells, Area area) const {
    if (empty()) return nullptr;
    cells = std::clamp(cells, 2u, 64u);
    vec2 lo = origin, hi = maxCorner();
    if (area.kind != Area::Kind::All) { area.bounds(lo, hi); lo = glm::max(lo, origin); hi = glm::min(hi, maxCorner()); }
    vec2 cell = (hi - lo) / (float)cells;
    json rows = json::array();
    for (uint32_t r = 0; r < cells; ++r) {
        json row = json::array();
        for (uint32_t c = 0; c < cells; ++c) {
            vec2 p = lo + (vec2(c, r) + 0.5f) * cell;
            row.push_back(std::round(heightAt(p.x, p.y) * 10.0f) / 10.0f);
        }
        rows.push_back(row);
    }
    return {{"note", "rows run south (low y) to north; values are ground heights (m) at cell centres"},
            {"min_corner", {lo.x, lo.y}}, {"cell_m", {cell.x, cell.y}}, {"rows", rows}};
}
}  // namespace df
