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
    target = look_target(cfg, terrain)
    for i, k in enumerate(keys):
        if target is not None:
            # aim at the target; yaw/pitch are framing offsets from that aim
            aim(k, k["world"], target)
        else:
            # heading follows the path tangent, plus the per-key yaw offset
            a = keys[max(i - 1, 0)]["world"]
            b = keys[min(i + 1, len(keys) - 1)]["world"]
            k["bearing"] = math.degrees(math.atan2(b[0] - a[0], b[1] - a[1])) + k["yaw"]
    return keys


def look_target(cfg, terrain, name=None):
    name = name or cfg["camera"].get("look_at")
    if name == "castle" and getattr(terrain, "castle", None):
        cx, cy = terrain.castle["center"]
        return (cx, cy, terrain.castle["top"] + 14.0)
    return None


def aim(k, pos, target):
    dx, dy, dz = (target[0] - pos[0], target[1] - pos[1], target[2] - pos[2])
    k["bearing"] = math.degrees(math.atan2(dx, dy)) + k.get("yaw", 0.0)
    k["pitch"] = math.degrees(math.atan2(dz, math.hypot(dx, dy))) + k.get("pitch_offset", k.get("pitch", 0.0))


def static_shot(cfg, terrain, shot, coll):
    """A fixed camera for an extra still, e.g. a long-lens castle portrait."""
    f = shot["from"]
    wl = cfg["biome"]["water_level_m"]
    x = float(terrain.river_center(np.array(f["y"]))) + f.get("x", 0.0)
    ground = max(float(terrain.height_at(np.array(x), np.array(f["y"]))), wl)
    pos = (x, f["y"], ground + f.get("height", 2.0))
    k = {"yaw": shot.get("yaw", 0.0), "pitch": shot.get("pitch", 0.0)}
    tgt = look_target(cfg, terrain, shot.get("look_at"))
    if tgt is not None:
        aim(k, pos, tgt)
    else:
        k["bearing"] = k["yaw"]
    data = bpy.data.cameras.new("RW_Shot_" + shot["name"])
    data.lens = shot.get("lens_mm", cfg["camera"]["lens_mm"])
    data.sensor_width = cfg["camera"]["sensor_mm"]
    data.clip_start = 0.1
    data.clip_end = 30000
    data.dof.use_dof = True
    data.dof.aperture_fstop = shot.get("fstop", 8.0)
    if tgt is not None:
        data.dof.focus_distance = math.dist(pos, tgt)
    ob = bpy.data.objects.new("RW_Shot_" + shot["name"], data)
    ob.location = pos
    ob.rotation_euler = (math.radians(90 + k["pitch"]), 0.0, -math.radians(k["bearing"]))
    coll.objects.link(ob)
    return ob


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
    # focus on the look-at target when there is one, else a third into the scene
    if "focus_m" in cfg["camera"]:
        data.dof.focus_distance = cfg["camera"]["focus_m"]
    else:
        data.dof.focus_distance = keys[0].get("focus", 120.0)
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
