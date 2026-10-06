"""Meadow features: weathered fences along the lane and across the fields,
dry-stone walls, and abandoned buildings, all original designs built with
the castle's architectural kit:

  * a roofless stone cottage with jagged walls, fallen rafters and slates
  * a timber barn skeleton with a sagging, holed roof and missing boards
  * a lone chimney stack over the footings of a burnt-out house
  * the arched gable wall of a ruined chapel
  * an old well and a cart wreck

Grass is allowed to grow inside the ruins; trees and rocks keep clear of
their footprints. Returns (object, info) like castle.build."""
import math

import bpy
import numpy as np
from mathutils import Vector

from .castle import (Builder, Frame, HAY, IVY, MAT_KEYS, ROOF, ROOF_ALT, RUBBLE, STONE, WIN_DARK, WOOD)


class Meadow:
    def __init__(self, cfg, terrain):
        self.cfg, self.T = cfg, terrain
        self.m = cfg["meadow"]
        self.rng = np.random.default_rng(self.m["seed"])
        self.B = Builder(0.0)
        self.footprints = []        # (x, y, radius): no trees / rocks here
        self.cameras = {}

    # ----------------------------------------------------------------- helpers
    def ground(self, x, y):
        return float(self.T.height_at(np.array(x, float), np.array(y, float)))

    def lane(self, y):
        return float(self.T.river_center(np.array(y, float)))

    def site(self, y, side, offset, jitter=0.25):
        """Frame for a building beside the lane, front (local -y) facing it."""
        x = self.lane(y) + side * offset
        yaw = -side * math.pi / 2 + self.rng.uniform(-jitter, jitter)
        return Frame(x, y, yaw)

    def sun_side(self, cx, cy, dist, turn_deg, height, target_z):
        """Camera on the sun's side of a subject (light falls across it),
        turned a little off the sun line so it isn't flat-lit."""
        az = math.radians(self.cfg["lighting"]["sun_azimuth_deg"] + turn_deg)
        x, y = cx + math.sin(az) * dist, cy + math.cos(az) * dist
        return {"pos": (x, y, self.ground(x, y) + height), "target": (cx, cy, target_z)}

    def ground_stats(self, F, hw, hd):
        zs = [self.ground(*F.P(x, y, 0)[:2]) for x in np.linspace(-hw, hw, 5) for y in np.linspace(-hd, hd, 5)]
        return float(np.min(zs)), float(np.median(zs))

    def broken_wall(self, F, x0, x1, thick, z0, profile, openings=(), colw=0.24, mat=RUBBLE):
        """Masonry wall along local x with a jagged, collapsed top.
        profile(x) -> absolute top height; openings (xc, w, zb, zt, arch)."""
        rng = self.rng
        for xa in np.arange(x0, x1 - 1e-6, colw):
            xb = min(xa + colw, x1)
            xm = (xa + xb) / 2
            top = profile(xm) + rng.uniform(-0.12, 0.12)
            if top <= z0 + 0.35:
                continue
            spans = []
            for xc, w, zb, zt, arch in openings:
                if abs(xm - xc) < w / 2:
                    if arch:
                        zt = zt - w / 2 + math.sqrt(max(0.0, (w / 2) ** 2 - (xm - xc) ** 2))
                    spans.append((zb, zt))
            zc = z0
            for zb, zt in sorted(spans):
                if zb > zc:
                    self.B.box(F, xa, xb, -thick / 2, thick / 2, zc, min(zb, top), mat)
                zc = max(zc, zt)
            if top > zc:
                self.B.box(F, xa, xb, -thick / 2, thick / 2, zc, top, mat)

    def wavy(self, base, amp, lo=0.0, collapse=None):
        """Smooth random height profile for ruined wall tops."""
        rng = self.rng
        f1, f2 = rng.uniform(0.25, 0.6), rng.uniform(0.9, 1.6)
        p1, p2 = rng.uniform(0, 6.28, 2)

        def prof(x):
            h = base + amp * (0.6 * math.sin(x * f1 + p1) + 0.4 * math.sin(x * f2 + p2))
            if collapse is not None:
                h *= collapse(x)
            return lo + max(0.0, h)
        return prof

    def rubble(self, F, cx, cy, spread, z, n, size=(0.22, 0.7), mat=RUBBLE):
        rng = self.rng
        for _ in range(n):
            x = cx + rng.normal(0, spread)
            y = cy + rng.normal(0, spread * 0.6)
            p = F.P(x, y, 0)
            zz = self.ground(p.x, p.y) + rng.uniform(-0.05, 0.12)
            d = Vector(rng.normal(0, 1, 3))
            d.z *= 0.35
            d.normalize()
            L = rng.uniform(*size)
            self.B.beam((p.x, p.y, zz), (p.x + d.x * L, p.y + d.y * L, zz + d.z * L),
                        L * rng.uniform(0.5, 0.9), L * rng.uniform(0.35, 0.7), mat)

    def slates(self, F, cx, cy, spread, n):
        rng = self.rng
        for _ in range(n):
            p = F.P(cx + rng.normal(0, spread), cy + rng.normal(0, spread * 0.6), 0)
            z = self.ground(p.x, p.y) + 0.03
            a = rng.uniform(0, 6.28)
            d = Vector((math.cos(a), math.sin(a), rng.uniform(-0.2, 0.3)))
            self.B.beam((p.x, p.y, z), (p.x + d.x * 0.34, p.y + d.y * 0.34, z + d.z * 0.34), 0.22, 0.02, ROOF)

    # ----------------------------------------------------------------- fences
    def fence(self, pts, damage=0.3, step=2.4):
        """Post-and-rail fence along a 2D polyline, weathered: leaning and
        missing posts, missing and dropped rails."""
        rng, B = self.rng, self.B
        pts = [Vector((p[0], p[1])) for p in pts]
        samples = []
        for a, b in zip(pts[:-1], pts[1:]):
            n = max(1, int((b - a).length // step))
            for k in range(n):
                samples.append(a + (b - a) * (k / n))
        samples.append(pts[-1])
        posts = []
        for q in samples:
            if rng.uniform() < 0.06 * damage / 0.3:
                posts.append(None)
                continue
            z = self.ground(q.x, q.y)
            lean = rng.normal(0, 0.05)
            if rng.uniform() < 0.12 * damage / 0.3:
                lean = rng.uniform(0.2, 0.5) * rng.choice([-1, 1])
            la = rng.uniform(0, 6.28)
            axis = Vector((math.cos(la) * math.sin(lean), math.sin(la) * math.sin(lean), math.cos(lean)))
            base = Vector((q.x, q.y, z - 0.35))
            B.beam(base, base + axis * 1.65, 0.13, 0.13, WOOD)
            posts.append((base, axis))
        for p0, p1 in zip(posts[:-1], posts[1:]):
            if p0 is None or p1 is None:
                continue
            for hgt in (0.85, 1.3):
                r = rng.uniform()
                a = p0[0] + p0[1] * hgt
                b = p1[0] + p1[1] * hgt
                if r < 0.15 * damage / 0.3:
                    continue                                  # missing rail
                if r < 0.25 * damage / 0.3:
                    b = Vector((b.x, b.y, self.ground(b.x, b.y) + 0.05))   # dropped at one end
                B.beam(a, b, 0.06, 0.13, WOOD)

    def dry_stone_wall(self, pts, height=1.0):
        for a, b in zip(pts[:-1], pts[1:]):
            a, b = Vector(a), Vector(b)
            d = b - a
            F = Frame(a.x, a.y, math.atan2(d.y, d.x))
            zg = min(self.ground(a.x, a.y), self.ground(b.x, b.y))
            gap_at = self.rng.uniform(0.2, 0.8) * d.length if self.rng.uniform() < 0.6 else -99

            def prof(x, zg=zg, ga=gap_at):
                base = zg + height * (0.75 + 0.25 * math.sin(x * 0.7))
                return zg - 1.0 if abs(x - ga) < 2.0 else base          # a collapsed gap
            self.broken_wall(F, 0.0, d.length, 0.75, zg - 0.4, prof, colw=0.6)

    # ----------------------------------------------------------------- ruins
    def cottage(self, y, side, offset):
        F = self.site(y, side, offset)
        W, D, t = 9.0, 6.5, 0.7
        z0, zg = self.ground_stats(F, W / 2, D / 2)
        self.B.v0 = zg
        z0 -= 0.4
        hw, hd = W / 2, D / 2
        coll = lambda x: 0.35 + 0.65 * (1 - max(0.0, (x - 1.5) / 3.0)) if x > 1.5 else 1.0  # noqa: E731
        self.broken_wall(F.sub(0, -hd + t / 2), -hw, hw, t, z0, self.wavy(2.6, 1.0, zg, coll),
                         [(-1.3, 1.1, zg, zg + 2.1, False), (2.3, 0.9, zg + 0.9, zg + 2.0, False)])
        self.broken_wall(F.sub(0, hd - t / 2), -hw, hw, t, z0, self.wavy(3.0, 0.8, zg),
                         [(0.4, 0.9, zg + 0.9, zg + 2.0, False)])
        gable = lambda x: 3.2 + 2.6 * max(0.0, 1 - abs(x) / (hd - t)) * (1.0 if x < 0.6 else 0.45)  # noqa: E731
        self.broken_wall(F.sub(hw - t / 2, 0, math.pi / 2), -hd + t, hd - t, t, z0, lambda x: zg + gable(x))
        self.broken_wall(F.sub(-hw + t / 2, 0, math.pi / 2), -hd + t, hd - t, t, z0, self.wavy(1.4, 0.8, zg))
        # hearth and chimney breast on the east gable
        self.B.box(F, hw - t - 1.0, hw - t, -1.1, 1.1, zg - 0.2, zg + 0.5, RUBBLE)
        self.B.box(F, hw - t - 0.6, hw - t, -0.8, 0.8, zg + 0.5, zg + 4.6, RUBBLE)
        # fallen rafters and slates, rubble inside and out
        for k in range(5):
            x = -hw + 1.2 + k * 1.6 + self.rng.uniform(-0.3, 0.3)
            top = F.P(x, hd - t, zg + 2.7)
            low = F.P(x + self.rng.uniform(-0.8, 0.8), self.rng.uniform(-1.5, 1.0), 0)
            self.B.beam(top, (low.x, low.y, self.ground(low.x, low.y) + 0.12), 0.16, 0.2, WOOD)
        self.slates(F, 0.5, 0.0, 1.8, 40)
        self.rubble(F, 3.0, -hd - 0.9, 1.1, 0, 60)          # where the front wall came down
        self.rubble(F, -hw - 0.7, 0.0, 1.3, 0, 50)          # the collapsed west gable
        self.rubble(F, 0.0, 0.0, 2.0, 0, 30)
        self.B.ivy_wall(F.sub(0, hd, 0), self.rng.uniform(-2.5, 2.5), 3.2, 0.0, zg - 0.3, 3.0, self.rng, 650)
        self.footprints.append((F.ox, F.oy, 9.0))
        cam = Vector((self.lane(y - 26), y - 26))
        self.cameras["cottage_camera"] = {"pos": (cam.x, cam.y, self.ground(cam.x, cam.y) + 1.6),
                                          "target": (F.ox, F.oy, zg + 2.0)}
        return F, zg

    def well(self, x, y):
        zg = self.ground(x, y)
        self.B.v0 = zg
        self.B.round_shell((x, y), 0.72, 1.05, zg - 0.4, zg + 0.8, (), seg=16, mat=RUBBLE)
        self.B.disc_slab((x, y), 0.73, zg + 0.1, 0.05, mat=WIN_DARK, seg=12)
        F = Frame(x, y, self.rng.uniform(0, 6.28))
        self.B.beam(F.P(-1.1, 0, zg - 0.3), F.P(-1.1, 0, zg + 2.3), 0.16, 0.16, WOOD)
        self.B.beam(F.P(1.1, 0, zg - 0.3), F.P(1.6, 0.3, zg + 2.0), 0.16, 0.16, WOOD)          # leaning post
        self.B.beam(F.P(-1.1, 0, zg + 2.2), F.P(0.6, 0.6, zg + 0.9), 0.12, 0.12, WOOD)         # fallen beam
        for k in range(4):
            p = F.P(self.rng.uniform(-1.8, 1.8), self.rng.uniform(1.2, 2.4), 0)
            z = self.ground(p.x, p.y) + 0.03
            self.B.beam((p.x, p.y, z), (p.x + 1.4, p.y + self.rng.uniform(-0.4, 0.4), z + 0.05), 0.22, 0.03, WOOD)
        self.footprints.append((x, y, 3.0))

    def barn(self, y, side, offset):
        F = self.site(y, side, offset, 0.15)
        rng, B = self.rng, self.B
        W, D, H, RH = 14.0, 9.0, 4.4, 3.6
        z0, zg = self.ground_stats(F, W / 2, D / 2)
        xs = np.linspace(-W / 2, W / 2, 5)
        tops = {}
        for x in xs:
            for s in (-1, 1):
                base = F.P(x, s * D / 2, zg - 0.4)
                lean = (0.0, 0.0)
                if x == xs[-1] and s > 0:
                    lean = (0.9, -0.6)                       # collapsing corner
                top = F.P(x + lean[0], s * D / 2 + lean[1], zg + H - (0.5 if lean[0] else 0.0))
                B.beam(base, top, 0.25, 0.25, WOOD)
                tops[(x, s)] = top
        for s in (-1, 1):                                     # wall plates
            for x0, x1 in zip(xs[:-1], xs[1:]):
                B.beam(tops[(x0, s)], tops[(x1, s)], 0.2, 0.22, WOOD)
        ridge = []
        for x in xs:
            B.beam(tops[(x, -1)], tops[(x, 1)], 0.2, 0.25, WOOD)                       # tie beam
            r = F.P(x, 0, zg + H + RH - (0.6 if x == xs[-1] else 0.0))                  # ridge sags at the end
            ridge.append(r)
            for s in (-1, 1):
                if rng.uniform() < 0.25:
                    continue                                                          # missing rafter
                B.beam(tops[(x, s)], r, 0.18, 0.2, WOOD)
        for a, b in zip(ridge[:-1], ridge[1:]):
            B.beam(a, b, 0.2, 0.22, WOOD)
        # remaining roof boards on part of one slope, with holes
        for k in range(16):
            u = (k + 0.5) / 16
            if rng.uniform() < 0.3:
                continue
            for x0 in np.arange(-W / 2, 1.0, 2.6):
                if rng.uniform() < 0.25:
                    continue
                p0 = F.P(x0, -D / 2 * (1 - u), zg + H + RH * u + 0.15)
                p1 = F.P(x0 + rng.uniform(2.0, 2.6), -D / 2 * (1 - u), zg + H + RH * u + 0.15 - rng.uniform(0, 0.1))
                B.beam(p0, p1, 0.3, 0.035, WOOD)
        # side boards with gaps and broken lengths
        for x in np.arange(-W / 2 + 0.2, W / 2 - 0.2, 0.3):
            if rng.uniform() < 0.3:
                continue
            h = H * (1.0 if rng.uniform() < 0.6 else rng.uniform(0.3, 0.8))
            B.beam(F.P(x, -D / 2 - 0.15, zg - 0.2), F.P(x, -D / 2 - 0.15, zg + h), 0.27, 0.035, WOOD)
        for k in range(4):                                                            # hay remnants
            B.box(F, -4.0 + k * 1.3, -2.8 + k * 1.3, 1.0, 1.6, zg, zg + 0.5 + 0.25 * (k % 2), HAY)
        self.footprints.append((F.ox, F.oy, 11.0))
        # paddock behind the barn
        far = F.vec(0, 1)
        c0 = Vector((F.ox, F.oy)) + far * (D / 2 + 1.0)
        right = Vector((far.y, -far.x))
        corners = [c0 - right * 16, c0 + right * 14, c0 + right * 14 + far * 22, c0 - right * 16 + far * 22, c0 - right * 16]
        self.fence([tuple(p) for p in corners], damage=0.45)
        self.cameras["barn_camera"] = self.sun_side(F.ox, F.oy, 30.0, 40.0, 1.7, zg + 3.2)

    def chimney_ruin(self, y, side, offset):
        F = self.site(y, side, offset)
        W, D = 8.0, 6.0
        z0, zg = self.ground_stats(F, W / 2, D / 2)
        self.B.v0 = zg
        z0 -= 0.4
        for fr, x0, x1 in ((F.sub(0, -D / 2), -W / 2, W / 2), (F.sub(0, D / 2), -W / 2, W / 2),
                           (F.sub(-W / 2, 0, math.pi / 2), -D / 2, D / 2)):
            self.broken_wall(fr, x0, x1, 0.6, z0, self.wavy(0.5, 0.45, zg))
        # the stack, with a fireplace opening at its foot
        cx = W / 2 - 0.3
        self.B.box(F, cx - 0.8, cx + 0.5, -0.8, -0.35, z0, zg + 7.8, RUBBLE)
        self.B.box(F, cx - 0.8, cx + 0.5, 0.35, 0.8, z0, zg + 7.8, RUBBLE)
        self.B.box(F, cx - 0.1, cx + 0.5, -0.35, 0.35, z0, zg + 7.8, RUBBLE)
        self.B.box(F, cx - 0.8, cx - 0.1, -0.35, 0.35, zg + 1.2, zg + 7.8, RUBBLE)
        self.B.box(F, cx - 0.95, cx + 0.65, -0.95, 0.95, zg + 7.8, zg + 8.05, RUBBLE)
        for k in range(6):                                                            # charred timbers
            p = F.P(self.rng.uniform(-3, 2.5), self.rng.uniform(-2, 2), 0)
            z = self.ground(p.x, p.y) + 0.1
            a = self.rng.uniform(0, 6.28)
            self.B.beam((p.x, p.y, z), (p.x + 2.6 * math.cos(a), p.y + 2.6 * math.sin(a), z + 0.15), 0.18, 0.18, WOOD)
        self.rubble(F, 0.0, 0.0, 2.4, 0, 70)
        self.footprints.append((F.ox, F.oy, 8.0))

    def chapel(self, y, side, offset):
        F = self.site(y, side, offset, 0.1)
        W, L, t = 9.0, 13.0, 0.9
        z0, zg = self.ground_stats(F, W / 2, L / 2)
        self.B.v0 = zg
        z0 -= 0.5
        gable = lambda x: zg + 5.5 + 5.0 * max(0.0, 1 - abs(x) / (W / 2)) * (1.0 if x < 2.2 else 0.55)  # noqa: E731
        self.broken_wall(F.sub(0, -L / 2), -W / 2, W / 2, t, z0, gable,
                         [(0.0, 2.0, zg + 3.0, zg + 7.4, True), (0.0, 1.6, zg, zg + 2.7, True)], colw=0.4)
        for s in (-1, 1):                                      # side walls fading into rubble
            fade = lambda x: max(0.0, 1.0 - (x + L / 2) / (L * 0.9))  # noqa: E731
            self.broken_wall(F.sub(s * (W / 2 - t / 2), 0, math.pi / 2), -L / 2 + t, L / 2, t, z0,
                             self.wavy(3.2, 1.0, zg, fade),
                             [(-L / 2 + 4.0, 1.2, zg + 1.6, zg + 4.0, True)])
        self.rubble(F, 0.0, 1.5, 3.2, 0, 110)
        self.B.ivy_wall(F.sub(0, -L / 2 - t / 2, math.pi), self.rng.uniform(-2.5, 2.5), 3.0, 0.0, zg - 0.3, 6.0,
                        self.rng, 900)
        self.footprints.append((F.ox, F.oy, 12.0))
        self.cameras["chapel_camera"] = self.sun_side(F.ox, F.oy, 34.0, -35.0, 1.3, zg + 5.0)

    def cart(self, y, side, offset):
        F = self.site(y, side, offset, 0.6)
        zg = self.ground(F.ox, F.oy)
        B = self.B
        B.beam(F.P(-1.5, -0.8, zg + 0.25), F.P(1.5, -0.8, zg + 0.75), 0.15, 0.15, WOOD)
        B.beam(F.P(-1.5, 0.8, zg + 0.25), F.P(1.5, 0.8, zg + 0.75), 0.15, 0.15, WOOD)
        for x in np.arange(-1.4, 1.5, 0.3):
            if self.rng.uniform() < 0.3:
                continue
            zz = zg + 0.3 + (x + 1.5) / 3.0 * 0.5
            B.beam(F.P(x, -0.8, zz + 0.1), F.P(x, 0.8, zz + 0.1), 0.26, 0.04, WOOD)
        B.wheel(F.P(0.6, 0.95, zg + 0.55), F.vec(0, 1), 0.55)
        B.wheel(F.P(-2.4, -1.4, zg + 0.55), F.vec(0.3, 1), 0.55)                       # loose wheel
        B.barrel(F.P(2.2, -1.2, 0), zg - 0.05, 0.3, 0.8)
        self.footprints.append((F.ox, F.oy, 4.0))


# ---------------------------------------------------------------------------
def build(cfg, terrain, mats, coll):
    M = Meadow(cfg, terrain)
    rng = M.rng
    core = cfg["meadow"]["radius_m"]

    _, zc = M.cottage(-430.0, 1, 22.0)
    cottage_xy = (M.lane(-430.0) + 22.0 + 7.0, -421.0)
    M.well(cottage_xy[0], cottage_xy[1] + 3.0)
    M.barn(-180.0, -1, 32.0)
    M.chimney_ruin(90.0, 1, 40.0)
    M.cart(105.0, -1, 8.0)
    M.chapel(390.0, -1, 46.0)

    # fences along stretches of the lane, either side
    for y0, y1, s in ((-640.0, -520.0, -1), (-640.0, -560.0, 1), (-380.0, -255.0, 1),
                      (-120.0, -15.0, -1), (150.0, 265.0, 1), (300.0, 360.0, -1)):
        ys = np.linspace(y0, y1, 12)
        M.fence([(M.lane(y) + s * 2.7, y) for y in ys], damage=0.3)

    # field fences and dry-stone walls scattered across the meadow
    placed = 0
    tries = 0
    while placed < cfg["meadow"]["field_fences"] and tries < 400:
        tries += 1
        r = core * 0.85 * math.sqrt(rng.uniform())
        a = rng.uniform(0, 2 * math.pi)
        x, y = r * math.cos(a), r * math.sin(a) * 1.1
        if abs(x - M.lane(y)) < 25 or any(math.hypot(x - fx, y - fy) < fr + 15 for fx, fy, fr in M.footprints):
            continue
        hd = rng.uniform(0, 2 * math.pi)
        L = rng.uniform(40.0, 110.0)
        pts = []
        for k in range(5):
            hd += rng.normal(0, 0.12)
            pts.append((x, y))
            x += math.cos(hd) * L / 4
            y += math.sin(hd) * L / 4
        if any(abs(px - M.lane(py)) < 8 for px, py in pts):
            continue
        if placed % 3 == 2:
            M.dry_stone_wall(pts, rng.uniform(0.8, 1.15))
        else:
            M.fence(pts, damage=0.5)
        placed += 1

    terrain.features = M.footprints
    ys = -core * 1.2
    start = (M.lane(ys), ys)
    nxt = (M.lane(ys + 5.0), ys + 5.0)
    info = {
        "lights": M.B.lights,
        "player_start": {"pos": (start[0], start[1], M.ground(*start) + 1.2),
                         "yaw_deg": math.degrees(math.atan2(nxt[1] - start[1], nxt[0] - start[0]))},
        **M.cameras,
    }
    ov = (M.lane(-760.0) + 25.0, -760.0)
    info["overview_camera"] = {"pos": (ov[0], ov[1], M.ground(*ov) + 38.0),
                               "target": (M.lane(-120.0), -120.0, M.ground(M.lane(-120.0), -120.0))}
    # dense grass wherever a camera looks or a player is likely to linger
    terrain.hotspots = [(c["pos"][0], c["pos"][1], 130.0) for k, c in M.cameras.items()] + \
                       [(fx, fy, fr + 45.0) for fx, fy, fr in M.footprints]

    me = bpy.data.meshes.new("RW_MeadowProps")
    M.B.bm.to_mesh(me)
    M.B.bm.free()
    for key in MAT_KEYS:
        me.materials.append(mats[key])
    ob = bpy.data.objects.new("RW_MeadowProps", me)
    coll.objects.link(ob)
    return ob, info
