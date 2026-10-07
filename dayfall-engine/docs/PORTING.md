# Porting maps into DAYFALL

Two sources feed the engine: the Blender reference worlds (already ported: `maps/golden_valley`,
`maps/serene_meadow`, the content library) and the Unreal project FloatingIslet (to be ported on the developer's
machine by a local agent, with the tools in `tools/unreal/`). This page covers both. The last section is a
prompt to hand to a local agent.

## Coordinates

| | Unreal | Blender | DAYFALL |
|---|---|---|---|
| units | centimetres | metres | metres |
| up | Z | Z | Z |
| handedness | left (X forward, Y right) | right | right (X east, Y north) |
| point | (x, y, z) | (x, y, z) | Unreal (x, -y, z) / 100; Blender unchanged |
| rotation | quaternion (x, y, z, w) | | Unreal (-x, y, -z, w) |
| scale | (sx, sy, sz) | | unchanged (per-axis scale is supported) |
| yaw | degrees, X towards Y | | Unreal -yaw; DAYFALL yaw is counter-clockwise from +X |
| glTF | exporter writes (x, z, y) * 0.01 | exporter writes (x, z, -y) | read back as Z up |

Light units: the engine's sun strength is about 5 with point lights on the same relative scale (taken from the
Blender worlds). The Unreal exporter keeps each local light's ratio to the sun (candela / lux), so the balance of
the Unreal level survives; overall brightness is then tuned in DAYFALL with `environment_set`.

## The Blender reference worlds

