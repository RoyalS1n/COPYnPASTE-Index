"""Procedural castle: an original, large-scale, walkable design.

Everything is assembled from an architectural kit (walls with real
openings, rooms, floor slabs with stairwells, straight and curved stairs,
round tower shells, roofs, beams, furniture) so the result is a playable
space, not just a silhouette:

  * gatehouse with a walk-through passage, raised portcullis, open gates
  * curtain walls with walkable wall-walks, reached by courtyard stairs
  * hollow round towers: ground room, wall-walk room, curved stair up to
    a roofed top room or an open battlemented platform
  * four-storey keep: storage, throne hall, chambers, armoury, stair
    flights through stairwells, a roof deck with a pavilion and turrets
  * timber-trussed great hall with tables, high table, fireplace, banners
    and chandeliers; houses; a well
  * torch / chandelier / fire light positions for Blender and Unreal

Faces are wound outward. Spaces seen from inside (under roofs) get extra
inward-facing surfaces, because Unreal culls back faces."""
import math

import bmesh
import bpy
import numpy as np
from mathutils import Vector

STONE, ROOF, WOOD, WIN_DARK, WIN_LIT, CLOTH, ROOF_ALT, IRON, FIRE, HAY = range(10)
MAT_KEYS = ("stone", "roof", "wood", "window_dark", "window_lit", "cloth", "roof_alt", "iron", "fire", "hay")
WIND = Vector((0.6, 0.8))

TORCH = (1.0, 0.55, 0.22)
CANDLE = (1.0, 0.62, 0.3)


class Frame:
    """2D placement frame: origin + yaw; z stays world z."""

    def __init__(self, ox, oy, yaw):
        self.ox, self.oy, self.yaw = ox, oy, yaw
        self.c, self.s = math.cos(yaw), math.sin(yaw)

    def P(self, x, y, z):
        return Vector((self.ox + x * self.c - y * self.s, self.oy + x * self.s + y * self.c, z))

    def sub(self, x, y, dyaw=0.0):
        p = self.P(x, y, 0)
        return Frame(p.x, p.y, self.yaw + dyaw)

    def vec(self, x, y):
        return Vector((x * self.c - y * self.s, x * self.s + y * self.c))


