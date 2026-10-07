#pragma once
// Skeletal animation of rigged characters: the skeleton (glTF nodes), clips of
// translation / rotation / scale tracks, pose sampling and blending, and CPU
// skinning into engine vertices. Loaded by GltfLoader (loadGltfCharacter,
// loadGltfClips); the player's state machine is game/CharacterAnim.
//
// Poses are local node transforms in glTF space; Skeleton::base takes the
// glTF scene into model space (Z up, the character facing +Y, feet at the
// origin), so skin matrices come out in model space like the bind-pose mesh.
#include "scene/MeshAsset.h"
#include <string>
#include <vector>

namespace df {
struct Skeleton {
    std::vector<std::string> names;     // glTF node names
    std::vector<int> parent;            // -1: a root; parents come before their children
    std::vector<vec3> restT, restS;     // rest pose, local, glTF space
    std::vector<quat> restR;
    mat4 base{1};                       // glTF scene -> model space
    int find(const std::string& name) const;   // exact, else ignoring case and "prefix:" / "prefix|"
    mat4 restGlobal(int node) const;           // glTF scene space
};

struct AnimTrack {
    uint32_t node = 0;
    uint8_t path = 0;                   // 0 translation, 1 rotation, 2 scale
    uint8_t interp = 0;                 // 0 linear, 1 step, 2 cubic spline (values only, tangents ignored)
    std::vector<float> times;           // seconds from the clip start
    std::vector<vec4> values;           // xyz, or a quaternion xyzw
};
struct AnimClip {
    std::string name, source;           // source: the file it came from
    float duration = 0;
    float speed = 0;                    // ground speed (m/s) from glTF animation extras "speed_mps"; 0 = unknown
    std::vector<AnimTrack> tracks;
    int rootNode = -1;                  // the topmost node with a translation track (root motion)
    vec2 travel{0};                     // its horizontal travel over the clip, model space (m)
};

// skin matrix = global(node) * offset (model space); node -1: identity (unrigged parts)
struct SkinBone { int node = -1; mat4 offset{1}; };

// A rigged character: the bind-pose mesh in model space plus up to 4 bone influences per vertex.
struct CharacterAsset {
    MeshAsset mesh;
    std::vector<uvec4> joints;          // per vertex, into bones
    std::vector<vec4> weights;
    std::vector<SkinBone> bones;
    Skeleton skeleton;
    std::vector<AnimClip> clips;
    std::vector<std::string> warnings;  // e.g. clip tracks whose nodes the skeleton lacks
    int clip(const std::string& name) const;   // exact, else ignoring case; -1 if missing
    float height() const { return mesh.aabbMax.z - mesh.aabbMin.z; }
};

struct Pose { std::vector<vec3> t, s; std::vector<quat> r; };
void restPose(const Skeleton& sk, Pose& out);
// Samples a clip at `time` (wrapped when looping, clamped otherwise) over `out`
// (nodes without tracks keep their values). stripTravel removes the root
// node's horizontal travel so the clip plays in place.
void sampleClip(const CharacterAsset& c, const AnimClip& clip, float time, bool loop, bool stripTravel, Pose& out);
void blendPose(Pose& a, const Pose& b, float w);   // a = mix(a, b, w)
// Skins every vertex of the character for a pose (out: mesh.vertices.size() entries).
void skinVertices(const CharacterAsset& c, const Pose& pose, GpuVertex* out);
// Fills AnimClip::rootNode / travel (needs the skeleton).
void measureTravel(const Skeleton& sk, AnimClip& clip);
}  // namespace df
