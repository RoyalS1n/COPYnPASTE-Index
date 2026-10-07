// DAYFALL engine entry point: the live editor (window or headless), the MCP
// server for agents, and command-line batch modes (capture, walk test, exec).
#include "core/Error.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Log.h"
#include "editor/Editor.h"
#include "editor/McpServer.h"
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>

using namespace df;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
const char* kUsage = R"(DAYFALL engine

usage: dayfall [MAP_DIR] [options]

  MAP_DIR                 map directory with map.json (default: maps/starter)
  --new NAME              create a new map in MAP_DIR (with --terrain PRESET, default hills)
  --terrain PRESET        flat | hills | valley | mountains | meadow | island (with --new)
  --play                  start in play mode
  --headless              no window (servers, CI, cloud agents)
  --mcp http[:PORT]       serve MCP on http://127.0.0.1:PORT/mcp (the windowed editor does this on 7777 by default)
  --mcp stdio             serve MCP on stdin/stdout (an agent launches the engine itself; logs go to stderr)
  --no-mcp                no MCP server
  --list-tools            print the agent tool reference (Markdown) and exit
  --exec FILE             run tool calls from a JSON file [{"tool": ..., "args": {...}}], print results, exit
  --capture VIEWS         render views (overview, player, top_down, all, or camera names; comma separated) and exit
  --out DIR               where --capture writes images (default: <map>/captures)
  --walk-test ROUTE       walk the mannequin along a route (or "path:<id>"), print the report, exit 0 if passed
  --bench SECONDS         orbit the map for SECONDS and report frame times
  --size WxH              window / capture size (default 1600x900)
  --msaa N                1, 2, 4 or 8 (default 4)
  --shadow-size N         shadow map size per cascade (default 2048)
  --no-vsync              uncapped frame rate
  --no-rules              do not enforce the batch rules (verify after edits, terrain / lighting / character apart)
  --content DIR           content library (default: auto-detected 'content' next to the engine)
  --validation            Vulkan validation layers
  --gpu N                 pick a GPU by index

Editor controls: hold right mouse + WASD/QE to fly (Shift faster, wheel: speed), F frame the map,
P play / stop, Ctrl+S save, Ctrl+Z undo, Ctrl+Y redo, F12 screenshot.
Play: WASD move, Shift run, Space jump, mouse look, Esc back to the editor. Gamepads work too.
)";

fs::path findContentDir() {
    fs::path exe = executableDir();
    for (fs::path p : {exe / "content", exe / ".." / "content", exe / ".." / ".." / "content", exe / ".." / ".." / ".." / "content",
                       fs::current_path() / "content"})
        if (fs::exists(p / "library.json")) return fs::weakly_canonical(p);
    return exe / "content";
}

struct InputState {
    double lastX = 0, lastY = 0;
    bool haveMouse = false;
    float flySpeed = 12.0f;
    double scroll = 0;
    bool keyPrev[GLFW_KEY_LAST + 1]{};
};
InputState* g_input = nullptr;

bool pressedOnce(GLFWwindow* w, int key) {
    bool down = glfwGetKey(w, key) == GLFW_PRESS;
    bool was = g_input->keyPrev[key];
    g_input->keyPrev[key] = down;
    return down && !was;
}

