#pragma once
// Spatial helpers shared by the layout tools (Tools.cpp) and the world tools (WorldTools.cpp): where an object of the
// world document stands and how big it is (before the scene is rebuilt), relative placement and group rotation.
#include "editor/Editor.h"
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace df {
struct ItemBox {
    std::string id, section;           // objects or entities
    vec3 lo{0}, hi{0};                 // world-space box (entities: a 0.5 m box around the point)
    Placement placement;
    float yawDeg = 0;                  // the item's yaw (from yaw_deg or its rotation)
    bool collides = true;
    std::string mesh, category;        // mesh name ("" for primitives / entities), library category
    std::set<std::string> tags;
    vec3 center() const { return (lo + hi) * 0.5f; }
};

// The box of a world item by id, or of an object document not yet in the world. Throws df::Error.
ItemBox itemBox(Editor& E, const std::string& id);
ItemBox objectBox(Editor& E, const nlohmann::json& o);
// Boxes of every visible object (objects whose mesh does not resolve are skipped; the build reports them)
std::vector<ItemBox> allObjectBoxes(Editor& E);

// object_add "place": {"on": id, "at": [x, y]} | {"next_to": id, "side": north|south|east|west, "gap_m", "offset_m"} |
// {"relative_to": id, "offset": [dx, dy(, dz)] in its frame, "match_yaw": true}. Sets o["position"] (and yaw_deg).
void applyRelativePlacement(Editor& E, nlohmann::json& o);
// turns an object or entity about a vertical axis through pivot: moves its position and adds to its yaw
void rotateItemAbout(nlohmann::json& o, vec2 pivot, float degrees);
}  // namespace df
