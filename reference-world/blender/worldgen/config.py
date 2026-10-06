"""Preset loading. A preset JSON only needs the keys it changes; everything
else falls back to DEFAULTS (deep-merged)."""
import copy
import json
import os

PRESET_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "presets"))

DEFAULTS = {
    "name": "default",
    "seed": 7,
    "terrain": {
        "size_m": 2400.0,          # square terrain edge length
        "resolution": 1009,        # UE landscape friendly (1009 = 8x8 comps of 126 quads)
        "mountain_height_m": 400.0,
        "valley_width_m": 240.0,   # flat floor width
        "valley_slope_m": 520.0,   # distance over which the floor rises into mountains
        "meander_amp_m": 110.0,
        "meander_wavelength_m": 1100.0,
        "river_width_m": 18.0,
        "river_depth_m": 2.2,
        "hill_height_m": 45.0,
        "backdrop_height_m": 320.0, # extra massif closing the valley at the far (+Y) end
        "skirt_height_m": 1500.0,  # distant ranges outside the main terrain
        "warp_strength": 0.35,
        "thermal_passes": 22,
    },
    "biome": {
        "water_level_m": -0.7,
        "treeline_m": 260.0,
        "snowline_m": 380.0,
        "conifer_ratio": 0.72,
        "tree_count": 9000,
        "bush_count": 4000,
        "rock_count": 1600,
        "grass_count": 420000,
        "flower_ratio": 0.08,
        "grass_radius_m": 120.0,   # grass clumps are only scattered near the camera path
        "camera_clearance_m": 14.0,
    },
    # linear albedos, kept in physically plausible ranges (grass ~0.1, rock ~0.15)
    "palette": {
        "grass": [0.050, 0.095, 0.018],
        "grass_dry": [0.20, 0.15, 0.045],
        "soil": [0.085, 0.060, 0.038],
        "rock": [0.17, 0.15, 0.13],
        "rock_dark": [0.045, 0.040, 0.036],
        "moss": [0.055, 0.085, 0.020],
        "snow": [0.80, 0.82, 0.86],
        "needles": [0.022, 0.048, 0.016],
        "leaves": [0.055, 0.115, 0.020],
        "leaves_alt": [0.20, 0.14, 0.025],
        "bark": [0.050, 0.036, 0.026],
        "flowers": [[0.85, 0.82, 0.75], [0.85, 0.55, 0.05], [0.35, 0.18, 0.65]],
        "water": [0.02, 0.05, 0.05],
    },
    "lighting": {
        "sun_elevation_deg": 14.0,
        "sun_azimuth_deg": -163.0,  # compass bearing; 0 = down the valley (+Y), +-180 = behind the camera
        "sun_strength": 5.5,
        "sun_color": [1.0, 0.70, 0.42],
        "sun_angle_deg": 0.6,
        "sky_type": "MULTIPLE_SCATTERING",
        "sky_strength": 0.10,      # sky texture is physically scaled; ~0.1 keeps a 3:1 sun/sky ratio
        "air_density": 1.0,
        "aerosol_density": 1.6,
        "ozone_density": 1.0,
        "haze_color": [0.62, 0.52, 0.42],
        "haze_amount": 0.72,
        "mist_start_m": 30.0,
        "mist_depth_m": 6500.0,
        "clouds": True,
        "cloud_coverage": 0.45,
        "cloud_color": [1.0, 0.70, 0.50],
        "exposure": 0.9,
        "look": "AgX - Medium High Contrast",
        "bloom": 0.2,
        "vignette": 0.25,
    },
    "camera": {
        "lens_mm": 32.0,
        "sensor_mm": 36.0,
        "fstop": 6.3,
        "frames": 240,
        "fps": 24,
        # path keys: y along the valley (m), height above ground/water (m),
        # x offset from the river centre (m), yaw offset (deg), pitch (deg)
        "path": [
            {"frame": 1,   "y": -1020.0, "height": 4.5,  "x": -6.0, "yaw": 4.0,  "pitch": 3.0},
            {"frame": 120, "y": -860.0,  "height": 11.0, "x": 4.0,  "yaw": -2.0, "pitch": 4.0},
            {"frame": 240, "y": -700.0,  "height": 24.0, "x": 10.0, "yaw": -6.0, "pitch": 3.0},
        ],
        "stills": [
            {"name": "hero",     "frame": 1},
            {"name": "mid",      "frame": 120},
            {"name": "end",      "frame": 240},
        ],
    },
    "render": {
        "resolution": [1920, 1080],
        "samples": 96,
        "video_resolution": [960, 540],
        "video_samples": 24,
    },
}


def _merge(base, over):
    out = copy.deepcopy(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def load_preset(name_or_path: str) -> dict:
    path = name_or_path
    if not os.path.isfile(path):
        path = os.path.join(PRESET_DIR, name_or_path + ".json")
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    cfg = DEFAULTS
    # a preset may inherit from another preset
    parent = data.pop("extends", None)
    if parent:
        cfg = load_preset(parent)
    return _merge(cfg, data)
