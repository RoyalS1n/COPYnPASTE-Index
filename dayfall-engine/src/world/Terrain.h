#pragma once
// Editable heightfield terrain. The map stores the BASE heights and painted
// layers; paths are applied non-destructively on top at build time, so moving
// or deleting a path re-carves the ground correctly.
#include "scene/MeshAsset.h"
#include "world/Area.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

namespace df {
class Terrain {
public:
    enum Paint : uint32_t { PaintDirt = 0, PaintRock, PaintSnow, PaintWet, PaintDry, PaintGrass, kPaintChannels = 8 };
    static const char* paintName(uint32_t c);
    static int paintIndex(const std::string& name);   // -1 if unknown

    // grid: n x n samples, square
    uint32_t n = 0;
    float spacing = 1.0f;
    vec2 origin{0};                  // world xy of sample (0, 0)
    uint32_t seed = 1;
    std::vector<float> base;         // editable heights
    std::vector<uint8_t> paint;      // kPaintChannels per sample, 0..255
    // biome (automatic layers)
    float waterLevel = -1000.0f;
    float snowline = 100000.0f;
    float rockSlopeDeg = 40.0f;
    float dryAmount = 0.5f;
    uint32_t chunkQuads = 64;
    std::vector<float> lodDistances{90.0f, 200.0f, 450.0f, 1e9f};

    // derived by applyPaths(): final heights and path masks
    std::vector<float> height, pathMask, laneMask, laneDist;
    uint64_t version = 1;            // bumped on every change

    bool empty() const { return n == 0; }
    float size() const { return (n - 1) * spacing; }
    vec2 maxCorner() const { return origin + vec2(size()); }
    bool inside(vec2 p) const { vec2 q = (p - origin) / spacing; return q.x >= 0 && q.y >= 0 && q.x <= n - 1 && q.y <= n - 1; }
    void create(uint32_t samples, float spacing, vec2 origin, float h = 0.0f);

    // procedural base terrain: {"preset": "flat|hills|valley|mountains|meadow|island", "size_m", "spacing_m", "seed", ...}
    void generate(const nlohmann::json& params);
    // brush edits of the base heights: {"op": "raise|lower|flatten|smooth|noise|set|level_path", "area": ..., "strength", ...}
    nlohmann::json sculpt(const nlohmann::json& params);
    // paint a layer: {"layer": "dirt|rock|snow|wet|dry|grass", "area": ..., "strength": 0..1, "mode": "add|set|erase"}
    nlohmann::json paintLayer(const nlohmann::json& params);
    // final heights = base + paths (the map's "paths" array)
    void applyPaths(const nlohmann::json& paths);

    // queries on the FINAL surface (edges clamp)
    float heightAt(float x, float y) const;
    float baseAt(float x, float y) const;
    vec3 normalAt(float x, float y) const;
    float slopeDegAt(float x, float y) const { return glm::degrees(std::acos(glm::clamp(normalAt(x, y).z, -1.0f, 1.0f))); }
    float pathAt(float x, float y) const;
    float paintAt(uint32_t channel, float x, float y) const;
    // automatic + painted layer weights at a world point: rock, snow, wet, dry, path
    vec4 layersAt(float x, float y) const;

    // render chunks
    uint32_t chunksPerSide() const { return n ? (n - 1 + chunkQuads - 1) / chunkQuads : 0; }
    MeshAsset chunkMesh(uint32_t cx, uint32_t cy) const;

    void load(const std::filesystem::path& heightFile, const std::filesystem::path& paintFile, uint32_t samples, float spacing, vec2 origin);
    void save(const std::filesystem::path& heightFile, const std::filesystem::path& paintFile) const;
    nlohmann::json summary() const;
    // a coarse height grid for planning (rows from south to north)
    nlohmann::json heightGrid(uint32_t cells, Area area) const;

private:
    float sampleArr(const std::vector<float>& a, float x, float y) const;
    float& at(std::vector<float>& a, uint32_t i, uint32_t j) { return a[(size_t)j * n + i]; }
    float at(const std::vector<float>& a, uint32_t i, uint32_t j) const { return a[(size_t)j * n + i]; }
    vec2 pos(uint32_t i, uint32_t j) const { return origin + vec2(i, j) * spacing; }
    void thermalErosion(int passes, float talus, float rate);
    void sampleMasks(uint32_t i, uint32_t j, float slopeDeg, vec4& c0, vec4& c1) const;
};
}  // namespace df
