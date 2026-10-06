"""Procedural castle: an original design assembled from architectural parts
(curtain walls, round towers, gatehouse, keep with turrets, spire, hall and
houses). Every surface gets real-scale UVs (metres) so stone and slate
tiling read correctly. Foundations are extended down to the terrain, so
walls on the cliff edge grow out of the rock."""
import math

import bmesh
import bpy
import numpy as np
from mathutils import Vector

STONE, ROOF, WOOD, WIN_DARK, WIN_LIT, CLOTH, ROOF_ALT = range(7)


class Builder:
    def __init__(self):
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")

    def face(self, pts, mat, uvs):
        vs = [self.bm.verts.new(p) for p in pts]
        f = self.bm.faces.new(vs)
        f.material_index = mat
        for loop, uv in zip(f.loops, uvs):
            loop[self.uv].uv = uv
        return f

    # ------------------------------------------------------------- primitives
    def box(self, c, sx, sy, z0, z1, yaw, mat=STONE, top=True, bottom=False, u0=0.0):
        """Axis box of footprint sx*sy centred on c (xy), rotated by yaw."""
        ca, sa = math.cos(yaw), math.sin(yaw)

        def P(x, y, z):
            return Vector((c[0] + x * ca - y * sa, c[1] + x * sa + y * ca, z))

        hx, hy = sx / 2, sy / 2
        corners = [(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)]
        u = u0
        for i in range(4):
            a, b = corners[i], corners[(i + 1) % 4]
            L = math.dist(a, b)
            self.face([P(*a, z0), P(*b, z0), P(*b, z1), P(*a, z1)], mat,
                      [(u, 0), (u + L, 0), (u + L, z1 - z0), (u, z1 - z0)])
            u += L
        if top:
            self.face([P(*p, z1) for p in corners], mat, [(x, y) for x, y in corners])
        if bottom:
            self.face([P(*p, z0) for p in reversed(corners)], mat, [(x, y) for x, y in reversed(corners)])

    def segment_box(self, p0, p1, thick, z0, z1, mat=STONE, top=True):
        d = Vector((p1[0] - p0[0], p1[1] - p0[1]))
        mid = ((p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2)
        self.box(mid, d.length, thick, z0, z1, math.atan2(d.y, d.x), mat, top)

    def frustum(self, c, r0, r1, z0, z1, seg=24, mat=STONE, cap=False):
        u_scale = (r0 + r1) / 2
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            pts = [Vector((c[0] + r0 * math.cos(a0), c[1] + r0 * math.sin(a0), z0)),
                   Vector((c[0] + r0 * math.cos(a1), c[1] + r0 * math.sin(a1), z0)),
                   Vector((c[0] + r1 * math.cos(a1), c[1] + r1 * math.sin(a1), z1)),
                   Vector((c[0] + r1 * math.cos(a0), c[1] + r1 * math.sin(a0), z1))]
            self.face(pts, mat, [(a0 * u_scale, 0), (a1 * u_scale, 0), (a1 * u_scale, z1 - z0), (a0 * u_scale, z1 - z0)])
        if cap:
            pts = [Vector((c[0] + r1 * math.cos(2 * math.pi * i / seg), c[1] + r1 * math.sin(2 * math.pi * i / seg), z1))
                   for i in range(seg)]
            self.face(pts, mat, [(p.x, p.y) for p in pts])

    def cone(self, c, r, z0, h, seg=24, mat=ROOF, eave=0.5):
        """Conical roof with a short flared eave."""
        apex = Vector((c[0], c[1], z0 + h))
        slant = math.hypot(r, h)
        re = r + eave
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            b0 = Vector((c[0] + r * math.cos(a0), c[1] + r * math.sin(a0), z0))
            b1 = Vector((c[0] + r * math.cos(a1), c[1] + r * math.sin(a1), z0))
            self.face([b0, b1, apex], mat, [(a0 * r, 0), (a1 * r, 0), ((a0 + a1) / 2 * r, slant)])
            e0 = Vector((c[0] + re * math.cos(a0), c[1] + re * math.sin(a0), z0 - eave * 0.6))
            e1 = Vector((c[0] + re * math.cos(a1), c[1] + re * math.sin(a1), z0 - eave * 0.6))
            self.face([e0, e1, b1, b0], mat, [(a0 * r, -eave), (a1 * r, -eave), (a1 * r, 0), (a0 * r, 0)])
        # finial
        self.frustum(c, 0.12, 0.02, z0 + h - 0.3, z0 + h + 1.6, 6, WOOD)

    def hip_roof(self, c, sx, sy, z0, h, yaw, mat=ROOF, eave=0.6):
        ca, sa = math.cos(yaw), math.sin(yaw)

        def P(x, y, z):
            return Vector((c[0] + x * ca - y * sa, c[1] + x * sa + y * ca, z))

        hx, hy = sx / 2 + eave, sy / 2 + eave
        ridge = max(0.0, hx - hy)
        r0, r1 = P(-ridge, 0, z0 + h), P(ridge, 0, z0 + h)
        b = [P(-hx, -hy, z0 - eave * 0.5), P(hx, -hy, z0 - eave * 0.5), P(hx, hy, z0 - eave * 0.5), P(-hx, hy, z0 - eave * 0.5)]
        sl = math.hypot(hy, h)
        self.face([b[0], b[1], r1, r0], mat, [(0, 0), (2 * hx, 0), (hx + ridge, sl), (hx - ridge, sl)])
        self.face([b[2], b[3], r0, r1], mat, [(0, 0), (2 * hx, 0), (hx + ridge, sl), (hx - ridge, sl)])
        sl2 = math.hypot(hx - ridge, h)
        self.face([b[1], b[2], r1], mat, [(0, 0), (2 * hy, 0), (hy, sl2)])
        self.face([b[3], b[0], r0], mat, [(0, 0), (2 * hy, 0), (hy, sl2)])

    def gable_roof(self, c, length, depth, z0, h, yaw, mat=ROOF, eave=0.5):
        ca, sa = math.cos(yaw), math.sin(yaw)

        def P(x, y, z):
            return Vector((c[0] + x * ca - y * sa, c[1] + x * sa + y * ca, z))

        hl, hd = length / 2 + eave, depth / 2 + eave
        sl = math.hypot(hd, h + eave * 0.5)
        for s in (-1, 1):
            pts = [P(-hl, s * hd, z0 - eave * 0.5), P(hl, s * hd, z0 - eave * 0.5), P(hl, 0, z0 + h), P(-hl, 0, z0 + h)]
            if s > 0:
                pts = pts[::-1]
            self.face(pts, mat, [(0, 0), (2 * hl, 0), (2 * hl, sl), (0, sl)])
        for s in (-1, 1):  # stone gable ends, wound so normals face outward
            x = s * length / 2
            pts = [P(x, -depth / 2, z0), P(x, depth / 2, z0), P(x, 0, z0 + h)]
            uvs = [(0, 0), (depth, 0), (depth / 2, h)]
            if s < 0:
                pts, uvs = [pts[1], pts[0], pts[2]], [uvs[1], uvs[0], uvs[2]]
            self.face(pts, STONE, uvs)

    def opening(self, base, normal, w, h, mat, arch=True, depth=0.05):
        """Window/door: an arched dark panel standing just off a wall."""
        n = Vector((normal[0], normal[1], 0)).normalized()
        t = Vector((-n.y, n.x, 0))
        o = Vector(base) + n * depth
        pts = [o - t * w / 2, o + t * w / 2]
        if arch:
            spring = h - w / 2
            pts += [o + t * (w / 2 * math.cos(a)) + Vector((0, 0, spring + w / 2 * math.sin(a)))
                    for a in np.linspace(0, math.pi, 7)]
        else:
            pts += [o + t * w / 2 + Vector((0, 0, h)), o - t * w / 2 + Vector((0, 0, h))]
        self.face(pts, mat, [(0, 0)] * len(pts))

    def merlons_line(self, p0, p1, z, thick, outer_shift, mw=1.0, gap=0.75, mh=0.85, ph=0.95):
        """Parapet on the outer edge of a wall top plus merlons above it."""
        d = Vector((p1[0] - p0[0], p1[1] - p0[1]))
        L = d.length
        if L < 0.5:
            return
        dn = d / L
        nrm = Vector((dn.y, -dn.x))
        off = nrm * outer_shift
        a = Vector(p0) + off
        self.segment_box(a, a + d, thick, z, z + ph)
        n = int(L // (mw + gap))
        start = (L - n * (mw + gap) + gap) / 2
        for i in range(n):
            m0 = a + dn * (start + i * (mw + gap))
            self.segment_box(m0, m0 + dn * mw, thick, z + ph, z + ph + mh)

    def merlons_ring(self, c, r, z, seg=None, mw=1.0, mh=0.85, ph=0.95):
        seg = seg or max(8, int(2 * math.pi * r / 1.75))
        self.frustum(c, r, r, z, z + ph, max(12, seg * 2), STONE)
        self.frustum(c, r - 0.5, r - 0.5, z, z + ph, max(12, seg * 2), STONE)
        for i in range(seg):
            a = 2 * math.pi * i / seg
            p = (c[0] + (r - 0.25) * math.cos(a), c[1] + (r - 0.25) * math.sin(a))
            self.box(p, mw, 0.5, z + ph, z + ph + mh, a + math.pi / 2)


# ---------------------------------------------------------------------------
def build(cfg, terrain, mats, coll):
    c = cfg["castle"]
    site = terrain.castle
    if not site:
        return None
    rng = np.random.default_rng(c["seed"])
    B = Builder()
    cx, cy = site["center"]
    top = site["top"]
    pr = site["plateau_r"]
    gate_a = site["gate_angle"]
    wall_h, thick = c["wall_height_m"], c["wall_thickness_m"]
    lit = c["lit_windows"]

    def ground_min(xs, ys):
        return float(np.min(terrain.height_at(np.asarray(xs), np.asarray(ys))))

    def win_mat():
        return WIN_LIT if rng.uniform() < lit else WIN_DARK

    # ---- curtain wall polygon; edge (n-1 -> 0) straddles the gate direction
    n = int(c["tower_count"])
    step = 2 * math.pi / n
    angs = [gate_a + (i + 0.5) * step + rng.uniform(-0.12, 0.12) * step for i in range(n)]
    rads = [(pr - 6.5) * rng.uniform(0.86, 1.0) for _ in range(n)]
    P = [(cx + r * math.cos(a), cy + r * math.sin(a)) for a, r in zip(angs, rads)]
    wall_top = top + wall_h

    def wall_run(p0, p1):
        d = Vector((p1[0] - p0[0], p1[1] - p0[1]))
        if d.length < 0.5:
            return
        nrm = Vector((d.y, -d.x)).normalized()
        if nrm.dot(Vector(((p0[0] + p1[0]) / 2 - cx, (p0[1] + p1[1]) / 2 - cy))) < 0:
            nrm = -nrm  # point outward
        ts = np.linspace(0, 1, 9)
        xs = [p0[0] + t * d.x + s * nrm.x * thick for t in ts for s in (-0.5, 0.5, 1.6)]
        ys = [p0[1] + t * d.y + s * nrm.y * thick for t in ts for s in (-0.5, 0.5, 1.6)]
        z0 = ground_min(xs, ys) - 2.0
        # sloped plinth (batter) where the wall meets rock, then the wall
        B.segment_box(Vector(p0) + nrm * 0.6, Vector(p1) + nrm * 0.6, thick + 1.2, z0, min(top + 1.0, wall_top - 4))
        B.segment_box(p0, p1, thick, z0, wall_top)
        B.merlons_line(p0, p1, wall_top, 0.55, thick / 2 - 0.27)
        # arrow slits on the outer face
        for t in np.linspace(0.15, 0.85, max(1, int(d.length / 7))):
            base = Vector((p0[0] + t * d.x, p0[1] + t * d.y, wall_top - 4.2)) + nrm.to_3d() * (thick / 2)
            B.opening(base, nrm, 0.28, 1.5, WIN_DARK, arch=False)
        return nrm

    gate_mid = ((P[-1][0] + P[0][0]) / 2, (P[-1][1] + P[0][1]) / 2)
    gdir = Vector((P[0][0] - P[-1][0], P[0][1] - P[-1][1])).normalized()
    for i in range(n - 1):
        wall_run(P[i], P[i + 1])
    gw = 4.6
    wall_run(P[-1], (gate_mid[0] - gdir.x * gw, gate_mid[1] - gdir.y * gw))
    wall_run((gate_mid[0] + gdir.x * gw, gate_mid[1] + gdir.y * gw), P[0])

    # ---- gatehouse
    out = Vector((gate_mid[0] - cx, gate_mid[1] - cy)).normalized()
    gyaw = math.atan2(gdir.y, gdir.x)
    gz0 = ground_min([gate_mid[0]], [gate_mid[1]]) - 1.5
    gh = wall_top + 5.0
    B.box(gate_mid, 2 * gw + 0.5, 8.0, gz0, gh, gyaw)
    B.merlons_line(Vector(gate_mid) - gdir * gw + out * 4.0, Vector(gate_mid) + gdir * gw + out * 4.0, gh, 0.55, -0.3)
    for s in (-1, 1):
        tc = Vector(gate_mid) + gdir * s * (gw + 0.6) + out * 2.6
        tz0 = ground_min([tc.x], [tc.y]) - 2.0
        B.frustum(tc, 3.9, 3.3, tz0, top + 2.0, 20)
        B.frustum(tc, 3.3, 3.3, top + 2.0, gh + 2.5, 20)
        B.frustum(tc, 3.6, 3.6, gh + 2.5, gh + 3.3, 20, cap=True)
        B.merlons_ring(tc, 3.6, gh + 3.3)
        for k in range(2):
            a = math.atan2(out.y, out.x) + s * 0.5
            base = Vector((tc.x + 3.3 * math.cos(a), tc.y + 3.3 * math.sin(a), top + 6 + k * 5))
            B.opening(base, (math.cos(a), math.sin(a)), 0.3, 1.5, WIN_DARK, arch=False)
    gate_face = Vector(gate_mid) + out * 4.0
    B.opening(Vector((gate_face.x, gate_face.y, top - 0.2)), out, 3.6, 5.6, WOOD)
    B.opening(Vector((gate_face.x, gate_face.y, top + 7.0)), out, 1.0, 1.9, win_mat())

    # ---- corner towers
    for i, p in enumerate(P):
        r = c["tower_radius_m"] * rng.uniform(0.88, 1.15)
        th = top + c["tower_height_m"] * rng.uniform(0.85, 1.15)
        xs = [p[0] + r * 1.3 * math.cos(a) for a in np.linspace(0, 2 * math.pi, 12)]
        ys = [p[1] + r * 1.3 * math.sin(a) for a in np.linspace(0, 2 * math.pi, 12)]
        tz0 = ground_min(xs, ys) - 2.0
        B.frustum(p, r * 1.18, r, tz0, top + 3.0, 24)
        B.frustum(p, r, r, top + 3.0, th, 24)
        B.frustum(p, r + 0.4, r + 0.4, th, th + 0.9, 24, cap=True)  # corbelled ring
        if rng.uniform() < 0.72:
            B.frustum(p, r + 0.15, r + 0.15, th + 0.9, th + 2.6, 24)
            B.cone(p, r + 0.35, th + 2.6, (r + 0.35) * rng.uniform(2.1, 2.7), 24)
        else:
            B.merlons_ring(p, r + 0.4, th + 0.9)
        for k in range(int((th - top) // 5)):
            a = rng.uniform(0, 2 * math.pi)
            base = Vector((p[0] + r * math.cos(a), p[1] + r * math.sin(a), top + 3.5 + k * 5))
            if k >= 2 and rng.uniform() < 0.5:
                B.opening(base, (math.cos(a), math.sin(a)), 0.8, 1.6, win_mat())
            else:
                B.opening(base, (math.cos(a), math.sin(a)), 0.28, 1.4, WIN_DARK, arch=False)

    # ---- keep, toward the back of the plateau
    back = -Vector((math.cos(gate_a), math.sin(gate_a)))
    side = Vector((-back.y, back.x))
    K = Vector((cx, cy)) + back * pr * 0.32 + side * rng.uniform(-4, 4)
    ks, kh = c["keep_size_m"], top + c["keep_height_m"]
    kyaw = gate_a
    B.box(K, ks + 2.0, ks + 2.0, top - 3.0, top + 3.5, kyaw)   # plinth
    B.box(K, ks, ks, top + 3.5, kh, kyaw)
    for z in (top + 12.0, kh - 7.0):                          # string courses
        B.box(K, ks + 0.35, ks + 0.35, z, z + 0.45, kyaw)
    B.box(K, ks + 0.8, ks + 0.8, kh, kh + 0.8, kyaw)            # corbel table
    ca, sa = math.cos(kyaw), math.sin(kyaw)

    def kp(x, y):
        return Vector((K.x + x * ca - y * sa, K.y + x * sa + y * ca))

    hs = ks / 2 + 0.4
    corners = [kp(-hs, -hs), kp(hs, -hs), kp(hs, hs), kp(-hs, hs)]
    for i in range(4):
        B.merlons_line(corners[i], corners[(i + 1) % 4], kh + 0.8, 0.5, -0.25)
    B.hip_roof(K, ks - 2.2, ks - 2.2, kh + 1.2, (ks - 2.2) * 0.85, kyaw)
    for i, cp in enumerate(corners):                          # bartizans
        if i == 2:
            continue  # the spire tower takes this corner
        B.frustum(cp, 0.4, 1.7, kh - 5.0, kh - 2.5, 12)
        B.frustum(cp, 1.7, 1.7, kh - 2.5, kh + 2.0, 12)
        B.cone(cp, 1.9, kh + 2.0, 4.6, 12)
    for face in range(4):                                     # window rows
        fa = kyaw + face * math.pi / 2
        nrm = Vector((math.cos(fa - math.pi / 2), math.sin(fa - math.pi / 2)))
        for lvl, z in enumerate(np.arange(top + 7.0, kh - 4.0, 5.5)):
            for t in (-0.25, 0.25) if lvl % 2 == 0 else (0.0,):
                center = K + nrm * (ks / 2) + Vector((-nrm.y, nrm.x)) * t * ks
                big = lvl >= 2
                B.opening(Vector((center.x, center.y, z)), nrm, 1.0 if big else 0.32,
                          2.0 if big else 1.4, win_mat() if big else WIN_DARK, arch=big)

    # ---- spire tower at the keep's far corner
    sp = corners[2] + (corners[2] - K).normalized() * 1.2
    sr = 2.9
    sh = top + c["spire_height_m"] - sr * 3.4
    B.frustum(sp, sr * 1.12, sr, top - 3.0, top + 4.0, 20)
    B.frustum(sp, sr, sr, top + 4.0, sh, 20)
    B.frustum(sp, sr + 0.35, sr + 0.35, sh, sh + 0.8, 20, cap=True)
    B.frustum(sp, sr + 0.1, sr + 0.1, sh + 0.8, sh + 3.0, 20)
    for a in np.linspace(0, 2 * math.pi, 5)[:-1]:
        B.opening(Vector((sp.x + (sr + 0.1) * math.cos(a), sp.y + (sr + 0.1) * math.sin(a), sh + 1.0)),
                  (math.cos(a), math.sin(a)), 0.7, 1.5, win_mat())
    B.cone(sp, sr + 0.3, sh + 3.0, (sr + 0.3) * 3.4, 20)
    flag_z = sh + 3.0 + (sr + 0.3) * 3.4 + 1.2
    B.frustum(sp, 0.06, 0.05, flag_z - 1.0, flag_z + 2.6, 6, WOOD)
    wind = Vector((0.6, 0.8))
    fz = flag_z + 2.4
    B.face([Vector((sp.x, sp.y, fz)), Vector((sp.x, sp.y, fz - 1.3)),
            Vector((sp.x + wind.x * 3.6, sp.y + wind.y * 3.6, fz - 0.9))], CLOTH, [(0, 0), (0, 1), (1, 0.5)])

    # ---- great hall along a back-side wall, houses near the gate side
    def inner_building(i_edge, length_frac, depth, height, roof_mat, max_len=26.0):
        p0, p1 = P[i_edge], P[(i_edge + 1) % n]
        d = Vector((p1[0] - p0[0], p1[1] - p0[1]))
        mid = Vector(((p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2))
        inward = (Vector((cx, cy)) - mid).normalized()
        L = min(d.length * length_frac, max_len)
        bc = mid + inward * (thick / 2 + depth / 2 + 0.4)
        yaw = math.atan2(d.y, d.x)
        bz1 = top + height
        B.box(bc, L, depth, top - 2.0, bz1, yaw, top=False)
        rh = depth / 2 * math.tan(math.radians(55))
        B.gable_roof(bc, L, depth, bz1, rh, yaw, roof_mat)
        dn = d.normalized()
        for t in np.linspace(-0.38, 0.38, max(2, int(L / 6))):
            base = bc + dn * t * L - inward * 0.0 + inward * (depth / 2)
            B.opening(Vector((base.x, base.y, top + 2.5)), inward, 1.1, 2.8, win_mat())
        for t in (-0.3, 0.3):  # chimneys
            ch = bc + dn * t * L
            B.box(ch, 1.1, 1.1, bz1, bz1 + rh + 1.8, yaw)

    edge_dirs = [math.atan2((P[i][1] + P[(i + 1) % n][1]) / 2 - cy, (P[i][0] + P[(i + 1) % n][0]) / 2 - cx)
                 for i in range(n)]

    def edge_toward(angle):
        return min(range(n - 1), key=lambda i: abs(math.remainder(edge_dirs[i] - angle, 2 * math.pi)))

    hall_e = edge_toward(gate_a + math.radians(125))
    inner_building(hall_e, 0.6, 9.5, 10.0, ROOF_ALT)
    for off in (55, -60):
        e = edge_toward(gate_a + math.radians(off))
        if e != hall_e:
            inner_building(e, 0.45, 6.5, 6.5, ROOF, max_len=13.0)

    me = bpy.data.meshes.new("RW_Castle")
    # faces are emitted with outward winding (no recalc: it can flip isolated
    # faces such as windows, which would then be culled in Unreal)
    B.bm.to_mesh(me)
    B.bm.free()
    for key in ("stone", "roof", "wood", "window_dark", "window_lit", "cloth", "roof_alt"):
        me.materials.append(mats[key])
    ob = bpy.data.objects.new("RW_Castle", me)
    coll.objects.link(ob)
    return ob


# ---------------------------------------------------------------------------
def build_cliff(cfg, terrain, mats, coll, res_m=0.5):
    """A rock-face mesh wrapped around the crag in polar coordinates.

    A heightfield cannot hold vertical faces or overhangs, so the cliff band
    gets its own mesh: for every angle and height we find where the terrain
    reaches that height, then push the surface outward with columnar
    jointing, ledges and fine fracture noise. It sits just proud of the
    terrain, ducks under it at the foot, and stays clear of the road."""
    from .noise import Perlin2D, fbm, ridged, smoothstep

    site = terrain.castle
    if not site:
        return None
    cx, cy = site["center"]
    pr, cr = site["plateau_r"], site["crag_r"]
    top, base = site["top"], site["site"]
    ch = top - base
    circ = 2 * math.pi * pr
    n_a = int(circ / res_m)
    z_lo = base + ch * 0.22
    n_z = int((top + 0.3 - z_lo) / res_m) + 1
    angs = np.linspace(0, 2 * math.pi, n_a, endpoint=False)
    zs = np.linspace(z_lo, top + 0.3, n_z)

    # radial terrain profiles: H[a, r]
    rr = np.arange(pr - 18.0, cr, 0.25)
    A, R = np.meshgrid(angs, rr, indexing="ij")
    H = terrain.height_at(cx + R * np.cos(A), cy + R * np.sin(A))
    # running minimum outward, so a single crossing per height exists
    H = np.minimum.accumulate(H, axis=1)
    radius = np.empty((n_a, n_z))
    for j, z in enumerate(zs):
        below = H < z
        idx = np.where(below.any(1), below.argmax(1), len(rr) - 1)
        radius[:, j] = rr[idx]

    # displacement field over (arc length, height)
    n1, n2, n3 = Perlin2D(cfg["seed"] + 70), Perlin2D(cfg["seed"] + 71), Perlin2D(cfg["seed"] + 72)
    U = (angs * pr)[:, None] * np.ones((1, n_z))
    Z = np.ones((n_a, 1)) * zs[None, :]
    columns = ridged(n1, U / 4.5, Z / 16.0, 5)                 # vertical jointing
    blocks = ridged(n2, U / 9.0 + 3.0, Z / 3.2, 4)            # horizontal bedding / ledges
    fine = fbm(n3, U / 1.3, Z / 1.3, 4)
    disp = 0.9 + 2.6 * columns + 1.1 * blocks + 0.45 * fine
    t = (zs - z_lo) / (top + 0.3 - z_lo)
    disp *= smoothstep(0.0, 0.18, t)[None, :]                   # dive into the slope at the foot
    disp -= 0.8 * (1 - smoothstep(0.0, 0.1, t))[None, :]
    disp = disp * (1 - 0.75 * smoothstep(0.9, 1.0, t))[None, :] + 0.25 * smoothstep(0.9, 1.0, t)[None, :]

    X = cx + (radius + disp) * np.cos(angs)[:, None]
    Y = cy + (radius + disp) * np.sin(angs)[:, None]
    # keep the road open: pull the face back under the terrain near it
    path = terrain.sample(terrain.masks["path"], X, Y)
    pull = smoothstep(0.02, 0.3, path)
    X = cx + (radius + disp * (1 - pull) - 1.2 * pull) * np.cos(angs)[:, None]
    Y = cy + (radius + disp * (1 - pull) - 1.2 * pull) * np.sin(angs)[:, None]

    verts = np.stack([X, Y, Z], -1).reshape(-1, 3).astype(np.float32)
    idx = np.arange(n_a * n_z).reshape(n_a, n_z)
    nxt = np.roll(idx, -1, axis=0)
    # outward winding: angle increases to the right, height upward
    faces = np.stack([idx[:, :-1], nxt[:, :-1], nxt[:, 1:], idx[:, 1:]], -1).reshape(-1, 4).astype(np.int32)
    me = bpy.data.meshes.new("RW_Crag")
    me.vertices.add(len(verts))
    me.vertices.foreach_set("co", verts.ravel())
    me.loops.add(faces.size)
    me.loops.foreach_set("vertex_index", faces.ravel())
    me.polygons.add(len(faces))
    me.polygons.foreach_set("loop_start", np.arange(0, faces.size, 4, dtype=np.int32))
    me.update(calc_edges=True)
    me.shade_smooth()
    uv = me.uv_layers.new(name="UVMap")
    lv = faces.ravel()
    uvs = np.stack([U.ravel()[lv], Z.ravel()[lv]], 1).astype(np.float32)
    uv.data.foreach_set("uv", uvs.ravel())
    me.materials.append(mats["rock"])
    ob = bpy.data.objects.new("RW_Crag", me)
    coll.objects.link(ob)
    return ob
