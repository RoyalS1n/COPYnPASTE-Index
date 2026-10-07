#pragma once
// Procedural placement rules ("scatter" in the map): forests, grass, rocks.
// Deterministic for a given rule + terrain, so the map stores rules, not
// millions of instances.
#include "world/Area.h"
#include "world/Terrain.h"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>

namespace df {
struct ScatterPoint { uint32_t variant; vec3 position; quat rotation; float scale; };
struct Footprint { vec2 center; float radius; };   // keep-out circle (objects, entities)

// Keep-out circles bucketed on a grid: fast tests for millions of scatter points.
class FootprintIndex {
public:
    explicit FootprintIndex(const std::vector<Footprint>& fps, float cell = 24.0f);
    bool blocked(vec2 p, float margin) const;
    bool empty() const { return fps_.empty(); }
private:
    std::vector<Footprint> fps_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid_;
    float cell_;
};

struct ScatterContext {
    const Terrain* terrain = nullptr;
    float waterLevel = -1000.0f;
    std::vector<Footprint> footprints;
};

// Validates a rule and fills defaults; throws df::Error with a useful message.
nlohmann::json normalizeScatterRule(const nlohmann::json& rule);
// Variants: rule["meshes"] = [{"mesh": ..., "weight": w}] (normalised from "mesh").
// Footprints are tested after min spacing, so evaluating without them and filtering the points afterwards with
// FootprintIndex::blocked(p, avoid_objects_m) gives the same result (the scene builder caches it that way).
std::vector<ScatterPoint> evaluateScatter(const nlohmann::json& rule, const ScatterContext& ctx, size_t maxPoints = 2000000);
}  // namespace df