Run with a Python that has `bpy` (Blender 5.2's own Python, or a venv with the `bpy` module), from
`reference-world/blender/`:

```
python export_content.py                       # the content library: dayfall-engine/content/
python export_dayfall_map.py --preset serene_meadow
python export_dayfall_map.py --preset golden_valley
```

They rebuild the worlds with the same generators as the Blender scenes, so a change to `worldgen/` is ported by
running them again (the maps are overwritten; edits made in DAYFALL since then are lost).

## Whole scene files (Unity, Blender, Unreal scene exports)

`tools/fbx_scene_to_dayfall.py` (Blender) turns one scene file (FBX, glTF, .blend) into a map: one GLB per unique
mesh (LOD0 of `*_LOD0/1/2` siblings; the engine builds its own LODs), every placement in instance files, materials
guessed from their names or taken from `--materials file.json`, textures matched by name from `--textures DIR`, and
the sun. Meshes are baked to metres and Z up (exporters often keep centimetres and a -90 degree X turn on every
placement). Unity's terrain and Unreal landscapes are not part of an FBX: with `--ground auto` it fits a ground
under the objects' feet, which is right where things stand and a guess elsewhere (no lakes or riverbeds). For the
real ground, export the terrain heightmap (Unity: Terrain > Export Raw, 16-bit; Unreal: through
`export_level_to_dayfall.py`) and load it with `terrain_import`.

```
blender -b --factory-startup --python dayfall-engine/tools/fbx_scene_to_dayfall.py -- Scene.fbx --out dayfall-engine/maps/<name> --textures <folder with the scene's textures>
```

Check the licence of third-party packs before committing their meshes and textures to this repository.

## The Unreal level

### What the tools do

`tools/unreal/export_level_to_dayfall.py` runs inside the Unreal Editor and only reads the level. It never saves,
changes or creates Unreal assets and enables no plugins. It writes a map folder:

| Unreal | DAYFALL |
|---|---|
| static meshes | `assets/<mesh>.glb` (glTF Exporter plugin) or `_fbx/<mesh>.fbx` + `fbx_to_glb.py` |
| materials | `materials` entries: base colour / normal / ORM textures exported to `textures/`, blend mode, two-sided, foliage and water guessed from the shading model and names, `normal_convention: "directx"` |
| Static Mesh Actors and other mesh components | `objects` (absolute position, rotation, per-axis scale, material overrides, collision from the Pawn channel) |
| 16 or more identical placed meshes, Instanced / Hierarchical Instanced / foliage components | `instance_files` |
| Landscape | `terrain` heightfield traced at the landscape's own resolution (16-bit PNG) |
| point, spot and rect lights | `lights` (spot and rect lights become point lights) |
| directional light | `environment.sun` (direction, colour) and a preset by sun height |
| camera actors | `cameras` (use them to compare views) |
| Player Start | `player_start` (feet on the floor, facing the same way) |
| fog, sky light, post process | not converted; the fog values are in the report |
| skeletal meshes, Niagara, decals, water bodies, landscape layers and grass types, Blueprint logic, audio | not exported; listed in the report |

Everything it could not handle goes to `unreal_export_report.json`: warnings, skipped actor classes with
counts, materials and their unread textures, heavy meshes (with Nanite flags), mirrored instances, spot lights
converted to points, and the counts of what was written.

The conversion maths is in `dayfall_convert.py`. Tests (plain Python, no Unreal):

```
python tools/unreal/test_dayfall_convert.py --engine    # maths, then a fake export loaded in bin/dayfall
python tools/unreal/test_export_mock.py --engine        # the whole exporter against a mock unreal module
python tools/unreal/test_fbx_to_glb.py                  # needs bpy: FBX conversion, including misread axes
```

Run them after any change to these scripts. They prove the script logic and the engine side; they cannot prove
what a given Unreal version's Python API returns, so the first real export must be checked as below.

### Steps

1. **Build the engine** on the machine (README: `cmake --preset windows`, `cmake --build build/windows --config Release`).
   The engine was developed and run on Linux; its Windows build compiles and links with MinGW (GCC 13) but has
   not been built with Visual Studio or run on Windows yet. If MSVC reports errors, they will be small (a header,
   a conversion): fix them in the engine and note them in the change log.
2. **Export.** In the Unreal Editor, open the level and run in the Output Log (Python):
   ```
   py "C:/<repo>/dayfall-engine/tools/unreal/export_level_to_dayfall.py" --out "C:/<repo>/dayfall-engine/maps/islet_fortress" --name "Islet Fortress"
   ```
   or without the editor window:
   ```
   "C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "C:/Users/keega/Documents/Unreal Projects/FloatingIslet/FloatingIslet.uproject" -run=pythonscript -script="C:/<repo>/dayfall-engine/tools/unreal/export_level_to_dayfall.py --map /Game/ClaudeFortress/Maps/L_Islet_Fortress --out C:/<repo>/dayfall-engine/maps/islet_fortress"
   ```
   Options: `--only-selected` (try a few actors first), `--terrain-actors "RW Terrain*"` (trace mesh terrain
   into a heightfield instead of keeping it as meshes; not for floating islands, whose undersides a heightfield
   cannot hold), `--instance-threshold`, `--skip-meshes` (re-export placements, keep the assets), `--format fbx`.
   World Partition levels export only the loaded regions: load them all first.
3. **If the report says `"mesh_format": "fbx"`** (the glTF Exporter plugin is off; enabling it is the user's
   call), convert with Blender:
   ```
   "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe" -b --factory-startup --python dayfall-engine/tools/unreal/fbx_to_glb.py -- dayfall-engine/maps/islet_fortress/_fbx/manifest.json
   ```
   Check `_fbx/convert_report.json` (every mesh `box_error` near 0), then delete `_fbx/`.
4. **Read `unreal_export_report.json`** before looking at anything.
5. **First look.** `bin\dayfall.exe maps\islet_fortress --capture all --out captures_first` renders every camera.
   Render the same cameras in Unreal (the project's own render scripts) and compare side by side.
6. **Check, in this order** (each is one batch through the editor's MCP tools; see AGENT_WORKFLOW.md):
   - Orientation and placement: an asymmetric landmark (a gate, a tower, a sign) is on the same side and
     facing the same way; nothing floats or sinks. A wrong mirror shows everywhere at once and is a conversion
     bug: fix `dayfall_convert.py` and its tests, not the map.
   - Scale: doors about 2 m, the player's height against stairs and doorways.
   - Collision: `walk_test` along a route set with `route_set` through gates, stairs and bridges; `play_sim`
     for jumps.
   - Materials: textures present; bumps lit from the correct side (if they look inverted, set the material's
     `normal_convention` to `opengl`); foliage cut-outs; water. Fix with `material_set`.
   - Lighting: sun direction and colour against Unreal; overall exposure and the point lights' brightness with
     `environment_set` / `light_set`.
   - Performance: `stats`; meshes in `heavy_meshes` (often Nanite source meshes with millions of triangles) get
     `lods` in their `meshes` entry, or a lighter version exported from Blender.
7. **Rebuild what did not come over** with DAYFALL tools: grass and ground cover as `scatter` rules, landscape
   paint with `terrain_paint`, fog and sky with `environment_set`, gameplay with `entity_add`.
8. Once ported, the DAYFALL map is the source of truth. A later re-export goes to a new folder and is merged
   by hand; it would overwrite edits made in DAYFALL.

## Prompt for a local agent

> Port the current DAYFALL Unreal level into the DAYFALL engine. First read
> `C:\Users\keega\Documents\Unreal Projects\FloatingIslet\AI_README.md` (project rules: don't enable plugins,
> change the startup map or project settings without asking), then in this repository `dayfall-engine/AGENTS.md`,
> `dayfall-engine/docs/PORTING.md` and `dayfall-engine/docs/AGENT_WORKFLOW.md`.
> Build the engine, run `tools/unreal/export_level_to_dayfall.py` on `/Game/ClaudeFortress/Maps/L_Islet_Fortress`
> (or the level I name) into `dayfall-engine/maps/islet_fortress`, convert FBX with Blender if the report says
> so, read `unreal_export_report.json`, then work through the checks in PORTING.md step 6 in separate batches
> with the dayfall MCP tools, comparing captures against Unreal renders of the same cameras. Fix conversion bugs
> in the scripts (with their tests), not by hand-editing the map. Rebuild what the exporter could not bring over
> (grass, landscape paint, fog) with DAYFALL tools. Report what matches, what does not and why, with captures.
> The repository's `changes.md` says to merge its entries into AI_README.md's Change summaries and delete it:
> do that first, then log every change of yours there.
