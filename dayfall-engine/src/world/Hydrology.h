#pragma once
// Rivers where water would really run. Priority-Flood (Barnes, Lehman & Mulla 2014, "Priority-Flood: An Optimal
// Depression-Filling and Watershed-Labeling Algorithm") fills the pits from the map edge and the sea inward with a
// tiny slope, so every cell drains; each cell then drains to its steepest lower neighbour (D8) on the filled surface,
// rain accumulates downhill, and channels whose catchment passes a threshold are traced from their heads to the sea,
// the edge or the river they join. The longest are kept and become ordinary river bodies (bed carving, flow speed and
// water_set editing all apply).
#include "world/Area.h"
#include "world/Terrain.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace df {
struct TracedRiver {
    std::vector<vec2> points;   // upstream first, simplified and smoothed
    float catchmentM2 = 0;      // at the mouth
    float lengthM = 0;
    float widthM = 0, depthM = 0;
    std::string ends;           // "sea", "edge" or "joins" (the index of the river it joins in joinsIndex)
    int joinsIndex = -1;
};
// params: max_rivers (3), min_catchment_m2 (default: the wettest 0.5% of cells, at least 2000 m2), min_length_m (80),
// width_scale (1). Returns stats (threshold, cells, simulated cell size).
nlohmann::json traceRivers(const Terrain& t, const Area& area, const nlohmann::json& params, std::vector<TracedRiver>& out);
}  // namespace df
