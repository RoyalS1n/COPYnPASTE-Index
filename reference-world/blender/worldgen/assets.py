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
    """Float layers carrying an optional per-vertex 'shading normal', plus a
    'leafvar' brightness used by the foliage shader."""
    bm.verts.layers.float.get("leafvar") or bm.verts.layers.float.new("leafvar")
    return [bm.verts.layers.float.get(n) or bm.verts.layers.float.new(n) for n in _NORMAL_LAYERS]


def _set_leafvar(bm, verts, value):
    lay = bm.verts.layers.float["leafvar"]
    for v in verts:
        v[lay] = value


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
    lvar = me.attributes.get("leafvar")
    if lvar is not None:
        # same per-vertex shading as vertex colour, for Unreal (FBX carries
        # colour attributes, not generic floats); 0 = unset -> 1.0, /1.5 to fit 0..1
        n = len(me.vertices)
        lv = np.zeros(n, dtype=np.float32)
        lvar.data.foreach_get("value", lv)
        lv = np.where(lv == 0, 1.0, lv) / 1.5
        col = me.color_attributes.new("LeafVar", "FLOAT_COLOR", "POINT")
        col.data.foreach_set("color", np.stack([lv, lv, lv, np.ones(n, np.float32)], 1).astype(np.float32).ravel())
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
        # shaded base -> sunlit, slightly yellow new growth at the tip
        k_ = float(rng.uniform(0.85, 1.15))
        for v_, val in zip(vs, (0.5, 0.95, 1.4, 0.95)):
            _set_leafvar(bm, [v_], val * k_)

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
            for sgi in range(segs + 1):
                _set_leafvar(bm, [centre[sgi], left[sgi], right[sgi]], 0.45 + 0.6 * sgi / segs)
            # fringe: tufts along both edges and at the tip
            tufts = max(5, int(length / (0.28 * k)))
            for i in range(tufts):
                u = (i + rng.uniform(0.2, 0.9)) / tufts
                si = min(segs, int(round(u * segs)))
                for side, sign in ((left, 1), (right, -1)):
                    base = side[si].co.copy()
                    ang = yaw + sign * rng.uniform(0.5, 1.2) * (1 - 0.5 * u)
                    d = Vector((math.cos(ang), math.sin(ang), -rng.uniform(0.1, 0.45))).normalized()
                    L = (0.64 - 0.25 * u) * k * rng.uniform(0.8, 1.2)
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
        # mostly clump-outward (soft volume) with some of the card's own
        # orientation kept, so the canopy still shows leafy texture
        outward = (Vector(p) + Vector((0, 0, radius * 0.3))).normalized()
        _set_normal(bm, vs, (outward * 0.6 + n * 0.4).normalized())
        depth = float(np.linalg.norm(p) / radius)
        _set_leafvar(bm, vs, float(rng.uniform(0.85, 1.15) * (0.5 + 0.75 * depth)))


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
    # crown silhouette per variant: wide spreading, tall ovoid, or leaning
    shape = int(seed) % 3
    if shape == 0:
        crown_r, sx, sz, lift, lean2 = height * 0.42, 1.0, 0.55, 0.18, Vector((0, 0, 0))
    elif shape == 1:
        crown_r, sx, sz, lift, lean2 = height * 0.27, 1.0, 1.35, 0.32, Vector((0, 0, 0))
    else:
        crown_r, sx, sz, lift, lean2 = height * 0.34, 1.15, 0.8, 0.22, Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), 0)).normalized() * height * 0.12
    crown_c = top + Vector((0, 0, height * lift)) + lean2
    # clumps sit on the crown's shell (with gaps), a few dark masses inside
    for _ in range(int(rng.integers(15, 22))):
        d = Vector(rng.normal(0, 1, 3)).normalized()
        d.z = abs(d.z) * 0.9 - 0.15
        off = Vector((d.x * crown_r * sx, d.y * crown_r, d.z * crown_r * sz)) * rng.uniform(0.62, 1.0)
        clumps.append((crown_c + off, height * rng.uniform(0.085, 0.14)))
    leaf_len = 0.18 * k
    for _ in range(3):
        c = crown_c + Vector(rng.normal(0, 0.25, 3)) * crown_r
        core = _blob(bm, c, crown_r * 0.42, rng, 1, 2, sz * 0.8, 0.45)
        for v in core:
            _set_normal(bm, [v], (v.co - c + Vector((0, 0, crown_r * 0.3))).normalized())
            _set_leafvar(bm, [v], 0.45)
    for c, r in clumps:
        core = _blob(bm, c, r * 0.5, rng, 1, 2, 0.8, 0.5)     # dense core: no see-through holes
        for v in core:
            _set_normal(bm, [v], (v.co - c + Vector((0, 0, r * 0.3))).normalized())
            _set_leafvar(bm, [v], 0.6)
        _leaf_cards(bm, c, r, rng, int(2300 * (r / (2.4 * k)) ** 2), leaf_len, 1)
    # root flare
    for a in np.linspace(0, 2 * math.pi, 5, endpoint=False) + rng.uniform(0, 1):
        d = Vector((math.cos(a), math.sin(a), 0))
        pts = [Vector((0, 0, 0.6)) + d * 0.15, d * 0.55 + Vector((0, 0, 0.1)), d * 0.95 + Vector((0, 0, -0.25))]
        _tapered_tube(bm, pts, [0.22 * k, 0.14 * k, 0.05 * k], 6, mat=0)
    return _obj(name, bm, mats, coll)


