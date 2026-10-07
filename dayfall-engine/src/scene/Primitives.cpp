#include "scene/Primitives.h"
#include "core/Error.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <format>

namespace df {
using json = nlohmann::json;
namespace {
constexpr float kPi = 3.14159265358979f;

// a flat quad with its own vertices; uv in metres along the two edges
void face(MeshBuilder& b, const std::string& mat, vec3 o, vec3 u, vec3 v) {
    vec3 n = glm::normalize(glm::cross(u, v));
    vec4 t(glm::normalize(u), 1.0f);
    float lu = glm::length(u), lv = glm::length(v);
    uint32_t a = b.vertex(makeVertex(o, n, vec2(0, 0), t));
    uint32_t c = b.vertex(makeVertex(o + u, n, vec2(lu, 0), t));
    uint32_t d = b.vertex(makeVertex(o + u + v, n, vec2(lu, lv), t));
    uint32_t e = b.vertex(makeVertex(o + v, n, vec2(0, lv), t));
    b.quad(mat, a, c, d, e);
}

void boxInto(MeshBuilder& b, vec3 lo, vec3 hi, const std::string& mat) {
    vec3 s = hi - lo;
    face(b, mat, {lo.x, lo.y, lo.z}, {s.x, 0, 0}, {0, 0, s.z});            // -Y
    face(b, mat, {hi.x, hi.y, lo.z}, {-s.x, 0, 0}, {0, 0, s.z});           // +Y
    face(b, mat, {hi.x, lo.y, lo.z}, {0, s.y, 0}, {0, 0, s.z});            // +X
    face(b, mat, {lo.x, hi.y, lo.z}, {0, -s.y, 0}, {0, 0, s.z});           // -X
    face(b, mat, {lo.x, lo.y, hi.z}, {s.x, 0, 0}, {0, s.y, 0});            // top
    face(b, mat, {lo.x, hi.y, lo.z}, {s.x, 0, 0}, {0, -s.y, 0});           // bottom
}

// Generators lay v out upward (walls) and northward (floors); images are stored top row first, so the build flips
// v (and the tangent frame's handedness with it) to show textures the right way up.
MeshAsset finish(MeshBuilder& b, const std::string& name) {
    for (uint32_t i = 0; i < b.vertexCount(); ++i) {
        GpuVertex& v = b.at(i);
        v.p1.w = -v.p1.w;
        v.tangent.w = -v.tangent.w;
    }
    return b.build(name);
}

vec3 v3(const json& j, vec3 def) {
    if (j.is_number()) return vec3(j.get<float>());
    return j.is_array() && j.size() >= 3 ? vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>()) : def;
}
vec2 v2(const json& j, vec2 def) {
    if (j.is_number()) return vec2(j.get<float>());
    return j.is_array() && j.size() >= 2 ? vec2(j[0].get<float>(), j[1].get<float>()) : def;
}
float num(const json& j, const char* k, float def) { return j.contains(k) && j[k].is_number() ? j[k].get<float>() : def; }
void positive(float v, const char* what) {
    if (!(v > 0.0f) || v > 10000.0f) throw Error(std::format("primitive {} must be between 0 and 10000 m", what));
}
}  // namespace

MeshAsset makeBox(vec3 size, const std::string& material) {
    MeshBuilder b;
    boxInto(b, vec3(-size.x * 0.5f, -size.y * 0.5f, 0.0f), vec3(size.x * 0.5f, size.y * 0.5f, size.z), material);
    return finish(b, "box");
}

MeshAsset makePlane(vec2 size, const std::string& material, uint32_t sub) {
    MeshBuilder b;
    sub = std::max(1u, sub);
    for (uint32_t y = 0; y <= sub; ++y)
        for (uint32_t x = 0; x <= sub; ++x) {
            vec2 p = vec2((float)x / sub - 0.5f, (float)y / sub - 0.5f) * size;
            b.vertex(makeVertex(vec3(p, 0), vec3(0, 0, 1), p + size * 0.5f, vec4(1, 0, 0, 1)));
        }
    for (uint32_t y = 0; y < sub; ++y)
        for (uint32_t x = 0; x < sub; ++x) {
            uint32_t i = y * (sub + 1) + x;
            b.quad(material, i, i + 1, i + sub + 2, i + sub + 1);
        }
    return finish(b, "plane");
}

