# Map format

A map is a folder:

```
maps/<name>/
  map.json             the world document (this page)
  terrain/height.png   base heights: 16-bit greyscale PNG, samples_per_side square, north at the top;
                       0 and 65535 map to terrain.height_range_m
  terrain/paint_a.png  painted layers dirt, rock, snow, wet (RGBA8)
  terrain/paint_b.png  painted layers dry, grass and 2 reserved (RGBA8)
  assets/              models imported with asset_import
  captures/            screenshots taken by agents and F12 (not committed)
```

World space: metres, Z up, X east, Y north. `yaw_deg` turns counter-clockwise seen from above (0 = east,
90 = north). A position `[x, y]` means "on the ground" and follows later terrain edits; `[x, y, z]` is absolute.

The editor writes `map.json` compactly and with sorted keys, so diffs stay small. Everything below is optional
except `format`.

## Top level

| key | meaning |
|---|---|
| `format` | always `"dayfall-map"` |
| `version` | 2 |
| `name` | display name |
| `environment` | sun, sky, haze, clouds, tone mapping, wind, water: see below |
| `terrain` | heightfield settings (the heights are in the binary files) |
| `paths` | roads and trails that flatten and paint the terrain |
| `objects` | placed meshes and primitives |
| `scatter` | procedural placement rules (forests, grass, rocks) |
| `entities` | gameplay objects: collectibles, goals, triggers |
| `lights` | point lights |
| `materials` | material definitions (override built-ins of the same name) |
| `meshes` | the map's mesh library (imported models) |
| `player` | character settings |
| `player_start` | `{"position": [x, y(, z)], "yaw_deg": 90}` |
| `hud` | the in-game HUD: minimap, counters, messages |
| `cameras` | named viewpoints `{"position", "target", "vfov_deg"}` |
| `routes` | named test routes `{"points": [[x, y], ...]}` |
| `instance_files` | large instance sets in binary files (ported maps) |
| `gltf_scenes` | whole glTF scenes placed as they are (levels exported from Unreal or Blender) |

## environment

```json
"environment": {
  "preset": "golden_hour",
  "time_of_day": 17.5,
  "sun": {"elevation_deg": 14, "azimuth_deg": -163, "strength": 5.5, "color": [1, 0.7, 0.42], "angle_deg": 0.6},
  "sky": {"strength": 0.1, "aerosol_density": 1.6},
  "haze": {"color_near": [0.62, 0.52, 0.42], "color_far": [0.48, 0.47, 0.5], "amount": 0.72, "start_m": 30,
           "depth_m": 6500, "height_fog_density": 0, "height_fog_falloff": 0.3, "height_fog_base_m": 0},
  "clouds": {"enabled": true, "coverage": 0.55, "height_m": 1500, "color": [1, 0.7, 0.5]},
  "tonemap": {"exposure_ev": 0.9, "contrast": 1.15, "saturation": 1.05, "vignette": 0.25},
  "wind": {"direction": [0.6, 0.8], "strength": 1},
  "water": {"enabled": true, "level_m": 2.0, "plane": true},
  "shadow_distance_m": 250,
  "sky_occlusion": {"enabled": true, "cell_m": 0.5, "rays": 48, "strength": 1.0}
}
```

