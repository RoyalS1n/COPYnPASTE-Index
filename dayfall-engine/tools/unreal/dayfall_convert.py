"""Unreal -> DAYFALL conversions with no Unreal dependency (unit-tested by test_dayfall_convert.py).

Unreal: centimetres, X forward, Y right, Z up (left-handed).
DAYFALL: metres, X east, Y north, Z up (right-handed).

The two differ by a mirror across Y plus the unit change:
    point       (x, y, z)     -> (x, -y, z) / 100
    quaternion  (x, y, z, w)  -> (-x, y, -z, w)     (M R M with M = diag(1, -1, 1))
    scale       (sx, sy, sz)  -> (sx, sy, sz)       (a diagonal scale commutes with the mirror)
    yaw         a degrees     -> -a degrees         (DAYFALL yaw: counter-clockwise from +X)
Mesh vertices get the same mirror from the glTF exporter (Unreal (x, y, z) -> glTF (x, z, y)), which the
engine reads back as (x, -y, z), so a mesh placed with the converted transform lands where Unreal shows it.
"""
import math
import re
import struct
import zlib

CM = 0.01


# ---------------------------------------------------------------- coordinates

def pos(x, y, z):
    return [x * CM, -y * CM, z * CM]


def quat(x, y, z, w):
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    q = [-x / n, y / n, -z / n, w / n]
    if q[3] < 0:  # canonical sign keeps map.json diffs stable
        q = [-c for c in q]
    return q


def is_identity_quat(q, eps=1e-5):
    return abs(q[0]) < eps and abs(q[1]) < eps and abs(q[2]) < eps


def scale(sx, sy, sz, eps=1e-4):
    """A number when uniform, else [x, y, z]."""
    if abs(sx - sy) <= eps * max(abs(sx), 1.0) and abs(sx - sz) <= eps * max(abs(sx), 1.0) and sx > 0:
        return sx
    return [sx, sy, sz]


def mirrored(sx, sy, sz):
    return sx * sy * sz < 0


def yaw(ue_yaw_deg):
    return -ue_yaw_deg


def direction(x, y, z):
    """A direction vector (no unit change)."""
    return [x, -y, z]


def sun_angles(forward):
    """Unreal directional light forward vector (the way the light travels) -> DAYFALL sun elevation and azimuth
    in degrees. DAYFALL sun direction: (sin az cos el, cos az cos el, sin el), pointing at the sun."""
    fx, fy, fz = direction(*forward)
    tx, ty, tz = -fx, -fy, -fz
    n = math.sqrt(tx * tx + ty * ty + tz * tz) or 1.0
    tx, ty, tz = tx / n, ty / n, tz / n
    el = math.degrees(math.asin(max(-1.0, min(1.0, tz))))
    az = math.degrees(math.atan2(tx, ty))
    return el, az


def hfov_to_vfov(hfov_deg, aspect=16 / 9):
    return math.degrees(2 * math.atan(math.tan(math.radians(hfov_deg) / 2) / aspect))


# ---------------------------------------------------------------- colour and light units

def srgb_to_linear(c):
    c = c / 255.0 if c > 1.0 else c
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def fcolor_to_linear(r, g, b):
    """FColor (sRGB bytes, as Unreal stores light colours) -> linear RGB 0..1."""
    return [srgb_to_linear(r / 255.0), srgb_to_linear(g / 255.0), srgb_to_linear(b / 255.0)]


def kelvin_to_linear(t):
    """Blackbody colour (approximation good to a few percent from 1000 K to 40000 K), linear RGB with max 1."""
    t = max(1000.0, min(40000.0, t)) / 100.0
    r = 255.0 if t <= 66 else 329.698727446 * (t - 60) ** -0.1332047592
    g = 99.4708025861 * math.log(t) - 161.1195681661 if t <= 66 else 288.1221695283 * (t - 60) ** -0.0755148492
    b = 255.0 if t >= 66 else (0.0 if t <= 19 else 138.5177312231 * math.log(t - 10) - 305.0447927307)
    lin = [srgb_to_linear(max(0.0, min(255.0, v)) / 255.0) for v in (r, g, b)]
    m = max(lin) or 1.0
    return [v / m for v in lin]