def bush(name, mats, coll, seed, size=1.6):
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    for _ in range(int(rng.integers(4, 8))):
        off = Vector((rng.normal(0, 0.35), rng.normal(0, 0.35), 0)) * size
        r = size * rng.uniform(0.35, 0.55)
        _blob(bm, off + Vector((0, 0, r * 0.55)), r, rng, 0, 2, 0.7, 0.5)
    return _obj(name, bm, mats, coll)


def boulder(name, mats, coll, seed, size=2.0, subdiv=5, fractures=7, flat=0.7):
    """Rock: displaced icosphere with planar fractures, flattened base."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    res = bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=size * 0.5)
    stretch = Vector(rng.uniform(0.7, 1.35, 3))
    stretch.z *= flat
    off = Vector(rng.uniform(-100, 100, 3))
    planes = [(Vector(rng.normal(0, 1, 3)).normalized(), rng.uniform(0.32, 0.45) * size) for _ in range(fractures)]
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


def grass_tall(name, mats, coll, seed):
    """Tall, wispy grass with drooping seed heads (catches backlight)."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    for _ in range(int(rng.integers(26, 38))):
        base = Vector((rng.normal(0, 0.12), rng.normal(0, 0.12), -0.02))
        h = rng.uniform(0.55, 1.05)
        w = rng.uniform(0.006, 0.012)
        yaw = rng.uniform(0, 2 * math.pi)
        lean = rng.uniform(0.1, 0.5)
        rot = Matrix.Rotation(yaw, 3, "Z")
        prev = None
        segs = 6
        for sgi in range(segs + 1):
            u = sgi / segs
            c = base + rot @ Vector((lean * h * u * u, 0, h * u - 0.2 * lean * h * u * u))
            side = rot @ Vector((0, w * (1 - 0.7 * u) + 0.001, 0))
            pair = (bm.verts.new(c - side), bm.verts.new(c + side), u)
            if prev:
                f = bm.faces.new((prev[0], prev[1], pair[1], pair[0]))
                for loop, vv in zip(f.loops, (prev[2], prev[2], pair[2], pair[2])):
                    loop[uv].uv = (0.5, vv)
            prev = pair
        if rng.uniform() < 0.6:  # seed head: a thin spindle at the tip
            tip = (prev[0].co + prev[1].co) / 2
            d = (rot @ Vector((lean, 0, 1))).normalized()
            head = bmesh.ops.create_cone(bm, cap_ends=True, segments=4, radius1=0.012, radius2=0.002,
                                         depth=0.09, matrix=Matrix.Translation(tip + d * 0.045) @ d.to_track_quat("Z", "Y").to_matrix().to_4x4())
            for f in {f for v in head["verts"] for f in v.link_faces}:
                for loop in f.loops:
                    loop[uv].uv = (0.5, 1.0)
    return _obj(name, bm, mats, coll)