`water.plane: false` keeps the water level (wet ground, the walk test's water events) but draws no sea plane;
use it when ponds and rivers are their own meshes. The preset applies first; any field given overrides it.
Presets: `golden_hour`, `serene`, `noon`, `misty_morning`, `dusk`, `overcast`. `sky` also takes `model`
(`multiple`, the default: multiple scattering like Blender's MULTIPLE_SCATTERING sky; `single` for the older sky)
and `ground_albedo` (0.3). Clouds follow the Blender worlds' cloud plane: lit through by the sun, hazed with distance. `time_of_day` (hours) moves the sun along a simple day arc. Sun azimuth:
0 = the sun is north, 90 = east.

`sky_occlusion` (on by default; `false` turns it off) darkens the sky light under roofs, inside walls and near
structures: the engine bakes how much of the sky every point around buildings, ruins and cliffs can see into
small 3D volumes (at most 16, 4 M cells and 32 MB together) when the scene is built, and lighting scales the
ambient sky light and sky reflections by it. The sun keeps its shadows; point lights are not affected. Open terrain
has no volume. `cell_m` (0.25-4, default 0.5) is the preferred cell size: large structures get coarser cells to
fit the budget, `rays` (8-256, default 48) the directions traced from every cell over the whole sphere,
`strength` (0-1) scales the effect. Static instances with opaque surfaces at least 1.5 m across occlude
(vegetation, grass and water do not); the volumes are retraced only when they or the ground under them change.
`project_info` reports the volumes under `build.sky_occlusion`.

## terrain

```json
"terrain": {"samples_per_side": 513, "spacing_m": 1.0, "origin": [-256, -256], "seed": 7,
            "snowline_m": 380, "rock_slope_deg": 40, "dry_amount": 0.5, "material": "terrain",
            "horizon": {"enabled": true, "radius_m": 6000, "height_m": 300, "roughness": 0.6},
            "height_file": "terrain/height.png", "height_range_m": [-12.5, 140.2],
            "paint_files": ["terrain/paint_a.png", "terrain/paint_b.png"], "png_rows": "north_first"}
```

The editor writes the file keys when it saves. `png_rows` says which way the image rows run: `north_first` (any
image editor or exporter) or `south_first` (maps saved before it existed; the default when missing). Older maps
with raw `height.f32` / `paint.u8` still load and are converted to PNG on the next save. Bring in an outside
heightmap with the `terrain_import` tool rather than by hand.

Rock appears on slopes steeper than `rock_slope_deg`, snow above `snowline_m`, wet ground near the water level;
painted layers add to these. `horizon` is the distant land around the map.

## paths

```json
{"id": "main_path", "points": [[0, -200], [8, -120], [0, 0]], "width_m": 3.5, "style": "dirt",
 "carve_m": 0.12, "flatten": 0.85, "falloff_m": 3.6, "smooth_m": 16, "max_grade": 0.15}
```

The points are smoothed into a spline. Under the path the ground is levelled across its width, smoothed along
its length (`smooth_m`), optionally limited to `max_grade`, carved by `carve_m` and painted as earth.
`style: "lane"` draws two wheel ruts. Paths never change the stored heights: move or delete one and the ground
comes back.

## objects

```json
{"id": "house_1", "mesh": "cottage", "position": [-12, 150], "yaw_deg": 10, "scale": 1.0,
 "collision": "auto", "shadow": true, "tags": ["town"]}
{"id": "wall_1", "mesh": {"type": "box", "size": [8, 0.6, 3], "material": "stone"}, "position": [4, 140]}
```

| key | meaning |
|---|---|
| `mesh` | a mesh name (map `meshes`, the content library) or a primitive spec |
| `position` | `[x, y]` on the ground (+ `offset_z`), or `[x, y, z]` |
| `yaw_deg`, `pitch_deg`, `roll_deg` or `rotation` | orientation (`rotation` is a quaternion `[x, y, z, w]`) |
| `align_to_ground` | tilt to the terrain normal |
| `scale` | a number, or per-axis `[x, y, z]` in the mesh's own axes (stretched kit pieces) |
| `materials` | swap materials on this object only: `{"mesh material": "replacement"}` |
| `collision` | `auto`, `none`, `mesh`, `convex`, `box`, `cylinder` (`{"type": "cylinder", "radius", "height"}`), `sphere` |
| `footprint_m` | radius kept clear of scatter (default: automatic for objects under 50 m; `false` for none) |
| `shadow`, `cull_distance_m`, `hidden`, `tags` | |

Primitives: `box` (size), `plane` (size, subdivisions), `cylinder`, `cone`, `sphere`, `capsule` (radius,
height), `ramp`, `stairs` (size, steps), `gem` (size); all take `material`. Their origin is the bottom centre.

## scatter

```json
{"id": "forest_west", "meshes": [{"mesh": "conifer_a", "weight": 2}, {"mesh": "broadleaf_a", "weight": 1}],
 "area": {"rect": {"center": [-120, 0], "size": [140, 400]}, "falloff": 30},
 "density_per_100m2": 1.2, "min_spacing_m": 3, "clumping": 0.4, "scale": [0.8, 1.3],
 "slope_deg": [0, 32], "height_m": [-1000, 400], "avoid_paths_m": 4, "avoid_objects_m": 3,
 "collision": {"type": "cylinder", "radius": 0.35, "height": 6}, "seed": 3}
```

Other keys: `tilt_deg`, `align_to_slope`, `avoid_water`, `max_rock`, `max_path`, `sink_m`, `clump_scale_m`,
`yaw_deg` (fixed yaw), `shadow`, `cull_distance_m`, `max_instances`, `avoid_ruts` (keep out of the wheel ruts of
`lane` paths but allow the strip between them). Results are deterministic for a given rule and terrain.

Areas (used by scatter, sculpting, painting and deletion):
`{"circle": {"center": [x, y], "radius": r}}`, `{"rect": {"center": [x, y], "size": [w, h], "yaw_deg": a}}`,
`{"polygon": [[x, y], ...]}`, `{"line": [[x, y], ...], "width": w}`, `{"union": [area, area, ...]}` or `"all"`,
plus `"falloff"` in metres.

## entities

```json
{"id": "gem_1", "type": "collectible", "position": [5, -120], "color": [1, 0.72, 0.28], "size_m": 0.6,
 "hover_m": 1.1, "light": true, "radius_m": 1.3}
{"id": "town_goal", "type": "goal", "position": [1, 160], "radius_m": 3, "message": "Welcome to the town"}
```

Types: `collectible`, `goal`, `trigger`, `spawn`, `waypoint`.

`message` is shown in play: a toast when a collectible is picked up (besides "Collected n / N") or the player
enters a trigger (its id when empty), a large banner when the player reaches a goal ("You made it!" when
empty). Collectibles, goals and waypoints also appear on the minimap.

## hud

The in-game HUD, drawn over the final image while playing (P in the editor, `--play`, `play_sim` captures,
`capture` while playing); never over the editor's fly camera. Every key is optional (`hud_set` edits it):

```json
"hud": {"enabled": true, "scale": 1.0,
        "minimap": {"enabled": true, "corner": "top_right", "size_px": 220, "range_m": 120, "north_up": true,
                    "shape": "round", "route": "loop"},
        "counters": true, "timer": false, "messages": true}
```

| key | meaning |
|---|---|
| `enabled` | draw the HUD at all (default true) |
| `scale` | multiplies every size (0.25 to 4). Sizes are for 1080p and follow the output height, a little more than proportionally below 1080p so 960 x 540 captures stay legible |
| `minimap` | `false` hides it; else the keys below |
| `minimap.corner` | `top_right` (default), `top_left`, `bottom_right`, `bottom_left` |
| `minimap.size_px` | diameter at 1080p (64 to 1024, default 220) |
| `minimap.range_m` | metres from the player to the edge (default 120) |
| `minimap.north_up` | true: north at the top (default); false: the view direction points up |
| `minimap.shape` | `round` (default) or `square` |
| `minimap.route` | a named route (`routes`) drawn as a dotted trail |
| `counters` | collectibles found / total, top left (top right when the minimap is there) |
| `timer` | play time under the counter (default false) |
| `messages` | toasts at the top centre (pickups, entity messages) and a large banner when a goal is reached |

The minimap is the map seen from straight above in neutral light (a high north-west sun, no haze or clouds),
rendered when the HUD first needs it and again only when something visible from above changes (terrain,
paths, objects, scatter, materials, water level). Paths are tinted tan and ground below the water level blue.
Instances seen from 250 m away are drawn (trees, rocks, buildings; not grass), largest cull distance first
within a triangle budget (much smaller on software rasterisers such as llvmpipe); the ones over the budget
appear as dots (foliage dark green, the rest grey). Live markers: the player arrow (facing) with a view cone,
collectibles not yet taken (in their colour), goals (rings) and `waypoint` entities (white dots); markers
beyond the range sit on the rim.

## lights

```json
{"id": "lantern_1", "position": [3, 150], "offset_z": 2.4, "color": [1, 0.75, 0.45], "intensity": 12, "range_m": 10}
```

## materials

```json
"materials": {
  "stone": {"model": "courses", "color_a": [0.25, 0.215, 0.17], "color_b": [0.155, 0.13, 0.094],
            "block_width": 0.95, "block_height": 0.44, "mortar_width": 0.014, "grime": 0.5},
  "door": {"model": "lit", "base_color": [0.3, 0.18, 0.1], "roughness": 0.7, "textures": {"base": "assets/door.png"}}
}
```

Models: `lit` (glTF metallic-roughness; `base_color`, `roughness`, `metallic`, `emissive`,
`emissive_strength`, `textures` {base, normal, orm, emissive}), `courses` (procedural stone, brick, tiles,
planks), `terrain`, `foliage`, `grass`, `water`, `emissive`, `unlit`. Common keys: `two_sided`,
`alpha` (`opaque`, `mask`, `blend`), `alpha_cutoff`, `vertex_color`, `wind` {strength, height, speed},
`normal_convention` (`opengl`, the default for glTF and Blender, or `directx` for textures from Unreal).
Built-in materials: see `catalog`.

## meshes

```json
"meshes": {
  "ranger_house": {"file": "assets/ranger_house.glb", "category": "buildings", "front": "-y",
                   "lods": [{"ratio": 0.3, "distance_m": 80}], "lod0_distance_m": 35,
                   "collision": "mesh", "import_scale": 1.0, "ground_origin": true}
}
```

`front` is the side that `face_towards` turns towards a target (`-y` for Blender exports).

## instance_files

Large sets of one mesh (foliage, rocks, repeated kit pieces) in a binary file next to `map.json`:

```json
"instance_files": [
  {"id": "pines", "file": "instances/conifer_a.bin", "mesh": "conifer_a", "collision": "cylinder",
   "shadow": true, "cull_distance_m": 900},
  {"id": "walls", "file": "instances/sm_wall.bin", "mesh": "SM_Wall", "layout": "pos_quat_scale3",
   "collision": "mesh", "materials": {"M_Stone": "M_Stone_Mossy"}}
]
```

Little-endian float32 records. `layout` `pos_scale_quat` (default, 32 bytes): position xyz, uniform scale,
quaternion xyzw. `pos_quat_scale3` (40 bytes): position xyz, quaternion xyzw, scale xyz. Positions are absolute
(they do not follow terrain edits). `materials` works as on objects.

## gltf_scenes

```json
"gltf_scenes": [{"id": "castle", "file": "assets/castle_level.glb", "collision": "mesh", "shadow": true}]
```

Every node with a mesh in the file is placed where the file puts it (glTF's Y up is converted; per-axis scale
and `EXT_mesh_gpu_instancing` are kept), and its point lights are added. Use it for a level exported in one
piece; split it into `meshes` + `objects` / `instance_files` when agents need to edit the parts.

## player

```json
"player": {"character": "mannequin", "camera": "third_person", "walk_speed": 4.2, "run_speed": 7.6,
           "jump_height_m": 1.25, "height_m": 1.8, "radius_m": 0.32, "max_slope_deg": 48,
           "step_height_m": 0.4, "camera_distance_m": 4.2}
```

`character` is `"mannequin"` (the default jointed figure, animated procedurally) or the name of a rigged mesh: a
map `meshes` entry or a content-library mesh whose glTF has a skin (`JOINTS_0` / `WEIGHTS_0`, inverse bind
matrices, a joint hierarchy). A rigged character plays its animation clips:

```json
"player": {"character": "hero",
           "animations": {"idle": "Idle", "walk": "Walk", "run": {"clip": "Run", "speed_mps": 5.2},
                          "jump": "Jump", "fall": "Fall", "land": "Land"},
           "animation_files": ["assets/hero_anims.glb"], "character_scale": 1.0, "character_yaw_offset_deg": 0,
           "root_motion": "keep", "animation_blend_s": 0.2}
```

| key | meaning |
|---|---|
| `animations` | state -> clip name, or `{"clip", "speed_mps"}`. States: `idle`, `walk`, `run`, `jump` (once from the take-off, then holds), `fall` (loops while falling), `land` (once on touch-down). A state left out takes the clip whose name contains it (the shortest such name), so clips named Idle, Walk, Run ... map themselves. Missing clips fall back: walk and run use each other, jump and fall each other, land is skipped, idle holds the rest pose. |
| `animation_files` | more GLBs with clips for the same skeleton, relative to the map (Mixamo style, one clip per file). Joints match by name, ignoring a `prefix:` and case; a clip named `mixamo.com` takes the file name. Only the root bone's translation is taken from these files, so the character keeps its own bone lengths. |
| `character_scale` | model scale (default 1). The physics capsule stays `height_m` / `radius_m`. |
| `character_yaw_offset_deg` | turns the model about Z when it does not look where it walks (default 0) |
| `root_motion` | `keep` (default, for in-place clips) or `strip`: remove the root bone's horizontal travel over each clip, so a clip that walks forward plays in place (sway and bob stay) |
| `animation_blend_s` | crossfade between states, seconds (default 0.2; jump and land use at most 0.12, fall at least 0.25) |

Walk and run play at the ground speed divided by the clip's `speed_mps` (0.5x to 2x). `speed_mps` comes from
`animations`, else from the glTF animation's `extras` (`{"speed_mps": 1.7}`: in Blender a custom property on the
action, exported with custom properties on), else from the root bone's travel per second, else `walk_speed` /
`run_speed`. The run clip takes over above halfway between the walk and run clip speeds, so the clip follows the
actual speed rather than the Shift key.

The character's mesh entry: `front` (default `-y`, the glTF convention: the model looks along glTF +Z, as Blender
and Mixamo export) is turned to the walking direction; `import_scale`, `import_yaw_deg` and `ground_origin` work as
for other meshes (the model's origin belongs between its feet); `animation_files` lists clip files relative to the
entry (how a content-library character carries its clips); with `"category": "characters"` the `catalog` lists the
clips. Skinning runs on the CPU every frame (the 4 strongest influences per vertex) into the character's own range
of the vertex buffer; non-skinned meshes in the file follow their nearest joint.
