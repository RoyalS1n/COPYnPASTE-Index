#include "world/SkyOcclusion.h"
#include "core/Log.h"
#include "world/Terrain.h"
#include <glm/gtx/component_wise.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

// How it works. Static instances with opaque lit / courses / rock surfaces are
// occluders (vegetation, grass, water and anything under kMinRadius are not).
// Geometry standing kTallM above the terrain is binned into 2D tiles; clusters
// of such tiles become regions (padded boxes), the best first, at most
// kMaxSkyRegions, with cell sizes grown until the cell budget fits. Each region
// is voxelized (conservative triangle / box tests; cells under the terrain are
// ground), then traced in K directions spread over the sphere. A direction is
// one sweep through the volume: a cell's transmittance is the bilinear mix of
// its upstream neighbours one step along the ray, so every cell's ray is
// marched to the region's edge with O(1) work per cell (the interpolation makes
// each ray a thin cone). Rays leaving the region are open. Upward rays stop at
// solids; downward rays stop at solids and ground and take the sky exposure of
// the surface they hit (the light it bounces), so a courtyard floor counts as
// lit ground and a hall floor does not.
namespace df {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxCells = 4u << 20;   // all regions together: 32 MB (8 bytes a cell)
constexpr uint32_t kMaxDim = 384;        // cells per axis
constexpr float kMinRadius = 1.5f;       // smaller instances (grass, flowers, pebbles) never occlude
constexpr float kTallM = 2.0f;           // geometry this high above the ground can shelter someone
constexpr float kRockWeight = 0.33f;     // boulders and outcrops shelter less than walls of the same area
constexpr float kMinScore = 150.0f;      // m2 of tall geometry for a region
constexpr float kPadM = 4.0f;            // margin: a structure darkens the ground around it

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
uint64_t hmix(uint64_t h, uint64_t v) { return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))) * 0x100000001b3ull; }
uint64_t hf(uint64_t h, float f) { uint32_t b; std::memcpy(&b, &f, 4); return hmix(h, b); }
uint64_t hv(uint64_t h, vec3 v) { return hf(hf(hf(h, v.x), v.y), v.z); }

struct Box {
    vec3 lo{1e30f}, hi{-1e30f};
    void add(vec3 p) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    void add(const Box& b) { lo = glm::min(lo, b.lo); hi = glm::max(hi, b.hi); }
    bool overlaps(const Box& b) const { return glm::all(glm::lessThanEqual(lo, b.hi)) && glm::all(glm::lessThanEqual(b.lo, hi)); }
};

// a static instance whose opaque surfaces block the sky
struct Occluder {
    const std::vector<uint32_t>* parts;   // LOD0 submeshes that occlude
    vec3 pos, scale;
    quat q;
    Box box;
    uint64_t key;
    vec3 world(const Scene& s, uint32_t vertex) const { return pos + q * (scale * vec3(s.vertices[vertex].p0)); }
};

// calls fn(a, b, c) for triangles [first, first + count) of submesh sm of o, in world space
template <typename F> void triangles(const Scene& s, const Occluder& o, uint32_t sm, uint32_t first, uint32_t count, F&& fn) {
    const Submesh& p = s.submeshes[sm];
    const uint32_t* idx = &s.indices[p.firstIndex];
    for (uint32_t i = first * 3, e = std::min(p.indexCount / 3, first + count) * 3; i < e; i += 3)
        fn(o.world(s, p.vertexOffset + idx[i]), o.world(s, p.vertexOffset + idx[i + 1]), o.world(s, p.vertexOffset + idx[i + 2]));
}

