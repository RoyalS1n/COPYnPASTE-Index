"""Generate DAYFALL's procedural texture library: 100 seamless (tileable) albedo textures, 1024 x 1024 JPEGs.

    python tools/make_texture_library.py [--out DIR] [--size N] [--only NAME[,NAME...]] [--sheet PATH]
                                         [--tiling PATH] [--materials FILE.json] [--jobs N] [--quality Q]

(a Python with numpy and the `bpy` module, run with -I; bpy only writes and reads the JPEGs). Each texture is
content/textures/<category>_<name>.jpg, top of the image = up. The images are original work from the code below,
with fixed seeds, so every run gives the same files (about three minutes on four cores). Every pattern is built
from functions that are periodic over the tile (lattice noise with wrapped lattice coordinates, cellular noise
with wrapped cells, brick / plank / tile layouts whose counts divide the tile), so each texture tiles without
seams. Each run checks that twice per texture: the difference across the wrap seam against that between
neighbouring columns and rows (before JPEG, and on the luma of the decoded JPEG against the interior 8 x 8 block
edges; decoders rebuild 4:2:0 chroma at the image border from clamped samples, a one-pixel codec effect), and a
small render shifted by half a tile, which must equal the first one rolled by half its size.

Colours are worked out in linear light with plausible albedo (no pure black or white; most surfaces have a mean
luminance between 0.04 and 0.5) and encoded to sRGB in the files. In the engine, give a texture to a lit
material as {"textures": {"base": ...}, "triplanar": {"tile_m": <metres one tile covers>}} with base_color 0.4
(see shadeTriplanar in shaders/include/surface.glsl); --materials writes one such definition per texture.

  --out DIR          where the JPEGs go (default: content/textures next to this tools folder)
  --size N           pixels per side (default 1024)
  --only A,B         just these textures (names with or without the category, e.g. brick_flemish or flemish)
  --sheet PATH       also write a contact sheet (one row per category, names underneath)
  --tiling PATH      also write a tiling check: six textures repeated 2 x 2
  --materials PATH   also write the material definitions (JSON) for content/textures/library.json
  --jobs N           worker processes (default: CPU count, up to 8)
  --quality Q        JPEG quality; files over 400 KB are saved again at lower quality, down to 78 (default 88)

Building blocks: periodic gradient noise and fBm (noise / fbm / ridged), domain warping (warp), periodic
cellular noise with true edge distances (worley), unit layouts for masonry, planks and tiles (courses, herringbone,
grid_units, basket), a scatter of shapes for leaves and pebbles (scatter), periodic blur and cavity, colour ramps
and palettes. The 100 recipes follow, one function each, grouped by category.
"""
import argparse
import functools
import json
import math
import os
import sys
import time
import zlib
from types import SimpleNamespace

for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")     # parallelism comes from worker processes
import numpy as np

F32 = np.float32
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, "..", "content", "textures"))
LUMA = np.array([0.2126, 0.7152, 0.0722], F32)       # Rec.709 luminance of linear RGB


# ---------------------------------------------------------------- small maths
def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def fade(t):
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


def nrm(a, lo=0.0, hi=1.0):
    """Rescale an array to lo..hi (min to max)."""
    a0, a1 = float(a.min()), float(a.max())
    return (lo + (a - a0) * ((hi - lo) / max(a1 - a0, 1e-9))).astype(F32)


def hash01(ids, seed):
    """A float in [0, 1) for every integer id (a 32-bit integer hash), stable across runs."""
    h = np.asarray(ids).astype(np.uint32) ^ np.uint32(seed & 0xFFFFFFFF)
    h = h * np.uint32(0x9E3779B1)
    h ^= h >> np.uint32(16)
    h *= np.uint32(0x7FEB352D)
    h ^= h >> np.uint32(15)
    h *= np.uint32(0x846CA68B)
    h ^= h >> np.uint32(16)
    return (h >> np.uint32(8)).astype(F32) * F32(1.0 / (1 << 24))


def blur(a, r):
    """Gaussian blur with sigma r (tile units), wrapping around the tile (FFT, so periodic by construction)."""
    n = a.shape[0]
    f = np.fft.fftfreq(n).astype(F32)
    s = r * n
    gy = np.exp(-2.0 * (np.pi * s * f) ** 2)
    gx = np.exp(-2.0 * (np.pi * s * np.fft.rfftfreq(n)) ** 2)
    k = gy[:, None] * gx[None, :]
    if a.ndim == 3:
        return np.stack([np.fft.irfft2(np.fft.rfft2(a[..., i]) * k, s=a.shape[:2]) for i in range(a.shape[2])],
                        -1).astype(F32)
    return np.fft.irfft2(np.fft.rfft2(a) * k, s=a.shape).astype(F32)


def cavity(h, r):
    """How far each point sits below its surroundings (h minus its blur), >= 0; for baked crevice darkening."""
    return np.maximum(blur(h, r) - h, 0.0)


# ---------------------------------------------------------------- colour
def C(hexstr):
    """An sRGB hex colour ('#8a7f72') as linear RGB."""
    h = hexstr.lstrip("#")
    s = np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], F32)
    return srgb_to_linear(s)


def srgb_to_linear(s):
    s = np.asarray(s, F32)
    return np.where(s <= 0.04045, s / 12.92, ((s + 0.055) / 1.055) ** 2.4).astype(F32)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055).astype(F32)


def col(c):
    return C(c) if isinstance(c, str) else np.asarray(c, F32)


def ramp(t, *stops):
    """Colour ramp: stops are colours evenly spaced over 0..1, or (position, colour) pairs. Interpolated in
    sRGB-like space (perceptually even), returned linear."""
    if not isinstance(stops[0], tuple):
        stops = [(i / (len(stops) - 1), s) for i, s in enumerate(stops)]
    pos = np.array([p for p, _ in stops], F32)
    cs = np.array([linear_to_srgb(col(c)) for _, c in stops], F32)
    t = np.asarray(t, F32)
    out = np.stack([np.interp(t, pos, cs[:, i]) for i in range(3)], -1)
    return srgb_to_linear(out)


def pick(r, *cols):
    """One of the colours for each value of r in [0, 1) (a per-unit random number)."""
    cs = np.array([col(c) for c in cols], F32)
    return cs[np.minimum((np.asarray(r) * len(cs)).astype(np.int32), len(cs) - 1)]


def mix(a, b, t):
    """Blend colours (or colour images) a -> b by t (an array of weights or a scalar)."""
    t = np.clip(np.asarray(t, F32), 0.0, 1.0)
    if t.ndim:
        t = t[..., None]
    return (a + (b - a) * t).astype(F32)


def mul(img, k):
    """Multiply a colour image by a scalar field."""
    return (img * np.asarray(k, F32)[..., None]).astype(F32)


def luminance(img):
    return img @ LUMA


def hue_jitter(img, r, amount):
    """Shift each unit's colour slightly towards warm or cool (r in 0..1 per pixel, usually per unit)."""
    k = (np.asarray(r, F32) - 0.5) * 2.0 * amount
    return (img * np.stack([1.0 + k, np.ones_like(k), 1.0 - k], -1)).astype(F32)


# ---------------------------------------------------------------- the tile
class Tile:
    """The texture being built: pixel-centre coordinates x (right) and y (down), both 0..1 over the tile, plus a
    stream of sub-seeds derived from the texture name (so every recipe is deterministic and independent of the
    others). All noise is periodic over the tile; frequencies are whole numbers of cells per tile."""

    def __init__(self, n, name, shift=0):
        self.n = n
        self.px = 1.0 / n
        self.shift = shift          # whole pixels; the periodicity check renders the tile shifted half a tile
        c = (np.arange(n, dtype=np.float64) + 0.5 + shift) / n
        self.x, self.y = np.meshgrid(c.astype(F32), c.astype(F32))
        self._base = zlib.crc32(name.encode())
        self._k = 0

    def seed(self):
        self._k += 1
        return (self._base * 2654435761 + self._k * 40503 + 12345) % (1 << 32)

    def _xy(self, x, y):
        return (self.x if x is None else x), (self.y if y is None else y)

    # -- gradient noise and fBm ------------------------------------------------
    def noise(self, f, fy=None, x=None, y=None, seed=None):
        """Periodic gradient noise, about zero mean and unit deviation, f x fy lattice cells over the tile."""
        return gnoise(self, x, y, int(f), int(fy or f), self.seed() if seed is None else seed)

    def fbm(self, f, octaves=5, gain=0.5, fy=None, x=None, y=None, lac=2):
        """Fractal sum of noise octaves (frequencies f, 2f, 4f...), normalised to about unit deviation."""
        fy = fy or f
        tot, amp, norm = 0.0, 1.0, 0.0
        for o in range(octaves):
            tot = tot + amp * self.noise(f * lac ** o, fy * lac ** o, x, y)
            norm += amp * amp
            amp *= gain
        return (tot / math.sqrt(norm)).astype(F32)

    def ridged(self, f, octaves=5, gain=0.5, fy=None, x=None, y=None):
        """Ridged fBm in about 0..1: sharp crests where the noise crosses zero (veins, cracks, creases)."""
        fy = fy or f
        tot, amp, norm = 0.0, 1.0, 0.0
        for o in range(octaves):
            r = 1.0 - np.minimum(np.abs(self.noise(f * 2 ** o, fy * 2 ** o, x, y)) * 0.8, 1.0)
            tot = tot + amp * r * r
            norm += amp
            amp *= gain
        return (tot / norm).astype(F32)

    def warp(self, f, amount, octaves=3, x=None, y=None, fy=None):
        """Domain warp: coordinates displaced by fBm (amount in tile units). Periodic, since the offset is."""
        x, y = self._xy(x, y)
        return (x + amount * self.fbm(f, octaves, fy=fy, x=x, y=y),
                y + amount * self.fbm(f, octaves, fy=fy, x=x, y=y))

    # -- cellular ----------------------------------------------------------------
    def worley(self, fx, fy=None, jitter=0.85, stagger=0.0, x=None, y=None, edges=True, rounded=0.0, aspect=1.0):
        return worley(self, fx, fy or fx, jitter, stagger, x, y, edges, self.seed(), rounded, aspect)

    def rand(self, ids):
        """A random number in [0, 1) per unit id (fresh for each call)."""
        return hash01(ids, self.seed())


def gnoise(t, x, y, f, fy, seed):
    rng = np.random.default_rng(seed)
    a = rng.random(f * fy, dtype=F32) * F32(2.0 * np.pi)
    gx, gy = np.cos(a) / F32(0.2155), np.sin(a) / F32(0.2155)     # unit deviation
    if x is None and y is None and f <= 256 and fy <= 256:
        # on the pixel grid the noise is separable: two small matrix products instead of gathers
        ax, bx = _axis_weights(t.n, f)
        ay, by = _axis_weights(t.n, fy)
        Gx, Gy = gx.reshape(fy, f), gy.reshape(fy, f)
        out = ((by @ Gx) @ ax.T + (ay @ Gy) @ bx.T).astype(F32)
        return np.roll(out, (-t.shift, -t.shift), (0, 1)) if t.shift else out
    x, y = t._xy(x, y)
    X, Y = x * F32(f), y * F32(fy)
    x0, y0 = np.floor(X), np.floor(Y)
    tx, ty = (X - x0).astype(F32), (Y - y0).astype(F32)
    i0 = x0.astype(np.int64) % f
    i1 = (i0 + 1) % f
    j0 = (y0.astype(np.int64) % fy) * f
    j1 = ((y0.astype(np.int64) + 1) % fy) * f
    k = j0 + i0
    n00 = gx[k] * tx + gy[k] * ty
    k = j0 + i1
    n10 = gx[k] * (tx - 1) + gy[k] * ty
    k = j1 + i0
    n01 = gx[k] * tx + gy[k] * (ty - 1)
    k = j1 + i1
    n11 = gx[k] * (tx - 1) + gy[k] * (ty - 1)
    sx, sy = fade(tx), fade(ty)
    a = n00 + sx * (n10 - n00)
    b = n01 + sx * (n11 - n01)
    return (a + sy * (b - a)).astype(F32)


@functools.lru_cache(maxsize=64)
def _axis_weights(n, f):
    """Interpolation weights from f lattice points to n pixel centres along one axis, wrapping: B blends values,
    A blends gradient terms (offset times weight)."""
    t = (np.arange(n) + 0.5) / n * f
    i0 = np.floor(t).astype(np.int64)
    tx = t - i0
    s = fade(tx)
    rows = np.arange(n)
    A = np.zeros((n, f))
    B = np.zeros((n, f))
    np.add.at(B, (rows, i0 % f), 1 - s)
    np.add.at(B, (rows, (i0 + 1) % f), s)
    np.add.at(A, (rows, i0 % f), tx * (1 - s))
    np.add.at(A, (rows, (i0 + 1) % f), (tx - 1) * s)
    return A.astype(F32), B.astype(F32)


def worley(t, fx, fy, jitter, stagger, x, y, edges, seed, rounded=0.0, aspect=1.0):
    """Periodic cellular (Voronoi) noise: one feature point per cell of an fx x fy grid, jittered, odd rows
    shifted by `stagger` cells (0.5 with no jitter gives hexagons). Distances are in tile units. Returns
    f1 / f2 (nearest and second distances), edge (distance to the cell's border), id (cell index, wraps with the
    tile), dx / dy (from the feature point to the pixel). `rounded` rounds the cells' corners (tile units).
    aspect < 1 measures vertical distances shrunk by that factor, so the cells come out 1/aspect times taller
    than wide (use fy = fx * aspect); edge is still a true distance in tile units."""
    x, y = t._xy(x, y)
    rng = np.random.default_rng(seed)
    jx = ((rng.random(fx * fy) - 0.5) * jitter).astype(F32)
    jy = ((rng.random(fx * fy) - 0.5) * jitter).astype(F32)
    cw, ch = 1.0 / fx, 1.0 / fy * aspect               # cell size in the (vertically scaled) metric
    rx = max(1, math.ceil(ch / cw * 0.75 + 0.25))
    ry = max(1, math.ceil(cw / ch * 0.75 + 0.25))
    X, Y = x * F32(fx), y * F32(fy)
    iy0 = np.floor(Y).astype(np.int64)

    def candidates():
        for dj in range(-ry, ry + 1):
            j = iy0 + dj
            sh = (j % 2).astype(F32) * F32(stagger)
            ix0 = np.floor(X - sh).astype(np.int64)
            for di in range(-rx, rx + 1):
                i = ix0 + di
                k = (j % fy) * fx + (i % fx)
                vx = (i.astype(F32) + 0.5 + sh + jx[k] - X) * F32(cw)
                vy = (j.astype(F32) + 0.5 + jy[k] - Y) * F32(ch)
                yield k, vx, vy

    big = np.full(x.shape, 1e9, F32)
    d1, d2 = big.copy(), big.copy()
    bx, by = np.zeros_like(big), np.zeros_like(big)
    bid = np.zeros(x.shape, np.int64)
    for k, vx, vy in candidates():
        d = vx * vx + vy * vy
        closer = d < d1
        d2 = np.where(closer, d1, np.minimum(d2, d))
        d1 = np.where(closer, d, d1)
        bx = np.where(closer, vx, bx)
        by = np.where(closer, vy, by)
        bid = np.where(closer, k, bid)
    out = SimpleNamespace(f1=np.sqrt(d1), f2=np.sqrt(d2), id=bid, dx=-bx, dy=-by / F32(aspect))
    if edges:
        e = big.copy()           # distance to the nearest border; with `rounded`, a polynomial smooth minimum
        sk = 4.0 * rounded       # that rounds the cell's corners by about that radius
        for _, vx, vy in candidates():
            wx, wy = vx - bx, vy - by
            l2 = wx * wx + wy * wy
            ok = l2 > 1e-12
            norm = np.sqrt(np.where(ok, wx * wx + (aspect * aspect) * wy * wy, 1.0))   # tile-space distance
            dist = ((bx + vx) * 0.5 * wx + (by + vy) * 0.5 * wy) / norm
            dist = np.where(ok, dist, 1e9)
            if rounded:
                h = np.clip((sk - np.abs(e - dist)) / sk, 0.0, 1.0)
                e = np.minimum(e, dist) - h * h * (sk * 0.25)
            else:
                e = np.minimum(e, dist)
        out.edge = e.astype(F32)
    return out


# ---------------------------------------------------------------- unit layouts (bricks, stones, planks, tiles)
def courses(x, y, heights, widths, offsets):
    """Rows of units (courses of bricks or stones, rows of planks or shingles). heights: one per row, summing to
    1; widths: per row, unit widths summing to 1; offsets: each row's horizontal shift. Every row wraps at the tile
    edge, so the layout is periodic. Returns per pixel: id (unique per unit), row, k (index in its row), u / v
    (0..1 across / down the unit), w / h (unit size), edge (distance to the unit's border), all in tile units.
    The layout is shifted so that no joint runs along the tile's edge (where it would hide in the seam check)."""
    heights = np.asarray(heights, np.float64)
    yb = np.concatenate([[0.0], np.cumsum(heights)])
    yb[-1] = 1.0
    nrows = len(heights)
    def near_edge(pos, s):              # distance of each joint from the tile's edge after a shift by -s
        return np.abs(np.mod(pos - s + 0.5, 1.0) - 0.5)
    cand = np.mod(0.1237 + 0.0173 * np.arange(57), 1.0)
    sx = min(cand, key=lambda s: sum(h for h, w, o in zip(heights, widths, offsets)
                                     if near_edge(np.cumsum(np.asarray(w) / np.sum(w)) + o, s).min() < 0.012))
    sy = max(np.mod(0.0731 + 0.0119 * np.arange(83), 1.0), key=lambda s: near_edge(yb, s).min())
    x, y = x + sx, y + sy
    yy = np.mod(y, 1.0).astype(np.float64)
    row = np.clip(np.searchsorted(yb, yy, side="right") - 1, 0, nrows - 1)
    v = (yy - yb[row]) / heights[row]
    starts, ws, first = [], [], []
    for r in range(nrows):
        w = np.asarray(widths[r], np.float64)
        w = w / w.sum()
        first.append(len(starts))
        starts.extend(r + np.concatenate([[0.0], np.cumsum(w)[:-1]]))
        ws.extend(w)
    starts, ws, first = np.array(starts), np.array(ws), np.array(first)
    off = np.asarray(offsets, np.float64)
    xs = np.mod(x - off[row], 1.0)
    idx = np.clip(np.searchsorted(starts, row + xs, side="right") - 1, 0, len(starts) - 1)
    w = ws[idx]
    u = (xs - (starts[idx] - row)) / w
    h = heights[row]
    out = SimpleNamespace(id=idx, row=row, k=idx - first[row], u=u.astype(F32), v=v.astype(F32),
                          w=w.astype(F32), h=h.astype(F32))
    out.ex = (np.minimum(u, 1 - u) * w).astype(F32)
    out.ey = (np.minimum(v, 1 - v) * h).astype(F32)
    out.edge = np.minimum(out.ex, out.ey)
    return out


def random_widths(rng, n_rows, mean, spread=0.35, min_w=0.3):
    """Per-row unit widths (fractions of the tile) around `mean`, each row summing to 1."""
    rows = []
    for _ in range(n_rows):
        ws = []
        while sum(ws) < 1.0 - 0.6 * mean:
            ws.append(mean * max(min_w, 1.0 + spread * (rng.random() * 2 - 1)))
        rows.append(np.array(ws) / sum(ws))
    return rows


def random_heights(rng, n_rows, spread=0.3):
    h = 1.0 + spread * (rng.random(n_rows) * 2 - 1)
    return h / h.sum()


def bond(x, y, rows, cols, shift=0.5):
    """Running bond: rows x cols equal units, each row shifted by `shift` units from the one below."""
    return courses(x, y, [1.0 / rows] * rows, [[1.0 / cols] * cols] * rows,
                   [(r * shift % 1.0) / cols for r in range(rows)])


def grid_units(x, y, nx, ny=None):
    """A plain grid of tiles (stack bond)."""
    return bond(x, y, ny or nx, nx, 0.0)


def herringbone(x, y, n, ratio=2, diagonal=False):
    """Herringbone of 1 x ratio units: n short sides across the tile (n a multiple of 2*ratio); diagonal turns the
    pattern 45 degrees (then n is the scale, any whole number). Returns id, o (0 or 1: which way the unit runs),
    u (0..1 along it), v (0..1 across it) and edge (tile units)."""
    L = ratio
    if diagonal:
        c = L * n                               # keeps (1, 0) and (0, 1) on the pattern's lattice
        X, Y = c * (x + y), c * (y - x)
        unit = 1.0 / (c * math.sqrt(2.0))
        n_wrap = None
    else:
        X, Y = x * n + 0.5, y * n + 0.5        # half a unit off, so joints do not run along the tile's edge
        unit = 1.0 / n
        n_wrap = n
    r = np.floor(Y)
    s = np.mod(X - r, 2 * L)
    horiz = s < L
    cidx = np.floor(np.clip(s - L, 0, L - 1e-6))
    along = np.where(horiz, s / L, (Y - (r + cidx - L + 1)) / L)
    across = np.where(horiz, Y - r, s - L - cidx)
    ox = np.where(horiz, X - s, X - s + L + cidx)
    oy = np.where(horiz, r, r + cidx - L + 1)
    if n_wrap:
        ox, oy = np.mod(ox, n_wrap), np.mod(oy, n_wrap)
    else:                                       # undo the rotation to wrap the unit's origin in tile space
        tx = (ox - oy) / (2 * c)
        ty = (ox + oy) / (2 * c)
        ox = np.round(np.mod(tx, 1.0) * 4 * c) % (4 * c)
        oy = np.round(np.mod(ty, 1.0) * 4 * c) % (4 * c)
    oid = (ox.astype(np.int64) * 7919 + oy.astype(np.int64) * 104729 + horiz.astype(np.int64)) % (1 << 31)
    out = SimpleNamespace(id=oid, o=(~horiz).astype(np.int32), u=along.astype(F32), v=across.astype(F32))
    out.edge = (np.minimum(np.minimum(along, 1 - along) * L, np.minimum(across, 1 - across)) * unit).astype(F32)
    out.unit = unit
    return out


def basket(x, y, n, strips=3):
    """Basket weave: n x n squares, each split into `strips` strips, alternate squares turned 90 degrees."""
    X, Y = x * n, y * n
    i, j = np.floor(X), np.floor(Y)
    fx, fy = X - i, Y - j
    turn = ((i + j) % 2).astype(bool)
    across = np.where(turn, fx, fy) * strips
    s = np.floor(across)
    along = np.where(turn, fy, fx)
    v = across - s
    sid = ((np.mod(i, n) * n + np.mod(j, n)) * strips + s).astype(np.int64)
    out = SimpleNamespace(id=sid, o=turn.astype(np.int32), u=along.astype(F32), v=v.astype(F32))
    out.edge = (np.minimum(np.minimum(along, 1 - along), np.minimum(v, 1 - v) / strips) / n).astype(F32)
    return out


def joints(t, units, width, chip=0.0, chip_f=40, soft=1.0):
    """Coverage of the units (1) against the joints between them (0). chip > 0 bites occasional chips out of
    the edges (mostly straight, sometimes broken), scaled by the joint width. Returns (coverage, edge distance)."""
    e = units if isinstance(units, np.ndarray) else units.edge
    if chip:
        bite = np.maximum(t.fbm(chip_f, 2, gain=0.4) - 0.9, 0.0)       # smooth, so chips stay on the edge
        e = e - chip * width * (1.3 * bite + 0.12 * t.noise(chip_f * 6))
    return smoothstep(width - soft * t.px, width + soft * t.px, e).astype(F32), e


def bevel(e, width, depth):
    """Darkening towards a unit's edge (baked cavity), 1 in the middle, 1 - depth at the joint."""
    return (1.0 - depth * np.exp(-np.maximum(e, 0) / max(width, 1e-6))).astype(F32)


