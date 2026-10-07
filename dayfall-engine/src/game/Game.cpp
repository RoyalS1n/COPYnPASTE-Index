#include "game/Game.h"
#include "core/Log.h"
#include <cmath>
#include <format>

namespace df {
using json = nlohmann::json;

PlayerConfig PlayerConfig::parse(const json& j) {
    PlayerConfig c;
    if (!j.is_object()) return c;
    c.character = j.value("character", c.character);
    c.camera = j.value("camera", c.camera);
    c.walkSpeed = j.value("walk_speed", c.walkSpeed);
    c.runSpeed = j.value("run_speed", c.runSpeed);
    c.jumpHeight = j.value("jump_height_m", c.jumpHeight);
    c.height = j.value("height_m", c.height);
    c.radius = j.value("radius_m", c.radius);
    c.maxSlopeDeg = j.value("max_slope_deg", c.maxSlopeDeg);
    c.stepHeight = j.value("step_height_m", c.stepHeight);
    c.cameraDistance = j.value("camera_distance_m", c.cameraDistance);
    if (j.contains("animations") && j["animations"].is_object()) c.animations = j["animations"];
    for (auto& f : j.value("animation_files", json::array())) if (f.is_string()) c.animationFiles.push_back(f.get<std::string>());
    c.characterScale = j.value("character_scale", c.characterScale);
    c.characterYawOffsetDeg = j.value("character_yaw_offset_deg", c.characterYawOffsetDeg);
    c.rootMotion = j.value("root_motion", c.rootMotion);
    c.animationBlend = j.value("animation_blend_s", c.animationBlend);
    return c;
}

json PlayerConfig::toJson() const {
    return {{"character", character}, {"camera", camera}, {"walk_speed", walkSpeed}, {"run_speed", runSpeed},
            {"jump_height_m", jumpHeight}, {"height_m", height}, {"radius_m", radius}, {"max_slope_deg", maxSlopeDeg},
            {"step_height_m", stepHeight}, {"camera_distance_m", cameraDistance}, {"animations", animations},
            {"animation_files", animationFiles}, {"character_scale", characterScale}, {"character_yaw_offset_deg", characterYawOffsetDeg},
            {"root_motion", rootMotion}, {"animation_blend_s", animationBlend}};
}

void Game::begin(const Scene& scene, std::vector<EntityState>& entities, Physics& physics, const PlayerConfig& cfg,
                 const SceneBuilder& builder) {
    config = cfg;
    partsFirst_ = builder.playerFirst;
    partsCount_ = builder.playerCount;
    CharacterSettings cs{cfg.height, cfg.radius, cfg.maxSlopeDeg, cfg.stepHeight};
    feet = scene.playerStart.position + vec3(0, 0, 0.05f);
    facing = viewYaw = glm::radians(scene.playerStart.yawDeg - 90.0f);   // yaw_deg 90 = facing +Y
    viewPitch = -0.12f;
    physics.createCharacter(cs, feet);
    anim_.reset();
    active_ = true;
    time = 0;
    collected = 0;
    collectedIds.clear();
    collectibles = 0;
    for (auto& e : entities) if (e.type == "collectible") ++collectibles;
    goalInside_.assign(entities.size(), false);
    messages.clear();
    if (collectibles) messages.push_back({std::format("Find the {} glowing collectibles", collectibles), 5.0f});
}

void Game::end(Physics& physics, Scene& scene) {
    physics.destroyCharacter();
    active_ = false;
    poseCharacter(scene, false);
}

void Game::teleport(Physics& physics, vec3 f, float yawDeg) {
    feet = f;
    facing = viewYaw = glm::radians(yawDeg - 90.0f);
    physics.teleportCharacter(f);
}

void Game::rebind(const SceneBuilder& builder, const PlayerConfig& cfg) {
    partsFirst_ = builder.playerFirst;
    partsCount_ = builder.playerCount;
    config = cfg;
    anim_.bind(builder.character, cfg);
}

void Game::showIdle(Scene& scene, const SceneBuilder& builder, vec3 at, float yaw, bool visible) {
    partsFirst_ = builder.playerFirst;
    partsCount_ = builder.playerCount;
    if (visible) anim_.reset();
    vec3 keepFeet = feet;
    float keepFacing = facing;
    feet = at;
    facing = yaw;
    MannequinPose keep = pose_;
    pose_ = MannequinPose();
    poseCharacter(scene, visible);
    pose_ = keep;
    feet = keepFeet;
    facing = keepFacing;
}

void Game::animateEntities(float t, Scene& scene, const std::vector<EntityState>& entities) {
    for (auto& e : entities) {
        if (e.instance == UINT32_MAX || e.instance >= scene.instances.size()) continue;
        GpuInstance& inst = scene.instances[e.instance];
        if (e.type == "collectible") {
            if (inst.cullDistance < 0) continue;   // collected
            float ph = (float)(std::hash<std::string>{}(e.id) % 628) * 0.01f;
            inst.posScale = vec4(e.position + vec3(0, 0, 0.18f * std::sin(t * 2.0f + ph)), 1.0f);
            quat q = glm::angleAxis(t * 1.6f + ph, vec3(0, 0, 1));
            inst.rot = vec4(q.x, q.y, q.z, q.w);
        }
    }
}

void Game::poseCharacter(Scene& scene, bool visible) {
    if (partsCount_ == 0 || partsFirst_ + partsCount_ > scene.instances.size()) return;
    if (const CharacterAsset* c = anim_.character()) {   // rigged: one instance whose vertices are skinned on the CPU
        GpuInstance& inst = scene.instances[partsFirst_];
        quat q = glm::angleAxis(facing + glm::radians(config.characterYawOffsetDeg), vec3(0, 0, 1));
        inst.posScale = vec4(feet, config.characterScale);
        inst.rot = vec4(q.x, q.y, q.z, q.w);
        inst.cullDistance = visible ? 2000.0f : -1.0f;
        const Mesh& m = scene.meshes[inst.mesh];
        if (visible && m.vertexCount == c->mesh.vertices.size() && m.vertexStart + m.vertexCount <= scene.vertices.size())
            anim_.skin(&scene.vertices[m.vertexStart]);
        return;
    }
    std::vector<vec3> pos;
    std::vector<quat> rot;
    poseMannequin(pose_, feet, facing, config.height / 1.8f, pos, rot);
    for (uint32_t i = 0; i < partsCount_; ++i) {
        GpuInstance& inst = scene.instances[partsFirst_ + i];
        inst.posScale = vec4(pos[i], config.height / 1.8f);
        inst.rot = vec4(rot[i].x, rot[i].y, rot[i].z, rot[i].w);
        inst.cullDistance = visible ? 2000.0f : -1.0f;
    }
}

void Game::update(float dt, const PlayerInput& in, Physics& physics, Scene& scene, std::vector<EntityState>& entities) {
    if (!active_) return;
    time += dt;
    bool fp = config.camera == "first_person";
    viewYaw += in.lookYaw;
    viewPitch = std::clamp(viewPitch + in.lookPitch, -1.35f, 1.2f);
    // camera-relative movement
    vec2 fwd(-std::sin(viewYaw), std::cos(viewYaw)), right(fwd.y, -fwd.x);
    vec2 move = in.move;
    if (glm::length(move) > 1.0f) move = glm::normalize(move);
    vec2 wishDir = fwd * move.y + right * move.x;
    float speed = in.run ? config.runSpeed : config.walkSpeed;
    vec2 wish = wishDir * speed;
    float jump = in.jump ? std::sqrt(2.0f * 9.81f * config.jumpHeight) : 0.0f;
    bool wasGround = onGround;
    CharacterState st = physics.stepCharacter(dt, wish, jump);
    feet = st.position;
    velocity = st.velocity;
    onGround = st.onGround;
    // facing follows movement (third person) or the view (first person)
    if (fp) facing = viewYaw;
    else if (glm::length(wishDir) > 0.1f) {
        float target = std::atan2(-wishDir.x, wishDir.y);
        float d = std::remainder(target - facing, 6.2831853f);
        facing += d * std::min(1.0f, dt * 10.0f);
    }
    // procedural animation (mannequin) or the clip state machine (rigged character)
    float hs = glm::length(vec2(velocity));
    anim_.update(dt, hs, onGround, velocity.z, in.jump && wasGround && velocity.z > 0.5f);
    pose_.stride = std::clamp(hs / config.runSpeed, 0.0f, 1.0f);
    pose_.phase += dt * (hs / std::max(0.9f * config.height / 1.8f, 0.3f)) * 1.6f;
    pose_.airborne = glm::mix(pose_.airborne, onGround ? 0.0f : 1.0f, std::min(1.0f, dt * 8.0f));
    pose_.lean = 0.12f * pose_.stride;
    pose_.time = time;
    poseCharacter(scene, !fp);
    // pickups and goals
    vec3 centre = feet + vec3(0, 0, config.height * 0.5f);
    for (size_t i = 0; i < entities.size(); ++i) {
        EntityState& e = entities[i];
        float d = glm::length(e.position - centre);
        if (e.type == "collectible" && e.instance < scene.instances.size() && scene.instances[e.instance].cullDistance >= 0 && d < e.radius + 0.4f) {
            scene.instances[e.instance].cullDistance = -1.0f;
            if (e.light >= 0 && e.light < (int)scene.lights.size()) { scene.lights[e.light].intensity = 0.0f; lightsChanged = true; }
            ++collected;
            collectedIds.push_back(e.id);
            if (!e.message.empty()) messages.push_back({e.message, 3.5f});
            messages.push_back({collected == collectibles ? std::format("All {} collected!", collectibles)
                                                         : std::format("Collected {} / {}", collected, collectibles), 3.0f});
        } else if (e.type == "goal" || e.type == "trigger") {
            bool inside = glm::length(vec2(e.position) - vec2(feet)) < e.radius && std::abs(e.position.z - feet.z) < 4.0f;
            if (inside && !goalInside_[i]) {
                bool goal = e.type == "goal";
                messages.push_back({e.message.empty() ? (goal ? std::string("You made it!") : e.id) : e.message, goal ? 5.0f : 4.0f, goal});
            }
            goalInside_[i] = inside;
        }
    }
    for (auto& m : messages) m.timeLeft -= dt;
    std::erase_if(messages, [](const HudMessage& m) { return m.timeLeft <= 0; });
    animateEntities(time, scene, entities);
}

Camera Game::camera(const Physics& physics, float) const {
    Camera c;
    c.vfov = glm::radians(config.camera == "first_person" ? 70.0f : 55.0f);
    c.yaw = viewYaw;
    c.pitch = viewPitch;
    float eye = config.height * 0.92f;
    if (config.camera == "first_person") {
        c.position = feet + vec3(0, 0, eye);
        c.nearPlane = 0.05f;
        return c;
    }
    vec3 pivot = feet + vec3(0, 0, config.height * 0.85f);
    vec3 back = -c.forward();
    vec3 right = glm::normalize(glm::cross(c.forward(), vec3(0, 0, 1)));
    vec3 want = pivot + back * config.cameraDistance + right * 0.45f + vec3(0, 0, 0.25f);
    RayHit h = physics.raycast(pivot, want - pivot, glm::length(want - pivot));
    c.position = h.hit ? pivot + (want - pivot) * std::max(0.1f, (h.distance - 0.25f) / glm::length(want - pivot)) : want;
    c.nearPlane = 0.1f;
    return c;
}

json runWalkTest(const WalkTestOptions& opt, const Scene& scene, std::vector<EntityState> entities, Physics& physics,
                 const PlayerConfig& cfg, float waterLevel, std::vector<vec3>* problems) {
    json events = json::array(), trace = json::array();
    if (opt.points.size() < 2) return {{"error", "a route needs at least 2 points"}};
    CharacterSettings cs{cfg.height, cfg.radius, cfg.maxSlopeDeg, cfg.stepHeight};
    auto ground = [&](vec2 p) {
        RayHit h = physics.groundBelow(p.x, p.y);
        return h.hit ? h.position.z : 0.0f;
    };
    vec3 start(opt.points[0], ground(opt.points[0]) + 0.05f);
    physics.createCharacter(cs, start);
    const float dt = 1.0f / 60.0f;
    float speed = opt.run ? cfg.runSpeed : cfg.walkSpeed;
    size_t target = 1;
    float t = 0, dist = 0, maxSlope = 0, stuckTimer = 0, bestDist = 1e30f, airborneFrom = start.z, lastTrace = -1;
    bool wasGround = true, inWater = false;
    vec3 prev = start;
    int jumps = 0, pickups = 0;
    std::string lastType;
    vec3 lastPos(1e9f);
    auto event = [&](const std::string& type, vec3 p, const std::string& detail) {
        // one event per problem spot: the same kind within 4 m is the same problem
        if (type == lastType && glm::length(p - lastPos) < 4.0f && type != "pickup") return;
        lastType = type;
        lastPos = p;
        if (events.size() < 40) events.push_back({{"type", type}, {"time_s", std::round(t * 10) / 10},
                                                  {"position", {std::round(p.x * 10) / 10, std::round(p.y * 10) / 10, std::round(p.z * 10) / 10}},
                                                  {"detail", detail}});
        if (problems && type != "pickup" && problems->size() < 6) problems->push_back(p);
    };
    std::vector<bool> taken(entities.size(), false);
    bool cancelled = false;
    int frame = 0;
    while (t < opt.maxSeconds && target < opt.points.size()) {
        if (opt.progress && frame++ % 30 == 0 && !opt.progress(target - 1, opt.points.size() - 1, t)) {
            cancelled = true;
            break;
        }
        CharacterState st = physics.characterState();
        vec2 to = opt.points[target] - vec2(st.position);
        float d = glm::length(to);
        if (d < opt.waypointRadius) {
            ++target;
            bestDist = 1e30f;
            stuckTimer = 0;
            continue;
        }
        bool jump = false;
        if (d < bestDist - 0.25f) { bestDist = d; stuckTimer = 0; }
        else stuckTimer += dt;
        if (stuckTimer > 1.2f && st.onGround && std::fmod(stuckTimer, 1.0f) < dt) { jump = true; ++jumps; }
        if (stuckTimer > 6.0f) {
            event("stuck", st.position, std::format("no progress towards waypoint {} for 6 s ({:.1f} m away); skipped ahead", target, d));
            // put the character at the waypoint and continue, so one problem doesn't hide the rest
            vec2 wp = opt.points[target];
            physics.teleportCharacter(vec3(wp, ground(wp) + 0.1f));
            ++target;
            bestDist = 1e30f;
            stuckTimer = 0;
            continue;
        }
        vec2 wish = glm::normalize(to) * speed;
        st = physics.stepCharacter(dt, wish, jump ? std::sqrt(2.0f * 9.81f * cfg.jumpHeight) : 0.0f);
        t += dt;
        dist += glm::length(vec2(st.position) - vec2(prev));
        if (st.onGround) maxSlope = std::max(maxSlope, glm::degrees(std::acos(std::clamp(st.groundNormal.z, -1.0f, 1.0f))));
        if (!st.onGround && wasGround) airborneFrom = st.position.z;
        if (st.onGround && !wasGround && airborneFrom - st.position.z > 3.0f)
            event("fall", st.position, std::format("dropped {:.1f} m", airborneFrom - st.position.z));
        if (st.onSteepGround && stuckTimer > 0.5f) event("steep", st.position, "standing on a slope too steep to walk up");
        wasGround = st.onGround;
        float surface = opt.waterAt ? opt.waterAt(vec2(st.position)) : waterLevel;
        bool wet = st.position.z < surface - 0.4f;
        if (wet && !inWater) event("water", st.position, std::format("walked into water {:.1f} m deep", surface - st.position.z));
        inWater = wet;
        if (st.position.z < scene.boundsMin.z - 50.0f) { event("fell_out_of_world", st.position, "below the world"); break; }
        for (size_t i = 0; i < entities.size(); ++i)
            if (!taken[i] && entities[i].type == "collectible" &&
                glm::length(entities[i].position - (st.position + vec3(0, 0, cfg.height * 0.5f))) < entities[i].radius + 0.4f) {
                taken[i] = true;
                ++pickups;
                event("pickup", st.position, entities[i].id);
            }
        if (t - lastTrace >= 1.0f) {
            trace.push_back({std::round(st.position.x * 10) / 10, std::round(st.position.y * 10) / 10, std::round(st.position.z * 10) / 10});
            lastTrace = t;
        }
        prev = st.position;
    }
    CharacterState end = physics.characterState();
    physics.destroyCharacter();
    bool reached = target >= opt.points.size();
    if (cancelled) return {{"cancelled", true}, {"time_s", std::round(t * 10) / 10}};
    if (!reached && t >= opt.maxSeconds) event("timeout", end.position, std::format("ran out of time at waypoint {}", target));
    int problemsCount = 0;
    for (auto& e : events) if (e["type"] != "pickup") ++problemsCount;
    int total = 0;
    for (auto& e : entities) if (e.type == "collectible") ++total;
    return {{"passed", reached && problemsCount == 0},
            {"reached_end", reached},
            {"waypoints_reached", std::format("{}/{}", std::min(target, opt.points.size()) - 1, opt.points.size() - 1)},
            {"time_s", std::round(t * 10) / 10},
            {"distance_m", std::round(dist)},
            {"max_ground_slope_deg", std::round(maxSlope)},
            {"auto_jumps", jumps},
            {"collectibles_picked_up", std::format("{}/{}", pickups, total)},
            {"problems", problemsCount},
            {"events", events},
            {"trace_1s", trace}};
}
}  // namespace df