// Akenine-Moller triangle / box overlap (separating axes); vertices relative to the box centre, h = half size
bool triBox(vec3 a, vec3 b, vec3 c, vec3 h) {
    vec3 e0 = b - a, e1 = c - b, e2 = a - c;
    vec3 n = glm::cross(e0, e1);
    if (std::abs(glm::dot(n, a)) > glm::dot(glm::abs(n), h)) return false;
    for (int k = 0; k < 3; ++k)
        if (std::min({a[k], b[k], c[k]}) > h[k] || std::max({a[k], b[k], c[k]}) < -h[k]) return false;
    for (vec3 e : {e0, e1, e2})
        for (vec3 ax : {vec3(0, -e.z, e.y), vec3(e.z, 0, -e.x), vec3(-e.y, e.x, 0)}) {
            float p0 = glm::dot(ax, a), p1 = glm::dot(ax, b), p2 = glm::dot(ax, c), r = glm::dot(glm::abs(ax), h);
            if (std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r) return false;
        }
    return true;
}

void parallel(uint32_t n, const std::function<void(uint32_t)>& fn) {
    std::vector<std::thread> t;
    for (uint32_t i = 1; i < n; ++i) t.emplace_back(fn, i);
    fn(0);
    for (auto& x : t) x.join();
}

// K directions spread evenly over the sphere (Fibonacci), the upper half first
std::vector<vec3> sphereDirections(uint32_t k) {
    std::vector<vec3> d(k);
    for (uint32_t i = 0; i < k; ++i) {
        float z = 1.0f - (2.0f * i + 1.0f) / k, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.39996323f * i;
        d[i] = vec3(r * std::cos(phi), r * std::sin(phi), z);
    }
    return d;
}

