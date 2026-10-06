"""Vectorised gradient noise for heightfields (numpy only, deterministic).

Everything here works on whole arrays at once, so a 1009x1009 terrain with
domain warping and a dozen octaves builds in a few seconds.
"""
import numpy as np


class Perlin2D:
    def __init__(self, seed: int):
        rng = np.random.default_rng(seed)
        perm = rng.permutation(256)
        self.perm = np.concatenate([perm, perm]).astype(np.int64)
        angles = rng.uniform(0.0, 2.0 * np.pi, 256)
        self.gx = np.cos(angles)
        self.gy = np.sin(angles)
        self.offset = rng.uniform(-1000.0, 1000.0, 2)

    @staticmethod
    def _fade(t):
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)

    def __call__(self, x, y):
        """Gradient noise in roughly [-1, 1]."""
        x = np.asarray(x, dtype=np.float64) + self.offset[0]
        y = np.asarray(y, dtype=np.float64) + self.offset[1]
        xi = np.floor(x).astype(np.int64)
        yi = np.floor(y).astype(np.int64)
        xf = x - xi
        yf = y - yi
        xi &= 255
        yi &= 255
        p = self.perm

        def grad(ix, iy, dx, dy):
            h = p[p[ix] + iy]
            return self.gx[h] * dx + self.gy[h] * dy

        n00 = grad(xi, yi, xf, yf)
        n10 = grad(xi + 1, yi, xf - 1.0, yf)
        n01 = grad(xi, yi + 1, xf, yf - 1.0)
        n11 = grad(xi + 1, yi + 1, xf - 1.0, yf - 1.0)
        u = self._fade(xf)
        v = self._fade(yf)
        nx0 = n00 + u * (n10 - n00)
        nx1 = n01 + u * (n11 - n01)
        return (nx0 + v * (nx1 - nx0)) * 1.41421356


def fbm(noise, x, y, octaves=6, lacunarity=2.0, gain=0.5):
    total = np.zeros(np.broadcast(x, y).shape)
    amp, freq, norm = 1.0, 1.0, 0.0
    for i in range(octaves):
        # rotate each octave a little to hide grid alignment
        a = 0.5 * i
        ca, sa = np.cos(a), np.sin(a)
        total += amp * noise((x * ca - y * sa) * freq, (x * sa + y * ca) * freq)
        norm += amp
        amp *= gain
        freq *= lacunarity
    return total / norm


def ridged(noise, x, y, octaves=7, lacunarity=2.03, gain=0.5, sharpness=2.0):
    """Ridged multifractal in [0, 1]; each octave is weighted by the previous
    one so detail concentrates on ridges, like eroded mountain crests."""
    total = np.zeros(np.broadcast(x, y).shape)
    weight = np.ones_like(total)
    amp, freq, norm = 1.0, 1.0, 0.0
    for i in range(octaves):
        a = 0.37 * i
        ca, sa = np.cos(a), np.sin(a)
        n = noise((x * ca - y * sa) * freq, (x * sa + y * ca) * freq)
        n = (1.0 - np.abs(n)) ** sharpness
        n *= weight
        weight = np.clip(n * 1.6, 0.0, 1.0)
        total += n * amp
        norm += amp
        amp *= gain
        freq *= lacunarity
    return total / norm


def smoothstep(e0, e1, x):
    if e1 == e0:  # zero-width edge: a hard step
        return (np.asarray(x) >= e0).astype(np.float64)
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)
