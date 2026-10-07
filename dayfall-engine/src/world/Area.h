#pragma once
// A region of the ground plane with a soft edge, parsed from JSON:
//   {"circle": {"center": [x, y], "radius": r}}
//   {"rect": {"center": [x, y], "size": [w, h], "yaw_deg": a}}
//   {"polygon": [[x, y], ...]}
//   {"line": [[x, y], ...], "width": w}            (a corridor along a polyline)
//   {"union": [area, area, ...]}                    (any of several areas)
//   "all"
// plus an optional "falloff": metres over which the weight fades to 0 outside.
#include "core/Math.h"
#include <nlohmann/json.hpp>
#include <vector>

namespace df {
struct Area {
    enum class Kind { All, Circle, Rect, Polygon, Line, Union } kind = Kind::All;
    std::vector<Area> children;    // Union
    vec2 center{0};
    float radius = 0;
    vec2 size{0};
    float yaw = 0;                 // radians
    std::vector<vec2> points;
    float width = 0;
    float falloff = 0;

    static Area parse(const nlohmann::json& j);   // throws df::Error
    // Named areas (the map's "areas" section): a string other than "all" names one. expandNamed replaces the names in
    // an area (unions included); expandArgs does it for every "area" value in a tool's arguments.
    static nlohmann::json expandNamed(const nlohmann::json& j, const nlohmann::json& named);
    static nlohmann::json expandArgs(const nlohmann::json& args, const nlohmann::json& named);
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
