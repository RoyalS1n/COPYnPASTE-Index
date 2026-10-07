#include "game/Hud.h"
#include "core/Error.h"
#include <algorithm>
#include <cmath>
#include <format>

namespace df {
using json = nlohmann::json;

HudConfig HudConfig::parse(const json& j) {
    HudConfig c;
    if (j.is_null()) return c;
    if (!j.is_object()) throw Error("hud must be an object");
    c.enabled = j.value("enabled", c.enabled);
    c.scale = j.value("scale", c.scale);
    c.counters = j.value("counters", c.counters);
    c.timer = j.value("timer", c.timer);
    c.messages = j.value("messages", c.messages);
    const json& m = j.value("minimap", json::object());
    if (m.is_boolean()) c.minimap = m.get<bool>();
    else if (m.is_object()) {
        c.minimap = m.value("enabled", c.minimap);
        c.corner = m.value("corner", c.corner);
        c.sizePx = m.value("size_px", c.sizePx);
        c.rangeM = m.value("range_m", c.rangeM);
        c.northUp = m.value("north_up", c.northUp);
        std::string shape = m.value("shape", std::string("round"));
        if (shape != "round" && shape != "square") throw Error("hud.minimap.shape must be round or square");
        c.round = shape == "round";
        c.route = m.value("route", c.route);
    } else throw Error("hud.minimap must be an object or a boolean");
    if (c.corner != "top_right" && c.corner != "top_left" && c.corner != "bottom_right" && c.corner != "bottom_left")
        throw Error("hud.minimap.corner must be top_right, top_left, bottom_right or bottom_left");
    if (!(c.sizePx >= 64 && c.sizePx <= 1024)) throw Error("hud.minimap.size_px must be 64 to 1024");
    if (!(c.rangeM >= 5 && c.rangeM <= 10000)) throw Error("hud.minimap.range_m must be 5 to 10000");
    if (!(c.scale >= 0.25f && c.scale <= 4.0f)) throw Error("hud.scale must be 0.25 to 4");
    return c;
}

json HudConfig::toJson() const {
    json m = {{"enabled", minimap}, {"corner", corner}, {"size_px", sizePx}, {"range_m", rangeM}, {"north_up", northUp},
              {"shape", round ? "round" : "square"}};
    if (!route.empty()) m["route"] = route;
    return {{"enabled", enabled}, {"scale", scale}, {"minimap", m}, {"counters", counters}, {"timer", timer}, {"messages", messages}};
}

namespace {
uint32_t black(float a) { return hudColor(0, 0, 0, a); }
float fade(const HudMessage& m) { return std::clamp(m.timeLeft / 0.6f, 0.0f, 1.0f); }   // full at once (captures), out over 0.6 s

void drawMinimap(HudCanvas& c, const HudConfig& cfg, const HudState& s, float u, float margin) {
    float W = (float)c.width, H = (float)c.height;
    bool top = cfg.corner.rfind("top", 0) == 0, right = cfg.corner.find("right") != std::string::npos;
    float R = std::min(cfg.sizePx * u, 0.42f * std::min(W, H)) * 0.5f;
    vec2 C(right ? W - margin - R : margin + R, top ? margin + R : H - margin - R);
    float th = cfg.northUp ? 0.0f : s.viewYaw;
    vec2 ax(std::cos(th), std::sin(th)), ay(-std::sin(th), std::cos(th));   // world directions of screen right and up
    float ppm = R / cfg.rangeM;
    auto toScreen = [&](vec2 p) { vec2 d = p - s.player; return C + vec2(glm::dot(d, ax), -glm::dot(d, ay)) * ppm; };
    auto screenDir = [&](vec2 w) { return vec2(glm::dot(w, ax), -glm::dot(w, ay)); };
    // inside test and clamp to the edge (inset px), for round and square maps
    auto edge = [&](vec2 off) { return cfg.round ? glm::length(off) : std::max(std::abs(off.x), std::abs(off.y)); };

    // backdrop and the map image (uv outside 0..1 shades as "beyond the map")
    if (cfg.round) c.disc(C, R + 5 * u, black(0.5f));
    else c.rect(C - vec2(R + 5 * u), C + vec2(R + 5 * u), black(0.5f));
    HudVertex v[4];
    const vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; ++i) {
        vec2 o = corners[i] * R;
        vec2 w = s.player + (ax * o.x - ay * o.y) / ppm;
        vec2 uv = s.mapSize > 0 ? vec2((w.x - s.mapOrigin.x) / s.mapSize, (s.mapOrigin.y + s.mapSize - w.y) / s.mapSize) : vec2(-1);
        v[i] = {C + o, uv, corners[i], hudColor(1, 1, 1, 0.96f), cfg.round ? HudMapRound : HudMap};
    }
    c.quad(v);

