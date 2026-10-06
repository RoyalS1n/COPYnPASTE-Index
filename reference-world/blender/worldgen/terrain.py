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


def grid_mesh(bpy, name, X, Y, H, masks, uv_size):
    """Build a quad-grid mesh with mask attributes, a packed 'Masks' colour
    attribute (R grass, G rock, B snow, A wet) and planar UVs."""
    r_y, r_x = H.shape
    verts = np.dstack([X, Y, H]).reshape(-1, 3).astype(np.float32)
    idx = np.arange(r_y * r_x).reshape(r_y, r_x)
    faces = np.stack([idx[:-1, :-1].ravel(), idx[:-1, 1:].ravel(),
                      idx[1:, 1:].ravel(), idx[1:, :-1].ravel()], axis=1).astype(np.int32)
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
    packed = np.stack([masks["grass"].ravel(), masks["rock"].ravel(),
                       masks["snow"].ravel(), masks["wet"].ravel()], axis=1)
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
    def river_center(self, y):
        t = self.cfg["terrain"]
        lam = t["meander_wavelength_m"]
        amp = t["meander_amp_m"]
        rng = np.random.default_rng(self.seed + 11)
        ph1, ph2 = rng.uniform(0, 2 * np.pi, 2)
        return (amp * np.sin(2 * np.pi * y / lam + ph1)
                + 0.32 * amp * np.sin(2 * np.pi * y / (0.41 * lam) + ph2))

    def build(self):
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
        self.dist_river = np.where(river_zone, dist, 1e4)
        self.slope_deg, self.masks = compute_masks(self.cfg, self.height, X, Y, self.spacing)
        return self

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
        return self.sample(self.height, x, y)

    # ------------------------------------------------------------------ mesh
    def to_mesh(self, bpy, name="Terrain", step=1):
        s = slice(None, None, step)
        masks = {k: m[s, s] for k, m in self.masks.items()}
        return grid_mesh(bpy, name, self.X[s, s], self.Y[s, s], self.height[s, s], masks, self.size)

    def skirt_mesh(self, bpy, name="Terrain_Far"):
        X, Y, H, masks, size = self.build_skirt()
        return grid_mesh(bpy, name, X, Y, H, masks, size)
