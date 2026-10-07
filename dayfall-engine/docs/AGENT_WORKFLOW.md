# Building DAYFALL with Claude in the DAYFALL engine

This is the Unreal + Claude workflow, rebuilt for our own engine. The trick is the same: let the agent work
inside the **live editor**. The difference is that the engine itself enforces the rule that saves the project.

## The eight steps

### 1. The engine and the starter map: walk and jump from minute one

Build once (see the README), then run:

```
bin\dayfall.exe
```

It opens `maps/starter`: rolling hills, a blockout training course (ramp, stairs, jump blocks, a bridge between
two towers), a dirt loop, three glowing collectibles and a goal on the hill. Press **P** to play: WASD to move,
Shift to run, Space to jump, mouse to look, Esc to go back to editing. While playing, a minimap, the
collectible counter and messages are drawn over the view. In edit mode, hold the right mouse button
and use WASD / Q / E to fly.

### 2. The editor starts its MCP server for you

The windowed editor serves MCP at `http://127.0.0.1:7777/mcp` as soon as it opens; there is no plugin to enable.
The log line `MCP server listening on http://127.0.0.1:7777/mcp` confirms it. Other options:

| situation | command |
|---|---|
| live editor (normal) | `bin\dayfall.exe [map]` |
| another port | `bin\dayfall.exe --mcp http:7801` |
| no window (cloud agent, CI) | `bin/dayfall maps/x --headless --mcp http:7777` |
| the agent launches the engine itself | `dayfall maps/x --headless --mcp stdio` as a stdio MCP server |

### 3. Connect Claude Code and read the project first

Open Claude Code in `dayfall-engine/`. The project's `.mcp.json` connects to the editor (run `/mcp` to check
that `dayfall` is connected). `CLAUDE.md` and the project skill `.claude/skills/dayfall-level` tell the agent the
workflow, and the server sends the same rules when it connects. The first calls of every session are reads:
`project_info`, then `world_get`, `catalog` and `terrain_info`. Nothing is changed until the agent knows the map.

The server also offers the docs, the live map document and the newest captures as MCP resources (in Claude Code:
`@dayfall:` then e.g. `dayfall://map/section/routes`), and this workflow as prompts (`build_level`, `review_level`,
`refine_area`, `test_route`; in Claude Code they appear as `/mcp__dayfall__build_level` and so on).

### 4. One prompt builds a playable base level

> Build a playable base level on a new map maps/valley_town: a 512 m valley, a dirt path along the valley floor
> from the south end to a small town at the north end (five or six houses blocked out with primitives, a well,
> fences along the last stretch of the path), and 3 glowing collectibles along the route. Put the player start at
> the south end of the path. Work in batches and show me captures.

The agent creates the map (`map_new`), shapes the terrain (`terrain_generate`, `terrain_info`), lays the path
(`path_set`), blocks out the town (`object_add`, `place_along_path`), adds the collectibles (`entity_add`) and
captures after each batch. Every result comes back as images in the conversation and as files in
`maps/<map>/captures/`.

### 5. Test the whole route with the default mannequin before any custom art

> Define the route from the player start along the path to the town square and walk-test it. Fix every problem
> it reports, then test the jumps onto the platforms with play_sim.

`walk_test` drives the mannequin along the route with real physics and reports every stuck point, fall, steep
slope or water crossing, with a capture of each spot. `play_sim` plays scripted input (move, run, jump, turn)
through the real player controller: use it for jumps, ledges and stairs. A route is done when `passed` is true.

### 6. Bring in a rigged character

Export the character as GLB with its skeleton (a skin) and its clips: idle, walk, run, jump, fall and land
(Blender, Mixamo or any rig; the clips may also come as separate GLBs for the same skeleton, one clip per file,
joints matched by name). Then:

> Import Characters/Ranger.glb as the player character and wire idle, walk, run and jump to its clips.

The agent calls `asset_import` (a rigged model becomes category `characters` and the result lists its clips; a GLB
with clips but no mesh is only copied, for `animation_files`), then, in a character batch on its own,
`player_set {"character": "ranger", "animations": {"idle": "Idle", "walk": "Walk", ...}}`. The result shows the
clip each state plays and rejects clip names the file does not have; `catalog` and `project_info` list the clips
too. Then it checks the moves with `play_sim` and `"camera": "side"`: every step of the timeline reports the
animation state (idle, walk, run, jump, fall, land) and the captures show the pose; `expect_animation` on a step
turns that into a test (see `tests/character.json`). Walk and run play at a rate matched to the speed (clip
`speed_mps`); `root_motion: "strip"` keeps clips that walk forward in place; `character_scale` and
`character_yaw_offset_deg` fix a model that is too big or faces sideways. The physics capsule (and `walk_test`)
stays the same whatever the model. `rigged_dummy` in the content library is a test character with all six clips.
Until a rigged character is assigned, the default jointed mannequin is used, animated procedurally.

### 7. Import your forest and town assets and place them along the route

