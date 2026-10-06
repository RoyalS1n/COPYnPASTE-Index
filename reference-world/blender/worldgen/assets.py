"""Procedural, original vegetation and rock meshes (bmesh). Each builder
returns a Blender object placed at the origin with its base at z=0.

Names are prefixed with a two-digit index because Geometry Nodes'
Collection Info (separate children) orders instances alphabetically and
the scatter 'variant' attribute indexes into that order."""
import math

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector, noise


_NORMAL_LAYERS = ("cn_x", "cn_y", "cn_z", "cn_set")


def _normal_layers(bm):
    """Float layers carrying an optional per-vertex 'shading normal'."""
    return [bm.verts.layers.float.get(n) or bm.verts.layers.float.new(n) for n in _NORMAL_LAYERS]


def _set_normal(bm, verts, n):
    lx, ly, lz, ls = _normal_layers(bm)
    for v in verts:
        v[lx], v[ly], v[lz], v[ls] = n.x, n.y, n.z, 1.0


def _obj(name, bm, mats, coll):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for m in mats:
        me.materials.append(m)
    me.shade_smooth()
    if me.attributes.get("cn_set") is not None:
        # foliage cards shade with normals pointing out of their clump, so
        # a canopy lights like one soft volume while its outline stays broken
        n = len(me.vertices)
        buf = {}
        for key in _NORMAL_LAYERS:
            arr = np.zeros(n, dtype=np.float32)
            me.attributes[key].data.foreach_get("value", arr)
            buf[key] = arr
        base = np.zeros(n * 3, dtype=np.float32)
        me.vertices.foreach_get("normal", base)
        base = base.reshape(n, 3)
        custom = np.stack([buf["cn_x"], buf["cn_y"], buf["cn_z"]], 1)
        use = buf["cn_set"] > 0.5
        base[use] = custom[use]
        me.normals_split_custom_set_from_vertices([tuple(v) for v in base])
        for key in _NORMAL_LAYERS:
            me.attributes.remove(me.attributes[key])
    ob = bpy.data.objects.new(name, me)
    coll.objects.link(ob)
    return ob


def _tapered_tube(bm, path, radii, sides=8, mat=0, uv_layer=None):
    """Sweep a ring along a polyline. path: list of Vector, radii: list."""
    rings = []
    for i, (p, r) in enumerate(zip(path, radii)):
        if i < len(path) - 1:
            d = (path[i + 1] - p).normalized()
        else:
            d = (p - path[i - 1]).normalized()
        q = d.to_track_quat("Z", "Y")
        ring = []
        for s in range(sides):
            a = 2 * math.pi * s / sides
            v = bm.verts.new(p + q @ Vector((math.cos(a) * r, math.sin(a) * r, 0)))
            ring.append(v)
        rings.append(ring)
    for i in range(len(rings) - 1):
        for s in range(sides):
            f = bm.faces.new((rings[i][s], rings[i][(s + 1) % sides],
                              rings[i + 1][(s + 1) % sides], rings[i + 1][s]))
            f.material_index = mat
    return rings


def _blob(bm, center, radius, rng, mat, subdiv=2, squash=0.8, rough=0.35):
    """Displaced icosphere: canopy clump / bush / rock base shape."""
    res = bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=radius,
                                     matrix=Matrix.Translation(center))
    off = Vector(rng.uniform(-100, 100, 3))
    for v in res["verts"]:
        local = v.co - center
        n = noise.fractal((local / radius) * 1.6 + off, 0.6, 2.0, 4)
        local *= 1.0 + rough * n
        local.z *= squash
        v.co = center + local
    for f in {f for v in res["verts"] for f in v.link_faces}:
        f.material_index = mat
    return res["verts"]


