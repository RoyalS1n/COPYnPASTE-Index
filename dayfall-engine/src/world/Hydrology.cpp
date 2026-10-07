#include "world/Hydrology.h"
#include "core/Error.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <queue>

namespace df {
using json = nlohmann::json;
namespace {
float num(const json& j, const char* k, float def) {
    if (!j.contains(k)) return def;
    if (!j[k].is_number()) throw Error(std::format("rivers {} must be a number", k));
    return j[k].get<float>();
}

// Douglas-Peucker: drop points closer than tol to the line between the kept ones
void simplify(const std::vector<vec2>& in, float tol, std::vector<vec2>& out) {
    if (in.size() < 3) { out = in; return; }
    std::vector<char> keep(in.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, in.size() - 1}};
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        vec2 ab = in[b] - in[a];
        float len2 = std::max(glm::dot(ab, ab), 1e-9f);
        float best = -1;
        size_t bi = a;
        for (size_t i = a + 1; i < b; ++i) {
            float t = std::clamp(glm::dot(in[i] - in[a], ab) / len2, 0.0f, 1.0f);
            float d = glm::length(in[i] - (in[a] + ab * t));
            if (d > best) { best = d; bi = i; }
        }
        if (best > tol) {
            keep[bi] = 1;
            stack.push_back({a, bi});
            stack.push_back({bi, b});
        }
    }
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) if (keep[i]) out.push_back(in[i]);
}

// Chaikin corner cutting, ends kept
std::vector<vec2> chaikin(const std::vector<vec2>& p) {
    if (p.size() < 3) return p;
    std::vector<vec2> o{p.front()};
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        vec2 a = p[i], b = p[i + 1];
        if (i > 0) o.push_back(a * 0.75f + b * 0.25f);
        if (i + 2 < p.size()) o.push_back(a * 0.25f + b * 0.75f);
    }
    o.push_back(p.back());
    return o;
}
}  // namespace

