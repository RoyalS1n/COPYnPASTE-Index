"""Shrink static GLB meshes with KHR_mesh_quantization (about half the size, no visible change).

    python tools/quantize_glb.py in.glb out.glb          (or: --in-place a.glb b.glb ...)

Per vertex: positions become 16-bit integers of a uniform per-mesh scale (applied by the node transform, so
normals are not distorted), normals and tangents 8-bit, texture coordinates 16-bit when they lie in [-1, 1],
colours 8-bit when every value is a multiple of 1/255 (exact) and 16-bit otherwise. Typical position error is
below 1/30000 of the mesh size (1.2 mm on an 80 m keep). Skinned or morphing meshes, sparse accessors and
attributes already stored as integers are left unchanged. The engine (cgltf) and Blender both read the result.
"""
import json
import struct
import sys

import numpy as np

FLOAT, BYTE, UBYTE, SHORT, USHORT, UINT = 5126, 5120, 5121, 5122, 5123, 5125
DTYPE = {FLOAT: np.float32, BYTE: np.int8, UBYTE: np.uint8, SHORT: np.int16, USHORT: np.uint16, UINT: np.uint32}
NORM_MAX = {BYTE: 127.0, UBYTE: 255.0, SHORT: 32767.0, USHORT: 65535.0}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
EXT = "KHR_mesh_quantization"


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        raise ValueError("%s is not a glTF 2 binary" % path)
    off, doc, binary = 12, None, b""
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8:off + 8 + clen]
        if ctype == 0x4E4F534A:
            doc = json.loads(chunk)
        elif ctype == 0x004E4942:
            binary = bytes(chunk)
        off += 8 + clen
    return doc, binary


def write_glb(path, doc, binary):
    js = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    js += b" " * (-len(js) % 4)
    binary += b"\0" * (-len(binary) % 4)
    total = 12 + 8 + len(js) + (8 + len(binary) if binary else 0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        if binary:
            f.write(struct.pack("<II", len(binary), 0x004E4942) + binary)


def read_accessor(doc, binary, i):
    a = doc["accessors"][i]
    if "sparse" in a or "bufferView" not in a:
        raise ValueError("sparse accessor")
    bv = doc["bufferViews"][a["bufferView"]]
    if bv.get("buffer", 0) != 0:
        raise ValueError("external buffer")
    dt, n = np.dtype(DTYPE[a["componentType"]]), NCOMP[a["type"]]
    stride = bv.get("byteStride") or dt.itemsize * n
    start = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    raw = np.frombuffer(binary, np.uint8, count=stride * (a["count"] - 1) + dt.itemsize * n, offset=start)
    rows = np.lib.stride_tricks.as_strided(raw, (a["count"], dt.itemsize * n), (stride, 1))
    out = np.ascontiguousarray(rows).view(dt).reshape(a["count"], n).astype(np.float64)
    if a.get("normalized") and a["componentType"] in NORM_MAX:
        out = np.maximum(out / NORM_MAX[a["componentType"]], -1.0)
    return out


def quantize(values, ctype, normalized=True):
    m = NORM_MAX[ctype]
    lo = -m if ctype in (BYTE, SHORT) else 0
    return np.clip(np.round(values * m), lo, m).astype(DTYPE[ctype])


def encode(name, v):
    """(array, componentType, normalized) for one attribute, or None to keep it as it is."""
    if name == "NORMAL":
        v = v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-12)
        return quantize(v, BYTE), BYTE, True
    if name == "TANGENT":
        xyz = v[:, :3] / np.maximum(np.linalg.norm(v[:, :3], axis=1, keepdims=True), 1e-12)
        return quantize(np.hstack([xyz, np.sign(v[:, 3:4]) + (v[:, 3:4] == 0)]), BYTE), BYTE, True
    if name.startswith("TEXCOORD_"):
        if v.size and v.min() >= 0.0 and v.max() <= 1.0:
            return quantize(v, USHORT), USHORT, True
        if v.size and v.min() >= -1.0 and v.max() <= 1.0:
            return quantize(v, SHORT), SHORT, True
        return None
    if name.startswith("COLOR_"):
        if np.all(np.abs(v * 255.0 - np.round(v * 255.0)) < 1e-3):
            return quantize(v, UBYTE), UBYTE, True
        return quantize(v, USHORT), USHORT, True
    return None