def light_candela(intensity, units):
    """Unreal local light intensity -> candela. units: 'candelas', 'lumens', 'ev', 'unitless'."""
    u = (units or "unitless").lower()
    if "candela" in u:
        return intensity
    if "lumen" in u:
        return intensity / (4 * math.pi)   # Unreal spreads lumens over the full sphere for point and spot lights
    if u.startswith("ev") or "ev100" in u:
        return 2.0 ** (intensity - 3.0)
    return intensity / 625.0               # legacy unitless: 5000 unitless = 8 cd


# DAYFALL light units follow the Blender worlds: sun strength ~5 and point intensities on the same relative
# scale. With a sun in the level, a point light keeps its Unreal ratio to the sun (cd / lux); without one, assume
# a physical 100000 lux sun.
DAYFALL_SUN_STRENGTH = 5.0
ASSUMED_SUN_LUX = 100000.0


def point_intensity(candela, sun_lux):
    return candela * DAYFALL_SUN_STRENGTH / (sun_lux if sun_lux and sun_lux > 0 else ASSUMED_SUN_LUX)


def preset_for_sun(elevation_deg):
    if elevation_deg < 6:
        return "dusk"
    if elevation_deg < 24:
        return "golden_hour"
    return "noon"


# ---------------------------------------------------------------- names

def safe_name(name):
    s = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(name)).strip("._")
    return s or "unnamed"


class UniqueNames:
    def __init__(self):
        self.used = set()

    def get(self, base):
        base = safe_name(base)
        name, k = base, 2
        while name.lower() in self.used:
            name, k = f"{base}_{k}", k + 1
        self.used.add(name.lower())
        return name


# ---------------------------------------------------------------- materials

WATER_WORDS = ("water", "ocean", "lake", "river", "pond", "sea", "stream")
FOLIAGE_WORDS = ("leaf", "leaves", "foliage", "grass", "fern", "needle", "branch", "frond", "ivy", "plant", "bush",
                 "shrub", "moss_card", "flower", "canopy", "crown", "sorrel", "salal", "clover", "weed")