def grass_short(name, mats, coll, seed):
    """Short, dense lawn grass for the near ground."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    for _ in range(int(rng.integers(70, 95))):
        base = Vector((rng.normal(0, 0.16), rng.normal(0, 0.16), -0.01))
        h = rng.uniform(0.07, 0.24)
        w = rng.uniform(0.006, 0.011)
        rot = Matrix.Rotation(rng.uniform(0, 2 * math.pi), 3, "Z")
        lean = rng.uniform(0.1, 0.6)
        pts = []
        for u in (0.0, 0.5, 1.0):
            c = base + rot @ Vector((lean * h * u * u, 0, h * u))
            side = rot @ Vector((0, w * (1 - 0.8 * u) + 0.0008, 0))
            pts.append((bm.verts.new(c - side), bm.verts.new(c + side), u))
        for a0, a1 in zip(pts[:-1], pts[1:]):
            f = bm.faces.new((a0[0], a0[1], a1[1], a1[0]))
            for loop, vv in zip(f.loops, (a0[2], a0[2], a1[2], a1[2])):
                loop[uv].uv = (0.5, vv)
    return _obj(name, bm, mats, coll)


def fern(name, mats, coll, seed, size=0.9):
    """Fern: arching fronds with paired, tapering pinnae."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    _normal_layers(bm)
    for fi in range(int(rng.integers(7, 12))):
        yaw = 2 * math.pi * fi / 10 + rng.uniform(-0.3, 0.3)
        L = size * rng.uniform(0.7, 1.15)
        rise = rng.uniform(0.5, 0.9)
        rot = Matrix.Rotation(yaw, 3, "Z")
        n_p = 14
        for i in range(1, n_p):
            u = i / n_p
            # rachis arcs up then droops
            c = rot @ Vector((L * u, 0, L * rise * math.sin(math.pi * u * 0.75) - 0.15 * L * u * u))
            tangent = (rot @ Vector((1, 0, rise * math.cos(math.pi * u * 0.75) * 2.3 - 0.3 * u))).normalized()
            plen = L * 0.22 * math.sin(math.pi * min(1.0, u * 1.05)) + 0.02
            for side in (-1, 1):
                perp = (rot @ Vector((0, side, 0)))
                d = (perp * 0.85 + tangent * 0.35 - Vector((0, 0, 0.25))).normalized()
                w = plen * 0.28
                q = tangent * w
                vs = [bm.verts.new(c - q * 0.5), bm.verts.new(c + d * plen * 0.5 + q * 0.3),
                      bm.verts.new(c + d * plen), bm.verts.new(c + d * plen * 0.5 - q * 0.6)]
                bm.faces.new(vs).material_index = 0
                _set_normal(bm, vs, (Vector((c.x, c.y, 0)).normalized() + Vector((0, 0, 1.2))).normalized())
    return _obj(name, bm, mats, coll)


# ------------------------------------------------------------------- marsh
def _strap(bm, base, rot, height, width, lean, droop, twist, segs, mat, uv=None, leaf=True):
    """Flat, tapering leaf strap that arches out and droops, twisting along
    its length. Per-vertex leafvar shades it dark at the base, light at the tip."""
    prev = None
    up = Vector((0, 0, 1))
    for k in range(segs + 1):
        u = k / segs
        c = base + rot @ Vector((lean * height * u * u, 0, height * u - droop * height * u ** 3))
        nxt = base + rot @ Vector((lean * height * min(1, u + 0.05) ** 2, 0,
                                   height * min(1, u + 0.05) - droop * height * min(1, u + 0.05) ** 3))
        tan = (nxt - c).normalized() if (nxt - c).length > 1e-6 else up
        side0 = rot @ Vector((0, 1, 0))
        side = (Matrix.Rotation(twist * u, 3, tan) @ side0) * (width * (1 - 0.85 * u) * 0.5 + 0.0008)
        pair = (bm.verts.new(c - side), bm.verts.new(c + side), u)
        if leaf:
            _set_leafvar(bm, pair[:2], 0.55 + 0.75 * u)
        if prev:
            f = bm.faces.new((prev[0], prev[1], pair[1], pair[0]))
            f.material_index = mat
            if uv is not None:
                for loop, vv in zip(f.loops, (prev[2], prev[2], pair[2], pair[2])):
                    loop[uv].uv = (0.5, vv)
        prev = pair


