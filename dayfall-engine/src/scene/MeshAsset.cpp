#include "scene/MeshAsset.h"
#include <meshoptimizer.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>

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

namespace {
// keeps whole connected pieces (leaf cards, grass blades): a hash of each piece picks `ratio` of them
// Thins foliage for a far LOD: keeps whole cards (connected pieces) with probability `ratio` and grows each kept
// card about its centre by 1/sqrt(ratio), so the canopy covers about the same area from afar instead of turning
// into bare sticks. Kept cards get their own vertices (appended to m.vertices).
std::vector<uint32_t> thinCards(MeshAsset& m, size_t first, size_t count, float ratio) {
    const uint32_t* idx = &m.indices[first];
    std::unordered_map<uint32_t, uint32_t> parent;
    std::function<uint32_t(uint32_t)> find = [&](uint32_t v) {
        auto it = parent.find(v);
        if (it == parent.end()) { parent[v] = v; return v; }
        uint32_t r = v;
        while (parent[r] != r) r = parent[r];
        while (parent[v] != r) { uint32_t n = parent[v]; parent[v] = r; v = n; }
        return r;
    };
    for (size_t t = 0; t + 2 < count; t += 3) {
        uint32_t a = find(idx[t]), b = find(idx[t + 1]), c = find(idx[t + 2]);
        parent[b] = a;
        parent[find(c)] = a;
    }
    auto keep = [&](uint32_t r) {
        uint32_t h = r * 2654435761u;
        h ^= h >> 15;
        return (h & 0xFFFF) < (uint32_t)(ratio * 65536.0f);
    };
    std::unordered_map<uint32_t, vec4> centre;   // xyz sum, count
    for (size_t t = 0; t + 2 < count; t += 3) {
        uint32_t r = find(idx[t]);
        if (!keep(r)) continue;
        vec4& c = centre[r];
        for (int k = 0; k < 3; ++k) c += vec4(vec3(m.vertices[idx[t + k]].p0), 1.0f);
    }
    float grow = std::min(1.0f / std::sqrt(std::max(ratio, 0.05f)), 3.5f);
    std::unordered_map<uint32_t, uint32_t> remap;
    std::vector<uint32_t> out;
    for (size_t t = 0; t + 2 < count; t += 3) {
        uint32_t r = find(idx[t]);
        if (!keep(r)) continue;
        vec4 c4 = centre[r];
        vec3 c = vec3(c4) / c4.w;
        for (int k = 0; k < 3; ++k) {
            uint32_t v = idx[t + k];
            auto it = remap.find(v);
            if (it == remap.end()) {
                GpuVertex nv = m.vertices[v];
                vec3 p = c + (vec3(nv.p0) - c) * grow;
                nv.p0 = vec4(p, nv.p0.w);
                it = remap.emplace(v, (uint32_t)m.vertices.size()).first;
                m.vertices.push_back(nv);
            }
            out.push_back(it->second);
        }
    }
    return out;
}
}  // namespace

void generateLods(MeshAsset& m, float lod0Distance, const std::vector<LodSpec>& specs, const std::vector<std::string>& foliage) {
    if (specs.empty() || m.lods.empty()) return;
    m.lods.resize(1);
    m.lodDistances = {lod0Distance};
    std::vector<MeshPart> base = m.lods[0];
    for (const LodSpec& l : specs) {
        std::vector<MeshPart> lod;
        for (const MeshPart& src : base) {
            if (l.dropFoliage && std::find(foliage.begin(), foliage.end(), src.material) != foliage.end()) {
                std::vector<uint32_t> kept = thinCards(m, src.firstIndex, src.indexCount, l.ratio);
                if (kept.empty()) continue;
                MeshPart p = src;
                p.firstIndex = (uint32_t)m.indices.size();
                p.indexCount = (uint32_t)kept.size();
                m.indices.insert(m.indices.end(), kept.begin(), kept.end());
                lod.push_back(p);
                continue;
            }
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
