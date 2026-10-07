#include "physics/Physics.h"
#include "core/Log.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <thread>
#include <unordered_map>

namespace df {
namespace {
namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0, kMoving = 1, kCount = 2;
}
namespace BPLayers {
constexpr JPH::BroadPhaseLayer kStatic(0), kMoving(1);
constexpr uint32_t kCount = 2;
}
class BPInterface final : public JPH::BroadPhaseLayerInterface {
public:
    uint32_t GetNumBroadPhaseLayers() const override { return BPLayers::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return l == Layers::kStatic ? BPLayers::kStatic : BPLayers::kMoving; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override { return l == BPLayers::kStatic ? "static" : "moving"; }
#endif
};
class ObjVsBP final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer o, JPH::BroadPhaseLayer b) const override { return o == Layers::kMoving || b == BPLayers::kMoving; }
};
class ObjPair final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == Layers::kMoving || b == Layers::kMoving; }
};

JPH::Vec3 J(vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat J(quat q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
vec3 G(JPH::Vec3 v) { return vec3(v.GetX(), v.GetY(), v.GetZ()); }
// Jolt's capsules, cylinders and heightfields are Y-up; this rotates Y onto Z
const JPH::Quat kYtoZ = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::JPH_PI * 0.5f);

std::once_flag g_joltInit;
}  // namespace

struct Physics::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    BPInterface bp;
    ObjVsBP objVsBp;
    ObjPair objPair;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::vector<JPH::BodyID> bodies;
    std::unordered_map<std::string, JPH::ShapeRefC> meshShapes;
    JPH::Ref<JPH::CharacterVirtual> character;
    CharacterSettings charSettings;
    float verticalVelocity = 0;
};

Physics::Physics() : p_(std::make_unique<Impl>()) {}
Physics::~Physics() { shutdown(); }

void Physics::init() {
    std::call_once(g_joltInit, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
    p_->temp = std::make_unique<JPH::TempAllocatorImpl>(32 * 1024 * 1024);
    p_->jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                          (int)std::max(1u, std::thread::hardware_concurrency() / 2));
    p_->system = std::make_unique<JPH::PhysicsSystem>();
    p_->system->Init(262144, 0, 65536, 65536, p_->bp, p_->objVsBp, p_->objPair);
    p_->system->SetGravity(JPH::Vec3(0, 0, -9.81f));
}

void Physics::shutdown() {
    if (!p_ || !p_->system) return;
    destroyCharacter();
    JPH::BodyInterface& bi = p_->system->GetBodyInterface();
    if (!p_->bodies.empty()) {
        bi.RemoveBodies(p_->bodies.data(), (int)p_->bodies.size());
        bi.DestroyBodies(p_->bodies.data(), (int)p_->bodies.size());
        p_->bodies.clear();
    }
    p_->meshShapes.clear();
    p_->system.reset();
    p_->jobs.reset();
    p_->temp.reset();
}

size_t Physics::staticBodies() const { return p_->bodies.size(); }