MeshAsset makeCylinder(float r, float h, uint32_t seg, const std::string& material, bool caps) {
    MeshBuilder b;
    seg = std::max(3u, seg);
    float circ = 2.0f * kPi * r;
    for (uint32_t i = 0; i <= seg; ++i) {
        float a = 2.0f * kPi * i / seg;
        vec3 n(std::cos(a), std::sin(a), 0);
        vec4 t(-n.y, n.x, 0, 1);
        b.vertex(makeVertex(n * r, n, vec2(circ * i / seg, 0), t));
        b.vertex(makeVertex(n * r + vec3(0, 0, h), n, vec2(circ * i / seg, h), t));
    }
    for (uint32_t i = 0; i < seg; ++i) b.quad(material, i * 2, i * 2 + 2, i * 2 + 3, i * 2 + 1);
    if (caps) {
        for (int top = 0; top < 2; ++top) {
            float z = top ? h : 0.0f;
            vec3 n(0, 0, top ? 1.0f : -1.0f);
            uint32_t c = b.vertex(makeVertex(vec3(0, 0, z), n, vec2(0), vec4(1, 0, 0, 1)));
            uint32_t first = (uint32_t)b.vertexCount();
            for (uint32_t i = 0; i <= seg; ++i) {
                float a = 2.0f * kPi * i / seg;
                vec3 p(std::cos(a) * r, std::sin(a) * r, z);
                b.vertex(makeVertex(p, n, vec2(p), vec4(1, 0, 0, 1)));
            }
            for (uint32_t i = 0; i < seg; ++i) {
                if (top) b.tri(material, c, first + i, first + i + 1);
                else b.tri(material, c, first + i + 1, first + i);
            }
        }
    }
    return finish(b, "cylinder");
}

MeshAsset makeCone(float r, float h, uint32_t seg, const std::string& material) {
    MeshBuilder b;
    seg = std::max(3u, seg);
    float slant = std::sqrt(r * r + h * h);
    for (uint32_t i = 0; i < seg; ++i) {
        float a0 = 2.0f * kPi * i / seg, a1 = 2.0f * kPi * (i + 1) / seg, am = (a0 + a1) * 0.5f;
        auto nrm = [&](float a) { return glm::normalize(vec3(std::cos(a) * h, std::sin(a) * h, r)); };
        uint32_t p0 = b.vertex(makeVertex(vec3(std::cos(a0) * r, std::sin(a0) * r, 0), nrm(a0), vec2(r * a0, 0)));
        uint32_t p1 = b.vertex(makeVertex(vec3(std::cos(a1) * r, std::sin(a1) * r, 0), nrm(a1), vec2(r * a1, 0)));
        uint32_t tip = b.vertex(makeVertex(vec3(0, 0, h), nrm(am), vec2(r * am, slant)));
        b.tri(material, p0, p1, tip);
    }
    uint32_t c = b.vertex(makeVertex(vec3(0), vec3(0, 0, -1), vec2(0)));
    uint32_t first = (uint32_t)b.vertexCount();
    for (uint32_t i = 0; i <= seg; ++i) {
        float a = 2.0f * kPi * i / seg;
        vec3 p(std::cos(a) * r, std::sin(a) * r, 0);
        b.vertex(makeVertex(p, vec3(0, 0, -1), vec2(p)));
    }
    for (uint32_t i = 0; i < seg; ++i) b.tri(material, c, first + i + 1, first + i);
    return finish(b, "cone");
}

