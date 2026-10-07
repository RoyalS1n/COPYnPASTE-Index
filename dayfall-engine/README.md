# DAYFALL engine

A custom C++20 / Vulkan 1.3 engine for DAYFALL, built so AI agents can build the game in a live editor.

- **Agent-first editor.** The editor serves MCP (`http://127.0.0.1:7777/mcp`). Agents read the world, edit it
  with the tools in [docs/TOOLS.md](docs/TOOLS.md), see every change as rendered images, and test routes with the real character physics. The
  workflow rules are enforced: a batch cannot close until its edits have been looked at, and terrain, lighting
  and character are never changed in one batch. See [docs/AGENT_WORKFLOW.md](docs/AGENT_WORKFLOW.md).
- **One world document.** A map is `maps/<name>/map.json` plus a terrain heightfield. Agents and people can
  read and diff it; every edit through the tools can be undone. See [docs/MAP_FORMAT.md](docs/MAP_FORMAT.md).
- **Renderer.** GPU-driven: compute culling and LOD selection into indirect draws, vertex pulling, bindless
  textures, cascaded shadows, MSAA, a physical sky with sky lighting, water refraction, AgX tone mapping. The
  materials (coursed stone, terrain, foliage, grass, water) are the shading models from the Blender worlds.
- **Gameplay.** Jolt Physics, a walk / run / jump character with a third- or first-person camera, gamepad
  support, collectibles and goals.

## Build

Requirements: CMake 3.24+, a C++20 compiler (Visual Studio 2022 on Windows), Git, and a GPU driver with Vulkan
1.3 (any GTX 10 / RX 400 / Arc or newer). Every library is downloaded and pinned by CMake. The Vulkan SDK is
optional: with it, shaders compile with its `glslangValidator` and `--validation` works; without it CMake builds
glslang itself.

Windows:

```
cmake --preset windows
cmake --build build/windows --config Release
```

Linux:

```
cmake --preset linux
cmake --build build/linux -j
```

The engine lands in `bin/` (`bin\dayfall.exe` plus `bin\shaders\`).

## Run

```
bin\dayfall.exe                      # the editor on maps/starter, MCP on port 7777
bin\dayfall.exe maps\my_level        # another map
bin\dayfall.exe maps\new --new "New" --terrain valley    # create a map
bin\dayfall.exe --help               # every option
```

Editor: hold the right mouse button and use WASD / Q / E to fly (Shift: faster, wheel: speed), F frames the map,
P plays, Ctrl+S saves, Ctrl+Z / Ctrl+Y undo / redo, F12 saves a screenshot. Play: WASD, Shift to run, Space to
jump, mouse to look, Esc to stop.

## Connect an agent

Open Claude Code in this folder while the editor runs: `.mcp.json` connects it to the editor, and `CLAUDE.md`,
`AGENTS.md` and the `dayfall-level` skill teach it the workflow. Then ask for a level, for example:

> Build a playable base level on a new map maps/valley_town: a valley, a dirt path to a small town at the north
> end, and 3 glowing collectibles along the way. Walk-test the route.

Without a window (cloud agents, CI): `bin/dayfall maps/x --headless --mcp http:7777`, or register
`bin/dayfall maps/x --headless --mcp stdio` as a stdio MCP server so the agent launches the engine itself.

## Porting maps

`maps/golden_valley` and `maps/serene_meadow` are the Blender reference worlds, ported; `maps/fortress_kit` is a
courtyard castle built from the Unreal project's fortress kit and materials (`content/fortress`). The Unreal level is ported
on the developer's machine with `tools/unreal/export_level_to_dayfall.py` (runs inside the Unreal Editor) and,
when Unreal's glTF Exporter plugin is off, `tools/unreal/fbx_to_glb.py` (Blender). Steps, checks and a prompt for
a local agent: [docs/PORTING.md](docs/PORTING.md).

## Command-line tools

```
bin/dayfall maps/starter --exec tests/smoke.json     # run tool calls from a file (with optional expectations)
bin/dayfall maps/starter --capture all --out shots   # render the saved views
bin/dayfall maps/starter --walk-test loop            # exit code 0 if the route passes
bin/dayfall maps/starter --bench 20                  # orbit the map and report frame times
bin/dayfall --list-tools                             # the tool reference (docs/TOOLS.md)
```

## Layout

| folder | contents |
|---|---|
| `src/gfx` | Vulkan device, swapchain, resources, pipelines |
| `src/render` | the renderer, sky, light grid, camera, GPU data layout |
| `src/scene` | meshes, materials, primitives, glTF loading, skeletons and clips, the render-ready scene |
| `src/world` | the map document, terrain, paths, scatter, environment, the scene builder |
| `src/physics` | Jolt: static world, ray casts, the character controller |
| `src/game` | the player, the mannequin, the rigged-character animation states, collectibles, the walk test |
| `src/editor` | the editor, the agent tools, the MCP server |
| `shaders` | GLSL, compiled to SPIR-V at build time |
| `maps` | maps; `maps/starter` is the template |
| `content` | the shared content library (meshes, materials); `content/fortress` is the FloatingIslet fortress kit |
| `tools` | scripts: `make_starter.json` rebuilds the starter map; `unreal/` ports Unreal levels |
| `docs` | workflow, map format, tool reference, porting |
