#pragma once
// The player's animation state machine for rigged characters: idle / walk /
// run / jump / fall / land, chosen from the controller's horizontal speed,
// ground contact and vertical velocity, with crossfades. Walk and run play at
// a rate matched to the ground speed (clip speed_mps).
#include "scene/Skin.h"
#include <array>
#include <memory>
#include <nlohmann/json.hpp>

namespace df {
struct PlayerConfig;
enum class AnimState { Idle, Walk, Run, Jump, Fall, Land, Count };

class CharacterAnimator {
public:
    // binds a character (null: none) and maps the states to its clips from the player config
    void bind(std::shared_ptr<const CharacterAsset> c, const PlayerConfig& cfg);
    const CharacterAsset* character() const { return c_.get(); }
    void reset();   // idle from the start
    // speed: horizontal (m/s); vz: vertical velocity (m/s); jumped: took off this step
    void update(float dt, float speed, bool onGround, float vz, bool jumped);
    void skin(GpuVertex* out);   // poses the character and skins it into out
    AnimState state() const { return cur_.state; }
    static const char* name(AnimState s);
    // clips, the state -> clip mapping, problems, skinning cost
    nlohmann::json info() const;
    std::vector<std::string> problems;   // explicitly mapped clips that do not exist

private:
    struct Slot { int clip = -1; float speed = 0; };
    struct Layer { AnimState state = AnimState::Idle; float time = 0; };
    int source(AnimState s) const;       // the state whose clip plays for s (fallbacks), -1: rest pose
    const AnimClip* clip(AnimState s) const { int k = source(s); return k < 0 ? nullptr : &c_->clips[slots_[k].clip]; }
    float rate(AnimState s, float speed) const;
    AnimState locomotion(float speed) const;
    void enter(AnimState s, float fade);
    void samplePose(const Layer& l, Pose& out) const;
    std::shared_ptr<const CharacterAsset> c_;
    std::array<Slot, (size_t)AnimState::Count> slots_{};
    Layer cur_, prev_;
    float fade_ = 1, fadeLen_ = 0.2f, blend_ = 0.2f, stateTime_ = 0, airTime_ = 0, lastAir_ = 0, speed_ = 0, runAbove_ = 6;
    bool strip_ = false;
    Pose pose_, other_;
    double skinMs_ = 0;
    uint64_t skins_ = 0;
};
}  // namespace df
