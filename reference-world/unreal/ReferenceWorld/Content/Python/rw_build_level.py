"""Build the Reference World level inside Unreal Editor (UE 5.3+).

Prerequisite: run the Blender pipeline with --export. It writes the meshes,
scatter and manifest into <project>/WorldData/.

Run in the editor: Tools > Execute Python Script... > this file, or in the
Output Log (Python mode):

    import rw_build_level; rw_build_level.main()                 # latest export
    import rw_build_level; rw_build_level.main("serene_meadow")  # a specific preset

What it does
  1. Imports every FBX in WorldData/meshes as Nanite static meshes.
  2. Creates master materials + instances driven by the preset palette.
  3. Creates /Game/ReferenceWorld/Maps/L_ReferenceWorld with sun, sky
     atmosphere, sky light, volumetric clouds, height fog, post process.
  4. Places terrain, distant ranges, water, and all scattered instances
     (one HISM component per mesh variant).
  5. Creates a CineCamera and LS_Flythrough level sequence matching the
     Blender camera move.
  6. Makes it playable: collision (complex on terrain/cliff/castle so stairs,
     floors and wall-walks are walkable; simple proxies on trees and rocks;
     none on grass, ferns, pebbles, bushes), castle lights, a PlayerStart at
     the foot of the castle road, invisible world bounds and the
     RWGameMode (first-person explorer, compiled from Source/).
"""
import json
import math
import os

import unreal

# set per preset by configure(): each Blender preset gets its own asset
# folder and map, so the castle valley and the meadow don't overwrite each other
ROOT = "/Game/ReferenceWorld/golden_valley"
MESH_DIR = ROOT + "/Meshes"
MAT_DIR = ROOT + "/Materials"
MAP_PATH = ROOT + "/Maps/L_golden_valley"
SEQ_DIR = ROOT + "/Cinematics"
PRESET = "golden_valley"


def configure(preset):
    global ROOT, MESH_DIR, MAT_DIR, MAP_PATH, SEQ_DIR, PRESET
    safe = "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in preset)
    PRESET = preset
    ROOT = f"/Game/ReferenceWorld/{safe}"
    MESH_DIR, MAT_DIR, SEQ_DIR = ROOT + "/Meshes", ROOT + "/Materials", ROOT + "/Cinematics"
    MAP_PATH = f"{ROOT}/Maps/L_{safe}"

# Optional caps to keep the editor responsive on modest GPUs (None = all)
INSTANCE_CAPS = {"grass": None, "trees": None, "bushes": None, "rocks": None}

mel = unreal.MaterialEditingLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary


def log(msg):
    unreal.log("[ReferenceWorld] " + str(msg))


def data_dir(preset=None):
    """WorldData/<preset>/ written by the Blender build. With no preset,
    use the most recently exported one."""
    root = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), "WorldData")
    if preset:
        return os.path.join(root, preset)
    if os.path.isfile(os.path.join(root, "manifest.json")):      # older single-folder layout
        return root
    subs = [os.path.join(root, d) for d in os.listdir(root)
            if os.path.isfile(os.path.join(root, d, "manifest.json"))] if os.path.isdir(root) else []
    if not subs:
        raise RuntimeError(f"no exports in {root} - run the Blender build with --export first")
    return max(subs, key=lambda d: os.path.getmtime(os.path.join(d, "manifest.json")))


def lc(rgb, a=1.0):
    return unreal.LinearColor(float(rgb[0]), float(rgb[1]), float(rgb[2]), a)


# --------------------------------------------------------------------------
# import
def import_meshes(mesh_dir):
    # UE 5.5+ routes FBX through Interchange by default; the legacy importer
    # honours the options below (custom normals, vertex colours, Nanite).
    try:
        unreal.SystemLibrary.execute_console_command(None, "Interchange.FeatureFlags.Import.FBX false")
    except Exception:
        pass
    tasks = []
    for fn in sorted(os.listdir(mesh_dir)):
        if not fn.lower().endswith(".fbx"):
            continue
        name = os.path.splitext(fn)[0]
        task = unreal.AssetImportTask()
        task.filename = os.path.join(mesh_dir, fn)
        task.destination_path = MESH_DIR
        task.destination_name = name
        task.automated = True
        task.replace_existing = True
        task.save = True
        ui = unreal.FbxImportUI()
        ui.import_mesh = True
        ui.import_materials = False
        ui.import_textures = False
        ui.import_as_skeletal = False
        ui.mesh_type_to_import = unreal.FBXImportType.FBXIT_STATIC_MESH
        sm = ui.static_mesh_import_data
        sm.combine_meshes = True
        sm.generate_lightmap_u_vs = False
        sm.auto_generate_collision = False  # UCX_ proxies come with trees/rocks; big meshes use complex
        sm.normal_import_method = unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS
        sm.vertex_color_import_option = unreal.VertexColorImportOption.REPLACE
        try:
            sm.build_nanite = True
        except Exception:
            pass
        task.options = ui
        tasks.append(task)
    asset_tools.import_asset_tasks(tasks)
    meshes = {}
    for t in tasks:
        path = f"{MESH_DIR}/{t.destination_name}"
        asset = unreal.load_asset(path)
        if asset is None:
            unreal.log_warning(f"[ReferenceWorld] import failed: {t.filename}")
            continue
        meshes[t.destination_name] = asset
        try:  # make sure Nanite is on even if the import option was ignored
            settings = asset.get_editor_property("nanite_settings")
            settings.enabled = True
            asset.set_editor_property("nanite_settings", settings)
        except Exception:
            pass
    log(f"imported {len(meshes)} meshes")
    return meshes


# --------------------------------------------------------------------------
# materials
def _new_material(name):
    path = f"{MAT_DIR}/{name}"
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    return asset_tools.create_asset(name, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())


def _expr(mat, cls, x, y, **props):
    e = mel.create_material_expression(mat, cls, x, y)
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def _vparam(mat, name, rgb, x, y):
    return _expr(mat, unreal.MaterialExpressionVectorParameter, x, y, parameter_name=name, default_value=lc(rgb))


