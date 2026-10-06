#include "scene/MeshAsset.h"
#include <meshoptimizer.h>
#include <algorithm>
#include <cstring>

namespace df {
uint32_t packColor(vec4 c) {
    c = glm::clamp(c, vec4(0.0f), vec4(1.0f));
    return (uint32_t)(c.r * 255.0f + 0.5f) | ((uint32_t)(c.g * 255.0f + 0.5f) << 8) | ((uint32_t)(c.b * 255.0f + 0.5f) << 16) |
           ((uint32_t)(c.a * 255.0f + 0.5f) << 24);
}

GpuVertex makeVertex(vec3 p, vec3 n, vec2 uv, vec4 tangent, vec4 color0, vec4 color1, vec2 uv1) {
    GpuVertex v;
    v.p0 = vec4(p, uv.x);
    v.p1 = vec4(n, uv.y);
    v.tangent = tangent;
    uint32_t a = packColor(color0), b = packColor(color1);
    float fa, fb;
    std::memcpy(&fa, &a, 4);
    std::memcpy(&fb, &b, 4);
    v.p3 = vec4(uv1, fa, fb);
    return v;
}

size_t MeshAsset::triangleCount() const {
    size_t n = 0;
    if (!lods.empty())
        for (auto& p : lods[0]) n += p.indexCount / 3;
    return n;
}

void MeshAsset::computeBounds() {
    vec3 lo(1e30f), hi(-1e30f);
    for (auto& v : vertices) { lo = glm::min(lo, vec3(v.p0)); hi = glm::max(hi, vec3(v.p0)); }
    if (vertices.empty()) lo = hi = vec3(0);
    aabbMin = lo;
    aabbMax = hi;
    vec3 c = (lo + hi) * 0.5f;
    float r = 0.0f;
    for (auto& v : vertices) r = std::max(r, glm::length(vec3(v.p0) - c));
    bounds = vec4(c, std::max(r, 0.01f));
}

void MeshBuilder::tri(const std::string& material, uint32_t a, uint32_t b, uint32_t c) {
    auto it = std::find_if(groups_.begin(), groups_.end(), [&](auto& g) { return g.first == material; });
    if (it == groups_.end()) { groups_.push_back({material, {}}); it = groups_.end() - 1; }
    it->second.insert(it->second.end(), {a, b, c});
}

void MeshBuilder::polygon(const std::string& material, const std::vector<vec3>& pts, vec2 uvScale) {
    if (pts.size() < 3) return;
    vec3 n(0);
    for (size_t i = 0; i < pts.size(); ++i) n += glm::cross(pts[i], pts[(i + 1) % pts.size()]);
    n = glm::length(n) > 1e-12f ? glm::normalize(n) : vec3(0, 0, 1);
    // planar UVs in the polygon's plane (metre scale)
    vec3 t = std::abs(n.z) > 0.9f ? vec3(1, 0, 0) : glm::normalize(glm::cross(vec3(0, 0, 1), n));
    vec3 bt = glm::cross(n, t);
    uint32_t base = (uint32_t)verts_.size();
    for (auto& p : pts) verts_.push_back(makeVertex(p, n, vec2(glm::dot(p, t), glm::dot(p, bt)) * uvScale, vec4(t, 1)));
    for (uint32_t i = 1; i + 1 < pts.size(); ++i) tri(material, base, base + i, base + i + 1);
}

MeshAsset MeshBuilder::build(const std::string& name) {
    MeshAsset m;
    m.name = name;
    m.vertices = std::move(verts_);
    m.lods.resize(1);
    for (auto& [mat, idx] : groups_) {
        MeshPart p;
        p.firstIndex = (uint32_t)m.indices.size();
        p.indexCount = (uint32_t)idx.size();
        p.material = mat;
        m.indices.insert(m.indices.end(), idx.begin(), idx.end());
        m.lods[0].push_back(p);
    }
    m.lodDistances = {1e9f};
    m.computeBounds();
    verts_.clear();
    groups_.clear();
    return m;
}

void generateLods(MeshAsset& m, float lod0Distance, const std::vector<LodSpec>& specs) {
    if (specs.empty() || m.lods.empty()) return;
    m.lods.resize(1);
    m.lodDistances = {lod0Distance};
    std::vector<MeshPart> base = m.lods[0];
    for (const LodSpec& l : specs) {
        std::vector<MeshPart> lod;
        for (const MeshPart& src : base) {
            size_t target = std::max<size_t>(3, (size_t)(src.indexCount * l.ratio) / 3 * 3);
            std::vector<uint32_t> out(src.indexCount);
            float err = 0.0f;
            const float* pos = &m.vertices[0].p0.x;
            size_t count = l.sloppy
                ? meshopt_simplifySloppy(out.data(), &m.indices[src.firstIndex], src.indexCount, pos, m.vertices.size(),
                                         sizeof(GpuVertex), target, 0.05f, &err)
                : meshopt_simplify(out.data(), &m.indices[src.firstIndex], src.indexCount, pos, m.vertices.size(),
                                   sizeof(GpuVertex), target, 0.02f, meshopt_SimplifyLockBorder, &err);
            if (count == 0) continue;
            MeshPart p = src;
            p.firstIndex = (uint32_t)m.indices.size();
            p.indexCount = (uint32_t)count;
            m.indices.insert(m.indices.end(), out.begin(), out.begin() + count);
            lod.push_back(p);
        }
        if (!lod.empty()) {
            m.lods.push_back(std::move(lod));
            m.lodDistances.push_back(l.distance);
        }
    }
}

void computeNormals(MeshAsset& m) {
    std::vector<vec3> acc(m.vertices.size(), vec3(0));
    if (m.lods.empty()) return;
    for (const MeshPart& p : m.lods[0])
        for (uint32_t i = p.firstIndex; i + 2 < p.firstIndex + p.indexCount; i += 3) {
            uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
            vec3 pa = m.vertices[a].p0, pb = m.vertices[b].p0, pc = m.vertices[c].p0;
            vec3 fn = glm::cross(pb - pa, pc - pa);
            acc[a] += fn; acc[b] += fn; acc[c] += fn;
        }
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        vec3 n = glm::length(acc[i]) > 0 ? glm::normalize(acc[i]) : vec3(0, 0, 1);
        m.vertices[i].p1 = vec4(n, m.vertices[i].p1.w);
    }
}
}  // namespace df
