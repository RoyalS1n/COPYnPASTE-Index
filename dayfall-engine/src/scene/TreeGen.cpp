#include "scene/TreeGen.h"
#include "core/Error.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>

namespace df {
using json = nlohmann::json;
namespace {
constexpr float kPi = 3.14159265358979f;

struct Rand {   // splitmix64: the same tree for the same seed on every machine
    uint64_t s;
    explicit Rand(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float u() { return (float)((next() >> 40) * (1.0 / 16777216.0)); }
    float u(float a, float b) { return a + (b - a) * u(); }
    vec3 ball() {
        for (;;) {
            vec3 p(u(-1, 1), u(-1, 1), u(-1, 1));
            if (glm::dot(p, p) <= 1.0f) return p;
        }
    }
};

struct Species {
    const char* name;
    float height, crown, base, trunk;   // crown radius, crown base and trunk radius as fractions of the height
    float tropism;                      // + reaches up, - droops
    float attractors, leafSize, leavesPerTip, jitter;
    bool conifer, dome, leaves;
    const char* bark;
    const char* leaf;
};
const Species kSpecies[] = {
    {"oak", 10.0f, 0.42f, 0.30f, 0.030f, 0.04f, 1100, 0.6f, 5, 0.25f, false, false, true, "bark", "leaves"},
    {"birch", 12.0f, 0.20f, 0.35f, 0.017f, -0.10f, 800, 0.42f, 5, 0.2f, false, false, true, "bark_birch", "leaves_light"},
    {"pine", 14.0f, 0.24f, 0.18f, 0.024f, 0.0f, 0, 0.0f, 0, 0.0f, true, false, true, "bark", "needles"},
    {"bush", 2.2f, 0.65f, 0.08f, 0.035f, 0.10f, 500, 0.3f, 5, 0.3f, false, true, true, "bark", "leaves"},
    {"dead", 9.0f, 0.38f, 0.30f, 0.032f, 0.02f, 420, 0.0f, 0, 0.45f, false, false, false, "bark", "leaves"},
};

const Species& speciesOf(const json& spec) {
    std::string n = spec.value("species", "oak");
    for (auto& s : kSpecies)
        if (n == s.name) return s;
    throw Error("unknown tree species '" + n + "' (oak, birch, pine, bush, dead)");
}
float num(const json& j, const char* k, float def) {
    if (!j.contains(k)) return def;
    if (!j[k].is_number()) throw Error(std::format("tree {} must be a number", k));
    return j[k].get<float>();
}

struct Node { vec3 p; int parent; float r = 0; int kids = 0; };

// nodes bucketed by cell for the nearest-node searches
struct Grid {
    float cell;
    std::unordered_map<int64_t, std::vector<int>> m;
    static int64_t key(ivec3 c) {
        return ((int64_t)(c.x & 0x1FFFFF) << 42) | ((int64_t)(c.y & 0x1FFFFF) << 21) | (int64_t)(c.z & 0x1FFFFF);
    }
    ivec3 at(vec3 p) const { return ivec3(glm::floor(p / cell)); }
    void add(vec3 p, int i) { m[key(at(p))].push_back(i); }
    template <class F> void near(vec3 p, F f) const {
        ivec3 c = at(p);
        for (int z = -1; z <= 1; ++z)
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                    if (auto it = m.find(key(c + ivec3(x, y, z))); it != m.end())
                        for (int i : it->second) f(i);
    }
};

float dist2(vec3 a, vec3 b) { vec3 d = a - b; return glm::dot(d, d); }

// Space colonisation. The crown is the union of a few ellipsoidal lobes inside the species' envelope, so crowns are
// lumpy and differ per seed.
void colonize(std::vector<Node>& nodes, const Species& sp, const TreeShape& t, Rand& rng, float detail, vec3& crownCentre) {
    float H = t.height, R = t.crownRadius;
    float D = std::max(H / 42.0f, 0.04f) / detail;   // growth step
    float di = 8.5f * D, dk = 1.9f * D;                // influence and kill distances (Runions et al.: a few steps)
    vec3 centre = sp.dome ? vec3(0, 0, t.crownBase) : vec3(0, 0, 0.5f * (t.crownBase + H));
    vec3 radii = sp.dome ? vec3(R, R, H - t.crownBase) : vec3(R, R, 0.5f * (H - t.crownBase));
    radii.x *= rng.u(0.85f, 1.15f);
    radii.y *= rng.u(0.85f, 1.15f);
    centre += vec3(rng.u(-0.08f, 0.08f) * R, rng.u(-0.08f, 0.08f) * R, 0.0f);
    crownCentre = sp.dome ? centre + vec3(0, 0, 0.4f * radii.z) : centre;
    struct Lobe { vec3 c; float f; };
    std::vector<Lobe> lobes;
    int nl = 5 + (int)(rng.u() * 4.0f);
    for (int k = 0; k < nl; ++k) {
        vec3 q = rng.ball() * 0.55f;
        if (sp.dome) q.z = std::abs(q.z);
        lobes.push_back({centre + q * radii, rng.u(0.45f, 0.72f)});
    }
    std::vector<vec3> pts;
    int want = (int)(sp.attractors * detail * detail);
    for (int tries = 0; (int)pts.size() < want && tries < want * 60; ++tries) {
        vec3 q = rng.ball();
        if (sp.dome) q.z = std::abs(q.z);
        vec3 p = centre + q * radii;
        for (auto& l : lobes)
            if (glm::length((p - l.c) / radii) < l.f) { pts.push_back(p); break; }
    }
    // the trunk, up to the crown
    nodes.push_back({vec3(0), -1});
    vec3 lean(rng.u(-0.04f, 0.04f), rng.u(-0.04f, 0.04f), 1.0f);
    float top = sp.dome ? 2.0f * D : t.crownBase;
    auto grow = [&](vec3 dir) {
        int last = (int)nodes.size() - 1;
        nodes.push_back({nodes[last].p + glm::normalize(dir) * D, last});
        nodes[last].kids++;
    };
    while (nodes.back().p.z < top - D) grow(lean + vec3(rng.u(-1, 1), rng.u(-1, 1), 0.0f) * 0.06f);
    // keep growing toward the crown until it is within reach
    for (int k = 0; k < 400 && !pts.empty(); ++k) {
        float best = 1e30f;
        for (auto& p : pts) best = std::min(best, dist2(p, nodes.back().p));
        if (best < di * di) break;
        grow(glm::normalize(crownCentre - nodes.back().p) + lean);
    }
    Grid grid{di, {}};
    for (int i = 0; i < (int)nodes.size(); ++i) grid.add(nodes[i].p, i);
    std::vector<vec3> acc;
    std::vector<int> hits;
    size_t maxNodes = (size_t)(24000 * detail);
    for (int it = 0; it < 800 && !pts.empty() && nodes.size() < maxNodes; ++it) {
        acc.assign(nodes.size(), vec3(0));
        hits.assign(nodes.size(), 0);
        size_t w = 0;
        for (size_t a = 0; a < pts.size(); ++a) {
            int best = -1;
            float bd = di * di;
            grid.near(pts[a], [&](int i) {
                float d = dist2(nodes[i].p, pts[a]);
                if (d < bd) { bd = d; best = i; }
            });
            if (best >= 0 && bd < dk * dk) continue;   // reached: the point dies
            pts[w++] = pts[a];
            if (best >= 0) {
                acc[best] += (pts[a] - nodes[best].p) / std::sqrt(bd);
                hits[best]++;
            }
        }
        pts.resize(w);
        size_t count = nodes.size();
        int grown = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!hits[i]) continue;
            vec3 a = acc[i];
            if (glm::dot(a, a) < 1e-6f) a = rng.ball();   // pulls that cancel out
            vec3 dir = glm::normalize(glm::normalize(a) + vec3(0, 0, sp.tropism) + rng.ball() * (sp.jitter * 0.3f));
            vec3 np = nodes[i].p + dir * D;
            bool dup = false;
            grid.near(np, [&](int j) { dup = dup || dist2(nodes[j].p, np) < 0.09f * D * D; });
            if (dup) continue;
            nodes.push_back({np, (int)i});
            nodes[i].kids++;
            grid.add(np, (int)nodes.size() - 1);
            ++grown;
        }
        if (!grown) break;
    }
}

// Pipe model: a parent's cross-section carries its children's, r^n = sum r_child^n, scaled so the base has the
// trunk radius; the base flares a little.
void pipeRadii(std::vector<Node>& nodes, float trunkR, float n, float H) {
    std::vector<double> s(nodes.size(), 0.0);
    const double tip = std::pow(1.0, n);
    for (int i = (int)nodes.size() - 1; i >= 0; --i) {
        if (nodes[i].kids == 0) s[i] = tip;
        if (nodes[i].parent >= 0) s[nodes[i].parent] += s[i];
    }
    float k = trunkR / (float)std::pow(s[0], 1.0 / n);
    float minR = std::max(0.004f, trunkR * 0.02f);
    for (size_t i = 0; i < nodes.size(); ++i) {
        float r = std::max((float)std::pow(s[i], 1.0 / n) * k, minR);
        r *= 1.0f + 0.45f * std::exp(-nodes[i].p.z / std::max(0.05f * H, 0.05f));   // root flare
        nodes[i].r = r;
    }
}

struct Card { vec3 c, u, v, n; float length, width; bool needles; };

// a pine: a leader with whorls of branches, longest at the bottom, drooping toward their tips; needle sprays along them
void conifer(std::vector<Node>& nodes, std::vector<Card>& cards, const TreeShape& t, Rand& rng, float detail, vec3& crownCentre) {
    float H = t.height, R = t.crownRadius;
    int segs = std::max(8, (int)(H / 0.6f));
    nodes.push_back({vec3(0), -1, t.trunkRadius * 1.35f});
    vec2 sway(rng.u(-1, 1), rng.u(-1, 1));
    for (int k = 1; k <= segs; ++k) {
        float f = (float)k / segs;
        vec3 p(sway * (0.012f * H * f * f), H * f);
        nodes.push_back({p, k - 1, t.trunkRadius * std::pow(1.0f - f, 0.85f) + 0.01f});
        nodes[k - 1].kids++;
    }
    crownCentre = vec3(0, 0, 0.5f * (t.crownBase + H));
    float step = std::max(0.25f, H / rng.u(19.0f, 24.0f)) / std::sqrt(detail);
    for (float z = t.crownBase; z < H * 0.95f; z += step * rng.u(0.8f, 1.2f)) {
        float tt = (z - t.crownBase) / std::max(H - t.crownBase, 0.1f);
        int nb = 5 + (int)(rng.u() * 3.0f);
        float phase = rng.u(0.0f, 2.0f * kPi);
        int attach = std::clamp((int)std::round(z / H * segs), 0, segs);
        float L0 = R * std::pow(std::max(1.02f - tt, 0.02f), 0.95f) + 0.15f;
        float pitch0 = glm::mix(-0.32f, 0.5f, tt);
        for (int b = 0; b < nb; ++b) {
            float az = phase + 2.0f * kPi * b / nb + rng.u(-0.3f, 0.3f);
            vec3 h(std::cos(az), std::sin(az), 0.0f);
            float L = L0 * rng.u(0.85f, 1.1f);
            int ns = L > 1.5f ? 4 : 3;
            float br = std::max(t.trunkRadius * 0.28f * (1.0f - tt), 0.012f);
            int prev = attach;
            vec3 p = vec3(vec2(nodes[attach].p), z);
            for (int s = 1; s <= ns; ++s) {
                float f = (float)s / ns;
                float pitch = pitch0 - 0.4f * f * (1.0f - tt);
                vec3 d = h * std::cos(pitch) + vec3(0, 0, std::sin(pitch));
                vec3 q = p + d * (L / ns);
                nodes.push_back({q, prev, br * (1.0f - 0.75f * f)});
                nodes[prev].kids++;
                prev = (int)nodes.size() - 1;
                // sprays of needles over this stretch: one along the branch and two fanning out to the sides, flat and a
                // little tilted, narrowing toward the tip
                vec3 side = glm::normalize(glm::cross(vec3(0, 0, 1), d));
                float w = (0.42f * L * (1.0f - 0.5f * f) + 0.22f) * rng.u(0.85f, 1.15f);
                for (int k = -1; k <= 1; ++k) {
                    vec3 a = glm::normalize(d + side * (0.75f * k) - vec3(0, 0, 0.12f * std::abs(k)));
                    vec3 sd = glm::normalize(glm::cross(vec3(0, 0, 1), a));
                    vec3 nrm = glm::normalize(glm::cross(a, sd) + rng.ball() * 0.2f);
                    float len = (k == 0 ? 1.7f : 1.15f) * L / ns;
                    cards.push_back({(p + q) * 0.5f + a * (0.3f * len * std::abs(k)), a, sd, nrm, len, k == 0 ? w : 0.75f * w, true});
                }
                p = q;
            }
        }
    }
    // the leader's tuft
    for (int k = 0; k < 3; ++k) {
        float az = rng.u(0.0f, 2.0f * kPi);
        vec3 side(std::cos(az), std::sin(az), 0.0f);
        cards.push_back({vec3(vec2(nodes[segs].p), H * 0.96f), vec3(0, 0, 1), side, glm::normalize(glm::cross(vec3(0, 0, 1), side)),
                         0.12f * H, 0.25f + 0.04f * H, true});
    }
}

// tubes along branch chains: each chain follows the thickest child; the other children start chains of their own
// from inside their parent. Rings use parallel-transported frames so the tubes do not twist.
void tubes(MeshBuilder& mb, const std::vector<Node>& nodes, const std::string& mat, float detail, float minR) {
    size_t N = nodes.size();
    std::vector<std::vector<int>> kids(N);
    for (size_t i = 1; i < N; ++i) kids[nodes[i].parent].push_back((int)i);
    std::vector<int> cont(N, -1);
    for (size_t i = 0; i < N; ++i) {
        float best = -1;
        for (int c : kids[i])
            if (nodes[c].r > best) { best = nodes[c].r; cont[i] = c; }
    }
    std::vector<std::pair<int, int>> todo{{-1, 0}};   // (start node or -1 for the root, first node)
    while (!todo.empty()) {
        auto [from, first] = todo.back();
        todo.pop_back();
        std::vector<vec3> P;
        std::vector<float> Rr;
        if (from >= 0) { P.push_back(nodes[from].p); Rr.push_back(nodes[first].r); }
        for (int k = first; k >= 0; k = cont[k]) {
            P.push_back(nodes[k].p);
            Rr.push_back(nodes[k].r);
            for (int c : kids[k])
                if (c != cont[k]) todo.push_back({k, c});
        }
        if (P.size() < 2 || Rr[0] < minR) continue;
        float r0 = Rr[0];
        int sides = r0 > 0.12f ? 10 : r0 > 0.05f ? 8 : r0 > 0.02f ? 6 : 4;
        sides = std::max(3, (int)std::round(sides * std::min(detail, 1.5f)));
        vec3 T0 = glm::normalize(P[1] - P[0]);
        vec3 Nn = std::abs(T0.z) < 0.9f ? glm::normalize(glm::cross(T0, vec3(0, 0, 1))) : glm::normalize(glm::cross(T0, vec3(1, 0, 0)));
        float along = 0;
        uint32_t prevRing = 0;
        for (size_t k = 0; k < P.size(); ++k) {
            vec3 T = k == 0 ? T0 : k + 1 < P.size() ? glm::normalize(P[k + 1] - P[k - 1]) : glm::normalize(P[k] - P[k - 1]);
            Nn = glm::normalize(Nn - glm::dot(Nn, T) * T);
            vec3 B = glm::cross(T, Nn);
            if (k > 0) along += glm::length(P[k] - P[k - 1]);
            uint32_t ring = (uint32_t)mb.vertexCount();
            for (int s = 0; s <= sides; ++s) {
                float a = 2.0f * kPi * s / sides;
                vec3 dir = Nn * std::cos(a) + B * std::sin(a);
                mb.vertex(makeVertex(P[k] + dir * Rr[k], dir, vec2((float)s / sides * 2.0f * kPi * r0, along), vec4(T, 1.0f)));
            }
            if (k > 0)
                for (int s = 0; s < sides; ++s)
                    mb.quad(mat, prevRing + s, prevRing + s + 1, ring + s + 1, ring + s);
            prevRing = ring;
        }
        // close the tip
        vec3 T = glm::normalize(P.back() - P[P.size() - 2]);
        uint32_t tip = mb.vertex(makeVertex(P.back() + T * Rr.back(), T, vec2(0, along + Rr.back()), vec4(Nn, 1.0f)));
        for (int s = 0; s < sides; ++s) mb.tri(mat, prevRing + s, prevRing + s + 1, tip);
    }
}

// a leaf: a kite, its front facing outward; normals bent toward the crown's outside so the canopy shades as a volume
void leafCard(MeshBuilder& mb, const std::string& mat, const Card& c, vec3 crownCentre) {
    vec3 out = glm::normalize(c.c - crownCentre + vec3(0, 0, 0.3f));
    vec3 n = glm::normalize(glm::mix(c.n, out, 0.65f));
    if (!c.needles) {
        vec3 a = c.c - c.u * (0.5f * c.length), b = c.c + c.u * (0.5f * c.length), m = c.c - c.u * (0.1f * c.length);
        uint32_t i0 = mb.vertex(makeVertex(a, n, vec2(0.5f, 0.0f), vec4(c.u, 1.0f)));
        uint32_t i1 = mb.vertex(makeVertex(m + c.v * (0.5f * c.width), n, vec2(1.0f, 0.4f), vec4(c.u, 1.0f)));
        uint32_t i2 = mb.vertex(makeVertex(b, n, vec2(0.5f, 1.0f), vec4(c.u, 1.0f)));
        uint32_t i3 = mb.vertex(makeVertex(m - c.v * (0.5f * c.width), n, vec2(0.0f, 0.4f), vec4(c.u, 1.0f)));
        if (glm::dot(glm::cross(c.v, c.u), c.n) < 0) mb.quad(mat, i0, i3, i2, i1);
        else mb.quad(mat, i0, i1, i2, i3);
        return;
    }
    // needles: a spindle with a serrated edge, so the spray reads as needles, not a plate
    const int steps = 6;
    vec3 start = c.c - c.u * (0.5f * c.length);
    uint32_t prevS = 0, prevL = 0, prevR = 0;
    for (int i = 0; i <= steps; ++i) {
        float f = (float)i / steps;
        float w = 0.5f * c.width * std::pow(std::sin(kPi * std::clamp(f * 0.92f + 0.04f, 0.0f, 1.0f)), 0.6f) * (i % 2 ? 0.6f : 1.0f);
        vec3 s = start + c.u * (f * c.length);
        uint32_t is = mb.vertex(makeVertex(s, n, vec2(0.5f, f), vec4(c.u, 1.0f)));
        uint32_t il = mb.vertex(makeVertex(s + c.v * w, n, vec2(0.0f, f), vec4(c.u, 1.0f)));
        uint32_t ir = mb.vertex(makeVertex(s - c.v * w, n, vec2(1.0f, f), vec4(c.u, 1.0f)));
        if (i > 0) {
            mb.quad(mat, prevS, is, il, prevL);
            mb.quad(mat, prevR, ir, is, prevS);
        }
        prevS = is; prevL = il; prevR = ir;
    }
}
}  // namespace

