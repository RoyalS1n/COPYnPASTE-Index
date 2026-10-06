"""Heightfield generation, biome masks and Blender mesh construction.

Coordinates: metres, Blender world space. The valley runs along +Y, the
camera starts near the -Y edge looking north up the valley.

Two meshes are produced:
  * the main terrain (size_m square, full resolution, carries the river)
  * a coarse "skirt" (skirt_size_m square) of distant ranges that gives the
    shot layered depth and hides the world edge. Inside the main square the
    skirt sits a few metres under the main terrain.
"""
import numpy as np

from .noise import Perlin2D, fbm, ridged, smoothstep


def compute_masks(cfg, h, X, Y, spacing):
    """Biome weights from height/slope/noise. Returns (slope_deg, masks)."""
    b = cfg["biome"]
    gy, gx = np.gradient(h, spacing)
    nz = 1.0 / np.sqrt(1.0 + gx * gx + gy * gy)
    slope = np.degrees(np.arccos(np.clip(nz, -1, 1)))
    n = Perlin2D(int(cfg["seed"]) + 50)
    brk = fbm(n, X / 55.0, Y / 55.0, 4)
    big = fbm(n, X / 400.0 + 9, Y / 400.0 - 4, 4)

    snow_line = b["snowline_m"] + 45.0 * big
    snow = smoothstep(snow_line - 25, snow_line + 25, h + 15 * brk) * (1 - smoothstep(40, 56, slope + 8 * brk))
    rock = smoothstep(36, 50, slope + 9 * brk)
    rock = np.maximum(rock, smoothstep(b["treeline_m"] + 60, b["snowline_m"] + 40, h + 30 * brk) * 0.7)
    rock *= (1 - snow)
    wet = 1 - smoothstep(b["water_level_m"] + 0.2, b["water_level_m"] + 1.6, h)
    grass = np.clip(1 - rock - snow, 0, 1) * (1 - wet * 0.7)
    dry = smoothstep(-0.2, 0.6, big + 0.4 * brk) * smoothstep(b["treeline_m"] * 0.4, b["treeline_m"], h + 40 * brk)
    return slope, {
        "grass": grass.astype(np.float32),
        "rock": rock.astype(np.float32),
        "snow": snow.astype(np.float32),
        "wet": wet.astype(np.float32),
        "dry": dry.astype(np.float32),
    }


def grid_mesh(bpy, name, X, Y, H, masks, uv_size, hole=None):
    """Build a quad-grid mesh with mask attributes, a packed 'Masks' colour
    attribute (R grass, G rock, B snow, A wet) and planar UVs. hole =
    (r0, r1, c0, c1) removes those grid cells (a detail patch fills them)."""
    r_y, r_x = H.shape
    verts = np.dstack([X, Y, H]).reshape(-1, 3).astype(np.float32)
    idx = np.arange(r_y * r_x).reshape(r_y, r_x)
    faces = np.stack([idx[:-1, :-1].ravel(), idx[:-1, 1:].ravel(),
                      idx[1:, 1:].ravel(), idx[1:, :-1].ravel()], axis=1).astype(np.int32)
    if hole is not None:
        r0, r1, c0, c1 = hole
        ii, jj = np.meshgrid(np.arange(r_y - 1), np.arange(r_x - 1), indexing="ij")
        keep = ~((ii >= r0) & (ii < r1) & (jj >= c0) & (jj < c1))
        faces = faces[keep.ravel()]
    me = bpy.data.meshes.new(name)
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
    uvs = np.stack([verts[lv, 0] / uv_size + 0.5, verts[lv, 1] / uv_size + 0.5], axis=1)
    uv.data.foreach_set("uv", uvs.astype(np.float32).ravel())

    for k, m in masks.items():
        me.attributes.new(k, "FLOAT", "POINT").data.foreach_set("value", m.ravel())
    col = me.color_attributes.new("Masks", "FLOAT_COLOR", "POINT")
    bare = np.maximum(masks["wet"], masks["path"]) if "path" in masks else masks["wet"]
    packed = np.stack([masks["grass"].ravel(), masks["rock"].ravel(),
                       masks["snow"].ravel(), bare.ravel()], axis=1)
    col.data.foreach_set("color", packed.astype(np.float32).ravel())
    return me