// occ (x fastest): 0 air, 1 ground, 2 solid. Fills r.rgba. Each direction is a task for any thread: it
// sweeps a copy of the volume laid out with its dominant axis k slowest and a contiguous axis a, writes
// 8-bit transmittance to the thread's own volume, then adds that into the shared sums z-slab by slab
// under a lock per slab. The upper half runs first: downward rays need the sky exposure of what they hit.
void trace(SkyRegion& r, const std::vector<uint8_t>& occ, const std::vector<vec3>& dirs, uint32_t threads) {
    const ivec3 n(r.dims);
    const size_t cells = (size_t)n.x * n.y * n.z;
    const uint32_t K = (uint32_t)dirs.size();
    // per cell, the six ambient-cube lobes +x -x +y -y +z -z: sums of the 8-bit transmittance t times the
    // ray's cosine to the lobe axis; norm = the same sums for t = 255 (open), so open is exactly 1
    std::vector<uint16_t> sum(cells * 6, 0);
    float norm[6] = {};
    for (vec3 d : dirs)
        for (int i = 0; i < 3; ++i) norm[i * 2 + (d[i] < 0)] += std::abs(d[i]) * 255.0f;
    auto skyUp = [&](size_t c) { return std::min(sum[c * 6 + 4] / norm[4], 1.0f); };
    auto axes = [](int k) { return ivec2(k == 0 ? 1 : 0, k == 2 ? 1 : 2); };   // a (contiguous), b
    auto index = [&](int k, int x, int y, int z) {
        ivec3 p(x, y, z);
        ivec2 ab = axes(k);
        return (size_t)p[ab.x] + (size_t)n[ab.x] * (p[ab.y] + (size_t)n[ab.y] * p[k]);
    };
    // per layout: what a ray reaching the cell carries on, 255 = it passes (T), else v / 254
    std::array<std::vector<uint8_t>, 3> rule;
    for (auto& v : rule) v.resize(cells);
    const int slabs = std::min<int>(n.z, (int)threads * 4);
    std::vector<std::mutex> locks(slabs);
    std::atomic<uint32_t> next{0};
    for (int phase = 0; phase < 2; ++phase) {
        bool down = phase == 1;
        for (int z = 0; z < n.z; ++z)
            for (int y = 0; y < n.y; ++y)
                for (int x = 0; x < n.x; ++x) {
                    size_t c = x + (size_t)n.x * (y + (size_t)n.y * z), above = c + (size_t)n.x * n.y;
                    // up: ground lets rays through. down: a surface bounces what its top (the air above) sees of the sky
                    bool top = z + 1 < n.z && occ[above] == 0;
                    uint8_t v = !down ? (occ[c] == 2 ? 0 : 255) : occ[c] == 0 ? 255 : (uint8_t)std::lround(skyUp(top ? above : c) * 254.0f);
                    for (int k = 0; k < 3; ++k) rule[k][index(k, x, y, z)] = v;
                }
        next = down ? K / 2 : 0;
        parallel(threads, [&](uint32_t th) {
            std::vector<uint8_t> tv(cells);
            std::vector<float> bufA, bufB;
            for (uint32_t i; (i = next++) < (down ? K : K / 2);) {
                // sweep against d one slice of the dominant axis k at a time
                vec3 d = dirs[i], ad = glm::abs(d);
                int k = ad.x >= ad.y && ad.x >= ad.z ? 0 : ad.y >= ad.z ? 1 : 2;
                int a = axes(k).x, b = axes(k).y, na = n[a], nb = n[b], nk = n[k], pw = na + 2;
                float oa = d[a] / ad[k], ob = d[b] / ad[k];   // upstream offset per step, in cells (-1..1)
                int fa = std::min((int)std::floor(oa), 0), fb = std::min((int)std::floor(ob), 0);
                float ta = oa - fa, tb = ob - fb;
                float w00 = (1 - ta) * (1 - tb), w10 = ta * (1 - tb), w01 = (1 - ta) * tb, w11 = ta * tb;
                bufA.assign((size_t)pw * (nb + 2), 1.0f);   // outside the region everything is open
                bufB.assign(bufA.size(), 1.0f);
                float* prev = bufA.data();
                float* cur = bufB.data();
                const uint8_t* rl = rule[k].data();
                int kc = d[k] > 0 ? nk - 1 : 0, step = d[k] > 0 ? -1 : 1;
                for (int s = 0; s < nk; ++s, kc += step) {
                    for (int vb = 0; vb < nb; ++vb) {
                        const float* p0 = prev + (size_t)(vb + fb + 1) * pw + fa + 1;
                        const float* p1 = p0 + pw;
                        float* out = cur + (size_t)(vb + 1) * pw + 1;
                        size_t base = (size_t)na * (vb + (size_t)nb * kc);
                        const uint8_t* rr = rl + base;
                        uint8_t* tt = tv.data() + base;
                        for (int va = 0; va < na; ++va) {
                            float T = w00 * p0[va] + w10 * p0[va + 1] + w01 * p1[va] + w11 * p1[va + 1];
                            out[va] = rr[va] == 255 ? T : rr[va] * (1.0f / 254.0f);
                            tt[va] = (uint8_t)(T * 255.0f + 0.5f);
                        }
                    }
                    std::swap(prev, cur);
                }
                // add into the sums (x runs contiguously in layouts 1 and 2, with stride ny * nz in layout 0)
                int lx = d.x < 0, ly = 2 + (d.y < 0), lz = 4 + (d.z < 0);
                vec3 w = glm::abs(d);
                size_t dx = k == 0 ? (size_t)n.y * n.z : 1;
                for (int j = 0; j < slabs; ++j) {
                    int slab = (j + (int)(th * slabs / threads)) % slabs;
                    std::lock_guard<std::mutex> lock(locks[slab]);
                    for (int z = slab * n.z / slabs; z < (slab + 1) * n.z / slabs; ++z)
                        for (int y = 0; y < n.y; ++y) {
                            size_t c = (size_t)n.x * (y + (size_t)n.y * z), t = index(k, 0, y, z);
                            for (int x = 0; x < n.x; ++x, ++c, t += dx) {
                                float v = tv[t];
                                uint16_t* sc = &sum[c * 6];
                                sc[lx] += (uint16_t)(v * w.x + 0.5f);
                                sc[ly] += (uint16_t)(v * w.y + 0.5f);
                                sc[lz] += (uint16_t)(v * w.z + 0.5f);
                            }
                        }
                }
            }
        });
    }
    // two texels per cell: (+x -x +y -y) at x, (+z -z) at x + nx
    r.rgba.assign(cells * 8, 0);
    for (size_t c = 0; c < cells; ++c) {
        size_t row = c / n.x, x = c % n.x;
        uint8_t* lo = &r.rgba[(row * 2 * n.x + x) * 4];
        uint8_t* hi = lo + (size_t)n.x * 4;
        for (int l = 0; l < 6; ++l) (l < 4 ? lo[l] : hi[l - 4]) = (uint8_t)std::lround(std::min(sum[c * 6 + l] / norm[l], 1.0f) * 255.0f);
    }
}
}  // namespace