    // route: a dotted trail
    if (s.route.size() >= 2) {
        float step = 7.0f * u / ppm;
        int budget = 4000;
        for (size_t i = 0; i + 1 < s.route.size() && budget > 0; ++i) {
            vec2 a = s.route[i], b = s.route[i + 1];
            float len = glm::length(b - a);
            for (float t = 0; t < len && budget > 0; t += step, --budget) {
                vec2 p = toScreen(a + (b - a) * (t / std::max(len, 1e-3f)));
                if (edge(p - C) < R - 3 * u) c.disc(p, 1.8f * u, hudColor(1, 1, 1, 0.75f));
            }
        }
    }
    // markers; off-range ones sit on the edge, smaller
    for (const HudMarker& m : s.markers) {
        vec2 p = toScreen(m.pos), off = p - C;
        float inset = 7 * u, e = edge(off);
        bool far = e > R - inset;
        if (far) p = C + off * ((R - inset) / e);
        float k = far ? 0.7f : 1.0f;
        if (m.kind == 0) {
            c.disc(p, 6.0f * u * k, black(0.85f));
            c.disc(p, 4.4f * u * k, hudColor(m.color));
        } else if (m.kind == 1) {
            c.disc(p, 9.5f * u * k, black(0.6f));
            c.ring(p, 7.5f * u * k, 2.6f * u * k, hudColor(m.color));
            c.disc(p, 2.6f * u * k, hudColor(m.color));
        } else {
            c.disc(p, 3.6f * u * k, black(0.8f));
            c.disc(p, 2.4f * u * k, hudColor(0.95f, 0.95f, 0.95f));
        }
    }
    // view cone (what the camera looks at), then the player arrow
    vec2 vd = screenDir(vec2(-std::sin(s.viewYaw), std::cos(s.viewYaw)));
    float cone = 0.62f, len = 0.5f * R;
    uint32_t c0 = hudColor(1, 1, 1, 0.28f), c1 = hudColor(1, 1, 1, 0.0f);
    for (int i = 0; i < 6; ++i) {
        auto dir = [&](float t) { float a = -cone + 2 * cone * t; return vec2(vd.x * std::cos(a) - vd.y * std::sin(a), vd.x * std::sin(a) + vd.y * std::cos(a)); };
        vec2 a = C + dir(i / 6.0f) * len, b = C + dir((i + 1) / 6.0f) * len;
        c.verts.insert(c.verts.end(), {HudVertex{C, vec2(0), vec2(0), c0, HudSolid}, HudVertex{a, vec2(0), vec2(0), c1, HudSolid},
                                       HudVertex{b, vec2(0), vec2(0), c1, HudSolid}});
    }
    vec2 f = screenDir(vec2(-std::sin(s.facing), std::cos(s.facing))), n(-f.y, f.x);
    for (int pass = 0; pass < 2; ++pass) {
        float sz = (pass ? 1.0f : 1.45f) * u;
        uint32_t col = pass ? hudColor(1.0f, 0.96f, 0.86f) : black(0.9f);
        vec2 tip = C + f * 11.0f * sz, l = C - f * 7.0f * sz + n * 7.5f * sz, r = C - f * 7.0f * sz - n * 7.5f * sz, notch = C - f * 3.0f * sz;
        c.tri(tip, l, notch, col);
        c.tri(tip, notch, r, col);
    }
    // rim and north
    uint32_t rim = hudColor(0.93f, 0.9f, 0.82f, 0.9f);
    if (cfg.round) c.ring(C, R + 2.5f * u, 3.0f * u, rim);
    else {
        float a = R + 2.5f * u, t = 3.0f * u;
        c.rect(C + vec2(-a, -a), C + vec2(a, -a + t), rim);
        c.rect(C + vec2(-a, a - t), C + vec2(a, a), rim);
        c.rect(C + vec2(-a, -a + t), C + vec2(-a + t, a - t), rim);
        c.rect(C + vec2(a - t, -a + t), C + vec2(a, a - t), rim);
    }
    vec2 nd = screenDir(vec2(0, 1));
    vec2 np = C + (cfg.round ? nd * (R + 1.0f * u) : nd / std::max(std::abs(nd.x), std::abs(nd.y)) * (R + 1.0f * u));
    float ts = 2.6f * u;
    c.disc(np, 12.0f * u, hudColor(0.1f, 0.1f, 0.12f, 0.92f));
    c.text(np - vec2(HudCanvas::textWidth("N", ts), HudCanvas::kCapHeight * ts) * 0.5f, "N", ts, hudColor(1, 0.9f, 0.55f), 0);
}
}  // namespace