MeshAsset makeSphere(float r, uint32_t seg, const std::string& material) {
    MeshBuilder b;
    seg = std::max(4u, seg);
    uint32_t rings = seg / 2;
    for (uint32_t y = 0; y <= rings; ++y)
        for (uint32_t x = 0; x <= seg; ++x) {
            float th = kPi * y / rings, ph = 2.0f * kPi * x / seg;
            vec3 n(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), -std::cos(th));
            b.vertex(makeVertex(n * r + vec3(0, 0, r), n, vec2(2.0f * kPi * r * x / seg, kPi * r * y / rings),
                                vec4(-std::sin(ph), std::cos(ph), 0, 1)));
        }
    for (uint32_t y = 0; y < rings; ++y)
        for (uint32_t x = 0; x < seg; ++x) {
            uint32_t i = y * (seg + 1) + x;
            b.quad(material, i, i + 1, i + seg + 2, i + seg + 1);
        }
    return finish(b, "sphere");
}

MeshAsset makeCapsule(float r, float h, uint32_t seg, const std::string& material) {
    MeshBuilder b;
    seg = std::max(4u, seg);
    h = std::max(h, 2.0f * r);
    uint32_t half = seg / 4;   // rings per hemisphere
    std::vector<std::pair<float, float>> rings;   // (z of ring centre, polar angle)
    for (uint32_t i = 0; i <= half; ++i) rings.push_back({r, kPi * 0.5f * i / half});                 // bottom cap
    for (uint32_t i = 0; i <= half; ++i) rings.push_back({h - r, kPi * 0.5f + kPi * 0.5f * i / half}); // top cap
    for (uint32_t y = 0; y < rings.size(); ++y)
        for (uint32_t x = 0; x <= seg; ++x) {
            float th = rings[y].second, ph = 2.0f * kPi * x / seg;
            vec3 n(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), -std::cos(th));
            b.vertex(makeVertex(n * r + vec3(0, 0, rings[y].first), n, vec2(2.0f * kPi * r * x / seg, rings[y].first - n.z * r)));
        }
    for (uint32_t y = 0; y + 1 < rings.size(); ++y)
        for (uint32_t x = 0; x < seg; ++x) {
            uint32_t i = y * (seg + 1) + x;
            b.quad(material, i, i + 1, i + seg + 2, i + seg + 1);
        }
    return finish(b, "capsule");
}

MeshAsset makeRamp(vec3 s, const std::string& material) {
    MeshBuilder b;
    float x0 = -s.x * 0.5f, x1 = s.x * 0.5f, y0 = -s.y * 0.5f, y1 = s.y * 0.5f;
    b.polygon(material, {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, s.z}, {x0, y1, s.z}});   // slope
    b.polygon(material, {{x0, y1, 0}, {x0, y1, s.z}, {x1, y1, s.z}, {x1, y1, 0}});   // back
    b.polygon(material, {{x0, y0, 0}, {x0, y1, s.z}, {x0, y1, 0}});                  // sides
    b.polygon(material, {{x1, y0, 0}, {x1, y1, 0}, {x1, y1, s.z}});
    b.polygon(material, {{x0, y0, 0}, {x0, y1, 0}, {x1, y1, 0}, {x1, y0, 0}});       // bottom
    return finish(b, "ramp");
}

MeshAsset makeStairs(vec3 s, uint32_t steps, const std::string& material) {
    MeshBuilder b;
    steps = std::max(1u, steps);
    float d = s.y / steps, h = s.z / steps;
    for (uint32_t i = 0; i < steps; ++i)
        boxInto(b, vec3(-s.x * 0.5f, -s.y * 0.5f + d * i, 0.0f), vec3(s.x * 0.5f, -s.y * 0.5f + d * (i + 1), h * (i + 1)), material);
    return finish(b, "stairs");
}

MeshAsset makeGem(float size, const std::string& material) {
    MeshBuilder b;
    float r = size * 0.5f, hz = size * 0.75f;
    const int n = 6;
    for (int i = 0; i < n; ++i) {
        float a0 = 2.0f * kPi * i / n, a1 = 2.0f * kPi * (i + 1) / n;
        vec3 p0(std::cos(a0) * r, std::sin(a0) * r, 0), p1(std::cos(a1) * r, std::sin(a1) * r, 0);
        b.polygon(material, {p0, p1, vec3(0, 0, hz)});
        b.polygon(material, {p1, p0, vec3(0, 0, -hz)});
    }
    return finish(b, "gem");
}

