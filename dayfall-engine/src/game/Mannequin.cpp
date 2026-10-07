#include "game/Mannequin.h"
#include "scene/Primitives.h"
#include <cmath>

namespace df {
namespace {
// a capsule hanging down from its pivot by `length`
MeshAsset limb(float r, float length, const std::string& mat) {
    MeshAsset m = makeCapsule(r, length + 2 * r, 16, mat);
    for (auto& v : m.vertices) {
        v.p0.z = -(v.p0.z - r);          // pivot at the top sphere centre, hanging down
        v.p1.z = -v.p1.z;
        v.p0.y = -v.p0.y;                // mirror y as well to keep the winding
        v.p1.y = -v.p1.y;
    }
    m.computeBounds();
    return m;
}
MeshAsset shifted(MeshAsset m, vec3 d) {
    for (auto& v : m.vertices) { v.p0.x += d.x; v.p0.y += d.y; v.p0.z += d.z; }
    m.computeBounds();
    return m;
}
MeshAsset merged(std::vector<MeshAsset> parts) {
    MeshBuilder b;
    for (auto& p : parts) {
        uint32_t base = (uint32_t)b.vertexCount();
        for (auto& v : p.vertices) b.vertex(v);
        for (auto& part : p.lods[0])
            for (uint32_t i = part.firstIndex; i + 2 < part.firstIndex + part.indexCount; i += 3)
                b.tri(part.material, base + p.indices[i], base + p.indices[i + 1], base + p.indices[i + 2]);
    }
    return b.build("part");
}
}  // namespace

const std::vector<MannequinPart>& mannequinParts() {
    static std::vector<MannequinPart> parts = [] {
        const std::string M = "mannequin", J = "mannequin_joint";
        std::vector<MannequinPart> p;
        // 0 pelvis (root): pivot at hip height
        p.push_back({"pelvis", -1, {0, 0, 0.95f}, shifted(makeBox({0.3f, 0.17f, 0.16f}, M), {0, 0, -0.08f})});
        // 1 spine: pivot at the waist
        p.push_back({"spine", 0, {0, 0, 0.06f}, makeCapsule(0.15f, 0.42f, 16, M)});
        // 2 chest / shoulders
        p.push_back({"chest", 1, {0, 0, 0.36f}, shifted(makeBox({0.42f, 0.2f, 0.16f}, M), {0, 0, -0.04f})});
        // 3 head (with a dark visor showing the facing direction)
        p.push_back({"head", 2, {0, 0, 0.14f},
                     merged({makeCylinder(0.045f, 0.08f, 12, J), shifted(makeSphere(0.11f, 20, M), {0, 0, 0.05f}),
                             shifted(makeBox({0.13f, 0.04f, 0.045f}, J), {0, 0.095f, 0.15f})})});
        // 4/5 upper arms, 6/7 forearms, 8/9 hands
        for (int s = -1; s <= 1; s += 2) p.push_back({s < 0 ? "upper_arm_l" : "upper_arm_r", 2, {0.25f * s, 0, 0.06f}, limb(0.055f, 0.27f, M)});
        for (int s = -1; s <= 1; s += 2) p.push_back({s < 0 ? "forearm_l" : "forearm_r", s < 0 ? 4 : 5, {0, 0, -0.29f}, limb(0.045f, 0.25f, M)});
        for (int s = -1; s <= 1; s += 2) p.push_back({s < 0 ? "hand_l" : "hand_r", s < 0 ? 6 : 7, {0, 0, -0.27f}, shifted(makeSphere(0.05f, 12, J), {0, 0, -0.08f})});
        // 10/11 thighs, 12/13 shins, 14/15 feet
        for (int s = -1; s <= 1; s += 2) p.push_back({s < 0 ? "thigh_l" : "thigh_r", 0, {0.1f * s, 0, -0.05f}, limb(0.075f, 0.4f, M)});
        for (int s = -1; s <= 1; s += 2) p.push_back({s < 0 ? "shin_l" : "shin_r", s < 0 ? 10 : 11, {0, 0, -0.42f}, limb(0.06f, 0.38f, M)});
        for (int s = -1; s <= 1; s += 2) p.push_back({s < 0 ? "foot_l" : "foot_r", s < 0 ? 12 : 13, {0, 0, -0.42f}, shifted(makeBox({0.1f, 0.24f, 0.07f}, J), {0, 0.05f, -0.05f})});
        for (auto& part : p) part.mesh.name = "mannequin_" + part.name;
        return p;
    }();
    return parts;
}

void poseMannequin(const MannequinPose& ps, vec3 root, float yaw, float scale, std::vector<vec3>& pos, std::vector<quat>& rot) {
    const auto& parts = mannequinParts();
    size_t n = parts.size();
    pos.resize(n);
    rot.resize(n);
    std::vector<quat> local(n, quat(1, 0, 0, 0));
    auto X = [](float a) { return glm::angleAxis(a, vec3(1, 0, 0)); };
    auto Y = [](float a) { return glm::angleAxis(a, vec3(0, 1, 0)); };
    auto Z = [](float a) { return glm::angleAxis(a, vec3(0, 0, 1)); };
    float s = std::sin(ps.phase), c = std::cos(ps.phase);
    float st = ps.stride, air = ps.airborne;
    float swing = 0.55f * st + 0.08f;                                  // leg swing amplitude
    float breathe = 0.02f * std::sin(ps.time * 1.7f) * (1 - st);
    // legs: thigh swing, knee bend on the back swing; in the air: tucked
    float thighL = -swing * s * (1 - air) - 0.5f * air, thighR = swing * s * (1 - air) - 0.25f * air;
    float kneeL = (0.15f + 0.9f * st * std::max(0.0f, -c)) * (1 - air) + 0.9f * air;
    float kneeR = (0.15f + 0.9f * st * std::max(0.0f, c)) * (1 - air) + 0.5f * air;
    if (st < 0.05f) { kneeL = kneeR = 0.05f + 0.6f * air; }
    local[10] = X(thighL);
    local[11] = X(thighR);
    local[12] = X(-kneeL);
    local[13] = X(-kneeR);
    local[14] = X(0.3f * kneeL - thighL * 0.5f);
    local[15] = X(0.3f * kneeR - thighR * 0.5f);
    // arms swing opposite to the legs
    float armSwing = 0.5f * st + 0.05f;
    local[4] = X(armSwing * s * (1 - air) + 0.9f * air) * Y(-0.12f - 0.25f * air);
    local[5] = X(-armSwing * s * (1 - air) + 0.9f * air) * Y(0.12f + 0.25f * air);
    local[6] = X(0.3f + 0.7f * st);
    local[7] = X(0.3f + 0.7f * st);
    // torso: lean, counter-rotation, bob
    local[1] = X(ps.lean + 0.05f * st) * Z(0.12f * st * s) * X(breathe);
    local[2] = Z(-0.08f * st * s);
    local[3] = X(-ps.lean * 0.5f);
    float bob = (0.045f * st * std::abs(c) - 0.03f * st) * (1 - air);
    local[0] = Z(-0.05f * st * s);
    quat world = glm::angleAxis(yaw, vec3(0, 0, 1));
    for (size_t i = 0; i < n; ++i) {
        const MannequinPart& p = parts[i];
        if (p.parent < 0) {
            rot[i] = world * local[i];
            pos[i] = root + world * (p.offset + vec3(0, 0, bob)) * scale;
        } else {
            rot[i] = rot[p.parent] * local[i];
            pos[i] = pos[p.parent] + rot[p.parent] * p.offset * scale;
        }
    }
}
}  // namespace df