TreeShape treeShape(const json& spec) {
    const Species& sp = speciesOf(spec);
    TreeShape t;
    t.height = num(spec, "height", sp.height);
    if (!(t.height >= 0.5f && t.height <= 80.0f)) throw Error("tree height must be between 0.5 and 80 m");
    t.crownRadius = num(spec, "crown_radius", sp.crown * t.height);
    if (!(t.crownRadius >= 0.1f && t.crownRadius <= 2.0f * t.height)) throw Error("tree crown_radius must be between 0.1 m and twice the height");
    t.trunkRadius = num(spec, "trunk_radius", sp.trunk * t.height);
    if (!(t.trunkRadius >= 0.005f && t.trunkRadius <= 0.2f * t.height)) throw Error("tree trunk_radius must be between 0.005 m and a fifth of the height");
    t.crownBase = sp.base * t.height;
    return t;
}

MeshAsset makeTree(const json& spec) {
    const Species& sp = speciesOf(spec);
    TreeShape t = treeShape(spec);
    float detail = std::clamp(num(spec, "detail", 1.0f), 0.3f, 2.0f);
    uint32_t seed = (uint32_t)num(spec, "seed", 1.0f);
    bool leaves = sp.leaves && (!spec.contains("leaves") || spec["leaves"].get<bool>());
    std::string bark = spec.value("bark_material", std::string(sp.bark)), leaf = spec.value("leaf_material", std::string(sp.leaf));
    Rand rng(seed * 2654435761u + (uint32_t)(sp.name[0]) * 977u);
    std::vector<Node> nodes;
    std::vector<Card> cards;
    vec3 crownCentre;
    if (sp.conifer) {
        conifer(nodes, cards, t, rng, detail, crownCentre);
    } else {
        colonize(nodes, sp, t, rng, detail, crownCentre);
        pipeRadii(nodes, t.trunkRadius, 2.4f, t.height);
        if (leaves) {
            // clusters at the twig ends and along the finest twigs
            float fine = 2.5f * nodes.back().r;
            float maxLeaves = 7000.0f * detail;
            for (size_t i = 1; i < nodes.size() && cards.size() < maxLeaves; ++i) {
                const Node& nd = nodes[i];
                bool tip = nd.kids == 0;
                if (!tip && (nd.r > fine || rng.u() > 0.35f)) continue;
                int count = tip ? (int)sp.leavesPerTip : 2;
                vec3 dir = glm::normalize(nd.p - nodes[nd.parent].p);
                for (int k = 0; k < count; ++k) {
                    float size = sp.leafSize * rng.u(0.75f, 1.25f) * std::sqrt(t.height / sp.height);
                    vec3 n = glm::normalize(rng.ball() + glm::normalize(nd.p - crownCentre) * 0.8f + vec3(0, 0, 0.4f));
                    vec3 u = glm::normalize(dir + rng.ball() * 0.9f - vec3(0, 0, 0.3f));
                    u = glm::normalize(u - glm::dot(u, n) * n);
                    vec3 v = glm::cross(n, u);
                    cards.push_back({nd.p + rng.ball() * (0.45f * size), u, v, n, size, size * 0.55f, false});
                }
            }
        }
    }
    MeshBuilder mb;
    // the finest twigs vanish under the leaves: leafy trees skip them, bare ones keep them
    float minR = sp.conifer ? 0.0f : (leaves ? 1.2f : 0.5f) * std::max(0.004f, t.trunkRadius * 0.02f) / detail;
    tubes(mb, nodes, bark, detail, minR);
    if (leaves)
        for (auto& c : cards) leafCard(mb, leaf, c, crownCentre);
    MeshAsset m = mb.build("tree");
    // level of detail like the library trees, scaled to the tree's size
    float s = std::clamp(t.height / 10.0f, 0.35f, 3.0f);
    generateLods(m, 45.0f * s, {{0.45f, 110.0f * s, false, true}, {0.18f, 320.0f * s, true, true}, {0.08f, 900.0f * s, true, true}},
                 {leaf});
    m.computeBounds();
    return m;
}

json treeCatalog() {
    json a = json::array();
    for (auto& s : kSpecies)
        a.push_back({{"species", s.name}, {"height_m", s.height}, {"crown_radius_m", s.crown * s.height}, {"leaves", s.leaves},
                     {"bark_material", s.bark}, {"leaf_material", s.leaf}});
    return a;
}
}  // namespace df