std::string primitiveKey(const json& spec) { return "prim:" + spec.dump(); }

bool makePrimitive(const json& spec, MeshAsset& out) {
    std::string type = spec.value("type", "");
    std::string mat = spec.value("material", "default");
    uint32_t seg = (uint32_t)std::clamp(num(spec, "segments", 24.0f), 3.0f, 128.0f);
    if (type == "box") {
        vec3 s = v3(spec.value("size", json()), vec3(1));
        positive(s.x, "size.x"); positive(s.y, "size.y"); positive(s.z, "size.z");
        out = makeBox(s, mat);
    } else if (type == "plane") {
        vec2 s = v2(spec.value("size", json()), vec2(10));
        positive(s.x, "size.x"); positive(s.y, "size.y");
        out = makePlane(s, mat, (uint32_t)std::clamp(num(spec, "subdivisions", 1.0f), 1.0f, 256.0f));
    } else if (type == "cylinder") {
        float r = num(spec, "radius", 0.5f), h = num(spec, "height", 1.0f);
        positive(r, "radius"); positive(h, "height");
        out = makeCylinder(r, h, seg, mat);
    } else if (type == "cone") {
        float r = num(spec, "radius", 0.5f), h = num(spec, "height", 1.0f);
        positive(r, "radius"); positive(h, "height");
        out = makeCone(r, h, seg, mat);
    } else if (type == "sphere") {
        float r = num(spec, "radius", 0.5f);
        positive(r, "radius");
        out = makeSphere(r, seg, mat);
    } else if (type == "capsule") {
        float r = num(spec, "radius", 0.35f), h = num(spec, "height", 1.8f);
        positive(r, "radius"); positive(h, "height");
        out = makeCapsule(r, h, seg, mat);
    } else if (type == "ramp") {
        vec3 s = v3(spec.value("size", json()), vec3(2, 4, 1));
        positive(s.x, "size.x"); positive(s.y, "size.y"); positive(s.z, "size.z");
        out = makeRamp(s, mat);
    } else if (type == "stairs") {
        vec3 s = v3(spec.value("size", json()), vec3(2, 3, 1.5f));
        positive(s.x, "size.x"); positive(s.y, "size.y"); positive(s.z, "size.z");
        out = makeStairs(s, (uint32_t)std::clamp(num(spec, "steps", std::round(s.z / 0.18f)), 1.0f, 200.0f), mat);
    } else if (type == "gem") {
        float s = num(spec, "size", 0.6f);
        positive(s, "size");
        out = makeGem(s, mat);
    } else {
        return false;
    }
    out.name = primitiveKey(spec);
    return true;
}

json primitiveCatalog() {
    return json::array({
        {{"type", "box"}, {"params", "size [x,y,z] m (default 1), material"}, {"origin", "bottom centre"}},
        {{"type", "plane"}, {"params", "size [x,y] m, subdivisions, material"}, {"origin", "centre"}},
        {{"type", "cylinder"}, {"params", "radius, height, segments, material"}, {"origin", "bottom centre"}},
        {{"type", "cone"}, {"params", "radius, height, segments, material"}, {"origin", "bottom centre"}},
        {{"type", "sphere"}, {"params", "radius, segments, material"}, {"origin", "bottom (resting on the ground)"}},
        {{"type", "capsule"}, {"params", "radius, height, segments, material"}, {"origin", "bottom centre"}},
        {{"type", "ramp"}, {"params", "size [x,y,z]: rises along +Y to height z, material"}, {"origin", "bottom centre"}},
        {{"type", "stairs"}, {"params", "size [x,y,z], steps (default z/0.18), material; climbs along +Y"}, {"origin", "bottom centre"}},
        {{"type", "gem"}, {"params", "size, material"}, {"origin", "centre"}},
    });
}
}  // namespace df
