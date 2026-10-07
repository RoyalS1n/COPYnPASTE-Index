#include "world/World.h"
#include "core/Error.h"
#include "core/FileSystem.h"
#include "core/JsonFormat.h"
#include "core/Log.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>

namespace df {
using json = nlohmann::json;
namespace fs = std::filesystem;

const std::vector<std::string>& World::idSections() {
    static const std::vector<std::string> s = {"objects", "scatter", "paths", "entities", "lights"};
    return s;
}

void World::docToTerrain() {
    if (!doc.contains("terrain") || !doc["terrain"].is_object()) return;
    const json& t = doc["terrain"];
    terrain.seed = t.value("seed", terrain.seed);
    terrain.snowline = t.value("snowline_m", terrain.snowline);
    terrain.rockSlopeDeg = t.value("rock_slope_deg", terrain.rockSlopeDeg);
    terrain.dryAmount = t.value("dry_amount", terrain.dryAmount);
    const json& env = doc.value("environment", json::object());
    const json& water = env.value("water", json::object());
    terrain.waterLevel = water.value("enabled", false) ? water.value("level_m", 0.0f) : -1000.0f;
}

void World::terrainToDoc() {
    if (terrain.empty()) { doc.erase("terrain"); return; }
    json& t = doc["terrain"];
    if (!t.is_object()) t = json::object();
    t["samples_per_side"] = terrain.n;
    t["spacing_m"] = terrain.spacing;
    t["origin"] = {terrain.origin.x, terrain.origin.y};
    t["seed"] = terrain.seed;
    t["snowline_m"] = terrain.snowline;
    t["rock_slope_deg"] = terrain.rockSlopeDeg;
    t["dry_amount"] = terrain.dryAmount;
    t.erase("chunk_quads");
    t.erase("height_flip_x");
    t.erase("height_flip_y");
    // new maps and old raw files both end up as PNGs: compact in git and viewable
    if (!t.contains("height_file") || t["height_file"] == "terrain/height.f32") t["height_file"] = "terrain/height.png";
    if (!t.contains("paint_files")) t["paint_files"] = {"terrain/paint_a.png", "terrain/paint_b.png"};
    t.erase("paint_file");
    if (!t.contains("material")) t["material"] = "terrain";
}

void World::load(const fs::path& mapDir) {
    fs::path file = mapDir / "map.json";
    if (!fs::exists(file)) throw Error("no map.json in " + mapDir.string());
    json j;
    try {
        j = json::parse(readText(file));
    } catch (const json::exception& e) {
        throw Error(std::format("{}: invalid JSON: {}", file.string(), e.what()));
    }
    if (j.value("format", "") != "dayfall-map") throw Error(file.string() + ": missing \"format\": \"dayfall-map\"");
    dir = mapDir;
    doc = std::move(j);
    terrain = Terrain();
    if (doc.contains("terrain") && doc["terrain"].is_object()) {
        const json& t = doc["terrain"];
        uint32_t n = t.value("samples_per_side", 0u);
        float sp = t.value("spacing_m", 1.0f);
        vec2 org(t["origin"][0].get<float>(), t["origin"][1].get<float>());
        docToTerrain();
        std::vector<fs::path> paints;
        if (t.contains("paint_files")) for (auto& f : t["paint_files"]) paints.push_back(dir / f.get<std::string>());
        else paints.push_back(dir / t.value("paint_file", "terrain/paint.u8"));
        vec2 range(0, 1);
        if (t.contains("height_range_m")) range = vec2(t["height_range_m"][0].get<float>(), t["height_range_m"][1].get<float>());
        terrain.load(dir / t.value("height_file", "terrain/height.f32"), paints, n, sp, org, range,
                     t.value("png_rows", "south_first") == "north_first");
        // maps exported by other tools may store mirrored heightmaps; normalised on the next save
        terrain.flip(t.value("height_flip_x", false), t.value("height_flip_y", false));
        docToTerrain();
    }
    undo_.clear();
    redo_.clear();
    dirty = false;
    savedDoc = doc;
    savedTerrainVersion = terrain.version;
    version = Terrain::nextVersion();
    logInfo("loaded map '{}' from {}", doc.value("name", "?"), dir.string());
}

void World::save() {
    if (dir.empty()) throw Error("the world has no directory to save to");
    fs::create_directories(dir);
    terrainToDoc();
    if (!terrain.empty()) {
        json& t = doc["terrain"];
        std::vector<fs::path> paints;
        for (auto& f : t["paint_files"]) paints.push_back(dir / f.get<std::string>());
        vec2 range = terrain.save(dir / t["height_file"].get<std::string>(), paints);
        if (fs::path(t["height_file"].get<std::string>()).extension() == ".png") {
            t["height_range_m"] = {range.x, range.y};
            t["png_rows"] = "north_first";
        }
    }
    fs::path tmp = dir / "map.json.tmp";
    if (!writeText(tmp, dumpReadable(doc) + "\n")) throw Error("cannot write " + tmp.string());
    fs::rename(tmp, dir / "map.json");
    dirty = false;
    savedDoc = doc;
    savedTerrainVersion = terrain.version;
    logInfo("saved map to {}", dir.string());
}

void World::createNew(const fs::path& mapDir, const std::string& name) {
    dir = mapDir;
    doc = json::object();
    doc["format"] = "dayfall-map";
    doc["version"] = 2;
    doc["name"] = name;
    doc["environment"] = {{"preset", "golden_hour"}};
    doc["player_start"] = {{"position", {0, 0}}, {"yaw_deg", 90}};
    doc["player"] = json::object();
    for (auto& s : idSections()) doc[s] = json::array();
    doc["cameras"] = json::object();
    doc["routes"] = json::object();
    terrain = Terrain();
    terrain.generate({{"preset", "flat"}, {"size_m", 256}, {"spacing_m", 1.0}});
    terrainToDoc();
    docToTerrain();
    undo_.clear();
    redo_.clear();
    dirty = true;
    savedDoc = json::object();   // nothing saved yet: world_diff since save lists everything
    savedTerrainVersion = 0;
    version = Terrain::nextVersion();
}

// --------------------------------------------------------------------------- undo
void World::beginEdit(const std::string& label, bool touchesTerrain) {
    if (editing_) cancelEdit();
    editing_ = true;
    editTerrain_ = touchesTerrain;
    editLabel_ = label;
    editDoc_ = doc;
    if (touchesTerrain) {
        editBase_ = terrain.base;
        editPaint_ = terrain.paint;
        editN_ = terrain.n;
        editSpacing_ = terrain.spacing;
        editOrigin_ = terrain.origin;
    }
}

void World::cancelEdit() {
    if (!editing_) return;
    doc = editDoc_;
    if (editTerrain_) {
        if (terrain.n != editN_) terrain.create(std::max(editN_, 3u), editSpacing_, editOrigin_);
        terrain.base = editBase_;
        terrain.paint = editPaint_;
        terrain.spacing = editSpacing_;
        terrain.origin = editOrigin_;
        if (editN_ == 0) terrain = Terrain();
        docToTerrain();
        terrain.touch();
    }
    editing_ = false;
    version = Terrain::nextVersion();
}

void World::endEdit() {
    if (!editing_) return;
    editing_ = false;
    Entry e;
    e.label = editLabel_;
    bool docChanged = doc != editDoc_;
    if (editTerrain_) {
        auto p = std::make_shared<TerrainPatch>();
        if (terrain.n != editN_ || terrain.spacing != editSpacing_ || terrain.origin != editOrigin_) {
            p->resized = true;
            p->nBefore = editN_;
            p->spacingBefore = editSpacing_;
            p->originBefore = editOrigin_;
            p->n = terrain.n;
            p->spacing = terrain.spacing;
            p->origin = terrain.origin;
            p->baseBefore = std::move(editBase_);
            p->paintBefore = std::move(editPaint_);
            p->baseAfter = terrain.base;
            p->paintAfter = terrain.paint;
            e.terrain = p;
        } else {
            // the changed rectangle only
            uint32_t n = terrain.n, i0 = n, i1 = 0, j0 = n, j1 = 0;
            const uint32_t C = Terrain::kPaintChannels;
            for (uint32_t j = 0; j < n; ++j)
                for (uint32_t i = 0; i < n; ++i) {
                    size_t k = (size_t)j * n + i;
                    bool ch = terrain.base[k] != editBase_[k] || std::memcmp(&terrain.paint[k * C], &editPaint_[k * C], C) != 0;
                    if (ch) { i0 = std::min(i0, i); i1 = std::max(i1, i); j0 = std::min(j0, j); j1 = std::max(j1, j); }
                }
            if (i0 <= i1) {
                p->n = n;
                p->i0 = i0; p->j0 = j0; p->w = i1 - i0 + 1; p->h = j1 - j0 + 1;
                for (uint32_t j = j0; j <= j1; ++j) {
                    size_t k = (size_t)j * n + i0;
                    p->baseBefore.insert(p->baseBefore.end(), editBase_.begin() + k, editBase_.begin() + k + p->w);
                    p->baseAfter.insert(p->baseAfter.end(), terrain.base.begin() + k, terrain.base.begin() + k + p->w);
                    p->paintBefore.insert(p->paintBefore.end(), editPaint_.begin() + k * C, editPaint_.begin() + (k + p->w) * C);
                    p->paintAfter.insert(p->paintAfter.end(), terrain.paint.begin() + k * C, terrain.paint.begin() + (k + p->w) * C);
                }
                e.terrain = p;
            }
        }
        editBase_.clear();
        editPaint_.clear();
    }
    if (!docChanged && !e.terrain) return;
    if (e.terrain) terrainToDoc();
    e.docBefore = std::move(editDoc_);
    e.docAfter = doc;
    undo_.push_back(std::move(e));
    redo_.clear();
    // keep memory bounded: at most 200 steps and ~1 GB of terrain patches
    size_t bytes = 0;
    for (auto& u : undo_) if (u.terrain) bytes += (u.terrain->baseBefore.size() * 2) * 4 + u.terrain->paintBefore.size() * 2;
    while (undo_.size() > 200 || (bytes > (1ull << 30) && undo_.size() > 1)) {
        if (undo_.front().terrain) bytes -= (undo_.front().terrain->baseBefore.size() * 2) * 4 + undo_.front().terrain->paintBefore.size() * 2;
        undo_.pop_front();
    }
    touch();
}

void World::applyPatch(const TerrainPatch& p, bool after) {
    const uint32_t C = Terrain::kPaintChannels;
    if (p.resized) {
        uint32_t n = after ? p.n : p.nBefore;
        if (n == 0) { terrain = Terrain(); return; }
        terrain.create(n, after ? p.spacing : p.spacingBefore, after ? p.origin : p.originBefore);
        terrain.base = after ? p.baseAfter : p.baseBefore;
        terrain.paint = after ? p.paintAfter : p.paintBefore;
    } else {
        const auto& b = after ? p.baseAfter : p.baseBefore;
        const auto& pt = after ? p.paintAfter : p.paintBefore;
        for (uint32_t r = 0; r < p.h; ++r) {
            size_t k = (size_t)(p.j0 + r) * p.n + p.i0;
            std::copy(b.begin() + (size_t)r * p.w, b.begin() + (size_t)(r + 1) * p.w, terrain.base.begin() + k);
            std::copy(pt.begin() + (size_t)r * p.w * C, pt.begin() + (size_t)(r + 1) * p.w * C, terrain.paint.begin() + k * C);
        }
    }
    terrain.touch();
}

bool World::undo(std::string* label) {
    if (editing_) cancelEdit();
    if (undo_.empty()) return false;
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    doc = e.docBefore;
    if (e.terrain) applyPatch(*e.terrain, false);
    docToTerrain();
    if (label) *label = e.label;
    redo_.push_back(std::move(e));
    touch();
    return true;
}

bool World::redo(std::string* label) {
    if (redo_.empty()) return false;
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    doc = e.docAfter;
    if (e.terrain) applyPatch(*e.terrain, true);
    docToTerrain();
    if (label) *label = e.label;
    undo_.push_back(std::move(e));
    touch();
    return true;
}

json World::history(size_t max) const {
    json u = json::array(), r = json::array();
    for (size_t i = undo_.size(); i-- > 0 && u.size() < max;) u.push_back(undo_[i].label);
    for (size_t i = redo_.size(); i-- > 0 && r.size() < max;) r.push_back(redo_[i].label);
    return {{"undo", u}, {"redo", r}};
}

// --------------------------------------------------------------------------- doc helpers
json& World::list(const std::string& section) {
    json& s = doc[section];
    if (!s.is_array()) s = json::array();
    return s;
}

json* World::find(const std::string& id, std::string* section) {
    for (auto& s : idSections()) {
        if (!doc.contains(s) || !doc[s].is_array()) continue;
        for (json& o : doc[s])
            if (o.value("id", "") == id) {
                if (section) *section = s;
                return &o;
            }
    }
    return nullptr;
}

bool World::erase(const std::string& id) {
    for (auto& s : idSections()) {
        if (!doc.contains(s) || !doc[s].is_array()) continue;
        json& arr = doc[s];
        for (size_t i = 0; i < arr.size(); ++i)
            if (arr[i].value("id", "") == id) { arr.erase(i); return true; }
    }
    return false;
}

std::string World::newId(const std::string& prefix) const {
    // smallest free number for this prefix
    std::set<std::string> used;
    for (auto& s : idSections())
        if (doc.contains(s) && doc[s].is_array())
            for (auto& o : doc[s]) used.insert(o.value("id", ""));
    for (uint32_t i = 1;; ++i) {
        std::string id = std::format("{}_{}", prefix, i);
        if (!used.count(id)) return id;
    }
}
}  // namespace df
