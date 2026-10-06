"""Deterministic scattering. Placements are computed once in numpy, rendered
in Blender through Geometry Nodes instancing, and exported verbatim for the
Unreal side so both engines show the same world."""
import math

import bpy
import numpy as np

from .noise import Perlin2D, fbm, smoothstep


def _camera_path_xy(cam_keys, n=200):
    ys = np.linspace(cam_keys[0]["world"][1], cam_keys[-1]["world"][1], n)
    xs = np.interp(ys, [k["world"][1] for k in cam_keys], [k["world"][0] for k in cam_keys])
    return np.stack([xs, ys], 1)


def _dist_to_path(px, py, path):
    d = np.full(px.shape, np.inf)
    for i in range(len(path) - 1):
        a, b = path[i], path[i + 1]
        ab = b - a
        t = np.clip(((px - a[0]) * ab[0] + (py - a[1]) * ab[1]) / max(ab @ ab, 1e-9), 0, 1)
        dx = px - (a[0] + t * ab[0])
        dy = py - (a[1] + t * ab[1])
        d = np.minimum(d, np.hypot(dx, dy))
    return d


def _rejection(rng, terrain, density_fn, count, bounds, batch=200000, max_iter=60):
    xs, ys = [], []
    got = 0
    for _ in range(max_iter):
        x = rng.uniform(bounds[0], bounds[1], batch)
        y = rng.uniform(bounds[2], bounds[3], batch)
        d = density_fn(x, y)
        keep = rng.uniform(0, 1, batch) < d
        xs.append(x[keep])
        ys.append(y[keep])
        got += int(keep.sum())
        if got >= count:
            break
    x = np.concatenate(xs)[:count]
    y = np.concatenate(ys)[:count]
    return x, y


def _align_to_normal(terrain, x, y, amount):
    n = terrain.normals()
    nx = terrain.sample(n[..., 0], x, y)
    ny = terrain.sample(n[..., 1], x, y)
    # small-angle Euler approximation that tilts +Z towards the normal
    return np.arcsin(np.clip(-ny, -1, 1)) * amount, np.arcsin(np.clip(nx, -1, 1)) * amount


def compute(cfg, terrain, cam_keys):
    b = cfg["biome"]
    rng = np.random.default_rng(cfg["seed"] + 1000)
    h_noise = Perlin2D(cfg["seed"] + 77)
    half = terrain.half * 0.98
    full = (-half, half, -half, half)
    path = _camera_path_xy(cam_keys)
    wl = b["water_level_m"]
    clear = b["camera_clearance_m"]

    def forest_density(x, y):
        h = terrain.height_at(x, y)
        slope = terrain.sample(terrain.slope_deg, x, y)
        patches = smoothstep(-0.15, 0.35, fbm(h_noise, x / 180.0, y / 180.0, 4))
        line = 1 - smoothstep(b["treeline_m"] - 70, b["treeline_m"], h + 30 * fbm(h_noise, x / 60, y / 60, 3))
        flat = 1 - smoothstep(30, 40, slope)
        dry_land = smoothstep(wl + 1.0, wl + 3.5, h)
        near_cam = smoothstep(clear, clear * 2.5, _dist_to_path(x, y, path))
        river = smoothstep(terrain.cfg["terrain"]["river_width_m"] * 1.5, 40.0,
                           terrain.sample(terrain.dist_river, x, y))
        return patches * line * flat * dry_land * near_cam * (0.35 + 0.65 * river)

    out = {}

    # ---- trees
    x, y = _rejection(rng, terrain, forest_density, b["tree_count"], full)
    h = terrain.height_at(x, y)
    conifer_p = np.clip(b["conifer_ratio"] + (h / max(b["treeline_m"], 1)) * 0.35, 0, 1)
    is_con = rng.uniform(0, 1, len(x)) < conifer_p
    variant = np.where(is_con, rng.integers(0, 4, len(x)), rng.integers(4, 7, len(x)))
    tilt_x, tilt_y = _align_to_normal(terrain, x, y, 0.15)
    out["trees"] = dict(x=x, y=y, z=h - 0.25, rx=tilt_x + rng.normal(0, 0.03, len(x)),
                        ry=tilt_y + rng.normal(0, 0.03, len(x)), rz=rng.uniform(0, 2 * np.pi, len(x)),
                        s=rng.uniform(0.7, 1.35, len(x)), v=variant)

    # ---- bushes: forest edges and river banks
    def bush_density(x, y):
        return np.clip(forest_density(x, y) * 1.4, 0, 1) * 0.6 + 0.4 * (
            1 - smoothstep(10, 45, terrain.sample(terrain.dist_river, x, y))) * smoothstep(wl + 0.6, wl + 1.5, terrain.height_at(x, y)) \
            * smoothstep(clear * 0.6, clear * 1.5, _dist_to_path(x, y, path))

    x, y = _rejection(rng, terrain, bush_density, b["bush_count"], full)
    out["bushes"] = dict(x=x, y=y, z=terrain.height_at(x, y) - 0.15, rx=np.zeros_like(x), ry=np.zeros_like(x),
                         rz=rng.uniform(0, 2 * np.pi, len(x)), s=rng.lognormal(0, 0.25, len(x)),
                         v=rng.integers(0, 3, len(x)))

    # ---- rocks: steep ground, scree and river banks
    def rock_density(x, y):
        slope = terrain.sample(terrain.slope_deg, x, y)
        r = terrain.sample(terrain.masks["rock"], x, y)
        bank = 1 - smoothstep(8, 30, terrain.sample(terrain.dist_river, x, y))
        return np.clip(0.15 + 0.6 * r + 0.25 * smoothstep(20, 35, slope) + 0.7 * bank, 0, 1) \
            * smoothstep(clear * 0.5, clear, _dist_to_path(x, y, path))

    x, y = _rejection(rng, terrain, rock_density, b["rock_count"], full)
    tx, ty = _align_to_normal(terrain, x, y, 0.8)
    s = rng.lognormal(0, 0.45, len(x))
    out["rocks"] = dict(x=x, y=y, z=terrain.height_at(x, y) - 0.1 * s, rx=tx + rng.normal(0, 0.15, len(x)),
                        ry=ty + rng.normal(0, 0.15, len(x)), rz=rng.uniform(0, 2 * np.pi, len(x)),
                        s=s, v=rng.integers(0, 5, len(x)))

    # ---- grass clumps: only near the camera path, where they are visible
    gr = b["grass_radius_m"]
    ymin = path[:, 1].min() - 30
    ymax = path[:, 1].max() + gr * 2.5
    xmin = path[:, 0].min() - gr * 1.6
    xmax = path[:, 0].max() + gr * 1.6

    def grass_density(x, y):
        g = terrain.sample(terrain.masks["grass"], x, y)
        d = _dist_to_path(x, y, path)
        # wider band ahead of the camera than to the sides
        # dense right next to the lens, thinning with distance (terrain
        # shading carries the far field)
        reach = (1 - smoothstep(gr * 0.5, gr * 1.6, d)) * (0.18 + 0.82 / (1.0 + (d / 28.0) ** 2))
        patch = smoothstep(-0.3, 0.2, fbm(h_noise, x / 25.0, y / 25.0, 3))
        return g * reach * (0.45 + 0.55 * patch) * smoothstep(wl + 0.15, wl + 0.5, terrain.height_at(x, y))

    x, y = _rejection(rng, terrain, grass_density, b["grass_count"], (xmin, xmax, ymin, ymax), batch=400000)
    flower = rng.uniform(0, 1, len(x)) < b["flower_ratio"]
    out["grass"] = dict(x=x, y=y, z=terrain.height_at(x, y), rx=rng.normal(0, 0.08, len(x)),
                        ry=rng.normal(0, 0.08, len(x)), rz=rng.uniform(0, 2 * np.pi, len(x)),
                        s=rng.uniform(0.7, 1.5, len(x)), v=np.where(flower, 3, rng.integers(0, 3, len(x))))
    return out


