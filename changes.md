# Changes (temporary; merge into the local change log when complete)

**When this work is complete and on your machine, merge every entry below into the "Change summaries" section
at the bottom of `C:\Users\keega\Documents\Unreal Projects\FloatingIslet\AI_README.md` (newest first, same
format), then delete this file.** There must be only one tracking file; this one exists only because that file is
not reachable from the cloud session that made these changes.

Until then, every change adds one entry here, at the top, in the same commit as the change. The format matches
AI_README's Change summaries exactly so the entries can be pasted in unchanged:

`- YYYY-MM-DD [Agent] what changed — why — where`

Never edit or delete another agent's entry; add a new one that corrects it.

## Change summaries
- 2026-10-06 [Claude] Reformatted this log to match AI_README's Change summaries and added the instruction to merge it into AI_README.md and delete it once the work is complete — the user wants a single tracking file — `changes.md`
- 2026-10-06 [Claude] Started this change log, backfilled from the git history — So every change is recorded in one place — `changes.md`
- 2026-10-06 [Claude] Added 1080p meadow previews of the lane and the barn — To show the meadow at full quality — `reference-world/previews/meadow_hero.jpg`, `meadow_barn.jpg`
- 2026-10-06 [Claude] Added a small, highly detailed marsh pond to the meadow: lobed shore, high-resolution terrain patch cut seamlessly into the main terrain, cattails, reeds, sedges, water lilies, boardwalk, fallen log, dead snag, stepping stones, duckweed water and wet-mud shading; Unreal patch, water mesh and materials — User request for a marsh — `reference-world/blender/worldgen/terrain.py`, `assets.py`, `materials.py`, `meadow.py`, `scatter.py`, `presets/serene_meadow.json`, `unreal/.../rw_build_level.py`
- 2026-10-06 [Claude] Loose capstones on the meadow ruins' broken wall tops — The column-built tops read as stair steps — `reference-world/blender/worldgen/meadow.py`
- 2026-10-06 [Claude] Added the serene meadow map: meadow terrain mode, two-track lane with per-pixel ruts, about 1.6 million grass clumps, weathered fences and dry-stone walls, and abandoned buildings (cottage, barn, chimney ruin, chapel, well, cart); per-preset Unreal folders and maps — User request for a meadow — `reference-world/blender/worldgen/meadow.py`, `terrain.py`, `scatter.py`, `presets/serene_meadow.json`, `unreal/...`
- 2026-10-06 [Claude] Documented the Unreal atmosphere settings — `reference-world/README.md`
- 2026-10-06 [Claude] Unreal atmosphere and grade: sun light shafts, fog scattering, a low valley mist layer, split-tone grade, wider auto exposure for interiors, higher Lumen quality — Quality pass — `unreal/.../rw_build_level.py`
- 2026-10-06 [Claude] Broadleaf trees get three distinct crown shapes and root flares — Crowns read as round blobs — `reference-world/blender/worldgen/assets.py`
- 2026-10-06 [Claude] Ivy on walls and towers, laid tables with candles in the great hall, tapestries and a rug in the keep — Quality pass — `reference-world/blender/worldgen/castle.py`, `materials.py`
- 2026-10-06 [Claude] Fence and 18 lantern posts along the castle approach road — Quality pass — `reference-world/blender/worldgen/castle.py`
- 2026-10-06 [Claude] Fixed a blue colour cast in interiors from the grade; blockier cliff fractures, large terrain relief, dirt and mud on stone floors — Found in preview renders — `reference-world/blender/worldgen/lighting.py`, `castle.py`, `materials.py`
- 2026-10-06 [Claude] Foliage shading carried into Unreal as vertex colour; Unreal terrain reads steep faces as rock — Parity with Blender — `reference-world/blender/worldgen/assets.py`, `unreal/.../rw_build_level.py`
- 2026-10-06 [Claude] Quality pass on foliage, rock, castle detail, clouds, haze and grade — User asked for more depth — `reference-world/blender/worldgen/*`
- 2026-10-06 [Claude] Lighter preview flythrough settings — To keep a CPU render to about an hour — `reference-world/blender/worldgen/config.py`
- 2026-10-06 [Claude] Added 1080p preview renders of the castle valley — `reference-world/previews/`
- 2026-10-06 [Claude] Torches on the wall-walks, braziers in tower rooms; persistent render data for animations — `reference-world/blender/worldgen/castle.py`, `lighting.py`
- 2026-10-06 [Claude] In-game HUD (centre dot, fading controls hint) and a `--stills` build option — `unreal/.../Source/ReferenceWorld/RWHUD.*`, `reference-world/blender/build_world.py`
- 2026-10-06 [Claude] Procedural Unreal materials (coursed stone, tiles, planks), foliage wind, water ripples, cull distances; castle doors, flags, banners and courtyard props — `unreal/.../rw_build_level.py`, `reference-world/blender/worldgen/castle.py`
- 2026-10-06 [Claude] Rebuilt the castle as a large walkable space with interiors, and made the Unreal map playable: first-person character and game mode in C++, collision, lights, player start, world bounds — User request for a playable map — `reference-world/blender/worldgen/castle.py`, `unreal/ReferenceWorld/Source/`, `rw_build_level.py`
- 2026-10-06 [Claude] Fixed the smoothstep zero-width guard for array edges — It crashed terrain mask generation — `reference-world/blender/worldgen/noise.py`
- 2026-10-06 [Claude] Added a procedural castle on a cliff, a polar cliff mesh, and more foliage and terrain detail — User request for a castle and more detail — `reference-world/blender/worldgen/castle.py`, `terrain.py`, `assets.py`, `scatter.py`
- 2026-10-06 [Claude] Plain string check for Unreal subobject add failures — Robustness — `unreal/.../rw_build_level.py`
- 2026-10-06 [Claude] Added the procedural Blender to Unreal reference world project: terrain, vegetation, materials, lighting, camera, exports, Unreal project and level script — User request for a UE/Blender world — `reference-world/`
- 2023-02-13 [Keegan] Original copy-and-paste index site — `index.html`