def cattail(name, mats, coll, seed):
    """Bulrush clump: strap leaves, stems, velvety brown seed heads with a
    thin spike above. mats: [reed leaves/stems, seed heads]."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    _normal_layers(bm)
    for _ in range(int(rng.integers(7, 13))):                       # stems
        base = Vector((rng.normal(0, 0.16), rng.normal(0, 0.16), -0.3))
        h = rng.uniform(1.4, 2.2)
        lean = Vector((rng.normal(0, 0.06), rng.normal(0, 0.06), 0))
        path = [base + Vector((0, 0, h * t)) + lean * (h * t) ** 1.5 for t in np.linspace(0, 1, 6)]
        rings = _tapered_tube(bm, path, list(np.linspace(0.011, 0.004, 6)), 5, mat=0)
        _set_leafvar(bm, [v for r in rings for v in r], 0.85)
        if rng.uniform() < 0.7:
            t0 = rng.uniform(0.72, 0.84)
            p0 = base + Vector((0, 0, h * t0)) + lean * (h * t0) ** 1.5
            p1 = p0 + (path[-1] - path[-2]).normalized() * rng.uniform(0.15, 0.24)
            _tapered_tube(bm, [p0, p0 + (p1 - p0) * 0.1, p1 - (p1 - p0) * 0.1, p1],
                          [0.012, 0.027, 0.027, 0.012], 10, mat=1)
    for _ in range(int(rng.integers(9, 15))):                       # leaves
        base = Vector((rng.normal(0, 0.14), rng.normal(0, 0.14), -0.3))
        rot = Matrix.Rotation(rng.uniform(0, 2 * math.pi), 3, "Z")
        _strap(bm, base, rot, rng.uniform(1.0, 1.8), rng.uniform(0.016, 0.026), rng.uniform(0.08, 0.32),
               rng.uniform(0.05, 0.3), rng.uniform(-1.2, 1.2), 8, 0)
    return _obj(name, bm, mats, coll)


def reed(name, mats, coll, seed):
    """Common reed clump: tall thin stems with alternate leaves and drooping,
    feathery plumes. mats: [reed leaves/stems, plume]."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    _normal_layers(bm)
    wind = Vector((rng.normal(0, 1), rng.normal(0, 1), 0)).normalized()
    for _ in range(int(rng.integers(14, 24))):
        base = Vector((rng.normal(0, 0.22), rng.normal(0, 0.22), -0.3))
        h = rng.uniform(1.9, 2.8)
        bend = wind * rng.uniform(0.05, 0.2)
        path = [base + Vector((0, 0, h * t)) + bend * (h * t) ** 2 / h for t in np.linspace(0, 1, 7)]
        rings = _tapered_tube(bm, path, list(np.linspace(0.008, 0.003, 7)), 4, mat=0)
        _set_leafvar(bm, [v for r in rings for v in r], 0.9)
        for k in range(int(rng.integers(5, 8))):                    # alternate leaves
            t = 0.2 + 0.6 * k / 7 + rng.uniform(-0.03, 0.03)
            node = base + Vector((0, 0, h * t)) + bend * (h * t) ** 2 / h
            rot = Matrix.Rotation(rng.uniform(0, 2 * math.pi) + k * math.pi, 3, "Z") @ Matrix.Rotation(1.1, 3, "Y")
            _strap(bm, node, rot, rng.uniform(0.25, 0.42), rng.uniform(0.014, 0.022), rng.uniform(0.2, 0.5),
                   rng.uniform(0.3, 0.7), rng.uniform(-0.6, 0.6), 4, 0)
        if rng.uniform() < 0.85:                                    # plume
            top = path[-1]
            for _ in range(int(rng.integers(30, 55))):
                u = rng.uniform(0, 1)
                at = top - Vector((0, 0, 0.32 * u))
                d = (wind * rng.uniform(0.4, 1.0) + Vector((rng.normal(0, 0.45), rng.normal(0, 0.45), -rng.uniform(0.1, 0.6))))
                d.normalize()
                L = rng.uniform(0.05, 0.12) * (1.2 - 0.6 * u)
                perp = d.cross(Vector((0, 0, 1)))
                perp = perp.normalized() * L * 0.18 if perp.length > 1e-5 else Vector((L * 0.18, 0, 0))
                vs = [bm.verts.new(at), bm.verts.new(at + d * L * 0.5 + perp), bm.verts.new(at + d * L),
                      bm.verts.new(at + d * L * 0.5 - perp)]
                bm.faces.new(vs).material_index = 1
    return _obj(name, bm, mats, coll)