# --------------------------------------------------------------------------
def conifer(name, mats, coll, seed, height=16.0):
    """Spruce/fir: tapered trunk and whorls of drooping branch fans (solid
    mass) fringed with needle-tuft cards (feathery, broken silhouette)."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    _normal_layers(bm)  # create layers up front: adding them later invalidates BMVert refs
    k = height / 16.0
    path = [Vector((0, 0, z)) for z in np.linspace(-0.6, height, 8)]
    _tapered_tube(bm, path, list(np.linspace(0.34, 0.03, 8) * k), 7, mat=0)

    def card(c, dvec, L, W):
        perp = dvec.cross(Vector((0, 0, 1)))
        if perp.length < 1e-4:
            perp = Vector((1, 0, 0))
        perp = (Matrix.Rotation(rng.uniform(-0.5, 0.5), 3, dvec) @ perp.normalized()) * W * 0.5
        vs = [bm.verts.new(c), bm.verts.new(c + dvec * L * 0.4 + perp),
              bm.verts.new(c + dvec * L), bm.verts.new(c + dvec * L * 0.4 - perp)]
        bm.faces.new(vs).material_index = 1
        out = Vector((c.x, c.y, 0)).normalized() + Vector((0, 0, 0.6))
        _set_normal(bm, vs, out.normalized())

    tiers = int(rng.integers(15, 20))
    base_z = height * rng.uniform(0.1, 0.18)
    for t in range(tiers):
        f = t / (tiers - 1)
        z = base_z + (height * 0.97 - base_z) * f + rng.uniform(-0.12, 0.12) * k
        radius = (height * 0.28) * (1.0 - f) ** 1.05 + 0.3 * k
        arms = int(rng.integers(7, 10))
        yaw0 = rng.uniform(0, 2 * math.pi)
        for a in range(arms):
            yaw = yaw0 + 2 * math.pi * a / arms + rng.uniform(-0.25, 0.25)
            length = radius * rng.uniform(0.75, 1.1)
            droop = rng.uniform(0.25, 0.42) * length
            width = length * rng.uniform(0.22, 0.32)
            rot = Matrix.Rotation(yaw, 3, "Z")
            segs = 6
            left, right, centre = [], [], []
            for sgi in range(segs + 1):
                u = sgi / segs
                x = u * length
                zz = z - droop * u * u + 0.12 * length * u * (1 - u)
                w = width * (math.sin(math.pi * min(1.0, u * 1.3)) * 0.5 + 0.1) * (1 - 0.55 * u)
                centre.append(bm.verts.new(rot @ Vector((x, 0, zz + 0.04 * length))))
                left.append(bm.verts.new(rot @ Vector((x, w, zz - 0.03 * length))))
                right.append(bm.verts.new(rot @ Vector((x, -w, zz - 0.03 * length))))
            for sgi in range(segs):
                for side in (left, right):
                    bm.faces.new((centre[sgi], side[sgi], side[sgi + 1], centre[sgi + 1])).material_index = 1
            out = (rot @ Vector((1, 0, 0.6))).normalized()
            _set_normal(bm, centre + left + right, out)
            # fringe: tufts along both edges and at the tip
            tufts = max(3, int(length / (0.5 * k)))
            for i in range(tufts):
                u = (i + rng.uniform(0.2, 0.9)) / tufts
                si = min(segs, int(round(u * segs)))
                for side, sign in ((left, 1), (right, -1)):
                    base = side[si].co.copy()
                    ang = yaw + sign * rng.uniform(0.5, 1.2) * (1 - 0.5 * u)
                    d = Vector((math.cos(ang), math.sin(ang), -rng.uniform(0.1, 0.45))).normalized()
                    L = (0.95 - 0.35 * u) * k * rng.uniform(0.8, 1.2)
                    card(base, d, L, L * rng.uniform(0.4, 0.55))
            tip = centre[-1].co.copy()
            card(tip, (rot @ Vector((1, 0, -0.3))).normalized(), 0.8 * k, 0.45 * k)
    # slim inner core hides gaps between whorls when seen from a distance
    inner = bmesh.ops.create_cone(bm, cap_ends=False, segments=10, radius1=height * 0.11,
                                  radius2=0.04, depth=height * 0.8,
                                  matrix=Matrix.Translation((0, 0, base_z + height * 0.42)))
    for f in {f for v in inner["verts"] for f in v.link_faces}:
        f.material_index = 1
    return _obj(name, bm, mats, coll)


def _leaf_cards(bm, center, radius, rng, count, leaf_len, mat):
    """Scatter small diamond leaf cards in a shell around a clump centre.
    Card normals lean outward so the clump shades like a soft volume."""
    d = rng.normal(0, 1, (count, 3))
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    d[:, 2] *= 0.75
    r = radius * rng.uniform(0.55, 1.0, count) ** 0.5
    pts = d * r[:, None]
    nrm = d + rng.normal(0, 0.6, (count, 3))
    nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
    for p, n in zip(pts, nrm):
        n = Vector(n)
        t = n.orthogonal().normalized()
        b = n.cross(t)
        rot = Matrix.Rotation(rng.uniform(0, math.pi), 3, n)
        t, b = rot @ t, rot @ b
        L = leaf_len * rng.uniform(0.7, 1.3)
        c = center + Vector(p)
        vs = [bm.verts.new(c + t * L * 0.5), bm.verts.new(c + b * L * 0.28),
              bm.verts.new(c - t * L * 0.5), bm.verts.new(c - b * L * 0.28)]
        f = bm.faces.new(vs)
        f.material_index = mat
        _set_normal(bm, vs, (Vector(p) + Vector((0, 0, radius * 0.3))).normalized())


def broadleaf(name, mats, coll, seed, height=12.0):
    """Deciduous tree: curved trunk, a few limbs, and canopy clumps built
    from a dark inner core plus thousands of leaf cards for a soft,
    broken silhouette."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    _normal_layers(bm)  # create layers up front: adding them later invalidates BMVert refs
    k = height / 12.0
    lean = Vector((rng.uniform(-0.6, 0.6), rng.uniform(-0.6, 0.6), 0))
    trunk_h = height * rng.uniform(0.38, 0.5)
    path = [Vector((0, 0, -0.5)) + lean * (z / trunk_h) ** 2 + Vector((0, 0, z))
            for z in np.linspace(0, trunk_h, 7)]
    _tapered_tube(bm, path, list(np.linspace(0.45, 0.22, 7) * k), 9, mat=0)
    top = path[-1]
    clumps = []
    for b in range(int(rng.integers(4, 7))):
        yaw = rng.uniform(0, 2 * math.pi)
        up = rng.uniform(0.45, 0.95)
        d = Vector((math.cos(yaw) * (1 - up * 0.6), math.sin(yaw) * (1 - up * 0.6), up)).normalized()
        length = height * rng.uniform(0.28, 0.42)
        pts = [top + d * length * t + Vector((0, 0, -0.15 * length * t * t)) for t in np.linspace(0, 1, 5)]
        _tapered_tube(bm, pts, list(np.linspace(0.2, 0.05, 5) * k), 6, mat=0)
        clumps.append((pts[-1] + Vector((0, 0, height * 0.06)), height * rng.uniform(0.17, 0.24)))
    crown_c = top + Vector((0, 0, height * 0.25))
    crown_r = height * 0.33
    for _ in range(int(rng.integers(4, 7))):
        off = Vector(rng.normal(0, 1, 3)).normalized() * crown_r * rng.uniform(0.2, 0.7)
        off.z = abs(off.z) * 0.7
        clumps.append((crown_c + off, height * rng.uniform(0.16, 0.22)))
    leaf_len = 0.24 * k
    for c, r in clumps:
        core = _blob(bm, c, r * 0.68, rng, 1, 2, 0.85, 0.3)   # dense core: no see-through holes
        for v in core:
            _set_normal(bm, [v], (v.co - c + Vector((0, 0, r * 0.3))).normalized())
        _leaf_cards(bm, c, r, rng, int(1100 * (r / (2.4 * k)) ** 2), leaf_len, 1)
    return _obj(name, bm, mats, coll)


