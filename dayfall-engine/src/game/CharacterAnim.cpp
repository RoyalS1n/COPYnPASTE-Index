#include "game/CharacterAnim.h"
#include "game/Game.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace df {
using json = nlohmann::json;
namespace {
bool loops(AnimState s) { return s == AnimState::Idle || s == AnimState::Walk || s == AnimState::Run || s == AnimState::Fall; }
double r2(double v) { return std::round(v * 100.0) / 100.0; }
}  // namespace

const char* CharacterAnimator::name(AnimState s) {
    static const char* n[] = {"idle", "walk", "run", "jump", "fall", "land"};
    return s < AnimState::Count ? n[(int)s] : "?";
}

void CharacterAnimator::bind(std::shared_ptr<const CharacterAsset> c, const PlayerConfig& cfg) {
    bool changed = c != c_;
    c_ = std::move(c);
    problems.clear();
    slots_.fill({});
    strip_ = cfg.rootMotion == "strip";
    blend_ = std::clamp(cfg.animationBlend, 0.0f, 1.0f);
    if (changed) reset();
    if (!c_) return;
    for (int k = 0; k < (int)AnimState::Count; ++k) {
        std::string key = name((AnimState)k);
        Slot& s = slots_[k];
        json e = cfg.animations.is_object() && cfg.animations.contains(key) ? cfg.animations[key] : json();
        std::string want = e.is_string() ? e.get<std::string>() : e.is_object() && e.contains("clip") && e["clip"].is_string() ? e["clip"].get<std::string>() : "";
        if (!want.empty()) {
            s.clip = c_->clip(want);
            if (s.clip < 0) problems.push_back(std::format("animations.{}: the character has no clip '{}'", key, want));
        } else {   // by name: the shortest clip name containing the state ("Walk", "Running", ...)
            for (size_t i = 0; i < c_->clips.size(); ++i) {
                std::string n = c_->clips[i].name;
                for (auto& ch : n) ch = (char)std::tolower((unsigned char)ch);
                if (n.find(key) != std::string::npos && (s.clip < 0 || n.size() < c_->clips[s.clip].name.size())) s.clip = (int)i;
            }
        }
        if (s.clip < 0) continue;
        const AnimClip& a = c_->clips[s.clip];
        float travel = a.duration > 0 ? glm::length(a.travel) / a.duration : 0.0f;
        s.speed = e.is_object() && e.contains("speed_mps") && e["speed_mps"].is_number() ? e["speed_mps"].get<float>()
                  : a.speed > 0 ? a.speed : travel > 0.2f ? travel : 0.0f;
    }
    // unknown clip speeds: the controller's walk / run speeds; the run clip takes over halfway between the two
    Slot& w = slots_[(int)AnimState::Walk];
    Slot& r = slots_[(int)AnimState::Run];
    if (w.clip >= 0 && w.speed <= 0) w.speed = cfg.walkSpeed;
    if (r.clip >= 0 && r.speed <= 0) r.speed = cfg.runSpeed;
    runAbove_ = w.clip >= 0 && r.clip >= 0 ? 0.5f * (w.speed + r.speed) : w.clip >= 0 ? 1e9f : 0.0f;
}

void CharacterAnimator::reset() {
    cur_ = prev_ = Layer();
    fade_ = 1;
    stateTime_ = airTime_ = lastAir_ = 0;
}

int CharacterAnimator::source(AnimState s) const {
    using A = AnimState;
    static const A order[(int)A::Count][3] = {{A::Idle, A::Idle, A::Idle}, {A::Walk, A::Run, A::Idle}, {A::Run, A::Walk, A::Idle},
                                              {A::Jump, A::Fall, A::Idle}, {A::Fall, A::Jump, A::Idle}, {A::Land, A::Idle, A::Idle}};
    for (A a : order[(int)s]) if (slots_[(int)a].clip >= 0) return (int)a;
    return -1;
}

float CharacterAnimator::rate(AnimState s, float speed) const {
    if (s != AnimState::Walk && s != AnimState::Run) return 1.0f;
    int k = source(s);
    if (k < 0 || slots_[k].speed <= 0 || (k != (int)AnimState::Walk && k != (int)AnimState::Run)) return 1.0f;
    return std::clamp(speed / slots_[k].speed, 0.5f, 2.0f);
}

AnimState CharacterAnimator::locomotion(float v) const {
    using A = AnimState;
    bool moving = cur_.state == A::Walk || cur_.state == A::Run;
    if (v < (moving ? 0.2f : 0.4f)) return A::Idle;
    if (cur_.state == A::Run) return v < runAbove_ - 0.3f ? A::Walk : A::Run;
    return v > runAbove_ + 0.3f ? A::Run : A::Walk;
}

