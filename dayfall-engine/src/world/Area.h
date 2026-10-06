#pragma once
// A region of the ground plane with a soft edge, parsed from JSON:
//   {"circle": {"center": [x, y], "radius": r}}
//   {"rect": {"center": [x, y], "size": [w, h], "yaw_deg": a}}
//   {"polygon": [[x, y], ...]}
//   {"line": [[x, y], ...], "width": w}            (a corridor along a polyline)
//   "all"
// plus an optional "falloff": metres over which the weight fades to 0 outside.
#include "core/Math.h"
#include <nlohmann/json.hpp>
#include <vector>

namespace df {
struct Area {
    enum class Kind { All, Circle, Rect, Polygon, Line } kind = Kind::All;
    vec2 center{0};
    float radius = 0;
    vec2 size{0};
    float yaw = 0;                 // radians
    std::vector<vec2> points;
    float width = 0;
    float falloff = 0;

    static Area parse(const nlohmann::json& j);   // throws df::Error
    nlohmann::json toJson() const;
    // signed distance to the edge (negative inside)
    float distance(vec2 p) const;
    // 1 inside, smooth fade to 0 over `falloff` outside
    float weight(vec2 p) const;
    bool contains(vec2 p) const { return distance(p) <= 0.0f; }
    // world-space bounds including the falloff (All: huge)
    void bounds(vec2& lo, vec2& hi) const;
    float areaM2() const;
};
}  // namespace df