def bush(name, mats, coll, seed, size=1.6):
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    for _ in range(int(rng.integers(4, 8))):
        off = Vector((rng.normal(0, 0.35), rng.normal(0, 0.35), 0)) * size
        r = size * rng.uniform(0.35, 0.55)
        _blob(bm, off + Vector((0, 0, r * 0.55)), r, rng, 0, 2, 0.7, 0.5)
    return _obj(name, bm, mats, coll)


def boulder(name, mats, coll, seed, size=2.0):
    """Rock: displaced icosphere with planar fractures, flattened base."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    res = bmesh.ops.create_icosphere(bm, subdivisions=5, radius=size * 0.5)
    stretch = Vector(rng.uniform(0.7, 1.35, 3))
    stretch.z *= 0.7
    off = Vector(rng.uniform(-100, 100, 3))
    planes = [(Vector(rng.normal(0, 1, 3)).normalized(), rng.uniform(0.32, 0.45) * size) for _ in range(7)]
    for v in res["verts"]:
        p = v.co.copy()
        p = Vector((p.x * stretch.x, p.y * stretch.y, p.z * stretch.z))
        n = noise.fractal(p / size * 2.2 + off, 0.55, 2.1, 6)
        p *= 1.0 + 0.22 * n
        for pn, d in planes:  # chop with planes -> faceted, fractured look
            dist = p.dot(pn)
            if dist > d:
                p -= pn * (dist - d) * 0.92
        p.z = max(p.z, -size * 0.18)
        v.co = p
    # set base so the bulk sits on z=0 but some is buried
    zmin = min(v.co.z for v in bm.verts)
    for v in bm.verts:
        v.co.z -= zmin + size * 0.12
    return _obj(name, bm, mats, coll)


def grass_clump(name, mats, coll, seed, flowers=False):
    """Clump of curved, tapered blades. UV.y runs root (0) -> tip (1) for the
    gradient in the grass shader. Optional flower heads use material 1."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    blades = int(rng.integers(34, 50))
    for _ in range(blades):
        base = Vector((rng.normal(0, 0.15), rng.normal(0, 0.15), -0.02))
        h = rng.uniform(0.2, 0.6)
        w = rng.uniform(0.010, 0.020)
        yaw = rng.uniform(0, 2 * math.pi)
        lean = rng.uniform(0.05, 0.45)
        rot = Matrix.Rotation(yaw, 3, "Z")
        segs = 4
        prev = None
        for s in range(segs + 1):
            u = s / segs
            x = lean * h * u * u
            z = h * u - 0.15 * lean * h * u * u
            ww = w * (1 - u) + 0.0015
            c = base + rot @ Vector((x, 0, z))
            side = rot @ Vector((0, ww, 0))
            pair = (bm.verts.new(c - side), bm.verts.new(c + side), u)
            if prev:
                f = bm.faces.new((prev[0], prev[1], pair[1], pair[0]))
                f.material_index = 0
                for loop, vv in zip(f.loops, (prev[2], prev[2], pair[2], pair[2])):
                    loop[uv].uv = (0.5, vv)
            prev = pair
    if flowers:
        for _ in range(int(rng.integers(2, 5))):
            base = Vector((rng.normal(0, 0.08), rng.normal(0, 0.08), 0))
            h = rng.uniform(0.35, 0.65)
            path = [base + Vector((0, 0, z)) for z in np.linspace(0, h, 3)]
            _tapered_tube(bm, path, [0.004, 0.003, 0.002], 4, mat=0)
            head = bmesh.ops.create_icosphere(bm, subdivisions=1, radius=0.025,
                                              matrix=Matrix.Translation(path[-1]) @ Matrix.Diagonal((1.6, 1.6, 0.5, 1)))
            for f in {f for v in head["verts"] for f in v.link_faces}:
                f.material_index = 1
    return _obj(name, bm, mats, coll)


