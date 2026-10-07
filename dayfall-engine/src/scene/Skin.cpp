#include "scene/Skin.h"
#include <algorithm>
#include <cctype>

namespace df {
namespace {
// "mixamorig:LeftArm" -> "leftarm": joint names match across files without prefixes or case
std::string plainName(const std::string& n) {
    size_t p = n.find_last_of(":|");
    std::string out;
    for (char ch : p == std::string::npos ? n : n.substr(p + 1))
        if (std::isalnum((unsigned char)ch)) out += (char)std::tolower((unsigned char)ch);
    return out;
}
mat4 localMatrix(vec3 t, quat r, vec3 s) { return glm::translate(mat4(1), t) * glm::mat4_cast(r) * glm::scale(mat4(1), s); }

vec4 sampleTrack(const AnimTrack& tr, float t) {
    const std::vector<float>& T = tr.times;
    size_t n = T.size();
    if (n == 1 || t <= T[0]) return tr.values[0];
    if (t >= T[n - 1]) return tr.values[n - 1];
    size_t i = (size_t)(std::upper_bound(T.begin(), T.end(), t) - T.begin()) - 1;
    if (tr.interp == 1) return tr.values[i];
    float u = (t - T[i]) / std::max(T[i + 1] - T[i], 1e-6f);
    vec4 a = tr.values[i], b = tr.values[i + 1];
    if (tr.path != 1) return glm::mix(a, b, u);
    if (glm::dot(a, b) < 0) b = -b;   // shortest arc
    vec4 q = glm::mix(a, b, u);
    return q / std::max(glm::length(q), 1e-8f);
}

int depth(const Skeleton& sk, int node) {
    int d = 0;
    for (int p = sk.parent[node]; p >= 0; p = sk.parent[p]) ++d;
    return d;
}
mat4 parentModel(const Skeleton& sk, int node) {
    int p = sk.parent[node];
    return sk.base * (p >= 0 ? sk.restGlobal(p) : mat4(1));
}
}  // namespace

int Skeleton::find(const std::string& name) const {
    for (size_t i = 0; i < names.size(); ++i) if (names[i] == name) return (int)i;
    std::string p = plainName(name);
    if (!p.empty())
        for (size_t i = 0; i < names.size(); ++i) if (plainName(names[i]) == p) return (int)i;
    return -1;
}

mat4 Skeleton::restGlobal(int node) const {
    mat4 m(1);
    for (int i = node; i >= 0; i = parent[i]) m = localMatrix(restT[i], restR[i], restS[i]) * m;
    return m;
}

int CharacterAsset::clip(const std::string& name) const {
    for (size_t i = 0; i < clips.size(); ++i) if (clips[i].name == name) return (int)i;
    auto lower = [](std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
    for (size_t i = 0; i < clips.size(); ++i) if (lower(clips[i].name) == lower(name)) return (int)i;
    return -1;
}

void restPose(const Skeleton& sk, Pose& out) {
    out.t = sk.restT;
    out.r = sk.restR;
    out.s = sk.restS;
}

void measureTravel(const Skeleton& sk, AnimClip& clip) {
    // the topmost translated node that travels (hips in most rigs); else the topmost translated node
    std::vector<const AnimTrack*> moved;
    for (const AnimTrack& tr : clip.tracks) if (tr.path == 0) moved.push_back(&tr);
    std::stable_sort(moved.begin(), moved.end(), [&](auto* a, auto* b) { return depth(sk, a->node) < depth(sk, b->node); });
    clip.rootNode = moved.empty() ? -1 : (int)moved[0]->node;
    clip.travel = vec2(0);
    for (const AnimTrack* tr : moved) {
        mat4 pm = parentModel(sk, tr->node);
        vec2 d = vec2(pm * vec4(vec3(sampleTrack(*tr, clip.duration)), 1.0f)) - vec2(pm * vec4(vec3(sampleTrack(*tr, 0.0f)), 1.0f));
        if (glm::length(d) > 0.05f) {
            clip.rootNode = (int)tr->node;
            clip.travel = d;
            break;
        }
    }
}

void sampleClip(const CharacterAsset& c, const AnimClip& clip, float time, bool loop, bool stripTravel, Pose& out) {
    float d = clip.duration;
    float t = d <= 0 ? 0.0f : loop ? std::fmod(time, d) : std::clamp(time, 0.0f, d);
    if (t < 0) t += d;
    for (const AnimTrack& tr : clip.tracks) {
        vec4 v = sampleTrack(tr, t);
        if (tr.path == 0) out.t[tr.node] = vec3(v);
        else if (tr.path == 1) out.r[tr.node] = quat(v.w, v.x, v.y, v.z);
        else out.s[tr.node] = vec3(v);
    }
    if (stripTravel && clip.rootNode >= 0 && d > 0 && glm::length(clip.travel) > 0) {
        // remove the travel in model space (horizontal = model X / Y), keep sway and bob
        mat4 pm = parentModel(c.skeleton, clip.rootNode);
        vec4 m = pm * vec4(out.t[clip.rootNode], 1.0f);
        m.x -= clip.travel.x * t / d;
        m.y -= clip.travel.y * t / d;
        out.t[clip.rootNode] = vec3(glm::inverse(pm) * m);
    }
}

void blendPose(Pose& a, const Pose& b, float w) {
    for (size_t i = 0; i < a.t.size(); ++i) {
        a.t[i] = glm::mix(a.t[i], b.t[i], w);
        a.s[i] = glm::mix(a.s[i], b.s[i], w);
        quat q = glm::dot(a.r[i], b.r[i]) < 0 ? -b.r[i] : b.r[i];
        a.r[i] = glm::normalize(a.r[i] * (1.0f - w) + q * w);
    }
}

void skinVertices(const CharacterAsset& c, const Pose& p, GpuVertex* out) {
    const Skeleton& sk = c.skeleton;
    thread_local std::vector<mat4> g;
    struct Rows { vec4 x, y, z; };   // the top three rows of an affine matrix
    thread_local std::vector<Rows> m;
    g.resize(sk.parent.size());
    for (size_t i = 0; i < g.size(); ++i) {
        mat4 l = localMatrix(p.t[i], p.r[i], p.s[i]);
        g[i] = sk.parent[i] < 0 ? sk.base * l : g[sk.parent[i]] * l;
    }
    m.resize(c.bones.size());
    for (size_t b = 0; b < c.bones.size(); ++b) {
        mat4 s = c.bones[b].node < 0 ? mat4(1) : g[c.bones[b].node] * c.bones[b].offset;
        m[b] = {vec4(s[0][0], s[1][0], s[2][0], s[3][0]), vec4(s[0][1], s[1][1], s[2][1], s[3][1]),
                vec4(s[0][2], s[1][2], s[2][2], s[3][2])};
    }
    auto rot = [](const Rows& r, vec3 v) { return vec3(glm::dot(vec3(r.x), v), glm::dot(vec3(r.y), v), glm::dot(vec3(r.z), v)); };
    for (size_t i = 0; i < c.mesh.vertices.size(); ++i) {
        const uvec4 j = c.joints[i];
        const vec4 w = c.weights[i];
        Rows r = m[j.x];
        r.x *= w.x; r.y *= w.x; r.z *= w.x;
        for (int k = 1; k < 4; ++k)
            if (w[k] > 0) {
                const Rows& o = m[j[k]];
                r.x += o.x * w[k]; r.y += o.y * w[k]; r.z += o.z * w[k];
            }
        const GpuVertex& s = c.mesh.vertices[i];
        GpuVertex d = s;
        vec4 P(s.p0.x, s.p0.y, s.p0.z, 1.0f);
        d.p0 = vec4(glm::dot(r.x, P), glm::dot(r.y, P), glm::dot(r.z, P), s.p0.w);
        vec3 n = rot(r, vec3(s.p1));
        d.p1 = vec4(n * glm::inversesqrt(std::max(glm::dot(n, n), 1e-20f)), s.p1.w);
        if (s.tangent.w != 0) {
            vec3 t = rot(r, vec3(s.tangent));
            d.tangent = vec4(t * glm::inversesqrt(std::max(glm::dot(t, t), 1e-20f)), s.tangent.w);
        }
        out[i] = d;
    }
}
}  // namespace df
