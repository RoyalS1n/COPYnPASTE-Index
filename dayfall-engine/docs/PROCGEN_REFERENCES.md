# Procedural-generation references: what was found and where it fits in DAYFALL

The user supplied two link lists on 2026-10-07: 19 r/proceduralgeneration posts and 14 C++ GitHub repositories. Three
research agents reviewed them, and the lead session checked their licence claims and implemented the strongest
ideas. This page is the result, for anyone continuing the work.

**Licence rule:** DAYFALL may take **code** only under MIT, BSD, Apache-2.0, zlib or CC0, with attribution in
`dayfall-engine/THIRD_PARTY.md`. From GPL/LGPL code, or code with no licence, take **ideas only**: reimplement from the
paper or article the code cites, never from the code.

**Status key:**
- **DONE**: in the engine (commit `2098a4f` unless noted).
- **TASK A / TASK B**: briefed in `HANDOFF.md`.
- **NEXT**: a good candidate, not started.
- **SKIP**: not worth it now.

## 1. GitHub repositories (all cloned and read; licences checked against the files)

| Repo | Licence | What it really is | Quality | Use in DAYFALL | Status |
|---|---|---|---|---|---|
| [Adrian104/Dungeon-Generator](https://github.com/Adrian104/Dungeon-Generator) ("dgen") | **MIT** © 2023 Adrian Kulawik | BSP dungeon with sparse areas, L/T rooms, A* corridors routed in the gaps between cells, extra loops, seeded. Outputs geometry or a tilemap | Solid; standard library only; maintained | **Code can be ported.** A `dungeon_generate` tool building rooms from primitives or fortress-kit pieces | **TASK A** |
| [dpaulat/worldengine-cpp](https://github.com/dpaulat/worldengine-cpp) | **MIT** (its plate-tectonics submodule is LGPL-3: don't use it) | C++ port of WorldEngine: plates → temperature / rain → rivers → humidity → Holdridge biomes; thresholds by quantile | Real, with tests, but the river and flow code copies the original's bugs (it walks a straight line, wraps y by width, `CleanUpFlow` edits a copy) | Biome table plus quantile thresholds (`FindThresholdF`), e.g. "snow on the top 5%" at any height scale. River code: ideas only | **NEXT** (biome map) |
| [fegennari/3DWorld](https://github.com/fegennari/3DWorld) | GPL-3.0 | Large, mature engine: droplet erosion, terrain shaping, voxels, roads, cities | Good, but its erosion races between OpenMP threads (results vary run to run) | **Ideas only.** It cites Volynskov's 2011 erosion blog. Shaping ideas: glaciate (`h^3`), plateau, crater, cracks, billowy noise; automatic **bridges and tunnels** where a path's fill or cut is large | Erosion and shaping **DONE** from the papers; bridges and tunnels **NEXT** |
| [santipaprika/procedural-buildings](https://github.com/santipaprika/procedural-buildings) | GPL-3.0 | Small CGA-style split grammar (scope stack, T/R/S, Subdiv, weighted rules), outputs PLY | Student project: no Repeat or Comp, so no real facades; unseeded `rand()` | **Ideas only:** reimplement from Müller et al. 2006 | **TASK B** (`building_generate`) |
| [quote591/City-Generator](https://github.com/quote591/City-Generator) | none | Turtle L-system roads (`X→F[+X]F[-X]F[-X]F[+X]F`), removes crossing roads, joins dangling ends, adds highways, lots along roads, Kenney OBJ buildings | Demo; O(n²); erases from a vector while iterating it (marked "BUG"); bundles de_dust2 and LearnOpenGL assets | **Ideas only:** pipeline shape for settlements | **TASK B** (`settlement_layout`) |
| [willigarneau/procedural-city](https://github.com/willigarneau/procedural-city) | none (the badge says MIT, the text says none) | 20×20 grid of random cubes | Toy | Nothing | SKIP |
| [Lucas-Vinicius-dev/Binary-Space-Partitioning-Dungeon-Generator](https://github.com/Lucas-Vinicius-dev/Binary-Space-Partitioning-Dungeon-Generator) | none | BSP rooms, L corridors, door detection, ASCII output | Clear teaching code with small bugs | Ideas only; dgen supersedes it | SKIP |
| [AaryaDevnani/ProceduralTreeGeneration](https://github.com/AaryaDevnani/ProceduralTreeGeneration) | none | 3D L-system and space colonisation, drawn as cylinders and leaf quads | Demo; not reproducible (`random_device`); undefined behaviour in its spatial hash; NaN axis bug | Ideas only → Runions et al. 2007 | **DONE** (`{"type":"tree"}`) |
| [SRombauts/UE4ProceduralMesh](https://github.com/SRombauts/UE4ProceduralMesh) | none (mesh component forked from Epic's EULA code) | UE 4.7 custom mesh, cube and lathe demos | Abandoned in 2016 | Idea: a **lathe** primitive (spin a profile `[[r,z],…]` around Z) for barrels, columns, domes | **NEXT** (half a day) |
| [lromerio/ProceduralTerrain](https://github.com/lromerio/ProceduralTerrain) | MIT | GPU ridged multifractal, tessellation, height/slope texture blend | 2017 course demo | Nothing new (DAYFALL already has ridged noise and warping) | SKIP |
| [Moneyl/World-Generator](https://github.com/Moneyl/World-Generator) | BSD-3 | Dwarf-Fortress-style 2D map: weather systems drop rain, a river walker | Archived by its author as "a big mess" | Nothing | SKIP |
| [LeoSery/ProceduralTerrainGeneration--UnrealEngine5-2024](https://github.com/LeoSery/ProceduralTerrainGeneration--UnrealEngine5-2024) | none | Endless chunked Perlin terrain on threads, chunk queue ordered by view direction | Student demo (a new RNG per gradient lookup; seams at chunk edges) | Ideas: **slope-damped fBm** (eroded look without simulation); chunk priority `dist²·(1−0.9·max(dot(view,dir),−0.5))` | fBm **NEXT**; chunks SKIP (maps are bounded) |
| [wyattmchalffey/Cavern](https://github.com/wyattmchalffey/Cavern) | none | README promises GPU marching-cubes caves; **the repo has no source** (only Voxel Plugin sample assets) | n/a | Nothing | SKIP |
| [pandeyp1426/WFCBiomeGenerator](https://github.com/pandeyp1426/WFCBiomeGenerator) | the licence file belongs to the SFML template, not the author | 48×48 simple-tiled WFC (bitmask adjacency, lowest entropy, restart on contradiction) | Small, correct core; toy scale; no weights | Ideas only; use [mxgmn/WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse) (MIT) as the reference instead | SKIP for now |

## 2. Reddit posts

**Caveat:** reddit.com was blocked from the cloud machine, so **no post was read directly**. Each was matched by its
title, the date implied by its ID, and web search. Four were matched with reasonable confidence; the rest are
unconfirmed. A local agent can open them and should check the "could not confirm" ones before relying on them.

| # | Post | What it is (best evidence) | Underlying sources and licence | Use in DAYFALL | Status |
|---|---|---|---|---|---|
| 1 | [fancy_rockpiles](https://www.reddit.com/r/proceduralgeneration/comments/1vcubxm/fancy_rockpiles/) | Could not confirm | Closest technique: Peytavie et al. 2009, "Procedural Generation of Rock Piles using Aperiodic Tiling" (paper) | `rock_pile` generator: drop library rocks with Jolt (already a dependency, MIT), settle them, bake into a prefab | **NEXT** |
| 2 | [procedural_scattering_of_natural_objects_blog](https://www.reddit.com/r/proceduralgeneration/comments/1ts4swk/procedural_scattering_of_natural_objects_blog/) | Very likely Newhead Studio's "Procedural Scattering of Natural Objects" (Unity, about May 2026; newheadstudio.com, blocked from the cloud) | Blog, no licence: ideas only | A Poisson footprint per object, rules that avoid each other, secondary objects around primaries, noise masks | Footprints and companions **DONE** (scatter `avoid_rules`, `footprint_m`, `companions`); noise mask **NEXT**. Watch out for US patent 8,219,517 (multi-class Poisson disk), which covers Wei's per-class radius method; the plain footprint-sum rule used here doesn't use it |
| 3 | [procedural_nature](https://www.reddit.com/r/proceduralgeneration/comments/1uaeud7/procedural_nature/) | Could not confirm | — | — | check locally |
| 4 | [crimson_tree_using_space_colonization_algorithm](https://www.reddit.com/r/proceduralgeneration/comments/1wbetyy/crimson_tree_using_space_colonization_algorithm/) | Post not confirmed; the technique is Runions et al. 2007 | [dsforza96/tree-gen](https://github.com/dsforza96/tree-gen) (**MIT**); jasonwebb's 2D experiments are CC BY-NC-SA (unusable) | Procedural trees | **DONE** (implemented from the paper) |
| 5 | [from_random_points_to_village_layout](https://www.reddit.com/r/proceduralgeneration/comments/1o25d1z/from_random_points_to_village_layout/) | Could not confirm | Related: [Radet5/little-villages](https://github.com/Radet5/little-villages) (no licence), [watabou/TownGeneratorOS](https://github.com/watabou/TownGeneratorOS) (GPL-3), Emilien et al. 2012 "Procedural Generation of Villages on Arbitrary Terrains" (paper) | Village layout: Poisson points on flat ground → Delaunay → minimum spanning tree + about 15% extra edges → A* paths → houses facing the roads | **TASK B** |
| 6 | [procedural_hydrology_meandering_rivers](https://www.reddit.com/r/proceduralgeneration/comments/18kesza/procedural_hydrology_meandering_rivers/) | Nick McDonald's article (12 Dec 2023), "Procedural Hydrology: Improvements and Meandering Rivers" (nickmcd.me) | [weigert/SimpleHydrology](https://github.com/weigert/SimpleHydrology): MIT per its README, but **no LICENSE file** (confirm before porting); successor soillib is LGPL-3 | Rivers that meander through a momentum term: particles carry discharge and momentum maps; the momentum bends the flow | **NEXT** (meander option) |
| 7 | [procedural_hydrology_article_source](https://www.reddit.com/r/proceduralgeneration/comments/g4hbib/procedural_hydrology_article_source/) | Nick McDonald's April 2020 "Procedural Hydrology" article and source | Same repo as #6 | Discharge map → wet paint along channels, trees removed where discharge > 0.3 | Partly **DONE** (rivers from flow); discharge paint **NEXT** |
| 8 | [simplified_artsy_marching_cube_voxel_terrain](https://www.reddit.com/r/proceduralgeneration/comments/1t23tx6/simplified_artsy_marching_cube_voxel_terrain/) | Could not confirm | Background: GPU Gems 3 ch. 1 (Geiss), [Transvoxel](https://transvoxel.org/) (Lengyel) | Caves and overhangs (DAYFALL is heightfield-only) | SKIP for now |
| 9 | [coursestutorials_on_procedural_generation_in_3d](https://www.reddit.com/r/proceduralgeneration/comments/mi004i/coursestutorials_on_procedural_generation_in_3d/) | A help thread (not confirmed) | See section 4 | Learning resources | — |
| 10 | [procedural_generation_noisemap_help](https://www.reddit.com/r/proceduralgeneration/comments/u1ey1f/procedural_generation_noisemap_help/) | A help thread (not confirmed) | — | — | SKIP |
| 11 | [chunk_loading_system_for_procedural_terrain](https://www.reddit.com/r/proceduralgeneration/comments/1o0bpqn/chunk_loading_system_for_procedural_terrain/) | Could not confirm | — | CDLOD already handles terrain LOD; maps are bounded | SKIP |
| 12 | [unusual_terrain_generation](https://www.reddit.com/r/proceduralgeneration/comments/ol23ui/unusual_terrain_generation/) | Could not confirm | — | — | check locally |
| 13 | [resources_for_terrain_creation](https://www.reddit.com/r/proceduralgeneration/comments/1uhx1pt/resources_for_terrain_creation/) | A help thread (not confirmed) | See section 4 | Learning resources | — |
| 14 | [how_do_i_properly_generate_procedural_terrain](https://www.reddit.com/r/proceduralgeneration/comments/1dhksiz/how_do_i_properly_generate_procedural_terrain/) | A help thread (not confirmed) | — | — | SKIP |
| 15 | [c_mesh_generation_in_unreal_engine](https://www.reddit.com/r/proceduralgeneration/comments/vsntfe/c_mesh_generation_in_unreal_engine/) | Probably about Unreal's ProceduralMeshComponent | — | Same idea as DAYFALL's MeshBuilder; nothing to take | SKIP |
| 16 | [procedural_tree_generator](https://www.reddit.com/r/proceduralgeneration/comments/1vjznay/procedural_tree_generator/) | Could not confirm | See #4 | Trees | **DONE** |
| 17 | [diving_into_graphics_programming_through_terrain](https://www.reddit.com/r/proceduralgeneration/comments/1ldicpn/diving_into_graphics_programming_through_terrain/) | Could not confirm (its June 2025 date overlaps Acerola's "Dirt Jam" terrain jam: a weak lead) | — | — | check locally |
| 18 | [my_procedural_opengl_terrain_in_4k](https://www.reddit.com/r/opengl/comments/1rm7sm0/my_procedural_opengl_terrain_in_4k/) (r/opengl) | Probably Andrea Buzzelli ("buzzelliart"), "OpenGL procedural mountains in 4K": fBm, tessellation, GPU particle erosion, snow placed by **accumulated sunlight** | No code found | **Snow by sun exposure**: add up a day of sunlight per sample with the terrain horizon, then paint snow on shaded high slopes | **NEXT** |
| 19 | [improved_procedurally_generated_terrain_opengl_c](https://www.reddit.com/r/proceduralgeneration/comments/r4yl6y/improved_procedurally_generated_terrain_opengl_c/) | Could not confirm | — | — | check locally |

## 3. What was built from all this (commit `2098a4f`, PR #1)

| Feature | Where | Based on |
|---|---|---|
| Procedural trees `{"type":"tree","species":"oak\|birch\|pine\|bush\|dead","seed":n}`: space colonisation, pipe-model radii, pine whorls, leaf cards, LODs, trunk collision; usable as scatter variants | `src/scene/TreeGen.*` | Runions, Lane & Prusinkiewicz 2007, "Modeling Trees with a Space Colonization Algorithm"; pipe model (Shinozaki et al.) |
| `terrain_erode` (hydraulic droplets plus thermal), `terrain_generate` `"erosion"` | `src/world/Terrain.cpp` (`Terrain::erode`) | Hans Beyer 2015, "Implementation of a method for hydraulic erosion" (TUM bachelor thesis); heights normalised to a mean slope of 0.06; edge fade; deposits spread with the erosion brush; 3×3 softening |
| `rivers_generate`: Priority-Flood + ε, D8 accumulation, channels traced to the sea, the edge or a confluence, written as ordinary rivers | `src/world/Hydrology.*` | Barnes, Lehman & Mulla 2014, "Priority-Flood: An Optimal Depression-Filling and Watershed-Labeling Algorithm" |
| Sculpt ops `terrace`, `redistribute` | `Terrain::sculpt` | 3DWorld shaping ideas, Red Blob Games "Making maps with noise functions" |
| Scatter `avoid_rules`, `footprint_m`, `companions` | `src/world/Scatter.cpp`, `src/world/SceneBuilder.cpp` | Newhead Studio scattering article (ideas) |

Tests: `dayfall-engine/tests/procedural.json`. Docs: `docs/MAP_FORMAT.md` (terrain, primitives, scatter sections).

## 4. Ranked next steps

1. **TASK A**: `dungeon_generate`, porting dgen (MIT). Brief in `HANDOFF.md`.
2. **TASK B**: `settlement_layout` + `building_generate`, from Parish & Müller 2001, Müller et al. 2006 and Wonka et al. 2003. Brief in `HANDOFF.md`.
3. **Meandering rivers**:
   - option A: SimpleHydrology's momentum and discharge particles (confirm the MIT licence first);
   - option B: Howard–Knutson 1984 centreline migration (`R1 = −R0 + 2.5·Σ R0·e^(−α·s) / Σ e^(−α·s)`, then move each node along its normal, cut off loops into oxbow lakes) via [zsylvester/meanderpy](https://github.com/zsylvester/meanderpy) (Apache-2.0).

   Either would add a `meander` option to `water_set` / `rivers_generate`.
4. **Rock piles** with Jolt:
   - sizes in about a 1:4:12 ratio (large : medium : small), largest dropped first, friction 0.8–1.0, restitution ≤ 0.1;
   - run until every body sleeps, discard rocks that rolled further than 1.5 R, sink them 5–10%;
   - bake into a prefab with a fixed seed.

   Tool: `rock_pile {center, radius, count, rock_set, shape: cone|wall|scree, seed}`.
5. **Biome map** from worldengine-cpp (MIT):
   - temperature = base − lapse rate × altitude, plus aspect and noise;
   - moisture = rain noise + irrigation `Σ water / (ln(1+d)+1)`;
   - quantile bands → Holdridge table → per-sample biome that drives paint and a `biome:` scatter filter.
6. **Smaller items:**
   - slope-damped fBm as a `terrain_generate` `noise_style` (Inigo Quilez, ["More Noise"](https://iquilezles.org/articles/morenoise/));
   - lathe primitive;
   - suggesting bridges and tunnels in `path_set` (3DWorld's fill/cut rule):
     - bridge when fill > 1.5·width·length and fill > 2·cut, or when the path crosses water;
     - tunnel when cut > width·length and cut > 2·fill;
   - snow by sun exposure (post 18);
   - discharge → wet paint (post 7);
   - a scatter noise mask (post 2).

## 5. Learning resources found along the way

- **Hydrology and erosion:**
  - Nick McDonald's articles (nickmcd.me: "Procedural Hydrology", 2020; "…Meandering Rivers", 2023);
  - [SebLague/Hydraulic-Erosion](https://github.com/SebLague/Hydraulic-Erosion) (MIT) and his "Coding Adventure: Hydraulic Erosion" video;
  - Beyer 2015 thesis;
  - Barnes et al. 2014 (Priority-Flood);
  - [redblobgames/mapgen4](https://github.com/redblobgames/mapgen4) (Apache-2.0: the same flow tree on triangles).
- **Survey:** Galin et al. 2019, "A Review of Digital Terrain Modeling" (Eurographics STAR).
- **Plants:** Runions et al. 2007; Prusinkiewicz & Lindenmayer, *The Algorithmic Beauty of Plants* (free PDF at algorithmicbotany.org).
- **Maps and noise:**
  - Red Blob Games, ["Making maps with noise functions"](https://www.redblobgames.com/maps/terrain-from-noise/);
  - Catlike Coding's Pseudorandom Noise and Procedural Meshes series;
  - the free book *Procedural Content Generation in Games* ([pcgbook.com](https://www.pcgbook.com/)).
- **Voxels and chunks:** GPU Gems 3 chapter 1 (Geiss); [Transvoxel](https://transvoxel.org/) (Lengyel); Sebastian Lague's "Procedural Landmass Generation" series.
- **Cities and villages:** Parish & Müller 2001; Emilien et al. 2012; Müller et al. 2006 (CGA shape); Wonka et al. 2003.
- **WFC:** [mxgmn/WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse) (MIT).
- **Rock piles:** Peytavie et al. 2009; [JoltPhysics](https://github.com/jrouwe/JoltPhysics) (MIT).
