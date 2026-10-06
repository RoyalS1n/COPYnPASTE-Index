#pragma once
// The editable world: a JSON document (the map's map.json) plus the terrain
// heightfield. Every edit goes through beginEdit()/endEdit() so it can be
// undone; the scene builder turns the world into a renderable Scene.
#include "world/Terrain.h"
#include <deque>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace df {
class World {
public:
    std::filesystem::path dir;
    nlohmann::json doc;
    Terrain terrain;
    uint64_t version = 1;          // bumps on every change
    bool dirty = false;            // unsaved changes

    void load(const std::filesystem::path& mapDir);
    void save();
    // a new map with flat ground, default lighting and a player start
    void createNew(const std::filesystem::path& mapDir, const std::string& name);

    // undo / redo -----------------------------------------------------------
    void beginEdit(const std::string& label, bool touchesTerrain);
    void endEdit();                // records the edit (no-op if nothing changed)
    void cancelEdit();             // restores the state from beginEdit()
    bool undo(std::string* label = nullptr);
    bool redo(std::string* label = nullptr);
    nlohmann::json history(size_t max = 20) const;

    // document helpers ------------------------------------------------------
    static const std::vector<std::string>& idSections();   // objects, scatter, paths, entities, lights
    nlohmann::json& list(const std::string& section);      // doc[section] as an array (created if missing)
    nlohmann::json* find(const std::string& id, std::string* section = nullptr);
    bool erase(const std::string& id);
    std::string newId(const std::string& prefix) const;
    void touch() { ++version; dirty = true; }
    // terrain settings live in doc["terrain"]; keep them in sync with `terrain`
    void terrainToDoc();
    void docToTerrain();

private:
    struct TerrainPatch {           // a rectangle of base heights + paint, before and after
        uint32_t i0 = 0, j0 = 0, w = 0, h = 0, n = 0;
        float spacing = 1;
        vec2 origin{0};
        bool resized = false;       // the whole grid changed (generate)
        std::vector<float> baseBefore, baseAfter;
        std::vector<uint8_t> paintBefore, paintAfter;
        uint32_t nBefore = 0;
        float spacingBefore = 1;
        vec2 originBefore{0};
    };
    struct Entry {
        std::string label;
        nlohmann::json docBefore, docAfter;
        std::shared_ptr<TerrainPatch> terrain;
    };
    void applyPatch(const TerrainPatch& p, bool after);
    std::deque<Entry> undo_, redo_;
    bool editing_ = false, editTerrain_ = false;
    std::string editLabel_;
    nlohmann::json editDoc_;
    std::vector<float> editBase_;
    std::vector<uint8_t> editPaint_;
    uint32_t editN_ = 0;
    float editSpacing_ = 1;
    vec2 editOrigin_{0};
};
}  // namespace df
