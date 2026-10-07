# Handoff: DAYFALL engine (cloud session → local session)

Written 2026-10-07 by the cloud session that built most of `dayfall-engine/` on branch
`claude/hopeful-lamport-xbvnfe`. Read this first, then `dayfall-engine/AGENTS.md`.
It follows the minimum-handoff template in section 12.3 of the user's `guide.md`, with two task briefs at the end.

## 0. First things to do locally

1. `git fetch origin && git checkout claude/hopeful-lamport-xbvnfe && git pull`. The working tree was clean and pushed at `2098a4f`
   (plus the commit that adds this file). The branch is open as **PR #1**
   (https://github.com/RoyalS1n/COPYnPASTE-Index/pull/1, into `main`, mergeable, no CI checks configured). Pushing to the
   branch updates the PR. Its auto-written description overstates the texture library: it has 100 textures, not "1900+ materials".
2. **Merge `changes.md` (repo root) into the "Change summaries" section of**
   `C:\Users\keega\Documents\Unreal Projects\FloatingIslet\AI_README.md` (newest first, entries pasted unchanged).
   Then delete `changes.md` and commit. Only one change log may exist; `changes.md` exists only because AI_README
   was unreachable from the cloud. From then on, log every change in AI_README as `- YYYY-MM-DD [Agent] what — why — where`.
3. Build on Windows: `cmake --preset windows` then `cmake --build build/windows --config Release` (inside
   `dayfall-engine/`). The output goes to `bin/dayfall.exe` and `bin/shaders/`.
4. Run the checks (all were green in the cloud, with `--validation` and 0 VUIDs):
   ```
   bin/dayfall maps/starter --validation --exec tests/<name>.json
   #   names: smoke, world_tools, terrain_tools, layout_tools, prefabs, water, procedural, character, hud, regressions
   python3 tools/test_mcp.py          # MCP server over stdio + HTTP; its SDK tests need: python -m pip install mcp
   bin/dayfall --list-tools > docs/TOOLS.md   # after any tool change
   ```
   The cloud ran on llvmpipe (CPU Vulkan), so **no real GPU timings exist yet**. Run
   `bin/dayfall maps/serene_meadow --bench 20` locally and record the results, especially the cost of the new water shader.

## 1. Rules carried over (from the user, the guide, AGENTS.md)

- **Git:** develop on `claude/hopeful-lamport-xbvnfe`; `git push -u origin claude/hopeful-lamport-xbvnfe`. No PRs unless asked.
  No model names in commits or files.
- **Change log:** one line per change, in the same commit (see step 2 above).
- **Licences:** take code only under MIT, BSD, Apache-2.0, zlib or CC0, attributed in `dayfall-engine/THIRD_PARTY.md`.
  GPL code and code with no licence are ideas only: reimplement from the paper.
  - No Unreal Engine code and no UE-only Fab assets.
  - Never treat `.uasset` as a generic asset.
  - The FantasyRPG Polytope assets must **not** be committed until the user confirms their licence.
- **Safety:**
  - Never store secrets.
  - **Never use the Meshy API key or make Meshy network calls.**
  - Downloaded archives are untrusted: extract into a new empty directory and run Python on them with `-I`, never from inside the extracted folder.
- **Unreal:** don't enable plugins or change the startup map or project settings without asking. Close the Unreal editor before saving assets through commandlets.
- **Scope:** the user said "don't port the fortress". The focus is the engine, its MCP server, and an agent-editable world.
- **Agents:** Opus only, at most 3 at a time. Haiku was ruled out. Grade and refine what they produce before committing.
- **Editing:** re-read a file right before editing it, and keep other agents' work.

## 2. What exists now (all committed)

Engine: C++20, Vulkan 1.3, GPU-driven renderer, Jolt physics, and an editor that serves MCP (protocol 2025-11-25, stdio or HTTP
`http:7777`). World space is Z-up, metres, X east, Y north. Maps are `maps/<name>/map.json` plus a terrain PNG.

This session's commits, newest first:

| Commit | What |
|---|---|
| `2098a4f` | Procedural **trees** (`{"type":"tree","species":"oak\|birch\|pine\|bush\|dead","seed":n}`: space colonisation plus pipe model, LODs, trunk collision); **`terrain_erode`** (droplet hydraulic plus thermal; `terrain_generate` `"erosion": 0.6`); **`rivers_generate`** (Priority-Flood, D8 accumulation, traced to sea / edge / confluence, written as ordinary rivers); sculpt ops `terrace` and `redistribute`; scatter `avoid_rules`, `footprint_m` and `companions`. Files: `src/scene/TreeGen.*`, `src/world/Hydrology.*`, `src/world/Terrain.*`, `Scatter.cpp`, `SceneBuilder.cpp`. Test: `tests/procedural.json` |
| `8ef09d7` | 100 original tileable texture JPGs (`tools/make_texture_library.py`), materials `TX_<category>_<name>` in `content/textures/library.json`, `maps/texture_gallery` |
| `85df76f` | Editor fixes found by the agents' tests (a failed rebuild now undoes its edit; `capture` sees new cameras; delete-by-area covers paths, rivers, scatter and lakes; `area_set` counts as an edit when scatter or water follows the area; exact rect bounds). Textures are now upright on walls (triplanar v = −z; primitive UVs). Tests: `terrain_tools`, `layout_tools`, `regressions` |
| `ec29f06` | Water rewrite: fetch-limited waves, screen-space reflections, absorption plus single scattering, caustics, foam, soft shoreline; new water material keys (see `docs/MAP_FORMAT.md` "Water materials") |
| `a153859`, `561641e`, `afd72eb`, ... | MCP prompts; lakes and rivers (`water_set`); prefabs; world tools (`world_check`, `find_space`, named areas, relative placement); editor selection shared with agents; MCP resources, prompts, progress and cancellation |

Docs to trust: `dayfall-engine/AGENTS.md`, `docs/AGENT_WORKFLOW.md`, `docs/MAP_FORMAT.md`, `docs/TOOLS.md` (generated),
`.claude/skills/dayfall-level/SKILL.md`, `THIRD_PARTY.md`.

## 3. Known defects and unverified claims

- **Not measured on a real GPU:** the water shader (48-step SSR march plus 14 waves per pixel), procedural forests (10–20k triangles per tree), and erosion on 4097² terrains (it simulates at reduced resolution above about 1.1M cells).
- **Water:** waves only change the lighting; the surface stays flat, which suits lakes and rivers but not a rough sea. Off-screen objects aren't reflected (the sky is the fallback). I don't claim it beats Unreal; it lacks geometric waves and ray-traced or planar reflections.
- **Textures:**
  - Five shiny metals (brushed_steel, hammered_bronze, diamond_plate, damascus, chainmail) look dark, because there are no local reflection probes.
  - `fabric_tartan` and the 0.5 m fabrics read poorly at distance.
  - `natural_basalt_columns` is a top-down pattern, so it looks wrong on walls.
- **Trees:** a tree's reported `size_m` includes the slightly larger LOD cards, so it can read about 10% tall. The inside of a canopy goes very dark.
- **Validation gaps:** many tools still accept wrongly typed values silently. A cloud suggestion card titled "Tighten argument validation in editor tools" lists every case found by the test agents: bad collision kind, `light color "red"`, `scale -1`, unknown primitive material, `doc_patch` duplicate ids, entity `move_by` z ignored, and others. Repeat it locally if the card is gone.
- **Reddit:** blocked by the cloud network policy, so the user's 19 Reddit links were researched via search only. Locally they can be read directly.

## 4. Research results from the user's link lists (licences checked)

The full write-up, with every link, its licence, what it really is and where it fits, is
**`dayfall-engine/docs/PROCGEN_REFERENCES.md`**. Short version:

- **Code that may be used (MIT, with attribution):**
  - `Adrian104/Dungeon-Generator` ("dgen"): the best repo of the lot. See task A.
  - `dpaulat/worldengine-cpp`: biome tables and quantile thresholds only. Its river code is buggy.
  - `weigert/SimpleHydrology`: MIT per its README, but there is no LICENSE file. Check before porting. Its momentum term is what makes rivers meander.
- **Ideas only:**
  - GPL: `santipaprika/procedural-buildings`, `fegennari/3DWorld`.
  - No licence: City-Generator, procedural-city, the BSP dungeon repo, ProceduralTreeGeneration, the UE5 terrain repo, UE4ProceduralMesh.
- **Empty:** `wyattmchalffey/Cavern` contains no source.
- **Still to do from this research** (beyond tasks A and B):
  - meandering rivers: SimpleHydrology momentum, or Howard–Knutson via meanderpy (Apache-2.0);
  - rock piles settled with Jolt, baked into prefabs;
  - a biome map with quantile bands (worldengine-cpp, MIT);
  - auto bridge and tunnel suggestions on paths.

---

## Task A: Add a `dungeon_generate` tool (port the MIT dgen)

Add a `dungeon_generate` MCP tool to the DAYFALL engine (`dayfall-engine/`, C++20/Vulkan; tools live in `src/editor/Tools.cpp` /
`src/editor/WorldTools.cpp`, primitives in `src/scene/Primitives.cpp`).

**Source to port:** https://github.com/Adrian104/Dungeon-Generator ("dgen"). It is MIT (Copyright (c) 2023 Adrian Kulawik), so
code may be ported. Keep the copyright and permission notice, and add an entry to `dayfall-engine/THIRD_PARTY.md`. A review found it
solid and standard-library-only.

**How it works:**
- **BSP split:** split the longer side at the fraction `(1-rand)/2 + U*rand` (spaceSizeRandomness 0.35). Stop at a random depth between minDepth and maxDepth (7–8). Shrink each leaf by interdistance+1, which leaves corridor gutters.
- **Sparse areas:** from sparseAreaDepth on, a subtree becomes sparse with probability 0.2. A leaf in a sparse subtree gets a room only with probability 0.4.
- **Rooms:** each room is U(0.45, 0.75) of its leaf on each axis, at least 4 cells. With probability 0.35 add a second rectangle, giving L- and T-shaped rooms. One entrance per side.
- **Corridors:** the graph points are the gutter corners plus the entrances. Link N/E/S/W neighbours and connect the left and right subtree rooms bottom-up with A*:
  - a step costs its Manhattan length;
  - a step on an existing corridor costs ×0.4;
  - the heuristic is weighted ×0.4;
  - in the top 2 levels add 2 extra connections across each split, which makes loops.

  Finally drop unused points and merge straight runs.
- **Axes:** DAYFALL is Z-up, metres, X east, Y north.

**Tool shape:** `{origin [x,y], size_m [w,d], cell_m (1-2), depth [min,max], room_size [min,max], double_rooms, loops, seed,
wall_height_m, floor_z or on_ground, style: "primitives"|"fortress_kit", id_prefix}`.
- `primitives` style: a floor box plus wall boxes per room, with gaps at the entrances; corridors as a floor box plus walls; use the existing `courses` materials (`stone`, `brick`).
- `fortress_kit` style: snap `fk_*` wall pieces from `content/fortress` (see the `catalog` tool) to the grid.
- Tag everything with one instance tag, as `prefab_place` does in `WorldTools.cpp`, so one delete-by-tag removes the whole dungeon.
- Return the rooms (centre, size, entrances), the corridor count and the bounding box.
- Report progress with `E.reportProgress` and honour cancellation.

**Done when:**
- the tool is in the `toolTitle` map in `src/editor/Editor.cpp`;
- `docs/MAP_FORMAT.md` and `docs/AGENT_WORKFLOW.md` mention it, and `docs/TOOLS.md` is regenerated;
- a new `tests/dungeon.json` runs green with `bin/dayfall maps/starter --validation --exec tests/dungeon.json`, covering room counts, determinism by seed, bad arguments, and `walk_test` reaching one room from another;
- there are captures of the result;
- one change-summary line is logged.

## Task B: Add settlement layout and building grammar tools

Add two MCP tools to the DAYFALL engine (`dayfall-engine/`; tools live in `src/editor/Tools.cpp` / `src/editor/WorldTools.cpp`,
meshes in `src/scene/Primitives.cpp`). `src/scene/TreeGen.cpp` shows how a procedural mesh type plugs into `makePrimitive`
and gets LODs, collision and catalog entries. Implement both **from the papers only**: the reviewed repos are GPL-3
(santipaprika/procedural-buildings) or have no licence (quote591/City-Generator), so they are ideas only.

1. **`building_generate`** (mesh type `{"type": "building", ...}`, or a tool that emits primitives)
   - Source: Müller et al. 2006, "Procedural Modeling of Buildings" (the CGA shape grammar), and Wonka et al. 2003, "Instant Architecture".
   - A shape is an oriented box plus a symbol, expanded recursively with a seeded RNG.
   - Operations: T/R/S; `Subdiv(axis, sizes)` with absolute and relative sizes; `Repeat(axis, tile)`; `Comp(faces)`; insert-asset.
   - Fixed rule chain per style:
     1. extrude the footprint by floors × 3 m;
     2. split it into its faces (Comp);
     3. Repeat the floors;
     4. Repeat the window tiles (about 2.5 m);
     5. split each tile into wall / window 1.2 m / wall;
     6. put a door at the front ground-floor centre;
     7. add a gable, hip or flat roof.
   - Build into one MeshBuilder with the existing `courses` materials, cached by spec key.
   - Parameters: `{footprint [w,d], floors, style (cottage|townhouse|tower), roof, seed}`.
2. **`settlement_layout`**
   - Source: Parish & Müller 2001, "Procedural Modeling of Cities". Its local constraints map onto DAYFALL's terrain queries:
     - snap to a junction within a radius;
     - cut a road short where it meets another;
     - reject water and steep slopes using `Terrain::waterSurfaceAt` and `slopeDegAt`.
   - Alternatively, for small villages: Poisson points on flat ground (as `find_space` in `WorldTools.cpp` finds them), then a Delaunay triangulation, its minimum spanning tree plus about 15% extra edges, routed as paths.
   - Output:
     - `path_set` roads (3 m main, 2 m lanes);
     - lots every 8–14 m along each path, offset sideways by half the road width plus a 2–3 m setback, facing the road, each checked with the `find_space` tests;
     - a `building_generate` house or a `prefab_place` on each lot.
   - Tag everything with one instance tag so it can be moved and deleted as one.
   - Parameters: `{area, houses, road_style, seed, prefab or building_style}`.

**Done when:**
- there are `toolTitle` entries in `src/editor/Editor.cpp`;
- `docs/MAP_FORMAT.md`, `docs/AGENT_WORKFLOW.md` and `.claude/skills/dayfall-level/SKILL.md` are updated, and `docs/TOOLS.md` is regenerated;
- a `tests/settlement.json` runs green with `--validation`, covering determinism by seed, bad arguments, no houses in water or on steep ground, and `world_check` showing no overlaps;
- there are captures of a generated village;
- one change-summary line is logged.
