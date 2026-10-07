#pragma once
// The live editor: owns the world, the built scene, physics and gameplay, and
// exposes every operation as a tool (see Tools.cpp) that agents call over MCP.
#include "engine/Engine.h"
#include "game/Game.h"
#include "game/Hud.h"
#include "physics/Physics.h"
#include "world/SceneBuilder.h"
#include "world/World.h"
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace df {
enum class ToolCategory { Read, Meta, Terrain, Lighting, Character, Layout, Foliage, Gameplay, Materials, Assets };
const char* categoryName(ToolCategory c);

struct ToolImage { std::string mime, base64, label, path; };
struct ToolResult {
    nlohmann::json data = nlohmann::json::object();
    std::vector<ToolImage> images;
    bool error = false;
    static ToolResult fail(const std::string& msg) { ToolResult r; r.error = true; r.data = {{"error", msg}}; return r; }
};
struct Tool {
    std::string name, description;
    ToolCategory category;
    nlohmann::json schema;            // JSON schema of the arguments
    std::function<ToolResult(const nlohmann::json&)> run;
    std::function<ToolCategory(const nlohmann::json&)> categoryFor;   // optional: category depends on the arguments
    bool verifies = false;                                            // a capture: counts as looking at the result
    bool edits() const { return category != ToolCategory::Read && category != ToolCategory::Meta; }
};

struct BatchState {
    bool open = false, implicit = false;
    int number = 0;
    std::string title, intent;
    std::set<ToolCategory> touched;
    int edits = 0, editsSinceCapture = 0, captures = 0;
    std::vector<std::string> calls;
    nlohmann::json docAtStart;               // the document when the batch opened (world_diff)
    uint64_t terrainVersionAtStart = 0;
    nlohmann::json toJson() const;
};

struct EditorOptions {
    bool enforceRules = true;
    std::filesystem::path contentDir;
    uint32_t captureWidth = 1024, captureHeight = 576;
};

struct CaptureView {
    std::string label;
    Camera camera;
    bool showPlayer = false;
    bool selection = false;              // draw the editor selection's boxes over it
};

class Editor {
public:
    void init(Engine& engine, const EditorOptions& opt);
    void shutdown();
    void openMap(const std::filesystem::path& dir);
    void newMap(const std::filesystem::path& dir, const std::string& name, const nlohmann::json& terrain);

    // main thread only
    ToolResult call(const std::string& tool, const nlohmann::json& args);
    const std::vector<Tool>& tools() const { return tools_; }
    std::string instructions() const;

    // frame loop
    void update(float dt, const PlayerInput& input);
    void render();                       // to the window
    bool rebuildIfNeeded();              // world -> scene -> GPU + physics; true if rebuilt
    void startPlay();
    void stopPlay();
    bool playing() const { return game.active(); }

    // captures
    std::vector<uint8_t> captureRgba(const CaptureView& v, uint32_t w, uint32_t h, float time);
    Camera overviewCamera() const;
    Camera playerStartCamera() const;
    float groundHeight(vec2 p) const;    // terrain + collision geometry
    // HUD (EditorHud.cpp), drawn over frames and captures through Engine::hudBuild
    void drawHud(HudCanvas& c);
    int hudForce = -1;                   // 1: also while editing (a preview at the player start), 0: never, -1: while playing

    // selection shared between the human and agents: a click in the editor window picks an item (shift adds),
    // editor_select highlights items for the human, editor_state tells an agent what "this" is
    struct Selected { std::string id; uint32_t instance = UINT32_MAX; vec3 point{0}; };   // instance: one scatter / file instance
    std::vector<Selected> selection;
    bool hasCursorGround = false;        // the surface under the mouse in the editor window
    vec3 cursorGround{0};
    void pick(vec2 ndc, bool add);       // ndc: -1..1, y up; a click in the window
    void hover(vec2 ndc);
    bool selectionBox(const Selected& s, vec3& lo, vec3& hi);   // world box of a selected item (or its one instance)

    Engine* engine = nullptr;
    EditorOptions options;
    World world;
    SceneBuilder builder;
    Scene scene;
    std::vector<EntityState> entities;
    Physics physics;
    Game game;
    BuildInfo lastBuild;
    BatchState batch;
    Camera editCamera;
    float time = 0;
    std::string status;                  // one line for the window title / overlay

private:
    void registerTools();                // Tools.cpp
    void registerWorldTools();           // WorldTools.cpp
    void addTool(Tool t) { tools_.push_back(std::move(t)); }
    ToolResult runEdit(const Tool& t, const nlohmann::json& args);
    std::vector<Tool> tools_;
    uint64_t builtVersion_ = 0, builtTerrainVersion_ = 0;
    const Terrain* terrainUploaded_ = nullptr;
    int captureCounter_ = 0;
    void updateMinimap();
    void drawEditorOverlay(HudCanvas& c);   // selection boxes and labels (EditorHud.cpp), window frames only
    bool rayFromWindow(vec2 ndc, vec3& origin, vec3& dir) const;
    bool windowFrame_ = false;
    const Camera* overlayCamera_ = nullptr;   // a capture with the selection drawn: its camera
    uint64_t minimapBuilt_ = ~0ull, minimapKey_ = ~0ull;
    vec2 minimapOrigin_{0};
    float minimapSize_ = 0;
    friend struct ToolRegistrar;
};
}  // namespace df