class Terrain:
    def __init__(self, cfg):
        t = cfg["terrain"]
        self.cfg = cfg
        self.size = float(t["size_m"])
        self.res = int(t["resolution"])
        self.half = self.size * 0.5
        self.spacing = self.size / (self.res - 1)
        self.seed = int(cfg["seed"])
        lin = np.linspace(-self.half, self.half, self.res)
        self.X, self.Y = np.meshgrid(lin, lin)  # row index = y, column = x
        self.height = None
        self.masks = {}

    # ------------------------------------------------------------------ shape
    @property
    def meadow(self):
        return self.cfg["terrain"].get("mode") == "meadow"

    def river_center(self, y):
        """Centre line of the valley river or, in meadow mode, of the lane.
        Cameras and scatter follow it in both modes."""
        if self.meadow:
            m = self.cfg["meadow"]
            rng = np.random.default_rng(self.seed + 13)
            ph1, ph2 = rng.uniform(0, 2 * np.pi, 2)
            lam = m["path_wavelength_m"]
            return (m["path_amp_m"] * np.sin(2 * np.pi * y / lam + ph1)
                    + 0.3 * m["path_amp_m"] * np.sin(2 * np.pi * y / (0.37 * lam) + ph2))
        t = self.cfg["terrain"]
        lam = t["meander_wavelength_m"]
        amp = t["meander_amp_m"]
        rng = np.random.default_rng(self.seed + 11)
        ph1, ph2 = rng.uniform(0, 2 * np.pi, 2)
        return (amp * np.sin(2 * np.pi * y / lam + ph1)
                + 0.32 * amp * np.sin(2 * np.pi * y / (0.41 * lam) + ph2))

    def build(self):
        if self.meadow:
            return self._build_meadow()
        t = self.cfg["terrain"]
        X, Y = self.X, self.Y
        n1, n2, n3, n4 = (Perlin2D(self.seed + i) for i in range(4))

        # domain warp makes ridges organic instead of grid-like
        ws = t["warp_strength"] * 260.0
        wx = X + ws * fbm(n3, X / 900.0, Y / 900.0, 4)
        wy = Y + ws * fbm(n4, X / 900.0 + 7.1, Y / 900.0 - 3.3, 4)

        center = self.river_center(Y)
        dist = np.abs(X - center + 18.0 * fbm(n2, X / 160.0, Y / 160.0, 3))

        half_floor = t["valley_width_m"] * 0.5
        valley = smoothstep(half_floor, half_floor + t["valley_slope_m"], dist)
        # the far end of the valley closes into a massif: a strong backdrop
        far = smoothstep(0.05 * self.half, 0.95 * self.half, Y)

        mountains = smoothstep(0.08, 0.95, ridged(n1, wx / 700.0, wy / 700.0, octaves=8))
        mountain_h = t["mountain_height_m"] * mountains * (0.25 + 0.75 * valley)
        mountain_h += t["backdrop_height_m"] * far * (0.45 + 0.55 * ridged(n2, wx / 520.0, wy / 520.0, octaves=7))

        hills = t["hill_height_m"] * (0.5 + 0.5 * fbm(n2, X / 260.0, Y / 260.0, 5))
        floor = 1.4 + 1.4 * fbm(n3, X / 70.0, Y / 70.0, 4) + 3.0 * fbm(n4, X / 420.0, Y / 420.0, 3)
        # hummocky micro-relief: tussocks and hollows the grass can sit in
        floor += 0.35 * fbm(n1, X / 7.0 + 11, Y / 7.0, 3) + 0.15 * np.abs(fbm(n2, X / 3.2, Y / 3.2, 2))

        rise = np.maximum(valley, far * 0.85)
        h = floor + hills * smoothstep(0.0, 0.6, rise) + mountain_h * rise

        # erosion-flavoured detail: high-frequency octaves damped on steep
        # slopes, which reads like gullies and scree fans
        gy, gx = np.gradient(h, self.spacing)
        slope = np.sqrt(gx * gx + gy * gy)
        h += 9.0 * fbm(n1, X / 38.0, Y / 38.0, 5) * rise / (1.0 + 2.5 * slope * slope)
        # sharper crests and gullies on the valley walls
        hm = t["mountain_height_m"]
        h += rise * (0.13 * hm * ridged(n4, wx / 230.0, wy / 230.0, octaves=6)
                     + 0.07 * hm * ridged(n3, wx / 75.0 + 3.3, wy / 75.0, octaves=4)
                     + 0.018 * hm * ridged(n2, wx / 26.0 - 1.7, wy / 26.0, octaves=3))

        h = self._thermal_erosion(h, int(t["thermal_passes"]), talus=1.15)
        h, path_w = self._castle_site(h)

        # carve the river channel last so it stays crisp; it fades out before
        # the backdrop so it never cuts a slot through the far massif
        rw = t["river_width_m"] * 0.5
        bank = smoothstep(rw, rw * 2.4, dist)
        bed = -t["river_depth_m"] * (1.0 - smoothstep(0.0, rw * 1.1, dist) ** 2)
        river_zone = Y < 0.55 * self.half
        carve = (dist < rw * 2.4) & river_zone
        h = np.where(carve, bed * (1 - bank) + np.maximum(h, 0.35) * bank, h)
        # keep everything else above water so the flat water plane only shows in the channel
        wl = self.cfg["biome"]["water_level_m"]
        outside = (dist > rw * 1.9) | ~river_zone
        h = np.where(outside & (h < wl + 0.25), wl + 0.25, h)

        self.height = h.astype(np.float64)
        self.marsh = None
        self.patch = None
        self.dist_river = np.where(river_zone, dist, 1e4)
        self.slope_deg, self.masks = compute_masks(self.cfg, self.height, X, Y, self.spacing)
        # road + courtyard: bare ground, no grass, no rock
        self.masks["path"] = path_w.astype(np.float32)
        for k in ("grass", "rock", "dry"):
            self.masks[k] = (self.masks[k] * (1 - path_w)).astype(np.float32)
        return self

    # ------------------------------------------------------------------ castle site
    def castle_center(self):
        c = self.cfg["castle"]
        return float(self.river_center(np.array(c["y"]))) + c["river_offset_m"], float(c["y"])

    def _castle_site(self, h):
        """Raise a cliff-sided crag with a flat plateau for the castle and cut
        a switchback road from the gate down to the valley floor."""
        c = self.cfg.get("castle", {})
        path_w = np.zeros_like(h)
        self.castle = None
        if not c.get("enabled"):
            return h, path_w
        X, Y = self.X, self.Y
        cx, cy = self.castle_center()
        pr, cr, ch = c["plateau_radius_m"], c["crag_radius_m"], c["crag_height_m"]
        box = (np.abs(X - cx) < cr * 2.2) & (np.abs(Y - cy) < cr * 2.2)
        n5, n6 = Perlin2D(self.seed + 60), Perlin2D(self.seed + 61)
        r = np.hypot(X - cx, Y - cy)
        r_eff = r + 9.0 * fbm(n5, X / 38.0, Y / 38.0, 3)
        site_h = float(np.median(h[r < cr]))
        top = site_h + ch
        cliff = 1 - smoothstep(pr, pr + 0.32 * (cr - pr), r_eff)
        apron = 1 - smoothstep(pr, cr, r_eff)
        crag = site_h + ch * (0.72 * cliff ** 1.6 + 0.28 * apron ** 2)
        # cliff band: vertical jointing (buttresses and gullies running down
        # the face), stepped ledges, and broken ridged detail
        band = np.clip(cliff * (1 - cliff) * 4.0, 0, 1)
        ang = np.arctan2(Y - cy, X - cx)
        arc = ang * pr
        joints = ridged(n5, arc / 6.0, r / 30.0, 4)
        crag += band * (5.5 * joints - 2.5 + 3.0 * ridged(n6, X / 11.0, Y / 11.0, 4))
        steps = np.floor(crag / 5.5) * 5.5
        crag = crag + (steps - crag) * 0.35 * band
        crag = np.where(r_eff < pr, top + 0.25 * fbm(n6, X / 9.0, Y / 9.0, 3), crag)
        h = np.where(box, np.maximum(h, crag), h)

        # road: gate faces the opening camera position, spirals down on the side
        # away from the river, then runs out across the valley floor
        k0 = self.cfg["camera"]["path"][0]
        camx = float(self.river_center(np.array(k0["y"]))) + k0["x"]
        gate_a = np.arctan2(k0["y"] - cy, camx - cx)
        sgn = 1.0 if c["river_offset_m"] > 0 else -1.0
        n_pts = 60
        tt = np.linspace(0, 1, n_pts)
        ang = gate_a + sgn * np.radians(125.0) * tt
        rad = (pr + 7.0) + (cr * 0.92 - pr - 7.0) * tt
        px = cx + rad * np.cos(ang)
        py = cy + rad * np.sin(ang)
        pz = top - 1.0 - (ch - 1.0) * tt ** 0.85
        # run-out across the floor toward the camera side
        dx, dy = camx - px[-1], k0["y"] - py[-1]
        L = np.hypot(dx, dy)
        out = np.linspace(0, 1, 15)[1:] * min(L * 0.6, 200.0)
        rx = px[-1] + dx / L * out
        ry = py[-1] + dy / L * out
        rz = self.sample(h, rx, ry)
        road = np.stack([np.concatenate([[cx + (pr - 2) * np.cos(gate_a)], px, rx]),
                         np.concatenate([[cy + (pr - 2) * np.sin(gate_a)], py, ry]),
                         np.concatenate([[top], pz, rz])], 1)

        # carve/fill the hillside along the road
        width = c["road_width_m"]
        sub = np.where(box)
        bx, by = X[sub], Y[sub]
        best_d = np.full(bx.shape, np.inf)
        best_z = np.zeros_like(bx)
        for i in range(len(road) - 1):
            a0, a1 = road[i], road[i + 1]
            ab = a1[:2] - a0[:2]
            tseg = np.clip(((bx - a0[0]) * ab[0] + (by - a0[1]) * ab[1]) / max(ab @ ab, 1e-9), 0, 1)
            d = np.hypot(bx - (a0[0] + tseg * ab[0]), by - (a0[1] + tseg * ab[1]))
            closer = d < best_d
            best_d = np.where(closer, d, best_d)
            best_z = np.where(closer, a0[2] + tseg * (a1[2] - a0[2]), best_z)
        w = 1 - smoothstep(width * 0.5, width * 0.5 + 3.5, best_d)
        hb = h[sub]
        h[sub] = hb * (1 - w) + best_z * w
        pw = 1 - smoothstep(width * 0.35, width * 0.6, best_d)
        path_w[sub] = pw
        path_w = np.maximum(path_w, (r_eff < pr - 1.0) * 0.85)  # courtyard: packed earth
        self.castle = {"center": (cx, cy), "top": top, "site": site_h, "plateau_r": pr,
                       "crag_r": cr, "gate_angle": float(gate_a), "road": road}
        return h, path_w

    def dist_to_castle(self, x, y):
        if not self.castle:
            return np.full(np.shape(x), 1e9)
        cx, cy = self.castle["center"]
        return np.hypot(np.asarray(x) - cx, np.asarray(y) - cy)

    # ------------------------------------------------------------------ meadow
    def _build_meadow(self):
        """Rolling grassland ringed by forested hills, a shallow swale that
        the lane follows, a small pond, and a two-track lane: bare wheel
        ruts with a grass strip between them."""
        t, m = self.cfg["terrain"], self.cfg["meadow"]
        X, Y = self.X, self.Y
        n1, n2, n3, n4 = (Perlin2D(self.seed + i) for i in range(4))
        core = m["radius_m"]
        r = np.hypot(X, Y / 1.15) + 70.0 * fbm(n3, X / 420.0, Y / 420.0, 3)
        rise = smoothstep(core * 0.85, core * 1.6, r)
        rolling = 5.0 * fbm(n1, X / 260.0, Y / 260.0, 4) + 2.2 * fbm(n2, X / 95.0, Y / 95.0, 4)
        micro = 0.35 * fbm(n1, X / 7.0 + 11, Y / 7.0, 3) + 0.15 * np.abs(fbm(n2, X / 3.2, Y / 3.2, 2))
        rim = m["rim_height_m"] * rise * (0.55 + 0.45 * ridged(n4, X / 600.0, Y / 600.0, 6))
        h = 5.0 + rolling * (1 - 0.5 * rise) + micro + rim
        dsigned = X - self.river_center(Y)
        dpath = np.abs(dsigned)
        h -= 1.4 * (1 - smoothstep(0.0, 70.0, dpath)) * (1 - rise)          # the lane follows low ground
        h = self._thermal_erosion(h, 12, talus=1.0)

        # two-track lane across the whole meadow. The ruts are far finer than
        # the mesh, so the mesh only carries a smooth lane band plus the exact
        # signed distance to the centreline (lane_d); the shader draws the
        # ruts per pixel from it and the grass scatter uses it analytically.
        along = smoothstep(core * 1.45, core * 1.25, np.abs(Y))
        band = (1 - smoothstep(1.2, 1.9, dpath)) * along
        h -= 0.1 * band

        wl = self.cfg["biome"]["water_level_m"]
        h_pre = h.copy()
        mc = m.get("marsh", {})
        self.marsh = None
        if mc.get("enabled", True):
            self.marsh = dict(mc, center=tuple(mc["center"]), water_level=wl)
            h, sd = self._marsh_height(X, Y, h, fine=False)
            far = sd > mc["margin_m"] * 1.3
            h = np.where(far, np.maximum(h, wl + 0.35), h)
            self.dist_river = np.abs(sd)                   # "water's edge" is the marsh shore
        else:
            h = np.maximum(h, wl + 0.35)
            self.dist_river = np.full_like(h, 1e4)
        self.height = h.astype(np.float64)
        self.rise = rise
        self.castle = None
        self.slope_deg, self.masks = compute_masks(self.cfg, self.height, X, Y, self.spacing)
        self.masks["path"] = band.astype(np.float32)
        self.masks["lane_d"] = np.clip(dsigned, -40.0, 40.0).astype(np.float32)
        self.masks["lane_rut"] = along.astype(np.float32)
        self.lane_along = along
        for k in ("rock", "dry"):
            self.masks[k] = (self.masks[k] * (1 - band)).astype(np.float32)
        self.patch = None
        if self.marsh:
            self._build_marsh_patch(h_pre)
        return self

    # ------------------------------------------------------------------ marsh
    def marsh_sd(self, x, y):
        """Signed distance (m) to the marsh pond shore; negative in water.
        A smooth union of lobes, warped by noise, gives an organic outline."""
        M = self.marsh
        cx, cy = M["center"]
        r = M["radius_m"]
        x = np.asarray(x, dtype=np.float64)
        y = np.asarray(y, dtype=np.float64)
        d = None
        for ox, oy, rr in ((0.0, 0.0, 1.0), (0.72, 0.42, 0.62), (-0.55, -0.58, 0.55), (-0.2, 0.78, 0.45)):
            di = np.hypot(x - (cx + ox * r), y - (cy + oy * r)) - rr * r
            d = di if d is None else -4.0 * np.logaddexp(-d / 4.0, -di / 4.0)   # smooth union
        na, nb = Perlin2D(self.seed + 70), Perlin2D(self.seed + 71)
        return d + 2.4 * fbm(na, x / 14.0, y / 14.0, 3) + 0.6 * fbm(nb, x / 4.0, y / 4.0, 2)

    def _marsh_height(self, X, Y, h_base, fine=False):
        """Pond bed (shallow reed shelf, deeper centre) and a low, wet margin
        rising back to the meadow. fine adds the micro-relief: tussocks,
        puddles, silt and mud texture."""
        M = self.marsh
        wl = M["water_level"]
        sd = self.marsh_sd(X, Y)
        inside = -sd
        depth = 0.28 * smoothstep(0.0, 2.5, inside) + 1.15 * smoothstep(5.0, 14.0, inside)
        bed = wl - 0.05 - depth
        mw = M["margin_m"]
        t = smoothstep(0.0, mw, sd)
        shore = wl + 0.06 + np.maximum(h_base - wl - 0.06, 0.0) * t ** 1.6
        h = np.where(sd < 0, bed, np.minimum(h_base, shore))
        if fine:
            n5, n6, n7, n8 = (Perlin2D(self.seed + i) for i in (80, 81, 82, 83))
            zone = 1 - smoothstep(mw * 0.7, mw * 1.15, sd)
            tz = smoothstep(-4.0, -1.0, sd) * (1 - smoothstep(6.0, 11.0, sd))        # shallows + inner margin
            tussock = 0.34 * smoothstep(0.58, 0.8, 0.5 + 0.5 * fbm(n5, X / 1.6, Y / 1.6, 3))
            pz = smoothstep(1.0, 3.0, sd) * (1 - smoothstep(9.0, 13.0, sd))           # puddles on the margin
            puddle = -0.24 * smoothstep(0.6, 0.78, 0.5 + 0.5 * fbm(n6, X / 5.0, Y / 5.0, 3))
            silt = 0.1 * fbm(n8, X / 3.0, Y / 3.0, 3) * (sd < 0)
            mud = 0.035 * fbm(n7, X / 0.45, Y / 0.45, 3)
            h = h + zone * (tz * tussock + pz * puddle + silt + mud)
        return h, sd

    def _sample_grid(self, arr, x0, y0, sp, x, y):
        ny, nx = arr.shape
        fx = np.clip((np.asarray(x) - x0) / sp, 0, nx - 1.001)
        fy = np.clip((np.asarray(y) - y0) / sp, 0, ny - 1.001)
        i0 = np.floor(fx).astype(int)
        j0 = np.floor(fy).astype(int)
        tx, ty = fx - i0, fy - j0
        a = arr[j0, i0] * (1 - tx) + arr[j0, i0 + 1] * tx
        b = arr[j0 + 1, i0] * (1 - tx) + arr[j0 + 1, i0 + 1] * tx
        return a * (1 - ty) + b * ty

    def _build_marsh_patch(self, h_pre):
        """High-resolution terrain patch over the marsh. Its rectangle is
        cut out of the main terrain on even grid lines; along those lines
        the main heights are made linear between even samples, so the patch
        edge matches both the full-resolution terrain and the half-resolution
        export exactly (no cracks)."""
        M = self.marsh
        cx, cy = M["center"]
        half = M["patch_half_m"]
        sp, H = self.spacing, self.height

        def even_lo(v):
            i = int(np.floor((v + self.half) / sp))
            return max(0, i - i % 2)

        def even_hi(v):
            i = int(np.ceil((v + self.half) / sp))
            return min(self.res - 1, i + i % 2)

        c0, c1, r0, r1 = even_lo(cx - half), even_hi(cx + half), even_lo(cy - half), even_hi(cy + half)
        for r in (r0, r1):
            for c in range(c0 + 1, c1, 2):
                H[r, c] = 0.5 * (H[r, c - 1] + H[r, c + 1])
        for c in (c0, c1):
            for r in range(r0 + 1, r1, 2):
                H[r, c] = 0.5 * (H[r - 1, c] + H[r + 1, c])
        sub = int(M["patch_sub"])
        nx, ny = (c1 - c0) * sub + 1, (r1 - r0) * sub + 1
        xs = np.linspace(self.X[0, c0], self.X[0, c1], nx)
        ys = np.linspace(self.Y[r0, 0], self.Y[r1, 0], ny)
        PX, PY = np.meshgrid(xs, ys)
        base = self.sample(h_pre, PX, PY)
        hp, _ = self._marsh_height(PX, PY, base, fine=True)
        edge = self.sample(H, PX, PY)
        db = np.minimum.reduce([PX - xs[0], xs[-1] - PX, PY - ys[0], ys[-1] - PY])
        w = smoothstep(0.0, 6.0, db)
        hp = edge * (1 - w) + hp * w
        psp = sp / sub
        _, masks = compute_masks(self.cfg, hp, PX, PY, psp)
        for k in ("path", "lane_d", "lane_rut"):
            masks[k] = self.sample(self.masks[k], PX, PY).astype(np.float32)
        self.patch = {"r0": r0, "r1": r1, "c0": c0, "c1": c1, "x0": xs[0], "x1": xs[-1], "y0": ys[0], "y1": ys[-1],
                      "sp": psp, "X": PX, "Y": PY, "H": hp, "masks": masks}

    def patch_mesh(self, bpy, name="Terrain_Detail"):
        p = self.patch
        return grid_mesh(bpy, name, p["X"], p["Y"], p["H"], p["masks"], self.size)

    def pond_water_mesh(self, bpy, name="PondWater", step=2):
        """Water surface over the marsh (pond and puddles) at the water
        level, with per-vertex 'depth' (m) for the shader and a Depth
        vertex colour (R = depth / 2) for Unreal."""
        p = self.patch
        wl = self.marsh["water_level"]
        s_ = slice(None, None, step)
        X, Y, D = p["X"][s_, s_], p["Y"][s_, s_], wl - p["H"][s_, s_]
        r_y, r_x = D.shape
        idx = np.arange(r_y * r_x).reshape(r_y, r_x)
        faces = np.stack([idx[:-1, :-1].ravel(), idx[:-1, 1:].ravel(),
                          idx[1:, 1:].ravel(), idx[1:, :-1].ravel()], axis=1)
        dmax = np.maximum.reduce([D[:-1, :-1], D[:-1, 1:], D[1:, 1:], D[1:, :-1]]).ravel()
        faces = faces[dmax > -0.03]
        used = np.unique(faces)
        remap = np.full(r_y * r_x, -1, dtype=np.int64)
        remap[used] = np.arange(len(used))
        faces = remap[faces].astype(np.int32)
        verts = np.stack([X.ravel()[used], Y.ravel()[used], np.full(len(used), wl)], 1).astype(np.float32)
        me = bpy.data.meshes.new(name)
        me.vertices.add(len(verts))
        me.vertices.foreach_set("co", verts.ravel())
        me.loops.add(faces.size)
        me.loops.foreach_set("vertex_index", faces.ravel())
        me.polygons.add(len(faces))
        me.polygons.foreach_set("loop_start", np.arange(0, faces.size, 4, dtype=np.int32))
        me.update(calc_edges=True)
        depth = D.ravel()[used].astype(np.float32)
        me.attributes.new("depth", "FLOAT", "POINT").data.foreach_set("value", depth)
        col = me.color_attributes.new("Depth", "FLOAT_COLOR", "POINT")
        dc = np.clip(depth / 2.0, 0, 1)
        col.data.foreach_set("color", np.stack([dc, dc, dc, np.ones_like(dc)], 1).astype(np.float32).ravel())
        uv = me.uv_layers.new(name="UVMap")
        lv = faces.ravel()
        uv.data.foreach_set("uv", np.stack([verts[lv, 0] / self.size + 0.5, verts[lv, 1] / self.size + 0.5], 1)
                            .astype(np.float32).ravel())
        return me

    def _thermal_erosion(self, h, passes, talus=0.9, rate=0.22):
        """Thermal erosion: material slides downhill where the slope exceeds
        the talus angle. Softens raw noise into believable scree slopes."""
        lim = talus * self.spacing
        ny, nx = h.shape
        for _ in range(passes):
            pad = np.pad(h, 1, mode="edge")
            moved = np.zeros_like(h)
            for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
                nb = pad[1 + dy:1 + dy + ny, 1 + dx:1 + dx + nx]
                diff = h - nb
                flow = np.where(diff > lim, (diff - lim) * rate * 0.25, 0.0)
                moved -= flow
                dep = np.zeros_like(pad)
                dep[1 + dy:1 + dy + ny, 1 + dx:1 + dx + nx] = flow
                moved += dep[1:-1, 1:-1]
            h = h + moved
        return h

    # ------------------------------------------------------------------ skirt
    def build_skirt(self, size=16000.0, res=321):
        """Distant mountain ranges around the main terrain."""
        t = self.cfg["terrain"]
        lin = np.linspace(-size / 2, size / 2, res)
        X, Y = np.meshgrid(lin, lin)
        n1, n2 = Perlin2D(self.seed + 90), Perlin2D(self.seed + 91)
        wx = X + 600 * fbm(n2, X / 4000, Y / 4000, 3)
        wy = Y + 600 * fbm(n2, X / 4000 + 3, Y / 4000 - 5, 3)
        ranges = smoothstep(0.1, 0.95, ridged(n1, wx / 2600.0, wy / 2600.0, octaves=7))
        ranges = ranges * (0.55 + 0.45 * fbm(n2, X / 5000, Y / 5000, 2) + 0.2)
        out_d = np.maximum(np.abs(X), np.abs(Y)) - self.half          # >0 outside the main square
        peak = t.get("skirt_height_m", 1500.0) * ranges * smoothstep(0, 2500, out_d) ** 0.7
        # continuity at the border: start from the main terrain's edge height
        edge_h = self.height_at(np.clip(X, -self.half, self.half), np.clip(Y, -self.half, self.half))
        base = edge_h * (1 - smoothstep(0, 900, out_d)) + (t["mountain_height_m"] * 0.35) * smoothstep(0, 900, out_d)
        # behind the camera (south) the ranges are never on screen but would
        # block a low sun: flatten them into low plains
        south = smoothstep(-self.half + 100, -self.half - 700, Y)
        base = base * (1 - south) + np.minimum(edge_h, 25.0) * south
        H = base + peak * (1 - south)
        H = np.where(out_d < -self.spacing * 2, edge_h - 6.0, H)  # hidden under the main terrain
        spacing = size / (res - 1)
        _, masks = compute_masks(self.cfg, H, X, Y, spacing)
        masks["path"] = np.zeros_like(masks["wet"])
        return X, Y, H, masks, size

    # ------------------------------------------------------------------ sampling
    def normals(self):
        gy, gx = np.gradient(self.height, self.spacing)
        n = np.dstack([-gx, -gy, np.ones_like(gx)])
        return n / np.linalg.norm(n, axis=2, keepdims=True)

    def sample(self, arr, x, y):
        """Bilinear sample of a grid array at world x, y."""
        fx = np.clip((np.asarray(x) + self.half) / self.spacing, 0, self.res - 1.001)
        fy = np.clip((np.asarray(y) + self.half) / self.spacing, 0, self.res - 1.001)
        x0 = np.floor(fx).astype(int)
        y0 = np.floor(fy).astype(int)
        tx = fx - x0
        ty = fy - y0
        a = arr[y0, x0] * (1 - tx) + arr[y0, x0 + 1] * tx
        b = arr[y0 + 1, x0] * (1 - tx) + arr[y0 + 1, x0 + 1] * tx
        return a * (1 - ty) + b * ty

    def height_at(self, x, y):
        h = self.sample(self.height, x, y)
        p = getattr(self, "patch", None)
        if p is not None:
            x = np.asarray(x, dtype=np.float64)
            y = np.asarray(y, dtype=np.float64)
            inside = (x >= p["x0"]) & (x <= p["x1"]) & (y >= p["y0"]) & (y <= p["y1"])
            if np.any(inside):
                h = np.where(inside, self._sample_grid(p["H"], p["x0"], p["y0"], p["sp"], x, y), h)
        return h

    # ------------------------------------------------------------------ mesh
    def to_mesh(self, bpy, name="Terrain", step=1):
        s = slice(None, None, step)
        masks = {k: m[s, s] for k, m in self.masks.items()}
        hole = None
        p = getattr(self, "patch", None)
        if p is not None:
            hole = (p["r0"] // step, p["r1"] // step, p["c0"] // step, p["c1"] // step)
        return grid_mesh(bpy, name, self.X[s, s], self.Y[s, s], self.height[s, s], masks, self.size, hole)

    def skirt_mesh(self, bpy, name="Terrain_Far"):
        X, Y, H, masks, size = self.build_skirt()
        return grid_mesh(bpy, name, X, Y, H, masks, size)
