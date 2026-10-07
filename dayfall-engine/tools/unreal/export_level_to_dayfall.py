"""Export an Unreal level to a DAYFALL map folder.

Run it inside the Unreal Editor (Python Editor Script Plugin, on by default in UE 5), from the Output Log's
Python console (Cmd: Python):

    py "C:/<path>/dayfall-engine/tools/unreal/export_level_to_dayfall.py" --out "C:/<path>/dayfall-engine/maps/islet_fortress"

or without opening an editor window:

    UnrealEditor-Cmd.exe "<project>.uproject" -run=pythonscript
        -script="C:/<path>/export_level_to_dayfall.py --map /Game/ClaudeFortress/Maps/L_Islet_Fortress --out C:/<path>/maps/islet_fortress"

Options: --map (load this level first; default: the open level), --name, --format auto|gltf|fbx, --terrain
auto|none, --terrain-actors "label*,other*", --terrain-spacing M, --terrain-max-samples N, --instance-threshold N,
--only-selected, --skip-textures, --skip-meshes (re-export placements only, keeping existing assets).

What it writes:
  map.json                    the DAYFALL world document (docs/MAP_FORMAT.md)
  assets/<mesh>.glb           one per static mesh, from the glTF Exporter plugin when it is enabled; otherwise
  _fbx/<mesh>.fbx             FBX files plus _fbx/manifest.json: run tools/unreal/fbx_to_glb.py in Blender next
  textures/<texture>.png      the textures the materials use
  instances/<set>.bin         instanced, hierarchical-instanced and foliage meshes
  terrain/height.png          landscape heights (16-bit, north at the top), when there is a landscape
  unreal_export_report.json   counts, warnings, and everything that was not exported (read it)

The script only reads the level: it never saves, changes or creates Unreal assets, and it enables no plugins.
Coordinates and units are converted in dayfall_convert.py (tested by test_dayfall_convert.py).
"""
import argparse
import fnmatch
import json
import math
import os
import struct
import sys
import time
import traceback

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dayfall_convert as C  # noqa: E402

LOG_PREFIX = "[dayfall export] "


def log(msg):
    unreal.log(LOG_PREFIX + msg)


def warn(msg):
    unreal.log_warning(LOG_PREFIX + msg)


# ---------------------------------------------------------------- small Unreal helpers (API differs between versions)

def prop(obj, name, default=None):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return getattr(obj, name, default)


def set_props(obj, **kw):
    for k, v in kw.items():
        try:
            obj.set_editor_property(k, v)
        except Exception:
            pass


def enum_name(v):
    s = str(v)
    if "." in s:
        s = s.split(".")[-1]
    return s.split(":")[0].strip("<> ").lower()


def subsystem(cls_name):
    cls = getattr(unreal, cls_name, None)
    if cls is None:
        return None
    try:
        return unreal.get_editor_subsystem(cls)
    except Exception:
        return None


def editor_world():
    s = subsystem("UnrealEditorSubsystem")
    if s:
        return s.get_editor_world()
    return unreal.EditorLevelLibrary.get_editor_world()


def level_actors():
    s = subsystem("EditorActorSubsystem")
    if s:
        return list(s.get_all_level_actors())
    return list(unreal.EditorLevelLibrary.get_all_level_actors())


def selected_actors():
    s = subsystem("EditorActorSubsystem")
    if s:
        return list(s.get_selected_level_actors())
    return list(unreal.EditorLevelLibrary.get_selected_level_actors())


def load_level(path):
    s = subsystem("LevelEditorSubsystem")
    if s:
        return s.load_level(path)
    return unreal.EditorLoadingAndSavingUtils.load_map(path)


def vec(v):
    return (float(v.x), float(v.y), float(v.z))


def transform_parts(t):
    """unreal.Transform -> ((x, y, z) cm, (qx, qy, qz, qw), (sx, sy, sz))."""
    q = t.rotation
    return vec(t.translation), (float(q.x), float(q.y), float(q.z), float(q.w)), vec(t.scale3d)


def component_bounds(comp):
    origin, extent, _ = unreal.SystemLibrary.get_component_bounds(comp)
    o, e = vec(origin), vec(extent)
    return (o[0] - e[0], o[1] - e[1], o[2] - e[2]), (o[0] + e[0], o[1] + e[1], o[2] + e[2])


def label(actor):
    try:
        return actor.get_actor_label()
    except Exception:
        return actor.get_name()


def instance_transform(comp, i):
    r = comp.get_instance_transform(i, True)
    if isinstance(r, tuple):   # (ok, transform) in some versions
        r = next((x for x in r if isinstance(x, unreal.Transform)), None)
    return r


