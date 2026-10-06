#pragma once
#include "scene/Scene.h"
#include <vector>

namespace df {
// A uniform world-space grid over the static point lights: per cell an
// [offset, count] pair, then the light indices. Torches, chandeliers and
// lanterns never move, so it is built once per map.
struct LightGrid {
    vec3 origin{0};
    float cell = 6.0f;
    uvec4 dims{0};             // xyz, light count
    std::vector<uint32_t> data;
    std::vector<GpuLight> lights;
};
LightGrid buildLightGrid(const std::vector<LightDef>& lights);
}  // namespace df
