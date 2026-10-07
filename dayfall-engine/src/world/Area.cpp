#include "world/Area.h"
#include "core/Error.h"
#include "world/Noise.h"
#include <cmath>
#include <format>

namespace df {
using json = nlohmann::json;
namespace {
vec2 p2(const json& j, const char* what) {
    if (!j.is_array() || j.size() < 2 || !j[0].is_number() || !j[1].is_number())
        throw Error(std::string("area: ") + what + " must be [x, y]");
    return {j[0].get<float>(), j[1].get<float>()};
}
std::vector<vec2> pts(const json& j, size_t minCount, const char* what) {
    if (!j.is_array() || j.size() < minCount) throw Error(std::format("area: {} needs at least {} [x, y] points", what, minCount));
    std::vector<vec2> out;
    for (auto& p : j) out.push_back(p2(p, what));
    return out;
}
float segDist(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = glm::dot(ab, ab) > 0 ? glm::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}
}  // namespace

Area Area::parse(const json& j) {
    Area a;
    if (j.is_null() || (j.is_string() && j.get<std::string>() == "all")) return a;
    if (j.is_string()) throw Error("area '" + j.get<std::string>() + "' is not a named area of this map (area_set defines one)");
    if (!j.is_object()) throw Error("area must be \"all\", a named area or an object with circle / rect / polygon / line");
    a.falloff = j.value("falloff", 0.0f);
    if (a.falloff < 0) throw Error("area: falloff must be >= 0");
    if (j.contains("circle")) {
        const json& c = j["circle"];
        a.kind = Kind::Circle;
        a.center = p2(c.value("center", json()), "circle.center");
        a.radius = c.value("radius", 0.0f);
        if (a.radius <= 0) throw Error("area: circle.radius must be > 0");
    } else if (j.contains("rect")) {
        const json& r = j["rect"];
        a.kind = Kind::Rect;
        a.center = p2(r.value("center", json()), "rect.center");
        a.size = p2(r.value("size", json()), "rect.size");
        a.yaw = glm::radians(r.value("yaw_deg", 0.0f));
        if (a.size.x <= 0 || a.size.y <= 0) throw Error("area: rect.size must be positive");
    } else if (j.contains("polygon")) {
        a.kind = Kind::Polygon;
        a.points = pts(j["polygon"], 3, "polygon");
    } else if (j.contains("line")) {
        a.kind = Kind::Line;
        a.points = pts(j["line"], 2, "line");
        a.width = j.value("width", 4.0f);
        if (a.width <= 0) throw Error("area: line width must be > 0");
    } else if (j.contains("union")) {
        a.kind = Kind::Union;
        if (!j["union"].is_array() || j["union"].empty()) throw Error("area: union needs a non-empty array of areas");
        for (auto& c : j["union"]) {
            Area child = parse(c);
            if (child.kind == Kind::All) throw Error("area: a union cannot contain \"all\"");
            a.children.push_back(child);
        }
    } else if (j.value("all", false)) {
        return a;
    } else {
        throw Error("area needs one of: circle, rect, polygon, line (or \"all\")");
    }
    return a;
}

namespace {
json expandNamedAt(const json& j, const json& named, int depth) {
    if (depth > 8) throw Error("named areas refer to each other in a loop");
    if (j.is_string() && j.get<std::string>() != "all") {
        std::string n = j.get<std::string>();
        if (!named.is_object() || !named.contains(n)) {
            std::string have;
            if (named.is_object())
                for (auto& [k, v] : named.items()) have += (have.empty() ? "" : ", ") + k;
            throw Error("no named area '" + n + "'" + (have.empty() ? " (the map has none; area_set defines one)" : " (named areas: " + have + ")"));
        }
        return expandNamedAt(named[n], named, depth + 1);
    }
    if (j.is_object() && j.contains("union") && j["union"].is_array()) {
        json out = j;
        for (auto& c : out["union"]) c = expandNamedAt(c, named, depth + 1);
        return out;
    }
    return j;
}
json expandArgsAt(const json& j, const json& named) {
    if (j.is_array()) {
        json out = json::array();
        for (auto& v : j) out.push_back(expandArgsAt(v, named));
        return out;
    }
    if (!j.is_object()) return j;
    json out = json::object();
    for (auto& [k, v] : j.items()) out[k] = k == "area" ? expandNamedAt(v, named, 0) : expandArgsAt(v, named);
    return out;
}
}  // namespace

json Area::expandNamed(const json& j, const json& named) { return expandNamedAt(j, named, 0); }
json Area::expandArgs(const json& args, const json& named) { return expandArgsAt(args, named); }

json Area::toJson() const {
    json j;
    switch (kind) {
        case Kind::All: return "all";
        case Kind::Circle: j["circle"] = {{"center", {center.x, center.y}}, {"radius", radius}}; break;
        case Kind::Rect: j["rect"] = {{"center", {center.x, center.y}}, {"size", {size.x, size.y}}, {"yaw_deg", glm::degrees(yaw)}}; break;
        case Kind::Polygon: { json p = json::array(); for (auto& v : points) p.push_back({v.x, v.y}); j["polygon"] = p; break; }
        case Kind::Line: { json p = json::array(); for (auto& v : points) p.push_back({v.x, v.y}); j["line"] = p; j["width"] = width; break; }
        case Kind::Union: { json u = json::array(); for (auto& c : children) u.push_back(c.toJson()); j["union"] = u; break; }
    }
    if (falloff > 0) j["falloff"] = falloff;
    return j;
}

float Area::distance(vec2 p) const {
    switch (kind) {
        case Kind::All: return -1e9f;
        case Kind::Circle: return glm::length(p - center) - radius;
        case Kind::Rect: {
            vec2 d = p - center;
            float c = std::cos(-yaw), s = std::sin(-yaw);
            vec2 l(d.x * c - d.y * s, d.x * s + d.y * c);
            vec2 q = glm::abs(l) - size * 0.5f;
            return glm::length(glm::max(q, vec2(0))) + std::min(std::max(q.x, q.y), 0.0f);
        }
        case Kind::Polygon: {
            float dmin = 1e30f;
            bool inside = false;
            for (size_t i = 0, k = points.size() - 1; i < points.size(); k = i++) {
                vec2 a = points[k], b = points[i];
                dmin = std::min(dmin, segDist(p, a, b));
                if (((b.y > p.y) != (a.y > p.y)) && (p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x)) inside = !inside;
            }
            return inside ? -dmin : dmin;
        }
        case Kind::Line: {
            float dmin = 1e30f;
            for (size_t i = 0; i + 1 < points.size(); ++i) dmin = std::min(dmin, segDist(p, points[i], points[i + 1]));
            return dmin - width * 0.5f;
        }
        case Kind::Union: {
            float d = 1e30f;
            for (auto& c : children) d = std::min(d, c.distance(p));
            return d;
        }
    }
    return 1e9f;
}

float Area::weight(vec2 p) const {
    if (kind == Kind::All) return 1.0f;
    if (kind == Kind::Union) {
        float w = 0;
        for (auto& c : children) w = std::max(w, c.weight(p));
        if (falloff > 0) w = std::max(w, 1.0f - smoothstep(0.0f, falloff, distance(p)));
        return w;
    }
    float d = distance(p);
    if (d <= 0) return 1.0f;
    if (falloff <= 0) return 0.0f;
    return 1.0f - smoothstep(0.0f, falloff, d);
}

void Area::bounds(vec2& lo, vec2& hi) const {
    switch (kind) {
        case Kind::All: lo = vec2(-1e9f); hi = vec2(1e9f); return;
        case Kind::Circle: lo = center - radius; hi = center + radius; break;
        case Kind::Rect: {   // the box around the turned rectangle
            float c = std::abs(std::cos(yaw)), sn = std::abs(std::sin(yaw));
            vec2 half(0.5f * (size.x * c + size.y * sn), 0.5f * (size.x * sn + size.y * c));
            lo = center - half;
            hi = center + half;
            break;
        }
        case Kind::Polygon:
        case Kind::Line:
            lo = vec2(1e30f); hi = vec2(-1e30f);
            for (auto& v : points) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
            if (kind == Kind::Line) { lo -= width * 0.5f; hi += width * 0.5f; }
            break;
        case Kind::Union:
            lo = vec2(1e30f); hi = vec2(-1e30f);
            for (auto& c : children) { vec2 a, b; c.bounds(a, b); lo = glm::min(lo, a); hi = glm::max(hi, b); }
            break;
    }
    lo -= falloff;
    hi += falloff;
}

float Area::areaM2() const {
    switch (kind) {
        case Kind::All: return 1e12f;
        case Kind::Circle: return 3.14159265f * radius * radius;
        case Kind::Rect: return size.x * size.y;
        case Kind::Polygon: {
            float a = 0;
            for (size_t i = 0, k = points.size() - 1; i < points.size(); k = i++) a += points[k].x * points[i].y - points[i].x * points[k].y;
            return std::abs(a) * 0.5f;
        }
        case Kind::Line: {
            float len = 0;
            for (size_t i = 0; i + 1 < points.size(); ++i) len += glm::length(points[i + 1] - points[i]);
            return len * width;
        }
        case Kind::Union: {
            float a = 0;
            for (auto& c : children) a += c.areaM2();   // overlaps counted twice: an upper bound
            return a;
        }
    }
    return 0;
}
}  // namespace df