void drawHud(HudCanvas& c, const HudConfig& cfg, const HudState& s) {
    if (!cfg.enabled || c.width == 0) return;
    const float u = c.ui * cfg.scale, W = (float)c.width, H = (float)c.height;
    const float margin = 24.0f * u;
    if (cfg.minimap) drawMinimap(c, cfg, s, u, margin);

    // counters: collectibles found / total and the play time, in the top corner the minimap leaves free
    bool rows[2] = {cfg.counters && s.total > 0, cfg.timer};
    if (rows[0] || rows[1]) {
        float ts = 4.0f * u, ts2 = 3.0f * u, pad = 12.0f * u, icon = 32.0f * u;
        std::string count = std::format("{} / {}", s.collected, s.total);
        int secs = (int)s.time;
        std::string clock = std::format("{}:{:02}", secs / 60, secs % 60);
        float w = std::max(rows[0] ? icon + HudCanvas::textWidth(count, ts) : 0.0f, rows[1] ? HudCanvas::textWidth(clock, ts2) : 0.0f);
        float h = (rows[0] ? 7.0f * ts : 0.0f) + (rows[1] ? 7.0f * ts2 : 0.0f) + (rows[0] && rows[1] ? 10.0f * u : 0.0f);
        bool left = !(cfg.minimap && cfg.corner == "top_left");
        vec2 p0(left ? margin : W - margin - w - 2 * pad, margin);
        c.rect(p0, p0 + vec2(w + 2 * pad, h + 2 * pad), black(0.38f));
        vec2 at = p0 + vec2(pad);
        if (rows[0]) {
            bool all = s.collected >= s.total;
            vec2 g = at + vec2(10.0f * u, 3.5f * ts);   // gem icon: a diamond with an outline
            for (int pass = 0; pass < 2; ++pass) {
                float k = pass ? 1.0f : 1.3f;
                uint32_t col = pass ? hudColor(1.0f, 0.78f, 0.32f) : black(0.9f);
                vec2 t = g - vec2(0, 12 * u * k), b = g + vec2(0, 12 * u * k), l = g - vec2(8 * u * k, 0), r = g + vec2(8 * u * k, 0);
                c.tri(t, l, b, col);
                c.tri(t, b, r, col);
            }
            c.text(at + vec2(icon, 0), count, ts, all ? hudColor(1.0f, 0.85f, 0.35f) : hudColor(1, 1, 1), black(0.9f));
            at.y += 7.0f * ts + 10.0f * u;
        }
        if (rows[1]) c.text(at, clock, ts2, hudColor(0.92f, 0.92f, 0.92f), black(0.9f));
    }

    if (!cfg.messages) return;
    // toasts at the top centre (newest last), and one large banner for the latest goal
    std::vector<const HudMessage*> toasts;
    const HudMessage* banner = nullptr;
    for (const HudMessage& m : s.messages) {
        if (m.timeLeft <= 0) continue;
        if (m.banner) banner = &m;
        else toasts.push_back(&m);
    }
    if (toasts.size() > 4) toasts.erase(toasts.begin(), toasts.end() - 4);
    float y = margin;
    for (const HudMessage* m : toasts) {
        float a = fade(*m), ts = 3.2f * u;
        float tw = HudCanvas::textWidth(m->text, ts);
        if (tw > W * 0.5f) { ts *= W * 0.5f / tw; tw = W * 0.5f; }
        float bh = 7.0f * ts + 20.0f * u;
        c.rect(vec2(W * 0.5f - tw * 0.5f - 16 * u, y), vec2(W * 0.5f + tw * 0.5f + 16 * u, y + bh), black(0.45f * a));
        c.text(vec2(W * 0.5f - tw * 0.5f, y + 10.0f * u), m->text, ts, hudColor(1, 1, 1, a), black(0.9f * a));
        y += bh + 6.0f * u;
    }
    if (banner) {
        float a = fade(*banner), ts = 7.5f * u;
        float tw = HudCanvas::textWidth(banner->text, ts);
        if (tw > W * 0.86f) { ts *= W * 0.86f / tw; tw = W * 0.86f; }
        float cy = H * 0.3f, bh = 7.0f * ts + 44.0f * u, t = 2.5f * u;
        uint32_t clear = black(0.0f), dark = black(0.5f * a), gold = hudColor(1.0f, 0.82f, 0.4f, 0.9f * a), gold0 = hudColor(1.0f, 0.82f, 0.4f, 0.0f);
        float x0 = W * 0.5f - std::max(tw * 0.5f + 80 * u, W * 0.3f), x1 = W - x0, xm0 = W * 0.5f - tw * 0.35f, xm1 = W - xm0;
        for (float yy : {cy - bh * 0.5f - t, cy + bh * 0.5f}) {   // thin gold lines
            c.rectH(vec2(x0, yy), vec2(xm0, yy + t), gold0, gold);
            c.rect(vec2(xm0, yy), vec2(xm1, yy + t), gold);
            c.rectH(vec2(xm1, yy), vec2(x1, yy + t), gold, gold0);
        }
        c.rectH(vec2(x0, cy - bh * 0.5f), vec2(xm0, cy + bh * 0.5f), clear, dark);
        c.rect(vec2(xm0, cy - bh * 0.5f), vec2(xm1, cy + bh * 0.5f), dark);
        c.rectH(vec2(xm1, cy - bh * 0.5f), vec2(x1, cy + bh * 0.5f), dark, clear);
        c.text(vec2(W * 0.5f - tw * 0.5f, cy - 3.5f * ts), banner->text, ts, hudColor(1.0f, 0.94f, 0.74f, a), black(0.95f * a));
    }
}
}  // namespace df