def trace_component_z(comp, x, y, z_top, z_bottom):
    """Height (cm) where a vertical ray at Unreal (x, y) first hits this component, or None."""
    r = comp.k2_line_trace_component(unreal.Vector(x, y, z_top), unreal.Vector(x, y, z_bottom), True, False, False)
    if r is None or r is False:
        return None
    if isinstance(r, tuple):
        if r and isinstance(r[0], bool):
            if not r[0]:
                return None
            r = r[1:]
        for v in r:
            if isinstance(v, unreal.Vector):
                return float(v.z)
        return None
    if isinstance(r, unreal.Vector):
        return float(r.z)
    return None


def light_color(comp):
    c = prop(comp, "light_color")
    rgb = C.fcolor_to_linear(c.r, c.g, c.b) if c is not None else [1.0, 1.0, 1.0]
    if prop(comp, "use_temperature", False):
        k = C.kelvin_to_linear(float(prop(comp, "temperature", 6500.0)))
        rgb = [a * b for a, b in zip(rgb, k)]
    return [round(v, 4) for v in rgb]


# ---------------------------------------------------------------- exporter

class Exporter:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        self.report = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "warnings": [], "skipped_actors": {},
                       "mirrored_instances": 0, "spot_lights_as_points": 0, "counts": {}}
        self.mesh_keys = {}           # StaticMesh path -> key
        self.mesh_info = {}           # key -> {"asset", "path", "materials": [names]}
        self.materials = {}           # material path -> name
        self.material_objs = {}       # name -> MaterialInterface
        self.names = C.UniqueNames()
        self.mat_names = C.UniqueNames()
        self.tex_names = C.UniqueNames()
        self.tex_files = {}           # texture path -> relative file
        self.placements = []          # dicts: key, overrides, collision, shadow, cull, transform, actor, comp
        self.instance_sets = []       # dicts: key, overrides, collision, shadow, cull, records, actor, comp
        self.lights, self.cameras = [], {}
        self.player_start, self.sun, self.fog = None, None, None
        self.terrain_comps = []
        self.doc = {"format": "dayfall-map"}

    def note(self, msg):
        warn(msg)
        if len(self.report["warnings"]) < 500:
            self.report["warnings"].append(msg)

    def skip(self, actor, why):
        k = f"{actor.get_class().get_name()}: {why}"
        self.report["skipped_actors"][k] = self.report["skipped_actors"].get(k, 0) + 1

    # ------------------------------------------------------------ collect

    def collect(self):
        if self.args.map:
            log(f"loading {self.args.map}")
            load_level(self.args.map)
        world = editor_world()
        self.world_name = world.get_name() if world else "level"
        actors = selected_actors() if self.args.only_selected else level_actors()
        log(f"{len(actors)} actors in {self.world_name}")
        patterns = [p.strip() for p in (self.args.terrain_actors or "").split(",") if p.strip()]
        with unreal.ScopedSlowTask(len(actors), "DAYFALL: reading the level") as task:
            task.make_dialog(True)
            for a in actors:
                if task.should_cancel():
                    raise RuntimeError("cancelled")
                task.enter_progress_frame(1)
                try:
                    self.collect_actor(a, patterns)
                except Exception as e:
                    self.note(f"{label(a)}: {e}")

    def collect_actor(self, a, terrain_patterns):
        lab = label(a)
        cls = a.get_class().get_name()
        if prop(a, "hidden", False) and not isinstance(a, unreal.PlayerStart):
            return self.skip(a, "hidden in game")
        if isinstance(a, unreal.PlayerStart):
            return self.collect_player_start(a)
        if isinstance(a, unreal.CameraActor):
            return self.collect_camera(a)
        if hasattr(unreal, "LandscapeProxy") and isinstance(a, unreal.LandscapeProxy):
            if self.args.terrain == "none":
                return self.skip(a, "landscape (--terrain none)")
            comps = a.get_components_by_class(unreal.LandscapeHeightfieldCollisionComponent)
            self.terrain_comps += list(comps)
            if not comps:
                self.note(f"landscape {lab} has no collision components: its heights cannot be traced")
            return
        if hasattr(unreal, "ExponentialHeightFog") and isinstance(a, unreal.ExponentialHeightFog):
            self.fog = a
            return
        used = False
        is_terrain = any(fnmatch.fnmatch(lab, p) for p in terrain_patterns)
        for comp in a.get_components_by_class(unreal.LightComponent):
            used = self.collect_light(a, comp) or used
        for comp in a.get_components_by_class(unreal.StaticMeshComponent):
            if is_terrain:
                self.terrain_comps.append(comp)
                used = True
                continue
            used = self.collect_mesh_component(a, comp) or used
        if not used:
            known = ("SkyAtmosphere", "SkyLight", "VolumetricCloud", "PostProcessVolume", "WorldSettings",
                     "Brush", "DefaultPhysicsVolume", "LightmassImportanceVolume", "NavMeshBoundsVolume")
            self.skip(a, "nothing exportable" if not cls.startswith(known) else "engine sky, volume or settings")

    def collect_player_start(self, a):
        p = vec(a.get_actor_location())
        half = 92.0
        try:
            cap = a.get_component_by_class(unreal.CapsuleComponent)
            half = float(cap.get_scaled_capsule_half_height())
        except Exception:
            pass
        yaw = float(a.get_actor_rotation().yaw)
        if self.player_start is None:
            self.player_start = {"position": [round(c, 3) for c in C.pos(p[0], p[1], p[2] - half)], "yaw_deg": round(C.yaw(yaw), 2)}
        else:
            self.note(f"more than one PlayerStart; using the first, ignoring {label(a)}")

    def collect_camera(self, a):
        p = vec(a.get_actor_location())
        f = vec(a.get_actor_forward_vector())
        comp = a.get_component_by_class(unreal.CameraComponent)
        hfov = float(prop(comp, "field_of_view", 90.0)) if comp else 90.0
        if comp is not None and hasattr(unreal, "CineCameraComponent") and isinstance(comp, unreal.CineCameraComponent):
            try:   # a cine camera's view comes from its lens and sensor
                fb = prop(comp, "filmback")
                focal = float(prop(comp, "current_focal_length"))
                hfov = math.degrees(2 * math.atan(float(prop(fb, "sensor_width")) / (2 * focal)))
            except Exception:
                pass
        aspect = float(prop(comp, "aspect_ratio", 16 / 9)) if comp else 16 / 9
        pos = C.pos(*p)
        d = C.direction(*f)
        tgt = [pos[k] + d[k] * 10.0 for k in range(3)]
        self.cameras[C.safe_name(label(a))] = {"position": [round(c, 3) for c in pos], "target": [round(c, 3) for c in tgt],
                                               "vfov_deg": round(C.hfov_to_vfov(hfov, aspect), 2)}

    def collect_light(self, a, comp):
        if not prop(comp, "visible", True):
            return False
        if isinstance(comp, unreal.DirectionalLightComponent):
            if self.sun is None or bool(prop(comp, "atmosphere_sun_light", False)):
                self.sun = {"forward": vec(a.get_actor_forward_vector()), "lux": float(prop(comp, "intensity", 10.0)),
                            "color": light_color(comp)}
            return True
        if isinstance(comp, unreal.SkyLightComponent):
            return True
        if not isinstance(comp, unreal.LocalLightComponent):
            return False
        units = enum_name(prop(comp, "intensity_units", "unitless"))
        cd = C.light_candela(float(prop(comp, "intensity", 0.0)), units)
        if isinstance(comp, unreal.SpotLightComponent) or (hasattr(unreal, "RectLightComponent") and isinstance(comp, unreal.RectLightComponent)):
            self.report["spot_lights_as_points"] += 1
        p = vec(comp.get_world_location())
        self.lights.append({"id": C.safe_name(f"{label(a)}_{comp.get_name()}"), "candela": cd,
                            "position": [round(c, 3) for c in C.pos(*p)], "color": light_color(comp),
                            "range_m": round(max(1.0, float(prop(comp, "attenuation_radius", 1000.0)) * C.CM), 2)})
        return True

    def mesh_key(self, mesh):
        path = mesh.get_path_name()
        if path not in self.mesh_keys:
            key = self.names.get(mesh.get_name())
            self.mesh_keys[path] = key
            self.mesh_info[key] = {"asset": mesh, "path": path}
        return self.mesh_keys[path]

    def material_name(self, mi):
        if mi is None:
            return None
        path = mi.get_path_name()
        if path not in self.materials:
            name = self.mat_names.get(mi.get_name())
            if name != C.safe_name(mi.get_name()):
                self.note(f"two materials are named {mi.get_name()}; {path} became {name}, which the exported meshes "
                          "do not use (rename one in Unreal for an exact port)")
            self.materials[path] = name
            self.material_objs[name] = mi
        return self.materials[path]

    def slot_materials(self, mesh):
        out = []
        for sm in prop(mesh, "static_materials", []) or []:
            out.append(prop(sm, "material_interface"))
        return out

    def overrides(self, comp, mesh):
        """{mesh material name: replacement name} for this component's override materials."""
        res = {}
        defaults = self.slot_materials(mesh)
        for m in defaults:
            self.material_name(m)
        try:
            over = list(prop(comp, "override_materials", []) or [])
        except Exception:
            over = []
        for i, m in enumerate(over):
            if m is None or i >= len(defaults) or defaults[i] is None:
                continue
            a, b = self.material_name(defaults[i]), self.material_name(m)
            if a != b:
                if a in res and res[a] != b:
                    self.note(f"{comp.get_name()}: material {a} is overridden differently in two slots; using {res[a]}")
                    continue
                res[a] = b
        return res

    def collision_of(self, comp):
        try:
            if enum_name(comp.get_collision_enabled()) in ("no_collision", "probe_only"):
                return "none"
            resp = comp.get_collision_response_to_channel(unreal.CollisionChannel.ECC_PAWN)
            if enum_name(resp) != "ecr_block" and "block" not in enum_name(resp):
                return "none"
        except Exception:
            pass
        return "mesh"

    def collect_mesh_component(self, a, comp):
        mesh = prop(comp, "static_mesh")
        if mesh is None:
            return False
        if not comp.is_visible() or prop(comp, "hidden_in_game", False):
            self.report["counts"]["hidden_components"] = self.report["counts"].get("hidden_components", 0) + 1
            return False
        key = self.mesh_key(mesh)
        entry = {"key": key, "overrides": self.overrides(comp, mesh), "collision": self.collision_of(comp),
                 "shadow": bool(prop(comp, "cast_shadow", True)), "actor": label(a), "comp": comp.get_name()}
        if isinstance(comp, unreal.InstancedStaticMeshComponent):
            n = comp.get_instance_count()
            recs = []
            for i in range(n):
                t = instance_transform(comp, i)
                if t is not None:
                    recs.append(transform_parts(t))
            cull = float(prop(comp, "instance_end_cull_distance", 0) or 0)
            entry.update(records=recs, cull=cull * C.CM if cull > 0 else None)
            self.instance_sets.append(entry)
        else:
            cull = float(prop(comp, "ld_max_draw_distance", 0.0) or 0.0)
            entry.update(transform=transform_parts(comp.get_world_transform()), cull=cull * C.CM if cull > 0 else None)
            self.placements.append(entry)
        return True

    # ------------------------------------------------------------ meshes

    def gltf_available(self):
        return hasattr(unreal, "GLTFExportOptions") and (hasattr(unreal, "GLTFExporter") or hasattr(unreal, "GLTFStaticMeshExporter"))

    def export_meshes(self):
        fmt = self.args.format
        if fmt == "auto":
            fmt = "gltf" if self.gltf_available() else "fbx"
            if fmt == "fbx":
                self.note("the glTF Exporter plugin is not enabled: meshes go out as FBX; run tools/unreal/fbx_to_glb.py "
                          "in Blender to finish (or ask the user to enable the plugin and export again)")
        self.report["mesh_format"] = fmt
        os.makedirs(os.path.join(self.out, "assets"), exist_ok=True)
        manifest = []
        keys = sorted(self.mesh_info)
        with unreal.ScopedSlowTask(len(keys), "DAYFALL: exporting meshes") as task:
            task.make_dialog(True)
            for key in keys:
                if task.should_cancel():
                    raise RuntimeError("cancelled")
                task.enter_progress_frame(1, f"mesh {key}")
                info = self.mesh_info[key]
                mesh = info["asset"]
                bb = mesh.get_bounding_box()
                mn, mx = vec(bb.min), vec(bb.max)
                info["extent_cm"] = [mx[i] - mn[i] for i in range(3)]
                info["radius_m"] = 0.5 * math.sqrt(sum(e * e for e in info["extent_cm"])) * C.CM
                try:
                    info["triangles"] = int(mesh.get_num_triangles(0))
                except Exception:
                    info["triangles"] = 0
                try:
                    info["nanite"] = bool(prop(prop(mesh, "nanite_settings"), "enabled", False))
                except Exception:
                    info["nanite"] = False
                if self.args.skip_meshes:
                    continue
                try:
                    if fmt == "gltf":
                        self.export_gltf(key, mesh, info)
                    else:
                        manifest.append(self.export_fbx(key, mesh, info, mn, mx))
                except Exception as e:
                    self.note(f"mesh {key} ({info['path']}) failed to export: {e}")
        if fmt == "fbx" and not self.args.skip_meshes:
            with open(os.path.join(self.out, "_fbx", "manifest.json"), "w") as f:
                json.dump({"meshes": manifest}, f, indent=1)

    def export_gltf(self, key, mesh, info):
        path = os.path.join(self.out, "assets", key + ".glb")
        opts = unreal.GLTFExportOptions()
        set_props(opts, export_uniform_scale=0.01, export_vertex_colors=True, export_lights=False, export_cameras=False,
                  export_level_sequences=False, export_animation_sequences=False, export_proxy_materials=False)
        bake = getattr(unreal, "GLTFMaterialBakeMode", None)
        if bake is not None:
            set_props(opts, bake_material_inputs=getattr(bake, "DISABLED", getattr(bake, "SIMPLE", None)))
        ok = False
        if hasattr(unreal, "GLTFExporter") and hasattr(unreal.GLTFExporter, "export_to_gltf"):
            try:
                r = unreal.GLTFExporter.export_to_gltf(mesh, path, opts, set())
                ok = (r[0] if isinstance(r, tuple) else bool(r)) and os.path.exists(path)
            except Exception as e:
                log(f"GLTFExporter.export_to_gltf: {e}; trying an export task")
        if not ok:
            task = unreal.AssetExportTask()
            set_props(task, object=mesh, filename=path, automated=True, prompt=False, replace_identical=True, options=opts)
            ok = unreal.Exporter.run_asset_export_task(task)
        if not ok or not os.path.exists(path):
            raise RuntimeError("glTF export returned no file")
        # check the axes and units the exporter used against the Unreal bounds
        mn, mx = glb_position_bounds(path)
        if mn is not None:
            s, axes_ok = C.gltf_scale_check(info["extent_cm"], mn, mx)
            if abs(s - 1.0) > 1e-3:
                info["import_scale"] = s
                self.note(f"mesh {key}: glTF came out at {1 / s:g}x size; import_scale {s:g} set in map.json")
            if not axes_ok:
                self.note(f"mesh {key}: glTF axes do not match the Unreal bounds (extent {info['extent_cm']} cm, glTF "
                          f"{[round(mx[i] - mn[i], 3) for i in range(3)]} m); check its orientation in a capture")
        info["file"] = f"assets/{key}.glb"

    def export_fbx(self, key, mesh, info, mn, mx):
        d = os.path.join(self.out, "_fbx")
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, key + ".fbx")
        task = unreal.AssetExportTask()
        opts = unreal.FbxExportOption()
        set_props(opts, vertex_color=True, level_of_detail=False, collision=False, ascii=False, force_front_x_axis=False)
        set_props(task, object=mesh, filename=path, automated=True, prompt=False, replace_identical=True, options=opts,
                  exporter=unreal.StaticMeshExporterFBX())
        if not unreal.Exporter.run_asset_export_task(task) or not os.path.exists(path):
            raise RuntimeError("FBX export returned no file")
        info["file"] = f"assets/{key}.glb"
        mats = [self.material_name(m) for m in self.slot_materials(mesh)]
        return {"key": key, "fbx": f"{key}.fbx", "glb": f"../assets/{key}.glb", "ue_min_cm": list(mn), "ue_max_cm": list(mx),
                "materials": mats, "source": info["path"]}

    # ------------------------------------------------------------ materials and textures

    def export_texture(self, tex):
        path = tex.get_path_name()
        if path in self.tex_files:
            return self.tex_files[path]
        if self.args.skip_textures:
            return None
        d = os.path.join(self.out, "textures")
        os.makedirs(d, exist_ok=True)
        name = self.tex_names.get(tex.get_name())
        rel = None
        for ext in (".png", ".tga"):
            f = os.path.join(d, name + ext)
            task = unreal.AssetExportTask()
            set_props(task, object=tex, filename=f, automated=True, prompt=False, replace_identical=True)
            try:
                if unreal.Exporter.run_asset_export_task(task) and os.path.exists(f):
                    rel = f"textures/{name}{ext}"
                    break
            except Exception:
                pass
        if rel is None:
            self.note(f"texture {path} could not be exported as PNG or TGA")
        self.tex_files[path] = rel
        return rel

    def material_info(self, name, mi):
        base = mi.get_base_material() if hasattr(mi, "get_base_material") else mi
        blend = enum_name(prop(base, "blend_mode", "BLEND_OPAQUE"))
        blend = "mask" if "masked" in blend else "blend" if "translucent" in blend or "additive" in blend or "modulate" in blend else "opaque"
        two_sided = bool(prop(base, "two_sided", False))
        bpo = prop(mi, "base_property_overrides")
        if bpo is not None and prop(bpo, "override_two_sided", False):
            two_sided = bool(prop(bpo, "two_sided", two_sided))
        if bpo is not None and prop(bpo, "override_blend_mode", False):
            b = enum_name(prop(bpo, "blend_mode", ""))
            blend = "mask" if "masked" in b else "blend" if "translucent" in b or "additive" in b else "opaque"
        shading = enum_name(prop(base, "shading_model", "MSM_DEFAULT_LIT"))
        shading = "foliage" if "foliage" in shading else "subsurface" if "subsurface" in shading else \
            "unlit" if "unlit" in shading else "default"
        info = {"name": name, "blend": blend, "two_sided": two_sided, "shading": shading, "textures": {}}
        mel = unreal.MaterialEditingLibrary
        is_instance = isinstance(mi, unreal.MaterialInstance)
        unused = []
        # texture parameters (material instances)
        if is_instance:
            try:
                for pname in mel.get_texture_parameter_names(mi):
                    slot = C.classify_texture_param(str(pname))
                    tex = mel.get_material_instance_texture_parameter_value(mi, pname)
                    if tex is None:
                        continue
                    if slot and slot not in info["textures"]:
                        info["textures"][slot] = self.export_texture(tex)
                    elif not slot:
                        unused.append(f"{pname}={tex.get_name()}")
            except Exception as e:
                self.note(f"material {name}: texture parameters unreadable ({e})")
            for pname, key in (("roughness", "roughness"), ("metallic", "metallic"), ("opacity", "opacity"),
                               ("opacitymaskclipvalue", "alpha_cutoff"), ("emissive", "emissive_strength"),
                               ("emissivestrength", "emissive_strength"), ("emissiveintensity", "emissive_strength")):
                try:
                    for n in mel.get_scalar_parameter_names(mi):
                        if str(n).lower().replace(" ", "").replace("_", "") == pname:
                            info[key] = float(mel.get_material_instance_scalar_parameter_value(mi, n))
                except Exception:
                    pass
            try:
                for n in mel.get_vector_parameter_names(mi):
                    t = C.name_tokens(str(n))
                    if any(k in ("tint", "color", "colour", "albedo", "basecolor") for k in t) and "emissive" not in t:
                        c = mel.get_material_instance_vector_parameter_value(mi, n)
                        info["tint"] = [float(c.r), float(c.g), float(c.b)]
                        break
            except Exception:
                pass
        # textures used anywhere in the material graph (plain materials, and slots the parameters missed)
        used = []
        for m in (mi, base):   # get_used_textures takes a Material in some versions
            try:
                used = mel.get_used_textures(m)
                break
            except Exception:
                continue
        for tex in used or []:
            slot = C.classify_texture(tex.get_name(), enum_name(prop(tex, "compression_settings", "")), bool(prop(tex, "srgb", True)))
            if slot and slot not in info["textures"]:
                info["textures"][slot] = self.export_texture(tex)
            elif not slot:
                unused.append(tex.get_name())
        if blend == "mask" and "alpha_cutoff" not in info:
            try:
                info["alpha_cutoff"] = float(prop(base, "opacity_mask_clip_value", 0.3333))
            except Exception:
                pass
        if unused:
            info["unread_textures"] = sorted(set(unused))
        return info

    def export_materials(self):
        mats, details = {}, {}
        names = sorted(self.material_objs)
        with unreal.ScopedSlowTask(len(names), "DAYFALL: materials and textures") as task:
            task.make_dialog(True)
            for name in names:
                task.enter_progress_frame(1, f"material {name}")
                try:
                    info = self.material_info(name, self.material_objs[name])
                    mats[name] = C.material_doc(info)
                    details[name] = {k: v for k, v in info.items() if k in ("blend", "shading", "two_sided", "unread_textures")}
                    details[name]["source"] = self.material_objs[name].get_path_name()
                except Exception as e:
                    self.note(f"material {name}: {e}")
        self.doc["materials"] = mats
        self.report["materials"] = details

    # ------------------------------------------------------------ terrain

    def export_terrain(self):
        comps = self.terrain_comps
        if not comps:
            return
        lo = [1e30, 1e30, 1e30]
        hi = [-1e30, -1e30, -1e30]
        cb = []
        for c in comps:
            mn, mx = component_bounds(c)
            cb.append((c, mn, mx))
            lo = [min(lo[i], mn[i]) for i in range(3)]
            hi = [max(hi[i], mx[i]) for i in range(3)]
        spacing = self.args.terrain_spacing
        if spacing <= 0:   # default: the landscape's own resolution, else 1 m
            spacing = 1.0
            for c in comps:
                try:
                    spacing = max(0.25, float(c.get_owner().get_actor_scale3d().x) * C.CM)
                    break
                except Exception:
                    pass
        n, s, org = C.terrain_grid(lo[0], lo[1], hi[0], hi[1], spacing, self.args.terrain_max_samples)
        log(f"tracing terrain: {n} x {n} samples, {s:.3f} m apart, over {len(comps)} components")
        h = [None] * (n * n)
        z_top, z_bot = hi[2] + 1000.0, lo[2] - 1000.0
        with unreal.ScopedSlowTask(len(cb), "DAYFALL: tracing terrain heights") as task:
            task.make_dialog(True)
            for c, mn, mx in cb:
                if task.should_cancel():
                    raise RuntimeError("cancelled")
                task.enter_progress_frame(1)
                # engine x = ue x / 100; engine y = -ue y / 100
                i0 = max(0, int(math.floor((mn[0] * C.CM - org[0]) / s)))
                i1 = min(n - 1, int(math.ceil((mx[0] * C.CM - org[0]) / s)))
                j0 = max(0, int(math.floor((-mx[1] * C.CM - org[1]) / s)))
                j1 = min(n - 1, int(math.ceil((-mn[1] * C.CM - org[1]) / s)))
                for j in range(j0, j1 + 1):
                    uy = -(org[1] + j * s) * 100.0
                    for i in range(i0, i1 + 1):
                        ux = (org[0] + i * s) * 100.0
                        z = trace_component_z(c, ux, uy, z_top, z_bot)
                        if z is not None:
                            k = j * n + i
                            zm = z * C.CM
                            if h[k] is None or zm > h[k]:
                                h[k] = zm
        hit = sum(1 for v in h if v is not None)
        if hit == 0:
            self.note("terrain: no trace hit the terrain components (collision off?); no heightfield written")
            return
        holes = C.fill_holes(h, n)
        if holes:
            self.report["counts"]["terrain_holes_filled"] = holes
        os.makedirs(os.path.join(self.out, "terrain"), exist_ok=True)
        lo_h, hi_h = C.write_height_png(os.path.join(self.out, "terrain", "height.png"), h, n)
        self.doc["terrain"] = {"samples_per_side": n, "spacing_m": round(s, 6), "origin": [round(org[0], 4), round(org[1], 4)],
                               "height_file": "terrain/height.png", "height_range_m": [round(lo_h, 4), round(hi_h, 4)],
                               "png_rows": "north_first", "paint_files": ["terrain/paint_a.png", "terrain/paint_b.png"],
                               "material": "terrain", "horizon": {"enabled": False}}

    # ------------------------------------------------------------ map.json

    def write_map(self):
        d = self.doc
        d["name"] = self.args.name or self.world_name
        d["notes"] = (f"Exported from Unreal level {self.world_name} by tools/unreal/export_level_to_dayfall.py on "
                      f"{self.report['started']}. See unreal_export_report.json.")
        # environment
        env = {"water": {"enabled": False}}
        sun_lux = None
        if self.sun:
            el, az = C.sun_angles(self.sun["forward"])
            sun_lux = self.sun["lux"]
            env["preset"] = C.preset_for_sun(el)
            env["sun"] = {"elevation_deg": round(el, 2), "azimuth_deg": round(az, 2), "strength": C.DAYFALL_SUN_STRENGTH,
                          "color": self.sun["color"]}
            self.report["sun"] = {"unreal_lux": sun_lux, "elevation_deg": el, "azimuth_deg": az}
        else:
            env["preset"] = "overcast"
            self.note("no directional light: environment preset overcast; light intensities assume a 100000 lux sun")
        if self.fog is not None:
            fc = self.fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
            if fc is not None:
                self.report["fog"] = {"density": float(prop(fc, "fog_density", 0.0)),
                                      "height_falloff": float(prop(fc, "fog_height_falloff", 0.0)),
                                      "base_z_m": vec(self.fog.get_actor_location())[2] * C.CM,
                                      "start_distance_m": float(prop(fc, "start_distance", 0.0)) * C.CM}
        d["environment"] = env
        # meshes
        meshes = {}
        for key, info in sorted(self.mesh_info.items()):
            e = {"file": info.get("file", f"assets/{key}.glb"), "category": "unreal", "source": info["path"],
                 "collision": "mesh"}
            if info.get("import_scale"):
                e["import_scale"] = info["import_scale"]
            e.update(C.lod_entry(info.get("radius_m", 1.0), info.get("triangles", 0)))
            if info.get("triangles"):
                e["triangles"] = info["triangles"]
            meshes[key] = e
        d["meshes"] = meshes
        # objects and instance files
        groups = {}
        for p in self.placements:
            g = (p["key"], json.dumps(p["overrides"], sort_keys=True), p["collision"], p["shadow"])
            groups.setdefault(g, []).append(p)
        objects, ids = [], C.UniqueNames()
        sets = list(self.instance_sets)
        for g, items in groups.items():
            if len(items) >= self.args.instance_threshold:
                sets.append({"key": g[0], "overrides": items[0]["overrides"], "collision": g[2], "shadow": g[3],
                             "cull": max((it["cull"] or 0) for it in items) or None, "actor": f"{g[0]} x{len(items)}",
                             "comp": "objects", "records": [it["transform"] for it in items]})
                continue
            for it in items:
                tp, tq, ts = it["transform"]
                if C.mirrored(*ts):
                    self.report["mirrored_instances"] += 1
                o = {"id": ids.get(it["actor"] if it["comp"] in ("StaticMeshComponent0", "StaticMeshComponent") else f"{it['actor']}_{it['comp']}"),
                     "mesh": it["key"], "position": [round(c, 4) for c in C.pos(*tp)]}
                q = C.quat(*tq)
                if not C.is_identity_quat(q):
                    o["rotation"] = [round(c, 6) for c in q]
                sc = C.scale(*ts)
                if sc != 1:
                    o["scale"] = round(sc, 5) if not isinstance(sc, list) else [round(c, 5) for c in sc]
                if it["overrides"]:
                    o["materials"] = it["overrides"]
                if it["collision"] != "mesh":
                    o["collision"] = it["collision"]
                if not it["shadow"]:
                    o["shadow"] = False
                if it["cull"]:
                    o["cull_distance_m"] = round(it["cull"], 1)
                objects.append(o)
        d["objects"] = objects
        files = []
        os.makedirs(os.path.join(self.out, "instances"), exist_ok=True)
        set_ids = C.UniqueNames()
        for st in sets:
            if not st["records"]:
                continue
            recs = []
            for tp, tq, ts in st["records"]:
                if C.mirrored(*ts):
                    self.report["mirrored_instances"] += 1
                recs.append((C.pos(*tp), C.quat(*tq), C.scale(*ts)))
            data, layout = C.pack_instances(recs)
            sid = set_ids.get(f"{st['key']}__{st['actor']}" if st["comp"] != "objects" else f"{st['key']}__objects")
            with open(os.path.join(self.out, "instances", sid + ".bin"), "wb") as f:
                f.write(data)
            e = {"id": sid, "file": f"instances/{sid}.bin", "mesh": st["key"], "count": len(recs),
                 "collision": st["collision"], "shadow": st["shadow"]}
            if layout != "pos_scale_quat":
                e["layout"] = layout
            if st["overrides"]:
                e["materials"] = st["overrides"]
            if st.get("cull"):
                e["cull_distance_m"] = round(st["cull"], 1)
            files.append(e)
        d["instance_files"] = files
        # lights, cameras, player
        d["lights"] = [{"id": l["id"], "position": l["position"], "color": l["color"], "range_m": l["range_m"],
                        "intensity": round(C.point_intensity(l["candela"], sun_lux), 4)} for l in self.lights]
        if self.cameras:
            d["cameras"] = self.cameras
        if self.player_start:
            d["player_start"] = self.player_start
        else:
            self.note("no PlayerStart in the level; DAYFALL will start the player at the map origin")
        with open(os.path.join(self.out, "map.json"), "w") as f:
            json.dump(d, f, indent=1, sort_keys=True)
        c = self.report["counts"]
        c.update(meshes=len(meshes), materials=len(d.get("materials", {})), objects=len(objects), instance_files=len(files),
                 instances=sum(e["count"] for e in files), lights=len(d["lights"]), cameras=len(self.cameras),
                 textures=sum(1 for v in self.tex_files.values() if v))

    def write_report(self):
        self.report["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
        big = sorted(((i.get("triangles", 0), k) for k, i in self.mesh_info.items() if i.get("triangles", 0) > 200000), reverse=True)
        if big:
            self.report["heavy_meshes"] = [{"mesh": k, "triangles": t, "nanite": self.mesh_info[k].get("nanite", False)} for t, k in big[:40]]
        with open(os.path.join(self.out, "unreal_export_report.json"), "w") as f:
            json.dump(self.report, f, indent=1, sort_keys=True)
        log(f"done: {json.dumps(self.report['counts'])}; {len(self.report['warnings'])} warnings; see "
            f"{os.path.join(self.out, 'unreal_export_report.json')}")


def glb_position_bounds(path):
    """(min, max) over every POSITION accessor in a GLB, from the accessor bounds glTF requires."""
    try:
        with open(path, "rb") as f:
            head = f.read(20)
            if head[:4] != b"glTF":
                return None, None
            jlen = struct.unpack("<I", head[12:16])[0]
            doc = json.loads(f.read(jlen))
        mn, mx = [1e30] * 3, [-1e30] * 3
        for m in doc.get("meshes", []):
            for p in m.get("primitives", []):
                a = doc["accessors"][p["attributes"]["POSITION"]]
                if "min" in a and "max" in a:
                    mn = [min(mn[i], a["min"][i]) for i in range(3)]
                    mx = [max(mx[i], a["max"][i]) for i in range(3)]
        return (mn, mx) if mn[0] < 1e29 else (None, None)
    except Exception:
        return None, None


def parse_args(argv):
    ap = argparse.ArgumentParser(prog="export_level_to_dayfall.py")
    ap.add_argument("--out", required=True, help="the DAYFALL map folder to write (created if missing)")
    ap.add_argument("--map", help="level to load first, e.g. /Game/ClaudeFortress/Maps/L_Islet_Fortress")
    ap.add_argument("--name", help="map name shown by the engine (default: the level name)")
    ap.add_argument("--format", choices=("auto", "gltf", "fbx"), default="auto")
    ap.add_argument("--terrain", choices=("auto", "none"), default="auto", help="auto: trace landscapes into a heightfield")
    ap.add_argument("--terrain-actors", default="", help="comma-separated actor label patterns traced into the heightfield instead of exported as meshes")
    ap.add_argument("--terrain-spacing", type=float, default=0.0, help="metres between height samples (default: the landscape's)")
    ap.add_argument("--terrain-max-samples", type=int, default=4097)
    ap.add_argument("--instance-threshold", type=int, default=16, help="this many identical placed meshes become one instance file")
    ap.add_argument("--only-selected", action="store_true")
    ap.add_argument("--skip-textures", action="store_true")
    ap.add_argument("--skip-meshes", action="store_true", help="keep existing assets/ and _fbx/; re-export everything else")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    os.makedirs(args.out, exist_ok=True)
    ex = Exporter(args)
    try:
        ex.collect()
        ex.export_meshes()
        ex.export_materials()
        ex.export_terrain()
        ex.write_map()
    except Exception as e:
        ex.report["error"] = f"{e}\n{traceback.format_exc()}"
        unreal.log_error(LOG_PREFIX + f"failed: {e}\n{traceback.format_exc()}")
    finally:
        ex.write_report()


if __name__ == "__main__":
    main()
