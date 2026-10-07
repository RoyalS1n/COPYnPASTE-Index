"""Convert a whole scene file (FBX from Unity, Unreal or Blender; also .glb / .gltf / .blend) into a DAYFALL map.

    "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe" -b --factory-startup
        --python tools/fbx_scene_to_dayfall.py -- SCENE.fbx --out maps/<name> [options]

(or `python fbx_scene_to_dayfall.py ...` with the bpy module). Options:
  --name NAME            map name (default: the file name)
  --textures DIR         folder with the scene's texture files; matched to materials by name
  --materials FILE.json  DAYFALL material definitions that replace the guessed ones, by material name
  --ground none|auto     auto: no terrain in the file, so rebuild one from where the objects stand (default: auto
                         when the scene has no mesh larger than 150 m, else none)
  --spacing M            ground sample spacing in metres (default 1)

What it does:
  - one GLB per unique mesh (assets/), from the mesh data shared by its placements; of LOD siblings named
    *_LOD0 / *_LOD1 / ..., only LOD0 is kept and the engine builds its own LODs (meshes entries get "lods");
  - every placement goes into an instance file per mesh (instances/), with per-axis scale where needed;
  - materials get DAYFALL definitions guessed from their names (water, foliage, bark, rock, ...) unless --materials
    gives them; textures found in --textures are attached (base colour, normal, ORM);
  - a sun light becomes the environment's sun; point lights become lights;
  - with --ground auto, a heightfield is fitted under the objects (grass, flowers, trees and rocks stand on the
    ground, so their origins sample it); areas inside water planes are lowered below the water.

Blender's world space is DAYFALL's (metres, Z up), so no axis conversion is needed after Blender imports the file.
A report goes to <out>/scene_import_report.json. Check the result with captures; guessed materials are a start.
"""
import argparse
import json
import math
import os
import re
import shutil
import struct
import sys
import zlib
from collections import defaultdict

import bpy
from mathutils import Matrix, Vector

LOD_RE = re.compile(r"^(?P<base>.+?)_LOD(?P<lod>\d+)(?:_\d+)?(?:\.\d+)?$", re.IGNORECASE)
DUP_RE = re.compile(r"(?:_\d+|\.\d+)+$")


def cli():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    ap = argparse.ArgumentParser(prog="fbx_scene_to_dayfall.py")
    ap.add_argument("scene")
    ap.add_argument("--out", required=True)
    ap.add_argument("--name")
    ap.add_argument("--textures")
    ap.add_argument("--materials")
    ap.add_argument("--ground", choices=("auto", "none"), default=None)
    ap.add_argument("--spacing", type=float, default=1.0)
    return ap.parse_args(argv)


def load_scene(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    ext = os.path.splitext(path)[1].lower()
    if ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path, use_custom_normals=True)
    elif ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".blend":
        bpy.ops.wm.open_mainfile(filepath=path)
    else:
        raise SystemExit(f"unsupported scene file {path}")


def safe(name):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("._") or "mesh"