class Builder:
    def __init__(self, v0):
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.v0 = v0          # UV v origin (courtyard level) for grime on walls
        self.lights = []

    # ------------------------------------------------------------- core
    def face(self, pts, mat, uvs):
        vs = [self.bm.verts.new(p) for p in pts]
        f = self.bm.faces.new(vs)
        f.material_index = mat
        for loop, uv in zip(f.loops, uvs):
            loop[self.uv].uv = uv
        return f

    def _box(self, M, x0, x1, y0, y1, z0, z1, mat, skip=()):
        """Box in a local frame given by mapping M(x, y, z) -> world (a
        proper rotation + translation, so windings stay outward)."""
        if x1 - x0 < 1e-4 or y1 - y0 < 1e-4 or z1 - z0 < 1e-4:
            return
        c = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
        v0 = self.v0
        for i in range(4):
            a, b = c[i], c[(i + 1) % 4]
            ua, ub = (a[0], b[0]) if abs(a[1] - b[1]) < 1e-9 else (a[1], b[1])
            self.face([M(*a, z0), M(*b, z0), M(*b, z1), M(*a, z1)], mat,
                      [(ua, z0 - v0), (ub, z0 - v0), (ub, z1 - v0), (ua, z1 - v0)])
        # horizontal faces: v offset keeps the wall-foot grime off floors
        if "t" not in skip:
            self.face([M(x, y, z1) for x, y in c], mat, [(x, y + 1000.0) for x, y in c])
        if "b" not in skip:
            self.face([M(x, y, z0) for x, y in reversed(c)], mat, [(x, y + 1000.0) for x, y in reversed(c)])

    def box(self, F, x0, x1, y0, y1, z0, z1, mat=STONE, skip=()):
        self._box(F.P, min(x0, x1), max(x0, x1), min(y0, y1), max(y0, y1), z0, z1, mat, skip)

    def beam(self, p0, p1, w, h, mat=WOOD):
        """Square-section beam between two world points."""
        p0, p1 = Vector(p0), Vector(p1)
        d = p1 - p0
        L = d.length
        d.normalize()
        y = Vector((0, 0, 1)).cross(d)
        if y.length < 1e-6:
            y = Vector((1, 0, 0)).cross(d)
        y.normalize()
        z = d.cross(y)
        self._box(lambda a, b, c: p0 + d * a + y * b + z * c, 0, L, -w / 2, w / 2, -h / 2, h / 2, mat)

    # ------------------------------------------------------------- walls
    def wall_x(self, F, x0, x1, y0, y1, z0, z1, openings=(), mat=STONE):
        """Wall slab along local x with real openings (xc, w, zb, zt).
        Openings sharing a column are stacked."""
        cols = {}
        for xc, w, zb, zt in openings:
            a, b = max(x0, xc - w / 2), min(x1, xc + w / 2)
            if b > a:
                cols.setdefault((round(a, 3), round(b, 3)), []).append((zb, zt))
        xs = x0
        for (a, b), spans in sorted(cols.items()):
            if a > xs:
                self.box(F, xs, a, y0, y1, z0, z1, mat)
            zc = z0
            for zb, zt in sorted(spans):
                if zb > zc:
                    self.box(F, a, b, y0, y1, zc, zb, mat)
                zc = max(zc, zt)
            if z1 > zc:
                self.box(F, a, b, y0, y1, zc, z1, mat)
            xs = max(xs, b)
        if x1 > xs:
            self.box(F, xs, x1, y0, y1, z0, z1, mat)

    def room(self, F, w, d, z0, z1, t, openings=None, mat=STONE):
        """Four walls of a w x d room (outer size). openings: side -> list of
        (u, width, zb, zt); S/N sides use u = local x, E/W use u = local y."""
        o = openings or {}
        hw, hd = w / 2, d / 2
        self.wall_x(F.sub(0, -hd + t / 2), -hw, hw, -t / 2, t / 2, z0, z1, o.get("S", ()), mat)
        self.wall_x(F.sub(0, hd - t / 2), -hw, hw, -t / 2, t / 2, z0, z1, o.get("N", ()), mat)
        self.wall_x(F.sub(hw - t / 2, 0, math.pi / 2), -hd + t, hd - t, -t / 2, t / 2, z0, z1, o.get("E", ()), mat)
        self.wall_x(F.sub(-hw + t / 2, 0, math.pi / 2), -hd + t, hd - t, -t / 2, t / 2, z0, z1, o.get("W", ()), mat)

    def slab(self, F, x0, x1, y0, y1, z, thick, hole=None, mat=WOOD):
        if not hole:
            self.box(F, x0, x1, y0, y1, z - thick, z, mat)
            return
        hx0, hx1, hy0, hy1 = max(hole[0], x0), min(hole[1], x1), max(hole[2], y0), min(hole[3], y1)
        self.box(F, x0, x1, y0, hy0, z - thick, z, mat)
        self.box(F, x0, x1, hy1, y1, z - thick, z, mat)
        self.box(F, x0, hx0, hy0, hy1, z - thick, z, mat)
        self.box(F, hx1, x1, hy0, hy1, z - thick, z, mat)

    def stair(self, F, axis, start, direction, lat0, lat1, z0, z1, tread=0.32, rise=0.25, mat=STONE):
        """Solid straight flight. axis 'x' or 'y' is the run direction."""
        n = max(1, math.ceil((z1 - z0) / rise - 1e-6))
        r = (z1 - z0) / n
        for i in range(n):
            a = start + direction * i * tread
            b = a + direction * tread
            zt = z0 + (i + 1) * r
            if axis == "x":
                self.box(F, a, b, lat0, lat1, z0, zt, mat)
            else:
                self.box(F, lat0, lat1, a, b, z0, zt, mat)
        return start + direction * n * tread

    def merlon_run(self, F, x0, x1, y0, y1, z, mw=1.0, gap=0.8, mh=0.9, ph=1.0):
        """Parapet (local x run) with merlons on top."""
        self.box(F, x0, x1, y0, y1, z, z + ph)
        L = x1 - x0
        n = int(L // (mw + gap))
        start = x0 + (L - n * (mw + gap) + gap) / 2
        for i in range(n):
            a = start + i * (mw + gap)
            self.box(F, a, a + mw, y0, y1, z + ph, z + ph + mh)

    # ------------------------------------------------------------- round
    def wedge(self, c, r0, r1, a0, a1, z0, z1, mat=STONE, seg=None):
        """Annular-sector block (r0 < r1, a0 < a1)."""
        n = seg or max(1, int(math.ceil((a1 - a0) * r1 / 1.2)))
        angs = np.linspace(a0, a1, n + 1)
        v0 = self.v0

        def p(r, a, z):
            return Vector((c[0] + r * math.cos(a), c[1] + r * math.sin(a), z))

        for i in range(n):
            a, b = angs[i], angs[i + 1]
            self.face([p(r1, a, z0), p(r1, b, z0), p(r1, b, z1), p(r1, a, z1)], mat,
                      [(a * r1, z0 - v0), (b * r1, z0 - v0), (b * r1, z1 - v0), (a * r1, z1 - v0)])
            if r0 > 0.02:
                self.face([p(r0, b, z0), p(r0, a, z0), p(r0, a, z1), p(r0, b, z1)], mat,
                          [(b * r0, z0 - v0), (a * r0, z0 - v0), (a * r0, z1 - v0), (b * r0, z1 - v0)])
            top = [p(r0, a, z1), p(r1, a, z1), p(r1, b, z1), p(r0, b, z1)]
            self.face(top, mat, [(q.x, q.y + 1000.0) for q in top])
            bot = [p(r0, b, z0), p(r1, b, z0), p(r1, a, z0), p(r0, a, z0)]
            self.face(bot, mat, [(q.x, q.y + 1000.0) for q in bot])
        self.face([p(r0, a0, z0), p(r1, a0, z0), p(r1, a0, z1), p(r0, a0, z1)], mat,
                  [(r0, z0 - v0), (r1, z0 - v0), (r1, z1 - v0), (r0, z1 - v0)])
        self.face([p(r1, a1, z0), p(r0, a1, z0), p(r0, a1, z1), p(r1, a1, z1)], mat,
                  [(r1, z0 - v0), (r0, z0 - v0), (r0, z1 - v0), (r1, z1 - v0)])

    def frustum(self, c, r0, r1, z0, z1, seg=24, mat=STONE, cap=False):
        u_scale = (r0 + r1) / 2
        v0 = self.v0
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            pts = [Vector((c[0] + r0 * math.cos(a0), c[1] + r0 * math.sin(a0), z0)),
                   Vector((c[0] + r0 * math.cos(a1), c[1] + r0 * math.sin(a1), z0)),
                   Vector((c[0] + r1 * math.cos(a1), c[1] + r1 * math.sin(a1), z1)),
                   Vector((c[0] + r1 * math.cos(a0), c[1] + r1 * math.sin(a0), z1))]
            self.face(pts, mat, [(a0 * u_scale, z0 - v0), (a1 * u_scale, z0 - v0),
                                 (a1 * u_scale, z1 - v0), (a0 * u_scale, z1 - v0)])
        if cap:
            pts = [Vector((c[0] + r1 * math.cos(2 * math.pi * i / seg), c[1] + r1 * math.sin(2 * math.pi * i / seg), z1))
                   for i in range(seg)]
            self.face(pts, mat, [(q.x, q.y + 1000.0) for q in pts])

    def round_shell(self, c, r_in, r_out, z0, z1, openings=(), seg=32, mat=STONE):
        """Hollow round wall; openings: (angle, width_m, zb, zt)."""
        da = 2 * math.pi / seg
        for i in range(seg):
            a, b = i * da, (i + 1) * da
            mid = (a + b) / 2
            spans = []
            for ang, w, zb, zt in openings:
                half = max(w / r_out / 2, da * 0.5)
                if abs(math.remainder(mid - ang, 2 * math.pi)) < half:
                    spans.append((zb, zt))
            zc = z0
            for zb, zt in sorted(spans):
                if zb > zc:
                    self.wedge(c, r_in, r_out, a, b, zc, zb, mat, seg=1)
                zc = max(zc, zt)
            if z1 > zc:
                self.wedge(c, r_in, r_out, a, b, zc, z1, mat, seg=1)

    def disc_slab(self, c, r, z, thick, hole=None, mat=WOOD, seg=28):
        """Round floor; hole = (a0, a1, r0, r1) annular sector left open."""
        da = 2 * math.pi / seg
        for i in range(seg):
            a, b = i * da, (i + 1) * da
            mid = (a + b) / 2
            if hole and abs(math.remainder(mid - (hole[0] + hole[1]) / 2, 2 * math.pi)) < (hole[1] - hole[0]) / 2:
                if hole[2] > 0.1:
                    self.wedge(c, 0.0, hole[2], a, b, z - thick, z, mat, seg=1)
                if hole[3] < r - 0.05:
                    self.wedge(c, hole[3], r, a, b, z - thick, z, mat, seg=1)
            else:
                self.wedge(c, 0.0, r, a, b, z - thick, z, mat, seg=1)

    def curved_stair(self, c, r0, r1, a_start, direction, z0, z1, rise=0.25, tread=0.45, thick=0.32, mat=STONE):
        n = max(1, math.ceil((z1 - z0) / rise - 1e-6))
        r = (z1 - z0) / n
        da = tread / ((r0 + r1) / 2)
        for i in range(n):
            a = a_start + direction * i * da
            lo, hi = sorted((a, a + direction * da))
            zt = z0 + (i + 1) * r
            self.wedge(c, r0, r1, lo, hi, zt - thick, zt, mat, seg=1)
        return a_start + direction * n * da, n * da

    def cone(self, c, r, z0, h, seg=24, mat=ROOF, eave=0.5):
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
        self.frustum(c, 0.14, 0.02, z0 + h - 0.3, z0 + h + 1.8, 6, IRON)

    def hip_roof(self, F, sx, sy, z0, h, mat=ROOF, eave=0.6):
        hx, hy = sx / 2 + eave, sy / 2 + eave
        ridge = max(0.0, hx - hy)
        r0, r1 = F.P(-ridge, 0, z0 + h), F.P(ridge, 0, z0 + h)
        ze = z0 - eave * 0.5
        b = [F.P(-hx, -hy, ze), F.P(hx, -hy, ze), F.P(hx, hy, ze), F.P(-hx, hy, ze)]
        sl = math.hypot(hy, h)
        self.face([b[0], b[1], r1, r0], mat, [(0, 0), (2 * hx, 0), (hx + ridge, sl), (hx - ridge, sl)])
        self.face([b[2], b[3], r0, r1], mat, [(0, 0), (2 * hx, 0), (hx + ridge, sl), (hx - ridge, sl)])
        sl2 = math.hypot(hx - ridge, h)
        self.face([b[1], b[2], r1], mat, [(0, 0), (2 * hy, 0), (hy, sl2)])
        self.face([b[3], b[0], r0], mat, [(0, 0), (2 * hy, 0), (hy, sl2)])

    def gable_roof(self, F, length, depth, z0, h, mat=ROOF, eave=0.6, inner=True):
        """Gable roof on a room in frame F (ridge along local x). With inner,
        adds inward-facing plank undersides and gable faces for interiors."""
        hl, hd = length / 2 + eave, depth / 2 + eave
        ze = z0 - eave * h / (depth / 2)
        sl = math.hypot(hd, h + (z0 - ze))
        for s in (-1, 1):
            pts = [F.P(-hl, s * hd, ze), F.P(hl, s * hd, ze), F.P(hl, 0, z0 + h), F.P(-hl, 0, z0 + h)]
            if s > 0:
                pts = pts[::-1]
            self.face(pts, mat, [(0, 0), (2 * hl, 0), (2 * hl, sl), (0, sl)])
            if inner:  # underside, a little lower, facing down/in
                q = [F.P(-length / 2, s * depth / 2, z0 - 0.02), F.P(length / 2, s * depth / 2, z0 - 0.02),
                     F.P(length / 2, 0, z0 + h - 0.25), F.P(-length / 2, 0, z0 + h - 0.25)]
                if s < 0:
                    q = q[::-1]
                self.face(q, WOOD, [(0, 0), (length, 0), (length, sl), (0, sl)])
        for s in (-1, 1):  # stone gable ends (outward + inward faces)
            x = s * length / 2
            pts = [F.P(x, -depth / 2, z0), F.P(x, depth / 2, z0), F.P(x, 0, z0 + h)]
            uvs = [(0, z0 - self.v0), (depth, z0 - self.v0), (depth / 2, z0 + h - self.v0)]
            if s < 0:
                pts, uvs = [pts[1], pts[0], pts[2]], [uvs[1], uvs[0], uvs[2]]
            self.face(pts, STONE, uvs)
            if inner:
                xi = x - s * 0.05
                pi = [F.P(xi, -depth / 2, z0), F.P(xi, depth / 2, z0), F.P(xi, 0, z0 + h)]
                if s > 0:
                    pi = [pi[1], pi[0], pi[2]]
                self.face(pi, STONE, uvs)

    # ------------------------------------------------------------- dressing
    def decal(self, base, normal, w, h, mat, arch=True, depth=0.05):
        """Arched dark panel just off a wall (arrow slits, blind windows)."""
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

    def light(self, pos, color, power, radius=0.08, kind="torch"):
        self.lights.append({"pos": tuple(pos), "color": tuple(color), "power": power, "radius": radius, "kind": kind})

    def torch(self, F, x, y, z, facing_y=1):
        """Wall torch on a wall at local (x, y), sticking out along +/-y."""
        s = facing_y
        self.box(F, x - 0.06, x + 0.06, y, y + s * 0.35, z - 0.05, z + 0.05, IRON)
        self.box(F, x - 0.05, x + 0.05, y + s * 0.3, y + s * 0.4, z, z + 0.35, WOOD)
        tip = F.P(x, y + s * 0.35, z + 0.45)
        self.frustum((tip.x, tip.y), 0.07, 0.0, z + 0.35, z + 0.65, 6, FIRE)
        self.light(F.P(x, y + s * 0.5, z + 0.55), TORCH, 70.0, 0.06, "torch")

    def table(self, F, x0, x1, y0, y1, z, h=0.8):
        self.box(F, x0, x1, y0, y1, z + h - 0.08, z + h, WOOD)
        for lx in (x0 + 0.15, x1 - 0.25):
            for ly in (y0 + 0.1, y1 - 0.2):
                self.box(F, lx, lx + 0.1, ly, ly + 0.1, z, z + h - 0.08, WOOD)

    def bench(self, F, x0, x1, y0, y1, z, h=0.45):
        self.box(F, x0, x1, y0, y1, z + h - 0.06, z + h, WOOD)
        self.box(F, x0 + 0.1, x0 + 0.2, y0, y1, z, z + h - 0.06, WOOD)
        self.box(F, x1 - 0.2, x1 - 0.1, y0, y1, z, z + h - 0.06, WOOD)

    def barrel(self, c, z, r=0.38, h=0.95):
        self.frustum(c, r * 0.85, r, z, z + h * 0.5, 12, WOOD)
        self.frustum(c, r, r * 0.85, z + h * 0.5, z + h, 12, WOOD, cap=True)
        for zz in (z + 0.15, z + h - 0.2):
            self.frustum(c, r * 0.93, r * 0.98, zz, zz + 0.05, 12, IRON)

    def crate(self, F, x, y, z, s=0.9):
        self.box(F, x, x + s, y, y + s, z, z + s, WOOD)

    def banner(self, F, x, y, z_top, w, h, facing_y=1):
        """Cloth banner hanging on a wall, with a swallow-tail hem."""
        s = facing_y
        yy = y + s * 0.06
        pts = [F.P(x - w / 2, yy, z_top - h), F.P(x, yy, z_top - h + 0.5), F.P(x + w / 2, yy, z_top - h),
               F.P(x + w / 2, yy, z_top), F.P(x - w / 2, yy, z_top)]
        if s > 0:
            pts = pts[::-1]
        self.face(pts, CLOTH, [(0, 0)] * 5)
        self.beam(F.P(x - w / 2 - 0.2, yy, z_top + 0.05), F.P(x + w / 2 + 0.2, yy, z_top + 0.05), 0.07, 0.07, WOOD)

    def chandelier(self, c, z, r=1.1, hang_to=None):
        self.frustum(c, r, r, z, z + 0.08, 16, IRON)
        self.frustum(c, r - 0.08, r - 0.08, z, z + 0.08, 16, IRON)
        for i in range(8):
            a = 2 * math.pi * i / 8
            p = (c[0] + r * math.cos(a), c[1] + r * math.sin(a))
            self.frustum(p, 0.04, 0.04, z + 0.08, z + 0.3, 5, WOOD, cap=True)
            self.frustum(p, 0.03, 0.0, z + 0.3, z + 0.42, 5, FIRE)
        if hang_to:
            self.beam((c[0], c[1], z + 0.08), (c[0], c[1], hang_to), 0.04, 0.04, IRON)
        self.light((c[0], c[1], z + 0.45), CANDLE, 260.0, 0.6, "chandelier")


    # ------------------------------------------------------------- props
    def door_leaf(self, hinge, direction, width, height, z0, thick=0.09):
        """Open door leaf standing at a jamb (hinge) and swung along a 2D
        direction, with iron straps."""
        d = Vector((direction[0], direction[1])).normalized()
        zc = z0 + height / 2
        p0 = Vector((hinge[0], hinge[1], zc))
        p1 = p0 + Vector((d.x, d.y, 0)) * width
        self.beam(p0, p1, thick, height, WOOD)
        for zz in (z0 + 0.4, z0 + height - 0.5):
            self.beam(Vector((p0.x, p0.y, zz)), Vector((p1.x, p1.y, zz)), thick + 0.03, 0.08, IRON)

    def pennant(self, top, length=3.2, height=1.2):
        x, y, z = top
        self.face([Vector((x, y, z)), Vector((x, y, z - height)),
                   Vector((x + WIND.x * length, y + WIND.y * length, z - height * 0.6))], CLOTH,
                  [(0, 0), (0, 1), (1, 0.5)])
        self.face([Vector((x, y, z - height)), Vector((x, y, z)),
                   Vector((x + WIND.x * length, y + WIND.y * length, z - height * 0.6))], CLOTH,
                  [(0, 1), (0, 0), (1, 0.5)])

    def wheel(self, c, axis, r=0.55, spokes=8):
        """Cart wheel in the vertical plane perpendicular to the 2D axle."""
        ax = Vector((axis[0], axis[1], 0)).normalized()
        u = Vector((-ax.y, ax.x, 0))
        w = Vector((0, 0, 1))
        cc = Vector(c)
        rim = [cc + (u * math.cos(a) + w * math.sin(a)) * r for a in np.linspace(0, 2 * math.pi, 13)]
        for a_, b_ in zip(rim[:-1], rim[1:]):
            self.beam(a_, b_, 0.08, 0.07, WOOD)
        for k in range(spokes):
            a = 2 * math.pi * k / spokes
            self.beam(cc, cc + (u * math.cos(a) + w * math.sin(a)) * (r - 0.03), 0.05, 0.05, WOOD)
        self.beam(cc - ax * 0.12, cc + ax * 0.12, 0.16, 0.16, IRON)

    def cart(self, F, z, hay=True):
        self.box(F, -1.5, 1.5, -0.8, 0.8, z + 0.55, z + 0.7, WOOD)                     # bed
        for s in (-1, 1):
            self.box(F, -1.5, 1.5, s * 0.8 - 0.05, s * 0.8 + 0.05, z + 0.7, z + 1.1, WOOD)  # sides
            self.wheel(F.P(0.2, s * 0.92, z + 0.55), F.vec(0, 1), 0.55)
        self.beam(F.P(1.5, -0.3, z + 0.6), F.P(3.4, -0.1, z + 0.15), 0.08, 0.08)          # shafts
        self.beam(F.P(1.5, 0.3, z + 0.6), F.P(3.4, 0.1, z + 0.15), 0.08, 0.08)
        if hay:
            self.box(F, -1.4, 1.3, -0.72, 0.72, z + 0.7, z + 1.5, HAY)

    def hay_bale(self, F, x, y, z):
        self.box(F, x, x + 1.1, y, y + 0.55, z, z + 0.45, HAY)

    def stall(self, F, z, canopy=CLOTH):
        """Market stall: posts, counter with goods, sloped cloth canopy."""
        for x in (-1.6, 1.6):
            for y in (-1.0, 1.0):
                h = 2.6 if y > 0 else 2.2
                self.box(F, x - 0.06, x + 0.06, y - 0.06, y + 0.06, z, z + h, WOOD)
        self.box(F, -1.6, 1.6, -1.0, -0.4, z + 0.85, z + 0.95, WOOD)                     # counter
        self.box(F, -1.6, 1.6, -0.95, -0.45, z, z + 0.85, WOOD)
        pts = [F.P(-1.8, -1.3, z + 2.15), F.P(1.8, -1.3, z + 2.15), F.P(1.8, 1.15, z + 2.65), F.P(-1.8, 1.15, z + 2.65)]
        self.face(pts, canopy, [(0, 0)] * 4)
        self.face(pts[::-1], canopy, [(0, 0)] * 4)
        for i, x in enumerate(np.linspace(-1.2, 1.2, 4)):                                # baskets of goods
            c = F.P(x, -0.7, 0)
            self.frustum((c.x, c.y), 0.16, 0.22, z + 0.95, z + 1.15, 8, WOOD, cap=False)
            self.frustum((c.x, c.y), 0.2, 0.12, z + 1.1, z + 1.25, 8, (HAY, ROOF_ALT, CLOTH, HAY)[i], cap=True)
        self.crate(F, 0.6, 0.2, z, 0.7)
        self.barrel(F.P(-1.0, 0.4, 0), z, 0.3, 0.8)

    def dummy(self, F, x, y, z):
        self.box(F, x - 0.07, x + 0.07, y - 0.07, y + 0.07, z, z + 1.9, WOOD)
        self.box(F, x - 0.6, x + 0.6, y - 0.05, y + 0.05, z + 1.4, z + 1.5, WOOD)
        c = F.P(x, y, 0)
        self.frustum((c.x, c.y), 0.22, 0.28, z + 0.9, z + 1.6, 10, HAY, cap=True)
        self.frustum((c.x, c.y), 0.15, 0.12, z + 1.6, z + 1.95, 8, CLOTH, cap=True)

    def logs(self, F, x, y, z, length=1.8, rows=3):
        for r in range(rows):
            for k in range(rows - r + 2):
                yy = y + (k + r * 0.5) * 0.24
                zz = z + 0.12 + r * 0.21
                self.beam(F.P(x, yy, zz), F.P(x + length, yy, zz), 0.22, 0.22, WOOD)

    def smithy(self, F, z):
        """Open-sided forge shed with a lit hearth, anvil and trough."""
        for x in (-2.5, 2.5):
            for y in (-1.8, 1.8):
                self.box(F, x - 0.1, x + 0.1, y - 0.1, y + 0.1, z, z + (3.4 if y > 0 else 2.6), WOOD)
        pts = [F.P(-2.9, -2.2, z + 2.5), F.P(2.9, -2.2, z + 2.5), F.P(2.9, 2.2, z + 3.5), F.P(-2.9, 2.2, z + 3.5)]
        self.face(pts, ROOF, [(p.x, p.y) for p in pts])
        self.face(pts[::-1], WOOD, [(p.x, p.y) for p in pts[::-1]])
        self.box(F, 0.6, 2.3, 0.6, 1.8, z, z + 0.9)                                       # forge
        self.box(F, 1.0, 2.0, 1.2, 1.8, z + 0.9, z + 3.6)                                 # flue
        for k in range(4):
            c = F.P(1.1 + 0.3 * k, 0.9, 0)
            self.frustum((c.x, c.y), 0.12, 0.0, z + 0.9, z + 1.25, 6, FIRE)
        self.light(F.P(1.45, 0.6, z + 1.4), (1.0, 0.45, 0.12), 260.0, 0.3, "forge")
        self.box(F, -0.6, -0.2, -0.3, 0.3, z, z + 0.6, WOOD)                               # anvil stump
        self.box(F, -0.75, -0.05, -0.15, 0.15, z + 0.6, z + 0.85, IRON)                    # anvil
        self.box(F, -2.3, -1.0, 0.8, 1.5, z, z + 0.6, WOOD)                                # trough
        self.box(F, -2.2, -1.1, 0.9, 1.4, z + 0.45, z + 0.58, WIN_DARK)                    # water
        for k in range(5):                                                                 # tool rack
            self.box(F, -2.4 + k * 0.3, -2.35 + k * 0.3, 1.75, 1.8, z + 1.0, z + 2.1, IRON)


# ---------------------------------------------------------------------------
def build(cfg, terrain, mats, coll):
    """Returns (object, info). info carries light positions, the player
    start and interior camera placements."""
    c = cfg["castle"]
    site = terrain.castle
    if not site:
        return None, {}
    rng = np.random.default_rng(c["seed"])
    cx, cy = site["center"]
    top = site["top"]
    floor0 = top + 0.3                      # building floors sit just above the plateau
    pr = site["plateau_r"]
    gate_a = site["gate_angle"]
    ug = Vector((math.cos(gate_a), math.sin(gate_a)))
    B = Builder(top)
    wall_h, wt = c["wall_height_m"], c["wall_thickness_m"]
    T_ww = top + wall_h                      # wall-walk level
    lit = c["lit_windows"]
    info = {"lights": B.lights}

    def ground_min(xs, ys):
        return float(np.min(terrain.height_at(np.asarray(xs, float), np.asarray(ys, float))))

    def found(F, x0, x1, y0, y1):
        xs, ys = [], []
        for x in np.linspace(x0, x1, 6):
            for y in np.linspace(y0, y1, 6):
                p = F.P(x, y, 0)
                xs.append(p.x)
                ys.append(p.y)
        return ground_min(xs, ys) - 2.0

    # ---------------------------------------------------------- layout
    n = int(c["tower_count"])
    step = 2 * math.pi / n
    angs = [gate_a + (i + 0.5) * step + rng.uniform(-0.08, 0.08) * step for i in range(n)]
    rads = [(pr - 9.0) * rng.uniform(0.9, 1.0) for _ in range(n)]
    P = [Vector((cx + r * math.cos(a), cy + r * math.sin(a))) for a, r in zip(angs, rads)]
    TR = [c["tower_radius_m"] * rng.uniform(0.92, 1.1) for _ in range(n)]
    center = Vector((cx, cy))

    def edge_frame(p0, p1):
        """Frame on a wall line with local +y pointing outward."""
        d = p1 - p0
        mid = (p0 + p1) / 2
        left = Vector((-d.y, d.x))
        if left.dot(mid - center) < 0:
            p0, p1 = p1, p0
            d = -d
        return Frame(p0.x, p0.y, math.atan2(d.y, d.x)), d.length, p0, p1

    # ---------------------------------------------------------- curtain walls
    GW, GD = 16.0, 14.0
    gate_mid = (P[-1] + P[0]) / 2
    gdir = (P[0] - P[-1]).normalized()
    runs = []
    for i in range(n):
        a, b = P[i], P[(i + 1) % n]
        da = (b - a).normalized()
        a2 = a + da * (TR[i] - 0.7)
        b2 = b - da * (TR[(i + 1) % n] - 0.7)
        if i == n - 1:  # gate edge: split around the gatehouse
            runs.append((a2, gate_mid - gdir * GW / 2, i))
            runs.append((gate_mid + gdir * GW / 2, b2, i))
        else:
            runs.append((a2, b2, i))

    stair_edges = {0, n - 2}   # courtyard stairs on the walls either side of the gate
    for a, b, ei in runs:
        F, L, _, _ = edge_frame(a, b)
        z0 = found(F, 0, L, -wt / 2, wt / 2 + 2)
        B.box(F, 0, L, -wt / 2, wt / 2, z0, T_ww)
        B.box(F, 0, L, wt / 2 - 0.3, wt / 2 + 1.3, z0, min(top + 1.5, T_ww - 5))      # batter
        B.merlon_run(F, 0, L, wt / 2 - 0.6, wt / 2, T_ww)                             # parapet
        for x in np.arange(9.0, L - 4.0, 18.0):                                        # wall-walk torches
            B.torch(F, x, wt / 2 - 0.6, T_ww + 0.75, facing_y=-1)
        for x in np.arange(5.0, L - 3.0, 7.0):                                         # arrow slits
            B.decal(F.P(x, wt / 2, T_ww - 4.5), F.vec(0, 1), 0.3, 1.6, WIN_DARK, arch=False)
        if ei in stair_edges and L * 0.75 > 21.5:
            # flight along the inner face, landing on the wall-walk
            end = B.stair(F, "x", L * 0.25, 1, -wt / 2 - 2.2, -wt / 2, floor0, T_ww, tread=0.36)
            B.torch(F, L * 0.25 - 1.0, -wt / 2, floor0 + 2.6, facing_y=-1)

    # ---------------------------------------------------------- towers
    tower_tops = []
    for i, p in enumerate(P):
        r_out = TR[i]
        r_in = r_out - 1.3
        T_top = top + c["tower_height_m"] * rng.uniform(0.92, 1.12)
        tower_tops.append(T_top)
        xs = [p.x + r_out * 1.3 * math.cos(a) for a in np.linspace(0, 2 * math.pi, 12)]
        ys = [p.y + r_out * 1.3 * math.sin(a) for a in np.linspace(0, 2 * math.pi, 12)]
        tz0 = ground_min(xs, ys) - 2.0
        B.frustum(p, r_out * 1.16, r_out, tz0, top, 28, cap=True)                    # solid footing
        a_in = math.atan2(cy - p.y, cx - p.x)                                          # courtyard door
        a_prev = math.atan2(P[i - 1].y - p.y, P[i - 1].x - p.x)
        a_next = math.atan2(P[(i + 1) % n].y - p.y, P[(i + 1) % n].x - p.x)
        roofed = rng.uniform() < 0.7
        z_room_top = T_top + (3.4 if roofed else 0.0)
        outs = [a_in + math.pi + k for k in (-0.9, 0.0, 0.9)]
        openings = [(a_in, 1.8, floor0, floor0 + 3.0),
                    (a_prev, 1.8, T_ww, T_ww + 2.7), (a_next, 1.8, T_ww, T_ww + 2.7)]
        openings += [(a, 0.9, T_ww + 3.6, T_ww + 5.6) for a in outs[::2]]
        if roofed:
            openings += [(a, 1.0, T_top + 0.9, T_top + 2.5) for a in outs]
        B.round_shell(p, r_in, r_out, top, z_room_top, openings, seg=36)
        B.disc_slab(p, r_in + 0.05, floor0, 0.32, mat=STONE)
        B.disc_slab(p, r_in + 0.05, T_ww, 0.5, mat=WOOD)
        # curved stair from the wall-walk room to the top level; start next to
        # one wall-walk door, run the way that passes the other door high up
        best = None
        for direction in (1, -1):
            start = a_prev + direction * 0.42
            dist = (direction * (a_next - start)) % (2 * math.pi)
            if best is None or dist > best[0]:
                best = (dist, direction, start)
        _, direction, start = best
        a_end, span = B.curved_stair(p, 2.2, r_in, start, direction, T_ww, T_top)
        frac = min(1.0, 2.4 / (T_top - T_ww))
        h0, h1 = sorted((a_end - direction * span * frac - direction * 0.1, a_end + direction * 0.05))
        B.disc_slab(p, r_in + 0.05, T_top, 0.5, hole=(h0, h1, 2.0, r_in + 0.1), mat=WOOD)
        if roofed:
            B.frustum(p, r_out + 0.45, r_out + 0.45, z_room_top, z_room_top + 0.9, 28, cap=True)  # corbel ring
            B.disc_slab(p, r_out + 0.4, z_room_top + 0.01, 0.4, mat=WOOD)                       # ceiling
            B.frustum(p, r_out + 0.2, r_out + 0.2, z_room_top + 0.9, z_room_top + 2.4, 28)
            cone_h = (r_out + 0.4) * rng.uniform(2.0, 2.5)
            B.cone(p, r_out + 0.4, z_room_top + 2.4, cone_h, 28)
            B.pennant((p.x, p.y, z_room_top + 2.4 + cone_h + 1.7), 2.6, 1.0)
            B.light((p.x, p.y, T_top + 2.6), CANDLE, 120.0, 0.3, "tower")
        else:
            seg = max(10, int(2 * math.pi * r_out / 1.8))
            for k in range(seg):
                a = 2 * math.pi * k / seg
                B.wedge(p, r_out - 0.6, r_out, a, a + 2 * math.pi / seg, T_top, T_top + 1.0, seg=1)
                if k % 2 == 0:
                    B.wedge(p, r_out - 0.6, r_out, a, a + 2 * math.pi / seg, T_top + 1.0, T_top + 1.9, seg=1)
        for k, a in enumerate(outs):                                                   # slits
            B.decal((p.x + r_out * math.cos(a), p.y + r_out * math.sin(a), floor0 + 2.0),
                    (math.cos(a), math.sin(a)), 0.3, 1.6, WIN_DARK, arch=False)
        jamb = a_in + 0.9 / r_out
        B.door_leaf((p.x + r_in * math.cos(jamb), p.y + r_in * math.sin(jamb)),
                    (-math.cos(jamb), -math.sin(jamb)), 1.5, 2.9, floor0)
        Ft = Frame(p.x, p.y, a_in + math.pi / 2)  # local +y toward courtyard
        B.barrel((p.x + 1.6 * math.cos(a_in + 2.2), p.y + 1.6 * math.sin(a_in + 2.2)), floor0)
        B.barrel((p.x + 2.4 * math.cos(a_in + 2.6), p.y + 2.4 * math.sin(a_in + 2.6)), floor0)
        B.crate(Ft, -2.8, -1.8, floor0, 0.9)
        B.light((p.x, p.y, floor0 + 2.5), TORCH, 60.0, 0.1, "tower")
        # brazier in the middle of the wall-walk room (clear of the stair band)
        B.frustum(p, 0.12, 0.12, T_ww, T_ww + 0.7, 8, IRON)
        B.frustum(p, 0.25, 0.5, T_ww + 0.7, T_ww + 1.0, 12, IRON, cap=True)
        for k in range(3):
            a = 2 * math.pi * k / 3
            B.frustum((p.x + 0.15 * math.cos(a), p.y + 0.15 * math.sin(a)), 0.14, 0.0, T_ww + 1.0, T_ww + 1.45, 6, FIRE)
        B.light((p.x, p.y, T_ww + 1.6), TORCH, 110.0, 0.3, "brazier")

    # ---------------------------------------------------------- gatehouse
    G = Frame(gate_mid.x, gate_mid.y, math.atan2(gdir.y, gdir.x))
    if G.vec(0, 1).dot(gate_mid - center) < 0:
        G = Frame(gate_mid.x, gate_mid.y, math.atan2(gdir.y, gdir.x) + math.pi)
    pw, ph = 5.4, 7.0
    gz0 = found(G, -GW / 2, GW / 2, -GD / 2, GD / 2)
    B.box(G, -GW / 2, -pw / 2, -GD / 2, GD / 2, gz0, T_ww)
    B.box(G, pw / 2, GW / 2, -GD / 2, GD / 2, gz0, T_ww)
    B.box(G, -pw / 2, pw / 2, -GD / 2, GD / 2, floor0 + ph, T_ww)
    B.box(G, -pw / 2, pw / 2, -GD / 2, GD / 2 + 3.0, gz0, floor0)                        # passage floor
    B.merlon_run(G, -GW / 2, GW / 2, GD / 2 - 0.6, GD / 2, T_ww)
    B.merlon_run(G, -GW / 2, GW / 2, -GD / 2, -GD / 2 + 0.6, T_ww, mh=0.0, ph=1.1)
    for s in (-1, 1):                                                                 # flanking towers
        tc = G.P(s * (GW / 2 + 0.8), GD / 2 - 1.0, 0)
        tz0 = ground_min([tc.x], [tc.y]) - 2.0
        B.frustum(tc, 5.3, 4.6, tz0, top + 2.0, 24)
        B.frustum(tc, 4.6, 4.6, top + 2.0, T_ww + 6.0, 24)
        B.frustum(tc, 5.0, 5.0, T_ww + 6.0, T_ww + 6.9, 24, cap=True)
        seg = 16
        for k in range(seg):
            a = 2 * math.pi * k / seg
            B.wedge(tc, 4.4, 5.0, a, a + 2 * math.pi / seg, T_ww + 6.9, T_ww + 7.9, seg=1)
            if k % 2 == 0:
                B.wedge(tc, 4.4, 5.0, a, a + 2 * math.pi / seg, T_ww + 7.9, T_ww + 8.8, seg=1)
        for k in range(3):
            a = math.atan2(G.vec(0, 1).y, G.vec(0, 1).x) + s * (0.4 + 0.5 * k)
            B.decal((tc.x + 4.6 * math.cos(a), tc.y + 4.6 * math.sin(a), top + 5 + 4 * k),
                    (math.cos(a), math.sin(a)), 0.3, 1.6, WIN_DARK, arch=False)
    for x in (-3.5, 3.5):                                                              # banners on the outer face
        B.banner(G.sub(0, GD / 2, 0), x, 0, T_ww - 0.6, 1.3, 5.5, facing_y=1)
    for x in np.arange(-pw / 2 + 0.25, pw / 2, 0.45):                                  # raised portcullis
        B.box(G, x - 0.05, x + 0.05, GD / 2 - 2.6, GD / 2 - 2.5, floor0 + 5.0, floor0 + ph, IRON)
    for z in (floor0 + 5.3, floor0 + 6.2):
        B.box(G, -pw / 2, pw / 2, GD / 2 - 2.62, GD / 2 - 2.48, z, z + 0.1, IRON)
    for s in (-1, 1):                                                                 # open gate leaves
        x = s * (pw / 2 - 0.12)
        B.box(G, x - 0.1, x + 0.1, -2.0 - 2.7, -2.0, floor0, floor0 + 5.6, WOOD)
    # sconces on the passage walls; frame +y points into the passage
    for s in (-1, 1):
        Fs = G.sub(s * pw / 2, 0, s * math.pi / 2)
        B.torch(Fs, -GD / 2 + 3.0 if s > 0 else GD / 2 - 3.0, 0, floor0 + 2.8, facing_y=1)

    # ---------------------------------------------------------- keep
    S, kt = c["keep_size_m"], 2.2
    Kc = center - ug * pr * 0.3
    K = Frame(Kc.x, Kc.y, gate_a + math.pi / 2)        # local -y faces the gate
    fl = [floor0 + 8.0 * k for k in range(4)]
    deck = floor0 + 32.0
    h = S / 2 - kt
    kz0 = found(K, -S / 2, S / 2, -S / 2, S / 2)
    B.box(K, -S / 2 - 1.0, S / 2 + 1.0, -S / 2 - 1.0, S / 2 + 1.0, kz0, floor0 - 0.3)      # plinth
    cols = (-7.0, 0.0, 7.0)
    kop = {side: [] for side in "SNEW"}
    for side in "SNEW":
        for u in cols:
            if side == "S" and u == 0.0:
                kop[side].append((u, 2.8, fl[0], fl[0] + 3.8))                          # main door
            else:
                kop[side].append((u, 0.35, fl[0] + 1.6, fl[0] + 3.4))                   # slits
            for k in (1, 2, 3):
                kop[side].append((u, 1.2, fl[k] + 1.0, fl[k] + 3.4))
    B.room(K, S, S, floor0 - 0.3, deck, kt, kop)
    for z in (fl[1] - 0.2, fl[3] - 0.2):                                                # string courses
        B.box(K, -S / 2 - 0.25, S / 2 + 0.25, -S / 2 - 0.25, S / 2 + 0.25, z, z + 0.4, skip=("t", "b"))
    B.box(K, -S / 2 - 0.6, S / 2 + 0.6, -S / 2 - 0.6, S / 2 + 0.6, deck - 0.9, deck)       # corbel table
    for side_frame in (K.sub(0, -S / 2 - 0.3), K.sub(0, S / 2 + 0.3, math.pi),
                       K.sub(S / 2 + 0.3, 0, math.pi / 2), K.sub(-S / 2 - 0.3, 0, -math.pi / 2)):
        B.merlon_run(side_frame, -S / 2 - 0.6, S / 2 + 0.6, -0.3, 0.4, deck)
    # floors + stair flights (runs along the four inner walls in turn)
    w_st = 1.9
    run = 32 * 0.32
    holes = [
        (-h, -h + 2.1, -h + 1.4 + run - 4.4, -h + 1.4 + run + 0.2),          # flight 0 on W wall, +y
        (-h + 1.4 + run - 4.4, -h + 1.4 + run + 0.2, h - 2.1, h),            # flight 1 on N wall, +x
        (h - 2.1, h, h - 1.4 - run - 0.2, h - 1.4 - run + 4.4),              # flight 2 on E wall, -y
        (h - 1.4 - run - 0.2, h - 1.4 - run + 4.4, -h, -h + 2.1),            # flight 3 on S wall, -x
    ]
    B.slab(K, -h, h, -h, h, fl[0], 0.3, mat=STONE)
    for k in range(1, 4):
        B.slab(K, -h, h, -h, h, fl[k], 0.45, hole=holes[k - 1], mat=WOOD)
    B.slab(K, -h - 0.01, h + 0.01, -h - 0.01, h + 0.01, deck, 0.6, hole=holes[3], mat=STONE)
    B.stair(K, "y", -h + 1.4, 1, -h, -h + w_st, fl[0], fl[1])
    B.stair(K, "x", -h + 1.4, 1, h - w_st, h, fl[1], fl[2])
    B.stair(K, "y", h - 1.4, -1, h - w_st, h, fl[2], fl[3])
    B.stair(K, "x", h - 1.4, -1, -h, -h + w_st, fl[3], deck)
    # ground floor: stores
    for i in range(7):
        B.barrel(K.P(6.0 + (i % 3) * 0.9, 6.0 + (i // 3) * 0.9, 0), fl[0])
    for i in range(5):
        B.crate(K, 5.5 + (i % 3) * 1.0, -9.5 + (i // 3) * 1.0, fl[0])
    B.crate(K, 6.0, -9.0, fl[0] + 0.9)
    # first floor: throne hall
    B.box(K, 6.2, h, -3.4, 3.4, fl[1], fl[1] + 0.5)                                       # dais
    B.box(K, 6.2 - 0.4, 6.2, -2.4, 2.4, fl[1], fl[1] + 0.25)
    B.box(K, 8.6, 9.6, -0.8, 0.8, fl[1] + 0.5, fl[1] + 1.0, WOOD)                       # throne seat
    B.box(K, 9.4, 9.7, -0.8, 0.8, fl[1] + 1.0, fl[1] + 3.2, WOOD)                       # throne back
    for s in (-1, 1):
        B.box(K, 8.6, 9.6, s * 0.8 - 0.1, s * 0.8 + 0.1, fl[1] + 1.0, fl[1] + 1.6, WOOD)
    B.box(K, -8.0, 6.2, -1.3, 1.3, fl[1], fl[1] + 0.03, CLOTH)                          # carpet
    for x in (-4.0, 2.5):
        for y in (-5.0, 5.0):
            B.frustum(K.P(x, y, 0), 0.7, 0.6, fl[1], fl[2] - 0.45, 16)                  # pillars
    for y in (-2.6, 2.6):
        B.banner(K.sub(h, 0, math.pi / 2), y, 0, fl[1] + 6.8, 1.6, 4.2, facing_y=1)
    for x in (-3.5, 3.0):
        B.chandelier(K.P(x, 0, 0), fl[1] + 5.4, 1.2, hang_to=fl[2] - 0.45)
    # second floor: chambers
    for x in (-8.0, -4.0):
        B.box(K, x, x + 2.2, -h, -h + 2.6, fl[2], fl[2] + 0.55, WOOD)                   # bed frame
        B.box(K, x + 0.1, x + 2.1, -h + 0.05, -h + 2.5, fl[2] + 0.55, fl[2] + 0.75, CLOTH)
        B.box(K, x, x + 2.2, -h, -h + 0.2, fl[2], fl[2] + 1.6, WOOD)                    # headboard
        B.box(K, x + 0.4, x + 1.8, -h + 2.8, -h + 3.5, fl[2], fl[2] + 0.6, WOOD)        # chest
    B.table(K, 0.5, 3.5, -3.0, -1.0, fl[2])
    B.bench(K, 0.5, 3.5, -3.7, -3.3, fl[2])
    B.chandelier(K.P(-1.0, -2.0, 0), fl[2] + 5.4, 1.0, hang_to=fl[3] - 0.45)
    # third floor: armoury
    for y in (-6.0, -2.0, 2.0):
        B.box(K, -h, -h + 0.4, y, y + 2.6, fl[3], fl[3] + 2.0, WOOD)                     # racks
        for k in range(6):
            B.box(K, -h + 0.45, -h + 0.5, y + 0.2 + k * 0.42, y + 0.25 + k * 0.42, fl[3], fl[3] + 2.6, IRON)
    B.table(K, -3.0, 3.0, -1.0, 1.0, fl[3])
    B.chandelier(K.P(0, 0, 0), fl[3] + 5.4, 1.0, hang_to=deck - 0.6)
    # wall torches, on the wall without a stair flight on that floor
    # (flights: floor 0 W, 1 N, 2 E, 3 S); frames have +y into the room
    inner = {"E": K.sub(h, 0, math.pi / 2), "S": K.sub(0, -h, 0), "W": K.sub(-h, 0, -math.pi / 2), "N": K.sub(0, h, math.pi)}
    for k, side in enumerate(("E", "S", "W", "N")):
        for x in (-3.5, 3.5):
            B.torch(inner[side], x, 0, fl[k] + 2.6, facing_y=1)
    for sx in (-1, 1):                                                                  # door leaves
        hinge = K.P(sx * 1.4, -S / 2 + kt, 0)
        B.door_leaf((hinge.x, hinge.y), K.vec(sx * 0.25, 1.0), 1.4, 3.7, fl[0])
    for x in (-4.6, 4.6):                                                               # heraldic banners
        B.banner(K.sub(0, -S / 2, math.pi), -x, 0, fl[2] + 4.0, 2.2, 8.5, facing_y=1)
    B.torch(K, -2.2, -S / 2, fl[0] + 2.8, facing_y=-1)                                  # outside the door
    B.torch(K, 2.2, -S / 2, fl[0] + 2.8, facing_y=-1)
    # roof deck: pavilion + bartizans
    pav = 9.0
    B.room(K, pav, pav, deck, deck + 4.5, 0.6, {"S": [(0.0, 1.6, deck, deck + 2.6)], "N": [(0.0, 1.0, deck + 1.2, deck + 3.0)]})
    B.slab(K, -pav / 2, pav / 2, -pav / 2, pav / 2, deck + 4.6, 0.3, mat=WOOD)
    B.hip_roof(K, pav, pav, deck + 4.6, pav * 0.75)
    B.light(K.P(0, 0, deck + 3.2), CANDLE, 90.0, 0.2, "pavilion")
    spire_corner = (S / 2, S / 2)
    for sx, sy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        cp = K.P(sx * (S / 2 + 0.3), sy * (S / 2 + 0.3), 0)
        if (sx, sy) == (1, 1):
            continue
        B.frustum(cp, 0.5, 2.0, deck - 5.5, deck - 2.5, 14)
        B.frustum(cp, 2.0, 2.0, deck - 2.5, deck + 2.6, 14)
        B.cone(cp, 2.2, deck + 2.6, 5.2, 14)
    # spire: slender round tower rising from the keep's back corner
    sp = K.P(spire_corner[0] + 2.2, spire_corner[1] + 2.2, 0)
    sr = 3.4
    sh = top + c["spire_height_m"] - sr * 3.4
    B.frustum(sp, sr * 1.15, sr, kz0, top + 4.0, 24)
    B.frustum(sp, sr, sr, top + 4.0, sh, 24)
    B.frustum(sp, sr + 0.35, sr + 0.35, sh, sh + 0.9, 24, cap=True)
    B.frustum(sp, sr + 0.1, sr + 0.1, sh + 0.9, sh + 3.2, 24)
    for a in np.linspace(0, 2 * math.pi, 5)[:-1]:
        B.decal((sp.x + (sr + 0.1) * math.cos(a), sp.y + (sr + 0.1) * math.sin(a), sh + 1.2),
                (math.cos(a), math.sin(a)), 0.8, 1.6, WIN_LIT if rng.uniform() < lit else WIN_DARK)
    B.cone(sp, sr + 0.3, sh + 3.2, (sr + 0.3) * 3.4, 24)
    fz = sh + 3.2 + (sr + 0.3) * 3.4 + 3.6
    B.frustum(sp, 0.07, 0.06, fz - 2.4, fz + 0.4, 6, IRON)
    wind = Vector((0.6, 0.8))
    B.face([Vector((sp.x, sp.y, fz)), Vector((sp.x, sp.y, fz - 1.6)),
            Vector((sp.x + wind.x * 4.4, sp.y + wind.y * 4.4, fz - 1.0))], CLOTH, [(0, 0), (0, 1), (1, 0.5)])

    # ---------------------------------------------------------- great hall + houses
    edge_mid = [((P[i] + P[(i + 1) % n]) / 2, i) for i in range(n)]

    def inner_frame(i_edge, depth):
        a, b = P[i_edge], P[(i_edge + 1) % n]
        mid = (a + b) / 2
        v_in = (center - mid).normalized()
        cpos = mid + v_in * (wt / 2 + depth / 2 + 0.6)
        return Frame(cpos.x, cpos.y, math.atan2(v_in.y, v_in.x) - math.pi / 2), (b - a).length

    # hall on the edge ~100 deg from the gate, houses opposite
    def edge_near(deg):
        target = gate_a + math.radians(deg)
        return min(range(n - 1), key=lambda i: abs(math.remainder(
            math.atan2(edge_mid[i][0].y - cy, edge_mid[i][0].x - cx) - target, 2 * math.pi)))

    hall_e = edge_near(103)
    D = 15.0
    H, elen = inner_frame(hall_e, D)
    L = min(elen * 0.6, 36.0)
    t = 1.2
    wtop = floor0 + 11.0
    hop = {"N": [(0.0, 2.8, floor0, floor0 + 4.0)], "W": [(0.0, 1.6, floor0 + 7.0, floor0 + 9.4)]}
    for u in (-13.5, -7.0, 7.0, 13.5):
        if abs(u) < L / 2 - 2.5:
            hop["N"].append((u, 1.5, floor0 + 3.2, floor0 + 7.8))
    hz0 = found(H, -L / 2, L / 2, -D / 2, D / 2)
    B.box(H, -L / 2, L / 2, -D / 2, D / 2, hz0, floor0 - 0.3)
    B.room(H, L, D, floor0 - 0.3, wtop, t, hop)
    B.slab(H, -L / 2 + t, L / 2 - t, -D / 2 + t, D / 2 - t, floor0, 0.3, mat=STONE)
    rh = (D / 2) * math.tan(math.radians(50))
    B.gable_roof(H, L, D, wtop, rh, ROOF_ALT)
    for x in np.arange(-L / 2 + 4.0, L / 2 - 2.0, 4.0):                                  # trusses
        B.beam(H.P(x, -D / 2 + t, wtop - 0.3), H.P(x, D / 2 - t, wtop - 0.3), 0.35, 0.4)
        B.beam(H.P(x, -D / 2 + t, wtop - 0.1), H.P(x, 0, wtop + rh - 0.6), 0.3, 0.35)
        B.beam(H.P(x, D / 2 - t, wtop - 0.1), H.P(x, 0, wtop + rh - 0.6), 0.3, 0.35)
        B.beam(H.P(x, 0, wtop - 0.3), H.P(x, 0, wtop + rh - 0.6), 0.25, 0.25)
    di = -L / 2 + t                                                                     # dais + high table
    B.box(H, di, di + 4.2, -D / 2 + t, D / 2 - t, floor0, floor0 + 0.6)
    B.table(H, di + 1.6, di + 2.6, -4.0, 4.0, floor0 + 0.6)
    B.bench(H, di + 0.6, di + 1.1, -3.8, 3.8, floor0 + 0.6)
    for y in (-2.6, 2.6):                                                               # long tables
        B.table(H, di + 7.0, L / 2 - 6.5, y - 0.55, y + 0.55, floor0)
        for s in (-1, 1):
            B.bench(H, di + 7.0, L / 2 - 6.5, y + s * 0.95 - 0.2, y + s * 0.95 + 0.2, floor0)
    fx = L / 2 - t                                                                      # fireplace
    B.box(H, fx - 1.4, fx, -2.4, 2.4, floor0, floor0 + 0.45)
    for s in (-1, 1):
        B.box(H, fx - 1.4, fx, s * 1.7 - 0.35, s * 1.7 + 0.35, floor0, floor0 + 2.8)
    B.box(H, fx - 1.6, fx, -2.6, 2.6, floor0 + 2.8, floor0 + 3.5)
    B.box(H, fx - 1.1, fx, -1.9, 1.9, floor0 + 3.5, wtop + rh * 0.6)
    B.box(H, fx - 1.1, fx, -0.9, 0.9, wtop + rh * 0.5, wtop + rh + 2.2)                 # chimney stack
    for k in range(5):
        B.frustum(H.P(fx - 0.7, -0.8 + 0.4 * k, 0), 0.18, 0.0, floor0 + 0.45, floor0 + 1.1 + 0.25 * (k % 2), 6, FIRE)
    B.light(H.P(fx - 1.0, 0, floor0 + 1.0), (1.0, 0.45, 0.15), 380.0, 0.5, "fire")
    for x in np.arange(-L / 2 + 6.0, L / 2 - 4.0, 7.0):                                   # banners on the back wall
        B.banner(H, x + 3.5, -D / 2 + t, wtop - 1.2, 1.4, 4.0, facing_y=1)
    for x in (-L / 4, 0.0, L / 4):
        B.chandelier(H.P(x, 0, 0), wtop - 2.2, 1.3, hang_to=wtop - 0.3)
    for u in (-10.0, 10.0):
        if abs(u) < L / 2 - 2:
            B.torch(H, u, D / 2 - t, floor0 + 2.8, facing_y=-1)
    for sx in (-1, 1):
        hinge = H.P(sx * 1.4, D / 2 - t, 0)
        B.door_leaf((hinge.x, hinge.y), H.vec(sx * 0.25, -1.0), 1.4, 3.9, floor0)
    B.torch(H, -2.2, D / 2, floor0 + 3.0, facing_y=1)
    B.torch(H, 2.2, D / 2, floor0 + 3.0, facing_y=1)
    info["hall_camera"] = {"pos": tuple(H.P(di + 1.0, 0.0, floor0 + 0.6 + 1.65)),
                           "target": tuple(H.P(fx, 0.0, floor0 + 2.6))}

    for k, deg in enumerate((-103.0, -150.0)):
        e = edge_near(deg)
        if e in (hall_e,):
            continue
        Dh, Lh = 8.0, 11.0
        Hf, _ = inner_frame(e, Dh)
        z1 = floor0 + 5.5
        B.box(Hf, -Lh / 2, Lh / 2, -Dh / 2, Dh / 2, found(Hf, -Lh / 2, Lh / 2, -Dh / 2, Dh / 2), floor0 - 0.3)
        B.room(Hf, Lh, Dh, floor0 - 0.3, z1, 0.8,
               {"N": [(-2.0, 1.6, floor0, floor0 + 2.6), (2.5, 1.0, floor0 + 1.2, floor0 + 2.8)],
                "E": [(0.0, 1.0, floor0 + 1.2, floor0 + 2.8)]})
        B.slab(Hf, -Lh / 2 + 0.8, Lh / 2 - 0.8, -Dh / 2 + 0.8, Dh / 2 - 0.8, floor0, 0.3, mat=WOOD)
        B.gable_roof(Hf, Lh, Dh, z1, (Dh / 2) * math.tan(math.radians(48)), ROOF if k else ROOF_ALT)
        B.table(Hf, -1.5, 1.0, -1.0, 0.2, floor0)
        B.bench(Hf, -1.5, 1.0, -1.6, -1.25, floor0)
        B.box(Hf, 2.2, 4.4, -3.2, -1.6, floor0, floor0 + 0.5, WOOD)
        B.box(Hf, 2.3, 4.3, -3.15, -1.65, floor0 + 0.5, floor0 + 0.65, CLOTH)
        B.barrel(Hf.P(-4.2, -2.8, 0), floor0)
        B.light(Hf.P(0, 0, floor0 + 3.0), TORCH, 80.0, 0.2, "house")
        hinge = Hf.P(-2.8, Dh / 2 - 0.8, 0)
        B.door_leaf((hinge.x, hinge.y), Hf.vec(-0.3, -1.0), 1.5, 2.5, floor0)

    # courtyard paving: flagstones at the building-floor level so every
    # threshold lines up (polygon inset from the curtain wall's inner face)
    ring = [center + (q - center) * ((q - center).length - wt / 2 - 0.2) / (q - center).length for q in P]
    zt, zb = floor0, top - 0.6
    pts_t = [Vector((q.x, q.y, zt)) for q in ring]
    B.face(pts_t, STONE, [(q.x, q.y + 1000.0) for q in pts_t])
    for i in range(n):
        a, b = ring[i], ring[(i + 1) % n]
        B.face([Vector((a.x, a.y, zb)), Vector((b.x, b.y, zb)), Vector((b.x, b.y, zt)), Vector((a.x, a.y, zt))], STONE,
               [(0, -1), (1, -1), (1, 0), (0, 0)])

    # well + courtyard dressing
    wc = center + ug * 14.0 + Vector((-ug.y, ug.x)) * 12.0
    B.frustum(wc, 1.5, 1.5, top - 0.5, floor0 + 0.9, 16)
    B.frustum(wc, 1.1, 1.1, top - 0.5, floor0 + 0.9, 16)
    Wf = Frame(wc.x, wc.y, gate_a)
    for s in (-1, 1):
        B.box(Wf, s * 1.4 - 0.1, s * 1.4 + 0.1, -0.1, 0.1, floor0, floor0 + 2.6, WOOD)
    B.gable_roof(Wf, 3.2, 1.6, floor0 + 2.6, 0.9, ROOF, eave=0.3, inner=False)
    B.beam(Wf.P(-1.4, 0, floor0 + 2.2), Wf.P(1.4, 0, floor0 + 2.2), 0.12, 0.12)
    for i in range(4):
        B.barrel(Wf.P(5.0 + (i % 2) * 0.9, 3.0 + (i // 2) * 0.9, 0), top)

    # ---------------------------------------------------------- courtyard life
    left = Vector((-ug.y, ug.x))

    def Pc(fw, lf, yaw_off=0.0):
        q = center + ug * fw + left * lf
        return Frame(q.x, q.y, gate_a + yaw_off)

    for k, (fw, lf) in enumerate(((31.0, -12.0), (31.0, -18.5), (24.0, -24.0))):      # market
        B.stall(Pc(fw, lf, math.pi / 2), floor0, (CLOTH, ROOF_ALT, HAY)[k % 3])
    B.cart(Pc(40.0, 16.0, 0.4), floor0)                                                 # hay cart by the gate
    for i in range(4):
        B.hay_bale(Pc(43.0 + (i % 2) * 1.2, 19.0 + (i // 2) * 0.7), 0, 0, floor0 + (0.45 if i == 3 else 0.0))
    B.smithy(Pc(-4.0, -30.0, math.pi / 2), floor0)                                      # smithy
    for i, lf in enumerate((-14.0, -16.5, -19.0)):                                      # training yard
        B.dummy(Pc(9.0 + (i % 2) * 2.0, lf), 0, 0, floor0)
    B.logs(Pc(-2.0, 36.0, math.pi / 2), 0, 0, floor0)                                   # firewood by the hall
    B.logs(Pc(1.0, 36.0, math.pi / 2), 0, 0, floor0)

    # ---------------------------------------------------------- player start
    road = site["road"]
    ps = Vector(road[-1])
    look = Vector((cx, cy, 0)) - Vector((ps.x, ps.y, 0))
    info["player_start"] = {"pos": (ps.x, ps.y, ps.z + 1.2),
                            "yaw_deg": math.degrees(math.atan2(look.y, look.x))}
    gi = G.P(0, -GD / 2 - 3.0, floor0 + 1.7)
    info["courtyard_camera"] = {"pos": tuple(gi), "target": tuple(K.P(0, -S / 2, fl[0] + 7.0))}

    me = bpy.data.meshes.new("RW_Castle")
    B.bm.to_mesh(me)
    B.bm.free()
    for key in MAT_KEYS:
        me.materials.append(mats[key])
    ob = bpy.data.objects.new("RW_Castle", me)
    coll.objects.link(ob)
    return ob, info


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
