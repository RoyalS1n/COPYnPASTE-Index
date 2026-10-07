"""Tests for quantize_glb.py: python tools/test_quantize_glb.py"""
import json
import os
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quantize_glb as Q  # noqa: E402


def make_glb(path, nodes, extra_children=False):
    """Two primitives (one shared POSITION accessor would be unusual; each has its own), float attributes."""
    rng = np.random.default_rng(1)
    pos = (rng.random((30, 3)) * [80, 12, 40] - [40, 2, 20]).astype(np.float32)
    nrm = rng.normal(size=(30, 3)); nrm = (nrm / np.linalg.norm(nrm, axis=1, keepdims=True)).astype(np.float32)
    uv = np.stack([rng.random(30), rng.choice([0.75, -1.0, 0.3], 30)], 1).astype(np.float32)
    col = (rng.integers(0, 256, (30, 4)) / 255.0).astype(np.float32)
    idx = np.arange(30, dtype=np.uint16)
    blobs, views, accs = [], [], []

    def add(arr, typ, ctype):
        off = sum(len(b) for b in blobs)
        data = arr.tobytes(); data += b"\0" * (-len(data) % 4)
        blobs.append(data)
        views.append({"buffer": 0, "byteOffset": off, "byteLength": arr.nbytes})
        a = {"bufferView": len(views) - 1, "componentType": ctype, "count": len(arr), "type": typ}
        if typ == "VEC3" and arr is pos:
            a["min"], a["max"] = pos.min(0).tolist(), pos.max(0).tolist()
        accs.append(a)
        return len(accs) - 1

    attrs = {"POSITION": add(pos, "VEC3", Q.FLOAT), "NORMAL": add(nrm, "VEC3", Q.FLOAT),
             "TEXCOORD_0": add(uv, "VEC2", Q.FLOAT), "COLOR_0": add(col, "VEC4", Q.FLOAT)}
    ind = add(idx, "SCALAR", Q.USHORT)
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": nodes,
           "meshes": [{"primitives": [{"attributes": attrs, "indices": ind, "material": 0}]}],
           "materials": [{"name": "FT_Sand"}], "accessors": accs, "bufferViews": views,
           "buffers": [{"byteLength": sum(len(b) for b in blobs)}]}
    Q.write_glb(path, doc, b"".join(blobs))
    return pos, nrm, uv, col


def world_positions(path):
    doc, b = Q.read_glb(path)
    out = []
    for node in doc["nodes"]:
        if "mesh" not in node:
            continue
        m = np.eye(4)
        t, r, s = node.get("translation", [0, 0, 0]), node.get("rotation", [0, 0, 0, 1]), node.get("scale", [1, 1, 1])
        p = Q.read_accessor(doc, b, doc["meshes"][node["mesh"]]["primitives"][0]["attributes"]["POSITION"])
        p = np.array([Q.rotate(r, v * s) for v in p]) + t
        # parent transforms (one level is enough here)
        for parent in doc["nodes"]:
            if doc["nodes"].index(node) in parent.get("children", []):
                pt, pr, ps = parent.get("translation", [0, 0, 0]), parent.get("rotation", [0, 0, 0, 1]), parent.get("scale", [1, 1, 1])
                p = np.array([Q.rotate(pr, v * ps) for v in p]) + pt
        out.append((p, doc, b, node))
    return out


class QuantizeTest(unittest.TestCase):
    def roundtrip(self, nodes):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "a.glb"), os.path.join(d, "b.glb")
            pos, nrm, uv, col = make_glb(src, nodes)
            before = os.path.getsize(src)
            self.assertEqual(Q.main([src, dst]), 0)
            self.assertLess(os.path.getsize(dst), before)
            (p, doc, b, node), = world_positions(dst)
            (p0, *_), = world_positions(src)
            self.assertIn(Q.EXT, doc["extensionsRequired"])
            self.assertLess(np.abs(p - p0).max(), 80 / 32767 * 1.01)
            prim = doc["meshes"][node["mesh"]]["primitives"][0]["attributes"]
            n = Q.read_accessor(doc, b, prim["NORMAL"])
            n /= np.linalg.norm(n, axis=1, keepdims=True)
            self.assertLess(np.degrees(np.arccos(np.clip((n * nrm).sum(1), -1, 1))).max(), 1.0)
            self.assertTrue(np.allclose(Q.read_accessor(doc, b, prim["COLOR_0"]), col, atol=1e-6))
            self.assertEqual(doc["accessors"][prim["COLOR_0"]]["componentType"], Q.UBYTE)
            self.assertLess(np.abs(Q.read_accessor(doc, b, prim["TEXCOORD_0"]) - uv).max(), 1e-4)
            for bv in doc["bufferViews"]:
                self.assertEqual(bv["byteOffset"] % 4, 0)
                self.assertEqual(bv.get("byteStride", 4) % 4, 0)

    def test_plain_node(self):
        self.roundtrip([{"mesh": 0}])

    def test_node_with_trs(self):
        self.roundtrip([{"mesh": 0, "translation": [5, 1, -2], "rotation": [0, 0.3826834, 0, 0.9238795], "scale": [2, 2, 2]}])

    def test_node_with_children_gets_a_child(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "a.glb"), os.path.join(d, "b.glb")
            make_glb(src, [{"mesh": 0, "children": [1]}, {"name": "child"}])
            Q.main([src, dst])
            doc, _ = Q.read_glb(dst)
            self.assertNotIn("mesh", doc["nodes"][0])
            self.assertEqual(doc["nodes"][doc["nodes"][0]["children"][-1]]["mesh"], 0)


if __name__ == "__main__":
    unittest.main()
