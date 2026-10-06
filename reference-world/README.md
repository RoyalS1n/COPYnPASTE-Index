# Reference World: a procedural Blender → Unreal Engine 5 environment

A cinematic river-valley world built entirely from code. Blender generates
the terrain, vegetation, rocks, materials, lighting and camera move, renders
stills and a flythrough, and exports everything Unreal Engine 5 needs. A
Python editor script then rebuilds the same world in Unreal with Lumen,
Nanite, a physical sky, volumetric clouds and a matching Sequencer shot.

> **About the reference video.** This was built to match a reference clip
> on X (`x.com/rewind02/status/2107086047985258639`), but the build
> environment's network policy blocks x.com, so the clip could not be viewed.
> The default preset is therefore an assumption: a golden-hour alpine river
> valley. See [Matching the reference](#matching-the-reference) to retarget it
> in a few minutes from screenshots.

![hero](previews/hero.jpg)

| | |
|---|---|
| ![mid](previews/mid.jpg) | ![end](previews/end.jpg) |

## What gets generated

| Element | How |
|---|---|
| Terrain | 2.4 km heightfield, 1009² samples: domain-warped ridged multifractal mountains, a meandering valley, thermal erosion, multi-scale gully detail and a carved river channel |
| Distant ranges | 16 km low-resolution skirt of snow-capped ranges for layered depth; it continues the main terrain's edge seamlessly |
| Biomes | Slope, height and noise masks for grass, dry grass, rock, snow and wet ground. They drive the shader, the scatter and the Unreal weightmaps |
| Vegetation | 4 spruce variants (branch whorls plus needle-tuft cards), 3 broadleaf trees (about 5k leaf cards each), bushes, 3 grass-clump variants and wildflowers. All are original procedural meshes with outward custom normals, so canopies shade like soft volumes |
| Rocks | Displaced, plane-fractured boulders with cavity darkening and moss on upward faces |
| Scatter | About 9k trees, 4k bushes, 1.6k rocks and 420k grass clumps, placed deterministically from the masks. Geometry Nodes instances them in Blender and the same transforms go to Unreal |
| Lighting | Physical multiple-scattering sky, low golden sun at a calibrated sun-to-sky ratio, a cirrus cloud layer, and AgX colour management |
| Grade | Compositor: aerial perspective from the mist pass, sky re-inserted behind geometry, bloom, slight chromatic dispersion and a vignette |
| Camera | 32 mm cine camera, 10 s rising flight up the river, with depth of field |

## Quick start (Blender)

Blender 4.2+ or the `bpy` module both work. The pipeline was developed and
verified on **Blender 5.2.2** (`pip install bpy`, Python 3.13).

```bash
# fast iteration: 640x360 hero still in about 45 s on 4 CPU cores
python blender/build_world.py --preview

# full quality: 1080p stills, plus exports for Unreal
python blender/build_world.py --export --render stills

# flythrough video (PNG frames, then MP4 if ffmpeg is on PATH)
python blender/build_world.py --render video

# the same commands with a Blender install instead of the bpy module
blender -b -P blender/build_world.py -- --export --render all
```

Outputs land in `build/<preset>/`, which is git-ignored:
`<preset>.blend`, `renders/*.png`, `renders/flythrough.mp4` and `exports/`.
Open the `.blend` to explore or art-direct the scene by hand. Rendering uses
Cycles on the CPU by default; switch the device to GPU in the `.blend` or in
`worldgen/lighting.py` for much faster renders.

## Unreal Engine 5

1. Run the Blender build with `--export`. It copies the data into
   `unreal/ReferenceWorld/WorldData/`.
2. Open `unreal/ReferenceWorld/ReferenceWorld.uproject`. If the engine
   version prompt appears, pick your installed UE 5.3 or later.
3. Run **Tools › Execute Python Script…** and choose
   `Content/Python/rw_build_level.py`.

The script imports every mesh as Nanite and builds the master materials and
instances from the preset palette. It then creates
`/Game/ReferenceWorld/Maps/L_ReferenceWorld` with:

- **Lighting.** A sun in lux, sky atmosphere, a real-time sky light,
  volumetric clouds, volumetric height fog and an unbound post-process volume.
- **World.** Terrain, distant ranges and water.
- **Scatter.** One hierarchical instanced mesh component per vegetation or
  rock variant.
- **Camera.** A `RW_Camera` cine camera and an `LS_Flythrough` sequence that
  replays the Blender camera move. Render it with Movie Render Queue.

**Optional landscape.** For a sculptable, paintable landscape instead of the
terrain mesh, open Landscape mode, choose *Import from File* with
`WorldData/heightmap_r16.png`, and use the `landscape` scale and location
values from `WorldData/manifest.json`. The `weight_*.png` files are the
matching layer weightmaps.

> The Unreal script was written against the UE 5.3–5.5 Python API, but it
> could not be executed in the build environment, which has no Unreal Engine.
> The Blender → Unreal transform conversion was verified numerically against
> Unreal's rotator maths. If an API call differs in your engine version, the
> Output Log names the line. Most calls already have fallbacks.

## Presets

Presets live in `presets/*.json`. Each one sets only what it changes and can
`extend` another preset. Every available key, with comments, is in
`blender/worldgen/config.py`.

| Preset | Look |
|---|---|
| `golden_valley` | Default. Low warm sun from behind-left, golden meadow, crisp distant snow ranges |
| `misty_morning` | Soft, high-haze, cool morning light |
| `alpine_dusk` | Sun on the horizon, valley in shadow, alpenglow on the peaks |

```bash
python blender/build_world.py --preset misty_morning --preview
```

## Matching the reference

1. Save 3 to 6 frames from the reference video, for example the wide
   establishing shot, a close shot and the end frame.
2. Derive a starting preset from them:

   ```bash
   python tools/match_reference.py ref_*.png --name reference
   python blender/build_world.py --preset reference --preview
   ```

   The tool estimates vegetation colours, sun colour and elevation, haze
   colour and amount, contrast and snow coverage. It also writes
   `presets/reference_swatches.png` so you can compare the palette with the
   reference at a glance.
3. Fine-tune by eye. The knobs that matter most:

   | To change | Edit |
   |---|---|
   | Time of day, light direction | `lighting.sun_elevation_deg`, `lighting.sun_azimuth_deg` (0 is straight down the valley, ±180 is behind the camera) |
   | Atmosphere depth | `lighting.haze_amount`, `lighting.mist_depth_m`, `lighting.aerosol_density` |
   | Mountain drama | `terrain.mountain_height_m`, `terrain.backdrop_height_m`, `terrain.skirt_height_m` |
   | Valley shape | `terrain.valley_width_m`, `terrain.meander_amp_m`, `terrain.river_width_m` |
   | Forest density | `biome.tree_count`, `biome.conifer_ratio`, `biome.treeline_m` |
   | Snow | `biome.snowline_m` |
   | Camera move | `camera.path` keys (`y` along the valley, `height`, `x` offset, `yaw`, `pitch`) |

If the reference is a different kind of world, such as a city, a desert or a
stylised toon look, the pipeline structure still applies. The terrain and
asset generators are the parts to swap.

## Layout

```
reference-world/
├── blender/
│   ├── build_world.py        entry point (CLI)
│   └── worldgen/
│       ├── config.py         defaults + preset loading
│       ├── noise.py          vectorised Perlin / fBm / ridged noise (numpy)
│       ├── terrain.py        heightfield, erosion, masks, skirt, mesh build
│       ├── materials.py      procedural Cycles shaders
│       ├── assets.py         trees, bushes, rocks, grass (bmesh)
│       ├── scatter.py        placement + Geometry Nodes instancers
│       ├── lighting.py       sky, sun, clouds, render + compositor
│       ├── camera.py         cine camera + flythrough keys
│       ├── export.py         FBX / heightmap / scatter / manifest for UE
│       └── png.py            dependency-free PNG writer
├── presets/                  look presets (JSON)
├── tools/match_reference.py  derive a preset from reference frames
├── unreal/ReferenceWorld/    UE5 project (config + editor Python script)
└── previews/                 committed preview renders
```
