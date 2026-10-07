"""Tests for dayfall_convert.py (plain Python 3, no Unreal).

    python tools/unreal/test_dayfall_convert.py            # unit tests
    python tools/unreal/test_dayfall_convert.py --engine   # also builds a fake Unreal export and checks it in bin/dayfall

The engine test writes a map the way export_level_to_dayfall.py does (heightfield traced in Unreal coordinates,
a mesh in the Unreal glTF exporter's axis order, stretched instances, a rotated object) and checks with
ground_query that everything lands where Unreal had it.
"""
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dayfall_convert as C  # noqa: E402

ENGINE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def write_file(path, data):
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    x, y, z, w = qmul(qmul(q, (v[0], v[1], v[2], 0.0)), (-q[0], -q[1], -q[2], q[3]))
    return (x, y, z)


def mirror(v):
    return (v[0], -v[1], v[2])


def ue_yaw_quat(deg):
    a = math.radians(deg) / 2
    return (0.0, 0.0, math.sin(a), math.cos(a))


class Coordinates(unittest.TestCase):
    def test_quaternion_mirror(self):
        rnd = random.Random(3)
        for _ in range(200):
            q = [rnd.uniform(-1, 1) for _ in range(4)]
            n = math.sqrt(sum(c * c for c in q))
            q = [c / n for c in q]
            v = [rnd.uniform(-5, 5) for _ in range(3)]
            want = mirror(qrot(q, v))
            got = qrot(C.quat(*q), mirror(v))
            for a, b in zip(want, got):
                self.assertAlmostEqual(a, b, places=9)

    def test_yaw(self):
        for deg in (0, 30, 90, -45, 170):
            fwd = C.direction(*qrot(ue_yaw_quat(deg), (1, 0, 0)))
            eng_yaw = math.degrees(math.atan2(fwd[1], fwd[0]))
            self.assertAlmostEqual((eng_yaw - C.yaw(deg) + 540) % 360 - 180, 0, places=6)

    def test_position(self):
        self.assertEqual(C.pos(100, 200, 300), [1.0, -2.0, 3.0])

    def test_scale(self):
        self.assertEqual(C.scale(2, 2, 2), 2)
        self.assertEqual(C.scale(1, 2, 3), [1, 2, 3])
        self.assertEqual(C.scale(-1, -1, -1), [-1, -1, -1])
        self.assertTrue(C.mirrored(-1, 1, 1))

    def test_sun(self):
        # light travelling towards +X (Unreal) and down 30 degrees: the sun is in the west (-X), 30 degrees up
        f = (math.cos(math.radians(30)), 0.0, -math.sin(math.radians(30)))
        el, az = C.sun_angles(f)
        self.assertAlmostEqual(el, 30, places=6)
        self.assertAlmostEqual(az, -90, places=6)
        # light travelling towards +Y in Unreal = towards engine -Y (south): the sun is north (azimuth 0)
        el, az = C.sun_angles((0.0, 1.0, -1.0))
        self.assertAlmostEqual(el, 45, places=6)
        self.assertAlmostEqual(az, 0, places=6)

    def test_terrain_grid(self):
        n, s, org = C.terrain_grid(-5000, -2000, 5000, 3000, 1.0, 4097)
        self.assertEqual(n, 101)
        self.assertAlmostEqual(s, 1.0)
        # corner sample (0, 0) is engine south-west: Unreal (min x, max y) of the square around the box
        ue_x, ue_y = org[0] * 100, -org[1] * 100
        self.assertAlmostEqual(ue_x, -5000)
        self.assertAlmostEqual(ue_y, 5500)
        n, s, org = C.terrain_grid(0, 0, 100000, 100000, 1.0, 513)
        self.assertEqual(n, 513)
        self.assertAlmostEqual(s, 1000 / 512)