def _sparam(mat, name, val, x, y):
    return _expr(mat, unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=float(val))


def _lerp(mat, a, b, alpha, x, y, a_out="", b_out="", alpha_out=""):
    e = _expr(mat, unreal.MaterialExpressionLinearInterpolate, x, y)
    mel.connect_material_expressions(a, a_out, e, "A")
    mel.connect_material_expressions(b, b_out, e, "B")
    mel.connect_material_expressions(alpha, alpha_out, e, "Alpha")
    return e


# --------------------------------------------------------------------------
# procedural HLSL (Custom nodes), so the castle and foliage in Unreal match
# the Blender look without baked textures

COURSES_HLSL = """
float2 sz = max(Size, float2(0.01, 0.01));
float2 p = UV / sz;
float row = floor(p.y);
p.x += frac(row * Offset);
float2 cell = floor(p);
float2 f = frac(p);
float2 d = min(f, 1.0 - f) * sz;
float e = min(d.x, d.y);
float mortar = 1.0 - smoothstep(Mortar * 0.5, Mortar * 0.5 + 0.012, e);
float rnd = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.5453);
return float3(rnd, mortar, e);
"""

COURSES_NORMAL_HLSL = """
struct RWCourses {
    float h(float2 uv, float2 sz, float m, float o) {
        float2 p = uv / sz;
        float row = floor(p.y);
        p.x += frac(row * o);
        float2 f = frac(p);
        float2 d = min(f, 1.0 - f) * sz;
        return smoothstep(m * 0.5, m * 0.5 + 0.04, min(d.x, d.y));
    }
};
RWCourses c;
float2 sz = max(Size, float2(0.01, 0.01));
float s = 0.006;
float h0 = c.h(UV, sz, Mortar, Offset);
float hx = c.h(UV + float2(s, 0), sz, Mortar, Offset);
float hy = c.h(UV + float2(0, s), sz, Mortar, Offset);
return normalize(float3((h0 - hx) / s * Strength, (h0 - hy) / s * Strength, 1.0));
"""

WIND_HLSL = """
float h = saturate(LocalPos.z / max(Height, 1.0));
float ph = Phase * 6.2831 + dot(WorldPos.xy, float2(0.0013, 0.0017));
float s = sin(T * Speed + ph) * 0.7 + sin(T * Speed * 2.31 + ph * 1.7) * 0.3;
return float3(0.8, 0.6, 0.0) * (s * Strength * h * h);
"""

WATER_NORMAL_HLSL = """
float2 p = WorldPos.xy * 0.01;
float2 g = float2(0, 0);
g += float2(0.8, 0.6) * cos(dot(p, float2(0.8, 0.6)) * 1.9 + T * 1.3) * 0.06;
g += float2(-0.4, 0.9) * cos(dot(p, float2(-0.4, 0.9)) * 3.7 + T * 2.1) * 0.035;
g += float2(0.9, -0.3) * cos(dot(p, float2(0.9, -0.3)) * 7.3 + T * 3.0) * 0.02;
g += float2(0.2, 1.0) * cos(dot(p, float2(0.2, 1.0)) * 13.0 + T * 4.2) * 0.012;
return normalize(float3(-g.x, -g.y, 1.0));
"""


def _custom(mat, code, inputs, x, y, out=unreal.CustomMaterialOutputType.CMOT_FLOAT3, desc="RW"):
    e = _expr(mat, unreal.MaterialExpressionCustom, x, y)
    e.set_editor_property("code", code)
    e.set_editor_property("output_type", out)
    e.set_editor_property("description", desc)
    pins = []
    for name in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        pins.append(ci)
    e.set_editor_property("inputs", pins)
    return e


def _mask(mat, src, x, y, r=False, g=False, b=False, a=False, src_out=""):
    e = _expr(mat, unreal.MaterialExpressionComponentMask, x, y, r=r, g=g, b=b, a=a)
    mel.connect_material_expressions(src, src_out, e, "")
    return e


def _op(mat, cls, a, b, x, y, a_out="", b_out=""):
    e = _expr(mat, cls, x, y)
    mel.connect_material_expressions(a, a_out, e, "A")
    if isinstance(b, (int, float)):
        e.set_editor_property("const_b", float(b))
    else:
        mel.connect_material_expressions(b, b_out, e, "B")
    return e


