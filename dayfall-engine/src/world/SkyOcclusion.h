#pragma once
// Sky occlusion: how much of the sky (and of the sky-lit ground below the
// horizon) a point near static structures can see. Baked at scene build into
// a few small 3D volumes around buildings, ruins and cliffs; lighting scales
// the ambient sky light and sky reflections by it (shaders/include/lighting.glsl).
#include "scene/Scene.h"
#include <memory>

namespace df {
class Terrain;

// One volume: an ambient cube per cell, the cosine-weighted open fraction of the
// hemisphere around +x -x +y -y +z -z (0..1; below the horizon, "open" is
// sky-lit ground). A surface facing n sees sum(n_i^2 * lobe(sign n_i, i)).
// Stored as two RGBA8 texels per cell in a volume 2 * dims.x wide (x fastest):
// (+x -x +y -y) at x, (+z -z 0 0) at dims.x + x.
struct SkyRegion {
    vec3 origin{0};          // min corner (world)
    float cell = 1.0f;       // cell size (m)
    uvec3 dims{0};           // cells
    std::vector<uint8_t> rgba;
    uint64_t key = 0, terrainKey = 0;   // occluders + settings, terrain heights it was traced from
};
struct SkyVolume {
    std::vector<std::shared_ptr<const SkyRegion>> regions;
    uint32_t rays = 0;
    size_t cells = 0;
    double ms = 0;           // build time (0 when every region came from the cache)
};

class SkyOcclusionBuilder {
public:
    // Traces the volumes for the scene's static instances and the terrain under them.
    // Regions whose geometry, terrain and settings did not change are reused, so a
    // rebuild after an unrelated edit costs nothing. nullptr: disabled or no structures.
    std::shared_ptr<const SkyVolume> build(const Scene& s, const Terrain& t);

private:
    uint64_t key_ = 0;       // all occluders + settings of last_
    std::shared_ptr<const SkyVolume> last_;
};
}  // namespace df
