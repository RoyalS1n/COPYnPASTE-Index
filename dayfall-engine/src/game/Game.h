#pragma once
// Gameplay runtime: the player (walk / run / jump with a third- or first-person
// camera), collectibles and goals, HUD state, and the automated walk test.
#include "game/CharacterAnim.h"
#include "game/Mannequin.h"
#include "physics/Physics.h"
#include "render/Camera.h"
#include "world/SceneBuilder.h"
#include <deque>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace df {
struct PlayerConfig {
    std::string character = "mannequin";
    std::string camera = "third_person";     // or first_person
    float walkSpeed = 4.2f, runSpeed = 7.6f, jumpHeight = 1.25f;
    float height = 1.8f, radius = 0.32f, maxSlopeDeg = 48.0f, stepHeight = 0.4f;
    float cameraDistance = 4.2f;
    // rigged characters (character names a skinned mesh): state -> clip ("Walk" or {"clip", "speed_mps"}),
    // extra clip files (map-relative), model scale and turn, root motion "keep" or "strip", crossfade seconds
    nlohmann::json animations = nlohmann::json::object();
    std::vector<std::string> animationFiles;
    float characterScale = 1.0f, characterYawOffsetDeg = 0.0f, animationBlend = 0.2f;
    std::string rootMotion = "keep";
    static PlayerConfig parse(const nlohmann::json& j);   // map "player" section
    nlohmann::json toJson() const;
};

struct PlayerInput {
    vec2 move{0};          // x right, y forward (camera relative), length <= 1
    bool run = false, jump = false;
    float lookYaw = 0, lookPitch = 0;   // radians this frame
};

struct HudMessage { std::string text; float timeLeft; bool banner = false; };   // banner: a goal reached

class Game {
public:
    void begin(const Scene& scene, std::vector<EntityState>& entities, Physics& physics, const PlayerConfig& cfg,
               const SceneBuilder& builder);
    void end(Physics& physics, Scene& scene);
    // after every scene build: the player's instances, its rigged character and the map's player settings
    void rebind(const SceneBuilder& builder, const PlayerConfig& cfg);
    // poses the mannequin standing at `at` (for captures while editing)
    void showIdle(Scene& scene, const SceneBuilder& builder, vec3 at, float yaw, bool visible);
    bool lightsChanged = false;    // a pickup switched a light off
    bool active() const { return active_; }
    // one simulation step; animates entities and the mannequin into scene.instances
    void update(float dt, const PlayerInput& in, Physics& physics, Scene& scene, std::vector<EntityState>& entities);
    // idle animation of pickups while editing
    static void animateEntities(float time, Scene& scene, const std::vector<EntityState>& entities);
    Camera camera(const Physics& physics, float aspect) const;
    const CharacterAnimator& animator() const { return anim_; }
    void teleport(Physics& physics, vec3 feet, float yawDeg);

    // state
    vec3 feet{0};
    float facing = 0, viewYaw = 0, viewPitch = -0.15f;
    vec3 velocity{0};
    bool onGround = false;
    float time = 0;
    int collected = 0, collectibles = 0;
    std::vector<std::string> collectedIds;
    std::deque<HudMessage> messages;
    PlayerConfig config;

private:
    void poseCharacter(Scene& scene, bool visible);
    bool active_ = false;
    uint32_t partsFirst_ = 0, partsCount_ = 0;
    MannequinPose pose_;
    CharacterAnimator anim_;          // rigged characters
    std::vector<bool> goalInside_;
};

// Runs the default character along a route with real physics, as fast as
// possible, and reports where it got stuck, fell, or entered water.
struct WalkTestOptions {
    std::vector<vec2> points;
    bool run = false;
    float maxSeconds = 240.0f;
    float waypointRadius = 1.5f;
};
nlohmann::json runWalkTest(const WalkTestOptions& opt, const Scene& scene, std::vector<EntityState> entities, Physics& physics,
                           const PlayerConfig& cfg, float waterLevel, std::vector<vec3>* problemPoints = nullptr);
}  // namespace df