# ---------------------------------------------------------------- scattered shapes (leaves, pebbles, chips)
def scatter(t, f, layers, shape, x=None, y=None, size=1.0):
    """Draw one shape per cell of an f x f grid per layer, later layers on top. shape(lx, ly, r) gets the pixel's
    position in the shape's own frame (rotated, in cell units, centred) and per-shape random numbers r (a list of
    arrays), and returns (coverage 0..1, value). Shapes may reach about one cell beyond their own. Returns the
    total coverage, and the value, id and layer of the topmost shape per pixel."""
    x, y = t._xy(x, y)
    cov = np.zeros(x.shape, F32)
    val = np.zeros(x.shape, F32)
    sid = np.zeros(x.shape, np.int64)
    lay = np.zeros(x.shape, np.int32)
    X, Y = x * f, y * f
    for layer in range(layers):
        s0 = t.seed()
        Xl, Yl = X + (layer * 0.37) % 1.0, Y + (layer * 0.61) % 1.0      # each layer's grid is offset
        i0, j0 = np.floor(Xl).astype(np.int64), np.floor(Yl).astype(np.int64)
        lc = np.zeros(x.shape, F32)
        lv = np.zeros(x.shape, F32)
        lid = np.zeros(x.shape, np.int64)
        for dj in (-1, 0, 1):
            for di in (-1, 0, 1):
                i, j = i0 + di, j0 + dj
                k = (j % f) * f + (i % f) + layer * f * f
                r = [hash01(k, s0 + 17 * q) for q in range(6)]
                px = Xl - (i + 0.5 + (r[0] - 0.5) * 0.9)
                py = Yl - (j + 0.5 + (r[1] - 0.5) * 0.9)
                a = r[2] * F32(2 * np.pi)
                ca, sa = np.cos(a), np.sin(a)
                lx = (px * ca + py * sa) / size
                ly = (-px * sa + py * ca) / size
                c, v = shape(lx, ly, r[3:])
                better = c > lc
                lc = np.where(better, c, lc)
                lv = np.where(better, v, lv)
                lid = np.where(better, k, lid)
        cov = cov + lc * (1 - cov)
        val = val * (1 - lc) + lv * lc
        sid = np.where(lc > 0.5, lid, sid)
        lay = np.where(lc > 0.5, layer, lay)
    return cov, val, sid, lay


# ---------------------------------------------------------------- the recipe table
TEXTURES = {}       # name -> recipe; filled by @texture below, in order


def texture(tile_m, rough, metal=0.0):
    """Register a recipe. The function's name is the texture's name, <category>_<name>; tile_m is how many metres
    one tile covers in the world, rough / metal its material's roughness and metalness."""
    def deco(fn):
        TEXTURES[fn.__name__] = SimpleNamespace(name=fn.__name__, category=fn.__name__.split("_")[0], fn=fn,
                                                tile_m=tile_m, rough=rough, metal=metal)
        return fn
    return deco


# ==== RECIPES ====


# ---------------------------------------------------------------- shared material helpers
def fill(t, c):
    """A flat colour image."""
    return np.broadcast_to(col(c), (t.n, t.n, 3)).astype(F32)


def unit_noise(t, ids, f, octaves=4, gain=0.5, fy=None, x=None, y=None):
    """fBm with a random offset per unit, so each brick, stone or plank gets its own patch of the pattern."""
    x, y = t._xy(x, y)
    return t.fbm(f, octaves, gain, fy=fy, x=x + hash01(ids, t.seed()) * 7.0, y=y + hash01(ids, t.seed()) * 7.0)


def speckle(t, f, cover, soft=0.3, x=None, y=None):
    """Spots covering about `cover` of the surface (grit, mica, pits, flecks), 0..1."""
    n = t.noise(f, x=x, y=y)
    thr = float(np.quantile(n, 1.0 - cover))
    return smoothstep(thr - soft, thr + soft, n).astype(F32)


