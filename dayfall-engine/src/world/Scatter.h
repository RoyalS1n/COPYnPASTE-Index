#pragma once
// Procedural placement rules ("scatter" in the map): forests, grass, rocks.
// Deterministic for a given rule + terrain, so the map stores rules, not
// millions of instances.
#include "world/Area.h"
#include "world/Terrain.h"
#include <nlohmann/json.hpp>
#include <vector>

namespace df {
struct ScatterPoint { uint32_t variant; vec3 position; quat rotation; float scale; };
struct Footprint { vec2 center; float radius; };   // keep-out circle (objects, entities)

struct ScatterContext {
    const Terrain* terrain = nullptr;
    float waterLevel = -1000.0f;
    std::vector<Footprint> footprints;
};

// Validates a rule and fills defaults; throws df::Error with a useful message.
nlohmann::json normalizeScatterRule(const nlohmann::json& rule);
// Variants: rule["meshes"] = [{"mesh": ..., "weight": w}] (normalised from "mesh").
std::vector<ScatterPoint> evaluateScatter(const nlohmann::json& rule, const ScatterContext& ctx, size_t maxPoints = 2000000);
}  // namespace df
