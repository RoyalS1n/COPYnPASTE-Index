"""Ports a reference-world preset to a DAYFALL engine map.

    python export_dayfall_map.py --preset serene_meadow [--out ../../dayfall-engine/maps] [--name serene_meadow]

Exact: terrain heights (incl. the marsh detail), castle / meadow structures,
trees, bushes, rocks, ferns, pebbles, outcrops and marsh plants (instance
files referencing the engine content library), lights, cameras, the player
start, lighting. Grass becomes engine scatter rules (1.6 M clumps would not
fit in git); the meadow lane becomes an engine path (it draws the ruts).
"""
import argparse
import json
import math
import struct
import sys
import zlib
from pathlib import Path

import bpy
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from worldgen import camera, castle, materials, meadow, scatter  # noqa: E402
from worldgen.config import load_preset  # noqa: E402
from worldgen.terrain import Terrain  # noqa: E402

# scatter category -> content-library mesh per variant index (assets.build_library order)
VARIANTS = {
    "trees": ["conifer_a", "conifer_b", "conifer_c", "conifer_d", "broadleaf_a", "broadleaf_b", "broadleaf_c"],
    "bushes": ["bush_a", "bush_b", "bush_c"],
    "rocks": ["rock_a", "rock_b", "rock_c", "rock_d", "rock_e"],
    "ferns": ["fern_a", "fern_b", "fern_c"],
    "pebbles": ["pebble_a", "pebble_b", "pebble_c", "pebble_d"],
    "outcrops": ["outcrop_a", "outcrop_b", "outcrop_c"],
    "reeds": ["cattail_a", "cattail_b", "cattail_c", "reed_a", "reed_b"],
    "sedges": ["sedge_a", "sedge_b", "sedge_c"],
    "lilies": ["lily_a", "lily_b", "lily_c"],
}
COLLISION = {"trees": "auto", "rocks": "auto", "outcrops": "auto"}
SHADOW = {"pebbles": False, "lilies": False, "sedges": False}


def png(path, w, h, data, depth, color_type):
    """Minimal PNG writer: 16-bit grey (heightmap) or 8-bit RGBA, filter 'up'."""
    bpp = {(16, 0): 2, (8, 6): 4}[(depth, color_type)]
    raw = np.ascontiguousarray(data).view(np.uint8).reshape(h, w * bpp)
    up = np.vstack([raw[:1], (raw[1:].astype(np.int16) - raw[:-1].astype(np.int16)).astype(np.uint8)])
    rows = np.hstack([np.full((h, 1), 2, np.uint8), up])
    rows[0, 0] = 0  # first row unfiltered
    rows[0, 1:] = raw[0]

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color_type, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(rows.tobytes(), 9)) + chunk(b"IEND", b"")
    Path(path).write_bytes(out)


def export_glb(ob, path, color_name=None):
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    if color_name and ob.data.color_attributes.get(color_name) is not None:
        ob.data.color_attributes.active_color = ob.data.color_attributes[color_name]
    bpy.ops.export_scene.gltf(
        filepath=str(path), export_format="GLB", use_selection=True, export_yup=True, export_apply=True,
        export_texcoords=True, export_normals=True, export_tangents=False, export_materials="EXPORT",
        export_vertex_color="NAME" if color_name else "NONE", export_vertex_color_name=color_name or "Color",
        export_all_vertex_colors=False, export_image_format="NONE", export_cameras=False, export_lights=False)


def euler_quat(rx, ry, rz):
    """Blender XYZ Euler -> quaternion (x, y, z, w) arrays: q = qz * qy * qx."""
    cx, sx = np.cos(rx / 2), np.sin(rx / 2)
    cy, sy = np.cos(ry / 2), np.sin(ry / 2)
    cz, sz = np.cos(rz / 2), np.sin(rz / 2)
    w = cz * cy * cx + sz * sy * sx
    x = cz * cy * sx - sz * sy * cx
    y = cz * sy * cx + sz * cy * sx
    z = sz * cy * cx - cz * sy * sx
    return x, y, z, w