void Physics::buildStatic(const Scene& scene, const Terrain& terrain) {
    JPH::BodyInterface& bi = p_->system->GetBodyInterface();
    if (!p_->bodies.empty()) {
        bi.RemoveBodies(p_->bodies.data(), (int)p_->bodies.size());
        bi.DestroyBodies(p_->bodies.data(), (int)p_->bodies.size());
        p_->bodies.clear();
    }
    std::vector<JPH::BodyID> created;
    auto addBody = [&](JPH::ShapeRefC shape, vec3 pos, quat rot, uint64_t user) {
        JPH::BodyCreationSettings bcs(shape, JPH::RVec3(pos.x, pos.y, pos.z), J(rot), JPH::EMotionType::Static, Layers::kStatic);
        bcs.mUserData = user;
        JPH::Body* b = bi.CreateBody(bcs);
        if (!b) { logWarn("physics: out of bodies"); return; }
        created.push_back(b->GetID());
    };

    // terrain: Jolt heightfields lie in local XZ with Y up; rotate onto world XY / Z up.
    if (!terrain.empty()) {
        uint32_t n = terrain.n, block = 4;
        uint32_t padded = (n + block - 1) / block * block;
        std::vector<float> samples((size_t)padded * padded);
        for (uint32_t z = 0; z < padded; ++z)
            for (uint32_t x = 0; x < padded; ++x) {
                uint32_t j = std::min(z, n - 1), i = std::min(x, n - 1);
                samples[(size_t)z * padded + x] = terrain.height[(size_t)(n - 1 - j) * n + i];   // local z runs towards -Y
            }
        // local (x, h, z) -> world (x, -z, h) after rotating +90 degrees about X
        float s = terrain.spacing;
        JPH::HeightFieldShapeSettings hs(samples.data(), JPH::Vec3(terrain.origin.x, 0.0f, -(terrain.origin.y + (n - 1) * s)),
                                         JPH::Vec3(s, 1.0f, s), padded);
        hs.mBlockSize = block;
        hs.mBitsPerSample = 8;
        auto res = hs.Create();
        if (res.HasError()) logWarn("physics: terrain heightfield: {}", res.GetError().c_str());
        else addBody(res.Get(), vec3(0), quat(kYtoZ.GetW(), kYtoZ.GetX(), kYtoZ.GetY(), kYtoZ.GetZ()), RayHit::kTerrain);
    }

    auto meshShape = [&](uint32_t meshId, bool convex) -> JPH::ShapeRefC {
        const Mesh& m = scene.meshes[meshId];
        std::string key = std::format("{}|{}|{}|{}", m.name, m.vertexCount, m.indexCount, convex);
        if (auto it = p_->meshShapes.find(key); it != p_->meshShapes.end()) return it->second;
        JPH::ShapeRefC shape;
        if (convex) {
            JPH::Array<JPH::Vec3> pts;
            uint32_t stride = std::max(1u, m.vertexCount / 512);
            for (uint32_t i = 0; i < m.vertexCount; i += stride) pts.push_back(J(vec3(scene.vertices[m.vertexStart + i].p0)));
            JPH::ConvexHullShapeSettings cs(pts);
            auto r = cs.Create();
            if (!r.HasError()) shape = r.Get();
        } else {
            JPH::VertexList verts;
            verts.reserve(m.vertexCount);
            for (uint32_t i = 0; i < m.vertexCount; ++i) {
                const vec4& p = scene.vertices[m.vertexStart + i].p0;
                verts.push_back(JPH::Float3(p.x, p.y, p.z));
            }
            JPH::IndexedTriangleList tris;
            for (uint32_t smi : m.lods[0].submeshes) {
                const Submesh& sm = scene.submeshes[smi];
                for (uint32_t k = 0; k + 2 < sm.indexCount; k += 3)
                    tris.push_back(JPH::IndexedTriangle(scene.indices[sm.firstIndex + k], scene.indices[sm.firstIndex + k + 1],
                                                        scene.indices[sm.firstIndex + k + 2], 0));
            }
            if (tris.empty()) return nullptr;
            JPH::MeshShapeSettings ms(std::move(verts), std::move(tris));
            ms.mPerTriangleUserData = false;
            auto r = ms.Create();
            if (r.HasError()) { logWarn("physics: mesh '{}': {}", m.name, r.GetError().c_str()); return nullptr; }
            shape = r.Get();
        }
        p_->meshShapes[key] = shape;
        return shape;
    };

    for (const InstanceSet& set : scene.sets) {
        if (set.collision.kind == CollisionKind::None) continue;
        for (uint32_t k = 0; k < set.count; ++k) {
            uint32_t idx = set.first + k;
            const GpuInstance& inst = scene.instances[idx];
            const Mesh& m = scene.meshes[inst.mesh];
            vec3 s3 = scene.axisScale(inst);
            float sc = inst.posScale.w;   // largest axis
            vec3 pos = vec3(inst.posScale);
            quat rot(inst.rot.w, inst.rot.x, inst.rot.y, inst.rot.z);
            JPH::ShapeRefC shape;
            switch (set.collision.kind) {
                case CollisionKind::Mesh:
                case CollisionKind::Convex: {
                    JPH::ShapeRefC base = meshShape(inst.mesh, set.collision.kind == CollisionKind::Convex);
                    if (!base) continue;
                    bool unit = glm::all(glm::lessThan(glm::abs(s3 - 1.0f), vec3(1e-4f)));
                    shape = unit ? base : JPH::ShapeRefC(new JPH::ScaledShape(base, J(s3)));
                    break;
                }
                case CollisionKind::Box: {
                    vec3 he = glm::max(glm::abs((m.aabbMax - m.aabbMin) * 0.5f * s3), vec3(0.02f));
                    vec3 c = (m.aabbMax + m.aabbMin) * 0.5f * s3;
                    shape = new JPH::RotatedTranslatedShape(J(c), JPH::Quat::sIdentity(), new JPH::BoxShape(J(he), std::min(0.05f, glm::min(he.x, glm::min(he.y, he.z)) * 0.5f)));
                    break;
                }
                case CollisionKind::Cylinder: {
                    float r = set.collision.radius * std::max(std::abs(s3.x), std::abs(s3.y)), h = set.collision.height * std::abs(s3.z);
                    shape = new JPH::RotatedTranslatedShape(JPH::Vec3(0, 0, h * 0.5f), kYtoZ, new JPH::CylinderShape(h * 0.5f, r, std::min(0.05f, r * 0.5f)));
                    break;
                }
                case CollisionKind::Sphere:
                    shape = new JPH::RotatedTranslatedShape(J(vec3(m.bounds) * s3), JPH::Quat::sIdentity(), new JPH::SphereShape(m.bounds.w * sc));
                    break;
                default: continue;
            }
            addBody(shape, pos, rot, idx);
        }
    }
    if (!created.empty()) {
        JPH::BodyInterface::AddState st = bi.AddBodiesPrepare(created.data(), (int)created.size());
        bi.AddBodiesFinalize(created.data(), (int)created.size(), st, JPH::EActivation::DontActivate);
    }
    p_->bodies = std::move(created);
    p_->system->OptimizeBroadPhase();
}

