#pragma once
#include "scene/Scene.h"
#include <nlohmann/json.hpp>

namespace df {
// doc["environment"] -> Environment. A "preset" applies first, explicit fields override it.
void parseEnvironment(const nlohmann::json& e, Environment& env);
const nlohmann::json& environmentPresets();
// sun direction (towards the sun) from elevation / azimuth in degrees; azimuth 0 = +Y (north), 90 = +X (east)
vec3 sunDirection(float elevationDeg, float azimuthDeg);
}  // namespace df
