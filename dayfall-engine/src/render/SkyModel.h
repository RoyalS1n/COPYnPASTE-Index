#pragma once
#include "scene/Scene.h"
#include <vector>

namespace df {
// Physically based single-scattering sky (Rayleigh, Mie, ozone) with an
// approximate multiple-scattering term, evaluated on the CPU once per map.
struct SkyData {
    uint32_t width = 256, height = 128;
    std::vector<uint16_t> lut;     // RGBA16F, azimuth relative to the sun x elevation
    vec4 sh[9];                    // irradiance SH (radiance coefficients), rgb
};
SkyData computeSky(const Environment& env);
}  // namespace df
