"""Exports for Unreal Engine 5:

  exports/<preset>/
    heightmap_r16.png        16-bit landscape heightmap (Landscape > Import)
    weight_{grass,rock,snow,wet}.png   8-bit layer weightmaps
    meshes/SM_Terrain.fbx    terrain as a (Nanite-friendly) static mesh, vertex colours = masks
    meshes/SM_<asset>.fbx    one FBX per vegetation / rock variant
    scatter.json             every instance transform (Blender metres, Z up, right-handed)
    manifest.json            lighting, camera keys, scales, material slot names
"""
import json
import math
import os

import bpy
import numpy as np

from .png import write_png


def _select_only(objs):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]


def _fbx(path, objs):
    _select_only(objs)
    bpy.ops.export_scene.fbx(
        filepath=path, use_selection=True, object_types={"MESH"},
        apply_unit_scale=True, apply_scale_options="FBX_SCALE_UNITS",
        mesh_smooth_type="FACE", use_mesh_modifiers=True, colors_type="LINEAR",
        add_leaf_bones=False, bake_anim=False, axis_forward="-Z", axis_up="Y")


# --------------------------------------------------------------------------
# Blender -> Unreal conversion. Blender is right-handed (X right, Y forward,
# Z up, metres); Unreal is left-handed (X forward, Y right, Z up, cm). The
# FBX exporter mirrors meshes across Y, so a point maps as (x, -y, z) * 100
# and a rotation R maps as M R M with M = diag(1, -1, 1). Unreal rotators
# are then extracted exactly as FMatrix::Rotator() does.

def to_ue_location(x, y, z):
    return np.stack([x * 100.0, -y * 100.0, z * 100.0], 1)


def _euler_xyz_matrices(rx, ry, rz):
    cx, sx, cy, sy, cz, sz = np.cos(rx), np.sin(rx), np.cos(ry), np.sin(ry), np.cos(rz), np.sin(rz)
    n = len(rx)
    R = np.empty((n, 3, 3))
    # Blender XYZ Euler: R = Rz @ Ry @ Rx
    R[:, 0, 0] = cy * cz
    R[:, 0, 1] = sx * sy * cz - cx * sz
    R[:, 0, 2] = cx * sy * cz + sx * sz
    R[:, 1, 0] = cy * sz
    R[:, 1, 1] = sx * sy * sz + cx * cz
    R[:, 1, 2] = cx * sy * sz - sx * cz
    R[:, 2, 0] = -sy
    R[:, 2, 1] = sx * cy
    R[:, 2, 2] = cx * cy
    return R


def to_ue_rotators(rx, ry, rz):
    """Blender XYZ Euler (radians) -> Unreal (pitch, yaw, roll) degrees."""
    M = np.diag([1.0, -1.0, 1.0])
    R = M @ _euler_xyz_matrices(rx, ry, rz) @ M
    X, Y, Z = R[:, :, 0], R[:, :, 1], R[:, :, 2]
    pitch = np.arctan2(X[:, 2], np.hypot(X[:, 0], X[:, 1]))
    yaw = np.arctan2(X[:, 1], X[:, 0])
    sy_axis = np.stack([-np.sin(yaw), np.cos(yaw), np.zeros_like(yaw)], 1)
    roll = np.arctan2((Z * sy_axis).sum(1), (Y * sy_axis).sum(1))
    return np.degrees(np.stack([pitch, yaw, roll], 1))


def direction_to_ue_rotator(v):
    """A Blender direction vector -> Unreal rotator whose forward (+X) points along it."""
    x, y, z = v[0], -v[1], v[2]
    return {"pitch": math.degrees(math.asin(max(-1.0, min(1.0, z)))),
            "yaw": math.degrees(math.atan2(y, x)), "roll": 0.0}


