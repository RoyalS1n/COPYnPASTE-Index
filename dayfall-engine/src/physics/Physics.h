#pragma once
// Jolt Physics wrapper: the static world (terrain heightfield + collision of
// placed objects), ray casts, and the player's virtual character controller.
// World space is Z up, metres.
#include "core/Math.h"
#include "scene/Scene.h"
#include "world/Terrain.h"
#include <memory>
#include <string>

namespace df {
struct RayHit {
    bool hit = false;
    vec3 position{0}, normal{0, 0, 1};
    float distance = 0;
    uint32_t instance = UINT32_MAX;   // UINT32_MAX - 1: terrain
    static constexpr uint32_t kTerrain = UINT32_MAX - 1;
};

struct CharacterSettings {
    float height = 1.8f, radius = 0.32f;
    float maxSlopeDeg = 48.0f, stepHeight = 0.4f;
};

struct CharacterState {
    vec3 position{0};        // feet
    vec3 velocity{0};
    bool onGround = false, onSteepGround = false;
    vec3 groundNormal{0, 0, 1};
};

class Physics {
public:
    Physics();
    ~Physics();
    void init();
    void shutdown();
    // Rebuilds every static body from the scene's instance sets and the terrain.
    void buildStatic(const Scene& scene, const Terrain& terrain);
    RayHit raycast(vec3 origin, vec3 dir, float maxDistance) const;
    // highest surface under (x, y) between zTop and zBottom
    RayHit groundBelow(float x, float y, float zTop = 10000.0f, float zBottom = -10000.0f) const;
    size_t staticBodies() const;

    // character ---------------------------------------------------------------
    void createCharacter(const CharacterSettings& s, vec3 feet);
    void destroyCharacter();
    bool hasCharacter() const;
    void teleportCharacter(vec3 feet);
    // horizontal velocity request (m/s), jump: initial upward speed (0 = none)
    CharacterState stepCharacter(float dt, vec2 wishVelocity, float jumpSpeed, float gravity = 9.81f);
    CharacterState characterState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}  // namespace df