def tokens(name):
    return [t.lower() for t in re.findall(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+", name)]


# ---------------------------------------------------------------- categories and materials

def category(name):
    t = set(tokens(name))
    n = name.lower()
    if t & {"water", "ocean", "lake", "river", "pond", "sea"}:
        return "water"
    if t & {"grass", "flower", "flowers", "poppy", "mushroom", "mushrooms", "fern", "clover", "weed", "weeds"}:
        return "ground_cover"
    if t & {"log", "logs", "stump", "fallen"}:
        return "structure"
    if t & {"shrub", "bush", "bushes", "hedge"}:
        return "shrub"
    if t & {"tree", "trees", "pine", "oak", "birch", "palm", "fir", "spruce", "trunk"} or "tree" in n:
        return "tree"
    if t & {"rock", "rocks", "boulder", "boulders", "stone", "stones", "menhir", "cliff", "ore", "pebble"}:
        return "rock"
    if t & {"bridge"}:
        return "bridge"
    return "structure"


def guess_material(name):
    """A DAYFALL material for a material known only by its name."""
    t = set(tokens(name))
    n = name.lower()
    if t & {"water", "ocean", "lake", "river", "pond", "sea"}:
        return {"model": "water", "color": [0.55, 0.62, 0.58], "absorption": 1.0}
    if t & {"leaves", "leaf", "foliage", "needles", "canopy"}:
        pine = "pine" in t or "needles" in t
        return {"model": "foliage", "two_sided": True, "color_a": [0.045, 0.1, 0.03] if pine else [0.08, 0.16, 0.035],
                "color_b": [0.07, 0.12, 0.03] if pine else [0.14, 0.18, 0.04], "tip": [0.2, 0.28, 0.06],
                "variation": 0.6, "translucency": 0.35, "roughness": 0.6}
    if "grass" in t:
        return {"model": "foliage", "two_sided": True, "color_a": [0.07, 0.15, 0.03], "color_b": [0.16, 0.17, 0.05],
                "tip": [0.3, 0.32, 0.08], "variation": 0.7, "translucency": 0.4}
    if "poppy" in t or "flower" in t or "flowers" in t:
        return {"model": "lit", "base_color": [0.55, 0.05, 0.03], "roughness": 0.55, "two_sided": True}
    if "mushroom" in t or "mushrooms" in t:
        return {"model": "lit", "base_color": [0.62, 0.22, 0.06], "roughness": 0.5}
    if "trunk" in t or "bark" in t or ("opaque" in t and "tree" in n):
        return {"model": "lit", "base_color": [0.12, 0.075, 0.045], "roughness": 0.85}
    if t & {"rock", "rocks", "stone", "stones", "cliff", "boulder"}:
        return {"model": "rock", "color": [0.16, 0.15, 0.135], "color_dark": [0.06, 0.055, 0.05], "moss_amount": 0.35,
                "lichen_amount": 0.4}
    if t & {"buildings", "building", "wood", "wooden", "plank", "planks", "bridge", "fence", "gate"}:
        return {"model": "lit", "base_color": [0.3, 0.2, 0.12], "roughness": 0.8}
    return {"model": "lit", "base_color": [0.5, 0.5, 0.5], "roughness": 0.8}


def find_textures(mat_name, tex_dir):
    """Textures for a material from a folder: files sharing the material's name tokens, by slot suffix."""
    if not tex_dir or not os.path.isdir(tex_dir):
        return {}
    skip = {"mat", "material", "m", "mi", "pt", "t"}
    want = [t for t in tokens(mat_name) if t not in skip]
    best = {}
    for root, _, files in os.walk(tex_dir):
        for f in files:
            if not f.lower().endswith((".png", ".tga", ".jpg", ".jpeg")):
                continue
            ft = tokens(os.path.splitext(f)[0])
            score = sum(1 for w in want if w in ft)
            if score < max(1, len(want) - 1):
                continue
            slot = "normal" if set(ft) & {"normal", "nrm", "n", "norm"} else "orm" if set(ft) & {"orm", "arm"} else \
                "emissive" if set(ft) & {"emissive", "emission", "e"} else \
                None if set(ft) & {"roughness", "metallic", "ao", "height", "mask", "r", "m"} else "base"
            if slot and (slot not in best or score > best[slot][0]):
                best[slot] = (score, os.path.join(root, f))
    return {k: v[1] for k, v in best.items()}


# ---------------------------------------------------------------- export helpers

def export_mesh(mesh, path, fix=None):
    """Write one mesh datablock at identity as a GLB; fix (4x4) turns and scales it into metres, Z up."""
    if fix is not None:
        mesh = mesh.copy()
        mesh.transform(fix)
    obj = bpy.data.objects.new("export_" + mesh.name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    props = set(bpy.ops.export_scene.gltf.get_rna_type().properties.keys())
    kw = dict(filepath=path, export_format="GLB", use_selection=True, export_apply=False, export_yup=True,
              export_tangents=True, export_normals=True, export_texcoords=True, export_materials="EXPORT",
              export_cameras=False, export_lights=False, export_animations=False)
    bpy.ops.export_scene.gltf(**{k: v for k, v in kw.items() if k in props or k == "filepath"})
    bpy.data.objects.remove(obj, do_unlink=True)
    if fix is not None:
        bpy.data.meshes.remove(mesh)


def png16(path, n, rows):
    """rows: n rows of n uint16, north (top) first."""
    raw = bytearray()
    for r in rows:
        raw.append(0)
        raw += struct.pack(f">{n}H", *r)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 16, 0, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def blur(grid, sigma):
    import numpy as np
    r = int(math.ceil(sigma * 3))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    g = np.apply_along_axis(lambda m: np.convolve(m, k, mode="same"), 0, grid)
    return np.apply_along_axis(lambda m: np.convolve(m, k, mode="same"), 1, g)


def fit_ground(samples, waters, spacing):
    """Heightfield under scattered ground samples [(x, y, z)]: multi-scale normalised splatting, fine where samples
    are dense, smooth elsewhere. waters: [(xmin, ymin, xmax, ymax, level)] only widen the area covered; lake beds
    are not invented (objects say where land is, not where the source terrain dipped under the water)."""
    import numpy as np
    xs = np.array([s[0] for s in samples]); ys = np.array([s[1] for s in samples]); zs = np.array([s[2] for s in samples])
    lo = np.array([xs.min(), ys.min()]) - 60.0
    hi = np.array([xs.max(), ys.max()]) + 60.0
    for w in waters:
        lo = np.minimum(lo, [w[0] - 20, w[1] - 20]); hi = np.maximum(hi, [w[2] + 20, w[3] + 20])
    size = float(max(hi - lo))
    n = 2 ** int(math.ceil(math.log2(size / spacing))) + 1
    sp = size / (n - 1)
    org = (lo + hi) / 2 - size / 2
    ix = np.clip(np.round((xs - org[0]) / sp).astype(int), 0, n - 1)
    iy = np.clip(np.round((ys - org[1]) / sp).astype(int), 0, n - 1)
    acc = np.zeros((n, n)); wgt = np.zeros((n, n))
    np.add.at(acc, (iy, ix), zs); np.add.at(wgt, (iy, ix), 1.0)
    height = None
    for sigma_m in (60.0, 24.0, 10.0, 4.0):   # coarse to fine
        s = max(sigma_m / sp, 0.5)
        a, w = blur(acc, s), blur(wgt, s)
        h = np.where(w > 1e-6, a / np.maximum(w, 1e-12), 0.0)
        if height is None:
            height = np.where(w > 1e-9, h, zs.mean())
        else:
            conf = np.clip(w * (2.0 * math.pi * s * s) / 3.0, 0.0, 1.0)   # about 3 samples within sigma: trust it
            height = height * (1 - conf) + h * conf
    # objects sit on the ground or slightly into it: drop the fitted surface a little so nothing floats
    height -= 0.05
    return height, n, sp, [float(org[0]), float(org[1])]


# ---------------------------------------------------------------- main

def main():
    a = cli()
    scene_path = os.path.abspath(a.scene)
    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, "assets"), exist_ok=True)
    os.makedirs(os.path.join(out, "instances"), exist_ok=True)
    load_scene(scene_path)
    objs = list(bpy.context.scene.objects)
    report = {"source": scene_path, "warnings": [], "skipped_lods": 0, "mirrored": 0}

    # placements per mesh datablock; LOD1+ siblings dropped
    groups = defaultdict(list)
    keys = {}
    for o in objs:
        if o.type != "MESH" or not o.data.polygons:
            continue
        m = LOD_RE.match(o.name)
        if m and int(m.group("lod")) > 0:
            report["skipped_lods"] += 1
            continue
        base = m.group("base") if m else DUP_RE.sub("", o.name)
        groups[o.data.name].append(o)
        keys.setdefault(o.data.name, base)
    names = {}
    used = set()
    for data_name, base in keys.items():
        k = safe(base)
        i = 2
        while k.lower() in used:
            k, i = f"{safe(base)}_{i}", i + 1
        used.add(k.lower())
        names[data_name] = k

    doc = {"format": "dayfall-map", "name": a.name or os.path.splitext(os.path.basename(scene_path))[0],
           "notes": f"Converted from {os.path.basename(scene_path)} by tools/fbx_scene_to_dayfall.py. Materials are "
                    "guessed from their names unless a materials file was given; see scene_import_report.json.",
           "meshes": {}, "materials": {}, "instance_files": [], "objects": [], "lights": [], "cameras": {}}
    waters, ground = [], []
    mats_used = set()
    big = False
    for data_name, placed in sorted(groups.items(), key=lambda kv: names[kv[0]]):
        key = names[data_name]
        mesh = bpy.data.meshes[data_name]
        cat = category(key + " " + " ".join(s.name for s in mesh.materials if s))
        # exporters often keep meshes in centimetres with a 0.01 scale on every placement: bake the typical
        # placement scale into the mesh so assets are in metres and LOD / cull distances mean something
        scales = sorted(max(abs(v) for v in o.matrix_world.to_scale()) for o in placed)
        s0 = scales[len(scales) // 2] or 1.0
        # ... and its up axis: Unity / Max exports often stand meshes up with a -90 degree X turn on every
        # placement. Engine collision cylinders, foliage wind and size_m expect mesh Z up.
        up = Vector((0, 0, 0))
        for o in placed:
            up += o.matrix_world.to_quaternion().inverted() @ Vector((0, 0, 1))
        up /= len(placed)
        B = Matrix.Identity(3)
        if up.length > 0.85:
            k = max(range(3), key=lambda i: abs(up[i]))
            axis = Vector((0, 0, 0))
            axis[k] = 1.0 if up[k] > 0 else -1.0
            if axis != Vector((0, 0, 1)):
                B = axis.rotation_difference(Vector((0, 0, 1))).to_matrix()
                B = Matrix([[round(v) for v in row] for row in B])   # exact signed permutation
        Bq = B.to_quaternion()
        fix = B.to_4x4() @ Matrix.Scale(s0, 4)
        export_mesh(mesh, os.path.join(out, "assets", key + ".glb"), fix if fix != Matrix.Identity(4) else None)
        for s in mesh.materials:
            if s:
                mats_used.add(s.name)
        corners = [B @ (Vector(c) * s0) for c in placed[0].bound_box]
        mn = Vector((min(c.x for c in corners), min(c.y for c in corners), min(c.z for c in corners)))
        mx = Vector((max(c.x for c in corners), max(c.y for c in corners), max(c.z for c in corners)))
        size = mx - mn
        radius = size.length / 2
        tris = sum(len(p.vertices) - 2 for p in mesh.polygons)
        entry = {"file": f"assets/{key}.glb", "category": cat, "size_m": [round(v, 2) for v in size], "triangles": tris}
        if tris >= 600:
            entry["lod0_distance_m"] = round(max(20.0, radius * 8), 1)
            entry["lods"] = [{"ratio": 0.45, "distance_m": round(max(60.0, radius * 25), 1)}] + \
                ([{"ratio": 0.15, "distance_m": round(max(180.0, radius * 70), 1), "sloppy": True}] if tris >= 3000 else [])
        entry["cull_distance_m"] = round(min(3000.0, max(80.0, radius * 120)), 1) if cat == "ground_cover" else \
            round(min(4000.0, max(150.0, radius * 200)), 1)
        doc["meshes"][key] = entry
        if max(size.x, size.y) > 150 and cat != "water":
            big = True
        recs = []
        for o in placed:
            loc, rot, sc = o.matrix_world.decompose()
            rot = rot @ Bq.inverted()                                   # undo the turn baked into the mesh
            sc = Vector([abs(v) for v in (B @ sc)]) * (1 if sc.x * sc.y * sc.z > 0 else -1) / s0
            if sc.x * sc.y * sc.z < 0:
                report["mirrored"] += 1
            uniform = abs(sc.x - sc.y) < 1e-4 * max(1, abs(sc.x)) and abs(sc.x - sc.z) < 1e-4 * max(1, abs(sc.x)) and sc.x > 0
            recs.append((loc, rot, sc, uniform))
            if cat == "water":
                wc = [o.matrix_world @ Vector(c) for c in o.bound_box]
                waters.append((min(c.x for c in wc), min(c.y for c in wc), max(c.x for c in wc), max(c.y for c in wc), loc.z))
            elif cat in ("ground_cover", "tree", "shrub", "rock"):
                ground.append((loc.x, loc.y, loc.z))
        layout = "pos_scale_quat" if all(r[3] for r in recs) else "pos_quat_scale3"
        data = bytearray()
        for loc, rot, sc, _ in recs:
            q = (rot.x, rot.y, rot.z, rot.w)
            if layout == "pos_scale_quat":
                data += struct.pack("<8f", loc.x, loc.y, loc.z, sc.x, *q)
            else:
                data += struct.pack("<10f", loc.x, loc.y, loc.z, *q, sc.x, sc.y, sc.z)
        with open(os.path.join(out, "instances", key + ".bin"), "wb") as f:
            f.write(data)
        coll = {"ground_cover": "none", "shrub": "none", "water": "none",
                "tree": {"type": "cylinder", "radius": round(max(0.15, min(size.x, size.y) * 0.06), 2), "height": round(size.z * 0.6, 2)}
                }.get(cat, "mesh")
        fe = {"id": key, "file": f"instances/{key}.bin", "mesh": key, "count": len(recs), "collision": coll,
              "shadow": cat not in ("water",)}
        if layout != "pos_scale_quat":
            fe["layout"] = layout
        doc["instance_files"].append(fe)

    # materials
    given = {}
    if a.materials:
        with open(a.materials) as f:
            given = json.load(f)
    tex_out = os.path.join(out, "textures")
    for mname in sorted(mats_used):
        d = given.get(mname) or guess_material(mname)
        tex = find_textures(mname, a.textures) if not given.get(mname) else {}
        if tex:
            os.makedirs(tex_out, exist_ok=True)
            d = dict(d)
            d["textures"] = {}
            for slot, src in tex.items():
                dst = os.path.join(tex_out, os.path.basename(src))
                shutil.copyfile(src, dst)
                d["textures"][slot] = "textures/" + os.path.basename(src)
            if d.get("model") == "foliage":
                d.update(color_a=[1, 1, 1], color_b=[0.88, 0.92, 0.84], tip=[1, 1, 1])
            elif d.get("model") in ("lit", "rock"):
                d = {k: v for k, v in d.items() if k not in ("color", "color_dark", "moss_amount", "lichen_amount")}
                d["model"] = "lit"
                d["base_color"] = [1, 1, 1]
        doc["materials"][mname] = d
    report["materials"] = {m: ("given" if m in given else "textured" if "textures" in doc["materials"][m] else "guessed")
                           for m in sorted(mats_used)}

    # lights
    env = {"preset": "golden_hour"}
    for o in objs:
        if o.type != "LIGHT":
            continue
        if o.data.type == "SUN":
            d = (o.matrix_world.to_3x3() @ Vector((0, 0, 1))).normalized()   # Blender suns shine along -Z
            el = math.degrees(math.asin(max(-1, min(1, d.z))))
            az = math.degrees(math.atan2(d.x, d.y))
            env = {"preset": "noon" if el > 30 else "golden_hour" if el > 8 else "dusk",
                   "sun": {"elevation_deg": round(el, 2), "azimuth_deg": round(az, 2), "color": [round(c, 3) for c in o.data.color]}}
        elif o.data.type in ("POINT", "SPOT", "AREA"):
            p = o.matrix_world.translation
            inten = o.data.energy / (4 * math.pi)
            doc["lights"].append({"id": safe(o.name), "position": [round(p.x, 3), round(p.y, 3), round(p.z, 3)],
                                  "color": [round(c, 3) for c in o.data.color], "intensity": round(inten, 3),
                                  "range_m": round(max(4.0, min(40.0, math.sqrt(max(inten, 1e-3) / 0.02))), 1)})
    if waters:
        env["water"] = {"enabled": True, "level_m": round(min(w[4] for w in waters), 3), "plane": False}
    else:
        env["water"] = {"enabled": False}
    doc["environment"] = env

    # ground
    mode = a.ground or ("none" if big else "auto")
    if mode == "auto" and len(ground) >= 20:
        h, n, sp, org = fit_ground(ground, waters, a.spacing)
        os.makedirs(os.path.join(out, "terrain"), exist_ok=True)
        lo, hi = float(h.min()), float(h.max())
        q = ((h - lo) / max(hi - lo, 0.01) * 65535).round().astype(int)
        png16(os.path.join(out, "terrain", "height.png"), n, [list(q[j]) for j in range(n - 1, -1, -1)])
        doc["terrain"] = {"samples_per_side": n, "spacing_m": round(sp, 6), "origin": [round(org[0], 4), round(org[1], 4)],
                          "height_file": "terrain/height.png", "height_range_m": [round(lo, 4), round(hi, 4)],
                          "png_rows": "north_first", "paint_files": ["terrain/paint_a.png", "terrain/paint_b.png"],
                          "material": "terrain", "horizon": {"enabled": True}}
        report["ground"] = {"samples": len(ground), "size": n, "spacing_m": sp,
                            "note": "rebuilt from object origins; the source had no ground mesh. For the exact ground, "
                                    "export the source terrain's heightmap (Unity: Terrain > Export Raw, 16-bit; Unreal: "
                                    "the landscape) and load it with the terrain_import tool"}
        low = [w for w in waters if w[4] < float(h.min()) + 1.0 or w[4] < float(h.mean()) - 3.0]
        if low:
            report["warnings"].append(f"{len(low)} water plane(s) lie below most of the rebuilt ground (level "
                                      f"{low[0][4]:.2f} m, ground mean {float(h.mean()):.2f} m): their lakes come from the "
                                      "source terrain, which the file does not contain; import it with terrain_import")
    elif mode == "auto":
        report["warnings"].append("too few ground-standing objects to rebuild a ground")

    # player start and an overview camera
    pts = ground or [(0, 0, 0)]
    cx = sum(p[0] for p in pts) / len(pts); cy = sum(p[1] for p in pts) / len(pts)
    near = min(pts, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cy) ** 2)
    doc["player_start"] = {"position": [round(near[0] + 2, 2), round(near[1], 2)], "yaw_deg": 90}
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]; zs = [p[2] for p in pts]
    ext = max(max(xs) - min(xs), max(ys) - min(ys))
    doc["cameras"]["overview"] = {"position": [round(cx, 1), round(min(ys) - ext * 0.45, 1), round(max(zs) + ext * 0.35, 1)],
                                  "target": [round(cx, 1), round(cy, 1), round(sum(zs) / len(zs), 1)], "vfov_deg": 50}
    with open(os.path.join(out, "map.json"), "w") as f:
        json.dump(doc, f, indent=1)
    report["counts"] = {"meshes": len(doc["meshes"]), "instances": sum(e["count"] for e in doc["instance_files"]),
                        "materials": len(doc["materials"]), "lights": len(doc["lights"])}
    with open(os.path.join(out, "scene_import_report.json"), "w") as f:
        json.dump(report, f, indent=1)
    print(f"[scene] {report['counts']}; LODs skipped {report['skipped_lods']}; ground: {mode}; materials "
          f"{report['materials']}")


if __name__ == "__main__":
    main()
