"""Exports the worldgen asset kit to the DAYFALL engine's content library.

    python export_content.py [--out ../../dayfall-engine/content] [--only trees,grass]

Writes one GLB per asset into <out>/meshes/ and <out>/library.json: mesh
entries (category, size, collision, LODs, front) and the engine materials that
reproduce the Blender shaders (the engine matches materials by name).
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from worldgen import assets, materials, meadow as meadow_mod  # noqa: E402
from worldgen.castle import Frame, MAT_KEYS  # noqa: E402
from worldgen.config import load_preset  # noqa: E402

LETTERS = "abcdefgh"


class FlatTerrain:
    """Stands in for the worldgen terrain: flat ground, the lane on x = 0."""
    def height_at(self, x, y):
        return np.zeros_like(np.asarray(x, dtype=float))

    def river_center(self, y):
        return np.zeros_like(np.asarray(y, dtype=float))


def export_glb(ob, path):
    bpy.ops.object.select_all(action="DESELECT")
    ob.location = (0.0, 0.0, 0.0)
    ob.rotation_euler = (0.0, 0.0, 0.0)
    ob.scale = (1.0, 1.0, 1.0)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    if ob.data.color_attributes.get("LeafVar") is not None:
        ob.data.color_attributes.active_color = ob.data.color_attributes["LeafVar"]
    bpy.ops.export_scene.gltf(
        filepath=str(path), export_format="GLB", use_selection=True, export_yup=True, export_apply=True,
        export_texcoords=True, export_normals=True, export_tangents=False, export_materials="EXPORT",
        export_vertex_color="NAME", export_vertex_color_name="LeafVar", export_all_vertex_colors=False,
        export_image_format="NONE", export_cameras=False, export_lights=False, export_extras=False)


def bounds(ob):
    co = np.array([ob.matrix_world @ v.co for v in ob.data.vertices])
    lo, hi = co.min(0), co.max(0)
    return [round(float(v), 2) for v in (hi - lo)], lo, hi


def tris(ob):
    return sum(len(p.vertices) - 2 for p in ob.data.polygons)


# ----------------------------------------------------------------------------- engine materials
def engine_materials(pal):
    def c(v, k=1.0):
        return [round(x * k, 4) for x in v]
    fol = lambda a, b, tip, tr, var, wind: {"model": "foliage", "color_a": a, "color_b": b, "tip": tip,  # noqa: E731
                                            "translucency": tr, "variation": var, "vertex_color": True, "wind": wind}
    tree_wind = {"strength": 0.07, "height": 12.0, "speed": 1.2}
    bush_wind = {"strength": 0.05, "height": 1.6, "speed": 1.6}
    low_wind = {"strength": 0.1, "height": 0.6, "speed": 2.0}
    courses = lambda a, b, mortar, bw, bh, mw, grime, rough=0.86, ns=0.03, off=0.5: {  # noqa: E731
        "model": "courses", "color_a": a, "color_b": b, "mortar_color": mortar, "block_width": bw, "block_height": bh,
        "mortar_width": mw, "grime": grime, "roughness": rough, "normal_strength": ns, "row_offset": off}
    return {
        "RW_Needles": fol(c(pal["needles"]), c(pal["needles"], 1.5), c(pal["needles"], 2.6), 0.25, 0.6, tree_wind),
        "RW_Leaves": fol(c(pal["leaves"]), c(pal["leaves_alt"]), [0.3, 0.34, 0.06], 0.4, 0.45, tree_wind),
        "RW_Bush": fol(c(pal["leaves"], 0.85), c(pal["needles"]), [0.22, 0.26, 0.05], 0.3, 0.7, bush_wind),
        "RW_Fern": fol(c(pal["leaves"], 1.1), c(pal["needles"]), [0.25, 0.3, 0.06], 0.45, 0.5, low_wind),
        "RW_Ivy": fol([0.025, 0.06, 0.015], [0.05, 0.09, 0.02], [0.12, 0.16, 0.04], 0.3, 0.6, {"strength": 0.01, "height": 3.0, "speed": 1.5}),
        "RW_Reed": fol([0.06, 0.1, 0.028], [0.13, 0.15, 0.045], [0.25, 0.24, 0.08], 0.4, 0.7, {"strength": 0.12, "height": 2.0, "speed": 1.8}),
        "RW_ReedPlume": fol([0.16, 0.1, 0.1], [0.3, 0.24, 0.16], [0.4, 0.33, 0.24], 0.55, 0.7, {"strength": 0.12, "height": 2.0, "speed": 1.8}),
        "RW_Grass": {"model": "grass", "color_a": c(pal["grass"]), "color_b": c(pal["grass_dry"]), "wind": low_wind},
        "RW_Sedge": {"model": "grass", "color_a": [0.03, 0.065, 0.028], "color_b": [0.11, 0.1, 0.045], "wind": low_wind},
        "RW_Flowers": {"model": "foliage", "color_a": c(pal["flowers"][0]), "color_b": c(pal["flowers"][1]), "tip": c(pal["flowers"][0]),
                       "translucency": 0.3, "variation": 1.0, "wind": low_wind},
        "RW_Bark": {"model": "lit", "base_color": c(pal["bark"], 1.3), "roughness": 0.92},
        "RW_DeadWood": {"model": "lit", "base_color": [0.075, 0.068, 0.058], "roughness": 0.9},
        "RW_Rock": {"model": "rock", "color": c(pal["rock"], 1.15), "color_dark": c(pal["rock_dark"], 1.4), "moss": c(pal["moss"]),
                    "lichen": [0.3, 0.29, 0.2], "moss_amount": 0.55, "lichen_amount": 0.5, "bump": 0.05},
        "RW_Moss": {"model": "lit", "base_color": c(pal["moss"]), "roughness": 0.95},
        "RW_Stone": courses(c(pal["stone"]), c(pal["stone_dark"], 1.4), [0.14, 0.12, 0.09], 0.95, 0.44, 0.014, 0.5),
        "RW_Rubble": courses([0.15, 0.14, 0.12], [0.09, 0.085, 0.075], [0.08, 0.075, 0.065], 0.46, 0.27, 0.02, 0.6),
        "RW_Roof": courses(c(pal["roof"]), c(pal["roof"], 0.6), [0.02, 0.02, 0.02], 0.3, 0.22, 0.02, 0.2, 0.55, 0.05),
        "RW_RoofAlt": courses(c(pal["roof_alt"]), c(pal["roof_alt"], 0.65), [0.05, 0.03, 0.02], 0.3, 0.22, 0.02, 0.2, 0.7, 0.06),
        "RW_Wood": courses(c(pal["wood"], 1.4), c(pal["wood"], 0.8), [0.03, 0.02, 0.012], 2.4, 0.2, 0.008, 0.4, 0.75, 0.02, 0.37),
        "RW_Iron": {"model": "lit", "base_color": c(pal["iron"]), "roughness": 0.45, "metallic": 1.0},
        "RW_Fire": {"model": "emissive", "base_color": [1.0, 0.42, 0.1], "emissive": [1.0, 0.42, 0.1], "emissive_strength": 12.0},
        "RW_WindowDark": {"model": "lit", "base_color": [0.01, 0.01, 0.012], "roughness": 0.2},
        "RW_WindowLit": {"model": "emissive", "base_color": [0.05, 0.03, 0.01], "emissive": c(pal["window_glow"]), "emissive_strength": 6.0},
        "RW_Cloth": {"model": "lit", "base_color": c(pal["cloth"]), "roughness": 0.7},
        "RW_Hay": {"model": "lit", "base_color": c(pal["hay"]), "roughness": 0.9},
        "RW_Cattail": {"model": "lit", "base_color": [0.065, 0.037, 0.02], "roughness": 0.9},
        "RW_LilyPad": {"model": "lit", "base_color": [0.035, 0.08, 0.018], "roughness": 0.35, "specular": 0.6},
        "RW_LilyFlower": {"model": "foliage", "color_a": [0.82, 0.8, 0.74], "color_b": [0.8, 0.38, 0.5], "tip": [0.9, 0.85, 0.8],
                          "translucency": 0.4, "variation": 1.0},
        "RW_LilyCenter": {"model": "lit", "base_color": [0.75, 0.45, 0.04], "roughness": 0.6},
    }


# ----------------------------------------------------------------------------- the kit
def kit(mats):
    """(category, engine name, Blender object, metadata) for every library asset."""
    root = bpy.context.scene.collection
    lib = assets.build_library(mats, root, marsh=True)
    out = []

    def add(cat, objs, base, meta):
        for i, ob in enumerate(objs):
            ob = bpy.data.objects[ob] if isinstance(ob, str) else ob
            out.append((cat, f"{base}_{LETTERS[i]}", ob, dict(meta)))

    trees = [bpy.data.objects[o] if isinstance(o, str) else o for o in lib["trees"][1]]
    tree_lods = {"lod0_distance_m": 45, "lods": [{"ratio": 0.45, "distance_m": 110, "foliage": "drop"},
                                                 {"ratio": 0.18, "distance_m": 320, "foliage": "drop", "sloppy": True}],
                 "cull_distance_m": 1500, "front": "-y"}
    add("trees", trees[:4], "conifer", dict(tree_lods, description="Conifer (spruce / fir), 14-23 m",
                                             collision={"type": "cylinder", "radius": 0.35, "height": 8}, footprint_m=2.5))
    add("trees", trees[4:], "broadleaf", dict(tree_lods, description="Broadleaf tree (oak / beech), 10-15 m",
                                               collision={"type": "cylinder", "radius": 0.4, "height": 5}, footprint_m=3))
    add("bushes", lib["bushes"][1], "bush", {"description": "Leafy bush, 1.2-2.2 m", "collision": "none", "cull_distance_m": 300,
                                             "lod0_distance_m": 30, "lods": [{"ratio": 0.4, "distance_m": 90, "foliage": "drop"}]})
    add("rocks", lib["rocks"][1], "rock", {"description": "Fractured boulder, 1.5-4.7 m", "collision": "convex", "cull_distance_m": 900,
                                           "lod0_distance_m": 40, "lods": [{"ratio": 0.35, "distance_m": 140}, {"ratio": 0.1, "distance_m": 400}]})
    grass = [bpy.data.objects[o] if isinstance(o, str) else o for o in lib["grass"][1]]
    glods = {"collision": "none", "cull_distance_m": 90, "lod0_distance_m": 14,
             "lods": [{"ratio": 0.5, "distance_m": 35, "foliage": "drop"}, {"ratio": 0.22, "distance_m": 90, "foliage": "drop"}]}
    add("grass", grass[0:3], "grass_clump", dict(glods, description="Meadow grass clump (scatter densely)"))
    add("grass", [grass[3], grass[7]], "flowers", dict(glods, description="Grass clump with wildflowers"))
    add("grass", grass[4:6], "grass_tall", dict(glods, description="Tall grass clump"))
    add("grass", [grass[6]], "grass_short", dict(glods, description="Short grass tuft (paths, under trees)"))
    add("ferns", lib["ferns"][1], "fern", {"description": "Fern, 0.8-1.4 m (shade, forest floor)", "collision": "none",
                                           "cull_distance_m": 120, "lod0_distance_m": 18, "lods": [{"ratio": 0.4, "distance_m": 50, "foliage": "drop"}]})
    add("rocks", lib["pebbles"][1], "pebble", {"description": "Pebble / small stone, 0.35 m", "collision": "none", "cull_distance_m": 80})
    add("rocks", lib["outcrops"][1], "outcrop", {"description": "Rock outcrop, about 9 m", "collision": "mesh", "cull_distance_m": 2500,
                                                 "lod0_distance_m": 60, "lods": [{"ratio": 0.3, "distance_m": 220}, {"ratio": 0.1, "distance_m": 700}]})
    add("marsh", lib["reeds"][1][:3], "cattail", {"description": "Cattail clump (water's edge)", "collision": "none", "cull_distance_m": 120})
    add("marsh", lib["reeds"][1][3:], "reed", {"description": "Reed clump with plumes", "collision": "none", "cull_distance_m": 120})
    add("marsh", lib["sedges"][1], "sedge", {"description": "Sedge tussock (wet ground)", "collision": "none", "cull_distance_m": 100})
    add("marsh", lib["lilies"][1], "lily", {"description": "Water lily cluster (place at the water level)", "collision": "none",
                                            "cull_distance_m": 120})
    return out


def buildings(cfg, mats):
    """The meadow ruins, each built alone at the origin, front facing -Y."""
    out = []
    specs = [
        ("cottage_ruin", "Ruined stone cottage, 9 x 6.5 m, ivy, fallen rafters", lambda M: M.cottage(0.0, 1, 0.0)),
        ("barn_ruin", "Ruined timber barn on a stone base", lambda M: M.barn(0.0, 1, 0.0)),
        ("chapel_ruin", "Ruined stone chapel", lambda M: M.chapel(0.0, 1, 0.0)),
        ("chimney_ruin", "Lone chimney stack of a burnt house", lambda M: M.chimney_ruin(0.0, 1, 0.0)),
        ("cart", "Abandoned wooden cart", lambda M: M.cart(0.0, 1, 0.0)),
        ("well_ruin", "Old stone well with a broken frame", lambda M: M.well(0.0, 0.0)),
        ("fence_segment", "Weathered timber fence, 12 m along +X (use place_along_path with spacing 12)",
         lambda M: M.fence([(-6.0, 0.0), (6.0, 0.0)], damage=0.15)),
        ("stone_wall_segment", "Dry-stone field wall, 12 m along +X", lambda M: M.dry_stone_wall([(-6.0, 0.0), (6.0, 0.0)], 1.0)),
    ]
    coll = bpy.data.collections.new("Buildings")
    bpy.context.scene.collection.children.link(coll)
    for name, desc, fn in specs:
        M = meadow_mod.Meadow(cfg, FlatTerrain())
        M.site = lambda y, side, offset, jitter=0.25: Frame(0.0, 0.0, 0.0)
        fn(M)
        me = bpy.data.meshes.new(name)
        M.B.bm.to_mesh(me)
        M.B.bm.free()
        for key in MAT_KEYS:
            me.materials.append(mats[key])
        ob = bpy.data.objects.new(name, me)
        coll.objects.link(ob)
        fp = max([f[2] for f in M.footprints], default=4.0)
        out.append(("buildings" if "segment" not in name else "fences", name, ob,
                    {"description": desc, "collision": "mesh", "front": "-y", "footprint_m": round(fp, 1), "cull_distance_m": 1200,
                     "lod0_distance_m": 70, "lods": [{"ratio": 0.4, "distance_m": 220}]}))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=str(HERE.parent.parent / "dayfall-engine" / "content"))
    p.add_argument("--preset", default="serene_meadow")
    p.add_argument("--only", default="", help="comma-separated categories")
    args = p.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:])
    out = Path(args.out)
    (out / "meshes").mkdir(parents=True, exist_ok=True)
    cfg = load_preset(args.preset)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    mats = materials.build_all(cfg)
    items = kit(mats) + buildings(cfg, mats)
    only = set(filter(None, args.only.split(",")))
    lib = {"format": "dayfall-content", "source": "reference-world/blender/export_content.py",
           "materials": engine_materials(cfg["palette"]), "meshes": {}}
    lib_file = out / "library.json"
    if only and lib_file.exists():
        lib["meshes"] = json.loads(lib_file.read_text()).get("meshes", {})
    total = 0
    for cat, name, ob, meta in items:
        if only and cat not in only:
            continue
        path = out / "meshes" / f"{name}.glb"
        export_glb(ob, path)
        size, lo, hi = bounds(ob)
        entry = {"file": f"meshes/{name}.glb", "category": cat, "size_m": size, "triangles": tris(ob), **meta}
        if abs(lo[2]) > 0.05 and cat not in ("marsh",):
            entry["note"] = f"base at z = {lo[2]:.2f}"
        lib["meshes"][name] = entry
        total += path.stat().st_size
        print(f"{cat:10s} {name:22s} {tris(ob):7d} tris  {path.stat().st_size / 1024:8.0f} KB  size {size}")
    lib["meshes"] = dict(sorted(lib["meshes"].items(), key=lambda kv: (kv[1]["category"], kv[0])))
    lib_file.write_text(json.dumps(lib, indent=1) + "\n")
    print(f"wrote {len(lib['meshes'])} meshes, {total / 1024 / 1024:.1f} MB, {lib_file}")


if __name__ == "__main__":
    main()