void CharacterAnimator::enter(AnimState s, float fade) {
    using A = AnimState;
    float phase = -1;   // walk <-> run: keep the feet in step
    const AnimClip* from = clip(cur_.state);
    if (((cur_.state == A::Walk && s == A::Run) || (cur_.state == A::Run && s == A::Walk)) && from && from->duration > 0)
        phase = std::fmod(cur_.time, from->duration) / from->duration;
    prev_ = cur_;
    cur_ = {s, 0.0f};
    const AnimClip* to = clip(s);
    if (phase >= 0 && to) cur_.time = phase * to->duration;
    fade_ = fade > 0 ? 0.0f : 1.0f;
    fadeLen_ = fade;
    stateTime_ = 0;
}

void CharacterAnimator::update(float dt, float speed, bool onGround, float vz, bool jumped) {
    if (!c_) return;
    using A = AnimState;
    speed_ = speed;
    stateTime_ += dt;
    if (onGround) airTime_ = 0;
    else lastAir_ = airTime_ += dt;
    A s = cur_.state, next = s;
    const AnimClip* cl = clip(s);
    bool done = !cl || cur_.time >= cl->duration;
    if (jumped) next = A::Jump;
    else if (s == A::Jump || s == A::Fall) {
        if (onGround && stateTime_ > 0.15f) next = slots_[(int)A::Land].clip >= 0 && lastAir_ > 0.35f ? A::Land : locomotion(speed);
        else if (s == A::Jump && !onGround && vz < -1.5f && done) next = A::Fall;
    } else if (!onGround && airTime_ > 0.2f && vz < -2.5f) {
        next = A::Fall;   // walked off a ledge
    } else if (s == A::Land) {
        if (!cl || cur_.time >= std::min(cl->duration, 0.8f) || (speed > 0.6f && stateTime_ > 0.15f)) next = locomotion(speed);
    } else {
        next = locomotion(speed);
    }
    if (next != s || jumped)
        enter(next, next == A::Jump || next == A::Land ? std::min(blend_, 0.12f) : next == A::Fall ? std::max(blend_, 0.25f) : blend_);
    cur_.time += dt * rate(cur_.state, speed);
    prev_.time += dt * rate(prev_.state, speed);
    fade_ = fadeLen_ > 0 ? std::min(1.0f, fade_ + dt / fadeLen_) : 1.0f;
}

void CharacterAnimator::samplePose(const Layer& l, Pose& out) const {
    restPose(c_->skeleton, out);
    if (const AnimClip* a = clip(l.state)) sampleClip(*c_, *a, l.time, loops(l.state), strip_, out);
}

void CharacterAnimator::skin(GpuVertex* out) {
    if (!c_) return;
    auto t0 = std::chrono::steady_clock::now();
    samplePose(cur_, pose_);
    if (fade_ < 1) {   // crossfade from the previous state
        samplePose(prev_, other_);
        blendPose(other_, pose_, fade_ * fade_ * (3 - 2 * fade_));
        std::swap(other_, pose_);
    }
    skinVertices(*c_, pose_, out);
    skinMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++skins_;
}

json CharacterAnimator::info() const {
    if (!c_) return {{"character", "mannequin"}};
    json clips = json::array();
    for (const AnimClip& a : c_->clips) {
        json j = {{"name", a.name}, {"seconds", r2(a.duration)}, {"source", a.source}};
        if (a.speed > 0) j["speed_mps"] = r2(a.speed);
        if (glm::length(a.travel) > 0.05f) j["root_travel_m"] = r2(glm::length(a.travel));
        clips.push_back(j);
    }
    json states = json::object();
    for (int k = 0; k < (int)AnimState::Count; ++k) {
        int src = source((AnimState)k);
        std::string v = src < 0 ? "rest pose (no clip)" : c_->clips[slots_[src].clip].name;
        if (src >= 0 && src != k) v += std::format(" (no {} clip)", name((AnimState)k));
        states[name((AnimState)k)] = v;
    }
    json j = {{"character", c_->mesh.name}, {"vertices", c_->mesh.vertices.size()}, {"joints", c_->skeleton.names.size()},
              {"height_m", r2(c_->height())}, {"clips", clips}, {"states", states}, {"root_motion", strip_ ? "strip" : "keep"},
              {"state", name(cur_.state)}, {"skin_ms", skins_ ? r2((float)(skinMs_ / skins_)) : 0.0f}};
    if (slots_[(int)AnimState::Walk].clip >= 0) j["walk_clip_mps"] = r2(slots_[(int)AnimState::Walk].speed);
    if (slots_[(int)AnimState::Run].clip >= 0) j["run_clip_mps"] = r2(slots_[(int)AnimState::Run].speed);
    if (runAbove_ > 0 && runAbove_ < 1e8f) j["run_clip_above_mps"] = r2(runAbove_);
    if (!problems.empty()) j["problems"] = problems;
    if (!c_->warnings.empty()) j["warnings"] = c_->warnings;
    return j;
}
}  // namespace df