json traceRivers(const Terrain& t, const Area& area, const json& p, std::vector<TracedRiver>& out) {
    if (t.empty()) throw Error("the map has no terrain");
    int maxRivers = std::clamp((int)num(p, "max_rivers", 3.0f), 1, 32);
    float minLength = std::max(num(p, "min_length_m", 80.0f), 0.0f);
    float widthScale = std::clamp(num(p, "width_scale", 1.0f), 0.1f, 10.0f);
    vec2 lo, hi;
    area.bounds(lo, hi);
    lo = glm::max(lo, t.origin);
    hi = glm::min(hi, t.maxCorner());
    if (hi.x - lo.x < 8 * t.spacing || hi.y - lo.y < 8 * t.spacing) throw Error("the area does not cover enough terrain");
    float cell = t.spacing;
    while (((hi.x - lo.x) / cell) * ((hi.y - lo.y) / cell) > 1.0e6f) cell *= 2.0f;
    int W = (int)std::floor((hi.x - lo.x) / cell) + 1, H = (int)std::floor((hi.y - lo.y) / cell) + 1;
    size_t N = (size_t)W * H;
    auto world = [&](int x, int y) { return lo + vec2(x, y) * cell; };
    std::vector<float> h(N);
    std::vector<char> inside(N);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            vec2 w = world(x, y);
            h[(size_t)y * W + x] = t.heightAt(w.x, w.y);
            inside[(size_t)y * W + x] = area.weight(w) > 0.0f;
        }
    // Priority-Flood + epsilon from the outlets: the region's edge, the sea and anything outside the area
    const float eps = 1e-4f;
    std::vector<float> F = h;
    std::vector<char> done(N, 0), outlet(N, 0);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = (size_t)y * W + x;
            if (x == 0 || y == 0 || x == W - 1 || y == H - 1 || h[i] < t.waterLevel || !inside[i]) {
                outlet[i] = done[i] = 1;
                pq.push({F[i], (uint32_t)i});
            }
        }
    const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const float dd[8] = {1, 1, 1, 1, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f};
    while (!pq.empty()) {
        auto [f, i] = pq.top();
        pq.pop();
        int x = (int)(i % W), y = (int)(i / W);
        for (int k = 0; k < 8; ++k) {
            int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            size_t j = (size_t)ny * W + nx;
            if (done[j]) continue;
            done[j] = 1;
            F[j] = std::max(h[j], F[i] + eps);
            pq.push({F[j], (uint32_t)j});
        }
    }
    // D8 on the filled surface: every cell but an outlet has a lower neighbour
    std::vector<int> recv(N, -1);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = (size_t)y * W + x;
            if (outlet[i]) continue;
            float best = 0;
            for (int k = 0; k < 8; ++k) {
                int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                size_t j = (size_t)ny * W + nx;
                float s = (F[i] - F[j]) / dd[k];
                if (s > best) { best = s; recv[i] = (int)j; }
            }
        }
    // rain on every cell, passed downhill from the highest cell to the lowest
    std::vector<uint32_t> order(N);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return F[a] > F[b]; });
    std::vector<float> A(N, 1.0f);
    for (uint32_t i : order)
        if (recv[i] >= 0) A[recv[i]] += A[i];
    float cellArea = cell * cell;
    // the channel threshold: given, or the wettest half percent of the land
    float threshold;
    if (p.contains("min_catchment_m2")) {
        threshold = num(p, "min_catchment_m2", 2000.0f);
        if (!(threshold > 0)) throw Error("min_catchment_m2 must be above 0");
    } else {
        std::vector<float> land;
        for (size_t i = 0; i < N; ++i) if (!outlet[i]) land.push_back(A[i]);
        if (land.empty()) throw Error("no land in the area");
        size_t q = (size_t)(land.size() * 0.995);
        std::nth_element(land.begin(), land.begin() + std::min(q, land.size() - 1), land.end());
        threshold = std::max(2000.0f, land[std::min(q, land.size() - 1)] * cellArea);
    }
    std::vector<char> channel(N, 0);
    std::vector<int> upChannels(N, 0);
    for (size_t i = 0; i < N; ++i) channel[i] = !outlet[i] && A[i] * cellArea >= threshold;
    for (size_t i = 0; i < N; ++i)
        if (channel[i] && recv[i] >= 0 && channel[recv[i]]) upChannels[recv[i]]++;
    std::vector<uint32_t> heads;
    for (size_t i = 0; i < N; ++i) if (channel[i] && upChannels[i] == 0) heads.push_back((uint32_t)i);
    // the longest channels first: main stems claim their cells, tributaries stop where they meet them
    auto pathLength = [&](uint32_t i) {
        float L = 0;
        for (int c = (int)i, guard = 0; recv[c] >= 0 && guard < (int)N; c = recv[c], ++guard) L += cell;
        return L;
    };
    std::vector<std::pair<float, uint32_t>> ranked;
    for (uint32_t hd : heads) ranked.push_back({pathLength(hd), hd});
    std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<int> owner(N, -1);
    out.clear();
    for (auto& [len, hd] : ranked) {
        if ((int)out.size() >= maxRivers) break;
        std::vector<uint32_t> cells;
        int c = (int)hd;
        int joins = -1;
        for (int guard = 0; guard < (int)N; ++guard) {
            cells.push_back((uint32_t)c);
            if (recv[c] < 0) break;
            c = recv[c];
            if (owner[c] >= 0) { joins = owner[c]; cells.push_back((uint32_t)c); break; }
        }
        float L = (cells.size() - 1) * cell;
        if (L < minLength) continue;
        TracedRiver r;
        r.joinsIndex = joins;
        uint32_t mouth = cells.back();
        r.ends = joins >= 0 ? "joins" : (h[mouth] < t.waterLevel ? "sea" : "edge");
        r.catchmentM2 = A[joins >= 0 ? cells[cells.size() - 2] : mouth] * cellArea;
        std::vector<vec2> pts;
        for (uint32_t ci : cells) pts.push_back(world((int)(ci % W), (int)(ci / W)));
        for (size_t k = 0; k + (joins >= 0 ? 1 : 0) < cells.size(); ++k) owner[cells[k]] = (int)out.size();
        // a clean centre line: simplify the staircase of cells and round its corners (at most about 60 points)
        float tol = 1.2f * cell;
        std::vector<vec2> s;
        do { simplify(pts, tol, s); tol *= 1.5f; } while (s.size() > 30);
        r.points = chaikin(s);
        for (size_t k = 1; k < r.points.size(); ++k) r.lengthM += glm::length(r.points[k] - r.points[k - 1]);
        // hydraulic geometry: width grows with the square root of the discharge (the catchment), depth more slowly
        r.widthM = std::clamp(widthScale * 0.5f * std::sqrt(r.catchmentM2 / 1000.0f), 2.0f, 40.0f);
        r.depthM = std::clamp(0.35f * std::pow(r.widthM, 0.7f), 0.5f, 4.0f);
        out.push_back(std::move(r));
    }
    return {{"threshold_m2", std::round(threshold)}, {"channel_heads", heads.size()}, {"cell_m", cell}};
}
}  // namespace df