> Import the meshes in Assets/Town/*.glb, replace the blockout houses with them, line the path with the fence
> and lantern meshes, and plant a mixed forest on the valley slopes away from the path.

`asset_import` registers each model (with LODs and collision), `object_update` swaps a blockout's mesh in place,
`place_along_path` lines paths with props and `scatter_set` plants forests, rocks and grass by rules (slope,
height, distance from paths and buildings). The engine's content library already holds the trees, rocks, grass
and buildings from the Blender worlds, and the FloatingIslet fortress kit (`fk_*`: walls, towers, gate hall, keeps,
houses, market props, with their `FT_*` materials); see `catalog` and `maps/fortress_kit`.

Placing things precisely, without guessing coordinates:

- `find_space {"size": [12, 9], "near": [40, 120], "near_path": {"max_m": 15}}` finds level, free sites for a
  12 x 9 m house near a path, and the yaw that turns its front to the path.
- `object_add` takes `place` instead of `position`: `{"on": "table_1"}`, `{"next_to": "house_1", "side": "east",
  "gap_m": 1}`, `{"relative_to": "house_1", "offset": [0, -6]}` (6 m in front of it, in its own frame).
- `area_set` names regions ("town", "square"); every `area` argument then takes the name, and a forest planted
  with `"area": "forest_north"` follows later changes to that area.
- `object_duplicate` makes rows and rings; `object_update {"tag": "farm", "rotate_by_deg": 30, "pivot": "center"}`
  turns a whole group.
- Water: `water_set {"id": "pond", "area": "pond_area", "carve_m": 1.5}` digs and fills a lake (its level finds the
  rim by itself); `water_set {"id": "river", "points": [...upstream first], "width_m": 5}` carves a river that never
  flows uphill. Like paths, they are derived: deleting one restores the ground.
- Prefabs: dress one farmstead (house, fence, well, lamp), `prefab_save {"name": "farmstead", "tag": "farm"}`, then
  `prefab_place {"name": "farmstead", "position": [...], "yaw_deg": 40}` at each site `find_space` returns (its
  `size` is the prefab's `size_m`). Each copy is tagged with its instance id, so it moves, turns and deletes as one.

### 8. Refine in small batches

> Batch 1: vegetation only, grass in the valley, ferns in the shade of the trees.
> Batch 2: lighting only, golden hour, lanterns in the town.
> Batch 3: the minimap: top-right, 150 m around the player, the main route as a trail, and a timer.
> Batch 4: the town entrance: an arch where the path meets the town, and a goal there that says "Welcome to
> the town".

Each batch is one theme, captured and inspected before the next.

The HUD (minimap, collectible counter, timer, messages) is drawn while playing and in `play_sim` captures.
`hud_set` changes it; it is a gameplay edit, so it fits in any batch. Verify it with `play_sim`, or with
`capture {"views": [{"player": true}], "hud": true}`, which previews it at the player start while editing
(`"hud": false` hides it). The minimap is rendered from the map itself (terrain, paths, water, buildings and
trees seen from above), so it follows the terrain, layout and foliage batches by itself. Collectibles, goals
and waypoints are live markers on it. A goal's `message` appears as a large banner when the player arrives;
collectible and trigger messages appear as toasts.

## The rule that saves the project

**A successful tool call doesn't mean the level is right.** Look at the editor after every batch, and never
change terrain, lighting and character in one prompt.

DAYFALL enforces both:

- `batch_end` refuses to close a batch while there are edits nobody has looked at. The agent has to call
  `capture`, inspect the images and fix what is wrong first.
- A batch that has changed terrain cannot change lighting or the character, and so on. The tool call fails
  with an explanation, and the agent closes the batch and opens a new one.
- Every edit can be undone (`undo`, Ctrl+Z in the editor), and `batch_end` saves the map.

The human and the agent share a selection. Clicking an object in the editor window selects it (Shift adds, Esc
clears) and draws its box; the agent reads it with `editor_state`, so "make this one bigger" or "put a lantern
here" just works. The other way round, `editor_select {ids, frame: true}` highlights items and moves the editor
camera to them, and `capture {selection: true}` shows the highlight in the agent's own images.

`world_check` finds what is easy to miss in a capture: floating or buried objects, duplicates, objects standing on
paths, collectibles inside walls, a player start inside a building, scatter rules that placed nothing. Run it
before `batch_end`; `world_diff` lists what the batch changed for its notes.

Run with `--no-rules` only for scripted rebuilds you have already verified.

## Prompting tips

- Name things: "house_1" is easier to fix later than "box_17". Tags group things ("tags": ["town"]).
- Ask for a capture of a specific place: "orbit the town square from the south-east at 40 m".
- Positions with two numbers sit on the ground and follow later terrain edits; give three numbers only for
  floating things or things on top of other objects.
- Scale: the mannequin is 1.8 m tall; doors are about 2.1 m, ceilings 2.8 to 3.5 m.
- Big changes in small steps: block out, test, then replace with art.