# --------------------------------------------------------------------------
def _instancer_group(name, collection):
    ng = bpy.data.node_groups.new(name, "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    N = ng.nodes
    gi = N.new("NodeGroupInput")
    go = N.new("NodeGroupOutput")
    ci = N.new("GeometryNodeCollectionInfo")
    ci.inputs["Collection"].default_value = collection
    ci.inputs["Separate Children"].default_value = True
    ci.inputs["Reset Children"].default_value = True
    iop = N.new("GeometryNodeInstanceOnPoints")
    iop.inputs["Pick Instance"].default_value = True

    def named(attr, dtype):
        na = N.new("GeometryNodeInputNamedAttribute")
        na.data_type = dtype
        na.inputs["Name"].default_value = attr
        return na.outputs["Attribute"]

    L = ng.links
    L.new(gi.outputs[0], iop.inputs["Points"])
    L.new(ci.outputs[0], iop.inputs["Instance"])
    L.new(named("variant", "INT"), iop.inputs["Instance Index"])
    erot = N.new("FunctionNodeEulerToRotation")
    L.new(named("rot", "FLOAT_VECTOR"), erot.inputs[0])
    L.new(erot.outputs[0], iop.inputs["Rotation"])
    L.new(named("scale", "FLOAT"), iop.inputs["Scale"])
    L.new(iop.outputs[0], go.inputs[0])
    return ng


def build_instancers(scatter, lib, coll):
    for cat, data in scatter.items():
        src_coll, _ = lib[cat]
        n = len(data["x"])
        me = bpy.data.meshes.new(f"SCATTER_{cat}")
        me.vertices.add(n)
        co = np.stack([data["x"], data["y"], data["z"]], 1).astype(np.float32)
        me.vertices.foreach_set("co", co.ravel())
        me.attributes.new("variant", "INT", "POINT").data.foreach_set("value", data["v"].astype(np.int32))
        rot = np.stack([data["rx"], data["ry"], data["rz"]], 1).astype(np.float32)
        me.attributes.new("rot", "FLOAT_VECTOR", "POINT").data.foreach_set("vector", rot.ravel())
        me.attributes.new("scale", "FLOAT", "POINT").data.foreach_set("value", data["s"].astype(np.float32))
        ob = bpy.data.objects.new(f"SCATTER_{cat}", me)
        coll.objects.link(ob)
        mod = ob.modifiers.new("Instancer", "NODES")
        mod.node_group = _instancer_group(f"GN_Instance_{cat}", src_coll)
    return True