int runExec(Editor& ed, const fs::path& file) {
    json calls = json::parse(readText(file));
    if (!calls.is_array()) throw Error("--exec expects a JSON array of {\"tool\": ..., \"args\": {...}}");
    int failures = 0;
    json out = json::array();
    for (auto& c : calls) {
        ToolResult r = ed.call(c.at("tool").get<std::string>(), c.value("args", json::object()));
        json o = {{"tool", c["tool"]}, {"ok", !r.error}, {"result", r.data}};
        json saved = json::array();
        for (auto& img : r.images) saved.push_back(img.path);
        if (!saved.empty()) o["images"] = saved;
        out.push_back(o);
        if (r.error) ++failures;
    }
    std::cout << out.dump(2) << std::endl;
    return failures ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
    fs::path mapDir;
    std::string newName, terrainPreset = "hills", mcp, capture, walkRoute, execFile;
    fs::path outDir, contentDir;
    bool play = false, headless = false, rules = true, noMcp = false, listTools = false;
    double bench = 0;
    EngineOptions eo;
    eo.render.msaa = 4;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { std::fputs(kUsage, stderr); return 0; }
        else if (a == "--new") newName = next();
        else if (a == "--terrain") terrainPreset = next();
        else if (a == "--play") play = true;
        else if (a == "--headless") headless = true;
        else if (a == "--mcp") mcp = next();
        else if (a == "--no-mcp") noMcp = true;
        else if (a == "--list-tools") listTools = true;
        else if (a == "--exec") execFile = next();
        else if (a == "--capture") capture = next();
        else if (a == "--out") outDir = next();
        else if (a == "--walk-test") walkRoute = next();
        else if (a == "--bench") bench = std::atof(next().c_str());
        else if (a == "--size") { std::string s = next(); std::sscanf(s.c_str(), "%ux%u", &eo.width, &eo.height); }
        else if (a == "--msaa") eo.render.msaa = (uint32_t)std::atoi(next().c_str());
        else if (a == "--shadow-size") eo.render.shadowSize = (uint32_t)std::atoi(next().c_str());
        else if (a == "--no-vsync") eo.vsync = false;
        else if (a == "--no-rules") rules = false;
        else if (a == "--content") contentDir = next();
        else if (a == "--validation") eo.validation = true;
        else if (a == "--gpu") eo.gpu = std::atoi(next().c_str());
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n\n%s", a.c_str(), kUsage); return 2; }
        else mapDir = a;
    }
    bool batchMode = !capture.empty() || !walkRoute.empty() || !execFile.empty() || listTools;
    if (batchMode && mcp.empty() && bench == 0) headless = true;
    if (listTools) headless = true;
    eo.headless = headless;
    if (mapDir.empty()) {
        // maps/starter next to the working directory, else next to the engine
        fs::path exeMaps = executableDir() / ".." / "maps" / "starter";
        if (fs::exists("maps/starter/map.json") || !newName.empty()) mapDir = "maps/starter";
        else if (fs::exists(exeMaps / "map.json")) mapDir = fs::weakly_canonical(exeMaps);
        else mapDir = "maps/untitled";
    }
    // the live editor serves MCP by default, like an editor plugin: agents attach over HTTP
    if (mcp.empty() && !noMcp && !headless && !batchMode && bench == 0) mcp = "http:7777";

    Engine engine;
    engine.init(eo);
    Editor ed;
    EditorOptions edo;
    edo.enforceRules = rules;
    edo.contentDir = contentDir.empty() ? findContentDir() : contentDir;
    ed.init(engine, edo);

    int exitCode = 0;
    try {
        if (!newName.empty()) {
            if (fs::exists(mapDir / "map.json")) throw Error(mapDir.string() + " already contains a map");
            ed.newMap(mapDir, newName, json{{"preset", terrainPreset}, {"size_m", 512}});
            ed.world.save();
        } else if (fs::exists(mapDir / "map.json")) {
            ed.openMap(mapDir);
        } else {
            logWarn("{} has no map.json: starting a new unsaved map there", mapDir.string());
            ed.newMap(mapDir, mapDir.filename().string(), json{{"preset", "hills"}, {"size_m", 512}});
        }
    } catch (const Error& e) {
        logError("{}", e.what());
        ed.shutdown();
        engine.shutdown();
        return 1;
    }
    for (auto& w : ed.lastBuild.warnings) (void)w;

    // ---------------------------------------------------------------- batch modes
    if (batchMode) {
        try {
            if (listTools) {
                std::cout << "# DAYFALL agent tools\n\nGenerated by `dayfall --list-tools`. Categories: read and meta tools never "
                             "count as edits; terrain, lighting and character edits must go in separate batches.\n\n";
                for (const Tool& t : ed.tools()) {
                    std::cout << "## " << t.name << "\n\n*" << categoryName(t.category) << "*" << (t.categoryFor ? " (depends on the arguments)" : "")
                              << "\n\n" << t.description << "\n\n";
                    const json& props = t.schema.value("properties", json::object());
                    if (!props.empty()) {
                        std::cout << "| argument | type | notes |\n|---|---|---|\n";
                        auto req = t.schema.value("required", json::array());
                        for (auto& [k, v] : props.items()) {
                            bool r = std::find(req.begin(), req.end(), k) != req.end();
                            std::string type = v.contains("type") ? (v["type"].is_string() ? v["type"].get<std::string>() : v["type"].dump()) : "any";
                            std::string d = v.value("description", "");
                            for (auto& ch : d) if (ch == '|') ch = '/';
                            std::cout << "| " << k << (r ? " (required)" : "") << " | " << type << " | " << d << " |\n";
                        }
                        std::cout << "\n";
                    }
                }
            }
            if (!execFile.empty()) exitCode = runExec(ed, execFile);
            if (!capture.empty()) {
                json views = json::array();
                std::string list = capture == "all" ? "overview,player,top_down" : capture;
                if (capture == "all") for (auto& [k, v] : ed.scene.cameras) if (k != "overview" && k != "player") list += "," + k;
                size_t pos = 0;
                while (pos <= list.size()) {
                    size_t e = list.find(',', pos);
                    std::string v = list.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                    if (ed.scene.cameras.count(v)) views.push_back({{"camera", v}});   // a map camera wins over a built-in view name
                    else if (v == "overview" || v == "player" || v == "editor") views.push_back({{v, true}});
                    else if (v == "top_down") views.push_back({{"top_down", json::object()}});
                    else if (!v.empty()) views.push_back({{"camera", v}});
                    if (e == std::string::npos) break;
                    pos = e + 1;
                }
                for (size_t k = 0; k < views.size(); k += 6) {
                    json chunk(views.begin() + k, views.begin() + std::min(views.size(), k + 6));
                    ToolResult r = ed.call("capture", {{"views", chunk}, {"width", eo.width}, {"height", eo.height}, {"save", true}});
                    if (r.error) throw Error(r.data.value("error", "capture failed"));
                    for (auto& img : r.images) {
                        if (!outDir.empty()) {
                            fs::create_directories(outDir);
                            fs::copy_file(img.path, outDir / (img.label + fs::path(img.path).extension().string()), fs::copy_options::overwrite_existing);
                        }
                        logInfo("captured {} -> {}", img.label, outDir.empty() ? img.path : (outDir / img.label).string());
                    }
                }
            }
            if (!walkRoute.empty()) {
                json args = walkRoute.rfind("path:", 0) == 0 ? json{{"path_id", walkRoute.substr(5)}} : json{{"route", walkRoute}};
                ToolResult r = ed.call("walk_test", args);
                std::cout << r.data.dump(2) << std::endl;
                exitCode = (!r.error && r.data.value("passed", false)) ? exitCode : 1;
            }
        } catch (const std::exception& e) {
            logError("{}", e.what());
            exitCode = 1;
        }
        ed.shutdown();
        engine.shutdown();
        return exitCode;
    }

    // ---------------------------------------------------------------- MCP + live loop
    MainThreadQueue queue;
    McpServer server(ed, queue);
    StdioTransport stdio;
    HttpTransport http;
    std::atomic<bool> quit{false};
    if (mcp == "stdio") stdio.start(server, quit);
    else if (mcp.rfind("http", 0) == 0) {
        int port = mcp.size() > 5 && mcp[4] == ':' ? std::atoi(mcp.c_str() + 5) : 7777;
        if (!http.start(server, "127.0.0.1", port)) { ed.shutdown(); engine.shutdown(); return 1; }
    } else if (!mcp.empty()) {
        logError("--mcp must be stdio or http[:PORT]");
        return 2;
    }
    if (play) ed.startPlay();

    InputState input;
    g_input = &input;
    if (engine.window) {
        glfwSetScrollCallback(engine.window, [](GLFWwindow*, double, double dy) { g_input->scroll += dy; });
    }
    auto last = std::chrono::steady_clock::now();
    double benchStart = -1, benchFrames = 0, benchGpu = 0;
    std::vector<double> frameTimes;
    while (!quit) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        queue.drain();
        PlayerInput pin;
        if (engine.window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(engine.window)) break;
            GLFWwindow* w = engine.window;
            double mx, my;
            glfwGetCursorPos(w, &mx, &my);
            float dx = input.haveMouse ? (float)(mx - input.lastX) : 0.0f, dy = input.haveMouse ? (float)(my - input.lastY) : 0.0f;
            input.lastX = mx;
            input.lastY = my;
            input.haveMouse = true;
            bool ctrl = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
            auto key = [&](int k) { return glfwGetKey(w, k) == GLFW_PRESS; };
            if (pressedOnce(w, GLFW_KEY_P) && !ctrl) {
                if (ed.playing()) ed.stopPlay(); else ed.startPlay();
                glfwSetInputMode(w, GLFW_CURSOR, ed.playing() ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            }
            if (pressedOnce(w, GLFW_KEY_ESCAPE) && ed.playing()) {
                ed.stopPlay();
                glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
            if (ctrl && pressedOnce(w, GLFW_KEY_S)) ed.call("save", {});
            if (ctrl && pressedOnce(w, GLFW_KEY_Z)) ed.call("undo", {});
            if (ctrl && pressedOnce(w, GLFW_KEY_Y)) ed.call("redo", {});
            if (pressedOnce(w, GLFW_KEY_F12)) ed.call("capture", {{"views", json::array({{{ed.playing() ? "player" : "editor", true}}})}});
            if (pressedOnce(w, GLFW_KEY_F) && !ed.playing()) ed.editCamera = ed.overviewCamera();
            if (ed.playing()) {
                pin.move = vec2((key(GLFW_KEY_D) ? 1.f : 0.f) - (key(GLFW_KEY_A) ? 1.f : 0.f), (key(GLFW_KEY_W) ? 1.f : 0.f) - (key(GLFW_KEY_S) ? 1.f : 0.f));
                pin.run = key(GLFW_KEY_LEFT_SHIFT);
                pin.jump = key(GLFW_KEY_SPACE);
                pin.lookYaw = -dx * 0.0025f;
                pin.lookPitch = -dy * 0.0025f;
                GLFWgamepadstate gp;
                if (glfwGetGamepadState(GLFW_JOYSTICK_1, &gp)) {
                    auto dz = [](float v) { return std::abs(v) < 0.15f ? 0.0f : v; };
                    pin.move += vec2(dz(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X]), -dz(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]));
                    pin.lookYaw -= dz(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_X]) * 2.6f * dt;
                    pin.lookPitch -= dz(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]) * 2.0f * dt;
                    pin.jump |= gp.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS;
                    pin.run |= gp.buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] == GLFW_PRESS || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.3f;
                }
            } else {
                Camera& c = ed.editCamera;
                if (input.scroll != 0) { input.flySpeed = std::clamp(input.flySpeed * (float)std::pow(1.2, input.scroll), 1.0f, 800.0f); input.scroll = 0; }
                if (glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
                    c.yaw -= dx * 0.003f;
                    c.pitch = std::clamp(c.pitch - dy * 0.003f, -1.55f, 1.55f);
                    vec3 f = c.forward(), r = glm::normalize(glm::cross(f, vec3(0, 0, 1)));
                    float sp = input.flySpeed * (key(GLFW_KEY_LEFT_SHIFT) ? 4.0f : 1.0f) * dt;
                    if (key(GLFW_KEY_W)) c.position += f * sp;
                    if (key(GLFW_KEY_S)) c.position -= f * sp;
                    if (key(GLFW_KEY_D)) c.position += r * sp;
                    if (key(GLFW_KEY_A)) c.position -= r * sp;
                    if (key(GLFW_KEY_E)) c.position.z += sp;
                    if (key(GLFW_KEY_Q)) c.position.z -= sp;
                }
            }
            if (bench > 0) {
                if (benchStart < 0) benchStart = 0;
                benchStart += dt;
                Camera& c = ed.editCamera;
                Camera o = ed.overviewCamera();
                float ang = (float)benchStart * 0.15f;
                vec3 centre = o.position + o.forward() * glm::length(o.position - vec3(o.position.x, o.position.y, 0)) * 1.2f;
                float rad = glm::length(vec2(o.position) - vec2(centre)) * 0.8f;
                c.position = vec3(vec2(centre) + vec2(std::cos(ang), std::sin(ang)) * rad, o.position.z * 0.6f);
                c.lookAt(centre);
                frameTimes.push_back(dt);
                benchGpu += engine.lastGpuMs();
                ++benchFrames;
                if (benchStart > bench) break;
            }
            ed.update(dt, pin);
            ed.render();
            static double titleTimer = 0;
            titleTimer += dt;
            if (titleTimer > 0.25) {
                titleTimer = 0;
                std::string msg = ed.playing() && !ed.game.messages.empty() ? "  |  " + ed.game.messages.back().text : "";
                std::string hud = ed.playing() && ed.game.collectibles ? std::format("  |  {}/{} collected", ed.game.collected, ed.game.collectibles) : "";
                glfwSetWindowTitle(w, std::format("DAYFALL  |  {}{}{}  |  {:.1f} ms GPU", ed.status, hud, msg, engine.lastGpuMs()).c_str());
            }
        } else {
            // headless: serve tool calls; keep pickups animating for captures
            queue.waitAndDrain(std::chrono::milliseconds(50));
            ed.update(dt, pin);
            if (mcp.empty()) break;
        }
    }
    if (bench > 0 && benchFrames > 0) {
        std::sort(frameTimes.begin(), frameTimes.end());
        double avg = 0;
        for (double t : frameTimes) avg += t;
        avg /= frameTimes.size();
        std::cout << json{{"frames", benchFrames}, {"avg_frame_ms", avg * 1000}, {"p99_frame_ms", frameTimes[(size_t)(frameTimes.size() * 0.99)] * 1000},
                          {"avg_gpu_ms", benchGpu / benchFrames}, {"gpu", engine.device.gpuName}}.dump(2) << std::endl;
    }
    http.stop();
    stdio.stop();
    if (ed.world.dirty) logWarn("exiting with unsaved changes in {}", ed.world.dir.string());
    ed.shutdown();
    engine.shutdown();
    std::fflush(stdout);
    std::_Exit(exitCode);   // the stdio reader may still be blocked in getline
}
