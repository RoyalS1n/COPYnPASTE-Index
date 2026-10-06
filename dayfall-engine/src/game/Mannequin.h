#pragma once
// The default test character: a jointed mannequin built from primitives and
// animated procedurally (walk / run / jump / idle). Used until a rigged
// character is assigned (map "player": {"character": ...}).
#include "scene/MeshAsset.h"
#include <string>
#include <vector>

namespace df {
struct MannequinPart {
    std::string name;
    int parent;              // index into the part list, -1 for the root (pelvis)
    vec3 offset;             // pivot position relative to the parent pivot (rest pose, facing +Y)
    MeshAsset mesh;          // modelled around its pivot
};
const std::vector<MannequinPart>& mannequinParts();

struct MannequinPose {
    float phase = 0;         // walk cycle phase (radians)
    float stride = 0;        // 0 idle .. 1 full run
    float airborne = 0;      // 0 grounded .. 1 in the air
    float lean = 0;          // forward lean (radians)
    float time = 0;          // for idle breathing
};
// World transforms (position, rotation) of every part for a body at `root`
// (feet position) facing `yaw` (radians, 0 = +Y).
void poseMannequin(const MannequinPose& pose, vec3 root, float yaw, float scale, std::vector<vec3>& pos, std::vector<quat>& rot);
}  // namespace df