class Units(unittest.TestCase):
    def test_lights(self):
        self.assertAlmostEqual(C.light_candela(5000, "unitless"), 8.0)
        self.assertAlmostEqual(C.light_candela(8, "Candelas"), 8.0)
        self.assertAlmostEqual(C.light_candela(4 * math.pi * 10, "LUMENS"), 10.0)
        self.assertAlmostEqual(C.point_intensity(8, 10), 4.0)

    def test_colour(self):
        self.assertEqual(C.fcolor_to_linear(255, 255, 255), [1.0, 1.0, 1.0])
        self.assertAlmostEqual(C.fcolor_to_linear(128, 0, 0)[0], 0.2158605, places=5)
        w = C.kelvin_to_linear(6500)
        self.assertTrue(all(c > 0.9 for c in w), w)
        warm = C.kelvin_to_linear(2700)
        self.assertTrue(warm[0] > warm[1] > warm[2])

    def test_hfov(self):
        self.assertAlmostEqual(C.hfov_to_vfov(90), 58.7155, places=3)


class Materials(unittest.TestCase):
    def test_params(self):
        cases = {"BaseColor": "base", "Albedo Texture": "base", "Normal": "normal", "NormalMap": "normal",
                 "ORM": "orm", "T_ARM": "orm", "Emissive Color Map": "emissive", "RoughnessTexture": None,
                 "DetailNormal": "normal", "MacroVariation": None, "Diffuse": "base", "AO": None}
        for k, v in cases.items():
            self.assertEqual(C.classify_texture_param(k), v, k)

    def test_textures(self):
        self.assertEqual(C.classify_texture("T_Wall_N", "TC_Normalmap", False), "normal")
        self.assertEqual(C.classify_texture("T_Wall_BC", "TC_Default", True), "base")
        self.assertEqual(C.classify_texture("T_Wall_ORM", "TC_Masks", False), "orm")
        self.assertEqual(C.classify_texture("T_Wall_RMA", "TC_Masks", False), None)
        self.assertEqual(C.classify_texture("T_Noise", "TC_Grayscale", False), None)

    def test_docs(self):
        d = C.material_doc({"name": "MI_Leaves", "blend": "mask", "two_sided": True, "textures": {"base": "t/a.png"}})
        self.assertEqual(d["model"], "foliage")
        d = C.material_doc({"name": "M_Water_Lake", "blend": "blend"})
        self.assertEqual(d["model"], "water")
        d = C.material_doc({"name": "MI_Stone", "textures": {"base": "a", "normal": "b", "orm": "c"}})
        self.assertEqual((d["model"], d["roughness"], d["metallic"], d["normal_convention"]), ("lit", 1.0, 1.0, "directx"))
        d = C.material_doc({"name": "MI_Plain", "tint": [0.2, 0.3, 0.4], "roughness": 0.4})
        self.assertEqual((d["base_color"], d["roughness"]), ([0.2, 0.3, 0.4], 0.4))


class Files(unittest.TestCase):
    def test_instances(self):
        data, layout = C.pack_instances([([1, 2, 3], [0, 0, 0, 1], 2.0)])
        self.assertEqual((len(data), layout), (32, "pos_scale_quat"))
        self.assertEqual(struct.unpack("<8f", data), (1, 2, 3, 2, 0, 0, 0, 1))
        data, layout = C.pack_instances([([1, 2, 3], [0, 0, 0, 1], 2.0), ([0, 0, 0], [0, 0, 0, 1], [1, 2, 3])])
        self.assertEqual((len(data), layout), (80, "pos_quat_scale3"))
        self.assertEqual(struct.unpack("<10f", data[40:]), (0, 0, 0, 0, 0, 0, 1, 1, 2, 3))

    def test_holes(self):
        h = [None, 1.0, None, None, None, None, 3.0, None, None]
        C.fill_holes(h, 3)
        self.assertNotIn(None, h)
        self.assertEqual(h[0], 1.0)
        self.assertEqual(h[8], 3.0)

    def test_png(self):
        n = 5
        hts = [float(j * 10 + i) for j in range(n) for i in range(n)]
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "h.png")
            lo, hi = C.write_height_png(p, hts, n)
            with open(p, "rb") as f:
                raw = f.read()
        self.assertEqual((lo, hi), (0.0, 44.0))
        idat = raw[raw.index(b"IDAT") + 4:raw.index(b"IEND") - 8]
        px = zlib.decompress(idat)
        stride = 1 + 2 * n
        top = struct.unpack(f">{n}H", px[1:stride])            # first PNG row = north = engine row n-1
        self.assertEqual(top[0], round((40 - lo) / (hi - lo) * 65535))


