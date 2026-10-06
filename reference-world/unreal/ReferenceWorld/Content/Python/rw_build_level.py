"""Build the Reference World level inside Unreal Editor (UE 5.3+).

Prerequisite: run the Blender pipeline with --export. It writes the meshes,
scatter and manifest into <project>/WorldData/.

Run in the editor: Tools > Execute Python Script... > this file, or in the
Output Log (Python mode):

    import rw_build_level; rw_build_level.main()

What it does
  1. Imports every FBX in WorldData/meshes as Nanite static meshes.
  2. Creates master materials + instances driven by the preset palette.
  3. Creates /Game/ReferenceWorld/Maps/L_ReferenceWorld with sun, sky
     atmosphere, sky light, volumetric clouds, height fog, post process.
  4. Places terrain, distant ranges, water, and all scattered instances
     (one HISM component per mesh variant).
  5. Creates a CineCamera and LS_Flythrough level sequence matching the
     Blender camera move.
"""
import json
import math
import os

import unreal

ROOT = "/Game/ReferenceWorld"
MESH_DIR = ROOT + "/Meshes"
MAT_DIR = ROOT + "/Materials"
MAP_PATH = ROOT + "/Maps/L_ReferenceWorld"
SEQ_DIR = ROOT + "/Cinematics"

# Optional caps to keep the editor responsive on modest GPUs (None = all)
INSTANCE_CAPS = {"grass": None, "trees": None, "bushes": None, "rocks": None}

mel = unreal.MaterialEditingLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary


def log(msg):
    unreal.log("[ReferenceWorld] " + str(msg))


def data_dir():
    return os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), "WorldData")


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
        sm.auto_generate_collision = name.startswith("SM_Terrain")
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


