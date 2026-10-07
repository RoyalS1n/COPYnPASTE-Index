# Agent guide: DAYFALL engine

Read this before changing anything in this folder.

## What this is

A custom C++20 / Vulkan 1.3 game engine for DAYFALL, built for AI agents to edit live. The editor serves MCP;
agents build levels through tools and verify them with captures. Engine code is in `src/`, shaders in
`shaders/`, maps in `maps/`, the content library in `content/`, docs in `docs/`.

## Level editing (through the dayfall MCP server)

Follow `docs/AGENT_WORKFLOW.md`. In short:

1. Read first: `project_info`, then `world_get` / `catalog` / `terrain_info` as needed.
2. One theme per batch: `batch_begin` -> edits -> `capture` -> inspect -> fix -> `batch_end`.
3. A successful tool call doesn't mean the level is right: look at the captures every time.
4. Never change terrain, lighting and character in one batch (the engine refuses).
5. Block out with primitives and `walk_test` the route before adding art; `play_sim` for jumps.
6. Report what you saw in the captures, not only what the tools returned.

Do not hand-edit `maps/*/terrain/*` (binary). Editing `map.json` by hand is fine while the editor is closed;
with the editor open, use the tools (or `doc_patch`) so undo and the live view stay in sync.

## Engine code

Build (Windows): `cmake --preset windows` then `cmake --build build/windows --config Release`.
Build (Linux): `cmake --preset linux && cmake --build build/linux -j`.
The engine always lands in `bin/` (`bin/dayfall.exe` + `bin/shaders/`).

Check a change without a window:

```
bin/dayfall maps/starter --exec tests/smoke.json        # run tool calls, prints JSON, exit 1 on failure
bin/dayfall maps/starter --capture all --out <dir>      # render every saved view
bin/dayfall maps/starter --walk-test loop               # exit 0 if the route passes
bin/dayfall --list-tools > docs/TOOLS.md                # regenerate the tool reference
```

Use `--validation` when you touch Vulkan code: validation errors are bugs.

Conventions:

- World space is Z up, metres, X east, Y north. glTF (Y up) is converted on load.
- `shaders/include/common.glsl` and `src/render/GpuTypes.h` mirror each other byte for byte: change both.
- Recoverable problems throw `df::Error` (tools report them to the agent); `fatal()` is for GPU failures only.
- Logs go to stderr: stdout carries MCP in `--mcp stdio` mode.
- A new tool goes in `src/editor/Tools.cpp` with a short, exact description and a schema; regenerate
  `docs/TOOLS.md`.
- Keep edits undoable: every world change goes through `World::beginEdit` / `endEdit`.

## Change log

Every change adds one line to the project change log, newest first, in the same commit:
`- YYYY-MM-DD [Agent] what changed — why — where`. On the developer's machine that is the "Change summaries"
section of `AI_README.md`; in this repository it is `changes.md` at the repository root, which is merged into
AI_README.md and then deleted. Never keep a second log.