# ---------------------------------------------------------------- engine integration

def write_glb(path, positions_ue_cm, indices, material):
    """A GLB as the Unreal glTF exporter writes it: Unreal (x, y, z) cm -> glTF (x, z, y) * 0.01."""
    pts = [(x * 0.01, z * 0.01, y * 0.01) for x, y, z in positions_ue_cm]
    pbin = b"".join(struct.pack("<3f", *p) for p in pts)
    ibin = b"".join(struct.pack("<I", i) for i in indices)
    binbuf = pbin + ibin
    mn = [min(p[k] for p in pts) for k in range(3)]
    mx = [max(p[k] for p in pts) for k in range(3)]
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "m"}],
           "materials": [{"name": material, "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.2, 0.2, 1]}}],
           "meshes": [{"name": "m", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
           "buffers": [{"byteLength": len(binbuf)}],
           "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(pbin)},
                           {"buffer": 0, "byteOffset": len(pbin), "byteLength": len(ibin)}],
           "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(pts), "type": "VEC3", "min": mn, "max": mx},
                         {"bufferView": 1, "componentType": 5125, "count": len(indices), "type": "SCALAR"}]}
    js = json.dumps(doc).encode()
    js += b" " * (-len(js) % 4)
    binbuf += b"\0" * (-len(binbuf) % 4)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binbuf))
    out += struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(binbuf), 0x004E4942) + binbuf
    write_file(path, out)


def box_ue(x0, y0, z0, x1, y1, z1):
    """A closed box in Unreal cm (12 triangles)."""
    v = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    f = [(0, 2, 1), (1, 2, 3), (4, 5, 6), (5, 7, 6), (0, 1, 4), (1, 5, 4), (2, 6, 3), (3, 6, 7), (0, 4, 2), (2, 4, 6),
         (1, 3, 5), (3, 7, 5)]
    return v, [i for t in f for i in t]


def ue_height(x_cm, y_cm):
    """The fake Unreal landscape: a slope rising towards Unreal +Y (engine south) plus a bump at (20 m, 30 m)."""
    return 200.0 + y_cm * 0.05 + 600.0 * math.exp(-((x_cm - 2000) ** 2 + (y_cm - 3000) ** 2) / (800.0 ** 2))