# --------------------------------------------------------------------------
def build_library(mats, root_coll):
    """Creates one collection per scatter category. Returns
    {category: (collection, [object names in instance order])}."""
    lib = {}

    def cat(name):
        c = bpy.data.collections.new("LIB_" + name)
        root_coll.children.link(c)
        return c

    c = cat("trees")
    objs = []
    for i in range(4):
        objs.append(conifer(f"{i:02d}_Conifer_{i}", [mats["bark"], mats["needles"]], c, 100 + i, 14.0 + 3.0 * i))
    for i in range(3):
        objs.append(broadleaf(f"{4 + i:02d}_Broadleaf_{i}", [mats["bark"], mats["leaves"]], c, 200 + i, 10.0 + 2.5 * i))
    lib["trees"] = (c, objs)

    c = cat("bushes")
    lib["bushes"] = (c, [bush(f"{i:02d}_Bush_{i}", [mats["bush"]], c, 300 + i, 1.2 + 0.5 * i) for i in range(3)])

    c = cat("rocks")
    lib["rocks"] = (c, [boulder(f"{i:02d}_Rock_{i}", [mats["rock"]], c, 400 + i, 1.5 + 0.8 * i) for i in range(5)])

    c = cat("grass")
    objs = [grass_clump(f"{i:02d}_Grass_{i}", [mats["grass"], mats["flowers"]], c, 500 + i) for i in range(3)]
    objs.append(grass_clump("03_Flowers_0", [mats["grass"], mats["flowers"]], c, 600, flowers=True))
    lib["grass"] = (c, objs)
    return lib