def environment(cfg):
    lt = cfg["lighting"]
    look = lt.get("look", "")
    contrast, sat = 1.15, 1.05
    if "Punchy" in look:
        contrast, sat = 1.35, 1.3
    elif "Base" in look:
        contrast, sat = 1.0, 1.0
    elif "High" in look and "Medium" not in look:
        contrast = 1.25
    haze = lt.get("haze_color", [0.62, 0.52, 0.42])
    far = [round(0.5 * a + 0.5 * b, 3) for a, b in zip(haze, [0.48, 0.47, 0.5])]
    return {
        "sun": {"elevation_deg": lt["sun_elevation_deg"], "azimuth_deg": lt["sun_azimuth_deg"], "strength": lt["sun_strength"],
                "color": lt["sun_color"], "angle_deg": lt.get("sun_angle_deg", 0.6)},
        "sky": {"strength": lt.get("sky_strength", 0.1), "air_density": lt.get("air_density", 1.0),
                "aerosol_density": lt.get("aerosol_density", 1.6), "ozone_density": lt.get("ozone_density", 1.0)},
        "haze": {"color_near": haze, "color_far": far, "amount": lt.get("haze_amount", 0.6), "start_m": lt.get("mist_start_m", 30.0),
                 "depth_m": lt.get("mist_depth_m", 6500.0)},
        "clouds": {"enabled": bool(lt.get("clouds", True)), "coverage": lt.get("cloud_coverage", 0.5),
                   "color": lt.get("cloud_color", [1.0, 0.75, 0.55])},
        "tonemap": {"exposure_ev": lt.get("exposure", 0.85), "contrast": contrast, "saturation": sat, "vignette": lt.get("vignette", 0.25)},
        "water": {"enabled": True, "level_m": cfg["biome"]["water_level_m"]},
        "shadow_distance_m": 300,
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--preset", default="serene_meadow")
    p.add_argument("--out", default=str(HERE.parent.parent / "dayfall-engine" / "maps"))
    p.add_argument("--name", default=None)
    args = p.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:])
    cfg = load_preset(args.preset)
    name = args.name or cfg["name"]
    out = Path(args.out) / name
    for d in ("terrain", "assets", "instances"):
        (out / d).mkdir(parents=True, exist_ok=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    coll = bpy.context.scene.collection
    print("terrain")
    T = Terrain(cfg).build()
    mats = materials.build_all(cfg)
    res, half, sp = T.res, T.half, T.spacing
    H = np.array(T.height, dtype=np.float64)
    if getattr(T, "patch", None) is not None:
        # the marsh's high-resolution heights, sampled at the main grid
        H = np.asarray(T.height_at(T.X, T.Y), dtype=np.float64)
    lo, hi = float(H.min()), float(H.max())
    q = np.round((H - lo) / max(hi - lo, 0.01) * 65535).astype(">u2")
    png(out / "terrain" / "height.png", res, res, np.flipud(q), 16, 0)      # images: north (top) row first
    paint_a = np.zeros((res, res, 4), np.uint8)       # dirt, rock, snow, wet
    paint_b = np.zeros((res, res, 4), np.uint8)       # dry, grass, -, -
    meadow_mode = cfg["terrain"].get("mode") == "meadow"
    if not meadow_mode and "path" in T.masks:          # the castle road
        paint_a[..., 0] = np.clip(T.masks["path"] * 255, 0, 255).astype(np.uint8)
    png(out / "terrain" / "paint_a.png", res, res, np.flipud(paint_a), 8, 6)
    png(out / "terrain" / "paint_b.png", res, res, np.flipud(paint_b), 8, 6)

    doc = {"format": "dayfall-map", "version": 2, "name": cfg.get("title", name.replace("_", " ").title()),
           "source": f"reference-world preset '{args.preset}' via reference-world/blender/export_dayfall_map.py",
           "environment": environment(cfg),
           "terrain": {"samples_per_side": res, "spacing_m": sp, "origin": [-half, -half], "seed": int(cfg["seed"]),
                       "height_file": "terrain/height.png", "height_range_m": [lo, hi], "png_rows": "north_first",
                       "paint_files": ["terrain/paint_a.png", "terrain/paint_b.png"],
                       "snowline_m": cfg["biome"]["snowline_m"], "rock_slope_deg": 40.0, "dry_amount": 0.5, "material": "terrain",
                       "horizon": {"enabled": True, "radius_m": 8000, "height_m": cfg["terrain"].get("skirt_height_m", 1500.0) * 0.6,
                                   "roughness": 0.8}},
           "materials": {}, "meshes": {}, "objects": [], "paths": [], "scatter": [], "instance_files": [], "entities": [],
           "lights": [], "cameras": {}, "routes": {}, "player": {}}

    # ---- structures
    info = {}
    if cfg.get("castle", {}).get("enabled") and not meadow_mode:
        print("castle")
        castle_ob, info = castle.build(cfg, T, mats, coll)
        crag_ob = castle.build_cliff(cfg, T, mats, coll)
        for key, ob in (("castle", castle_ob), ("castle_cliff", crag_ob)):
            export_glb(ob, out / "assets" / f"{key}.glb")
            doc["meshes"][key] = {"file": f"assets/{key}.glb", "category": "structures", "collision": "mesh",
                                  "lod0_distance_m": 1e9}
            doc["objects"].append({"id": key, "mesh": key, "position": [0, 0, 0], "collision": "mesh", "cull_distance_m": 1e6})
    if cfg.get("meadow", {}).get("enabled"):
        print("meadow structures")
        props_ob, info = meadow.build(cfg, T, mats, coll)
        export_glb(props_ob, out / "assets" / "meadow_props.glb")
        doc["meshes"]["meadow_props"] = {"file": "assets/meadow_props.glb", "category": "structures", "collision": "mesh"}
        doc["objects"].append({"id": "meadow_props", "mesh": "meadow_props", "position": [0, 0, 0], "collision": "mesh",
                               "cull_distance_m": 1e6})
    if getattr(T, "patch", None) is not None:
        pond = bpy.data.objects.new("RW_PondWater", T.pond_water_mesh(bpy, "RW_PondWater"))
        pond.data.materials.append(mats["pondwater"])
        coll.objects.link(pond)
        export_glb(pond, out / "assets" / "pond_water.glb", "Depth")
        doc["meshes"]["pond_water"] = {"file": "assets/pond_water.glb", "category": "water", "collision": "none"}
        doc["objects"].append({"id": "pond_water", "mesh": "pond_water", "position": [0, 0, 0], "collision": "none",
                               "shadow": False, "cull_distance_m": 1e6})
        doc["materials"]["RW_PondWater"] = {"model": "water", "color": [0.45, 0.5, 0.36], "absorption": 2.2, "roughness": 0.03,
                                            "duckweed": True, "duckweed_color": [0.08, 0.17, 0.025], "ripple_strength": 0.5}
        doc["environment"]["water"]["plane"] = False

    # ---- lights, cameras, player start
    for i, L in enumerate(info.get("lights", [])):
        intensity = L["power"] / (4 * math.pi)
        doc["lights"].append({"id": f"{L.get('kind', 'light')}_{i:03d}", "position": [round(v, 2) for v in L["pos"]],
                              "color": [round(c, 3) for c in L["color"]], "intensity": round(intensity, 3),
                              "range_m": round(float(np.clip(math.sqrt(intensity / 0.02), 4.0, 40.0)), 1)})
    for k, v in info.items():
        if isinstance(v, dict) and "pos" in v and "target" in v:
            doc["cameras"][k.replace("_camera", "")] = {"position": [round(c, 2) for c in v["pos"]],
                                                        "target": [round(c, 2) for c in v["target"]], "vfov_deg": 45}
    ps = info.get("player_start")
    if ps:
        doc["player_start"] = {"position": [round(ps["pos"][0], 2), round(ps["pos"][1], 2)], "yaw_deg": round(ps["yaw_deg"], 1)}

    # ---- scatter: exact instances for everything but grass
    print("scatter")
    keys = camera.resolve_keys(cfg, T)
    for i, k in enumerate(keys[:: max(1, len(keys) // 4)]):
        wx, wy, wz = k["world"]
        b, pt = math.radians(k["bearing"]), math.radians(k["pitch"])
        fwd = (math.sin(b) * math.cos(pt), math.cos(b) * math.cos(pt), math.sin(pt))
        doc["cameras"][f"path_{i}"] = {"position": [round(wx, 2), round(wy, 2), round(wz, 2)],
                                       "target": [round(wx + fwd[0] * 50, 2), round(wy + fwd[1] * 50, 2), round(wz + fwd[2] * 50, 2)],
                                       "vfov_deg": 45}
    points = scatter.compute(cfg, T, keys)
    total = 0
    for cat, data in points.items():
        if cat == "grass":
            continue
        names = VARIANTS.get(cat)
        if not names:
            print(f"  skip {cat}")
            continue
        qx, qy, qz, qw = euler_quat(data["rx"], data["ry"], data["rz"])
        rec = np.stack([data["x"], data["y"], data["z"], data["s"], qx, qy, qz, qw], 1).astype("<f4")
        for vi, mesh in enumerate(names):
            sel = rec[data["v"] == vi]
            if len(sel) == 0:
                continue
            f = f"instances/{mesh}.bin"
            (out / f).write_bytes(sel.tobytes())
            entry = {"id": f"{cat}_{mesh}", "mesh": mesh, "file": f, "shadow": SHADOW.get(cat, True)}
            if cat in COLLISION:
                entry["collision"] = "convex" if cat != "trees" else {"type": "cylinder", "radius": 0.4, "height": 6}
                if cat == "outcrops":
                    entry["collision"] = "mesh"
            doc["instance_files"].append(entry)
            total += len(sel)
        print(f"  {cat}: {len(rec)}")
    print(f"  {total} instances in files")

    # ---- grass as rules (dense near the route and the hotspots)
    grass_mix = [{"mesh": "grass_clump_a", "weight": 3}, {"mesh": "grass_clump_b", "weight": 3}, {"mesh": "grass_clump_c", "weight": 3},
                 {"mesh": "grass_tall_a", "weight": 1.2}, {"mesh": "grass_tall_b", "weight": 1.2}, {"mesh": "grass_short_a", "weight": 1.5},
                 {"mesh": "flowers_a", "weight": 0.6 if meadow_mode else 0.25}, {"mesh": "flowers_b", "weight": 0.6 if meadow_mode else 0.25}]
    common = {"meshes": grass_mix, "scale": [0.7, 1.4], "tilt_deg": 7, "shadow": False, "avoid_paths_m": 0.0, "max_path": 1.0,
              "avoid_ruts": True, "avoid_objects_m": 0.5, "max_rock": 0.45, "slope_deg": [0, 38], "clumping": 0.35, "clump_scale_m": 25}
    route = [[round(k["world"][0], 1), round(k["world"][1], 1)] for k in keys]
    gr = cfg["biome"]["grass_radius_m"]
    if meadow_mode:
        core = cfg["meadow"]["radius_m"]
        doc["scatter"].append(dict(common, id="grass_meadow", area={"circle": {"center": [0, 0], "radius": core * 1.05}, "falloff": core * 0.1},
                                   density_per_100m2=45, seed=21))
        hot = [{"circle": {"center": [round(h[0], 1), round(h[1], 1)], "radius": round(h[2] * 0.75, 1)}}
               for h in getattr(T, "hotspots", [])]
        if hot:
            doc["scatter"].append(dict(common, id="grass_hotspots", area={"union": hot, "falloff": 20}, density_per_100m2=280, seed=22))
        lane = [[round(float(T.river_center(np.array(y))), 2), y] for y in np.linspace(-core * 1.45, core * 1.45, 60).round(1).tolist()]
        doc["paths"].append({"id": "lane", "style": "lane", "points": lane, "width_m": 3.4, "carve_m": 0.0, "flatten": 0.25,
                             "falloff_m": 1.5, "smooth_m": 4})
        doc["routes"]["lane"] = {"points": lane[4:-4:3]}
    doc["scatter"].append(dict(common, id="grass_route", area={"line": route, "width": gr * 1.2, "falloff": gr * 0.6},
                               density_per_100m2=330, seed=23))
    if not meadow_mode:
        doc["routes"]["camera_path"] = {"points": route}
    (out / "map.json").write_text(json.dumps(doc, indent=1) + "\n")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