def name_tokens(name):
    """'T_Wall_BaseColor' -> ['t', 'wall', 'base', 'color']; 'ORM' -> ['orm']."""
    return [t.lower() for t in re.findall(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+", str(name))]


_EXCLUDE = ("rough", "metal", "spec", "occlusion", "ambient", "height", "disp", "mask", "opacity", "detail", "noise",
            "macro", "blend", "wind", "flow", "puddle", "wet", "curvature", "cavity", "thickness", "subsurface")


def classify_texture_param(name):
    """A material parameter name -> 'base' | 'normal' | 'orm' | 'emissive' | None."""
    t = name_tokens(name)
    if any("normal" in k or k in ("nrm", "nmap", "bump") for k in t) or (len(t) > 1 and t[-1] == "n"):
        return "normal"
    if any(k in ("orm", "arm", "aorm") for k in t) or {"occlusion", "roughness", "metallic"} <= set(t):
        return "orm"
    if any(k.startswith(("emissi", "emission", "glow")) for k in t):
        return "emissive"
    if any(k.startswith(_EXCLUDE) for k in t) or "ao" in t:
        return None
    if any(k in ("base", "basecolor", "albedo", "diffuse", "color", "colour", "col", "tex", "texture", "bc") for k in t) \
            or (len(t) > 1 and t[-1] in ("d", "c")) or t in (["d"], ["c"]):
        return "base"
    return None


def classify_texture(name, compression="", srgb=True):
    """A texture asset (by name and import settings) -> slot or None, for materials that are not instances."""
    c = (compression or "").lower()
    n = name.lower()
    if "normal" in c or re.search(r"(_n|_nrm|_normal|_nor)(_|$)", n):
        return "normal"
    if re.search(r"(_orm|_arm)(_|$)", n):
        return "orm"
    if re.search(r"(_rma|_mra|_rmo|_mro|_ao|_r|_m|_rough|_roughness|_metal|_metallic|_mask|_masks|_h|_height|_disp)(_|$)", n):
        return None   # channel orders the engine does not read; the report lists them
    if "emissive" in n or re.search(r"(_e|_emit|_emission)(_|$)", n):
        return "emissive"
    if "mask" in c or "grayscale" in c or "alpha" in c or "hdr" in c:
        return None
    if srgb or re.search(r"(_bc|_d|_c|_albedo|_basecolor|_base_color|_diffuse|_col|_color)(_|$)", n):
        return "base"
    return None


def material_doc(info):
    """info: {name, blend: opaque|mask|blend|additive, two_sided, shading: default|foliage|subsurface|unlit|clear_coat,
    textures: {slot: relative path}, tint: [r,g,b] linear or None, roughness, metallic, emissive_strength, opacity}
    -> a DAYFALL material definition."""
    name = info["name"].lower()
    blend = info.get("blend", "opaque")
    tex = {k: v for k, v in (info.get("textures") or {}).items() if v}
    tint = info.get("tint")
    shading = info.get("shading", "default")
    if blend in ("blend", "additive") and any(w in name for w in WATER_WORDS):
        d = {"model": "water", "roughness": 0.04}
        if tint:
            d["color"] = [round(c, 4) for c in tint]
        return d
    foliage = (shading in ("foliage", "subsurface") and blend == "mask") or \
        (blend == "mask" and any(w in name for w in FOLIAGE_WORDS))
    if foliage:
        d = {"model": "foliage", "alpha": "mask", "two_sided": True, "variation": 0.4, "translucency": 0.35,
             "roughness": round(info.get("roughness", 0.62), 3)}
        if "base" in tex:   # the texture carries the colour; without one the engine's leaf colours apply
            d.update(color_a=[1.0, 1.0, 1.0], color_b=[0.86, 0.9, 0.8], tip=[1.0, 1.0, 1.0])
        if tint:
            d["color_a"] = [round(c, 4) for c in tint]
            d["color_b"] = [round(c * 0.88, 4) for c in tint]
        if "base" in tex:
            d["textures"] = {"base": tex["base"]}
        if info.get("alpha_cutoff") is not None:
            d["alpha_cutoff"] = round(info["alpha_cutoff"], 3)
        return d
    model = "unlit" if shading == "unlit" else "lit"
    d = {"model": model}
    d["base_color"] = [round(c, 4) for c in tint] if tint else ([1.0, 1.0, 1.0] if "base" in tex else [0.6, 0.6, 0.6])
    if model == "lit":
        # with an ORM texture the scalars multiply its channels
        d["roughness"] = 1.0 if "orm" in tex else round(info.get("roughness", 0.6), 3)
        d["metallic"] = 1.0 if "orm" in tex else round(info.get("metallic", 0.0), 3)
    if tex:
        d["textures"] = dict(tex)
        if "normal" in tex:
            d["normal_convention"] = "directx"
    if info.get("emissive_strength"):
        d["emissive"] = [1.0, 1.0, 1.0]
        d["emissive_strength"] = round(info["emissive_strength"], 3)
    if blend == "mask":
        d["alpha"] = "mask"
        if info.get("alpha_cutoff") is not None:
            d["alpha_cutoff"] = round(info["alpha_cutoff"], 3)
    elif blend in ("blend", "additive"):
        d["alpha"] = "blend"
        if info.get("opacity") is not None and "base" not in tex:
            d["base_color"] = d["base_color"][:3] + [round(info["opacity"], 3)]
    if info.get("two_sided"):
        d["two_sided"] = True
    return d


# ---------------------------------------------------------------- meshes

def lod_entry(radius_m, triangles):
    """Engine LOD settings for a mesh from its bounding radius (m) and LOD0 triangle count."""
    e = {}
    if triangles >= 2000:
        e["lod0_distance_m"] = round(max(25.0, radius_m * 10.0), 1)
        lods = [{"ratio": 0.4, "distance_m": round(max(80.0, radius_m * 30.0), 1)}]
        if triangles >= 12000:
            lods.append({"ratio": 0.12, "distance_m": round(max(250.0, radius_m * 90.0), 1), "sloppy": True})
        e["lods"] = lods
    e["cull_distance_m"] = round(min(4000.0, max(120.0, radius_m * 160.0)), 1)
    return e


def gltf_scale_check(expected_ue_extent_cm, gltf_min, gltf_max):
    """Compare a glTF's POSITION bounds with the Unreal mesh extent. Returns (import_scale, axes_ok).
    The Unreal glTF exporter writes (x, z, y) * 0.01, so glTF extent should be (ex, ez, ey) / 100."""
    ex, ey, ez = expected_ue_extent_cm
    want = [ex * CM, ez * CM, ey * CM]
    got = [gltf_max[i] - gltf_min[i] for i in range(3)]
    big_w, big_g = max(want), max(got)
    if big_w <= 0 or big_g <= 0:
        return 1.0, True
    ratio = big_w / big_g
    s = 1.0
    for cand in (1.0, 0.01, 100.0, 0.1, 10.0):
        if abs(math.log10(ratio / cand)) < 0.15:
            s = cand
            break
    else:
        s = ratio
    axes_ok = all(abs(g * s - w) <= 0.05 * big_w + 0.01 for g, w in zip(got, want))
    return s, axes_ok


# ---------------------------------------------------------------- instance files

def pack_instances(records):
    """records: [(pos[3], quat[4], scale number or [3])] in DAYFALL units.
    Returns (bytes, layout): 32-byte pos_scale_quat when every scale is uniform, else 40-byte pos_quat_scale3."""
    uniform = all(not isinstance(s, (list, tuple)) for _, _, s in records)
    out = bytearray()
    if uniform:
        for p, q, s in records:
            out += struct.pack("<8f", p[0], p[1], p[2], s, q[0], q[1], q[2], q[3])
        return bytes(out), "pos_scale_quat"
    for p, q, s in records:
        sx, sy, sz = (s, s, s) if not isinstance(s, (list, tuple)) else s
        out += struct.pack("<10f", p[0], p[1], p[2], q[0], q[1], q[2], q[3], sx, sy, sz)
    return bytes(out), "pos_quat_scale3"


# ---------------------------------------------------------------- heightfield

def fill_holes(h, n, missing=None):
    """Fill None samples in an n*n row-major grid from the nearest filled sample along rows, then columns;
    anything still empty gets the lowest height. Returns the number of holes filled."""
    holes = sum(1 for v in h if v is None)
    if holes == 0:
        return 0
    filled = [v for v in h if v is not None]
    low = min(filled) if filled else 0.0
    for _ in range(2):
        for j in range(n):          # rows
            row = h[j * n:(j + 1) * n]
            idx = [i for i, v in enumerate(row) if v is not None]
            if not idx or len(idx) == n:
                continue
            k = 0
            for i in range(n):
                if row[i] is None:
                    while k + 1 < len(idx) and abs(idx[k + 1] - i) <= abs(idx[k] - i):
                        k += 1
                    h[j * n + i] = row[idx[k]]
        for i in range(n):          # columns
            col = [h[j * n + i] for j in range(n)]
            idx = [j for j, v in enumerate(col) if v is not None]
            if not idx or len(idx) == n:
                continue
            k = 0
            for j in range(n):
                if col[j] is None:
                    while k + 1 < len(idx) and abs(idx[k + 1] - j) <= abs(idx[k] - j):
                        k += 1
                    h[j * n + i] = col[idx[k]]
    for k, v in enumerate(h):
        if v is None:
            h[k] = low if missing is None else missing
    return holes


def _png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_height_png(path, heights, n):
    """heights: n*n metres, row 0 = south (the engine's row order). Writes a 16-bit greyscale PNG with north at the
    top, as DAYFALL expects with "png_rows": "north_first". Returns (lo, hi) for height_range_m."""
    lo, hi = min(heights), max(heights)
    if hi - lo < 0.01:
        hi = lo + 0.01
    k = 65535.0 / (hi - lo)
    raw = bytearray()
    for j in range(n - 1, -1, -1):
        raw.append(0)
        row = heights[j * n:(j + 1) * n]
        raw += struct.pack(f">{n}H", *[int(round((v - lo) * k)) for v in row])
    png = b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 16, 0, 0, 0, 0))
    png += _png_chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + _png_chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    return lo, hi


def terrain_grid(ue_min_x, ue_min_y, ue_max_x, ue_max_y, spacing_m, max_samples):
    """A square DAYFALL grid covering an Unreal XY box (cm). Returns (n, spacing_m, origin [x, y] in metres).
    Sample (i, j) sits at engine (origin.x + i*s, origin.y + j*s) = Unreal (x*100, -y*100)."""
    w = (ue_max_x - ue_min_x) * CM
    hgt = (ue_max_y - ue_min_y) * CM
    size = max(w, hgt, spacing_m)
    n = int(math.ceil(size / spacing_m)) + 1
    if n > max_samples:
        n = max_samples
    s = size / (n - 1)
    # centre the square on the box
    cx, cy = (ue_min_x + ue_max_x) * 0.5 * CM, -(ue_min_y + ue_max_y) * 0.5 * CM
    origin = [cx - size * 0.5, cy - size * 0.5]
    return n, s, origin