def grit(t, amount=0.06, f=None):
    """Fine grain: a multiplier around 1 from two high-frequency octaves."""
    f = f or t.n // 4
    return (1.0 + amount * (0.75 * t.noise(f) + 0.45 * t.noise(min(2 * f, t.n // 2)))).astype(F32)


def stains(t, f=3, amount=0.15, octaves=4):
    """Large, soft weathering and dirt: a multiplier around 1."""
    return (1.0 + amount * t.fbm(f, octaves)).astype(F32)


def streaks(t, amount=0.12, fx=24, fy=1):
    """Soft rain streaks running down the tile: a multiplier <= 1."""
    s = 0.6 * t.fbm(fx, 3, fy=fy) + 0.4 * t.fbm(4, 3)
    return (1.0 - amount * np.clip(s * 0.6 + 0.2, 0, 1)).astype(F32)


def round_corners(L, r):
    """Distance to the border of each unit with its corners rounded by radius r (tile units)."""
    near = (L.ex < r) & (L.ey < r)
    return np.where(near, r - np.hypot(r - L.ex, r - L.ey), np.minimum(L.ex, L.ey)).astype(F32)


def lichen(t, f, cover, size=0.4, x=None, y=None):
    """Round lichen rosettes with softly lobed rims: (mask 0..1, rim 0..1, rosette id)."""
    W = t.worley(f, jitter=0.9, x=x, y=y, edges=False)
    density = 2.0 * smoothstep(-0.6, 1.2, t.fbm(3, 3, x=x, y=y))            # colonies cluster
    keep = (t.rand(W.id) < cover * density).astype(F32)
    rad = size / f * (0.25 + 1.1 * t.rand(W.id) ** 2)
    d = W.f1 / rad * (1.0 + 0.12 * t.fbm(f * 3, 2, x=x, y=y))
    return (smoothstep(1.0, 0.85, d) * keep).astype(F32), smoothstep(0.45, 0.95, d).astype(F32), W.id


def moss(t, cols=("#2c3a14", "#4a5c20", "#6c7a2c", "#8b9140"), f=48):
    """A moss carpet colour: cushions with lighter tips. Returns (colour, cushion field)."""
    clump = t.fbm(f, 4)
    v = nrm(clump * 0.8 + 0.4 * t.noise(t.n // 4) + 0.25 * t.noise(t.n // 2))
    return ramp(v, *cols), clump


def mortar(t, c, amount=0.14, f=64):
    """Sandy mortar or grout: a base colour with grains and soft blotches."""
    g = 0.5 * t.fbm(f, 3) + 0.6 * t.noise(t.n // 3) + 0.4 * t.noise(t.n // 2)
    return mul(fill(t, c), 1.0 + amount * g)


def granite(t, base, dark="#2b2a2c", light="#d2cec6", pink=None, scale=1.0, ids=None):
    """Speckled crystalline stone: base colour with dark mica, light quartz (and optional pink feldspar) grains."""
    f = int(t.n * 0.28 * scale)
    img = base * (1.0 + 0.08 * t.fbm(16, 4))[..., None] if base.ndim == 3 else fill(t, base)
    img = mix(img, col(dark), 0.75 * speckle(t, f, 0.10))
    img = mix(img, col(light), 0.6 * speckle(t, f, 0.09))
    if pink:
        img = mix(img, col(pink), 0.5 * speckle(t, int(f * 0.7), 0.10))
    return img


def wood_grain(t, along, across, ids, rings=14.0, slope=0.1, wave=0.006, fibres=0.12, knots=0.0, size=None):
    """Flat-sawn wood grain in each board's own frame (along / across in tile units, ids per board). The growth
    rings are cylinders around a pith below the surface; the cut plane drifts relative to them, which gives the
    arched 'cathedral' figure, plus fine fibres along the board. knots: the chance of a knot per board (size =
    (length, width) arrays of the boards), with the grain swept around it. Returns 0..1, high in dark late wood."""
    r1, r2, r3, r4 = (hash01(ids, t.seed()) for _ in range(4))
    a = along + r1 * 5.0                                              # each board its own stretch of log
    c = across - (r2 - 0.5) * 0.08 + wave * t.fbm(4, 3, x=a * 0.5, y=r3 * 7.0)
    eye = 0.0
    if knots:
        r5, r6, r7, r8 = (hash01(ids, t.seed()) for _ in range(4))
        length, width = size
        kr = 0.005 + 0.008 * r8
        dx, dy = (along - (0.15 + 0.7 * r5) * length) / (kr * 1.5), (across - (0.25 + 0.5 * r6) * width) / kr
        d = np.sqrt(dx * dx + dy * dy)
        on = (r7 < knots).astype(F32)
        c = c + on * np.sign(dy) * kr * 1.6 * np.exp(-0.25 * d * d)          # grain sweeps around the knot
        eye = on * smoothstep(1.0, 0.75, d) * (0.75 + 0.25 * np.cos(d * 9.0))
    depth = 0.015 + 0.05 * r4 + slope * (r3 - 0.5) * along + 0.012 * t.fbm(2, 2, x=a * 0.4, y=r2 * 7.0)
    g = np.mod(np.sqrt(c * c + depth * depth) * rings * 10.0, 1.0)
    late = smoothstep(0.25, 0.88, g) ** 2 * (1.0 - smoothstep(0.88, 1.0, g))   # darkens slowly, resets sharply
    fib = 0.65 * t.noise(3, 600, x=a * 0.5, y=c + r4 * 3.0) + 0.45 * t.noise(2, 110, x=a * 0.5, y=c + r1 * 3.0)
    streak = t.fbm(2, 3, x=a * 0.5, y=r3 * 9.0 + across * 2.0)
    return np.clip(np.maximum(0.72 * late + fibres * fib + 0.08 * streak + 0.1, eye), 0, 1).astype(F32)


def rune_glyphs(rng, n=32):
    """A set of angular rune shapes: segments between the nodes of a 3 x 3 grid, mostly around a vertical stave.
    Returns an (n, 5, 4) array of segments (x0, y0, x1, y1) in a unit box, unused ones parked far away."""
    nodes = [(x, y) for y in (0.0, 0.5, 1.0) for x in (0.0, 0.5, 1.0)]
    g = np.full((n, 5, 4), 50.0, F32)
    for i in range(n):
        segs = [(0.5, 0.0, 0.5, 1.0) if rng.random() < 0.7 else (0.0, 0.0, 0.0, 1.0)]
        while len(segs) < 2 + rng.integers(0, 3):
            a, b = rng.choice(9, 2, replace=False)
            s = (*nodes[a], *nodes[b])
            if s not in segs and (s[2], s[3], s[0], s[1]) not in segs:
                segs.append(s)
        g[i, :len(segs)] = segs
    return g


def segments_distance(px, py, segs, sx, sy):
    """Distance from points to the nearest of per-pixel segment sets (segs: (..., k, 4)), in a box scaled sx, sy."""
    d = np.full(px.shape, 1e9, F32)
    for k in range(segs.shape[-2]):
        ax, ay, bx, by = (segs[..., k, i] for i in range(4))
        ax, bx = ax * sx, bx * sx
        ay, by = ay * sy, by * sy
        wx, wy = bx - ax, by - ay
        h = np.clip(((px - ax) * wx + (py - ay) * wy) / np.maximum(wx * wx + wy * wy, 1e-12), 0, 1)
        d = np.minimum(d, np.hypot(px - ax - h * wx, py - ay - h * wy))
    return d


# ---------------------------------------------------------------- stone and masonry
@texture(tile_m=3.0, rough=0.85)
def stone_ashlar_grey(t):
    """Dressed limestone blocks in courses of varying height, thin lime joints, diagonal tooling."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(16, 0.001)
    L = courses(x, y, random_heights(rng, 8, 0.25), random_widths(rng, 8, 0.24, 0.45), rng.random(8))
    stone = pick(t.rand(L.id), "#a6a196", "#979187", "#aea694", "#8f8c86", "#a09789", "#b0aca3")
    stone = mul(stone, (0.92 + 0.14 * t.rand(L.id)) * (1 + 0.07 * unit_noise(t, L.id, 6, 5)) * grit(t, 0.05))
    flip = t.rand(L.id) < 0.5
    tool = t.noise(260, 6, x=np.where(flip, x + y, x - y), y=np.where(flip, x - y, x + y))
    stone = mul(stone, 1 + 0.04 * tool * smoothstep(-0.3, 0.6, t.fbm(20, 2)))
    stone = mix(stone, col("#6f6a62"), 0.5 * speckle(t, 300, 0.025))          # pits
    stone = mix(stone, col("#5d5850"), 0.25 * smoothstep(0.4, 1.6, unit_noise(t, L.id, 3, 3)))   # soiling
    cov, e = joints(t, L, 0.003, chip=1.0, chip_f=56)
    stone = mul(stone, bevel(e - 0.003, 0.004, 0.25))
    img = mix(mortar(t, "#8b867c"), stone, cov)
    return mul(img, stains(t, 3, 0.1) * streaks(t, 0.1))


@texture(tile_m=3.5, rough=0.88)
def stone_ashlar_sandstone(t):
    """Large warm sandstone blocks with fine bedding, a few iron-stain bands, worn edges, buff mortar."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(12, 0.0015)
    L = courses(x, y, random_heights(rng, 6, 0.35), random_widths(rng, 6, 0.3, 0.4), rng.random(6))
    lx, ly = L.u * L.w, L.v * L.h
    base = pick(t.rand(L.id), "#c4a27a", "#b8946a", "#cdb08c", "#b38a62", "#c99f73", "#bfa088")
    bed = t.noise(3, 160, x=lx + t.rand(L.id) * 5, y=ly + 0.002 * unit_noise(t, L.id, 6, 3))
    cx, cy = t.rand(L.id) * L.w, t.rand(L.id) * L.h * 3 - L.h
    ring = np.sin(np.hypot(lx - cx, (ly - cy) * 1.3) * 90 + 2 * unit_noise(t, L.id, 3, 3)) * 0.5 + 0.5
    img = mul(base, (1 + 0.05 * bed) * (1 + 0.07 * unit_noise(t, L.id, 10, 5)) * grit(t, 0.07))
    img = mix(img, col("#a3693d"), 0.22 * smoothstep(0.75, 1.0, ring) * (t.rand(L.id) < 0.3))
    img = mix(img, col("#7d6650"), 0.4 * speckle(t, 200, 0.035))                # eroded pits
    cov, e = joints(t, L, 0.0035, chip=1.6, chip_f=36)
    img = mul(img, bevel(e - 0.0035, 0.008, 0.3))
    img = mix(mortar(t, "#c2b293"), img, cov)
    return mul(img, stains(t, 2, 0.12) * streaks(t, 0.1))


@texture(tile_m=3.0, rough=0.92)
def stone_rubble_wall(t):
    """Random rubble: rounded, irregular stones of mixed colour set in thick, sandy, recessed mortar."""
    x, y = t.warp(5, 0.01, 2)
    W = t.worley(8, 11, jitter=0.95, x=x, y=y, rounded=0.006)
    gap = 0.0055 * (1 + 0.4 * t.fbm(14, 2))
    e = W.edge - gap + 0.0015 * t.fbm(40, 3)
    cov = smoothstep(-t.px, t.px, e)
    stone = pick(t.rand(W.id), "#8d877d", "#7b7368", "#a09379", "#6f7377", "#94826b", "#a8a296", "#83705c")
    stone = mul(stone, (0.85 + 0.25 * t.rand(W.id)) * (1 + 0.1 * unit_noise(t, W.id, 7, 5)) * grit(t, 0.08))
    stone = mul(stone, 0.55 + 0.45 * np.sqrt(smoothstep(0.0, 0.03, e)))
    stone = mix(stone, col("#5c554b"), 0.4 * speckle(t, 240, 0.05))
    m = mul(mortar(t, "#a29884", 0.2), 0.8 + 0.2 * smoothstep(-0.004, 0.0, e))
    return mul(mix(m, stone, cov), stains(t, 3, 0.12))


@texture(tile_m=4.0, rough=0.9)
def stone_fieldstone(t):
    """Rounded field boulders of very different sizes, small stones packed between them, dark mortar."""
    x, y = t.warp(5, 0.01, 3)
    big = t.worley(6, 7, jitter=0.9, x=x, y=y, rounded=0.015)
    eb = big.edge - (0.004 + 0.016 * t.rand(big.id)) + 0.002 * t.fbm(40, 3)
    cb = smoothstep(-t.px, t.px, eb)
    small = t.worley(24, 24, jitter=0.9, x=x, y=y, rounded=0.004)
    es = small.edge - 0.0035
    cs = smoothstep(-t.px, t.px, es) * (1 - cb)
    cols = ("#8a8378", "#a19a8d", "#7c7a76", "#9a8a72", "#b0a796", "#857560", "#6e6c68")
    sb = mul(pick(t.rand(big.id), *cols), (0.85 + 0.25 * t.rand(big.id)) * (1 + 0.09 * unit_noise(t, big.id, 5, 5)))
    sb = mul(sb, 0.5 + 0.5 * np.sqrt(smoothstep(0, 0.05, eb)))
    ss = mul(pick(t.rand(small.id), *cols), (0.8 + 0.3 * t.rand(small.id)) * (0.55 + 0.45 * smoothstep(0, 0.008, es)))
    img = mix(mix(mortar(t, "#5f584f", 0.2), ss, cs), sb, cb)
    lm, rim, _ = lichen(t, 30, 0.25, 0.3)
    img = mix(img, mix(col("#c9c3b4"), col("#a9a27a"), rim), 0.6 * lm * cb)
    return mul(img, grit(t, 0.06) * stains(t, 2, 0.1))


@texture(tile_m=3.0, rough=0.9)
def stone_drystone(t):
    """Dry-stone wall: flat slabs of schist of uneven thickness stacked in rough courses, dark gaps, lichen."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(6, 0.005, 3)
    L = courses(x, y, random_heights(rng, 14, 0.55), random_widths(rng, 14, 0.15, 0.7), rng.random(14))
    cut = 0.45 * t.rand(L.id) ** 2                                  # thinner slabs leave a gap above them
    top = (L.v - cut) * L.h
    ex, ey = L.ex, np.minimum(top, (1 - L.v) * L.h)
    r = 0.014
    e = np.where((ex < r) & (ey < r), r - np.hypot(r - ex, r - ey), np.minimum(ex, ey))
    e = e + 0.003 * t.fbm(24, 4)
    cov = smoothstep(0.0015 - t.px, 0.0015 + t.px, e)
    stone = pick(t.rand(L.id), "#7d756a", "#8a8378", "#6e6a63", "#958a78", "#6b6255", "#857f74")
    lam = t.noise(5, 90, x=x + t.rand(L.id) * 3, y=y + t.rand(L.id) * 3)
    stone = mul(stone, (0.85 + 0.3 * t.rand(L.id)) * (1 + 0.07 * lam) * (1 + 0.06 * unit_noise(t, L.id, 8, 4)))
    stone = mul(stone, bevel(e - 0.0015, 0.006, 0.55) * grit(t, 0.07))
    lm, rim, _ = lichen(t, 26, 0.3, 0.35)
    stone = mix(stone, mix(col("#b9b7a8"), col("#a5a06a"), rim), 0.6 * lm)
    gapc = mul(mortar(t, "#3b342c", 0.25), 0.65 + 0.35 * smoothstep(-0.01, 0.0, e))
    return mul(mix(gapc, stone, cov), stains(t, 2, 0.12))


@texture(tile_m=4.0, rough=0.86)
def stone_castle_blocks(t):
    """Big rough-hewn granite blocks: pitched faces, speckled grain, dark joints with a little moss."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(10, 0.002)
    L = courses(x, y, random_heights(rng, 5, 0.12), random_widths(rng, 5, 0.3, 0.3), rng.random(5))
    base = pick(t.rand(L.id), "#7e7c78", "#878581", "#73726f", "#8b8781", "#6f6d6a")
    face = unit_noise(t, L.id, 7, 6, gain=0.55)
    base = mul(base, (0.9 + 0.2 * t.rand(L.id)) * (1 + 0.1 * face))
    img = granite(t, base, pink="#9c8378")
    cov, e = joints(t, L, 0.004, chip=2.0, chip_f=30)
    dome = np.sqrt(smoothstep(0.0, 0.045, e - 0.004))
    img = mul(img, (0.6 + 0.4 * dome) * (1 - 0.6 * np.clip(cavity(face, 0.01), 0, 0.5)))
    mo, _ = moss(t, f=64)
    mmask = smoothstep(0.2, 0.8, t.fbm(6, 3)) * (1 - cov)
    m = mix(mortar(t, "#55524d"), mo, 0.85 * mmask)
    return mul(mix(m, img, cov), stains(t, 2, 0.12) * streaks(t, 0.12))


@texture(tile_m=2.5, rough=0.7)
def stone_flint_wall(t):
    """Knapped flint nodules, dark and glassy with white cortex rims, bedded in pale lime mortar."""
    x, y = t.warp(7, 0.006, 3)
    W = t.worley(8, 9, jitter=0.9, x=x, y=y, rounded=0.01)
    gap = 0.0034 + 0.001 * t.fbm(30, 2)
    e = W.edge - gap + 0.0012 * t.fbm(60, 3)
    cov = smoothstep(-t.px, t.px, e)
    whole = (t.rand(W.id) < 0.15).astype(F32)
    core = pick(t.rand(W.id), "#2c2e35", "#383735", "#4a443d", "#2a2d35", "#55555a", "#3a3530", "#605d5a")
    ripple = np.sin(np.hypot(W.dx + 0.03 * t.rand(W.id), W.dy) * 700 + 3 * t.fbm(20, 2)) * 0.5 + 0.5
    core = mul(core, (1 + 0.18 * unit_noise(t, W.id, 12, 4)) * (1 + 0.1 * ripple))
    core = mix(core, col("#6b6f78"), 0.35 * smoothstep(0.3, 1.2, unit_noise(t, W.id, 9, 3)))
    rind_w = 0.0015 + 0.003 * t.rand(W.id)
    rind = smoothstep(rind_w + t.px, rind_w - t.px, e + 0.002 * t.fbm(50, 2))
    cortex = mul(pick(t.rand(W.id), "#d6cebd", "#a89a82", "#b9ad98"), grit(t, 0.12))
    stone = mix(core, cortex, np.maximum(rind, whole * 0.9))
    stone = mix(stone, col("#8a6a48"), 0.45 * whole * smoothstep(-0.3, 1, t.fbm(30, 3)))
    stone = mul(stone, 0.75 + 0.25 * smoothstep(0, 0.01, e))
    m = mortar(t, "#b3a68e", 0.2)
    m = mix(m, col("#7d7262"), 0.5 * speckle(t, 200, 0.1))
    return mul(mix(m, stone, cov), stains(t, 3, 0.12))


@texture(tile_m=6.0, rough=0.82)
def stone_cyclopean(t):
    """Polygonal cyclopean masonry: huge interlocking blocks with tight joints and pillowed faces."""
    x, y = t.warp(6, 0.003)
    W = t.worley(4, 4, jitter=1.0, x=x, y=y, rounded=0.004)
    base = pick(t.rand(W.id), "#8c8a80", "#837d72", "#97928a", "#7d7b74", "#908676")
    base = mul(base, (0.9 + 0.2 * t.rand(W.id)) * (1 + 0.07 * unit_noise(t, W.id, 5, 6)) * grit(t, 0.06))
    img = mix(base, col("#6a665e"), 0.5 * speckle(t, 160, 0.06))
    cov, e = joints(t, W.edge, 0.0018, chip=2.0, chip_f=24)
    img = mul(img, bevel(e - 0.0018, 0.025, 0.4) * bevel(e - 0.0018, 0.003, 0.25))
    lm, _, lid = lichen(t, 20, 0.35, 0.3)
    img = mix(img, mix(col("#c9c7bb"), col("#b38a3e"), (t.rand(lid) > 0.6) * 1.0), 0.55 * lm * cov)
    return mul(mix(fill(t, "#3b3833"), img, cov), stains(t, 2, 0.15) * streaks(t, 0.15))


@texture(tile_m=3.0, rough=0.8)
def stone_runic_blocks(t):
    """Blue-grey stone blocks, some carved with lines of angular runes holding a faint verdigris pigment."""
    rng = np.random.default_rng(t.seed())
    L = courses(t.x, t.y, [1 / 6] * 6, [[0.25] * 4 if r % 2 else [0.2, 0.3, 0.25, 0.25] for r in range(6)],
                [(r * 0.37 + 0.05) % 1 for r in range(6)])
    base = pick(t.rand(L.id), "#646b72", "#6e7276", "#5b5e63", "#737a80", "#686b70")
    img = mul(base, (0.9 + 0.2 * t.rand(L.id)) * (1 + 0.09 * unit_noise(t, L.id, 8, 5)) * grit(t, 0.07))
    img = mix(img, col("#8e949a"), 0.35 * speckle(t, 280, 0.05))
    img = mix(img, col("#474a4f"), 0.3 * smoothstep(0.3, 1.5, unit_noise(t, L.id, 4, 3)))
    glyphs = rune_glyphs(rng)
    lx, ly = L.u * L.w, L.v * L.h
    gh = L.h * 0.56
    gw = gh * 0.55
    pitch = gw * 1.55
    nslot = np.maximum(np.floor((L.w - 0.3 * L.h) / pitch), 1)
    x0 = (L.w - nslot * pitch) * 0.5
    slot = np.clip(np.floor((lx - x0) / pitch), 0, nslot - 1)
    gid = (hash01(L.id * 16 + slot.astype(np.int64), t.seed()) * len(glyphs)).astype(np.int64)
    px = lx - (x0 + slot * pitch + (pitch - gw) * 0.5)
    py = ly - (L.h - gh) * 0.5
    d = segments_distance(px, py, glyphs[gid], gw, gh) + 0.0008 * t.fbm(80, 2)
    carved = (t.rand(L.id) < 0.5) & (lx > x0) & (lx < L.w - x0)
    groove = smoothstep(0.0048 + t.px, 0.0048 - t.px, d) * carved
    depth = smoothstep(0.0048, 0.0, d)
    lip = smoothstep(0.008, 0.0048, d) * carved * (1 - groove)
    img = mul(img, 1 + 0.12 * lip)
    paint = mix(fill(t, "#33393c"), col("#3d6e63"), np.clip(0.35 + 0.3 * t.fbm(40, 2), 0, 1) * depth)
    img = mix(img, mul(paint, 0.8 + 0.2 * depth), 0.92 * groove)
    cov, e = joints(t, L, 0.003, chip=1.2, chip_f=50)
    img = mul(img, bevel(e - 0.003, 0.005, 0.3))
    return mul(mix(mortar(t, "#45474a"), img, cov), stains(t, 3, 0.14))


@texture(tile_m=3.0, rough=0.9)
def stone_mossy_ruins(t):
    """Weathered, broken limestone blocks with moss gathering in the joints and creeping over the faces."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(8, 0.004, 4)
    L = courses(x, y, random_heights(rng, 7, 0.3), random_widths(rng, 7, 0.22, 0.5), rng.random(7))
    e = round_corners(L, 0.02) - 0.012 * np.maximum(t.fbm(14, 4) - 0.6, 0) + 0.002 * t.fbm(40, 3)
    cov = smoothstep(0.004 - t.px, 0.004 + t.px, e)
    base = pick(t.rand(L.id), "#a5a093", "#97917f", "#ada696", "#8f8c82", "#9c937f")
    img = mul(base, (0.88 + 0.22 * t.rand(L.id)) * (1 + 0.1 * unit_noise(t, L.id, 6, 5)) * grit(t, 0.08))
    img = mul(img, bevel(e - 0.004, 0.012, 0.4))
    img = mix(img, col("#5a5546"), 0.45 * smoothstep(0.4, 1.5, t.fbm(10, 4)))            # damp stains
    lm, _, _ = lichen(t, 24, 0.2, 0.3)
    img = mix(img, col("#c3c2b4"), 0.5 * lm)
    mo, cush = moss(t)
    near = np.exp(-np.maximum(e - 0.004, 0) / 0.01)
    score = 0.45 * t.fbm(4, 4) + near + 0.3 * cush
    thr = float(np.quantile(score, 0.6))
    m = smoothstep(thr - 0.12, thr + 0.12, score)
    img = mix(mix(fill(t, "#3d3a30"), img, cov), mo, m)
    return mul(img, stains(t, 2, 0.1))


# ---------------------------------------------------------------- brick
def brick_faces(t, L, cols, var=0.08, flash=0.25, pits=0.03, pit_col="#3e2a22"):
    """Fired-clay brick faces: a colour per brick, fire flashing towards one end, mottling, sand grain, pits."""
    face = mul(pick(t.rand(L.id), *cols), (0.86 + 0.26 * t.rand(L.id)) * (1 + var * unit_noise(t, L.id, 9, 4)))
    end = np.where(t.rand(L.id) < 0.5, L.u, 1 - L.u)
    face = mul(face, 1 - flash * t.rand(L.id) * smoothstep(0.3, 1.0, end))
    face = mix(face, col(pit_col), 0.6 * speckle(t, t.n // 4, pits))
    return mul(face, grit(t, 0.07))


def brick_wall(t, L, face, mortar_col, joint=0.0035, chip=1.0, depth=0.3, recess=0.8):
    cov, e = joints(t, L, joint, chip=chip, chip_f=60)
    face = mul(face, bevel(e - joint, 0.003, depth))
    m = mul(mortar(t, mortar_col, 0.16), recess + (1 - recess) * smoothstep(0, joint, e))
    return mix(m, face, cov)


@texture(tile_m=1.8, rough=0.85)
def brick_red_running(t):
    """Classic red bricks in running bond with light grey mortar."""
    L = bond(t.x, t.y + 0.0012 * t.fbm(6, 2), 24, 8)
    face = brick_faces(t, L, ("#9b4a35", "#a5553c", "#8c3f2e", "#b0634a", "#7f3a2b", "#a34c37", "#6a3227"))
    img = brick_wall(t, L, face, "#aaa69d")
    return mul(img, stains(t, 3, 0.1) * streaks(t, 0.08))


@texture(tile_m=2.0, rough=0.82)
def brick_flemish(t):
    """Flemish bond: stretchers and dark glazed headers alternating in every course."""
    pairs, rows = 6, 24
    unit = 1.0 / (3 * pairs)
    L = courses(t.x, t.y, [1 / rows] * rows, [[2 * unit, unit] * pairs] * rows,
                [(r % 2) * 1.5 * unit for r in range(rows)])
    header = (L.k % 2 == 1)
    face = brick_faces(t, L, ("#a8573b", "#b2633f", "#9a4c35", "#bb6d48", "#a05a3e"))
    glaze = mul(pick(t.rand(L.id), "#3b3239", "#4a3b3e", "#2f2b33", "#54403d"), 1 + 0.12 * unit_noise(t, L.id, 6, 3))
    glaze = mix(glaze, col("#6d5560"), 0.3 * smoothstep(0.5, 1.5, t.fbm(60, 3)))
    face = np.where(header[..., None], glaze, face)
    img = brick_wall(t, L, face, "#b9b2a4")
    return mul(img, stains(t, 3, 0.1))


@texture(tile_m=2.0, rough=0.9)
def brick_weathered(t):
    """Old soft bricks: uneven sizes, eroded and spalled faces, washed-out mortar, white salt bloom."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(10, 0.0025, 3)
    L = courses(x, y, random_heights(rng, 20, 0.12), random_widths(rng, 20, 1 / 7, 0.12, 0.8), rng.random(20))
    face = brick_faces(t, L, ("#a65a3f", "#b86e4c", "#94503a", "#c27d5a", "#8a4834", "#b0644a"), var=0.12)
    erode = unit_noise(t, L.id, 14, 5)
    spall = smoothstep(0.55, 0.7, unit_noise(t, L.id, 5, 4)) * (t.rand(L.id) < 0.45)
    face = mul(face, 1 - 0.18 * np.clip(cavity(erode, 0.004), 0, 1.5))
    face = mix(face, mul(face * 1.15, grit(t, 0.15, t.n // 3)), spall)
    face = mul(face, 1 - 0.25 * (smoothstep(0.0, 0.08, spall) - smoothstep(0.08, 0.6, spall)))
    cov, e = joints(t, L, 0.004, chip=2.5, chip_f=40)
    face = mul(face, bevel(e - 0.004, 0.005, 0.35))
    m = mul(mortar(t, "#9c9282", 0.25), 0.55 + 0.45 * smoothstep(-0.004, 0.004, e - 0.002 * t.fbm(30, 3)))
    img = mix(m, face, cov)
    bloom = smoothstep(0.8, 2.0, t.fbm(5, 5) + 0.5 * t.fbm(30, 3)) * (0.6 + 0.4 * t.noise(t.n // 4))
    img = mix(img, col("#d8d4cb"), 0.4 * bloom)
    return mul(img, stains(t, 2, 0.15) * streaks(t, 0.12))


@texture(tile_m=1.8, rough=0.8)
def brick_painted_white(t):
    """Whitewashed brick: paint over brick and mortar, flaking off in patches to show the red brick beneath."""
    L = bond(t.x, t.y, 24, 8)
    brick = brick_faces(t, L, ("#9b4a35", "#a5553c", "#8c3f2e", "#b0634a"))
    under = brick_wall(t, L, brick, "#a39d92", chip=1.5)
    cov, e = joints(t, L, 0.0035, chip=1.5, chip_f=60)
    paint = mul(fill(t, "#dcd8cc"), (1 + 0.05 * t.fbm(30, 4)) * grit(t, 0.04) * bevel(e - 0.0035, 0.003, 0.3))
    paint = mul(paint, 0.85 + 0.15 * cov)
    flake = t.fbm(10, 6, gain=0.6) + 0.5 * unit_noise(t, L.id, 4, 2)
    thr = float(np.quantile(flake, 0.75))
    keep = smoothstep(thr + 0.03, thr - 0.03, flake)
    edge = smoothstep(thr - 0.12, thr, flake) * keep           # lifted, dirty paint edges
    img = mix(under, mul(paint, 1 - 0.25 * edge), keep)
    return mul(img, stains(t, 3, 0.1) * streaks(t, 0.15))


@texture(tile_m=2.0, rough=0.88)
def brick_herringbone(t):
    """Worn clay paving bricks laid in a herringbone, with sandy joints."""
    H = herringbone(t.x, t.y, 20, 2)
    face = mul(pick(t.rand(H.id), "#8e4b38", "#7a3f30", "#9a5a43", "#6e3a2e", "#a4664c", "#83483a"),
               (0.85 + 0.25 * t.rand(H.id)) * (1 + 0.1 * unit_noise(t, H.id, 8, 5)) * grit(t, 0.08))
    wear = smoothstep(0.0, 0.025, H.edge) * (0.4 + 0.6 * smoothstep(-0.5, 1.0, t.fbm(4, 3)))
    face = mix(face, mul(face, 1.25), 0.4 * wear)
    face = mix(face, col("#4a3b30"), 0.35 * smoothstep(0.3, 1.5, t.fbm(8, 4)))          # ground-in dirt
    cov, e = joints(t, H, 0.0035, chip=1.5, chip_f=50)
    face = mul(face, bevel(e - 0.0035, 0.006, 0.4))
    m = mix(mortar(t, "#8e8571", 0.25), col("#5b5a3a"), 0.4 * speckle(t, 120, 0.2))
    return mul(mix(m, face, cov), stains(t, 2, 0.15))


@texture(tile_m=1.8, rough=0.88)
def brick_yellow_stock(t):
    """Yellow London stock bricks in English garden-wall bond (a header course every sixth), sooty and mottled."""
    rows, cols = 24, 8
    widths = [[1 / (2 * cols)] * (2 * cols) if r % 6 == 5 else [1 / cols] * cols for r in range(rows)]
    offs = [((r % 2) * 0.5 / cols) if r % 6 != 5 else 0.25 / cols for r in range(rows)]
    L = courses(t.x, t.y, [1 / rows] * rows, widths, offs)
    face = brick_faces(t, L, ("#c9ad78", "#bfa06a", "#d1b98a", "#b89663", "#c4a575", "#a98a5c"), flash=0.15,
                       pits=0.05, pit_col="#3b3030")
    face = mix(face, col("#5c4a42"), 0.6 * speckle(t, t.n // 6, 0.06))
    soot = smoothstep(0.2, 1.4, 0.6 * t.fbm(5, 5) + 0.8 * unit_noise(t, L.id, 3, 2))
    face = mix(face, mul(face, 0.5), 0.6 * soot)
    img = brick_wall(t, L, face, "#7f7a72")
    return mul(img, stains(t, 3, 0.1) * streaks(t, 0.15))


@texture(tile_m=2.0, rough=0.86)
def brick_roman_thin(t):
    """Long, thin Roman-style bricks with wide joints of pinkish lime mortar."""
    rng = np.random.default_rng(t.seed())
    rows = 26
    L = courses(t.x, t.y + 0.002 * t.fbm(4, 2), [1 / rows] * rows, random_widths(rng, rows, 0.25, 0.25, 0.7),
                rng.random(rows))
    face = brick_faces(t, L, ("#c08257", "#b5764d", "#ca9168", "#a96a45", "#c7855e", "#b97f5f"), var=0.1, flash=0.3)
    face = mul(face, 1 + 0.04 * t.noise(4, 200, x=t.x, y=t.y))
    cov, e = joints(t, L, 0.0075, chip=1.2, chip_f=50)
    face = mul(face, bevel(e - 0.0075, 0.003, 0.3))
    m = mix(mortar(t, "#c4a890", 0.2), col("#9a6a55"), 0.5 * speckle(t, t.n // 5, 0.12))      # brick dust
    m = mul(m, 0.82 + 0.18 * smoothstep(0, 0.0075, e))
    return mul(mix(m, face, cov), stains(t, 3, 0.12))


@texture(tile_m=1.8, rough=0.6)
def brick_engineering_blue(t):
    """Dense blue engineering bricks in English bond (header and stretcher courses alternate), crisp joints."""
    rows, cols = 24, 8
    widths = [[1 / cols] * cols if r % 2 == 0 else [1 / (2 * cols)] * (2 * cols) for r in range(rows)]
    L = courses(t.x, t.y, [1 / rows] * rows, widths, [0.25 / cols if r % 4 == 1 else 0 for r in range(rows)])
    face = mul(pick(t.rand(L.id), "#3e3d47", "#45424d", "#36353d", "#4b4645", "#403b44", "#504a4e"),
               (0.88 + 0.24 * t.rand(L.id)) * (1 + 0.06 * unit_noise(t, L.id, 7, 4)) * grit(t, 0.04))
    sheen = smoothstep(0.2, 1.2, unit_noise(t, L.id, 3, 3))
    face = mix(face, col("#5d5d6b"), 0.35 * sheen)
    face = mix(face, col("#5a3f35"), 0.3 * (t.rand(L.id) < 0.12))
    img = brick_wall(t, L, face, "#5a5852", joint=0.003, chip=0.5, depth=0.2)
    return mul(img, stains(t, 3, 0.08))


@texture(tile_m=1.3, rough=0.95)
def brick_adobe(t):
    """Sun-dried mud bricks: large, soft-edged, flecked with straw, laid in thick mud mortar."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(6, 0.006, 3)
    L = courses(x, y, random_heights(rng, 10, 0.12), random_widths(rng, 10, 0.25, 0.15, 0.8), rng.random(10))
    face = mul(pick(t.rand(L.id), "#a8865f", "#9c7a55", "#b39370", "#a07f60", "#94744f"),
               (0.9 + 0.2 * t.rand(L.id)) * (1 + 0.1 * unit_noise(t, L.id, 6, 5)) * grit(t, 0.1))
    sx, sy = t.warp(80, 0.002, 2)
    s1 = smoothstep(0.82, 0.9, t.ridged(30, 2, fy=6, x=sx, y=sy))
    s2 = smoothstep(0.82, 0.9, t.ridged(6, 2, fy=30, x=sy, y=sx))
    straw = np.maximum(s1, s2) * smoothstep(0.0, 0.8, t.fbm(20, 2))
    face = mix(face, col("#c9b07a"), 0.7 * straw)
    e = round_corners(L, 0.02) + 0.004 * t.fbm(16, 4)
    cov = smoothstep(0.009 - t.px, 0.009 + t.px, e)
    face = mul(face, 0.7 + 0.3 * np.sqrt(smoothstep(0.009, 0.03, e)))
    m = mul(mortar(t, "#8f7354", 0.2), 0.85 + 0.1 * t.fbm(40, 3))
    m = mix(m, col("#b39a6e"), 0.5 * straw)
    return mul(mix(m, face, cov), stains(t, 2, 0.12))


@texture(tile_m=2.0, rough=0.8)
def brick_clinker(t):
    """Over-fired clinker bricks: warped, uneven, dark purple-brown with glassy blisters and rough joints."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(8, 0.003, 3)
    L = courses(x, y, random_heights(rng, 22, 0.18), random_widths(rng, 22, 0.13, 0.2, 0.8), rng.random(22))
    face = mul(pick(t.rand(L.id), "#4e2f2c", "#5c3a30", "#3e2a2b", "#6b4031", "#47302f", "#7a4a35", "#36282b"),
               (0.85 + 0.3 * t.rand(L.id)) * (1 + 0.15 * unit_noise(t, L.id, 10, 5)) * grit(t, 0.1))
    glass = speckle(t, 90, 0.07) * (0.3 + 0.7 * t.rand(L.id))
    face = mix(face, col("#2a2c33"), 0.7 * glass)
    face = mix(face, col("#8a5a3e"), 0.4 * speckle(t, 200, 0.06))
    e = round_corners(L, 0.006) - 0.004 * np.maximum(t.fbm(30, 2) - 0.3, 0) + 0.0015 * t.fbm(70, 3)
    cov = smoothstep(0.004 - t.px, 0.004 + t.px, e)
    face = mul(face, bevel(e - 0.004, 0.006, 0.4))
    m = mul(mortar(t, "#77726a", 0.22), 0.7 + 0.3 * smoothstep(0, 0.004, e))
    return mul(mix(m, face, cov), stains(t, 3, 0.12))


# ---------------------------------------------------------------- wood
def pores(t, along, across, ids, cover=0.06, f=(30, 500)):
    """Open pores: short dark dashes running along the grain (board frame coordinates)."""
    o = hash01(ids, t.seed()) * 5.0
    n = t.noise(f[0], f[1], x=along + o, y=across + o)
    thr = float(np.quantile(n, 1.0 - cover))
    return smoothstep(thr, thr + 0.4, n).astype(F32)


def nail_holes(along, across, length, width, inset=0.014, r=0.0022):
    """Two nails at each end of a board."""
    ex = np.abs(np.minimum(along, length - along) - inset)
    ey = np.minimum(np.abs(across - 0.3 * width), np.abs(across - 0.7 * width))
    return smoothstep(r, r * 0.6, np.hypot(ex, ey)).astype(F32)


@texture(tile_m=2.0, rough=0.6)
def wood_planks_oak(t):
    """Oak floorboards of random lengths: arched grain, knots, open pores, nail holes, thin dark gaps."""
    rng = np.random.default_rng(t.seed())
    rows = 8
    L = courses(t.x, t.y, [1 / rows] * rows, random_widths(rng, rows, 0.5, 0.45, 0.5), rng.random(rows))
    along, across = L.u * L.w, L.v * L.h
    g = wood_grain(t, along, across, L.id, rings=10, knots=0.3, size=(L.w, L.h))
    img = mul(ramp(g, "#c9a073", "#a87a4e", "#7a5232"), (0.82 + 0.3 * t.rand(L.id)) * grit(t, 0.03))
    img = mix(img, col("#5a3a22"), 0.45 * pores(t, along, across, L.id))
    img = mix(img, col("#2a2018"), 0.9 * nail_holes(along, across, L.w, L.h))
    cov, e = joints(t, L, 0.0012)
    img = mul(img, bevel(e - 0.0012, 0.002, 0.25))
    img = mix(fill(t, "#2b1d14"), img, cov)
    return mul(img, stains(t, 3, 0.1) * (1 - 0.08 * smoothstep(0.5, 1.5, t.fbm(6, 4))))


@texture(tile_m=2.5, rough=0.9)
def wood_barn_boards(t):
    """Vertical barn boards, weathered silver-grey: raised grain, splits, knots, gaps between the boards."""
    rng = np.random.default_rng(t.seed())
    cols = 10
    L = courses(t.y, t.x, [1 / cols] * cols, random_widths(rng, cols, 0.6, 0.4, 0.5), rng.random(cols))
    along, across = L.u * L.w, L.v * L.h
    g = wood_grain(t, along, across, L.id, rings=7, slope=0.15, wave=0.01, fibres=0.25, knots=0.6, size=(L.w, L.h))
    img = mul(ramp(g, "#a39d92", "#868075", "#58514a"), (0.85 + 0.25 * t.rand(L.id)) * grit(t, 0.06))
    img = mix(img, col("#7a5f45"), 0.45 * smoothstep(0.2, 1.4, unit_noise(t, L.id, 3, 4)))      # brown, less weathered
    split = smoothstep(0.93, 0.975, t.ridged(2, 3, fy=30, x=along + t.rand(L.id) * 3, y=across + t.rand(L.id) * 3))
    img = mix(img, col("#2a241e"), 0.85 * split * smoothstep(0.3, 0.9, unit_noise(t, L.id, 2, 2)))
    img = mix(img, col("#3a332b"), 0.8 * nail_holes(along, across, L.w, L.h, 0.03, 0.003))
    cov, e = joints(t, L, 0.0028, chip=0.6, chip_f=30)
    img = mul(img, bevel(e - 0.0028, 0.003, 0.35))
    return mul(mix(fill(t, "#2a231c"), img, cov), stains(t, 2, 0.12))


@texture(tile_m=1.5, rough=0.85)
def wood_log_ends(t):
    """A woodpile seen end-on: sawn log ends with growth rings, radial checks and bark rims, dark gaps."""
    W = t.worley(7, 8, jitter=0.3, stagger=0.5, edges=False)
    rad = (0.4 + 0.1 * t.rand(W.id)) / 7
    px, py = W.dx + (t.rand(W.id) - 0.5) * rad * 0.4, W.dy + (t.rand(W.id) - 0.5) * rad * 0.4   # off-centre pith
    ang = np.arctan2(W.dy, W.dx)
    d = W.f1 / rad * (1 + 0.04 * np.sin(ang * 3 + t.rand(W.id) * 6) + 0.02 * t.fbm(40, 2))
    inside = smoothstep(1.0 + 0.01, 1.0 - 0.01, d)
    r = np.hypot(px, py) / rad
    nring = 12 + 14 * t.rand(W.id)
    ring = np.mod(r * nring + 0.3 * t.fbm(30, 3), 1.0)
    late = smoothstep(0.6, 0.9, ring) * (1 - smoothstep(0.9, 1.0, ring))
    fresh = t.rand(W.id)
    heart = mix(fill(t, "#a87a4f"), col("#c9a77a"), smoothstep(0.3, 0.75, r))
    face = mix(heart, mul(heart, 0.62), 0.6 * late)
    grey = mul(fill(t, "#8c857b"), 1 + 0.1 * t.fbm(20, 3))                                   # older, weathered logs
    face = mix(face, grey, 0.75 * smoothstep(0.55, 0.9, fresh))
    saw = 1 + 0.05 * t.noise(8, 300, x=t.x + t.y, y=t.x - t.y)                               # saw marks
    face = mul(face, (0.9 + 0.2 * t.rand(W.id)) * saw * grit(t, 0.04))
    k = 2 + (t.rand(W.id) * 3).astype(np.int32)
    phase = np.mod((ang / (2 * np.pi) + t.rand(W.id)) * k, 1.0)
    crack = smoothstep(0.012 / np.maximum(r, 0.05), 0.0, np.minimum(phase, 1 - phase) * r) * smoothstep(0.85, 0.3, r)
    face = mix(face, col("#2a1d14"), 0.85 * crack * (t.rand(W.id) < 0.75))
    bark = mul(fill(t, "#4a3a2c"), 1 + 0.3 * t.fbm(60, 3))
    face = mix(face, bark, smoothstep(0.86, 0.9, d))
    face = mul(face, 0.8 + 0.2 * smoothstep(1.0, 0.8, d))
    gap = mix(fill(t, "#1d1712"), col("#4a3a2c"), 0.4 * speckle(t, 100, 0.2))
    return mul(mix(gap, face, inside), stains(t, 3, 0.08))


@texture(tile_m=1.5, rough=0.95)
def wood_bark_pine(t):
    """Pine bark: long vertical plates of stacked flaky layers between deep dark fissures."""
    x, y = t.warp(5, 0.012, 3)
    W = t.worley(12, 3, jitter=0.9, x=x, y=y, rounded=0.006, aspect=0.25)      # plates four times taller than wide
    gap = 0.006 + 0.004 * t.fbm(8, 3)
    e = W.edge - gap + 0.0025 * t.fbm(30, 3)
    cov = smoothstep(-t.px, t.px, e)
    plate = pick(t.rand(W.id), "#7a4a33", "#6b4532", "#85563c", "#70503f", "#7c5a44", "#5f4234")
    band = np.mod(unit_noise(t, W.id, 6, 3, fy=30) * 1.3, 1.0)
    plate = mul(plate, (0.85 + 0.25 * t.rand(W.id)) * (0.78 + 0.3 * smoothstep(0.0, 0.5, band)))
    plate = mix(plate, col("#a7866c"), 0.3 * smoothstep(0.8, 1.0, band))                   # lifted flake edges
    plate = mul(plate, (1 + 0.07 * t.fbm(40, 3, fy=8)) * grit(t, 0.08))
    plate = mix(plate, col("#8d8a84"), 0.3 * smoothstep(0.6, 1.6, t.fbm(8, 4)))          # grey weathering
    plate = mul(plate, 0.45 + 0.55 * np.sqrt(smoothstep(0, 0.02, e)))
    fiss = mul(fill(t, "#2b1e17"), (0.7 + 0.3 * smoothstep(-0.012, 0, e)) * (1 + 0.2 * t.fbm(64, 2, fy=16)))
    return mix(fiss, plate, cov)


@texture(tile_m=1.0, rough=0.7)
def wood_bark_birch(t):
    """Birch bark: chalky white with horizontal lenticels, black scars and patches peeling to orange."""
    img = mix(fill(t, "#dedad0"), col("#b4b0a6"), smoothstep(-0.5, 1.5, t.fbm(6, 5, fy=12)))
    img = mul(img, (1 + 0.04 * t.fbm(4, 3, fy=64)) * grit(t, 0.03))
    n = t.noise(10, 150)
    thr = float(np.quantile(n, 0.965))
    lent = smoothstep(thr, thr + 0.35, n)
    img = mix(img, col("#4f4842"), 0.85 * lent)
    scar = t.fbm(3, 4, fy=12) + 0.25 * t.fbm(24, 2, fy=48)
    sthr = float(np.quantile(scar, 0.94))
    s = smoothstep(sthr, sthr + 0.1, scar)
    img = mix(img, mul(fill(t, "#2d2a28"), 1 + 0.25 * t.fbm(60, 3)), s)
    peel = t.fbm(8, 4, fy=36)
    pthr = float(np.quantile(peel, 0.93))
    p = smoothstep(pthr, pthr + 0.05, peel) * (1 - s)
    img = mix(img, mul(fill(t, "#c79565"), 1 + 0.15 * t.fbm(40, 3, fy=8)), p)
    img = mul(img, 1 - 0.3 * (smoothstep(pthr - 0.1, pthr, peel) - p))                   # curled edge shadow
    return mul(img, stains(t, 2, 0.06))


@texture(tile_m=2.0, rough=0.75)
def wood_painted_blue(t):
    """Clapboards painted teal-blue, the paint worn through to grey wood at the edges and along the grain."""
    rng = np.random.default_rng(t.seed())
    rows = 12
    L = courses(t.x, t.y, [1 / rows] * rows, random_widths(rng, rows, 0.7, 0.3, 0.6), rng.random(rows))
    along, across = L.u * L.w, L.v * L.h
    g = wood_grain(t, along, across, L.id, rings=9, fibres=0.12)
    wood = mul(ramp(g, "#9a948a", "#7b746a", "#4d463e"), grit(t, 0.05))
    paint = mul(fill(t, "#4f7c86"), (0.9 + 0.15 * t.rand(L.id)) * (1 + 0.06 * t.fbm(10, 4)) * grit(t, 0.03))
    paint = mix(paint, col("#7aa1a6"), 0.35 * smoothstep(0.3, 1.5, t.fbm(3, 4)))         # chalky fading
    cov, e = joints(t, L, 0.0015, chip=1.0)
    near = np.exp(-np.maximum(e, 0) / 0.006)
    score = 0.6 * t.fbm(7, 6, gain=0.6) + 0.9 * near + 0.5 * g
    thr = float(np.quantile(score, 0.72))
    worn = smoothstep(thr - 0.03, thr + 0.03, score)
    img = mix(paint, wood, worn)
    img = mul(img, 1 - 0.18 * (smoothstep(thr - 0.12, thr, score) - worn).clip(0))
    img = mul(img, 1 - 0.35 * np.exp(-across / 0.004))                       # shadow under the board above
    img = mix(fill(t, "#1f2220"), img, cov)
    return mul(img, stains(t, 3, 0.1) * streaks(t, 0.1))


@texture(tile_m=2.0, rough=0.45)
def wood_parquet_herringbone(t):
    """Oak parquet in a 45-degree herringbone, honey tones varying stave to stave, varnished."""
    H = herringbone(t.x, t.y, 4, ratio=4, diagonal=True)
    along, across = H.u * 4 * H.unit, H.v * H.unit
    g = wood_grain(t, along, across, H.id, rings=16, slope=0.12, fibres=0.05)
    img = mul(ramp(g, "#cc9f68", "#b0834e", "#875c33"), (0.8 + 0.35 * t.rand(H.id)) * grit(t, 0.02))
    img = mix(img, col("#6a4a2a"), 0.35 * pores(t, along, across, H.id, 0.05))
    cov, e = joints(t, H, 0.0009)
    img = mul(img, bevel(e - 0.0009, 0.0015, 0.2))
    return mul(mix(fill(t, "#3a2818"), img, cov), stains(t, 3, 0.08))


@texture(tile_m=1.5, rough=0.5)
def wood_parquet_basket(t):
    """Basket-weave parquet: squares of three walnut strips, alternate squares turned."""
    n = 6
    B = basket(t.x, t.y, n, 3)
    along, across = B.u / n, B.v / (3 * n)
    g = wood_grain(t, along, across, B.id, rings=18, slope=0.08, fibres=0.06)
    img = mul(ramp(g, "#8f6240", "#744b2e", "#52321f"), (0.8 + 0.4 * t.rand(B.id)) * grit(t, 0.02))
    img = mix(img, col("#3b2416"), 0.3 * pores(t, along, across, B.id, 0.06))
    cov, e = joints(t, B, 0.0009)
    img = mul(img, bevel(e - 0.0009, 0.0015, 0.2))
    return mul(mix(fill(t, "#24170f"), img, cov), stains(t, 3, 0.1))


@texture(tile_m=1.5, rough=0.85)
def wood_log_cabin(t):
    """A log wall: rounded, weathered logs with checks and knots, pale lime chinking between them."""
    rows = 5
    v = np.mod(t.y + 0.031, 1.0) * rows
    row = np.floor(v).astype(np.int64)
    v = v - row
    chink = 0.085 + 0.03 * t.fbm(4, 2) + 0.006 * t.noise(24)
    s = (v - 0.5) / (0.5 - chink)
    log = smoothstep(1.0 + 0.025, 1.0 - 0.025, np.abs(s))
    shade = np.sqrt(np.clip(1 - s * s, 0, 1)) ** 0.7
    base = pick(hash01(row, t.seed()), "#a07a4e", "#8d6a44", "#b08a5c", "#97734c", "#a9835a")
    grain = t.fbm(3, 3, fy=120) + 0.5 * t.noise(8, 400)
    img = mul(base, (1 + 0.08 * grain) * (0.42 + 0.58 * shade) * grit(t, 0.05))
    img = mul(img, 1 + 0.07 * t.fbm(2, 3, fy=12))                                        # draw-knife facets
    bark = smoothstep(0.55, 0.95, np.abs(s)) * smoothstep(0.2, 0.8, t.fbm(8, 3))
    img = mix(img, mul(fill(t, "#4b3828"), 1 + 0.2 * t.fbm(60, 2)), 0.8 * bark)
    img = mix(img, col("#77706a"), 0.4 * smoothstep(0.0, 1.3, t.fbm(4, 4)))              # silvered patches
    check = smoothstep(0.95, 0.985, t.ridged(2, 3, fy=50)) * smoothstep(0.7, 0.2, np.abs(s))
    img = mix(img, col("#2d2219"), 0.8 * check)
    K = t.worley(6, rows * 2, jitter=0.8, edges=False)
    kd = np.hypot(K.dx / 1.8, K.dy) / 0.013
    knot = smoothstep(1.0, 0.7, kd) * (t.rand(K.id) < 0.35) * smoothstep(0.8, 0.4, np.abs(s))
    img = mix(img, mul(fill(t, "#6b4a30"), 0.8 + 0.2 * np.cos(kd * 8)), 0.6 * knot)
    ch = mul(mortar(t, "#8f8878", 0.22, 40), 0.65 + 0.35 * smoothstep(1.0, 1.25, np.abs(s)))
    return mul(mix(ch, img, log), stains(t, 3, 0.1))


@texture(tile_m=2.0, rough=0.4)
def wood_mahogany_panels(t):
    """Vertical mahogany panelling with strong cathedral figure, a smooth finish and V-grooves."""
    rng = np.random.default_rng(t.seed())
    cols = 4
    L = courses(t.y, t.x, [1 / cols] * cols, random_widths(rng, cols, 0.5, 0.3, 0.6), rng.random(cols))
    along, across = L.u * L.w, L.v * L.h
    g = wood_grain(t, along, across, L.id, rings=7, slope=0.3, wave=0.008, fibres=0.1)
    img = mul(ramp(g, "#94523a", "#78402b", "#55271a"), (0.85 + 0.25 * t.rand(L.id)) * grit(t, 0.015))
    img = mix(img, col("#3e1c12"), 0.3 * pores(t, along, across, L.id, 0.04, (20, 700)))
    img = mul(img, 1 + 0.06 * t.fbm(3, 3, fy=1))
    cov, e = joints(t, L, 0.0016)
    img = mul(img, bevel(e - 0.0016, 0.003, 0.4))
    return mul(mix(fill(t, "#2a120b"), img, cov), stains(t, 3, 0.06))


# ---------------------------------------------------------------- metal
def rust_patches(t, cover, f=6, x=None, y=None):
    """Rust: (coverage 0..1, colour) - patchy at several scales, with darker scale and bright orange flakes."""
    n = t.fbm(f, 6, gain=0.55, x=x, y=y) + 0.35 * t.fbm(f * 8, 3, x=x, y=y)
    thr = float(np.quantile(n, 1.0 - cover))
    m = smoothstep(thr - 0.08, thr + 0.2, n)
    tone = nrm(t.fbm(f * 3, 4) + 0.6 * t.noise(t.n // 4) + 0.4 * t.noise(t.n // 2))
    return m.astype(F32), ramp(tone, "#3e1f14", "#6b3219", "#8f4a22", "#ad6530")


@texture(tile_m=2.0, rough=0.8, metal=0.1)
def metal_rusted_iron(t):
    """An iron sheet almost eaten by rust: layered scale, bright flakes, pitting, a little dark metal left."""
    iron = mul(fill(t, "#46474a"), (1 + 0.1 * t.fbm(8, 4)) * grit(t, 0.06))
    m, rust = rust_patches(t, 0.8, 4)
    img = mix(iron, rust, m)
    m2, rust2 = rust_patches(t, 0.25, 10)
    img = mix(img, mul(rust2, 1.25), 0.8 * m2)
    img = mix(img, col("#c07a3c"), 0.6 * speckle(t, 150, 0.04) * m)                    # loose flakes
    img = mix(img, col("#241510"), 0.7 * speckle(t, t.n // 3, 0.05))                     # pits
    img = mul(img, streaks(t, 0.2, 30, 1))
    return mul(img, stains(t, 2, 0.15))


@texture(tile_m=2.0, rough=0.6, metal=0.0)
def metal_corrugated_painted(t):
    """Red-painted corrugated sheet (vertical ribs) with a lapped seam, screws and rust bleeding down."""
    ribs = 16
    ph = np.cos(t.x * 2 * np.pi * ribs)
    paint = mul(fill(t, "#8e2f25"), (1 + 0.08 * t.fbm(6, 4)) * (0.82 + 0.18 * (0.5 + 0.5 * ph)) * grit(t, 0.03))
    paint = mix(paint, col("#b46a5c"), 0.35 * smoothstep(0.0, 1.5, t.fbm(3, 4)))         # chalky fade
    seam_y = 0.53
    dy = np.mod(t.y - seam_y + 0.5, 1.0) - 0.5
    sx = np.mod(t.x * ribs + 0.5, 1.0) - 0.5                                             # screws on the ridges
    screw = smoothstep(0.0045, 0.003, np.hypot(sx / ribs, dy - 0.012))
    below = (dy > 0.0) * np.exp(-np.maximum(dy, 0) / 0.12)
    m, rust = rust_patches(t, 0.25, 5)
    run = smoothstep(0.1, 0.9, t.fbm(48, 3, fy=2)) * smoothstep(0.08, 0.0, np.abs(sx)) * below
    img = mix(paint, rust, np.clip(m + run * 1.2 + 0.8 * below * smoothstep(0.3, 1, t.fbm(24, 3, fy=3)), 0, 1))
    img = mul(img, 1 - 0.35 * smoothstep(0.003, 0.0, np.abs(dy)))                         # the lap edge
    img = mix(img, col("#5d5c5a"), screw)
    return mul(img, streaks(t, 0.15, 32, 1))


@texture(tile_m=2.0, rough=0.6, metal=0.0)
def metal_riveted_plates(t):
    """Painted steel plates in staggered rows, rows of domed rivets along every edge, grime in the seams."""
    L = courses(t.x, t.y, [1 / 4] * 4, [[1 / 3] * 3] * 4, [(r % 2) * 0.5 / 3 for r in range(4)])
    lx, ly = L.u * L.w, L.v * L.h
    plate = mul(fill(t, "#4e555b"),
                (0.9 + 0.15 * t.rand(L.id)) * (1 + 0.05 * unit_noise(t, L.id, 6, 4)) * grit(t, 0.03))
    m, rust = rust_patches(t, 0.12, 8)
    plate = mix(plate, rust, m)
    inset, sp, rr = 0.018, 0.04, 0.0075

    def rivet_dist(a, b, length, depth):        # rows of rivets along the two edges parallel to `a`
        n = np.maximum(np.round((length - 2 * inset) / sp), 1)
        step = (length - 2 * inset) / n
        c = inset + np.clip(np.round((a - inset) / step), 0, n) * step
        return np.hypot(a - c, np.minimum(np.abs(b - inset), np.abs(depth - b - inset)))
    dh, dv = rivet_dist(lx, ly, L.w, L.h), rivet_dist(ly, lx, L.h, L.w)
    d = np.minimum(dh, dv) / rr
    dome = np.sqrt(np.clip(1 - d * d, 0, 1))
    head = smoothstep(1.0, 0.9, d)
    plate = mul(plate, 1 - 0.3 * smoothstep(1.6, 1.0, d) * (1 - head))                    # grime ring
    plate = mix(plate, mul(fill(t, "#5f666c"), 0.65 + 0.45 * dome), head)
    cov, e = joints(t, L, 0.0016)
    plate = mul(plate, bevel(e - 0.0016, 0.004, 0.35))
    img = mix(fill(t, "#1e1c1a"), plate, cov)
    return mul(img, stains(t, 3, 0.15) * streaks(t, 0.15))


@texture(tile_m=2.0, rough=0.5, metal=0.3)
def metal_copper_patina(t):
    """Aged copper sheet turning to verdigris: chalky blue-green crust in patches and runs over brown copper."""
    cu = mul(ramp(nrm(t.fbm(6, 5) + 0.3 * t.fbm(40, 3)), "#4f2f22", "#74442f", "#8f573b", "#a86e4c"), grit(t, 0.05))
    n = t.fbm(4, 6, gain=0.6) + 0.6 * t.fbm(24, 3, fy=3) + 0.3 * t.noise(t.n // 6)
    thr = float(np.quantile(n, 0.5))
    m = smoothstep(thr - 0.15, thr + 0.35, n)
    green = ramp(nrm(t.fbm(10, 4) + 0.6 * t.noise(t.n // 4)), "#40675a", "#5a8a78", "#78a493", "#9cbcae")
    img = mix(cu, green, m)
    img = mix(img, col("#3b2a22"), 0.35 * smoothstep(0.3, 1.3, t.fbm(12, 4)) * (1 - m))      # dark oxide
    img = mix(img, col("#d8efe2"), 0.25 * speckle(t, 160, 0.06) * m)
    return mul(img, streaks(t, 0.12, 40, 2))


@texture(tile_m=1.0, rough=0.35, metal=1.0)
def metal_brushed_steel(t):
    """Brushed stainless steel: fine horizontal brushing, a few stray scratches and faint smudges."""
    img = mul(fill(t, "#8c8f93"), 1 + 0.06 * t.noise(2, 512) + 0.04 * t.noise(4, 256) + 0.03 * t.fbm(1, 3, fy=64))
    sc = t.ridged(3, 3, fy=1, x=t.x + t.y, y=t.x - t.y)
    img = mul(img, 1 + 0.12 * smoothstep(0.9, 0.98, sc) * smoothstep(0.3, 1.0, t.fbm(8, 2)))
    img = mul(img, 1 - 0.07 * smoothstep(0.2, 1.4, t.fbm(3, 4)))                          # smudges
    return img


@texture(tile_m=1.0, rough=0.4, metal=1.0)
def metal_hammered_bronze(t):
    """Hand-hammered bronze: overlapping round dimples, polished rims, dark patina in the hollows."""
    W = t.worley(16, jitter=0.8)
    size = W.f1 + W.edge                      # about the dimple's radius in this direction
    r = np.clip(W.f1 / np.maximum(size, 1e-4), 0, 1)
    tilt = (W.dx * 0.6 - W.dy * 0.8) / np.maximum(size, 1e-4)        # each concave facet catches the light
    base = mul(ramp(nrm(t.fbm(4, 4)), "#6e5230", "#8c6a3c", "#a6804a"), (0.88 + 0.2 * t.rand(W.id)))
    img = mul(base, (0.85 + 0.3 * tilt * (0.5 + 0.5 * r)) * (1 - 0.1 * (1 - r)))
    hollow = smoothstep(0.6, 0.0, r) * smoothstep(0.2, 1.2, t.fbm(6, 4))
    img = mix(img, col("#3d3a26"), 0.25 * hollow)
    img = mix(img, col("#4f7a5e"), 0.25 * smoothstep(0.8, 1.8, t.fbm(5, 4)))               # green patina
    return mul(img, grit(t, 0.04))


@texture(tile_m=0.8, rough=0.45, metal=1.0)
def metal_diamond_plate(t):
    """Steel tread plate: raised lozenges alternating in direction, scuffed tops, dirt in the low ground."""
    n = 12
    X, Y = t.x * n, t.y * n
    i, j = np.floor(X), np.floor(Y)
    lx, ly = X - i - 0.5, Y - j - 0.5
    s = np.where(np.mod(i + j, 2) == 0, 1.0, -1.0)
    u, v = (lx + s * ly) * 0.7071, (ly - s * lx) * 0.7071
    d = np.hypot(np.maximum(np.abs(u) - 0.25, 0), v) / 0.1
    bump = smoothstep(1.0, 0.8, d)
    top = np.sqrt(np.clip(1 - d * d, 0, 1))
    base = mul(fill(t, "#7d8186"), (1 + 0.06 * t.fbm(6, 4)) * grit(t, 0.05))
    img = mix(mul(base, 0.92), mul(base, 1.0 + 0.35 * top), bump)
    img = mul(img, 1 - 0.4 * smoothstep(1.5, 1.0, d) * (1 - bump))                        # dirt around the lozenges
    wear = smoothstep(0.2, 1.4, t.fbm(5, 5) + 0.3 * t.fbm(40, 2))
    img = mix(img, col("#5a5048"), 0.4 * wear * (1 - bump))
    img = mul(img, 1 + 0.12 * bump * smoothstep(0.5, 1.0, top) * smoothstep(-0.5, 0.8, t.fbm(10, 3)))
    return mul(img, stains(t, 2, 0.12))


@texture(tile_m=1.5, rough=0.5, metal=1.0)
def metal_galvanized(t):
    """Hot-dip galvanised steel: zinc spangle crystals with feathered grain, white rust in places."""
    W = t.worley(9, jitter=1.0)
    a = t.rand(W.id) * F32(np.pi)
    u = W.dx * np.cos(a) + W.dy * np.sin(a)
    v = -W.dx * np.sin(a) + W.dy * np.cos(a)
    feather = t.noise(6, 120, x=u + t.rand(W.id), y=v + t.rand(W.id))
    img = mul(fill(t, "#9a9ea0"), (0.85 + 0.25 * t.rand(W.id)) * (1 + 0.05 * feather) * grit(t, 0.03))
    img = mul(img, 1 - 0.08 * smoothstep(0.002, 0.0, W.edge))
    white = smoothstep(0.5, 1.6, t.fbm(5, 5) + 0.4 * t.fbm(30, 3))
    img = mix(img, mul(fill(t, "#cfd0cc"), grit(t, 0.1)), 0.7 * white)
    return mul(img, stains(t, 3, 0.1) * streaks(t, 0.1))


@texture(tile_m=0.5, rough=0.3, metal=1.0)
def metal_damascus(t):
    """Pattern-welded (Damascus) steel: folded light and dark layers, warped into ripples and raindrops."""
    x, y = t.warp(3, 0.04, 4)
    W = t.worley(7, jitter=0.9, x=x, y=y, edges=False)
    drop = 0.9 * smoothstep(0.06, 0.0, W.f1)
    phase = y * 26 + 0.8 * np.sin(x * 2 * np.pi * 5) + drop * np.cos(W.f1 * 140)
    layers = np.sin(phase * 2 * np.pi) * 0.5 + 0.5
    layers = smoothstep(0.25, 0.75, layers + 0.15 * t.fbm(60, 2))
    img = mix(fill(t, "#3c3e42"), col("#a5a8ac"), layers)
    img = mul(img, (1 + 0.05 * t.noise(2, 512, x=t.x + t.y, y=t.x - t.y)) * stains(t, 3, 0.05))
    return img


@texture(tile_m=0.4, rough=0.45, metal=0.9)
def metal_chainmail(t):
    """Riveted mail: interlinked steel rings, each passing over one neighbour and under the next."""
    cols, rows = 10, 20
    sx, sy = 1.0 / cols, 1.0 / rows
    R, w = 0.56 * sx, 0.1 * sx
    X, Y = t.x * cols, t.y * rows
    j0 = np.floor(Y).astype(np.int64)
    best = np.full(t.x.shape, -9.0, F32)
    shade = np.zeros(t.x.shape, F32)
    rid = np.zeros(t.x.shape, np.int64)
    for dj in range(-3, 4):
        j = j0 + dj
        off = (j % 2) * 0.5
        i0 = np.floor(X - off).astype(np.int64)
        for di in (-1, 0, 1):
            i = i0 + di
            cx, cy = (i + 0.5 + off) * sx, (j + 0.5) * sy
            dx, dy = t.x - cx, t.y - cy
            r = np.hypot(dx, dy)
            d = np.abs(r - R) / w
            on = d < 1.0
            tilt = np.where(j % 2 == 0, 0.8, 2.35)
            depth = np.cos(np.arctan2(dy, dx) - tilt) + 0.01 * hash01(j % rows, 7)
            take = on & (depth > best)
            best = np.where(take, depth, best)
            shade = np.where(take, np.sqrt(np.clip(1 - d * d, 0, 1)) * (0.75 + 0.25 * depth), shade)
            rid = np.where(take, (j % rows) * cols + (i % cols), rid)
    ring = best > -9.0
    metal = mul(fill(t, "#8a8d90"), (0.85 + 0.2 * hash01(rid, t.seed())) * (0.35 + 0.75 * shade))
    metal = mix(metal, col("#6b4a33"), 0.3 * smoothstep(0.5, 1.5, t.fbm(6, 4)))           # light rust
    under = mul(fill(t, "#1f1b17"), 1 + 0.2 * t.fbm(20, 3))
    return np.where(ring[..., None], metal, under).astype(F32)


# ---------------------------------------------------------------- roofing
def roof_courses(t, heights, widths, offsets, ragged=0.0, f=40):
    """Courses of overlapping roofing units (down the tile is down the roof): each course's lower edge lies over
    the top of the course below. ragged: depth of uneven or chipped lower edges, which show the unit beneath.
    Adds L.top, the distance below the edge of the course above (where its shadow falls)."""
    bite = ragged * smoothstep(-0.2, 1.2, t.fbm(f, 3)) if ragged else 0.0
    L = courses(t.x, t.y + bite, heights, widths, offsets)
    L.top = L.v * L.h
    return L


def course_shadow(L, width=0.006, depth=0.45):
    return (1.0 - depth * np.exp(-L.top / width)).astype(F32)


@texture(tile_m=3.0, rough=0.7)
def roof_slate(t):
    """Natural slate roof: blue-grey slates of random widths in staggered courses, chipped edges, a few lichens."""
    rng = np.random.default_rng(t.seed())
    rows = 14
    L = roof_courses(t, [1 / rows] * rows, random_widths(rng, rows, 0.075, 0.35, 0.6), rng.random(rows), 0.006, 50)
    s = mul(pick(t.rand(L.id), "#4f565e", "#5a5f66", "#474c55", "#61666b", "#5a5058", "#535a5c"),
            (0.85 + 0.25 * t.rand(L.id)) * (1 + 0.06 * unit_noise(t, L.id, 6, 4)) * grit(t, 0.05))
    riven = t.noise(3, 40, x=t.x + t.rand(L.id) * 3, y=t.y + t.rand(L.id) * 3)
    s = mul(s, 1 + 0.07 * riven)
    s = mul(s, course_shadow(L))
    gap = smoothstep(0.0012, 0.0022, L.ex)
    s = mul(s, 0.45 + 0.55 * gap)
    lm, rim, _ = lichen(t, 30, 0.1, 0.3)
    s = mix(s, mix(col("#b8b7a6"), col("#9d9b6a"), rim), 0.6 * lm)
    return mul(s, stains(t, 3, 0.12) * streaks(t, 0.1, 30, 2))


@texture(tile_m=2.5, rough=0.85)
def roof_wood_shingles(t):
    """Split cedar shakes: narrow and wide shakes with vertical grain, uneven butts, weathered brown to grey."""
    rng = np.random.default_rng(t.seed())
    rows = 12
    L = roof_courses(t, [1 / rows] * rows, random_widths(rng, rows, 0.055, 0.6, 0.45), rng.random(rows), 0.008, 30)
    along, across = L.v * L.h, L.u * L.w
    g = wood_grain(t, along, across, L.id, rings=9, slope=0.05, fibres=0.3)
    wood = mul(ramp(g, "#8a6a4c", "#6f5540", "#45362a"), (0.8 + 0.35 * t.rand(L.id)) * grit(t, 0.06))
    grey = smoothstep(0.2, 0.9, t.rand(L.id) * 0.7 + 0.3 * smoothstep(0, 1, t.fbm(3, 3)))
    wood = mix(wood, mul(ramp(g, "#8f8a80", "#77716a", "#4a463f"), grit(t, 0.05)), grey)
    split = smoothstep(0.95, 0.985, t.ridged(2, 2, fy=24, x=across * 3 + t.rand(L.id), y=along * 3))
    split = split * (t.rand(L.id) < 0.3)
    wood = mix(wood, col("#2a2018"), 0.8 * split)
    wood = mul(wood, course_shadow(L, 0.008, 0.55))
    wood = mul(wood, 0.35 + 0.65 * smoothstep(0.0012, 0.0028, L.ex))
    mo, _ = moss(t)
    wood = mix(wood, mo, 0.8 * smoothstep(0.6, 1.2, t.fbm(5, 4) + 0.6 * np.exp(-L.top / 0.01)) * 0.7)
    return mul(wood, stains(t, 3, 0.1))


@texture(tile_m=3.0, rough=0.8)
def roof_clay_pantiles(t):
    """S-shaped clay pantiles: a rounded roll and a hollow trough per tile, overlapping courses, lichen and moss."""
    cols, rows = 9, 11
    L = roof_courses(t, [1 / rows] * rows, [[1 / cols] * cols] * rows, [0.0] * rows, 0.002, 60)
    u = L.u
    roll = u < 0.6
    a = np.where(roll, u / 0.6, (u - 0.6) / 0.4) * np.pi
    prof = np.where(roll, np.sin(a), -0.45 * np.sin(a))                  # roll up, then the trough
    slope = np.where(roll, np.cos(a) / 0.6, -0.45 * np.cos(a) / 0.4)    # d(profile)/du, for the shading
    base = mul(pick(t.rand(L.id), "#a6533a", "#ad5a3c", "#9e4e35", "#b3603f", "#a85c40"),
               (0.93 + 0.12 * t.rand(L.id)) * (1 + 0.08 * unit_noise(t, L.id, 6, 4)) * grit(t, 0.06))
    img = mul(base, np.clip(0.72 + 0.38 * prof - 0.2 * np.tanh(slope * 0.5), 0.3, 1.3))
    img = mul(img, course_shadow(L, 0.01, 0.55))
    trough = smoothstep(0.0, -0.35, prof)
    mo, _ = moss(t)
    img = mix(img, mo, 0.85 * trough * smoothstep(0.7, 1.3, t.fbm(5, 4) + 0.3 * t.fbm(30, 2)))
    lm, _, lid = lichen(t, 36, 0.15, 0.35)
    img = mix(img, mix(col("#c4b26a"), col("#d0cdbf"), (t.rand(lid) < 0.4) * 1.0), 0.6 * lm)
    return mul(img, stains(t, 3, 0.12))


@texture(tile_m=2.0, rough=0.95)
def roof_thatch(t):
    """Straw thatch: dense, nearly straight straws running down the roof in stepped courses, greyed with age."""
    x, y = t.warp(4, 0.003, 2)
    fib = 0.5 * t.noise(420, 8, x=x, y=y) + 0.4 * t.noise(220, 5, x=x, y=y) + 0.3 * t.noise(90, 3, x=x, y=y)
    fib = fib + 0.25 * t.noise(512, 24, x=x, y=y)
    rows = 7
    v = np.mod(y * rows + 0.08 * t.fbm(6, 2), 1.0)
    course = 0.8 + 0.2 * smoothstep(0.0, 0.3, v)                    # butt ends of each course shade the next
    tone = ramp(nrm(fib), "#4a3e2e", "#76654a", "#9c865a", "#b9a271")
    img = mul(tone, course * (1 + 0.1 * t.fbm(4, 4)))
    old = smoothstep(0.0, 1.2, t.fbm(3, 5))
    img = mix(img, mul(ramp(nrm(fib), "#47433d", "#67635b", "#878279"), course), 0.55 * old)
    mo, _ = moss(t, f=32)
    img = mix(img, mo, 0.7 * smoothstep(0.9, 1.7, t.fbm(4, 4)))
    return img


@texture(tile_m=2.0, rough=0.7)
def roof_fishscale(t):
    """Fish-scale shingles: round-bottomed slates in staggered rows, dark green-grey with bluish ones mixed in."""
    cols, rows = 12, 18
    X, Y = t.x * cols + 0.25, (t.y + 0.017) * rows
    r0 = np.floor(Y).astype(np.int64)

    def row(r):
        off = (r % 2) * 0.5
        c = np.floor(X - off).astype(np.int64)
        s = (X - off - c) * 2 - 1
        bottom = r + 1 - 0.45 * (1 - np.sqrt(np.clip(1 - s * s, 0, 1)))
        return (r % rows) * cols + c % cols, s, bottom

    ids, ss, bs = zip(*(row(r0 + k) for k in (-1, 0, 1)))
    upper = Y <= bs[1]
    sid = np.where(upper, ids[1], ids[2])
    s = np.where(upper, ss[1], ss[2])
    above = np.where(upper, bs[1] - Y, bs[2] - Y) / rows
    below = np.where(upper, Y - bs[0], Y - bs[1]) / rows
    img = mul(pick(hash01(sid, t.seed()), "#3f4a47", "#47524f", "#3a4446", "#4c5552", "#424a55"),
              (0.85 + 0.25 * hash01(sid, t.seed())) * (1 + 0.06 * unit_noise(t, sid, 6, 3)) * grit(t, 0.05))
    img = mul(img, (1 - 0.5 * np.exp(-below / 0.005)) * (1 - 0.2 * smoothstep(0.8, 1.0, np.abs(s))))
    img = mul(img, 1 + 0.08 * smoothstep(0.012, 0.0, above))
    lm, _, _ = lichen(t, 30, 0.15, 0.3)
    img = mix(img, col("#a8a78f"), 0.5 * lm)
    return mul(img, stains(t, 3, 0.12) * streaks(t, 0.1, 24, 2))


@texture(tile_m=3.0, rough=0.75)
def roof_spanish_barrel(t):
    """Spanish barrel tiles: rounded caps laid over channel tiles in columns, terracotta with dark flashing."""
    cols, rows = 7, 7
    X = t.x * cols + 0.5
    c = np.floor(X).astype(np.int64)
    u = X - c                                          # cap centred at u = 0.5, channels between the caps
    cap = np.abs(u - 0.5) < 0.3
    s = np.where(cap, (u - 0.5) / 0.3, np.where(u < 0.5, u + 0.2, u - 1.2) / 0.2)
    yy = t.y * rows + np.where(cap, 0.5, 0.0) + 0.013
    rid = np.floor(yy).astype(np.int64)
    v = yy - rid
    uid = ((rid % rows) * cols + c % cols) * 2 + cap
    base = mul(pick(hash01(uid, t.seed()), "#b5583a", "#ac5236", "#b95f3d", "#a84f35"),
               (0.93 + 0.12 * hash01(uid, t.seed())) * (1 + 0.1 * unit_noise(t, uid, 5, 4)) * grit(t, 0.06))
    flash = smoothstep(0.5, 1.0, v) * hash01(uid, t.seed()) * 0.6
    roundness = np.sqrt(np.clip(1 - s * s, 0, 1))
    light = np.where(cap, 0.3 + 0.85 * roundness ** 0.7, 0.28 + 0.3 * (1 - roundness))
    img = mul(base, light * (1 - 0.3 * flash) * (1 - 0.45 * np.exp(-v / 0.05)))
    img = mul(img, np.where(cap, smoothstep(1.0, 0.9, np.abs(s)) * 0.6 + 0.4, 1.0))
    mo, _ = moss(t)
    img = mix(img, mo, 0.75 * (~cap) * smoothstep(0.4, 1.1, t.fbm(5, 4)))
    return mul(img, stains(t, 3, 0.12))


@texture(tile_m=4.0, rough=0.55, metal=0.1)
def roof_copper_seam(t):
    """Standing-seam copper roof gone fully to green patina, darker runs down each panel, brown at the seams."""
    panels = 8
    X = t.x * panels + 0.37
    u = X - np.floor(X)
    pid = np.floor(X).astype(np.int64) % panels
    d = np.minimum(u, 1 - u) / panels
    rib = smoothstep(0.0035, 0.002, d)
    side = smoothstep(0.012, 0.0035, d) * (1 - rib)
    green = ramp(nrm(t.fbm(6, 5) + 0.4 * t.noise(t.n // 4)), "#3e6d5c", "#5a917b", "#77ad95", "#93c0aa")
    img = mul(green, (0.9 + 0.15 * hash01(pid, t.seed())) * grit(t, 0.05))
    runs = smoothstep(0.0, 1.5, t.fbm(60, 3, fy=2) + 0.3 * t.fbm(6, 2))
    img = mix(img, col("#2f4c43"), 0.35 * runs)
    brown = smoothstep(0.03, 0.0, d) * smoothstep(0.0, 1.0, t.fbm(10, 3, fy=2))
    img = mix(img, col("#6a4a35"), 0.5 * brown)
    img = mul(img, (1 + 0.45 * rib * smoothstep(0.0035, 0.0, d)) * (1 - 0.3 * side))
    img = mix(img, col("#a7d1bb"), 0.25 * speckle(t, 200, 0.05))
    return mul(img, stains(t, 2, 0.1))


@texture(tile_m=3.0, rough=0.88)
def roof_stone_slates(t):
    """Heavy limestone roof slates diminishing up the roof, thick uneven edges, bright yellow and grey lichens."""
    rng = np.random.default_rng(t.seed())
    rows = 10
    L = roof_courses(t, random_heights(rng, rows, 0.35), random_widths(rng, rows, 0.13, 0.5, 0.5), rng.random(rows),
                     0.012, 24)
    s = mul(pick(t.rand(L.id), "#9c937f", "#a8a08c", "#8f8774", "#b0a690", "#958b78"),
            (0.85 + 0.25 * t.rand(L.id)) * (1 + 0.1 * unit_noise(t, L.id, 6, 5)) * grit(t, 0.1))
    s = mix(s, col("#5d564a"), 0.4 * speckle(t, 180, 0.06))
    s = mul(s, course_shadow(L, 0.01, 0.6))
    s = mul(s, 0.35 + 0.65 * smoothstep(0.001, 0.004, L.ex + 0.002 * t.fbm(40, 2)))
    lm, rim, lid = lichen(t, 22, 0.45, 0.4)
    yellow = t.rand(lid) < 0.5
    lc = np.where(yellow[..., None], mix(col("#c9a43e"), col("#b58a2e"), rim), mix(col("#c8c7b9"), col("#a9a89a"), rim))
    s = mix(s, lc, 0.75 * lm)
    mo, _ = moss(t)
    s = mix(s, mo, smoothstep(0.7, 1.0, np.exp(-L.top / 0.012) + 0.5 * t.fbm(6, 3)) * 0.8)
    return mul(s, stains(t, 2, 0.12))


@texture(tile_m=2.5, rough=0.8)
def roof_plain_tiles(t):
    """Small flat clay plain tiles in a rich mix of reds, browns and purples, with moss tufts in the joints."""
    rows, cols = 20, 15
    L = roof_courses(t, [1 / rows] * rows, [[1 / cols] * cols] * rows, [(r % 2) * 0.5 / cols for r in range(rows)],
                     0.003, 70)
    s = mul(pick(t.rand(L.id), "#9a4a34", "#8a3f2f", "#a85a3c", "#6f3a30", "#7d4a3a", "#b0663f", "#5e3530"),
            (0.85 + 0.3 * t.rand(L.id)) * (1 + 0.08 * unit_noise(t, L.id, 7, 4)) * grit(t, 0.08))
    s = mul(s, (0.85 + 0.15 * np.sin(L.u * np.pi)) * course_shadow(L, 0.006, 0.5))
    s = mul(s, 0.4 + 0.6 * smoothstep(0.0008, 0.002, L.ex))
    mo, _ = moss(t)
    tuft = smoothstep(0.55, 0.8, t.fbm(20, 3) + 0.5 * np.exp(-L.top / 0.006)) * smoothstep(0.0, 0.8, t.fbm(4, 3))
    s = mix(s, mo, tuft)
    return mul(s, stains(t, 3, 0.15))


@texture(tile_m=3.0, rough=0.9)
def roof_asphalt_shingles(t):
    """Three-tab asphalt shingles: mineral granules in blended greys, tab slots and shadow lines."""
    rows, tabs = 12, 9
    L = roof_courses(t, [1 / rows] * rows, [[1 / tabs] * tabs] * rows, [(r * 0.5 / tabs) % 1 for r in range(rows)])
    g = t.noise(t.n // 2)
    gran = ramp(nrm(g + 0.5 * t.noise(t.n // 3)), "#2b2b2c", "#4a4948", "#6a6866", "#8a8580")
    tone = pick(t.rand(L.id), "#9a968f", "#8a857e", "#a39f97", "#7d7a75")
    img = mul(gran * tone / 0.3, (0.9 + 0.15 * t.rand(L.id)) * course_shadow(L, 0.005, 0.5))
    slot = smoothstep(0.0025, 0.0012, L.ex) * smoothstep(0.35, 0.45, L.v)
    img = mul(img, 1 - 0.75 * slot)
    return mul(img, stains(t, 3, 0.15) * streaks(t, 0.15, 30, 2))


# ---------------------------------------------------------------- ground
def stones(t, f, cover, size=0.4, x=None, y=None, rounded=True):
    """Scattered small stones: (coverage, shade 0..1 across each stone, id)."""
    W = t.worley(f, jitter=0.9, x=x, y=y, edges=False)
    keep = (t.rand(W.id) < cover).astype(F32)
    rad = size / f * (0.5 + 0.8 * t.rand(W.id))
    d = W.f1 / rad * (1 + 0.15 * t.fbm(f * 2, 2, x=x, y=y))
    return smoothstep(1.0, 0.85, d) * keep, np.sqrt(np.clip(1 - d * d, 0, 1)), W.id


def soil(t, cols=("#3b2c20", "#5a4431", "#76593f", "#8d6f50"), f=4):
    """Earth: multi-scale mottling between dark and light browns."""
    n = t.fbm(f, 6, gain=0.55) + 0.5 * t.fbm(f * 8, 3) + 0.3 * t.noise(t.n // 3)
    return mul(ramp(nrm(n), *cols), grit(t, 0.08))


@texture(tile_m=3.0, rough=0.95)
def ground_dirt(t):
    """Packed earth path: mottled browns, embedded pebbles, crumbs and clods, damp darker hollows."""
    img = soil(t)
    img = mix(img, col("#2e2219"), 0.45 * smoothstep(0.4, 1.6, t.fbm(3, 5)))               # damp hollows
    clod, cs, cid = stones(t, 50, 0.3, 0.45)
    img = mix(img, mul(pick(t.rand(cid), "#6e5540", "#5a4636", "#7f644a"), 0.6 + 0.5 * cs), clod)
    for f, c in ((80, 0.15), (32, 0.1)):
        m, sh, sid = stones(t, f, c, 0.35)
        st = mul(pick(t.rand(sid), "#7a7268", "#8a7e6c", "#665f58", "#8b7a62", "#958b7c"), 0.45 + 0.6 * sh)
        img = mix(mul(img, 1 - 0.35 * smoothstep(0, 1, blur(m, 0.002))), st, m)
    return mul(img, stains(t, 2, 0.12))


@texture(tile_m=2.0, rough=0.9)
def ground_gravel(t):
    """Loose angular gravel in three sizes, greys and browns, dark voids between the stones."""
    img = mul(fill(t, "#2b2621"), 1 + 0.2 * t.fbm(30, 3))
    cols = ("#8c867c", "#a39d92", "#6f6a62", "#9b8a72", "#7d7468", "#b3ab9d", "#5e5a55")
    for f in (64, 40, 26):
        x, y = t.warp(f * 2, 0.08 / f, 2)
        W = t.worley(f, jitter=0.9, x=x, y=y, rounded=0.06 / f)
        shrink = (0.06 + 0.12 * t.rand(W.id)) / f
        e = W.edge - shrink
        cov = smoothstep(-t.px, t.px, e) * (t.rand(W.id) < 0.8)
        facet = t.rand(W.id) * 0.3 + 0.1 * (W.dx * (t.rand(W.id) - 0.5) + W.dy * (t.rand(W.id) - 0.5)) * f
        st = mul(pick(t.rand(W.id), *cols), (0.8 + facet) * (0.55 + 0.45 * smoothstep(0, 0.25 / f, e)) * grit(t, 0.08))
        img = mix(mul(img, 1 - 0.3 * cov), st, cov)
    return mul(img, stains(t, 3, 0.1))


@texture(tile_m=3.0, rough=0.95)
def ground_sand_ripples(t):
    """Wind-rippled sand: parallel, sometimes forking ripple crests, darker heavy grains in the troughs."""
    x, y = t.warp(2, 0.012, 3)
    ph = np.mod(y * 22 + 0.35 * t.fbm(3, 3, x=x, y=y) + 0.25 * np.sin(x * 2 * np.pi * 3), 1.0)
    prof = np.where(ph < 0.7, ph / 0.7, (1 - ph) / 0.3)                   # gentle windward, steep lee
    lee = smoothstep(0.7, 0.78, ph)
    base = ramp(nrm(t.fbm(4, 4) + 0.3 * t.noise(t.n // 3)), "#b59c72", "#c8b48a", "#d3c19a")
    img = mul(base, (0.8 + 0.22 * prof - 0.12 * lee) * grit(t, 0.07))
    img = mix(img, col("#7a6a52"), 0.4 * smoothstep(0.25, 0.0, prof) * smoothstep(-0.2, 0.8, t.noise(t.n // 4)))
    img = mix(img, col("#e8dcc0"), 0.2 * speckle(t, t.n // 2, 0.05))
    return mul(img, stains(t, 2, 0.08))


@texture(tile_m=3.0, rough=0.4)
def ground_mud(t):
    """Churned wet mud: soft ridges and hollows, dark standing puddles and bits of straw."""
    img = soil(t, ("#3a2b20", "#4d3a2b", "#5f4936", "#705842"), 3)
    lumps = t.fbm(14, 5, gain=0.55)
    img = mul(img, 1 + 0.15 * lumps - 0.6 * np.clip(cavity(lumps, 0.005), 0, 0.6))
    wet = t.fbm(4, 5, gain=0.55) - 0.4 * lumps
    thr = float(np.quantile(wet, 0.86))
    pud = smoothstep(thr, thr + 0.1, wet)
    img = mix(img, mul(fill(t, "#2a221b"), 1 + 0.1 * t.fbm(6, 3)), pud)
    img = mul(img, 1 - 0.25 * (smoothstep(thr - 0.3, thr, wet) - pud).clip(0))           # dark wet rims
    sx, sy = t.warp(60, 0.002, 2)
    straw = np.maximum(smoothstep(0.9, 0.96, t.ridged(18, 2, fy=4, x=sx, y=sy)),
                       smoothstep(0.9, 0.96, t.ridged(4, 2, fy=18, x=sx, y=sy)))
    img = mix(img, col("#8f7a52"), 0.6 * straw * (1 - pud) * smoothstep(0.3, 1.0, t.fbm(6, 2)))
    return img


@texture(tile_m=3.0, rough=0.95)
def ground_cracked_earth(t):
    """Sun-baked clay cracked into polygons with curled, paler rims and fine secondary cracks."""
    x, y = t.warp(6, 0.01, 4)
    W = t.worley(8, jitter=0.9, x=x, y=y, rounded=0.006)
    gap = 0.004 + 0.003 * t.fbm(20, 3)
    e = W.edge - gap + 0.001 * t.fbm(60, 2)
    cov = smoothstep(-t.px, t.px, e)
    base = mul(pick(t.rand(W.id), "#a88a68", "#b39572", "#9c7f5f", "#ad916f"), (0.92 + 0.12 * t.rand(W.id)))
    img = mul(base, (1 + 0.06 * t.fbm(10, 4)) * grit(t, 0.06))
    img = mul(img, 1 + 0.18 * smoothstep(0.012, 0.0, e) * cov)                              # curled rims
    W2 = t.worley(26, jitter=0.9, x=x, y=y)
    fine = smoothstep(0.0012, 0.0, W2.edge) * smoothstep(0.2, 0.9, t.fbm(5, 3))
    img = mul(img, 1 - 0.45 * fine)
    crack = mul(fill(t, "#3a2a1e"), 0.6 + 0.4 * smoothstep(-0.006, 0, e))
    return mul(mix(crack, img, cov), stains(t, 2, 0.1))


@texture(tile_m=2.0, rough=0.9)
def ground_forest_floor(t):
    """Leaf litter: overlapping fallen leaves in autumn browns and ochres, pine needles and twigs on dark soil."""
    img = soil(t, ("#1e1610", "#2e2218", "#3d2d20", "#4a3828"), 6)

    def leaf(lx, ly, r):
        L, Wd = 0.75 + 0.35 * r[0], 0.3 + 0.12 * r[1]
        s = np.clip(lx / L, -1.2, 1.2)
        half = Wd * np.sqrt(np.clip(1 - s * s, 0, 1)) * (1 - 0.35 * s)
        inside = smoothstep(0.02, -0.02, np.abs(ly) - half) * (np.abs(s) < 1)
        vein = 1 - 0.3 * smoothstep(0.03, 0.0, np.abs(ly)) * (np.abs(s) < 0.95)
        return inside.astype(F32), vein.astype(F32)

    cov, vein, sid, lay = scatter(t, 14, 6, leaf)
    lc = pick(hash01(sid, t.seed()), "#6e4626", "#7d5230", "#94682e", "#5e3a22", "#a07434", "#54442c", "#86502a",
              "#4a3626", "#6a5a3a", "#3f3226")
    depth = 0.55 + 0.45 * (lay / 6.0)
    lc = mul(lc, depth * vein * (0.85 + 0.3 * hash01(sid, t.seed())) * (1 + 0.1 * t.fbm(40, 3)))
    lc = mix(lc, col("#2e2418"), 0.4 * smoothstep(0.3, 1.5, t.fbm(20, 3)))                 # rot spots
    img = mix(img, lc, cov)

    def needle(lx, ly, r):
        L = 0.9 + 0.5 * r[0]
        return (smoothstep(0.03, 0.015, np.abs(ly)) * (np.abs(lx) < L)).astype(F32), np.ones_like(lx)

    nc, _, nid, _ = scatter(t, 36, 1, needle, size=0.8)
    some = hash01(nid, t.seed()) < 0.4
    img = mix(img, pick(hash01(nid, t.seed()), "#7a5c36", "#5e462c", "#8f6e40"), 0.8 * nc * some)
    return mul(img, stains(t, 3, 0.12))


@texture(tile_m=2.0, rough=0.75)
def ground_cobblestones(t):
    """Rounded cobbles in uneven rows, worn smooth on top, dark earth and moss between them."""
    x, y = t.warp(8, 0.004, 3)
    W = t.worley(14, 16, jitter=0.55, stagger=0.5, x=x, y=y, rounded=0.008)
    gap = 0.0045 + 0.0015 * t.fbm(30, 2)
    e = W.edge - gap + 0.0012 * t.fbm(60, 3)
    cov = smoothstep(-t.px, t.px, e)
    st = mul(pick(t.rand(W.id), "#6f6c68", "#7d7870", "#5f5d5b", "#8a8277", "#6a625a", "#77736c"),
             (0.85 + 0.25 * t.rand(W.id)) * (1 + 0.08 * unit_noise(t, W.id, 9, 4)) * grit(t, 0.06))
    st = granite(t, st, scale=0.8)
    dome = np.sqrt(smoothstep(0.0, 0.03, e))
    st = mul(st, (0.5 + 0.5 * dome) * (1 + 0.15 * smoothstep(0.5, 1.0, dome) * t.rand(W.id)))
    mo, _ = moss(t, f=80)
    joint = mix(mul(fill(t, "#2e2720"), 1 + 0.3 * t.fbm(40, 3)), mul(mo, 0.7), 0.5 * smoothstep(0.3, 1.0, t.fbm(6, 3)))
    return mul(mix(joint, st, cov), stains(t, 3, 0.12))


@texture(tile_m=4.0, rough=0.85)
def ground_flagstone_path(t):
    """Irregular flat flagstones with grass and moss growing in the wide gaps between them."""
    x, y = t.warp(5, 0.008, 3)
    W = t.worley(5, jitter=0.9, x=x, y=y, rounded=0.004)
    gap = 0.008 + 0.004 * t.fbm(10, 3)
    e = W.edge - gap + 0.0015 * t.fbm(50, 3)
    cov = smoothstep(-t.px, t.px, e)
    st = mul(pick(t.rand(W.id), "#8f877a", "#9a8d78", "#7f7a72", "#a39a88", "#8a7f6c"),
             (0.88 + 0.2 * t.rand(W.id)) * (1 + 0.08 * unit_noise(t, W.id, 6, 5)) * grit(t, 0.07))
    st = mix(st, col("#5f584d"), 0.35 * speckle(t, 160, 0.05))
    st = mul(st, bevel(e, 0.006, 0.35))
    st = mul(st, 1 - 0.12 * smoothstep(0.4, 1.4, t.fbm(8, 4)))
    grass = grass_carpet(t, 90)
    soilc = soil(t, ("#2b2119", "#3e3024", "#4d3c2c", "#5a4733"), 8)
    gfill = mix(soilc, grass, smoothstep(-0.3, 0.6, t.fbm(12, 3) + 0.8 * smoothstep(0.0, -0.012, e)))
    return mul(mix(gfill, st, cov), stains(t, 3, 0.1))


@texture(tile_m=2.0, rough=0.7)
def ground_granite_setts(t):
    """Granite setts in courses: small rectangular blocks with rounded tops, grey and pink, sandy joints."""
    rng = np.random.default_rng(t.seed())
    rows = 14
    L = courses(t.x, t.y, random_heights(rng, rows, 0.12), random_widths(rng, rows, 0.075, 0.3, 0.7), rng.random(rows))
    e = round_corners(L, 0.008)
    cov = smoothstep(0.0035 - t.px, 0.0035 + t.px, e + 0.001 * t.fbm(50, 3))
    base = pick(t.rand(L.id), "#8a8682", "#7c7874", "#958c86", "#8e817c", "#6f6d6b")
    base = mul(base, (0.88 + 0.2 * t.rand(L.id)) * (1 + 0.06 * unit_noise(t, L.id, 6, 3)))
    st = granite(t, base, pink="#a48478", scale=0.9)
    st = mul(st, 0.55 + 0.45 * np.sqrt(smoothstep(0.0035, 0.025, e)))
    st = mul(st, 1 + 0.1 * smoothstep(0.015, 0.03, e) * t.rand(L.id))                    # polished tops
    j = mortar(t, "#6e6555", 0.25)
    return mul(mix(j, st, cov), stains(t, 3, 0.12))


def grass_carpet(t, f=56, layers=7):
    """Short grass seen from above: thin blades in random directions, darker deep in the sward."""
    def blade(lx, ly, r):
        L = 0.9 + 0.6 * r[0]
        s = np.clip((lx / L + 1) * 0.5, 0, 1)
        w = 0.07 * (1 - s) + 0.01
        c = smoothstep(w, w * 0.6, np.abs(ly + 0.15 * (s * s) * (r[1] - 0.5))) * (np.abs(lx) < L)
        return c.astype(F32), (0.6 + 0.4 * s).astype(F32)

    base = mul(ramp(nrm(t.fbm(8, 4)), "#1c2a0c", "#2a3a12"), 1.0)
    cov, tip, sid, lay = scatter(t, f, layers, blade, size=1.0)
    gc = pick(hash01(sid, t.seed()), "#4b6a1c", "#5a7a22", "#3f5e18", "#6a8228", "#58702a", "#7a8a34")
    gc = mul(gc, (0.55 + 0.5 * lay / (layers - 1)) * tip * (1 + 0.15 * t.fbm(6, 3)))
    return mix(base, gc, cov)


@texture(tile_m=2.0, rough=0.9)
def ground_grass(t):
    """Short, dense turf: thousands of blades in mixed greens, with dry yellow and thin patches."""
    img = grass_carpet(t)
    dry = smoothstep(0.3, 1.4, t.fbm(3, 5))
    straw = mul(fill(t, "#8a7a40"), 0.4 + 0.9 * luminance(img) / 0.08)
    img = mix(img, straw, 0.35 * dry)
    bare = smoothstep(1.0, 1.8, t.fbm(4, 5))
    img = mix(img, soil(t, ("#2e2318", "#45352a", "#5a4634", "#6b543e"), 8), 0.7 * bare)
    return mul(img, stains(t, 3, 0.1))


# ---------------------------------------------------------------- natural surfaces
@texture(tile_m=8.0, rough=0.85)
def natural_rock_face(t):
    """Granite cliff face: big fractured facets in different tones, dark joints and cracks, lichen, streaks."""
    x, y = t.warp(4, 0.02, 4)
    W = t.worley(5, jitter=1.0, x=x, y=y)
    W2 = t.worley(12, jitter=1.0, x=x, y=y)
    tone = 0.75 + 0.35 * t.rand(W.id) + 0.12 * (t.rand(W2.id) - 0.5)
    base = mul(ramp(nrm(t.fbm(3, 4)), "#6f6a63", "#837d74", "#948d82"), tone)
    img = granite(t, mul(base, 1 + 0.08 * t.fbm(16, 5)), dark="#353333", light="#b7b2a8", scale=0.7)
    j1 = smoothstep(0.004, 0.0, W.edge + 0.002 * t.fbm(40, 2))
    j2 = smoothstep(0.0015, 0.0, W2.edge) * smoothstep(0.0, 0.8, t.fbm(6, 2))
    img = mul(img, (1 - 0.6 * j1) * (1 - 0.35 * j2) * bevel(W.edge, 0.02, 0.25))
    cr = smoothstep(0.93, 0.98, t.ridged(6, 4, x=x, y=y))
    img = mul(img, 1 - 0.4 * cr)
    lm, _, lid = lichen(t, 40, 0.25, 0.35)
    img = mix(img, np.where((t.rand(lid) < 0.5)[..., None], col("#c3c4b3"), col("#9da35e")), 0.55 * lm)
    return mul(img, streaks(t, 0.25, 20, 1) * stains(t, 2, 0.15))


@texture(tile_m=1.5, rough=0.95)
def natural_moss(t):
    """Thick moss carpet: lumpy cushions of fine fronds, yellow-green on top, dark in the hollows."""
    lump = 0.5 * t.fbm(6, 3) + 0.5 * t.fbm(20, 3)
    fronds = 0.6 * t.noise(t.n // 4) + 0.5 * t.noise(t.n // 2) + 0.35 * t.noise(t.n // 8)
    v = np.clip(0.5 + 0.13 * lump + 0.16 * fronds - 0.5 * np.clip(cavity(lump, 0.01), 0, 0.4), 0, 1)
    img = ramp(v, "#1a260c", "#2f4316", "#4f6620", "#748a2c", "#9da444")
    spore = speckle(t, t.n // 3, 0.012) * smoothstep(0.3, 1.0, t.fbm(8, 2))
    img = mix(img, col("#8a4a2a"), 0.6 * spore)
    return mul(img, stains(t, 2, 0.1))


@texture(tile_m=2.0, rough=0.85)
def natural_lichen_rock(t):
    """Grey rock crowded with lichen colonies: orange, yellow-green with black edges, pale grey-green lobes."""
    img = granite(t, mul(ramp(nrm(t.fbm(4, 5)), "#6c6a66", "#85827c", "#9a968e"), 1.0), scale=0.6)
    img = mul(img, 1 - 0.3 * smoothstep(0.92, 0.98, t.ridged(5, 4)))
    for f, cover, size, c1, c2 in ((14, 0.5, 0.5, "#a9b29a", "#c4c9b4"), (22, 0.45, 0.45, "#c47f2a", "#dca048"),
                                    (30, 0.5, 0.4, "#aeb13e", "#c9c86a"), (46, 0.4, 0.4, "#d6d4c8", "#bdbbb0")):
        m, rim, lid = lichen(t, f, cover, size)
        c = mix(fill(t, c1), col(c2), rim)
        c = mul(c, (0.85 + 0.3 * t.rand(lid)) * (1 + 0.15 * t.noise(t.n // 4)))
        edge = smoothstep(0.0, 0.25, m) - smoothstep(0.25, 0.6, m)
        img = mix(mul(img, 1 - 0.5 * edge), c, smoothstep(0.3, 0.6, m))
    return mul(img, stains(t, 3, 0.1))


@texture(tile_m=4.0, rough=0.6)
def natural_snow(t):
    """Wind-packed snow: soft rounded drifts and faint sastrugi, blue-grey in the hollows, sparkling grain."""
    x, y = t.warp(2, 0.04, 3)
    h = t.fbm(3, 4, x=x, y=y) + 0.1 * t.fbm(4, 3, fy=10, x=x, y=y)
    shade = np.clip(0.6 + 1.5 * (h - blur(h, 0.04)), 0, 1)
    img = mix(fill(t, "#c8d3df"), col("#f1f3f5"), 0.35 + 0.65 * shade)
    img = mul(img, grit(t, 0.025))
    img = mix(img, col("#ffffff"), 0.5 * speckle(t, t.n // 2, 0.01, soft=0.15))
    return img


@texture(tile_m=3.0, rough=0.15)
def natural_ice(t):
    """Lake ice: deep blue-green body with frosted patches, white fracture lines and trapped bubbles."""
    deep = ramp(nrm(t.fbm(3, 5)), "#24435a", "#3b6a80", "#5f8fa0", "#8db4c0")
    img = mul(deep, 1 + 0.06 * t.fbm(20, 3))
    frost = smoothstep(0.2, 1.4, t.fbm(4, 5) + 0.3 * t.fbm(40, 3))
    img = mix(img, mul(fill(t, "#d4e2e8"), grit(t, 0.06)), 0.7 * frost)
    x, y = t.warp(5, 0.01, 3)
    W = t.worley(4, jitter=1.0, x=x, y=y)
    W2 = t.worley(11, jitter=1.0, x=x, y=y)
    crack = smoothstep(0.0022, 0.0, W.edge) + 0.6 * smoothstep(0.0012, 0.0, W2.edge) * smoothstep(0, 0.8, t.fbm(5, 2))
    img = mix(img, col("#e9f2f4"), np.clip(crack, 0, 1) * 0.85)
    img = mul(img, 1 - 0.25 * smoothstep(0.012, 0.0022, W.edge) * (1 - np.clip(crack, 0, 1)))
    bub, sh, _ = stones(t, 70, 0.3, 0.25)
    img = mix(img, mul(fill(t, "#dfeef2"), 0.75 + 0.35 * (1 - sh)), 0.6 * bub * (1 - frost))
    return img


@texture(tile_m=3.0, rough=0.9)
def natural_lava_rock(t):
    """Cooled lava: black-brown scoria pitted with gas holes, ropy flow folds, rusty oxidised patches."""
    x, y = t.warp(3, 0.04, 4)
    rope = np.sin((y * 30 + 2.5 * t.fbm(3, 3, x=x, y=y)) * 2 * np.pi + 0.8 * t.fbm(20, 2)) * 0.5 + 0.5
    base = ramp(nrm(t.fbm(6, 5) + 0.4 * t.noise(t.n // 4)), "#2a2523", "#3b3431", "#4f443e", "#5e4d44")
    img = mul(base, (0.85 + 0.25 * rope) * grit(t, 0.1))
    rust = smoothstep(0.3, 1.4, t.fbm(4, 5) + 0.3 * t.fbm(30, 3))
    img = mix(img, mul(fill(t, "#7a3a22"), 0.8 + 0.4 * rope), 0.6 * rust)
    for f, cover in ((90, 0.5), (40, 0.25)):
        hole, sh, _ = stones(t, f, cover, 0.35)
        img = mix(img, mul(fill(t, "#161211"), 1 + 0.5 * (1 - sh)), 0.85 * hole)
    img = mix(img, col("#6b6560"), 0.3 * speckle(t, t.n // 3, 0.04))                      # glassy glints
    return img


@texture(tile_m=6.0, rough=0.9)
def natural_sandstone_strata(t):
    """Layered red sandstone: beds of different thickness, inclined cross-bedding inside them, vertical joints."""
    rng = np.random.default_rng(t.seed())
    x, y = t.warp(3, 0.008, 3)
    hb = random_heights(rng, 13, 0.7)
    yb = np.concatenate([[0.0], np.cumsum(hb)])
    yy = np.mod(y + 0.008 * t.fbm(3, 2, fy=1, x=x, y=y), 1.0)
    bed = np.clip(np.searchsorted(yb, yy, side="right") - 1, 0, len(hb) - 1)
    v = (yy - yb[bed]) / hb[bed]
    slope = np.array([4.0, -6.0, 5.0, -3.0, 7.0, -5.0, 3.0, -4.0, 6.0, -7.0, 4.0, -3.0, 5.0])[bed]   # whole: periodic
    lam = np.sin((y * 24 + slope * x * 2) * 2 * np.pi + 1.2 * t.fbm(8, 2)) * 0.5 + 0.5
    base = pick(hash01(bed, t.seed()), "#b2643e", "#c47a4c", "#a8563a", "#cf8f5e", "#9c5a3a", "#d39d6f")
    rough = t.fbm(12, 5, gain=0.6)
    relief = 1 + 0.12 * rough - 0.8 * np.clip(cavity(rough, 0.006), 0, 0.3)
    img = mul(base, (0.9 + 0.1 * lam) * relief * grit(t, 0.16))
    img = mul(img, 1 - 0.15 * smoothstep(0.9, 0.98, lam))                                    # thin dark laminae
    img = mix(img, col("#5a3424"), 0.4 * speckle(t, 200, 0.06))                              # erosion pits
    img = mul(img, 1 - 0.3 * smoothstep(0.93, 0.98, t.ridged(6, 3, x=x, y=y)))             # cracks
    img = mul(img, 1 - 0.35 * smoothstep(0.02, 0.0, np.minimum(v * hb[bed], (1 - v) * hb[bed])))   # bedding planes
    grooves = smoothstep(0.55, 1.0, t.fbm(2, 3, fy=40, x=x, y=y))
    img = mul(img, 1 - 0.18 * grooves)
    J = np.mod(x * 5 + 0.1 * t.fbm(2, 2) + 0.5 * (bed % 2), 1.0)
    img = mul(img, 1 - 0.5 * smoothstep(0.01, 0.0, np.abs(J - 0.5)) * smoothstep(0.1, 0.7, t.fbm(4, 2)))
    img = mix(img, col("#5e3424"), 0.3 * smoothstep(0.6, 1.6, t.fbm(5, 4)))               # desert varnish
    return mul(img, streaks(t, 0.2, 16, 1))


@texture(tile_m=1.5, rough=0.6)
def natural_river_pebbles(t):
    """Smooth, rounded river pebbles of many stones packed together on wet sand."""
    x, y = t.warp(6, 0.006, 2)
    W = t.worley(11, jitter=0.85, x=x, y=y, rounded=0.012)
    e = W.edge - (0.0015 + 0.004 * t.rand(W.id))
    cov = smoothstep(-t.px, t.px, e)
    st = pick(t.rand(W.id), "#8c8781", "#6d6a66", "#a59d90", "#7b5f4f", "#c4bdb0", "#4f4c4a", "#8e7a62", "#9a9fa2")
    st = mul(st, (0.85 + 0.25 * t.rand(W.id)) * (1 + 0.08 * unit_noise(t, W.id, 6, 4)) * grit(t, 0.04))
    vein = smoothstep(0.95, 0.99, t.ridged(3, 2, x=x + t.rand(W.id), y=y + t.rand(W.id))) * (t.rand(W.id) < 0.3)
    st = mix(st, col("#e2ddd2"), 0.8 * vein)
    st = mix(st, col("#3a3836"), 0.4 * speckle(t, 300, 0.05) * (t.rand(W.id) < 0.4))
    dome = np.sqrt(smoothstep(0.0, 0.03, e))
    st = mul(st, 0.55 + 0.5 * dome)
    sand = mul(soil(t, ("#3e3428", "#4f4334", "#61533f", "#6f604a"), 12), 0.8)
    S = t.worley(34, jitter=0.85, x=x, y=y, rounded=0.004)
    es = S.edge - 0.0015
    small = mul(pick(t.rand(S.id), "#8c8781", "#6d6a66", "#a59d90", "#7b5f4f", "#4f4c4a"),
                (0.8 + 0.3 * t.rand(S.id)) * (0.55 + 0.45 * np.sqrt(smoothstep(0.0, 0.008, es))))
    sand = mix(sand, small, smoothstep(-t.px, t.px, es) * (t.rand(S.id) < 0.7))
    return mul(mix(sand, st, cov), stains(t, 3, 0.1))


@texture(tile_m=2.0, rough=0.25)
def natural_crystal(t):
    """A bed of amethyst-like crystal: sharp facets in violets and blues, bright edges, milky inner veils."""
    x, y = t.warp(4, 0.01, 2, x=t.x + 0.31, y=t.y + 0.17)
    W = t.worley(9, jitter=1.0, x=x, y=y)
    a = t.rand(W.id) * F32(2 * np.pi)
    grad = (W.dx * np.cos(a) + W.dy * np.sin(a)) * 9.0
    base = pick(t.rand(W.id), "#5b3a7a", "#6e4a91", "#4a3570", "#7d5aa3", "#3f4a86", "#8a6ab0")
    img = mul(base, np.clip(0.75 + 0.6 * grad, 0.35, 1.5) * (0.85 + 0.3 * t.rand(W.id)))
    veil = smoothstep(0.4, 1.5, unit_noise(t, W.id, 5, 4))
    img = mix(img, col("#c9b8dc"), 0.25 * veil)
    edge = smoothstep(0.003, 0.0, W.edge)
    img = mix(img, col("#e6dcf0"), 0.7 * edge)
    W2 = t.worley(22, jitter=1.0, x=x, y=y)
    img = mix(img, col("#d7cbe6"), 0.3 * smoothstep(0.0015, 0.0, W2.edge) * smoothstep(0.0, 1.0, t.fbm(4, 2)))
    matrix = smoothstep(1.1, 1.6, t.fbm(3, 4))
    return mix(img, mul(fill(t, "#3a3438"), grit(t, 0.2)), 0.85 * matrix)


@texture(tile_m=6.0, rough=0.85)
def natural_basalt_columns(t):
    """Tops of basalt columns: hexagonal and pentagonal stones, dark joints, some columns standing lower."""
    x, y = t.warp(5, 0.006, 3)
    W = t.worley(9, 10, jitter=0.35, stagger=0.5, x=x, y=y, rounded=0.002)
    gap = 0.004 + 0.002 * t.fbm(20, 2)
    e = W.edge - gap
    cov = smoothstep(-t.px, t.px, e)
    low = t.rand(W.id)
    st = mul(ramp(nrm(t.fbm(5, 4)), "#454340", "#55524e", "#67625c"), (0.7 + 0.45 * low) * grit(t, 0.1))
    st = mul(st, 1 + 0.08 * unit_noise(t, W.id, 6, 4))
    st = mul(st, bevel(e, 0.01, 0.35))
    cracks = smoothstep(0.95, 0.99, t.ridged(5, 3, x=x + t.rand(W.id), y=y + t.rand(W.id)))
    st = mul(st, 1 - 0.4 * cracks)
    lm, _, _ = lichen(t, 30, 0.2, 0.35)
    st = mix(st, col("#a9aa98"), 0.5 * lm)
    wet = smoothstep(0.2, 0.0, low) * smoothstep(0.004, 0.02, e)
    st = mix(st, col("#22201e"), 0.6 * wet)
    return mix(fill(t, "#141312"), st, cov)


# ---------------------------------------------------------------- fabric and leather
def weave(t, n, kind="plain", gap=0.12, slub=0.15):
    """Woven cloth with n threads per tile each way (n even for plain and basket, a multiple of 3 for twill,
    so the weave repeats across the tile). Returns (warp_shown, shade, warp index, weft index): which thread
    shows at each pixel, a 0..1 relief shading (round threads, floats dipping under the crossing threads, 0 in
    the holes between threads) and the thread indices (for colours)."""
    X, Y = t.x * n + 0.25, t.y * n + 0.25
    i, j = np.floor(X).astype(np.int64), np.floor(Y).astype(np.int64)
    fx, fy = X - i, Y - j
    if kind == "plain":
        top = (i + j) % 2 == 0
        warp_along, weft_along = fy, fx
    elif kind == "basket":
        top = ((i // 2) + (j // 2)) % 2 == 0
        warp_along, weft_along = (j % 2 + fy) / 2, (i % 2 + fx) / 2
    else:                                       # 2/1 twill: warp over two wefts, under one, stepping each thread
        m = (i - j) % 3
        top = m != 0
        warp_along, weft_along = ((2 - m) + fy) / 2, fx
    wi, we = i % n, j % n
    def thickness(along_coord, idx):
        return 1.0 + slub * t.noise(3, 40, x=along_coord + hash01(idx, 3), y=hash01(idx, 5) * 7)
    hw = 0.5 * (1 - gap) * thickness(t.y, wi)
    hf = 0.5 * (1 - gap) * thickness(t.x, we)
    dw, df = np.abs(fx - 0.5) / hw, np.abs(fy - 0.5) / hf
    in_w, in_f = dw < 1, df < 1
    show_w = in_w & (top | ~in_f)
    show_f = in_f & ~show_w
    prof = np.sqrt(np.clip(1 - np.where(show_w, dw, df) ** 2, 0, 1))
    float_end = np.sin(np.pi * np.clip(np.where(show_w, warp_along, weft_along), 0, 1)) ** 0.4
    dive = np.where(show_w, smoothstep(hf, hf + 0.25, np.abs(fy - 0.5)), smoothstep(hw, hw + 0.25, np.abs(fx - 0.5)))
    on_top = np.where(show_w, top, ~top)
    level = np.where(on_top, 0.6 + 0.4 * float_end, 0.25 + 0.5 * dive)
    shade = np.where(show_w | show_f, prof * level, 0.0)
    return show_w, shade.astype(F32), wi, we


def fibres(t, amount=0.1):
    """Fine fuzz: short random hairs over a fabric (a multiplier around 1)."""
    h1 = smoothstep(0.9, 0.97, t.ridged(40, 2, fy=6)) + smoothstep(0.9, 0.97, t.ridged(6, 2, fy=40))
    return (1.0 + amount * (h1 - 0.3) + 0.5 * amount * t.noise(t.n // 2)).astype(F32)


@texture(tile_m=0.5, rough=0.9)
def fabric_canvas(t):
    """Natural cotton canvas: a tight plain weave with uneven, slubby threads and faint soiling."""
    top, shade, wi, we = weave(t, 150, "plain", gap=0.1, slub=0.2)
    tone = np.where(top, hash01(wi, t.seed()), hash01(we, t.seed()))
    img = mul(fill(t, "#cfc4ad"), (0.92 + 0.12 * tone) * (0.62 + 0.42 * shade) * fibres(t, 0.05))
    img = mix(img, col("#9c8e74"), 0.3 * smoothstep(0.3, 1.5, t.fbm(3, 5)))
    return mul(img, stains(t, 2, 0.08))


@texture(tile_m=0.5, rough=0.95)
def fabric_burlap(t):
    """Coarse jute burlap: thick hairy threads in an open plain weave, dark gaps between them."""
    top, shade, wi, we = weave(t, 44, "plain", gap=0.3, slub=0.3)
    tone = np.where(top, hash01(wi, t.seed()), hash01(we, t.seed()))
    jute = mul(fill(t, "#9a7d55"), (0.85 + 0.25 * tone) * (1 + 0.12 * t.noise(t.n // 3)))
    img = mul(jute, (0.3 + 0.8 * shade) * fibres(t, 0.25))
    hole = shade < 0.01
    img = np.where(hole[..., None], mul(fill(t, "#2a2118"), 1 + 0.3 * t.noise(80)), img)
    return mul(img, stains(t, 2, 0.1))


@texture(tile_m=0.5, rough=0.95)
def fabric_wool_twill(t):
    """Heathered wool twill: diagonal ribs, dark green warp and lighter flecked weft, soft and fuzzy."""
    top, shade, wi, we = weave(t, 108, "twill", gap=0.08, slub=0.15)
    warp = mul(fill(t, "#34432f"), 0.9 + 0.2 * hash01(wi, t.seed()))
    weft = mul(fill(t, "#5f6a4c"), 0.9 + 0.2 * hash01(we, t.seed()))
    img = np.where(top[..., None], warp, weft)
    img = mix(img, col("#8c8a6e"), 0.35 * speckle(t, t.n // 3, 0.12))                       # heather flecks
    img = mul(img, (0.65 + 0.4 * shade) * fibres(t, 0.15))
    img = 0.6 * img + 0.4 * blur(img, 0.0012)                                             # fuzz softens it
    return mul(img, stains(t, 3, 0.06))


@texture(tile_m=0.5, rough=0.6)
def fabric_leather_brown(t):
    """Brown leather: pebbled grain of little raised cells, deeper creases, rubbed lighter areas, scratches."""
    x, y = t.warp(12, 0.003, 2)
    W = t.worley(48, jitter=0.9, x=x, y=y, rounded=0.002)
    pebble = np.sqrt(smoothstep(0.0, 0.006, W.edge))
    crease = smoothstep(0.9, 0.98, t.ridged(4, 4, x=x, y=y))
    base = mul(ramp(nrm(t.fbm(3, 4) + 0.3 * t.fbm(20, 3)), "#4a2b18", "#5e3820", "#704428", "#7e5131"), 1.0)
    img = mul(base, (0.6 + 0.45 * pebble) * (1 - 0.5 * crease) * (1 + 0.05 * t.noise(t.n // 3)))
    rub = smoothstep(0.5, 1.6, t.fbm(3, 4)) * pebble
    img = mix(img, mul(img, 1.5), 0.45 * rub)
    scratch = smoothstep(0.965, 0.99, t.ridged(2, 2, x=t.x + t.y, y=t.x - t.y)) * smoothstep(0.3, 1.0, t.fbm(6, 2))
    return mix(img, col("#9c7656"), 0.5 * scratch)


@texture(tile_m=1.0, rough=0.7)
def fabric_quilted(t):
    """Quilted satin: diamond-stitched puffs in deep red, stitch lines in cream thread, soft sheen."""
    k = 7
    u, v = (t.x + t.y) * k, (t.x - t.y) * k
    fu, fv = u - np.floor(u), v - np.floor(v)
    du, dv = np.minimum(fu, 1 - fu), np.minimum(fv, 1 - fv)
    e = np.minimum(du, dv)
    puff = np.sqrt(smoothstep(0.0, 0.5, e))
    base = mul(fill(t, "#7a1e22"), (0.45 + 0.65 * puff) * (1 + 0.05 * t.noise(4, 400, x=t.x, y=t.y)))
    sheen = smoothstep(0.25, 0.5, e) * smoothstep(0.2, 1.0, t.fbm(3, 3))
    img = mix(base, col("#b84850"), 0.25 * sheen)
    along = np.where(du < dv, fv, fu)
    dash = (np.mod(along * 14, 1.0) < 0.6)
    stitch = smoothstep(0.03, 0.012, e) * dash
    img = mix(img, col("#d9c9a8"), 0.85 * stitch)
    return mul(img, stains(t, 3, 0.06))


@texture(tile_m=0.6, rough=0.9)
def fabric_tartan(t):
    """A tartan wool: green, navy, black and red sett woven in twill, so the colours cross and mix."""
    sett = [("#28395c", 10), ("#222224", 2), ("#28395c", 2), ("#222224", 2), ("#2a5534", 12), ("#a8302c", 2),
            ("#2a5534", 12), ("#222224", 2), ("#28395c", 2), ("#cdbd80", 1), ("#28395c", 1)]
    seq = np.concatenate([[k] * w for k, (_, w) in enumerate(sett)])
    n = len(seq) * 4
    cols = np.array([col(c) for c, _ in sett], F32)
    top, shade, wi, we = weave(t, n, "twill", gap=0.06, slub=0.1)
    warp = cols[seq[wi % len(seq)]]
    weft = cols[seq[we % len(seq)]]
    img = np.where(top[..., None], warp, weft)
    img = mul(img, (0.7 + 0.38 * shade) * fibres(t, 0.12))
    return mul(0.75 * img + 0.25 * blur(img, 0.001), stains(t, 3, 0.06))


@texture(tile_m=0.4, rough=0.95)
def fabric_knit(t):
    """Chunky stockinette knit in oatmeal wool: columns of V-shaped stitches."""
    cols, rows = 20, 28
    X, Y = t.x * cols, t.y * rows
    i, j = np.floor(X), np.floor(Y)
    lx, ly = X - i - 0.5, Y - j - 0.5
    best = np.zeros(t.x.shape, F32)
    for side in (-1.0, 1.0):
        for dy in (-1.0, 0.0, 1.0):
            px, py = lx - side * 0.22, (ly - dy) * (cols / rows) * 1.15
            a = side * 0.55
            u = px * np.cos(a) - py * np.sin(a)
            v = px * np.sin(a) + py * np.cos(a)
            d = (u / 0.2) ** 2 + (v / 0.42) ** 2
            best = np.maximum(best, np.sqrt(np.clip(1 - d, 0, 1)))
    base = mul(fill(t, "#cbbda2"), (0.9 + 0.15 * hash01((np.mod(i, cols) * 977 + np.mod(j, rows)).astype(np.int64), 3)))
    img = mul(base, (0.3 + 0.8 * best) * fibres(t, 0.12) * (1 + 0.06 * t.noise(t.n // 3)))
    img = mix(img, col("#8d7f68"), 0.25 * speckle(t, t.n // 4, 0.08))
    return mul(img, stains(t, 3, 0.06))


@texture(tile_m=0.5, rough=0.85)
def fabric_ticking_stripes(t):
    """Linen ticking: navy stripes of different widths on a cream ground, in a fine plain weave."""
    n = 192
    top, shade, wi, we = weave(t, n, "plain", gap=0.08, slub=0.15)
    pattern = np.zeros(48, bool)
    pattern[[2, 3, 4, 5, 6, 7, 10, 11, 14, 15, 16, 17, 18, 19]] = True
    navy = pattern[wi % 48] & top
    ground = mul(fill(t, "#d9d0bc"), 0.92 + 0.1 * np.where(top, hash01(wi, t.seed()), hash01(we, t.seed())))
    img = np.where(navy[..., None], mul(fill(t, "#253556"), 0.9 + 0.15 * hash01(wi, t.seed())), ground)
    img = mul(img, (0.68 + 0.36 * shade) * fibres(t, 0.05))
    img = mix(img, col("#a39a86"), 0.25 * smoothstep(0.3, 1.5, t.fbm(3, 4)))
    return img


@texture(tile_m=1.0, rough=0.5)
def fabric_dragon_hide(t):
    """Dragon hide: overlapping keeled scales in iridescent green and teal, darker at the edges."""
    cols, rows = 14, 20
    X, Y = t.x * cols + 0.25, (t.y + 0.011) * rows
    r0 = np.floor(Y).astype(np.int64)

    def row(r):
        off = (r % 2) * 0.5
        c = np.floor(X - off).astype(np.int64)
        s = (X - off - c) * 2 - 1
        bottom = r + 1.35 - 0.9 * np.abs(s) ** 1.5             # pointed scales
        return (r % rows) * cols + c % cols, s, bottom

    rws = [row(r0 + k) for k in (-1, 0, 1)]
    ids, ss, bs = zip(*rws)
    on0 = Y <= bs[0]
    on1 = (~on0) & (Y <= bs[1])
    sid = np.where(on0, ids[0], np.where(on1, ids[1], ids[2]))
    s = np.where(on0, ss[0], np.where(on1, ss[1], ss[2]))
    above = np.where(on0, bs[0] - Y, np.where(on1, bs[1] - Y, bs[2] - Y))
    keel = smoothstep(0.25, 0.0, np.abs(s)) * smoothstep(1.3, 0.3, above)
    tone = hash01(sid, t.seed())
    base = ramp(np.clip(0.3 + 0.5 * tone + 0.25 * t.fbm(2, 3), 0, 1),
                "#173a2e", "#225440", "#2d6a5a", "#3f7f6a", "#5a8c58")
    img = mul(base, (0.55 + 0.45 * smoothstep(0.0, 0.5, above)) * (1 - 0.35 * smoothstep(0.6, 1.0, np.abs(s))))
    img = mul(img, (1 + 0.35 * keel) * (1 + 0.06 * t.noise(t.n // 4)))
    img = mix(img, col("#c8b56a"), 0.15 * smoothstep(0.08, 0.0, above))                    # pale scale tips
    return mul(img, stains(t, 3, 0.1))


@texture(tile_m=0.8, rough=0.95)
def fabric_fur_pelt(t):
    """An animal pelt: dense hairs lying in gently curving tracts, dark roots between lighter tips, tufts."""
    x, y = t.warp(2, 0.035, 2)
    x, y = t.warp(8, 0.004, 2, x=x, y=y)
    hair = 0.55 * t.noise(300, 5, x=x, y=y) + 0.45 * t.noise(600, 9, x=x, y=y) + 0.35 * t.noise(120, 3, x=x, y=y)
    tufts = t.fbm(10, 3, fy=4, x=x, y=y)
    v = np.clip(0.5 + 0.2 * hair + 0.13 * tufts, 0, 1)
    img = ramp(v, "#24170e", "#4a301e", "#74502f", "#9c774f", "#bb9c74")
    return mul(img, 1 + 0.15 * t.fbm(3, 3))


# ---------------------------------------------------------------- plaster, render and concrete
def trowel(t, amount=0.05, f=4):
    """Trowel marks: broad, overlapping smoothing strokes with slightly sharper edges (a multiplier around 1)."""
    x, y = t.warp(2, 0.04, 2)
    s = t.noise(f, f * 3, x=x + y, y=x - y) + 0.7 * t.noise(f * 2, f * 4, x=x - y, y=x + y)
    s = np.tanh(1.5 * s)
    return (1.0 + amount * s).astype(F32)


def hairline_cracks(t, f=6, width=0.0012, amount=0.5):
    x, y = t.warp(8, 0.01, 3)
    r = t.ridged(f, 4, x=x, y=y)
    return (1.0 - amount * smoothstep(0.985 - width * 20, 0.995, r) * smoothstep(0.2, 0.9, t.fbm(4, 2))).astype(F32)


@texture(tile_m=3.0, rough=0.9)
def plaster_lime(t):
    """Limewashed lime plaster: chalky off-white, soft trowel marks, faint mottling, hairline cracks."""
    img = ramp(nrm(t.fbm(3, 5) + 0.4 * t.fbm(16, 3)), "#bdb5a5", "#cbc4b6", "#d7d1c4")
    img = mul(img, trowel(t) * grit(t, 0.03) * hairline_cracks(t))
    img = mix(img, col("#a39b8a"), 0.25 * smoothstep(0.5, 1.6, t.fbm(4, 5)))
    return mul(img, streaks(t, 0.12, 24, 1) * stains(t, 2, 0.06))


@texture(tile_m=3.0, rough=0.9)
def plaster_cracked_render(t):
    """Cream render cracked and fallen away in patches, showing the brickwork beneath."""
    L = bond(t.x, t.y, 30, 10)
    brick = brick_faces(t, L, ("#94503a", "#a35c42", "#874834", "#ad6a4c"))
    under = brick_wall(t, L, brick, "#8f877a", chip=1.5)
    render = mul(ramp(nrm(t.fbm(3, 5)), "#bdb39c", "#cbc2ac", "#d6cebb"), trowel(t, 0.05) * grit(t, 0.04))
    render = mul(render, hairline_cracks(t, 5, amount=0.6))
    loss = t.fbm(3, 6, gain=0.6) + 0.3 * t.fbm(20, 3)
    thr = float(np.quantile(loss, 0.72))
    keep = smoothstep(thr + 0.02, thr - 0.02, loss)
    edge = smoothstep(thr - 0.1, thr, loss) * keep
    img = mix(mul(under, 1 - 0.4 * smoothstep(thr + 0.12, thr, loss)), mul(render, 1 - 0.3 * edge), keep)
    return mul(img, streaks(t, 0.18, 24, 1) * stains(t, 2, 0.12))


@texture(tile_m=3.0, rough=0.85)
def plaster_concrete_formwork(t):
    """Board-formed concrete: the imprint of horizontal planks, form-tie holes in a grid, small air holes."""
    rng = np.random.default_rng(t.seed())
    rows = 16
    L = courses(t.x, t.y, [1 / rows] * rows, random_widths(rng, rows, 0.5, 0.4, 0.5), rng.random(rows))
    g = wood_grain(t, L.u * L.w, L.v * L.h, L.id, rings=10, fibres=0.3)
    img = mul(ramp(nrm(t.fbm(3, 5)), "#7f7d79", "#8e8c88", "#9c9a95"), (0.9 + 0.14 * t.rand(L.id)) * (1 - 0.13 * g))
    img = mul(img, 1 - 0.3 * smoothstep(0.0025, 0.0, L.edge))
    tx, ty = np.mod(t.x * 4 + 0.5, 1.0) - 0.5, np.mod(t.y * 4 + 0.5, 1.0) - 0.5
    td = np.hypot(tx, ty) / 4
    img = mul(img, (1 - 0.6 * smoothstep(0.008, 0.006, td)) * (1 + 0.1 * smoothstep(0.016, 0.008, td) * (td > 0.008)))
    img = mix(img, col("#4a4846"), 0.6 * speckle(t, t.n // 4, 0.02))                        # bug holes
    img = mul(img, grit(t, 0.05))
    return mul(img, streaks(t, 0.2, 30, 1) * stains(t, 2, 0.12))


@texture(tile_m=3.0, rough=0.95)
def plaster_adobe(t):
    """Mud render on an adobe wall: warm ochre, hand-smoothed swirls, straw flecks, rain-cut channels."""
    img = ramp(nrm(t.fbm(3, 5) + 0.4 * t.fbm(14, 4)), "#8e6a47", "#a8845e", "#bb9a72")
    x, y = t.warp(5, 0.04, 3)
    swirl = t.noise(6, 6, x=x, y=y) + 0.5 * t.noise(14, 14, x=x, y=y)
    img = mul(img, (1 + 0.08 * np.tanh(2 * swirl)) * grit(t, 0.12))
    pit, sh, pid = stones(t, 80, 0.2, 0.35)
    img = mix(img, mul(pick(t.rand(pid), "#7d6a55", "#9a8670", "#5f5244"), 0.6 + 0.5 * sh), 0.8 * pit)
    sx, sy = t.warp(80, 0.002, 2)
    straw = np.maximum(smoothstep(0.86, 0.94, t.ridged(30, 2, fy=6, x=sx, y=sy)),
                       smoothstep(0.86, 0.94, t.ridged(6, 2, fy=30, x=sy, y=sx)))
    img = mix(img, col("#cdb27c"), 0.6 * straw * smoothstep(0.0, 0.8, t.fbm(10, 2)))
    chan = smoothstep(0.85, 0.97, t.ridged(10, 3, fy=2)) * smoothstep(0.3, 1.0, t.fbm(3, 2))
    img = mul(img, (1 - 0.25 * chan) * hairline_cracks(t, 4, amount=0.4))
    cx, cy = t.warp(8, 0.008, 3)
    C = t.worley(9, jitter=0.9, x=cx, y=cy)
    shrink = smoothstep(0.0022, 0.0006, C.edge) * smoothstep(-0.2, 0.7, t.fbm(4, 3))      # shrinkage cracks
    img = mul(img, (1 - 0.55 * shrink) * (1 + 0.1 * smoothstep(0.008, 0.002, C.edge) * (1 - shrink)))
    eroded = smoothstep(0.7, 1.0, t.fbm(3, 5) + 0.3 * t.fbm(24, 3))
    img = mix(img, mul(ramp(nrm(t.noise(t.n // 3)), "#6e5238", "#8f6d4b", "#a98a62"), 1.0 - 0.2 * straw), 0.8 * eroded)
    return mul(img, stains(t, 2, 0.12))


@texture(tile_m=2.0, rough=0.85)
def plaster_stucco_knockdown(t):
    """Knock-down stucco painted warm cream: flattened splatter islands with soft shadowed rims."""
    n = t.fbm(14, 4, gain=0.6) + 0.35 * t.fbm(60, 2)
    thr = float(np.quantile(n, 0.55))
    flat = smoothstep(thr, thr + 0.06, n)
    rim = smoothstep(thr - 0.08, thr, n) * (1 - flat)
    img = mul(fill(t, "#cfbd94"), (0.88 + 0.12 * flat) * (1 - 0.25 * rim) * grit(t, 0.05))
    img = mul(img, 1 + 0.04 * t.fbm(3, 3))
    return mul(img, streaks(t, 0.1, 24, 1) * stains(t, 2, 0.08))


@texture(tile_m=1.5, rough=0.95)
def plaster_pebbledash(t):
    """Pebbledash render: small rounded pebbles in browns, greys and white thrown into grey-brown render."""
    img = mul(fill(t, "#7a7266"), (1 + 0.1 * t.fbm(10, 3)) * grit(t, 0.1))
    for f, cover in ((110, 0.6), (70, 0.6), (44, 0.7)):         # big pebbles last, on top
        W = t.worley(f, jitter=0.9, rounded=0.15 / f)
        e = W.edge - 0.08 / f
        m = smoothstep(-t.px, t.px, e) * (t.rand(W.id) < cover)
        st = pick(t.rand(W.id), "#8c7b64", "#a4927a", "#6e6a64", "#c8c0b2", "#5e5348", "#9c8f80", "#b3a48c")
        st = mul(st, (0.8 + 0.3 * t.rand(W.id)) * (0.5 + 0.55 * np.sqrt(smoothstep(0, 0.3 / f, e))))
        img = mix(mul(img, 1 - 0.3 * m), st, m)
    return mul(img, streaks(t, 0.15, 24, 1) * stains(t, 2, 0.12))


@texture(tile_m=4.0, rough=0.85)
def plaster_half_timber(t):
    """Half-timbering: dark oak posts, rails and braces framing panels of white lime plaster."""
    cols, rows, bw = 4, 2, 0.018
    X, Y = t.x * cols + 0.5, t.y * rows + 0.25
    i, j = np.floor(X), np.floor(Y)
    u, v = X - i, Y - j
    dpost = np.minimum(u, 1 - u) / cols
    drail = np.minimum(v, 1 - v) / rows
    brace = hash01((np.mod(i, cols) * 7 + np.mod(j, rows)).astype(np.int64), t.seed())
    dirn = np.where(brace < 0.5, 1.0, -1.0)
    pu, pv = u / cols, v / rows                         # position in the panel, tile units
    lx, ly = 1.0 / cols, 1.0 / rows
    dbrace = np.abs(pu * ly - np.where(dirn > 0, pv, ly - pv) * lx) / math.hypot(lx, ly)
    has = brace < 0.7
    post, rail = dpost < bw, drail < bw
    br = has & (dbrace < bw * 0.85) & ~post & ~rail
    beam = post | rail | br
    along = np.where(post, pv, np.where(rail, pu, (pu + pv * dirn)))
    across = np.where(post, dpost, np.where(rail, drail, dbrace))
    bid = np.where(post, np.mod(i, cols) * 2, np.where(rail, 100 + np.mod(j, rows) * 10 + np.mod(i, cols),
                                                         200 + np.mod(i, cols) * 7 + np.mod(j, rows))).astype(np.int64)
    g = wood_grain(t, along, across, bid, rings=6, fibres=0.3)
    wood = mul(ramp(g, "#4a3524", "#3a2a1c", "#22180f"), (0.85 + 0.25 * hash01(bid, t.seed())) * grit(t, 0.06))
    edge_d = np.where(post, bw - dpost, np.where(rail, bw - drail, bw * 0.85 - dbrace))
    wood = mul(wood, 0.6 + 0.4 * smoothstep(0.0, 0.006, edge_d))
    plaster = mul(ramp(nrm(t.fbm(3, 4)), "#c6bfaf", "#d2cbbb", "#dbd5c8"), trowel(t, 0.05) * grit(t, 0.03))
    near = np.minimum(np.minimum(dpost, drail), np.where(has, dbrace, 1.0))
    plaster = mul(plaster, (1 - 0.3 * smoothstep(bw + 0.012, bw, near)) * hairline_cracks(t, 6, amount=0.4))
    img = np.where(beam[..., None], wood, plaster)
    return mul(img, stains(t, 2, 0.12) * streaks(t, 0.12, 24, 1))


@texture(tile_m=2.4, rough=0.9)
def plaster_cinder_blocks(t):
    """Concrete blocks (cinder blocks) in running bond: porous grey faces, crisp recessed joints."""
    L = bond(t.x, t.y, 12, 6)
    face = mul(pick(t.rand(L.id), "#8a8884", "#84827e", "#908e89", "#7e7c78"),
               (0.93 + 0.1 * t.rand(L.id)) * (1 + 0.05 * unit_noise(t, L.id, 6, 4)) * grit(t, 0.12))
    face = mix(face, col("#4a4947"), 0.7 * speckle(t, t.n // 3, 0.07))                       # pores
    face = mix(face, col("#a8a59e"), 0.4 * speckle(t, t.n // 4, 0.06))
    img = brick_wall(t, L, face, "#9a978f", joint=0.004, chip=0.6, depth=0.25, recess=0.75)
    return mul(img, streaks(t, 0.15, 30, 1) * stains(t, 2, 0.12))


@texture(tile_m=2.0, rough=0.85)
def plaster_painted_peeling(t):
    """Faded ochre-pink paint on old plaster, crazed and peeling in curled flakes to the grey plaster under it."""
    under = mul(ramp(nrm(t.fbm(4, 4)), "#9c968c", "#b1aba0", "#c2bdb2"), grit(t, 0.05) * hairline_cracks(t, 7))
    paint = mul(ramp(nrm(t.fbm(3, 5)), "#b37e5c", "#c48f6a", "#d3a27e"), trowel(t, 0.04) * grit(t, 0.03))
    x, y = t.warp(10, 0.006, 3)
    W = t.worley(22, jitter=0.9, x=x, y=y)
    craze = smoothstep(0.0016, 0.0, W.edge)
    paint = mul(paint, 1 - 0.2 * craze)
    loss = t.fbm(4, 6, gain=0.6) + 0.6 * (t.rand(W.id) - 0.5)
    thr = float(np.quantile(loss, 0.68))
    keep = smoothstep(thr + 0.02, thr - 0.02, loss)
    lift = smoothstep(thr - 0.12, thr, loss) * keep
    img = mix(mul(under, 1 - 0.3 * smoothstep(thr + 0.1, thr, loss)), mul(paint, 1 - 0.3 * lift), keep)
    return mul(img, streaks(t, 0.18, 24, 1) * stains(t, 2, 0.12))


@texture(tile_m=3.0, rough=0.92)
def plaster_concrete_weathered(t):
    """Old weathered concrete: exposed aggregate, patched repairs, rust runs from rebar, dark water streaks."""
    img = mul(ramp(nrm(t.fbm(3, 5) + 0.3 * t.fbm(20, 3)), "#6e6c68", "#7f7c77", "#8f8b84"), grit(t, 0.1))
    W = t.worley(48, jitter=0.9, rounded=0.003)
    agg = smoothstep(0.0, 0.002, W.edge - 0.003) * (t.rand(W.id) < 0.6) * smoothstep(-0.3, 0.6, t.fbm(6, 3))
    img = mix(img, mul(pick(t.rand(W.id), "#8f8676", "#a39a8a", "#6e6a62", "#b1a99a"), 0.8 + 0.3 * t.rand(W.id)), agg)
    L = grid_units(t.x, t.y, 5, 4)
    patch = (t.rand(L.id) < 0.18) & (L.edge > 0.012 + 0.006 * t.fbm(30, 2))
    img = np.where(patch[..., None], mul(fill(t, "#97948d"), grit(t, 0.05) * (1 + 0.05 * t.fbm(8, 3))), img)
    rx = np.mod(t.x * 7 + 0.3, 1.0)
    rust = smoothstep(0.03, 0.0, np.abs(rx - 0.5) - 0.01 * t.fbm(30, 2, fy=4)) * smoothstep(0.2, 1.2, t.fbm(3, 3, fy=1))
    img = mix(img, col("#7a4a2a"), 0.55 * rust * smoothstep(0.0, 1.0, t.fbm(12, 3, fy=2)))
    img = mul(img, streaks(t, 0.35, 28, 1) * hairline_cracks(t, 5, amount=0.6))
    algae = smoothstep(0.5, 1.5, t.fbm(4, 4) + 0.5 * t.fbm(28, 3, fy=2))
    img = mix(img, mul(fill(t, "#2f3329"), 1 + 0.2 * t.noise(t.n // 4)), 0.55 * algae)
    return mul(img, stains(t, 2, 0.15))


# ---------------------------------------------------------------- floors and tiles
def marble(t, base, vein, f=3, strength=0.8, x=None, y=None, ids=None):
    """Veined marble: the zero lines of a strongly warped noise give long flowing veins with a soft halo,
    plus a second, finer network fading in and out, over a softly clouded base."""
    x, y = t._xy(x, y)
    if ids is not None:
        x, y = x + hash01(ids, t.seed()) * 5, y + hash01(ids, t.seed()) * 5
    wx, wy = t.warp(f, 0.12, 5, x=x, y=y)
    n1 = t.noise(f, x=wx, y=wy)
    n2 = t.noise(f * 3, x=wx, y=wy)
    fade = smoothstep(-0.8, 0.8, t.fbm(f, 3, x=wx, y=wy))
    v = (smoothstep(0.045, 0.0, np.abs(n1)) + 0.25 * smoothstep(0.35, 0.0, np.abs(n1))) * (0.35 + 0.65 * fade)
    v = v + 0.5 * smoothstep(0.03, 0.0, np.abs(n2)) * smoothstep(0.0, 1.0, t.fbm(f, 2, x=wx, y=wy))
    img = mul(base, 1 + 0.06 * t.fbm(f * 2, 4, x=wx, y=wy))
    return mix(img, vein, np.clip(v * strength, 0, 1))


@texture(tile_m=2.0, rough=0.7)
def floor_terracotta(t):
    """Square terracotta floor tiles (cotto): warm uneven colour, worn edges, grey grout."""
    L = grid_units(t.x, t.y, 6)
    face = mul(pick(t.rand(L.id), "#b5653f", "#a85a38", "#c0714a", "#9c5235", "#b86c48", "#a9603e"),
               (0.88 + 0.2 * t.rand(L.id)) * (1 + 0.1 * unit_noise(t, L.id, 5, 5)) * grit(t, 0.06))
    face = mix(face, col("#7a3f28"), 0.4 * smoothstep(0.3, 1.4, unit_noise(t, L.id, 3, 3)))   # firing clouds
    face = mix(face, col("#e0b48c"), 0.3 * speckle(t, t.n // 4, 0.05))
    wear = smoothstep(0.4, 1.4, t.fbm(3, 4))
    face = mix(face, mul(face, 1.2), 0.4 * wear)
    cov, e = joints(t, L, 0.004, chip=1.5, chip_f=40)
    face = mul(face, bevel(e - 0.004, 0.006, 0.3))
    return mul(mix(mortar(t, "#7d776d", 0.2), face, cov), stains(t, 3, 0.1))


@texture(tile_m=2.4, rough=0.25)
def floor_checker_marble(t):
    """Black and white marble laid as a checkerboard, each slab with its own veining."""
    n = 6
    L = grid_units(t.x, t.y, n)
    dark = (L.row + L.k) % 2 == 0
    white = marble(t, mul(fill(t, "#d8d6d0"), 0.95 + 0.08 * t.rand(L.id)), col("#8f8d88"), 3, 0.7, ids=L.id)
    black = marble(t, mul(fill(t, "#2a2a2c"), 0.9 + 0.15 * t.rand(L.id)), col("#8a8884"), 3, 0.45, ids=L.id)
    img = np.where(dark[..., None], black, white)
    cov, _ = joints(t, L, 0.0012)
    return mix(fill(t, "#5a5852"), img, cov)


@texture(tile_m=1.5, rough=0.6)
def floor_mosaic(t):
    """A Roman-style mosaic: small hand-cut tesserae following a meander border and rosette pattern."""
    x, y = t.warp(20, 0.0015, 2, x=t.x + 0.5 / 96, y=t.y + 0.5 / 96)     # grout lines off the tile's edge
    W = t.worley(96, jitter=0.35, x=x, y=y)
    cx, cy = np.mod(t.x - W.dx, 1.0), np.mod(t.y - W.dy, 1.0)         # each tessera's centre decides its colour
    u, v = np.mod(cx * 2 + 0.2, 1.0), np.mod(cy * 2 + 0.2, 1.0)        # 2 x 2 repeats of a motif
    du, dv = np.abs(u - 0.5), np.abs(v - 0.5)
    ring = np.maximum(du, dv)
    diamond = du + dv
    circle = np.hypot(du, dv)
    key = np.zeros(t.x.shape, np.int64)
    key = np.where((ring > 0.44), 1, key)                              # dark border band
    key = np.where((ring > 0.36) & (ring < 0.40), 2, key)              # red line
    key = np.where(diamond < 0.3, 3, key)
    key = np.where(diamond < 0.22, 4, key)
    key = np.where(circle < 0.1, 2, key)
    pal = np.array([col(c) for c in ("#d9cfb9", "#3b3632", "#9a3b2a", "#5d6b4a", "#c7a24e")], F32)
    tess = pal[key] * (0.85 + 0.25 * t.rand(W.id))[..., None]
    tess = mul(tess, grit(t, 0.05) * (1 + 0.06 * unit_noise(t, W.id, 8, 2)))
    cov, e = joints(t, W.edge, 0.0012, chip=1.0, chip_f=80)
    tess = mul(tess, bevel(e - 0.0012, 0.002, 0.25))
    return mul(mix(mortar(t, "#8a8275", 0.2), tess, cov), stains(t, 3, 0.12))


@texture(tile_m=1.2, rough=0.4)
def floor_hex_tiles(t):
    """Small hexagonal ceramic tiles, mostly white with scattered black ones forming flowers, grey grout."""
    W = t.worley(16, 18, jitter=0.0, stagger=0.5, x=t.x + 0.3 / 16, y=t.y + 0.2 / 18)
    r = t.rand(W.id)
    black = r < 0.12
    face = np.where(black[..., None], mul(fill(t, "#2b2b2d"), 0.9 + 0.2 * t.rand(W.id)),
                    mul(fill(t, "#d7d4cc"), 0.94 + 0.08 * t.rand(W.id)))
    face = mul(face, grit(t, 0.02) * (1 + 0.04 * unit_noise(t, W.id, 3, 2)))
    cov, e = joints(t, W.edge, 0.0016)
    face = mul(face, bevel(e - 0.0016, 0.002, 0.2))
    g = mortar(t, "#8e8a82", 0.15)
    img = mix(g, face, cov)
    return mul(img, 1 - 0.12 * smoothstep(0.3, 1.5, t.fbm(3, 4)))


@texture(tile_m=3.6, rough=0.15)
def floor_polished_marble(t):
    """Large polished slabs of warm Carrara-like marble with flowing grey veins and fine joints."""
    L = grid_units(t.x, t.y, 3)
    base = mul(ramp(nrm(t.fbm(2, 5)), "#bdb8ae", "#cbc6bd", "#d4d0c8"), 0.97 + 0.05 * t.rand(L.id))
    img = marble(t, base, col("#7a746c"), 2, 0.85, ids=L.id)
    img = marble(t, img, col("#b6aea2"), 5, 0.4, ids=L.id)
    cov, _ = joints(t, L, 0.0008)
    return mix(fill(t, "#a8a298"), img, cov)


@texture(tile_m=2.4, rough=0.75)
def floor_slate_tiles(t):
    """Riven slate floor tiles in mixed rust, grey and green, laid in a broken bond with dark grout."""
    rng = np.random.default_rng(t.seed())
    L = courses(t.x, t.y, [1 / 6] * 6, random_widths(rng, 6, 0.22, 0.4, 0.6), rng.random(6))
    face = pick(t.rand(L.id), "#55595b", "#4a4d4f", "#5e5a52", "#6a5a4a", "#4e5a52", "#615e58")
    riven = unit_noise(t, L.id, 5, 5, gain=0.6)
    face = mul(face, (0.88 + 0.2 * t.rand(L.id)) * (1 + 0.12 * riven - 0.6 * np.clip(cavity(riven, 0.006), 0, 0.3)))
    rust = smoothstep(0.3, 1.2, unit_noise(t, L.id, 4, 4)) * (t.rand(L.id) < 0.5)
    face = mix(face, col("#8a5a36"), 0.5 * rust)
    face = mul(face, grit(t, 0.05))
    cov, e = joints(t, L, 0.004, chip=2.0, chip_f=30)
    face = mul(face, bevel(e - 0.004, 0.004, 0.3))
    return mul(mix(mortar(t, "#3f3d3a", 0.2), face, cov), stains(t, 3, 0.1))


@texture(tile_m=1.6, rough=0.55)
def floor_encaustic(t):
    """Victorian encaustic cement tiles: a quatrefoil and star motif in terracotta, navy, ochre and cream."""
    n = 6
    X, Y = t.x * n + 0.5, t.y * n + 0.5
    i, j = np.floor(X), np.floor(Y)
    u, v = X - i - 0.5, Y - j - 0.5
    au, av = np.abs(u), np.abs(v)
    r = np.hypot(u, v)
    ang = np.arctan2(v, u)
    petal = r < 0.3 + 0.08 * np.cos(4 * ang)
    star = (au + av) < 0.18
    corner = np.hypot(au - 0.5, av - 0.5) < 0.2
    band = (np.maximum(au, av) > 0.44)
    key = np.full(t.x.shape, 0, np.int64)                    # cream ground
    key = np.where(band, 1, key)
    key = np.where(petal, 2, key)
    key = np.where(petal & (r < 0.2 + 0.04 * np.cos(4 * ang)), 0, key)
    key = np.where(star, 3, key)
    key = np.where(corner, 4, key)
    key = np.where(corner & (np.hypot(au - 0.5, av - 0.5) < 0.1), 3, key)
    pal = np.array([col(c) for c in ("#d6cbb2", "#2f3b52", "#a2482f", "#c9963e", "#2f3b52")], F32)
    img = pal[key]
    tid = (np.mod(i, n) * n + np.mod(j, n)).astype(np.int64)
    img = mul(img, (0.92 + 0.12 * hash01(tid, t.seed())) * grit(t, 0.04) * (1 + 0.05 * t.fbm(10, 3)))
    wear = smoothstep(0.3, 1.4, t.fbm(3, 4) + 0.3 * t.fbm(20, 3))
    img = mix(img, mul(fill(t, "#b8ab92"), grit(t, 0.08)), 0.35 * wear)
    e = (0.5 - np.maximum(au, av)) / n
    cov = smoothstep(0.0012, 0.002, e)
    return mul(mix(fill(t, "#6d675c"), img, cov), stains(t, 3, 0.08))


@texture(tile_m=2.0, rough=0.45)
def floor_octagon_dot(t):
    """Octagon-and-dot floor: large pale limestone octagons with small black squares set between them."""
    n = 5
    X, Y = t.x * n + 0.5, t.y * n + 0.5
    i, j = np.floor(X), np.floor(Y)
    u, v = X - i - 0.5, Y - j - 0.5
    au, av = np.abs(u), np.abs(v)
    cut = 0.78                                   # where the octagon's corner is cut off for the dot
    oct_d = np.minimum(0.5 - np.maximum(au, av), (cut - (au + av)) / math.sqrt(2)) / n
    dot = (au + av) > cut
    dot_d = (((au + av) - cut) / math.sqrt(2)) / n
    tid = (np.mod(i, n) * n + np.mod(j, n)).astype(np.int64)
    stone = marble(t, mul(fill(t, "#d2c9b6"), 0.94 + 0.08 * hash01(tid, t.seed())), col("#b3a68f"), 3, 0.5, ids=tid)
    stone = mul(stone, grit(t, 0.03))
    black = mul(fill(t, "#2a2928"), (1 + 0.1 * t.fbm(30, 3)))
    e = np.where(dot, dot_d, oct_d)
    img = np.where(dot[..., None], black, stone)
    cov = smoothstep(0.001, 0.0018, e)
    img = mix(fill(t, "#8c857a"), img, cov)
    return mul(img, 1 - 0.1 * smoothstep(0.4, 1.5, t.fbm(3, 4)))


@texture(tile_m=4.0, rough=0.8)
def floor_dungeon_flagstones(t):
    """Worn dungeon flagstones of mixed rectangular sizes, cracked and dirty, with dark earth in the joints."""
    rng = np.random.default_rng(t.seed())
    L = courses(t.x, t.y, random_heights(rng, 5, 0.4), random_widths(rng, 5, 0.28, 0.5, 0.5), rng.random(5))
    e = round_corners(L, 0.01)
    base = pick(t.rand(L.id), "#6f6b64", "#7a756c", "#64615c", "#827b70", "#6b665d")
    face = mul(base, (0.85 + 0.25 * t.rand(L.id)) * (1 + 0.1 * unit_noise(t, L.id, 4, 5)) * grit(t, 0.08))
    face = mix(face, col("#4a463f"), 0.4 * speckle(t, 150, 0.06))
    worn = smoothstep(0.0, 0.05, e) * smoothstep(0.0, 1.2, t.fbm(3, 4))
    face = mix(face, mul(face, 1.2), 0.4 * worn)
    crack = smoothstep(0.95, 0.985, t.ridged(4, 4, x=t.x + t.rand(L.id), y=t.y + t.rand(L.id))) * (t.rand(L.id) < 0.5)
    face = mul(face, 1 - 0.55 * crack)
    face = mix(face, col("#3a3128"), 0.35 * smoothstep(0.4, 1.5, t.fbm(5, 4)))
    cov = smoothstep(0.004 - t.px, 0.004 + t.px, e + 0.002 * t.fbm(40, 3))
    face = mul(face, bevel(e - 0.004, 0.008, 0.4))
    joint = mul(fill(t, "#2a241e"), 1 + 0.3 * t.fbm(30, 3))
    return mul(mix(joint, face, cov), stains(t, 2, 0.15))


@texture(tile_m=2.0, rough=0.35)
def floor_terrazzo(t):
    """Polished terrazzo: marble chips of many sizes and colours set in a pale cement, ground flat."""
    img = mul(fill(t, "#c9c3b6"), (1 + 0.04 * t.fbm(4, 4)) * grit(t, 0.03))
    cols = ("#8d8a84", "#5a5650", "#b9a48a", "#e8e4dc", "#7a3f30", "#3c4a3e", "#a8a29a", "#2d2b29")
    for f, cover in ((22, 0.25), (40, 0.4), (70, 0.5), (120, 0.5)):
        x, y = t.warp(f * 2, 0.15 / f, 2)
        W = t.worley(f, jitter=0.9, x=x, y=y)
        keep = t.rand(W.id) < cover
        e = W.edge - (0.1 + 0.25 * t.rand(W.id)) / f
        m = smoothstep(-t.px, t.px, e) * keep
        chip = mul(pick(t.rand(W.id), *cols), (0.85 + 0.25 * t.rand(W.id)) * (1 + 0.06 * t.noise(t.n // 4)))
        img = mix(img, chip, m)
    return img


# ---------------------------------------------------------------- output
def finish(img, name, lo=0.006, hi=0.9):
    """Linear albedo -> 8-bit sRGB, with a little dither against banding."""
    img = np.clip(np.nan_to_num(img), lo, hi)
    rng = np.random.default_rng(zlib.crc32(name.encode()) ^ 0x5EED)
    d = (rng.random(img.shape, dtype=F32) - rng.random(img.shape, dtype=F32)) * F32(0.5)
    return np.clip(linear_to_srgb(img) * 255.0 + d + 0.5, 0, 255).astype(np.uint8)


def seam_ratio(a, block=1):
    """Mean difference across the wrap seam over the mean difference between neighbouring interior columns
    (rows), for columns and rows; about 1 for a seamless image. block=8 compares with the interior 8 x 8 JPEG
    block boundaries only, since the seam of a decoded JPEG is always a block boundary."""
    a = a.astype(F32)
    dc = np.abs(np.diff(a, axis=1))[:, block - 1::block]
    dr = np.abs(np.diff(a, axis=0))[block - 1::block]
    cols = np.abs(a[:, -1] - a[:, 0]).mean() / max(dc.mean(), 1e-6)
    rows = np.abs(a[-1] - a[0]).mean() / max(dr.mean(), 1e-6)
    return float(cols), float(rows)


def periodicity_error(name, n=256):
    """Render the texture small, and again with every coordinate moved by half a tile: for a periodic texture the
    second is the first rolled by half its size. Returns the 99.9th percentile difference in 8-bit sRGB steps."""
    a = TEXTURES[name].fn(Tile(n, name))
    b = TEXTURES[name].fn(Tile(n, name, shift=n // 2))
    a = linear_to_srgb(np.roll(np.clip(a, 0.006, 0.9), (-(n // 2), -(n // 2)), (0, 1)))
    b = linear_to_srgb(np.clip(b, 0.006, 0.9))
    return float(np.percentile(np.abs(a - b), 99.9) * 255)


def build(args):
    """Worker: render one texture; returns the 8-bit image and its statistics."""
    name, n = args[:2]
    t0 = time.time()
    rec = TEXTURES[name]
    img = rec.fn(Tile(n, name))
    assert img.shape == (n, n, 3), (name, img.shape)
    lin = np.clip(np.nan_to_num(img), 0.006, 0.9)
    u8 = finish(img, name)
    sc, sr = seam_ratio(u8)
    stats = dict(name=name, seconds=round(time.time() - t0, 2), mean_lum=round(float(luminance(lin).mean()), 4),
                 min_lum=round(float(np.percentile(luminance(lin), 0.5)), 4),
                 max_lum=round(float(np.percentile(luminance(lin), 99.5)), 4),
                 mean_rgb=[round(float(v), 4) for v in lin.reshape(-1, 3).mean(0)], seam=round(max(sc, sr), 3))
    if len(args) < 3 or args[2]:
        stats["periodic_err"] = round(periodicity_error(name), 2)
    return name, u8, stats


def save_jpeg(u8, path, quality, max_bytes):
    import bpy
    path = os.path.abspath(path)
    n_h, n_w = u8.shape[:2]
    img = bpy.data.images.new("tex", n_w, n_h, alpha=False, float_buffer=False)
    rgba = np.concatenate([u8, np.full((n_h, n_w, 1), 255, np.uint8)], -1)[::-1]   # bpy rows run bottom-up
    img.pixels.foreach_set((rgba.astype(F32) / 255.0).ravel())
    img.filepath_raw = path
    img.file_format = "JPEG"
    q = quality
    while True:
        img.save(quality=q)
        if os.path.getsize(path) <= max_bytes or q <= 78:
            break
        q = max(78, q - 3)
    bpy.data.images.remove(img)
    return q


def load_jpeg(path):
    import bpy
    img = bpy.data.images.load(path)
    w, h = img.size
    p = np.empty(w * h * 4, F32)
    img.pixels.foreach_get(p)
    bpy.data.images.remove(img)
    return (p.reshape(h, w, 4)[::-1, :, :3] * 255.0 + 0.5).astype(np.uint8)


def resize_matrix(n_in, n_out):
    """Area-averaging resampling matrix (n_out x n_in)."""
    m = np.zeros((n_out, n_in), F32)
    s = n_in / n_out
    for o in range(n_out):
        a, b = o * s, (o + 1) * s
        for i in range(int(a), min(n_in, int(math.ceil(b)))):
            m[o, i] = min(b, i + 1) - max(a, i)
    return m / m.sum(1, keepdims=True)


def resize(u8, size):
    """Downsample an 8-bit sRGB image (averaging in linear light)."""
    lin = srgb_to_linear(u8.astype(F32) / 255.0)
    my, mx = resize_matrix(u8.shape[0], size[0]), resize_matrix(u8.shape[1], size[1])
    out = np.stack([my @ lin[..., c] @ mx.T for c in range(3)], -1)
    return (linear_to_srgb(out) * 255.0 + 0.5).astype(np.uint8)


# a 3 x 5 pixel font for the contact sheet labels: each glyph is five rows of three bits
FONT = dict(zip("abcdefghijklmnopqrstuvwxyz0123456789 _-", """
010101111101101 110101110101110 011100100100011 110101101101110 111100110100111 111100110100100
011100101101011 101101111101101 111010010010111 001001001101010 101101110101101 100100100100111
101111101101101 110101101101101 010101101101010 110101110100100 010101101110011 110101110101101
011100010001110 111010010010010 101101101101111 101101101101010 101101101111101 101101010101101
101101010010010 111001010100111 111101101101111 010110010010111 110001010100111 110001010001110
101101111001001 111100110001110 011100110101010 111001010010010 010101010101010 010101011001110
000000000000000 000000000000000 000000111000000""".split()))


def draw_text(canvas, x, y, text, scale=2, color=(225, 222, 214)):
    for ch in text.lower():
        bits = FONT.get(ch, FONT[" "])
        for r in range(5):
            for c in range(3):
                if bits[r * 3 + c] == "1":
                    canvas[y + r * scale:y + (r + 1) * scale, x + c * scale:x + (c + 1) * scale] = color
        x += 4 * scale


def contact_sheet(images, path, thumb=192, gap=8, label_h=18, margin=78):
    cats = []
    for name in images:
        cat = name.split("_")[0]
        if not cats or cats[-1][0] != cat:
            cats.append((cat, []))
        cats[-1][1].append(name)
    cols = max(len(v) for _, v in cats)
    W = margin + cols * (thumb + gap) + gap
    H = len(cats) * (thumb + label_h + gap) + gap
    sheet = np.full((H, W, 3), 32, np.uint8)
    for r, (cat, names) in enumerate(cats):
        y = gap + r * (thumb + label_h + gap)
        draw_text(sheet, 8, y + thumb // 2 - 5, cat)
        for c, name in enumerate(names):
            x = margin + c * (thumb + gap)
            sheet[y:y + thumb, x:x + thumb] = resize(images[name], (thumb, thumb))
            draw_text(sheet, x, y + thumb + 5, name.split("_", 1)[1])
    save_jpeg(sheet, path, 90, 1 << 30)


def tiling_check(images, path, names, size=768, gap=12):
    names = [n for n in names if n in images][:6]
    cols = 3
    rows = (len(names) + cols - 1) // cols
    sheet = np.full((rows * (size + gap + 18) + gap, cols * (size + gap) + gap, 3), 32, np.uint8)
    for i, name in enumerate(names):
        x, y = gap + (i % cols) * (size + gap), gap + (i // cols) * (size + gap + 18)
        sheet[y:y + size, x:x + size] = resize(np.tile(images[name], (2, 2, 1)), (size, size))
        draw_text(sheet, x, y + size + 4, name)
    save_jpeg(sheet, path, 90, 1 << 30)


def material(rec):
    return {"model": "lit", "base_color": [0.4, 0.4, 0.4], "roughness": rec.rough, "metallic": rec.metal,
            "textures": {"base": f"textures/{rec.name}.jpg"},
            "triplanar": {"tile_m": rec.tile_m, "albedo_strength": 1.0, "chroma_mix": 1.0, "normal_strength": 0.0}}


TILING_NAMES = ["brick_flemish", "stone_rubble_wall", "wood_parquet_herringbone", "ground_forest_floor",
                "roof_clay_pantiles", "natural_rock_face"]


def main():
    ap = argparse.ArgumentParser(prog="make_texture_library.py", description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--only")
    ap.add_argument("--sheet")
    ap.add_argument("--tiling")
    ap.add_argument("--materials")
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    ap.add_argument("--quality", type=int, default=88)
    a = ap.parse_args()
    names = list(TEXTURES)
    if a.only:
        want = [w.strip() for w in a.only.split(",") if w.strip()]
        names = [n for n in names if any(n == w or n.split("_", 1)[1] == w for w in want)]
        if not names:
            sys.exit(f"no texture matches --only {a.only}")
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()
    images, stats = {}, []
    jobs = [(n, a.size) for n in names]
    if a.jobs > 1 and len(jobs) > 1:
        import multiprocessing
        pool = multiprocessing.Pool(a.jobs)
        results = pool.imap(build, jobs)
    else:
        pool, results = None, map(build, jobs)
    total = 0
    print(f"{'texture':34s} {'time':>7s}  mean luminance (0.5%-99.5%)  seam ratio raw/jpeg  periodicity  JPEG")
    for name, u8, st in results:
        path = os.path.join(a.out, name + ".jpg")
        st["quality"] = save_jpeg(u8, path, a.quality, 400_000)
        st["kb"] = round(os.path.getsize(path) / 1024)
        total += os.path.getsize(path)
        back = load_jpeg(path)
        assert back.shape == (a.size, a.size, 3), (path, back.shape)
        luma = back.astype(F32) @ np.array([0.299, 0.587, 0.114], F32)
        st["seam_jpeg"] = round(max(seam_ratio(luma[..., None], 8)), 3)
        images[name] = u8
        stats.append(st)
        print(f"{name:34s} {st['seconds']:6.2f}s  lum {st['mean_lum']:.3f} ({st['min_lum']:.3f}-{st['max_lum']:.3f})"
              f"  seam {st['seam']:.2f}/{st['seam_jpeg']:.2f}  per {st['periodic_err']:.2f}"
              f"  q{st['quality']} {st['kb']} KB", flush=True)
    if pool:
        pool.close()
    raw = max(stats, key=lambda s: s["seam"])
    jpg = max(stats, key=lambda s: s["seam_jpeg"])
    per = max(stats, key=lambda s: s["periodic_err"])
    print(f"{len(stats)} textures in {time.time() - t0:.1f}s, {total / 1e6:.1f} MB; worst seam ratio "
          f"{raw['seam']:.2f} ({raw['name']}), after JPEG {jpg['seam_jpeg']:.2f} ({jpg['name']}); "
          f"worst periodicity error {per['periodic_err']:.2f} 8-bit steps ({per['name']})")
    if a.materials:
        os.makedirs(os.path.dirname(os.path.abspath(a.materials)), exist_ok=True)
        with open(a.materials, "w") as f:
            json.dump({f"TX_{n}": material(TEXTURES[n]) for n in names}, f, indent=1)
    if a.sheet:
        contact_sheet(images, os.path.abspath(a.sheet))
    if a.tiling:
        tiling_check(images, os.path.abspath(a.tiling), TILING_NAMES if not a.only else names)


if __name__ == "__main__":
    main()