def sedge(name, mats, coll, seed):
    """Sedge / rush tussock: dense, stiff, upright blades, a few brown seed
    spikes. UV.y runs root (0) -> tip (1). mats: [sedge blades, seed spikes]."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    for _ in range(int(rng.integers(50, 75))):
        base = Vector((rng.normal(0, 0.11), rng.normal(0, 0.11), -0.03))
        rot = Matrix.Rotation(rng.uniform(0, 2 * math.pi), 3, "Z")
        _strap(bm, base, rot, rng.uniform(0.38, 0.9), rng.uniform(0.006, 0.011), rng.uniform(0.04, 0.3),
               rng.uniform(0.0, 0.25), rng.uniform(-0.4, 0.4), 5, 0, uv=uv, leaf=False)
    for _ in range(int(rng.integers(2, 6))):
        base = Vector((rng.normal(0, 0.08), rng.normal(0, 0.08), 0))
        h = rng.uniform(0.6, 0.95)
        path = [base + Vector((0, 0, h * t)) for t in np.linspace(0, 1, 3)]
        _tapered_tube(bm, path, [0.003, 0.0025, 0.002], 4, mat=0)
        _tapered_tube(bm, [path[-1], path[-1] + Vector((0.01, 0, 0.05)), path[-1] + Vector((0.02, 0, 0.09))],
                      [0.008, 0.009, 0.003], 6, mat=1)
    return _obj(name, bm, mats, coll)


def lily_cluster(name, mats, coll, seed):
    """Water-lily cluster floating at z = 0: notched pads with slightly
    curled rims, a few open flowers (two petal rings and a golden centre)
    and closed buds. mats: [pads, petals, flower centres]."""
    rng = np.random.default_rng(seed)
    bm = bmesh.new()
    pads = []
    for k in range(int(rng.integers(5, 11))):
        c = Vector((rng.normal(0, 0.45), rng.normal(0, 0.45), 0.004 + 0.0025 * k))
        r = rng.uniform(0.1, 0.27)
        notch = rng.uniform(0, 2 * math.pi)
        segs = 22
        centre = bm.verts.new(c)
        inner, outer = [], []
        for i in range(segs + 1):
            a = notch + 0.22 + (2 * math.pi - 0.44) * i / segs
            curl = 0.012 * rng.uniform(0.3, 1.0) if rng.uniform() < 0.35 else 0.0
            inner.append(bm.verts.new(c + Vector((math.cos(a) * r * 0.55, math.sin(a) * r * 0.55, 0.001))))
            outer.append(bm.verts.new(c + Vector((math.cos(a) * r, math.sin(a) * r, curl))))
        for i in range(segs):
            bm.faces.new((centre, inner[i], inner[i + 1])).material_index = 0
            bm.faces.new((inner[i], outer[i], outer[i + 1], inner[i + 1])).material_index = 0
        pads.append((c, r))
    for c, r in pads[: int(rng.integers(1, 4))]:
        fc = c + Vector((rng.normal(0, r * 0.2), rng.normal(0, r * 0.2), 0.012))
        if rng.uniform() < 0.25:                                     # closed bud
            _tapered_tube(bm, [fc, fc + Vector((0, 0, 0.04)), fc + Vector((0, 0, 0.09))], [0.022, 0.026, 0.004], 8, 1)
            continue
        for ring, (n, L, tilt, w) in enumerate(((11, 0.085, 0.35, 0.028), (9, 0.065, 0.8, 0.024), (7, 0.045, 1.15, 0.02))):
            a0 = rng.uniform(0, 2 * math.pi)
            for i in range(n):
                a = a0 + 2 * math.pi * i / n
                d = Vector((math.cos(a) * math.cos(tilt), math.sin(a) * math.cos(tilt), math.sin(tilt)))
                side = Vector((-math.sin(a), math.cos(a), 0)) * w * 0.5
                base = fc + Vector((0, 0, 0.004 * ring))
                vs = [bm.verts.new(base), bm.verts.new(base + d * L * 0.45 + side), bm.verts.new(base + d * L + Vector((0, 0, 0.006))),
                      bm.verts.new(base + d * L * 0.45 - side)]
                bm.faces.new(vs).material_index = 1
        head = bmesh.ops.create_icosphere(bm, subdivisions=1, radius=0.016,
                                          matrix=Matrix.Translation(fc + Vector((0, 0, 0.022))))
        for f in {f for v in head["verts"] for f in v.link_faces}:
            f.material_index = 2
    return _obj(name, bm, mats, coll)


# --------------------------------------------------------------------------
def build_library(mats, root_coll, marsh=False):
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
    gm = [mats["grass"], mats["flowers"]]
    objs = [grass_clump(f"{i:02d}_Grass_{i}", gm, c, 500 + i) for i in range(3)]       # 0-2 meadow
    objs.append(grass_clump("03_Flowers_0", gm, c, 600, flowers=True))                 # 3   flowers
    objs += [grass_tall(f"{4 + i:02d}_GrassTall_{i}", gm, c, 700 + i) for i in range(2)]  # 4-5 tall
    objs.append(grass_short("06_GrassShort_0", gm, c, 800))                             # 6   short
    objs.append(grass_clump("07_Flowers_1", gm, c, 610, flowers=True))                 # 7   flowers
    lib["grass"] = (c, objs)

    c = cat("ferns")
    lib["ferns"] = (c, [fern(f"{i:02d}_Fern_{i}", [mats["fern"]], c, 900 + i, 0.8 + 0.3 * i) for i in range(3)])

    c = cat("pebbles")
    lib["pebbles"] = (c, [boulder(f"{i:02d}_Pebble_{i}", [mats["rock"]], c, 1000 + i, 0.35, subdiv=3, fractures=4, flat=0.55)
                          for i in range(4)])

    c = cat("outcrops")
    lib["outcrops"] = (c, [boulder(f"{i:02d}_Outcrop_{i}", [mats["rock"]], c, 1100 + i, 9.0, subdiv=5, fractures=12, flat=0.6)
                           for i in range(3)])

    if marsh:
        c = cat("reeds")
        rm = [mats["reed"], mats["cattail"]]
        objs = [cattail(f"{i:02d}_Cattail_{i}", rm, c, 1200 + i) for i in range(3)]          # 0-2
        objs += [reed(f"{3 + i:02d}_Reed_{i}", [mats["reed"], mats["plume"]], c, 1300 + i) for i in range(2)]  # 3-4
        lib["reeds"] = (c, objs)
        c = cat("sedges")
        lib["sedges"] = (c, [sedge(f"{i:02d}_Sedge_{i}", [mats["sedge"], mats["cattail"]], c, 1400 + i) for i in range(3)])
        c = cat("lilies")
        lib["lilies"] = (c, [lily_cluster(f"{i:02d}_Lily_{i}", [mats["lilypad"], mats["lilyflower"], mats["lilycenter"]],
                                          c, 1500 + i) for i in range(3)])
    return lib