def master_terrain(pal):
    """Vertex colour masks from Blender: R grass, G rock, B snow, A bare
    (wet banks, road, courtyard). Adds macro variation and rock strata."""
    m = _new_material("M_RW_Terrain")
    vc = _expr(m, unreal.MaterialExpressionVertexColor, -1400, 0)
    grass = _vparam(m, "Grass", pal["grass"], -1400, -400)
    dry = _vparam(m, "GrassDry", pal["grass_dry"], -1400, -250)
    rock = _vparam(m, "Rock", pal["rock"], -1400, 250)
    snow = _vparam(m, "Snow", pal["snow"], -1400, 400)
    wet = _vparam(m, "Wet", [c * 0.45 for c in pal["soil"]], -1400, 550)
    noise = _expr(m, unreal.MaterialExpressionNoise, -1200, -150, scale=0.0004, levels=4,
                  output_min=0.0, output_max=1.0)
    g = _lerp(m, grass, dry, noise, -1000, -300)
    # rock strata: sine bands on world height, broken up by noise
    wp = _expr(m, unreal.MaterialExpressionWorldPosition, -1400, 700)
    z = _mask(m, wp, -1250, 700, b=True)
    zn = _op(m, unreal.MaterialExpressionMultiply, z, 0.0035, -1100, 700)
    bn = _expr(m, unreal.MaterialExpressionNoise, -1250, 850, scale=0.002, levels=3, output_min=0.0, output_max=3.0)
    za = _op(m, unreal.MaterialExpressionAdd, zn, bn, -950, 750)
    band = _expr(m, unreal.MaterialExpressionSine, -800, 750)
    mel.connect_material_expressions(za, "", band, "")
    band_s = _op(m, unreal.MaterialExpressionMultiply, band, 0.15, -650, 750)
    band_o = _op(m, unreal.MaterialExpressionAdd, band_s, 0.9, -500, 750)
    rock_b = _op(m, unreal.MaterialExpressionMultiply, rock, band_o, -400, 600)
    # steep faces read as rock even between mask samples; ledges inside rock
    # catch vegetation (world-space normal z)
    nz = _mask(m, _expr(m, unreal.MaterialExpressionVertexNormalWS, -1400, 1000), -1250, 1000, b=True)
    steep = _op(m, unreal.MaterialExpressionSubtract, 0.86, nz, -1100, 1000)
    steep = _op(m, unreal.MaterialExpressionMultiply, steep, 6.0, -1000, 1000)
    steep_s = _expr(m, unreal.MaterialExpressionSaturate, -900, 1000)
    mel.connect_material_expressions(steep, "", steep_s, "")
    rock_mask = _expr(m, unreal.MaterialExpressionMax, -800, 1000)
    mel.connect_material_expressions(_mask(m, vc, -1000, 900, g=True), "", rock_mask, "A")
    mel.connect_material_expressions(steep_s, "", rock_mask, "B")
    ledge = _op(m, unreal.MaterialExpressionSubtract, nz, 0.78, -1100, 1150)
    ledge = _op(m, unreal.MaterialExpressionMultiply, ledge, 8.0, -1000, 1150)
    ledge_s = _expr(m, unreal.MaterialExpressionSaturate, -900, 1150)
    mel.connect_material_expressions(ledge, "", ledge_s, "")
    rock_b = _lerp(m, rock_b, _vparam(m, "Moss", pal["moss"], -800, 1250), ledge_s, -650, 1100)
    b = _lerp(m, g, rock_b, rock_mask, -800, -100)
    b = _lerp(m, b, snow, vc, -600, 0, alpha_out="B")
    b = _lerp(m, b, wet, vc, -400, 100, alpha_out="A")
    macro = _expr(m, unreal.MaterialExpressionNoise, -400, 300, scale=0.00015, levels=3,
                  output_min=0.82, output_max=1.12)
    b = _op(m, unreal.MaterialExpressionMultiply, b, macro, -200, 100)
    mel.connect_material_property(b, "", unreal.MaterialProperty.MP_BASE_COLOR)
    r_dry = _sparam(m, "Roughness", 0.9, -400, 450)
    r_wet = _sparam(m, "WetRoughness", 0.7, -400, 550)
    r = _lerp(m, r_dry, r_wet, vc, -200, 500, alpha_out="A")
    mel.connect_material_property(r, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(m)
    return m


def master_foliage():
    """Two-sided foliage: per-instance colour variation, plus wind (world
    position offset) that grows with height above the mesh pivot."""
    m = _new_material("M_RW_Foliage")
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    m.set_editor_property("two_sided", True)
    a = _vparam(m, "ColorA", [0.05, 0.1, 0.02], -800, -200)
    b = _vparam(m, "ColorB", [0.15, 0.12, 0.03], -800, 0)
    rnd = _expr(m, unreal.MaterialExpressionPerInstanceRandom, -800, 200)
    var = _sparam(m, "Variation", 0.6, -800, 300)
    mul = _expr(m, unreal.MaterialExpressionMultiply, -600, 250)
    mel.connect_material_expressions(rnd, "", mul, "A")
    mel.connect_material_expressions(var, "", mul, "B")
    col = _lerp(m, a, b, mul, -400, 0)
    # per-vertex shading baked by Blender (vertex colour R = leafvar / 1.5;
    # meshes without colours read white): darker card bases, warmer lit tips
    vc = _expr(m, unreal.MaterialExpressionVertexColor, -800, -400)
    lv = _op(m, unreal.MaterialExpressionMultiply, _mask(m, vc, -650, -400, r=True), 1.5, -500, -400)
    col = _op(m, unreal.MaterialExpressionMultiply, col, lv, -300, -100)
    tip = _op(m, unreal.MaterialExpressionSubtract, lv, 1.0, -350, -500)
    tip = _op(m, unreal.MaterialExpressionMultiply, tip, 1.2, -250, -500)
    tip_s = _expr(m, unreal.MaterialExpressionSaturate, -150, -500)
    mel.connect_material_expressions(tip, "", tip_s, "")
    col = _lerp(m, col, _vparam(m, "TipColor", [0.32, 0.36, 0.06], -300, -650), tip_s, -100, -200)
    mel.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    sss = _expr(m, unreal.MaterialExpressionMultiply, -200, 200)
    mel.connect_material_expressions(col, "", sss, "A")
    sss.set_editor_property("const_b", 0.6)
    mel.connect_material_property(sss, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    mel.connect_material_property(_sparam(m, "Roughness", 0.6, -200, 350), "", unreal.MaterialProperty.MP_ROUGHNESS)
    wind = _custom(m, WIND_HLSL, ["LocalPos", "WorldPos", "T", "Phase", "Strength", "Height", "Speed"], -300, 600,
                   desc="RW wind")
    mel.connect_material_expressions(_expr(m, unreal.MaterialExpressionPreSkinnedPosition, -700, 500), "", wind, "LocalPos")
    mel.connect_material_expressions(_expr(m, unreal.MaterialExpressionWorldPosition, -700, 600), "", wind, "WorldPos")
    mel.connect_material_expressions(_expr(m, unreal.MaterialExpressionTime, -700, 700), "", wind, "T")
    mel.connect_material_expressions(rnd, "", wind, "Phase")
    mel.connect_material_expressions(_sparam(m, "WindStrength", 8.0, -700, 800), "", wind, "Strength")
    mel.connect_material_expressions(_sparam(m, "WindHeight", 1500.0, -700, 900), "", wind, "Height")
    mel.connect_material_expressions(_sparam(m, "WindSpeed", 1.6, -700, 1000), "", wind, "Speed")
    mel.connect_material_property(wind, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    mel.recompile_material(m)
    return m


def master_courses():
    """Coursed masonry / roof tiles / planks on the castle's metre-scale
    UVs: per-block colour, recessed mortar with bevelled edges, weathering
    noise, and grime at the foot of walls (UV v = height above courtyard)."""
    m = _new_material("M_RW_Courses")
    tc = _expr(m, unreal.MaterialExpressionTextureCoordinate, -1600, 0, coordinate_index=0)
    size = _expr(m, unreal.MaterialExpressionAppendVector, -1400, 150)
    mel.connect_material_expressions(_sparam(m, "BlockWidth", 0.95, -1600, 120), "", size, "A")
    mel.connect_material_expressions(_sparam(m, "BlockHeight", 0.44, -1600, 220), "", size, "B")
    mortar_w = _sparam(m, "MortarWidth", 0.014, -1600, 320)
    offset = _sparam(m, "RowOffset", 0.5, -1600, 420)
    courses = _custom(m, COURSES_HLSL, ["UV", "Size", "Mortar", "Offset"], -1200, 0, desc="RW courses")
    nrm = _custom(m, COURSES_NORMAL_HLSL, ["UV", "Size", "Mortar", "Offset", "Strength"], -1200, 400,
                  desc="RW courses normal")
    for node in (courses, nrm):
        mel.connect_material_expressions(tc, "", node, "UV")
        mel.connect_material_expressions(size, "", node, "Size")
        mel.connect_material_expressions(mortar_w, "", node, "Mortar")
        mel.connect_material_expressions(offset, "", node, "Offset")
    mel.connect_material_expressions(_sparam(m, "NormalStrength", 0.03, -1600, 520), "", nrm, "Strength")
    rnd = _mask(m, courses, -1000, -100, r=True)
    mortar = _mask(m, courses, -1000, 50, g=True)
    base = _lerp(m, _vparam(m, "ColorB", [0.15, 0.13, 0.1], -900, -300),
                 _vparam(m, "ColorA", [0.25, 0.21, 0.17], -900, -200), rnd, -750, -200)
    weather = _expr(m, unreal.MaterialExpressionNoise, -900, -50, scale=0.003, levels=4, output_min=0.7, output_max=1.1)
    base = _op(m, unreal.MaterialExpressionMultiply, base, weather, -600, -150)
    base = _lerp(m, base, _vparam(m, "MortarColor", [0.12, 0.1, 0.08], -750, 50), mortar, -450, -100)
    v = _mask(m, tc, -1000, 250, g=True)
    foot = _expr(m, unreal.MaterialExpressionOneMinus, -700, 250)
    mel.connect_material_expressions(_op(m, unreal.MaterialExpressionDivide, v, 3.5, -850, 300), "", foot, "")
    foot_s = _expr(m, unreal.MaterialExpressionSaturate, -560, 250)
    mel.connect_material_expressions(foot, "", foot_s, "")
    grime = _op(m, unreal.MaterialExpressionMultiply, foot_s, _sparam(m, "Grime", 0.5, -700, 350), -420, 250)
    base = _lerp(m, base, _vparam(m, "GrimeColor", [0.05, 0.06, 0.03], -450, 150), grime, -250, 0)
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = _lerp(m, _sparam(m, "Roughness", 0.85, -450, 400), _expr(m, unreal.MaterialExpressionConstant, -450, 480, r=0.95),
                  mortar, -250, 400)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(_sparam(m, "Specular", 0.35, -250, 550), "", unreal.MaterialProperty.MP_SPECULAR)
    mel.connect_material_property(nrm, "", unreal.MaterialProperty.MP_NORMAL)
    mel.recompile_material(m)
    return m


def master_water():
    """Opaque river surface with animated ripple normals; Lumen supplies
    the reflections."""
    m = _new_material("M_RW_Water")
    mel.connect_material_property(_vparam(m, "Color", [0.02, 0.05, 0.05], -600, 0), "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(_sparam(m, "Roughness", 0.04, -600, 150), "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(_sparam(m, "Specular", 0.5, -600, 250), "", unreal.MaterialProperty.MP_SPECULAR)
    n = _custom(m, WATER_NORMAL_HLSL, ["WorldPos", "T"], -300, 400, desc="RW ripples")
    mel.connect_material_expressions(_expr(m, unreal.MaterialExpressionWorldPosition, -600, 400), "", n, "WorldPos")
    mel.connect_material_expressions(_expr(m, unreal.MaterialExpressionTime, -600, 500), "", n, "T")
    mel.connect_material_property(n, "", unreal.MaterialProperty.MP_NORMAL)
    mel.recompile_material(m)
    return m


def master_lit():
    m = _new_material("M_RW_Lit")
    col = _vparam(m, "Color", [0.2, 0.2, 0.2], -600, 0)
    mel.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(_sparam(m, "Roughness", 0.85, -600, 200), "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(_sparam(m, "Specular", 0.4, -600, 300), "", unreal.MaterialProperty.MP_SPECULAR)
    # emissive (lit castle windows); 0 strength for everything else
    emi = _expr(m, unreal.MaterialExpressionMultiply, -300, 400)
    mel.connect_material_expressions(_vparam(m, "EmissiveColor", [1.0, 0.55, 0.22], -600, 400), "", emi, "A")
    mel.connect_material_expressions(_sparam(m, "EmissiveStrength", 0.0, -600, 550), "", emi, "B")
    mel.connect_material_property(emi, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(m)
    return m


def instance(parent, name, vectors=None, scalars=None):
    path = f"{MAT_DIR}/{name}"
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    mi = asset_tools.create_asset(name, MAT_DIR, unreal.MaterialInstanceConstant,
                                  unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_parent(mi, parent)
    for k, v in (vectors or {}).items():
        mel.set_material_instance_vector_parameter_value(mi, k, lc(v))
    for k, v in (scalars or {}).items():
        mel.set_material_instance_scalar_parameter_value(mi, k, float(v))
    return mi


def build_materials(pal):
    terrain = master_terrain(pal)
    fol = master_foliage()
    lit = master_lit()
    crs = master_courses()
    water = master_water()
    mats = {
        "RW_Terrain": terrain,
        "RW_Needles": instance(fol, "MI_RW_Needles", {"ColorA": pal["needles"], "ColorB": [c * 1.5 for c in pal["needles"]]}, {"Variation": 0.6, "WindStrength": 10.0, "WindHeight": 1800.0}),
        "RW_Leaves": instance(fol, "MI_RW_Leaves", {"ColorA": pal["leaves"], "ColorB": pal["leaves_alt"]}, {"Variation": 0.45, "WindStrength": 16.0, "WindHeight": 1400.0}),
        "RW_Bush": instance(fol, "MI_RW_Bush", {"ColorA": [c * 0.85 for c in pal["leaves"]], "ColorB": pal["needles"]}, {"Variation": 0.7, "WindStrength": 4.0, "WindHeight": 250.0}),
        "RW_Grass": instance(fol, "MI_RW_Grass", {"ColorA": pal["grass"], "ColorB": pal["grass_dry"]}, {"Variation": 0.8, "Roughness": 0.55, "WindStrength": 6.0, "WindHeight": 60.0, "WindSpeed": 2.2}),
        "RW_Flowers": instance(lit, "MI_RW_Flowers", {"Color": pal["flowers"][0]}, {"Roughness": 0.5}),
        "RW_Bark": instance(lit, "MI_RW_Bark", {"Color": pal["bark"]}, {"Roughness": 0.9, "Specular": 0.3}),
        "RW_Rock": instance(lit, "MI_RW_Rock", {"Color": pal["rock"]}, {"Roughness": 0.82, "Specular": 0.35}),
        "RW_Water": instance(water, "MI_RW_Water", {"Color": pal["water"]}, {"Roughness": 0.04, "Specular": 0.5}),
        "RW_Fern": instance(fol, "MI_RW_Fern", {"ColorA": [c * 1.1 for c in pal["leaves"]], "ColorB": pal["needles"]}, {"Variation": 0.5, "WindStrength": 5.0, "WindHeight": 90.0}),
    }
    if "stone" in pal:  # castle
        mats.update({
            "RW_Stone": instance(crs, "MI_RW_Stone",
                                 {"ColorA": pal["stone"], "ColorB": [pal["stone"][0] * 0.62, pal["stone"][1] * 0.6, pal["stone"][2] * 0.55],
                                  "MortarColor": [c * 0.55 for c in pal["stone"]], "GrimeColor": pal["moss"]},
                                 {"BlockWidth": 0.95, "BlockHeight": 0.44, "MortarWidth": 0.014, "RowOffset": 0.5,
                                  "Grime": 0.5, "Roughness": 0.86, "Specular": 0.3, "NormalStrength": 0.03}),
            "RW_Roof": instance(crs, "MI_RW_Roof",
                                {"ColorA": pal["roof"], "ColorB": [c * 0.7 for c in pal["roof"]],
                                 "MortarColor": [c * 0.3 for c in pal["roof"]]},
                                {"BlockWidth": 0.34, "BlockHeight": 0.2, "MortarWidth": 0.012, "RowOffset": 0.5,
                                 "Grime": 0.0, "Roughness": 0.5, "Specular": 0.5, "NormalStrength": 0.04}),
            "RW_RoofAlt": instance(crs, "MI_RW_RoofAlt",
                                   {"ColorA": pal["roof_alt"], "ColorB": [c * 0.7 for c in pal["roof_alt"]],
                                    "MortarColor": [c * 0.3 for c in pal["roof_alt"]]},
                                   {"BlockWidth": 0.34, "BlockHeight": 0.2, "MortarWidth": 0.012, "RowOffset": 0.5,
                                    "Grime": 0.0, "Roughness": 0.55, "Specular": 0.5, "NormalStrength": 0.04}),
            "RW_Wood": instance(crs, "MI_RW_Wood",
                                {"ColorA": pal["wood"], "ColorB": [c * 0.7 for c in pal["wood"]],
                                 "MortarColor": [c * 0.25 for c in pal["wood"]]},
                                {"BlockWidth": 2.4, "BlockHeight": 0.22, "MortarWidth": 0.008, "RowOffset": 0.37,
                                 "Grime": 0.0, "Roughness": 0.75, "Specular": 0.3, "NormalStrength": 0.02}),
            "RW_WindowDark": instance(lit, "MI_RW_WindowDark", {"Color": [0.01, 0.01, 0.012]}, {"Roughness": 0.2}),
            "RW_WindowLit": instance(lit, "MI_RW_WindowLit", {"Color": [0.05, 0.03, 0.01], "EmissiveColor": pal["window_glow"]},
                                     {"Roughness": 0.3, "EmissiveStrength": 20.0}),
            "RW_Cloth": instance(fol, "MI_RW_Cloth", {"ColorA": pal["cloth"], "ColorB": pal["cloth"]},
                                 {"Variation": 0.0, "WindStrength": 0.0}),
            "RW_Ivy": instance(fol, "MI_RW_Ivy", {"ColorA": [0.025, 0.06, 0.015], "ColorB": [0.05, 0.09, 0.02]},
                               {"Variation": 0.6, "WindStrength": 1.5, "WindHeight": 100000.0}),
            "RW_Rubble": instance(crs, "MI_RW_Rubble",
                                  {"ColorA": pal.get("rubble", [0.15, 0.14, 0.12]),
                                   "ColorB": [c * 0.6 for c in pal.get("rubble", [0.15, 0.14, 0.12])],
                                   "MortarColor": [c * 0.5 for c in pal.get("rubble", [0.15, 0.14, 0.12])],
                                   "GrimeColor": pal["moss"]},
                                  {"BlockWidth": 0.46, "BlockHeight": 0.27, "MortarWidth": 0.02, "RowOffset": 0.5,
                                   "Grime": 0.8, "Roughness": 0.9, "Specular": 0.25, "NormalStrength": 0.04}),
            "RW_Hay": instance(lit, "MI_RW_Hay", {"Color": pal.get("hay", [0.42, 0.33, 0.12])}, {"Roughness": 0.9, "Specular": 0.2}),
            "RW_Iron": instance(lit, "MI_RW_Iron", {"Color": pal.get("iron", [0.03, 0.03, 0.032])}, {"Roughness": 0.45, "Specular": 0.6}),
            "RW_Fire": instance(lit, "MI_RW_Fire", {"Color": [0, 0, 0], "EmissiveColor": [1.0, 0.42, 0.1]},
                                {"EmissiveStrength": 60.0}),
        })
    log("materials built")
    return mats


def assign_materials(meshes, mats):
    for name, mesh in meshes.items():
        slots = mesh.get_editor_property("static_materials")
        for i, slot in enumerate(slots):
            key = str(slot.get_editor_property("material_slot_name"))
            # FBX slot names may carry suffixes like "RW_Bark.001" or "_skin";
            # exact names first (RW_Roof must not catch RW_RoofAlt)
            base = key.split(".")[0]
            mat = mats.get(base)
            if mat is None:
                for mk in sorted(mats, key=len, reverse=True):
                    if base.startswith(mk):
                        mat = mats[mk]
                        break
            if mat is not None:
                mesh.set_material(i, mat)
            else:
                unreal.log_warning(f"[ReferenceWorld] no material for slot {key} on {name}")
        eal.save_loaded_asset(mesh)


# --------------------------------------------------------------------------
# level
def spawn(cls, loc=(0, 0, 0), rot=None, label=None):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    r = unreal.Rotator(**rot) if rot else unreal.Rotator(0, 0, 0)
    a = eas.spawn_actor_from_class(cls, unreal.Vector(*loc), r)
    if label:
        a.set_actor_label(label)
    return a


def comp(actor, cls):
    return actor.get_component_by_class(cls)


def build_environment(man):
    lt = man["lighting"]
    # sun: physical units (lux); preset strength 5.5 ~ 20 klx golden-hour sun
    sun = spawn(unreal.DirectionalLight, (0, 0, 50000), lt["ue_sun_rotation"], "Sun")
    sc = comp(sun, unreal.DirectionalLightComponent)
    sc.set_mobility(unreal.ComponentMobility.MOVABLE)
    sc.set_editor_property("intensity", 20000.0 * lt["sun_strength"] / 5.5)
    sc.set_light_color(lc(lt["sun_color"]))
    sc.set_editor_property("atmosphere_sun_light", True)
    sc.set_editor_property("light_source_angle", max(0.5, lt["sun_angle_deg"]))
    # atmospheric depth: the sun lights the volumetric fog and throws shafts
    for k, v in (("volumetric_scattering_intensity", 1.6), ("enable_light_shaft_bloom", True),
                 ("bloom_scale", 0.12), ("bloom_threshold", 4.0)):
        try:
            sc.set_editor_property(k, v)
        except Exception as e:
            unreal.log_warning(f"[ReferenceWorld] sun setting {k}: {e}")

    atmo = spawn(unreal.SkyAtmosphere, (0, 0, 0), None, "SkyAtmosphere")
    ac = comp(atmo, unreal.SkyAtmosphereComponent)
    try:
        ac.set_editor_property("mie_scattering_scale", 0.003996 * lt["aerosol_density"] / 1.6)
    except Exception:
        pass

    sky = spawn(unreal.SkyLight, (0, 0, 1000), None, "SkyLight")
    skc = comp(sky, unreal.SkyLightComponent)
    skc.set_mobility(unreal.ComponentMobility.MOVABLE)
    skc.set_editor_property("real_time_capture", True)

    clouds = spawn(unreal.VolumetricCloud, (0, 0, 0), None, "VolumetricCloud")
    cmat = unreal.load_asset("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst")
    if cmat:
        comp(clouds, unreal.VolumetricCloudComponent).set_editor_property("material", cmat)

    fog = spawn(unreal.ExponentialHeightFog, (0, 0, man["terrain"]["water_level_m"] * 100.0), None, "HeightFog")
    fc = comp(fog, unreal.ExponentialHeightFogComponent)
    fc.set_editor_property("fog_density", 0.012 * lt["haze_amount"] / 0.72)
    fc.set_editor_property("fog_height_falloff", 0.06)
    fc.set_editor_property("start_distance", lt["mist_start_m"] * 100.0)
    fc.set_editor_property("volumetric_fog", True)
    fc.set_editor_property("volumetric_fog_scattering_distribution", 0.6)
    # a second, thin and low fog layer: valley mist that hugs the river
    try:
        second = fc.get_editor_property("second_fog_data")
        second.set_editor_property("fog_density", 0.025 * lt["haze_amount"] / 0.72)
        second.set_editor_property("fog_height_falloff", 0.35)
        second.set_editor_property("fog_height_offset", 0.0)
        fc.set_editor_property("second_fog_data", second)
    except Exception as e:
        unreal.log_warning(f"[ReferenceWorld] second fog layer: {e}")
    for prop in ("fog_inscattering_luminance", "fog_inscattering_color"):
        try:
            fc.set_editor_property(prop, lc([c * 0.2 for c in lt["haze_color"]]))
            break
        except Exception:
            continue

    ppv = spawn(unreal.PostProcessVolume, (0, 0, 0), None, "PostProcess")
    ppv.set_editor_property("unbound", True)
    s = ppv.get_editor_property("settings")
    ev = 13.0 - lt.get("exposure", 0.0)
    # interiors are far darker than the sunlit valley: give auto exposure
    # room to adapt when the player walks inside
    for k, v in (("auto_exposure_min_brightness", ev - 4.5), ("auto_exposure_max_brightness", ev + 1.5),
                 ("auto_exposure_speed_up", 2.0), ("auto_exposure_speed_down", 1.2),
                 ("bloom_intensity", 0.4 + lt["bloom"]), ("vignette_intensity", lt["vignette"] * 1.6),
                 ("film_grain_intensity", 0.08), ("scene_fringe_intensity", 0.4),
                 # split-tone grade: warm highlights, cool shadows, a touch of saturation
                 ("color_gain_highlights", unreal.Vector4(1.04, 1.0, 0.95, 1.0)),
                 ("color_gain_shadows", unreal.Vector4(0.97, 0.99, 1.03, 1.0)),
                 ("color_saturation", unreal.Vector4(1.0, 1.0, 1.0, 1.06)),
                 # cleaner bounce light in torch-lit interiors
                 ("lumen_final_gather_quality", 1.5), ("lumen_scene_lighting_quality", 1.5)):
        try:
            s.set_editor_property("override_" + k, True)
            s.set_editor_property(k, v)
        except Exception as e:
            unreal.log_warning(f"[ReferenceWorld] post setting {k}: {e}")
    ppv.set_editor_property("settings", s)
    log("environment built")


def place_static(mesh, label, loc=(0, 0, 0), scale=(1, 1, 1), material=None):
    a = spawn(unreal.StaticMeshActor, loc, None, label)
    smc = a.get_editor_property("static_mesh_component")
    smc.set_static_mesh(mesh)
    a.set_actor_scale3d(unreal.Vector(*scale))
    if material:
        smc.set_material(0, material)
    return a


def add_hism(actor, mesh, rows):
    """Adds a HierarchicalInstancedStaticMeshComponent to an editor actor."""
    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    root = sds.k2_gather_subobject_data_for_instance(actor)[0]
    params = unreal.AddNewSubobjectParams(parent_handle=root,
                                          new_class=unreal.HierarchicalInstancedStaticMeshComponent,
                                          blueprint_context=None)
    handle, fail = sds.add_new_subobject(params)
    if str(fail).strip():
        raise RuntimeError(str(fail))
    sds.rename_subobject(handle, unreal.Text(mesh.get_name()))
    data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(handle)
    c = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
    c.set_static_mesh(mesh)
    c.set_mobility(unreal.ComponentMobility.STATIC)
    xf = [unreal.Transform(unreal.Vector(r[0], r[1], r[2]),
                           unreal.Rotator(roll=r[5], pitch=r[3], yaw=r[4]),
                           unreal.Vector(r[6], r[6], r[6])) for r in rows]
    c.add_instances(xf, False, True)
    return c, len(xf)


def place_instances(meshes):
    with open(os.path.join(data_dir(), "scatter.json")) as fh:
        scatter = json.load(fh)["instances"]
    for cat, per_mesh in scatter.items():
        # the first HISM added to this plain actor becomes its root component
        holder = spawn(unreal.Actor, (0, 0, 0), None, f"Scatter_{cat}")
        cap = INSTANCE_CAPS.get(cat)
        total = 0
        for mesh_name, rows in per_mesh.items():
            mesh = meshes.get(mesh_name)
            if mesh is None:
                unreal.log_warning(f"[ReferenceWorld] missing mesh {mesh_name}")
                continue
            if cap:
                rows = rows[:cap]
            try:
                hism, k = add_hism(holder, mesh, rows)
                total += k
                if cat in NO_COLLISION_CATEGORIES:
                    hism.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
                if cat in CULL_DISTANCES:
                    hism.set_cull_distances(*CULL_DISTANCES[cat])
                if cat in NO_SHADOW_CATEGORIES:
                    hism.set_cast_shadow(False)
            except Exception as e:
                # fallback: individual actors for the big stuff, skip grass
                unreal.log_warning(f"[ReferenceWorld] HISM failed ({e}); falling back to actors for {mesh_name}")
                if cat == "grass":
                    continue
                if cat in NO_COLLISION_CATEGORIES:
                    continue
                for r in rows[:3000]:
                    a = place_static(mesh, mesh_name, (r[0], r[1], r[2]), (r[6],) * 3)
                    a.set_actor_rotation(unreal.Rotator(roll=r[5], pitch=r[3], yaw=r[4]), False)
                    total += 1
        log(f"{cat}: {total} instances")


def build_camera_and_sequence(man):
    cam_cfg = man["camera"]
    keys = cam_cfg["keys"]
    k0 = keys[0]
    cam = spawn(unreal.CineCameraActor, k0["ue_location_cm"], k0["ue_rotation"], "RW_Camera")
    cc = cam.get_cine_camera_component()
    fb = cc.get_editor_property("filmback")
    fb.set_editor_property("sensor_width", float(cam_cfg["sensor_mm"]))
    fb.set_editor_property("sensor_height", float(cam_cfg["sensor_mm"]) * 9.0 / 16.0)
    cc.set_editor_property("filmback", fb)
    cc.set_editor_property("current_focal_length", float(cam_cfg["lens_mm"]))
    cc.set_editor_property("current_aperture", float(cam_cfg["fstop"]))
    fs = cc.get_editor_property("focus_settings")
    fs.set_editor_property("focus_method", unreal.CameraFocusMethod.MANUAL)
    fs.set_editor_property("manual_focus_distance", 12000.0)
    cc.set_editor_property("focus_settings", fs)

    name = "LS_Flythrough"
    path = f"{SEQ_DIR}/{name}"
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    seq = asset_tools.create_asset(name, SEQ_DIR, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    fps = int(cam_cfg["fps"])
    seq.set_display_rate(unreal.FrameRate(fps, 1))
    seq.set_playback_start(keys[0]["frame"])
    seq.set_playback_end(keys[-1]["frame"] + 1)

    binding = seq.add_possessable(cam)
    track = binding.add_track(unreal.MovieScene3DTransformTrack)
    section = track.add_section()
    section.set_range(keys[0]["frame"], keys[-1]["frame"] + 1)
    try:
        chans = section.get_all_channels()
    except Exception:
        chans = section.get_channels_by_type(unreal.MovieSceneScriptingDoubleChannel)
    # channel order: location X Y Z, rotation X(roll) Y(pitch) Z(yaw), scale X Y Z
    prev_yaw = None
    for k in keys:
        f = unreal.FrameNumber(int(k["frame"]))
        loc = k["ue_location_cm"]
        rot = k["ue_rotation"]
        yaw = rot["yaw"]
        if prev_yaw is not None:  # unwind so the camera never spins the long way round
            while yaw - prev_yaw > 180:
                yaw -= 360
            while yaw - prev_yaw < -180:
                yaw += 360
        prev_yaw = yaw
        for ch, v in zip(chans[:6], (loc[0], loc[1], loc[2], rot["roll"], rot["pitch"], yaw)):
            ch.add_key(f, float(v), interpolation=unreal.MovieSceneKeyInterpolation.AUTO)

    try:
        cut_track = seq.add_track(unreal.MovieSceneCameraCutTrack)
    except Exception:
        cut_track = seq.add_master_track(unreal.MovieSceneCameraCutTrack)
    cut = cut_track.add_section()
    cut.set_range(keys[0]["frame"], keys[-1]["frame"] + 1)
    try:
        bid = seq.get_portable_binding_id(seq, binding)
    except Exception:
        bid = seq.make_binding_id(binding, unreal.MovieSceneObjectBindingSpace.LOCAL)
    cut.set_camera_binding_id(bid)
    eal.save_loaded_asset(seq)
    log("camera + sequence built")
    return cam, seq


# --------------------------------------------------------------------------
# playability
COMPLEX_COLLISION = ("SM_Terrain", "SM_TerrainFar", "SM_Castle", "SM_Crag", "SM_MeadowProps")
NO_COLLISION_CATEGORIES = ("grass", "ferns", "pebbles", "bushes")
# (start fade, fully culled) in cm; keeps the frame rate up in the meadow
CULL_DISTANCES = {"grass": (5000, 8000), "pebbles": (3500, 6000), "ferns": (7000, 11000), "bushes": (15000, 25000)}
NO_SHADOW_CATEGORIES = ("grass", "pebbles")


def setup_collision(meshes):
    """Walkable architecture and terrain use their render triangles as
    collision; everything else relies on imported UCX_ proxies."""
    for name, mesh in meshes.items():
        if name in COMPLEX_COLLISION:
            body = mesh.get_editor_property("body_setup")
            if body is None:
                unreal.log_warning(f"[ReferenceWorld] {name}: no body setup")
                continue
            body.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
            eal.save_loaded_asset(mesh)
    log("collision configured")


def place_lights(man):
    castle = man.get("castle") or {}
    n = 0
    for L in castle.get("lights", []):
        a = spawn(unreal.PointLight, L["location_cm"], None, f"Light_{L['kind']}_{n:03d}")
        lc_ = comp(a, unreal.PointLightComponent)
        lc_.set_mobility(unreal.ComponentMobility.MOVABLE)
        try:
            lc_.set_editor_property("intensity_units", unreal.LightUnits.LUMENS)
        except Exception:
            pass
        # Blender watts -> lumens, roughly (warm incandescent ~ 12 lm/W)
        lc_.set_editor_property("intensity", float(L["power_w"]) * 12.0)
        lc_.set_light_color(lc(L["color"]))
        lc_.set_editor_property("attenuation_radius", 1600.0 if L["kind"] in ("fire", "chandelier") else 900.0)
        lc_.set_editor_property("source_radius", 8.0)
        lc_.set_editor_property("cast_shadows", L["kind"] in ("fire", "chandelier"))
        n += 1
    log(f"{n} castle lights")


def place_player_and_bounds(man):
    castle = man.get("castle") or {}
    ps = castle.get("player_start")
    if ps:
        spawn(unreal.PlayerStart, ps["location_cm"], {"pitch": 0.0, "yaw": ps["yaw_deg"], "roll": 0.0}, "PlayerStart")
    # invisible walls just inside the main terrain edge
    half = castle.get("world_half_size_cm", man["terrain"]["size_m"] * 50.0) - 2000.0
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")   # 100 cm cube
    if cube:
        for i, (x, y, sx, sy) in enumerate(((half, 0, 1, 2 * half / 100), (-half, 0, 1, 2 * half / 100),
                                            (0, half, 2 * half / 100, 1), (0, -half, 2 * half / 100, 1))):
            a = place_static(cube, f"WorldBound_{i}", (x, y, 50000.0), (sx, sy, 1000.0))
            a.set_actor_hidden_in_game(True)
            a.get_editor_property("static_mesh_component").set_editor_property("cast_shadow", False)
    # game mode on this level (works once the C++ module is compiled)
    gm = unreal.load_class(None, "/Script/ReferenceWorld.RWGameMode")
    if gm:
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        world.get_world_settings().set_editor_property("default_game_mode", gm)
        log("game mode: RWGameMode")
    else:
        unreal.log_warning("[ReferenceWorld] RWGameMode not found - build the C++ module (see README), "
                           "then rerun; Play will use the default spectator until then")


# --------------------------------------------------------------------------
def main(preset=None):
    wd = data_dir(preset)
    man_path = os.path.join(wd, "manifest.json")
    if not os.path.isfile(man_path):
        raise RuntimeError(f"{man_path} not found - run the Blender build with --export first")
    with open(man_path) as fh:
        man = json.load(fh)
    configure(man["preset"])
    log(f"preset '{man['preset']}' from {wd} -> {MAP_PATH}")

    meshes = import_meshes(os.path.join(wd, "meshes"))
    mats = build_materials(man["palette"])
    assign_materials(meshes, mats)
    setup_collision(meshes)

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if eal.does_asset_exist(MAP_PATH):
        eal.delete_asset(MAP_PATH)
    les.new_level(MAP_PATH)

    build_environment(man)
    t = man["terrain"]
    place_static(meshes[t["mesh"]], "Terrain", material=mats["RW_Terrain"])
    if t.get("far_mesh") and t["far_mesh"] in meshes:
        place_static(meshes[t["far_mesh"]], "TerrainFar", material=mats["RW_Terrain"])
    # castle + its cliff face: authored in world space, so placed at the origin
    for key, label in (("castle_mesh", "Castle"), ("crag_mesh", "CastleCliff"), ("props_mesh", "MeadowProps")):
        if t.get(key) and t[key] in meshes:
            place_static(meshes[t[key]], label)
    plane = unreal.load_asset("/Engine/BasicShapes/Plane")
    size = t["size_m"]  # the engine plane is 1 m square
    place_static(plane, "Water", (0, 0, t["water_level_m"] * 100.0), (size, size, 1), mats["RW_Water"])
    place_instances(meshes)
    place_lights(man)
    place_player_and_bounds(man)
    build_camera_and_sequence(man)

    les.save_current_level()
    eal.save_directory(ROOT)
    log("done - open Cinematics/LS_Flythrough and pilot RW_Camera, or render with Movie Render Queue")


if __name__ == "__main__":
    main()