class Engine(unittest.TestCase):
    exe = os.path.join(ENGINE_DIR, "bin", "dayfall.exe" if os.name == "nt" else "dayfall")

    @unittest.skipUnless("--engine" in sys.argv, "pass --engine to run against bin/dayfall")
    def test_fake_unreal_export(self):
        d = tempfile.mkdtemp(prefix="dayfall_ue_")
        try:
            os.makedirs(os.path.join(d, "terrain"))
            os.makedirs(os.path.join(d, "assets"))
            os.makedirs(os.path.join(d, "instances"))
            # heightfield: traced in Unreal space over x, y in [-6000, 6000] cm
            n, s, org = C.terrain_grid(-6000, -6000, 6000, 6000, 1.0, 4097)
            hts = []
            for j in range(n):
                for i in range(n):
                    ex, ey = org[0] + i * s, org[1] + j * s
                    hts.append(ue_height(ex * 100, -ey * 100) * C.CM)
            lo, hi = C.write_height_png(os.path.join(d, "terrain", "height.png"), hts, n)
            # an L-shaped "flag": a pole at the origin and an arm pointing to Unreal +Y (engine -Y)
            v1, i1 = box_ue(-20, -20, 0, 20, 20, 600)
            v2, i2 = box_ue(-20, 20, 500, 20, 400, 600)
            write_glb(os.path.join(d, "assets", "SM_Flag.glb"), v1 + v2, i1 + [k + len(v1) for k in i2], "M_Flag")
            v, i = box_ue(-50, -50, 0, 50, 50, 100)
            write_glb(os.path.join(d, "assets", "SM_Cube.glb"), v, i, "M_Cube")
            # the flag at Unreal (1000, -1000, ground), turned 90 degrees (yaw): the arm then points to Unreal -X
            fx, fy = 1000.0, -1000.0
            q = C.quat(*ue_yaw_quat(90))
            objects = [{"id": "flag", "mesh": "SM_Flag", "position": C.pos(fx, fy, ue_height(fx, fy)), "rotation": q,
                        "collision": "mesh"}]
            # cubes stretched (2, 1, 3) along a row at Unreal y = 2000
            recs = []
            for k in range(4):
                ux, uy = -3000.0 + k * 500.0, 2000.0
                recs.append((C.pos(ux, uy, ue_height(ux, uy)), C.quat(0, 0, 0, 1), C.scale(2, 1, 3)))
            data, layout = C.pack_instances(recs)
            write_file(os.path.join(d, "instances", "cubes.bin"), data)
            doc = {"format": "dayfall-map", "name": "fake unreal export",
                   "terrain": {"samples_per_side": n, "spacing_m": s, "origin": org, "height_file": "terrain/height.png",
                               "height_range_m": [lo, hi], "png_rows": "north_first", "horizon": {"enabled": False}},
                   "meshes": {"SM_Flag": {"file": "assets/SM_Flag.glb", "collision": "mesh"},
                              "SM_Cube": {"file": "assets/SM_Cube.glb", "collision": "mesh"}},
                   "objects": objects,
                   "instance_files": [{"id": "cubes", "file": "instances/cubes.bin", "mesh": "SM_Cube", "layout": layout,
                                       "collision": "mesh"}],
                   "player_start": {"position": C.pos(0, 0, ue_height(0, 0)), "yaw_deg": C.yaw(0)}}
            write_file(os.path.join(d, "map.json"), json.dumps(doc))
            # probe points in Unreal space
            arm_ue = (fx - 300.0, fy)          # the arm after the 90 degree turn
            cube_ue = (-3000.0 + 500.0, 2000.0)
            bump_ue = (2000.0, 3000.0)
            probes = {"arm": arm_ue, "pole": (fx, fy), "beside_arm": (fx + 300.0, fy), "cube": cube_ue,
                      "bump": bump_ue, "open": (-4000.0, -4000.0)}
            pts = [[C.pos(x, y, 0)[0], C.pos(x, y, 0)[1]] for x, y in probes.values()]
            calls = [{"tool": "ground_query", "args": {"points": pts}}]
            cf = os.path.join(d, "calls.json")
            write_file(cf, json.dumps(calls))
            cmd = [self.exe, d, "--headless", "--no-mcp", "--exec", cf]
            if shutil.which("xvfb-run") and os.name != "nt" and not os.environ.get("DISPLAY"):
                cmd = ["xvfb-run", "-a"] + cmd
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            out = "\n".join(l for l in r.stdout.splitlines() if not l.lstrip().startswith("[ ") or l.strip() in ("[", "]"))
            res = json.loads(out[out.index("["):])
            got = {k: p for k, p in zip(probes, res[0]["result"]["points"])}
            m = C.CM
            self.assertAlmostEqual(got["bump"]["terrain_height"], ue_height(*bump_ue) * m, delta=0.05)
            self.assertAlmostEqual(got["open"]["terrain_height"], ue_height(-4000, -4000) * m, delta=0.05)
            self.assertAlmostEqual(got["pole"]["height"], (ue_height(fx, fy) + 600) * m, delta=0.05)
            self.assertAlmostEqual(got["arm"]["height"], (ue_height(fx, fy) + 600) * m, delta=0.05)
            self.assertLess(got["beside_arm"]["height"], (ue_height(fx, fy) + 100) * m)
            self.assertAlmostEqual(got["cube"]["height"], (ue_height(*cube_ue) + 300) * m, delta=0.05)
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(argv=[a for a in sys.argv if a != "--engine"])
