#pragma once
// The in-game HUD: the map's "hud" settings and the layout of one frame's
// minimap, counters and messages (drawn over the final image by HudRenderer).
#include "game/Game.h"
#include "render/HudRenderer.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace df {
struct HudConfig {
    bool enabled = true;                 // drawn while playing
    float scale = 1.0f;                  // on top of the automatic scaling with the output height
    bool minimap = true;
    std::string corner = "top_right";    // top_right | top_left | bottom_right | bottom_left
    float sizePx = 220.0f;               // minimap diameter at 1080p
    float rangeM = 120.0f;               // metres from the player to the minimap edge
    bool northUp = true;                 // false: the view direction points up
    bool round = true;                   // false: square
    std::string route;                   // a named route drawn as a dotted trail
    bool counters = true, timer = false, messages = true;
    static HudConfig parse(const nlohmann::json& j);   // the map's "hud" section; throws df::Error on bad values
    nlohmann::json toJson() const;
};

struct HudMarker { vec2 pos; vec3 color; uint32_t kind; };   // kind 0 collectible, 1 goal, 2 waypoint

struct HudState {
    vec2 player{0};
    float facing = 0, viewYaw = 0;       // radians, Camera convention (0 = looking along +Y, counter-clockwise)
    float time = 0;                      // seconds of play
    int collected = 0, total = 0;
    std::vector<HudMessage> messages;
    std::vector<HudMarker> markers;      // collectibles not yet taken, goals, waypoints
    std::vector<vec2> route;
    vec2 mapOrigin{0};                   // minimap image: south-west corner and side in metres, north at the top
    float mapSize = 0;                   // 0: no image (markers on a dark disc)
};

void drawHud(HudCanvas& c, const HudConfig& cfg, const HudState& s);
}  // namespace df