def export_all(cfg, terrain, lib, scatter, cam_keys, out_dir, terrain_obj_lowres, terrain_obj_far=None):
    os.makedirs(os.path.join(out_dir, "meshes"), exist_ok=True)
    h = terrain.height
    hmin, hmax = float(h.min()), float(h.max())

    # heightmap: PNG row 0 is the top (north / +Y) of the image -> flip rows
    hm = np.round((h - hmin) / (hmax - hmin) * 65535.0)[::-1]
    write_png(os.path.join(out_dir, "heightmap_r16.png"), hm, 16)
    for k in ("grass", "rock", "snow", "wet"):
        write_png(os.path.join(out_dir, f"weight_{k}.png"),
                  np.round(np.clip(terrain.masks[k], 0, 1) * 255)[::-1], 8)

    # UE landscape import scales (cm). A 16-bit value of 32768 is z=0, the
    # full range at Z scale 100 is 512 m.
    res = terrain.res
    xy_scale_cm = terrain.size * 100.0 / (res - 1)
    z_scale = (hmax - hmin) / 512.0 * 100.0
    landscape = {
        "resolution": res, "xy_scale_cm": xy_scale_cm, "z_scale": z_scale,
        "location_cm": [-terrain.half * 100.0, -terrain.half * 100.0, (hmin + hmax) * 0.5 * 100.0],
        "min_height_m": hmin, "max_height_m": hmax,
        "note": "Landscape > Manage > Import from File: heightmap_r16.png, then set these scales/location.",
    }

    # meshes
    _fbx(os.path.join(out_dir, "meshes", "SM_Terrain.fbx"), [terrain_obj_lowres])
    if terrain_obj_far is not None:
        _fbx(os.path.join(out_dir, "meshes", "SM_TerrainFar.fbx"), [terrain_obj_far])
    assets = {}
    for cat, (_, objs) in lib.items():
        names = []
        for ob in objs:
            clean = ob.name.split("_", 1)[1]  # drop the ordering prefix
            _fbx(os.path.join(out_dir, "meshes", f"SM_{clean}.fbx"), [ob])
            names.append({"mesh": f"SM_{clean}", "materials": [m.name for m in ob.data.materials]})
        assets[cat] = names

    # scatter: per category, instances grouped by variant, already in Unreal
    # space (cm, left-handed) so the editor script only builds transforms
    sc = {}
    for cat, d in scatter.items():
        per = {}
        for v in np.unique(d["v"]):
            sel = d["v"] == v
            loc = to_ue_location(d["x"][sel], d["y"][sel], d["z"][sel])
            rot = to_ue_rotators(d["rx"][sel], d["ry"][sel], d["rz"][sel])
            rows = np.concatenate([loc, rot, d["s"][sel][:, None]], 1)
            per[assets[cat][int(v)]["mesh"]] = np.round(rows, 2).tolist()
        sc[cat] = per
    with open(os.path.join(out_dir, "scatter.json"), "w") as fh:
        json.dump({"units": "Unreal space: centimetres, X forward, Y right, Z up; rotator degrees",
                   "fields": ["x", "y", "z", "pitch", "yaw", "roll", "scale"], "instances": sc}, fh)

    for k in cam_keys:
        x, y, z = k["location_m"]
        k["ue_location_cm"] = [x * 100.0, -y * 100.0, z * 100.0]
        k["ue_rotation"] = direction_to_ue_rotator(k["forward"])

    lt = cfg["lighting"]
    el = math.radians(lt["sun_elevation_deg"])
    az = math.radians(lt["sun_azimuth_deg"])
    manifest = {
        "preset": cfg["name"],
        "seed": cfg["seed"],
        "terrain": {"size_m": terrain.size, "water_level_m": cfg["biome"]["water_level_m"],
                    "landscape": landscape, "mesh": "SM_Terrain",
                    "far_mesh": "SM_TerrainFar" if terrain_obj_far is not None else None},
        "assets": assets,
        "palette": cfg["palette"],
        "lighting": {**lt,
                     "sun_vector_blender": [math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), math.sin(el)],
                     # a directional light points along the light's travel, i.e. away from the sun
                     "ue_sun_rotation": direction_to_ue_rotator(
                         [-math.sin(az) * math.cos(el), -math.cos(az) * math.cos(el), -math.sin(el)])},
        "camera": {**{k: v for k, v in cfg["camera"].items() if k not in ("path", "stills")}, "keys": cam_keys},
    }
    with open(os.path.join(out_dir, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2)
    return manifest
