"""Derive a starting preset from reference frames (screenshots of the
reference video). It estimates palette, warmth, haze and contrast. It does
not guess terrain shape. Review the result and fine-tune by eye.

    python tools/match_reference.py ref1.png ref2.jpg --name my_reference
    python blender/build_world.py --preset my_reference --preview

Runs with the same Python that has `bpy` installed (it uses Blender's image
loader, so PNG/JPG/EXR/WebP all work), or inside Blender with
`blender -b -P tools/match_reference.py -- <frames...>`.
"""
import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender"))


def load_linear(path):
    import bpy
    img = bpy.data.images.load(os.path.abspath(path), check_existing=False)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, img.channels)[..., :3]
    bpy.data.images.remove(img)
    px = px[::-1]  # Blender stores rows bottom-up; make row 0 the top
    if not path.lower().endswith((".exr", ".hdr")):
        px = np.where(px <= 0.04045, px / 12.92, ((px + 0.055) / 1.055) ** 2.4)  # sRGB -> linear
    step = max(1, w // 480)
    return px[::step, ::step]


def lum(c):
    return c[..., 0] * 0.2126 + c[..., 1] * 0.7152 + c[..., 2] * 0.0722


def norm_albedo(rgb, target):
    rgb = np.maximum(np.asarray(rgb, dtype=np.float64), 1e-4)
    return list(np.round(rgb * (target / max(lum(rgb), 1e-4)), 4))


def analyse(frames):
    sky, horizon, ground, highlights, all_px = [], [], [], [], []
    for f in frames:
        h = f.shape[0]
        sky.append(f[: int(h * 0.22)].reshape(-1, 3))
        horizon.append(f[int(h * 0.3): int(h * 0.5)].reshape(-1, 3))
        ground.append(f[int(h * 0.65):].reshape(-1, 3))
        # sunlit surfaces: brightest pixels below the sky band
        below = f[int(h * 0.3):]
        L = lum(below)
        highlights.append(below[L > np.percentile(L, 97)].reshape(-1, 3))
        all_px.append(below.reshape(-1, 3))
    sky, horizon, ground, highlights, all_px = (np.concatenate(a) for a in (sky, horizon, ground, highlights, all_px))

    # green-ish ground pixels give the vegetation hue; others the soil/dry tone
    g = ground[(ground[:, 1] > ground[:, 0] * 0.9) & (ground[:, 1] > ground[:, 2])]
    grass = np.median(g, 0) if len(g) > 50 else np.median(ground, 0)
    dry_px = ground[ground[:, 0] > ground[:, 1] * 0.95]
    dry = np.median(dry_px, 0) if len(dry_px) > 50 else grass * [1.6, 1.1, 0.6]

    hl = np.median(highlights, 0)
    sun_color = hl / max(hl.max(), 1e-4)

    Lall = np.log2(np.maximum(lum(all_px), 1e-5))
    spread = float(np.percentile(Lall, 95) - np.percentile(Lall, 5))
    look = "AgX - Base Contrast" if spread < 4.5 else ("AgX - Medium High Contrast" if spread < 6.5 else "AgX - Punchy")

    # haze: how washed-out the horizon band is relative to the sky
    h_contrast = float(np.std(np.log2(np.maximum(lum(horizon), 1e-5))))
    haze_amount = float(np.clip(1.15 - h_contrast * 0.45, 0.25, 0.95))
    haze_color = np.median(horizon[lum(horizon) > np.percentile(lum(horizon), 60)], 0)

    snow_frac = float(((all_px.min(1) > 0.55) & (all_px.max(1) - all_px.min(1) < 0.12)).mean())
    warmth = float(sun_color[0] - sun_color[2])

    return {
        "palette": {
            "grass": norm_albedo(grass, 0.075),
            "grass_dry": norm_albedo(dry, 0.14),
            "leaves": norm_albedo(grass * [0.9, 1.05, 0.9], 0.09),
        },
        "lighting": {
            "sun_color": [round(float(c), 3) for c in sun_color],
            # warm highlights suggest a low sun
            "sun_elevation_deg": round(float(np.interp(warmth, [0.1, 0.6], [35.0, 6.0])), 1),
            "haze_color": norm_albedo(haze_color, 0.5),
            "haze_amount": round(haze_amount, 2),
            "look": look,
        },
        "biome": {"snowline_m": round(float(np.interp(snow_frac, [0.0, 0.15], [420.0, 250.0])), 0)},
        "_analysis": {"contrast_stops": round(spread, 2), "horizon_log_std": round(h_contrast, 3),
                      "snow_fraction": round(snow_frac, 4), "sky_median": [round(float(c), 3) for c in np.median(sky, 0)]},
    }


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    p = argparse.ArgumentParser()
    p.add_argument("frames", nargs="+")
    p.add_argument("--name", default="reference_match")
    p.add_argument("--base", default="golden_valley")
    a = p.parse_args(argv)
    frames = [load_linear(f) for f in a.frames]
    est = analyse(frames)
    preset = {"name": a.name, "extends": a.base, **est}
    out = os.path.normpath(os.path.join(HERE, "..", "presets", a.name + ".json"))
    with open(out, "w") as fh:
        json.dump(preset, fh, indent=2)

    # swatch strip for a quick visual check against the reference
    from worldgen.png import write_png
    cols = [est["palette"]["grass"], est["palette"]["grass_dry"], est["lighting"]["sun_color"],
            est["lighting"]["haze_color"], est["_analysis"]["sky_median"]]
    strip = np.concatenate([np.tile(np.array(c, dtype=np.float64), (64, 64, 1)) for c in cols], 1)
    strip = np.clip(strip / max(strip.max(), 1e-4), 0, 1) ** (1 / 2.2)
    write_png(out.replace(".json", "_swatches.png"), np.round(strip * 255), 8)
    print(json.dumps(preset, indent=2))
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main()
