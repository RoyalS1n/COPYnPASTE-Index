"""Cinematic camera: a slow, low flight up the river, keyed from the preset.
Key positions are resolved against the terrain so the camera always clears
the ground and water."""
import math

import bpy
import numpy as np


def resolve_keys(cfg, terrain):
    wl = cfg["biome"]["water_level_m"]
    keys = []
    for k in cfg["camera"]["path"]:
        y = k["y"]
        x = float(terrain.river_center(np.array(y))) + k["x"]
        ground = max(float(terrain.height_at(np.array(x), np.array(y))), wl)
        keys.append(dict(k, world=(x, y, ground + k["height"])))
    # heading follows the path tangent, plus the per-key yaw offset
    for i, k in enumerate(keys):
        a = keys[max(i - 1, 0)]["world"]
        b = keys[min(i + 1, len(keys) - 1)]["world"]
        bearing = math.degrees(math.atan2(b[0] - a[0], b[1] - a[1]))
        k["bearing"] = bearing + k["yaw"]
    return keys


def build(cfg, keys, coll, scene):
    c = cfg["camera"]
    data = bpy.data.cameras.new("RW_Camera")
    data.lens = c["lens_mm"]
    data.sensor_width = c["sensor_mm"]
    data.clip_start = 0.1
    data.clip_end = 30000
    data.dof.use_dof = True
    data.dof.aperture_fstop = c["fstop"]
    ob = bpy.data.objects.new("RW_Camera", data)
    coll.objects.link(ob)
    scene.camera = ob
    scene.frame_start = 1
    scene.frame_end = c["frames"]
    scene.render.fps = c["fps"]

    for k in keys:
        ob.location = k["world"]
        ob.rotation_euler = (math.radians(90 + k["pitch"]), 0.0, -math.radians(k["bearing"]))
        ob.keyframe_insert("location", frame=k["frame"])
        ob.keyframe_insert("rotation_euler", frame=k["frame"])
    # focus roughly a third of the way into the scene
    data.dof.focus_distance = 120.0
    return ob


def keys_for_export(keys):
    out = []
    for k in keys:
        b = math.radians(k["bearing"])
        p = math.radians(k["pitch"])
        fwd = (math.sin(b) * math.cos(p), math.cos(b) * math.cos(p), math.sin(p))
        out.append({"frame": k["frame"], "location_m": list(k["world"]), "forward": list(fwd),
                    "bearing_deg": k["bearing"], "pitch_deg": k["pitch"]})
    return out
