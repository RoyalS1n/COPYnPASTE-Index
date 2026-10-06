"""Minimal dependency-free PNG writer (8/16-bit grey, 8-bit RGB)."""
import struct
import zlib

import numpy as np


def _chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_png(path, arr, bit_depth=8):
    """arr: (H, W) grey or (H, W, 3) RGB, values already in integer range.
    Row 0 is written as the TOP of the image."""
    arr = np.asarray(arr)
    h, w = arr.shape[:2]
    color_type = 0 if arr.ndim == 2 else 2
    dtype = ">u2" if bit_depth == 16 else "u1"
    raw = arr.astype(dtype).reshape(h, -1)
    rows = b"".join(b"\x00" + raw[i].tobytes() for i in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, bit_depth, color_type, 0, 0, 0)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(_chunk(b"IHDR", ihdr))
        fh.write(_chunk(b"IDAT", zlib.compress(rows, 6)))
        fh.write(_chunk(b"IEND", b""))