RayHit Physics::raycast(vec3 origin, vec3 dir, float maxDistance) const {
    RayHit h;
    if (!p_->system) return h;
    vec3 d = glm::normalize(dir) * maxDistance;
    JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z), J(d)};
    JPH::RayCastResult r;
    if (!p_->system->GetNarrowPhaseQuery().CastRay(ray, r)) return h;
    h.hit = true;
    h.distance = r.mFraction * maxDistance;
    h.position = origin + d * r.mFraction;
    JPH::BodyLockRead lock(p_->system->GetBodyLockInterface(), r.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& b = lock.GetBody();
        h.normal = G(b.GetWorldSpaceSurfaceNormal(r.mSubShapeID2, ray.GetPointOnRay(r.mFraction)));
        h.instance = (uint32_t)b.GetUserData();
    }
    return h;
}

RayHit Physics::groundBelow(float x, float y, float zTop, float zBottom) const {
    return raycast(vec3(x, y, zTop), vec3(0, 0, -1), zTop - zBottom);
}

void Physics::createCharacter(const CharacterSettings& s, vec3 feet) {
    destroyCharacter();
    p_->charSettings = s;
    float halfCyl = std::max(0.05f, s.height * 0.5f - s.radius);
    JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
    cs->mShape = new JPH::RotatedTranslatedShape(JPH::Vec3(0, 0, halfCyl + s.radius), kYtoZ, new JPH::CapsuleShape(halfCyl, s.radius));
    cs->mUp = JPH::Vec3::sAxisZ();
    cs->mMaxSlopeAngle = JPH::DegreesToRadians(s.maxSlopeDeg);
    cs->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisZ(), -s.radius);
    cs->mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    p_->character = new JPH::CharacterVirtual(cs, JPH::RVec3(feet.x, feet.y, feet.z), JPH::Quat::sIdentity(), 0, p_->system.get());
    p_->verticalVelocity = 0;
}

void Physics::destroyCharacter() { if (p_) p_->character = nullptr; }
bool Physics::hasCharacter() const { return p_->character != nullptr; }

void Physics::teleportCharacter(vec3 feet) {
    if (!p_->character) return;
    p_->character->SetPosition(JPH::RVec3(feet.x, feet.y, feet.z));
    p_->character->SetLinearVelocity(JPH::Vec3::sZero());
    p_->verticalVelocity = 0;
}

CharacterState Physics::characterState() const {
    CharacterState st;
    if (!p_->character) return st;
    auto& c = *p_->character;
    JPH::RVec3 pos = c.GetPosition();
    st.position = vec3((float)pos.GetX(), (float)pos.GetY(), (float)pos.GetZ());
    st.velocity = G(c.GetLinearVelocity());
    st.onGround = c.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    st.onSteepGround = c.GetGroundState() == JPH::CharacterBase::EGroundState::OnSteepGround;
    st.groundNormal = G(c.GetGroundNormal());
    return st;
}

CharacterState Physics::stepCharacter(float dt, vec2 wish, float jumpSpeed, float gravity) {
    if (!p_->character) return {};
    auto& c = *p_->character;
    c.UpdateGroundVelocity();
    bool grounded = c.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    JPH::Vec3 v = c.GetLinearVelocity();
    float vz = v.GetZ();
    if (grounded && vz <= 0.1f) vz = 0.0f;
    if (grounded && jumpSpeed > 0) vz = jumpSpeed;
    vz -= gravity * dt;
    vec2 horiz = wish;
    if (!grounded) {   // limited air control
        vec2 cur(v.GetX(), v.GetY());
        horiz = cur + (wish - cur) * std::min(1.0f, dt * 2.5f);
    }
    c.SetLinearVelocity(JPH::Vec3(horiz.x, horiz.y, vz));
    JPH::CharacterVirtual::ExtendedUpdateSettings us;
    us.mStickToFloorStepDown = JPH::Vec3(0, 0, -0.5f);
    us.mWalkStairsStepUp = JPH::Vec3(0, 0, p_->charSettings.stepHeight);
    c.ExtendedUpdate(dt, JPH::Vec3(0, 0, -gravity), us, p_->system->GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
                     p_->system->GetDefaultLayerFilter(Layers::kMoving), {}, {}, *p_->temp);
    return characterState();
}
}  // namespace df