def master_terrain(pal):
    """Vertex colour masks from Blender: R grass, G rock, B snow, A wet."""
    m = _new_material("M_RW_Terrain")
    vc = _expr(m, unreal.MaterialExpressionVertexColor, -1200, 0)
    grass = _vparam(m, "Grass", pal["grass"], -1200, -400)
    dry = _vparam(m, "GrassDry", pal["grass_dry"], -1200, -250)
    rock = _vparam(m, "Rock", pal["rock"], -1200, 250)
    snow = _vparam(m, "Snow", pal["snow"], -1200, 400)
    wet = _vparam(m, "Wet", [c * 0.45 for c in pal["soil"]], -1200, 550)
    # large-scale variation: world-space noise
    noise = _expr(m, unreal.MaterialExpressionNoise, -1000, -150, scale=0.0004, levels=4,
                  output_min=0.0, output_max=1.0)
    g = _lerp(m, grass, dry, noise, -800, -300)
    b = _lerp(m, g, rock, vc, -600, -100, alpha_out="G")
    b = _lerp(m, b, snow, vc, -400, 0, alpha_out="B")
    b = _lerp(m, b, wet, vc, -200, 100, alpha_out="A")
    mel.connect_material_property(b, "", unreal.MaterialProperty.MP_BASE_COLOR)
    r_dry = _sparam(m, "Roughness", 0.9, -400, 300)
    r_wet = _sparam(m, "WetRoughness", 0.6, -400, 400)
    r = _lerp(m, r_dry, r_wet, vc, -200, 350, alpha_out="A")
    mel.connect_material_property(r, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(m)
    return m


def master_foliage():
    """Two-sided foliage with per-instance colour variation, the UE
    counterpart of the Blender foliage shader."""
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
    mel.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    sss = _expr(m, unreal.MaterialExpressionMultiply, -200, 200)
    mel.connect_material_expressions(col, "", sss, "A")
    sss.set_editor_property("const_b", 0.6)
    mel.connect_material_property(sss, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    mel.connect_material_property(_sparam(m, "Roughness", 0.6, -200, 350), "", unreal.MaterialProperty.MP_ROUGHNESS)
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
    mats = {
        "RW_Terrain": terrain,
        "RW_Needles": instance(fol, "MI_RW_Needles", {"ColorA": pal["needles"], "ColorB": [c * 1.5 for c in pal["needles"]]}, {"Variation": 0.6}),
        "RW_Leaves": instance(fol, "MI_RW_Leaves", {"ColorA": pal["leaves"], "ColorB": pal["leaves_alt"]}, {"Variation": 0.45}),
        "RW_Bush": instance(fol, "MI_RW_Bush", {"ColorA": [c * 0.85 for c in pal["leaves"]], "ColorB": pal["needles"]}, {"Variation": 0.7}),
        "RW_Grass": instance(fol, "MI_RW_Grass", {"ColorA": pal["grass"], "ColorB": pal["grass_dry"]}, {"Variation": 0.8, "Roughness": 0.55}),
        "RW_Flowers": instance(lit, "MI_RW_Flowers", {"Color": pal["flowers"][0]}, {"Roughness": 0.5}),
        "RW_Bark": instance(lit, "MI_RW_Bark", {"Color": pal["bark"]}, {"Roughness": 0.9, "Specular": 0.3}),
        "RW_Rock": instance(lit, "MI_RW_Rock", {"Color": pal["rock"]}, {"Roughness": 0.82, "Specular": 0.35}),
        "RW_Water": instance(lit, "MI_RW_Water", {"Color": pal["water"]}, {"Roughness": 0.03, "Specular": 0.5}),
        "RW_Fern": instance(fol, "MI_RW_Fern", {"ColorA": [c * 1.1 for c in pal["leaves"]], "ColorB": pal["needles"]}, {"Variation": 0.5}),
    }
    if "stone" in pal:  # castle
        mats.update({
            "RW_Stone": instance(lit, "MI_RW_Stone", {"Color": pal["stone"]}, {"Roughness": 0.86, "Specular": 0.3}),
            "RW_Roof": instance(lit, "MI_RW_Roof", {"Color": pal["roof"]}, {"Roughness": 0.5, "Specular": 0.5}),
            "RW_RoofAlt": instance(lit, "MI_RW_RoofAlt", {"Color": pal["roof_alt"]}, {"Roughness": 0.55, "Specular": 0.5}),
            "RW_Wood": instance(lit, "MI_RW_Wood", {"Color": pal["wood"]}, {"Roughness": 0.8}),
            "RW_WindowDark": instance(lit, "MI_RW_WindowDark", {"Color": [0.01, 0.01, 0.012]}, {"Roughness": 0.2}),
            "RW_WindowLit": instance(lit, "MI_RW_WindowLit", {"Color": [0.05, 0.03, 0.01], "EmissiveColor": pal["window_glow"]},
                                     {"Roughness": 0.3, "EmissiveStrength": 20.0}),
            "RW_Cloth": instance(fol, "MI_RW_Cloth", {"ColorA": pal["cloth"], "ColorB": pal["cloth"]}, {"Variation": 0.0}),
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
    for k, v in (("auto_exposure_min_brightness", ev - 1.5), ("auto_exposure_max_brightness", ev + 1.5),
                 ("bloom_intensity", 0.4 + lt["bloom"]), ("vignette_intensity", lt["vignette"] * 1.6),
                 ("film_grain_intensity", 0.08), ("scene_fringe_intensity", 0.4)):
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
    return len(xf)


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
                total += add_hism(holder, mesh, rows)
            except Exception as e:
                # fallback: individual actors for the big stuff, skip grass
                unreal.log_warning(f"[ReferenceWorld] HISM failed ({e}); falling back to actors for {mesh_name}")
                if cat == "grass":
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
def main():
    wd = data_dir()
    man_path = os.path.join(wd, "manifest.json")
    if not os.path.isfile(man_path):
        raise RuntimeError(f"{man_path} not found - run the Blender build with --export first")
    with open(man_path) as fh:
        man = json.load(fh)
    log(f"preset '{man['preset']}' from {wd}")

    meshes = import_meshes(os.path.join(wd, "meshes"))
    mats = build_materials(man["palette"])
    assign_materials(meshes, mats)

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
    for key, label in (("castle_mesh", "Castle"), ("crag_mesh", "CastleCliff")):
        if t.get(key) and t[key] in meshes:
            place_static(meshes[t[key]], label)
    plane = unreal.load_asset("/Engine/BasicShapes/Plane")
    size = t["size_m"]  # the engine plane is 1 m square
    place_static(plane, "Water", (0, 0, t["water_level_m"] * 100.0), (size, size, 1), mats["RW_Water"])
    place_instances(meshes)
    build_camera_and_sequence(man)

    les.save_current_level()
    eal.save_directory(ROOT)
    log("done - open Cinematics/LS_Flythrough and pilot RW_Camera, or render with Movie Render Queue")


if __name__ == "__main__":
    main()