std::shared_ptr<const SkyVolume> SkyOcclusionBuilder::build(const Scene& s, const Terrain& t) {
    const Environment& env = s.env;
    if (!env.skyOcclusion) { last_.reset(); key_ = 0; return nullptr; }
    auto t0 = Clock::now();
    const uint32_t K = std::clamp(env.skyOccRays, 8u, 256u) & ~1u;
    const float cellPref = std::clamp(env.skyOccCell, 0.25f, 4.0f);
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    auto ground = [&](float x, float y) { return t.empty() ? 0.0f : t.heightAt(x, y); };

    // occluding parts of each mesh; vegetation (mostly foliage cards) lets the sky through
    std::vector<std::vector<uint32_t>> parts(s.meshes.size());
    for (size_t m = 0; m < s.meshes.size(); ++m) {
        if (s.meshes[m].lods.empty()) continue;
        size_t occTris = 0, leafTris = 0;
        for (uint32_t sm : s.meshes[m].lods[0].submeshes) {
            const MaterialDef& md = s.materials[s.submeshes[sm].material];
            uint32_t model = md.gpu.h0.x;
            if (model == ModelFoliage || model == ModelGrass) leafTris += s.submeshes[sm].indexCount / 3;
            else if ((md.group == GroupOpaque || md.group == GroupTwoSided) && (model == ModelLit || model == ModelCourses || model == ModelRock)) {
                parts[m].push_back(sm);
                occTris += s.submeshes[sm].indexCount / 3;
            }
        }
        if (leafTris > occTris) parts[m].clear();
    }
    std::vector<Occluder> occ;
    uint64_t key = hf(hmix(K, t.empty()), cellPref);
    for (uint32_t i = 0; i < std::min<size_t>(s.dynamicFirst, s.instances.size()); ++i) {
        const GpuInstance& in = s.instances[i];
        if (in.cullDistance <= 0 || parts[in.mesh].empty()) continue;
        const Mesh& m = s.meshes[in.mesh];
        vec3 sc = s.axisScale(in);
        if (m.bounds.w * glm::compMax(glm::abs(sc)) < kMinRadius) continue;
        Occluder o{&parts[in.mesh], vec3(in.posScale), sc, quat(in.rot.w, in.rot.x, in.rot.y, in.rot.z), {}, 0};
        for (int c = 0; c < 8; ++c)
            o.box.add(o.pos + o.q * (sc * vec3(c & 1 ? m.aabbMax.x : m.aabbMin.x, c & 2 ? m.aabbMax.y : m.aabbMin.y, c & 4 ? m.aabbMax.z : m.aabbMin.z)));
        o.key = hv(hf(hv(hv(hmix(hmix(std::hash<std::string>{}(m.name), m.vertexCount), m.indexCount), vec3(in.posScale)), vec3(in.rot)), in.rot.w), sc);
        key = hmix(key, o.key);
        occ.push_back(o);
    }
    if (occ.empty()) { last_.reset(); key_ = 0; return nullptr; }
    // terrain heights at the cell columns are part of a region's key (environment edits touch the
    // terrain's version without moving it, so the version cannot be the key)
    auto columnHeights = [&](const SkyRegion& r) {
        std::vector<float> h((size_t)r.dims.x * r.dims.y);
        for (uint32_t y = 0; y < r.dims.y; ++y)
            for (uint32_t x = 0; x < r.dims.x; ++x)
                h[(size_t)y * r.dims.x + x] = ground(r.origin.x + (x + 0.5f) * r.cell, r.origin.y + (y + 0.5f) * r.cell);
        return h;
    };
    auto heightsKey = [&](const std::vector<float>& v) {
        uint64_t h = 1;
        for (float f : v) h = hf(h, f);
        return h;
    };
    if (last_ && key == key_) {   // nothing static moved
        bool same = true;
        for (auto& r : last_->regions) same = same && heightsKey(columnHeights(*r)) == r->terrainKey;
        if (same) return last_;
    }

    // 1. regions: tiles holding geometry that stands kTallM above the ground, clustered
    Box all;
    for (auto& o : occ) all.add(o.box);
    float tile = std::max(8.0f, std::max(all.hi.x - all.lo.x, all.hi.y - all.lo.y) / 512.0f);
    int gx = (int)((all.hi.x - all.lo.x) / tile) + 1, gy = (int)((all.hi.y - all.lo.y) / tile) + 1;
    struct Tiles { std::vector<float> score; std::vector<Box> rock, built; };   // per tile: tall area, bounds of rock and the rest
    std::vector<Tiles> part(threads);
    struct Chunk { const Occluder* o; uint32_t sm, first; };   // 4096 triangles of a submesh
    std::vector<Chunk> allChunks;
    for (auto& o : occ)
        for (uint32_t sm : *o.parts)
            for (uint32_t f = 0; f < s.submeshes[sm].indexCount / 3; f += 4096) allChunks.push_back({&o, sm, f});
    std::atomic<size_t> nextChunk{0};
    parallel(threads, [&](uint32_t th) {
        Tiles& tl = part[th];
        tl.score.assign((size_t)gx * gy, 0.0f);
        tl.rock.resize((size_t)gx * gy);
        tl.built.resize((size_t)gx * gy);
        for (size_t j; (j = nextChunk++) < allChunks.size();) {
            const Chunk& ch = allChunks[j];
            bool rock = s.materials[s.submeshes[ch.sm].material].gpu.h0.x == ModelRock;
            triangles(s, *ch.o, ch.sm, ch.first, 4096, [&](vec3 a, vec3 b, vec3 c) {
                vec3 m = (a + b + c) / 3.0f;
                int tx = std::clamp((int)((m.x - all.lo.x) / tile), 0, gx - 1), ty = std::clamp((int)((m.y - all.lo.y) / tile), 0, gy - 1);
                size_t i = (size_t)ty * gx + tx;
                Box& bb = rock ? tl.rock[i] : tl.built[i];
                bb.add(a); bb.add(b); bb.add(c);
                if (m.z - ground(m.x, m.y) > kTallM) tl.score[i] += (rock ? kRockWeight : 1.0f) * 0.5f * glm::length(glm::cross(b - a, c - a));
            });
        }
    });
    std::vector<float>& score = part[0].score;
    std::vector<Box>& tileRock = part[0].rock;
    std::vector<Box>& tileBuilt = part[0].built;
    for (uint32_t th = 1; th < threads; ++th)
        for (size_t i = 0; i < score.size(); ++i) {
            score[i] += part[th].score[i];
            tileRock[i].add(part[th].rock[i]);
            tileBuilt[i].add(part[th].built[i]);
        }
    struct Cand { Box box; float score = 0; };
    std::vector<Cand> cands;
    std::vector<uint8_t> seen((size_t)gx * gy, 0);
    for (int i0 = 0; i0 < gx * gy; ++i0) {
        if (seen[i0] || score[i0] <= 0) continue;
        Cand c;
        Box rocks;   // the box holds the buildings of a cluster; rock alone (an outcrop, a crag) only without them
        std::vector<int> stack{i0};
        seen[i0] = 1;
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            c.box.add(tileBuilt[i]);
            rocks.add(tileRock[i]);
            c.score += score[i];
            int x = i % gx, y = i / gx;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy, j = ny * gx + nx;
                    if (nx < 0 || ny < 0 || nx >= gx || ny >= gy || seen[j] || score[j] <= 0) continue;
                    seen[j] = 1;
                    stack.push_back(j);
                }
        }
        if (c.score < kMinScore) continue;
        if (c.box.lo.x > c.box.hi.x) c.box = rocks;
        c.box.lo -= vec3(kPadM, kPadM, 0);
        c.box.hi += vec3(kPadM, kPadM, 1.5f);
        float tmin = c.box.lo.z;
        if (!t.empty())
            for (float y = c.box.lo.y; y <= c.box.hi.y + t.spacing; y += t.spacing)
                for (float x = c.box.lo.x; x <= c.box.hi.x + t.spacing; x += t.spacing) tmin = std::min(tmin, ground(x, y));
        c.box.lo.z = std::max(tmin, c.box.lo.z - 8.0f) - 1.0f;   // the ground under the structure, not a whole hillside
        cands.push_back(c);
    }
    for (bool merged = true; merged;) {   // overlapping boxes become one region
        merged = false;
        for (size_t i = 0; i < cands.size() && !merged; ++i)
            for (size_t j = i + 1; j < cands.size() && !merged; ++j)
                if (cands[i].box.overlaps(cands[j].box)) {
                    cands[i].box.add(cands[j].box);
                    cands[i].score += cands[j].score;
                    cands.erase(cands.begin() + j);
                    merged = true;
                }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    size_t dropped = cands.size() > kMaxSkyRegions ? cands.size() - kMaxSkyRegions : 0;
    cands.resize(std::min<size_t>(cands.size(), kMaxSkyRegions));
    if (cands.empty()) { last_.reset(); key_ = 0; return nullptr; }
    double regionMs = msSince(t0);

    // 2. cell sizes: the preferred size, grown for the largest regions until the budget fits
    std::vector<float> cell(cands.size(), cellPref);
    auto dimsOf = [&](size_t i) { return uvec3(glm::max(glm::ceil((cands[i].box.hi - cands[i].box.lo) / cell[i]), vec3(2))); };
    auto count = [](uvec3 d) { return (size_t)d.x * d.y * d.z; };
    for (size_t i = 0; i < cands.size(); ++i) cell[i] = std::max(cell[i], glm::compMax(cands[i].box.hi - cands[i].box.lo) / (kMaxDim - 1));
    for (;;) {
        size_t total = 0, big = 0;
        for (size_t i = 0; i < cands.size(); ++i) {
            total += count(dimsOf(i));
            if (count(dimsOf(i)) > count(dimsOf(big))) big = i;
        }
        if (total <= kMaxCells) break;
        cell[big] *= 1.05f;
    }

    // 3. trace the regions that changed
    auto vol = std::make_shared<SkyVolume>();
    vol->rays = K;
    std::vector<vec3> dirs = sphereDirections(K);
    double voxMs = 0, rayMs = 0;
    size_t traced = 0;
    for (size_t i = 0; i < cands.size(); ++i) {
        auto r = std::make_shared<SkyRegion>();
        r->cell = cell[i];
        r->dims = dimsOf(i);
        r->origin = cands[i].box.lo;
        Box box{r->origin, r->origin + vec3(r->dims) * r->cell};
        std::vector<const Occluder*> mine;
        r->key = hf(hv(hmix(K, r->dims.x + ((uint64_t)r->dims.y << 20) + ((uint64_t)r->dims.z << 40)), r->origin), r->cell);
        for (auto& o : occ)
            if (o.box.overlaps(box)) { mine.push_back(&o); r->key = hmix(r->key, o.key); }
        std::vector<float> heights = columnHeights(*r);
        r->terrainKey = heightsKey(heights);
        std::shared_ptr<const SkyRegion> cached;
        if (last_)
            for (auto& c : last_->regions) if (c->key == r->key && c->terrainKey == r->terrainKey) cached = c;
        logInfo("sky occlusion region {}: centre ({:.0f} {:.0f} {:.0f}) size {:.0f} x {:.0f} x {:.0f} m, cell {:.2f}, score {:.0f}, {} occluders{}", i,
                r->origin.x + r->dims.x * r->cell * 0.5f, r->origin.y + r->dims.y * r->cell * 0.5f, r->origin.z + r->dims.z * r->cell * 0.5f,
                r->dims.x * r->cell, r->dims.y * r->cell, r->dims.z * r->cell, r->cell, cands[i].score, mine.size(), cached ? " (cached)" : "");
        if (cached) { vol->regions.push_back(cached); vol->cells += count(cached->dims); continue; }
        auto tv = Clock::now();
        // occupancy: ground under the terrain, then solid cells touched by occluder triangles
        const uvec3 n = r->dims;
        std::vector<uint8_t> grid((size_t)n.x * n.y * n.z, 0);
        if (!t.empty())
            for (uint32_t y = 0; y < n.y; ++y)
                for (uint32_t x = 0; x < n.x; ++x)
                    for (uint32_t z = 0; z < n.z && r->origin.z + (z + 0.5f) * r->cell < heights[(size_t)y * n.x + x]; ++z)
                        grid[x + (size_t)n.x * (y + (size_t)n.y * z)] = 1;
        std::vector<Chunk> chunks;
        for (const Occluder* o : mine)
            for (uint32_t sm : *o->parts)
                for (uint32_t f = 0; f < s.submeshes[sm].indexCount / 3; f += 4096) chunks.push_back({o, sm, f});
        std::atomic<size_t> next{0};
        parallel(threads, [&](uint32_t) {
            const vec3 h(0.5f * 1.04f);   // slightly conservative: thin diagonal walls stay closed
            for (size_t j; (j = next++) < chunks.size();)
                triangles(s, *chunks[j].o, chunks[j].sm, chunks[j].first, 4096, [&](vec3 a, vec3 b, vec3 c) {
                    a = (a - r->origin) / r->cell; b = (b - r->origin) / r->cell; c = (c - r->origin) / r->cell;
                    ivec3 lo = glm::max(ivec3(glm::floor(glm::min(a, glm::min(b, c)) - 0.02f)), ivec3(0));
                    ivec3 hi = glm::min(ivec3(glm::floor(glm::max(a, glm::max(b, c)) + 0.02f)), ivec3(n) - 1);
                    for (int z = lo.z; z <= hi.z; ++z)
                        for (int y = lo.y; y <= hi.y; ++y)
                            for (int x = lo.x; x <= hi.x; ++x) {
                                vec3 m(x + 0.5f, y + 0.5f, z + 0.5f);
                                if (triBox(a - m, b - m, c - m, h))
                                    std::atomic_ref<uint8_t>(grid[x + (size_t)n.x * (y + (size_t)n.y * z)]).store(2, std::memory_order_relaxed);
                            }
                });
        });
        voxMs += msSince(tv);
        auto tr = Clock::now();
        trace(*r, grid, dirs, threads);
        rayMs += msSince(tr);
        vol->cells += count(r->dims);
        ++traced;
        vol->regions.push_back(r);
    }
    vol->ms = traced ? msSince(t0) : 0.0;
    float cmin = 1e9f, cmax = 0;
    for (auto& r : vol->regions) { cmin = std::min(cmin, r->cell); cmax = std::max(cmax, r->cell); }
    logInfo("sky occlusion: {} regions ({} traced, {} small ones skipped), {:.2f} M cells of {:.2f}-{:.2f} m, {} rays, {:.0f} ms "
            "(regions {:.0f} ms, voxels {:.0f} ms, rays {:.0f} ms, {} threads)", vol->regions.size(), traced, dropped, vol->cells / 1e6,
            cmin, cmax, K, vol->ms, regionMs, voxMs, rayMs, threads);
    key_ = key;
    last_ = vol;
    return vol;
}
}  // namespace df