def node_trs(node):
    if "matrix" in node:
        return None
    return (node.get("translation", [0, 0, 0]), node.get("rotation", [0, 0, 0, 1]), node.get("scale", [1, 1, 1]))


def rotate(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    v = np.asarray(v, float)
    return v + 2.0 * np.cross(u, np.cross(u, v) + w * v)


def quantize_glb(doc, binary):
    nodes = doc.get("nodes", [])
    skinned = {n["mesh"] for n in nodes if "mesh" in n and "skin" in n}
    views, chunks, size = [], [], [0]

    def add_view(arr, stride=None, target=None):
        size[0] += -size[0] % 4
        data = arr.tobytes()
        if stride:   # pad rows to a 4-byte stride (vertex attributes)
            rows = arr.reshape(len(arr), -1).view(np.uint8).reshape(len(arr), -1)
            pad = np.zeros((len(arr), stride - rows.shape[1]), np.uint8)
            data = np.hstack([rows, pad]).tobytes()
        chunks.append((size[0], data))
        bv = {"buffer": 0, "byteOffset": size[0], "byteLength": len(data)}
        if stride:
            bv["byteStride"] = stride
        if target:
            bv["target"] = target
        views.append(bv)
        size[0] += len(data)
        return len(views) - 1

    # keep every existing view (indices, images, animation, untouched attributes) so references stay valid; the
    # replaced attribute views are dropped below by rebuilding the buffer from what is still referenced
    replaced, dropped, dequant = {}, set(), {}   # (mesh, old accessor) -> new accessor
    for mi, mesh in enumerate(doc.get("meshes", [])):
        prims = mesh.get("primitives", [])
        if mi in skinned or any("targets" in p for p in prims):
            continue
        try:
            pos = [read_accessor(doc, binary, p["attributes"]["POSITION"]) for p in prims if "POSITION" in p["attributes"]]
        except ValueError:
            continue
        if not pos or any(doc["accessors"][p["attributes"]["POSITION"]]["componentType"] != FLOAT for p in prims):
            continue
        allp = np.vstack(pos)
        lo, hi = allp.min(0), allp.max(0)
        center, half = (lo + hi) / 2.0, max(float((hi - lo).max()) / 2.0, 1e-9)
        dequant[mi] = (center, half)
        for p in prims:
            for name, ai in list(p["attributes"].items()):
                if (mi, ai) in replaced:
                    p["attributes"][name] = replaced[(mi, ai)]
                    continue
                acc = doc["accessors"][ai]
                if acc["componentType"] != FLOAT and name != "POSITION" and not name.startswith("COLOR_"):
                    continue
                try:
                    v = read_accessor(doc, binary, ai)
                except ValueError:
                    continue
                if name == "POSITION":
                    arr, ctype, norm = quantize((v - center) / half, SHORT), SHORT, True
                else:
                    enc = encode(name, v)
                    if enc is None:
                        continue
                    arr, ctype, norm = enc
                comps = arr.shape[1]
                stride = ((arr.dtype.itemsize * comps + 3) // 4) * 4
                new = {"bufferView": add_view(arr, stride, 34962), "componentType": ctype, "normalized": norm,
                       "count": len(arr), "type": acc["type"]}
                if name == "POSITION":
                    new["normalized"] = True
                    new["min"] = arr.min(0).tolist()
                    new["max"] = arr.max(0).tolist()
                doc["accessors"].append(new)
                replaced[(mi, ai)] = len(doc["accessors"]) - 1
                dropped.add(ai)
                p["attributes"][name] = replaced[(mi, ai)]
    if not dequant:
        return doc, binary, 0

    # the node holding a quantized mesh applies translate(center) * scale(half)
    for ni, node in enumerate(list(nodes)):
        if node.get("mesh") not in dequant:
            continue
        center, half = dequant[node["mesh"]]
        trs = node_trs(node)
        if trs is None or node.get("children"):
            child = {"name": node.get("name", "") + "_q", "mesh": node.pop("mesh"),
                     "translation": center.tolist(), "scale": [half] * 3}
            nodes.append(child)
            node.setdefault("children", []).append(len(nodes) - 1)
            continue
        t, r, s = trs
        node["translation"] = (np.asarray(t) + rotate(r, np.asarray(s) * center)).tolist()
        node["scale"] = (np.asarray(s) * half).tolist()

    # rebuild the buffer: old views still referenced, then the new ones
    old_views = doc.get("bufferViews", [])
    n_old = len(doc["accessors"]) - len(chunks)
    # an old accessor stays if anything other than a quantized attribute still uses it
    still = {v for m in doc.get("meshes", []) for p in m.get("primitives", []) for v in p["attributes"].values()}
    dropped -= still
    used = {a["bufferView"] for i, a in enumerate(doc["accessors"][:n_old]) if i not in dropped and "bufferView" in a}
    for im in doc.get("images", []):
        if "bufferView" in im:
            used.add(im["bufferView"])
    remap, out_views, out = {}, [], bytearray()
    for i, bv in enumerate(old_views):
        if i not in used:
            continue
        out += b"\0" * (-len(out) % 4)
        start = bv.get("byteOffset", 0)
        nb = dict(bv, byteOffset=len(out))
        out += binary[start:start + bv["byteLength"]]
        remap[i] = len(out_views)
        out_views.append(nb)
    base = len(out_views)
    for k, (off, data) in enumerate(chunks):
        out += b"\0" * (-len(out) % 4)
        nb = dict(views[k], byteOffset=len(out))
        out += data
        out_views.append(nb)
    # accessors: drop the replaced ones and renumber every reference
    new_index, accessors = {}, []
    for i, a in enumerate(doc["accessors"]):
        if i in dropped:
            continue
        a = dict(a)
        if "bufferView" in a:
            a["bufferView"] = remap[a["bufferView"]] if i < n_old else base + (a["bufferView"])
        new_index[i] = len(accessors)
        accessors.append(a)
    for mesh in doc.get("meshes", []):
        for p in mesh.get("primitives", []):
            p["attributes"] = {k: new_index[v] for k, v in p["attributes"].items()}
            if "indices" in p:
                p["indices"] = new_index[p["indices"]]
            for t in p.get("targets", []):
                for k in t:
                    t[k] = new_index[t[k]]
    for sk in doc.get("skins", []):
        if "inverseBindMatrices" in sk:
            sk["inverseBindMatrices"] = new_index[sk["inverseBindMatrices"]]
    for an in doc.get("animations", []):
        for s in an.get("samplers", []):
            s["input"], s["output"] = new_index[s["input"]], new_index[s["output"]]
    for im in doc.get("images", []):
        if "bufferView" in im:
            im["bufferView"] = remap[im["bufferView"]]
    doc["accessors"], doc["bufferViews"] = accessors, out_views
    doc["buffers"] = [{"byteLength": len(out)}]
    for key in ("extensionsUsed", "extensionsRequired"):
        doc[key] = sorted(set(doc.get(key, [])) | {EXT})
    return doc, bytes(out), len(dequant)


def main(argv):
    if len(argv) >= 2 and argv[0] == "--in-place":
        pairs = [(p, p) for p in argv[1:]]
    elif len(argv) == 2:
        pairs = [(argv[0], argv[1])]
    else:
        print(__doc__)
        return 2
    for src, dst in pairs:
        doc, binary = read_glb(src)
        before = 12 + len(json.dumps(doc)) + len(binary)
        doc, binary, n = quantize_glb(doc, binary)
        write_glb(dst, doc, binary)
        after = 12 + len(json.dumps(doc)) + len(binary)
        print("%s: %d meshes quantized, %d -> %d KB" % (dst, n, before // 1024, after // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
