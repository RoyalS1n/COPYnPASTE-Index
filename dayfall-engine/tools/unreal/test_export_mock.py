"""Run export_level_to_dayfall.py against a fake `unreal` module, then load the result in the engine.

    python tools/unreal/test_export_mock.py            # export a fake level, check map.json
    python tools/unreal/test_export_mock.py --engine   # also load it in bin/dayfall and check positions

The mock implements only the Unreal Python API the exporter calls, with the behaviour documented for UE 5
(left-handed centimetres, FColor light colours, get_instance_transform returning a Transform, and so on). It
catches mistakes in the exporter's own code; it cannot prove what a real Unreal build returns, so the first real
export must still be checked against unreal_export_report.json and captures (docs/PORTING.md).
"""
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_dayfall_convert as T  # noqa: E402  (GLB writer, boxes, fake landscape)


# ---------------------------------------------------------------- the fake unreal module

def make_unreal():
    u = types.ModuleType("unreal")

    class Enum:
        def __init__(self, owner, name):
            self.owner, self.name = owner, name

        def __repr__(self):
            return f"<{self.owner}.{self.name}: 0>"
        __str__ = __repr__

        def __eq__(self, o):
            return isinstance(o, Enum) and o.name == self.name

        def __hash__(self):
            return hash(self.name)

    def enum(owner, *names):
        return type(owner, (), {n: Enum(owner, n) for n in names})

    u.CollisionEnabled = enum("CollisionEnabled", "NO_COLLISION", "QUERY_ONLY", "PHYSICS_ONLY", "QUERY_AND_PHYSICS")
    u.CollisionResponseType = enum("CollisionResponseType", "ECR_IGNORE", "ECR_OVERLAP", "ECR_BLOCK")
    u.CollisionChannel = enum("CollisionChannel", "ECC_PAWN", "ECC_VISIBILITY")
    u.BlendMode = enum("BlendMode", "BLEND_OPAQUE", "BLEND_MASKED", "BLEND_TRANSLUCENT")
    u.MaterialShadingModel = enum("MaterialShadingModel", "MSM_DEFAULT_LIT", "MSM_TWO_SIDED_FOLIAGE", "MSM_UNLIT")
    u.TextureCompressionSettings = enum("TextureCompressionSettings", "TC_DEFAULT", "TC_NORMALMAP", "TC_MASKS")
    u.LightUnits = enum("LightUnits", "UNITLESS", "CANDELAS", "LUMENS", "EV")
    u.GLTFMaterialBakeMode = enum("GLTFMaterialBakeMode", "DISABLED", "SIMPLE", "USE_MESH_DATA")

    class Vector:
        def __init__(self, x=0.0, y=0.0, z=0.0):
            self.x, self.y, self.z = float(x), float(y), float(z)

    class Quat:
        def __init__(self, x=0.0, y=0.0, z=0.0, w=1.0):
            self.x, self.y, self.z, self.w = x, y, z, w

    class Rotator:
        def __init__(self, roll=0.0, pitch=0.0, yaw=0.0):
            self.roll, self.pitch, self.yaw = roll, pitch, yaw

        def quaternion(self):   # pitch, then yaw (roll is not used by the fake level)
            cy, sy = math.cos(math.radians(self.yaw) / 2), math.sin(math.radians(self.yaw) / 2)
            cp, sp = math.cos(math.radians(self.pitch) / 2), math.sin(math.radians(self.pitch) / 2)
            qy = (0, 0, sy, cy)
            qp = (0, -sp, 0, cp)   # positive pitch looks up: X turns towards +Z
            x, y, z, w = T.qmul(qy, qp)
            return Quat(x, y, z, w)

    class Transform:
        def __init__(self, translation=None, rotation=None, scale3d=None):
            self.translation = translation or Vector()
            self.rotation = rotation or Quat()
            self.scale3d = scale3d or Vector(1, 1, 1)

    class Color:
        def __init__(self, r=255, g=255, b=255, a=255):
            self.r, self.g, self.b, self.a = r, g, b, a

    class LinearColor:
        def __init__(self, r=1.0, g=1.0, b=1.0, a=1.0):
            self.r, self.g, self.b, self.a = r, g, b, a

    class Box:
        def __init__(self, mn, mx):
            self.min, self.max = mn, mx

    class Obj:
        _path = "/Game/Fake"

        def __init__(self, name="obj", **kw):
            self._name = name
            for k, v in kw.items():
                setattr(self, k, v)

        def get_name(self):
            return self._name

        def get_path_name(self):
            return f"{self._path}/{self._name}.{self._name}"

        def get_editor_property(self, n):
            if not hasattr(self, n):
                raise AttributeError(n)
            return getattr(self, n)

        def set_editor_property(self, n, v):
            setattr(self, n, v)

        @classmethod
        def static_class(cls):
            return cls

        def get_class(self):
            c = type(self)
            return types.SimpleNamespace(get_name=lambda: c.__name__)

    # assets
    class MaterialInterface(Obj):
        pass

    class Material(MaterialInterface):
        blend_mode = u.BlendMode.BLEND_OPAQUE
        two_sided = False
        shading_model = u.MaterialShadingModel.MSM_DEFAULT_LIT
        opacity_mask_clip_value = 0.3333
        textures = ()

        def get_base_material(self):
            return self

    class MaterialInstance(MaterialInterface):
        parent = None
        tex_params, scalars, vectors = {}, {}, {}
        base_property_overrides = None

        def get_base_material(self):
            return self.parent.get_base_material()

    class MaterialInstanceConstant(MaterialInstance):
        pass

    class Texture(Obj):
        compression_settings = u.TextureCompressionSettings.TC_DEFAULT
        srgb = True

    class Texture2D(Texture):
        pass

    class StaticMaterial(Obj):
        pass

    class StaticMesh(Obj):
        verts, idx, static_materials = (), (), ()
        nanite_settings = None

        def get_bounding_box(self):
            xs, ys, zs = zip(*self.verts)
            return Box(Vector(min(xs), min(ys), min(zs)), Vector(max(xs), max(ys), max(zs)))

        def get_num_triangles(self, lod):
            return len(self.idx) // 3

    # components and actors
    class ActorComponent(Obj):
        owner = None

        def get_owner(self):
            return self.owner

    class SceneComponent(ActorComponent):
        transform = None

        def get_world_transform(self):
            return self.transform or self.owner.transform

        def get_world_location(self):
            return self.get_world_transform().translation

        def is_visible(self):
            return getattr(self, "visible", True)

    class PrimitiveComponent(SceneComponent):
        collision = u.CollisionEnabled.QUERY_AND_PHYSICS
        pawn_response = u.CollisionResponseType.ECR_BLOCK
        hidden_in_game = False
        cast_shadow = True

        def get_collision_enabled(self):
            return self.collision

        def get_collision_response_to_channel(self, ch):
            return self.pawn_response

    class MeshComponent(PrimitiveComponent):
        override_materials = ()

    class StaticMeshComponent(MeshComponent):
        static_mesh = None
        ld_max_draw_distance = 0.0

    class InstancedStaticMeshComponent(StaticMeshComponent):
        instances = ()
        instance_end_cull_distance = 0

        def get_instance_count(self):
            return len(self.instances)

        def get_instance_transform(self, i, world_space=False):
            t = self.instances[i]
            assert world_space
            return t

    class HierarchicalInstancedStaticMeshComponent(InstancedStaticMeshComponent):
        pass

    class LandscapeHeightfieldCollisionComponent(PrimitiveComponent):
        lo = hi = None
        func = None

        def k2_line_trace_component(self, start, end, complex_, show, persistent):
            if not (self.lo[0] <= start.x <= self.hi[0] and self.lo[1] <= start.y <= self.hi[1]):
                return None
            z = self.func(start.x, start.y)
            return (Vector(start.x, start.y, z), Vector(0, 0, 1), "None", object())

    class LightComponentBase(SceneComponent):
        visible = True
        light_color = None
        use_temperature = False
        temperature = 6500.0
        intensity = 10.0

    class LightComponent(LightComponentBase):
        pass

    class DirectionalLightComponent(LightComponent):
        atmosphere_sun_light = True

    class LocalLightComponent(LightComponent):
        intensity_units = u.LightUnits.CANDELAS
        attenuation_radius = 1000.0

    class PointLightComponent(LocalLightComponent):
        pass

    class SpotLightComponent(PointLightComponent):
        pass

    class RectLightComponent(LocalLightComponent):
        pass

    class SkyLightComponent(LightComponentBase):
        pass

    class CameraComponent(SceneComponent):
        field_of_view = 90.0
        aspect_ratio = 16 / 9

    class CineCameraComponent(CameraComponent):
        pass

    class CapsuleComponent(PrimitiveComponent):
        half = 92.0

        def get_scaled_capsule_half_height(self):
            return self.half

    class ExponentialHeightFogComponent(SceneComponent):
        fog_density = 0.02
        fog_height_falloff = 0.2
        start_distance = 0.0

    class Actor(Obj):
        transform = None
        label = ""
        hidden = False

        def __init__(self, name, transform=None, components=(), **kw):
            super().__init__(name, **kw)
            self.label = name
            self.transform = transform or Transform()
            self.components = list(components)
            for c in self.components:
                c.owner = self

        def get_actor_label(self):
            return self.label

        def get_components_by_class(self, cls):
            return [c for c in self.components if isinstance(c, cls)]

        def get_component_by_class(self, cls):
            r = self.get_components_by_class(cls)
            return r[0] if r else None

        def get_actor_location(self):
            return self.transform.translation

        def get_actor_rotation(self):
            return self.rotator

        def get_actor_forward_vector(self):
            q = self.transform.rotation
            f = T.qrot((q.x, q.y, q.z, q.w), (1, 0, 0))
            return Vector(*f)

        def get_actor_scale3d(self):
            return self.transform.scale3d

    StaticMeshActor = type("StaticMeshActor", (Actor,), {})
    PlayerStart = type("PlayerStart", (Actor,), {})
    CameraActor = type("CameraActor", (Actor,), {})
    CineCameraActor = type("CineCameraActor", (CameraActor,), {})
    PointLight = type("PointLight", (Actor,), {})
    DirectionalLight = type("DirectionalLight", (Actor,), {})
    SkyLight = type("SkyLight", (Actor,), {})
    ExponentialHeightFog = type("ExponentialHeightFog", (Actor,), {})
    LandscapeProxy = type("LandscapeProxy", (Actor,), {})
    Landscape = type("Landscape", (LandscapeProxy,), {})

    # libraries
    class SystemLibrary:
        @staticmethod
        def get_component_bounds(comp):
            if isinstance(comp, LandscapeHeightfieldCollisionComponent):
                lo, hi = comp.lo, comp.hi
            else:
                bb = comp.static_mesh.get_bounding_box()
                t = comp.get_world_transform()
                q = t.rotation
                pts = []
                for x in (bb.min.x, bb.max.x):
                    for y in (bb.min.y, bb.max.y):
                        for z in (bb.min.z, bb.max.z):
                            v = (x * t.scale3d.x, y * t.scale3d.y, z * t.scale3d.z)
                            r = T.qrot((q.x, q.y, q.z, q.w), v)
                            pts.append((r[0] + t.translation.x, r[1] + t.translation.y, r[2] + t.translation.z))
                lo = [min(p[k] for p in pts) for k in range(3)]
                hi = [max(p[k] for p in pts) for k in range(3)]
            o = Vector(*[(a + b) / 2 for a, b in zip(lo, hi)])
            e = Vector(*[(b - a) / 2 for a, b in zip(lo, hi)])
            return o, e, math.sqrt(e.x ** 2 + e.y ** 2 + e.z ** 2)

    class MaterialEditingLibrary:
        @staticmethod
        def get_texture_parameter_names(m):
            return list(m.tex_params)

        @staticmethod
        def get_material_instance_texture_parameter_value(m, n):
            return m.tex_params[n]

        @staticmethod
        def get_scalar_parameter_names(m):
            return list(m.scalars)

        @staticmethod
        def get_material_instance_scalar_parameter_value(m, n):
            return m.scalars[n]

        @staticmethod
        def get_vector_parameter_names(m):
            return list(m.vectors)

        @staticmethod
        def get_material_instance_vector_parameter_value(m, n):
            return m.vectors[n]

        @staticmethod
        def get_used_textures(m):
            if not isinstance(m, Material):
                raise TypeError("expects a Material")   # as in some UE versions
            return list(m.textures)

    class ScopedSlowTask:
        def __init__(self, n, msg=""):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def make_dialog(self, cancel=False):
            pass

        def should_cancel(self):
            return False

        def enter_progress_frame(self, n=1, msg=""):
            pass

    def png8(path, rgba):
        raw = b"\0" + bytes(rgba) * 4
        raw = raw * 4
        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

    class AssetExportTask(Obj):
        object = filename = options = exporter = None
        automated = prompt = replace_identical = False

        def __init__(self):
            super().__init__("task")

    class GLTFExportOptions(Obj):
        export_uniform_scale = 0.01

        def __init__(self):
            super().__init__("opts")

    class FbxExportOption(Obj):
        def __init__(self):
            super().__init__("fbx")

    class StaticMeshExporterFBX(Obj):
        def __init__(self):
            super().__init__("fbxexp")

    def write_mesh(mesh, path, scale):
        mats = [s.material_interface.get_name() for s in mesh.static_materials]
        T.write_glb(path, [(x * scale * 100, y * scale * 100, z * scale * 100) for x, y, z in mesh.verts], list(mesh.idx), mats[0])

    class Exporter:
        @staticmethod
        def run_asset_export_task(task):
            o = task.object
            if isinstance(o, Texture):
                if not task.filename.endswith(".png"):
                    return False
                png8(task.filename, (200, 120, 60, 255) if "BC" in o.get_name() else (128, 128, 255, 255))
                return True
            if isinstance(o, StaticMesh):
                if isinstance(task.options, GLTFExportOptions):
                    write_mesh(o, task.filename, task.options.export_uniform_scale)
                    return True
                with open(task.filename, "wb") as f:
                    f.write(b"FBX mock")
                return True
            return False

    class GLTFExporter:
        @staticmethod
        def export_to_gltf(obj, path, options, selected):
            write_mesh(obj, path, options.export_uniform_scale)
            return True, object()

    state = types.SimpleNamespace(actors=[], world=Obj("L_Fake"))

    class UnrealEditorSubsystem:
        pass

    class EditorActorSubsystem:
        pass

    class LevelEditorSubsystem:
        pass

    def get_editor_subsystem(cls):
        if cls is UnrealEditorSubsystem:
            return types.SimpleNamespace(get_editor_world=lambda: state.world)
        if cls is EditorActorSubsystem:
            return types.SimpleNamespace(get_all_level_actors=lambda: state.actors, get_selected_level_actors=lambda: [])
        if cls is LevelEditorSubsystem:
            return types.SimpleNamespace(load_level=lambda p: True)
        raise TypeError(cls)

    u.log = lambda m: None
    u.log_warning = lambda m: None
    u.log_error = lambda m: print(m, file=sys.stderr)
    for k, v in list(locals().items()):
        if isinstance(v, type) or callable(v):
            if not k.startswith("_") and k not in ("u", "make_unreal", "enum", "png8", "write_mesh"):
                setattr(u, k, v)
    u.state = state
    return u


# ---------------------------------------------------------------- the fake level

def fake_level(u):
    V, Tf, Q = u.Vector, u.Transform, u.Quat

    def mat(name, cls=None, **kw):
        m = (cls or u.Material)(name, **kw)
        return m

    t_bc = u.Texture2D("T_Crate_BC")
    t_n = u.Texture2D("T_Crate_N", compression_settings=u.TextureCompressionSettings.TC_NORMALMAP, srgb=False)
    m_flag = mat("M_Flag")
    m_base = mat("M_Master", textures=[t_bc, t_n])
    mi_crate = mat("MI_Crate", u.MaterialInstanceConstant, parent=m_base, tex_params={"BaseColor": t_bc, "Normal": t_n},
                   scalars={"Roughness": 0.7}, vectors={})
    m_gold = mat("M_Gold", vectors={})
    m_leaf = mat("M_Leaves", blend_mode=u.BlendMode.BLEND_MASKED, two_sided=True,
                 shading_model=u.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)

    def mesh(name, parts, m):
        verts, idx = [], []
        for p in parts:
            v, i = T.box_ue(*p)
            idx += [k + len(verts) for k in i]
            verts += v
        return u.StaticMesh(name, verts=verts, idx=idx, static_materials=[u.StaticMaterial("s", material_interface=m)])

    sm_flag = mesh("SM_Flag", [(-20, -20, 0, 20, 20, 600), (-20, 20, 500, 20, 400, 600)], m_flag)
    sm_cube = mesh("SM_Cube", [(-50, -50, 0, 50, 50, 100)], mi_crate)
    sm_bush = mesh("SM_Bush", [(-40, -40, 0, 40, 40, 60)], m_leaf)

    def rot(yaw):
        return u.Rotator(0, 0, yaw).quaternion()

    def smc(m, t=None, **kw):
        return u.StaticMeshComponent("StaticMeshComponent0", static_mesh=m, transform=t, **kw)

    def at(x, y, dz=0.0, yaw=0.0, s=(1, 1, 1)):
        return Tf(V(x, y, T.ue_height(x, y) + dz), rot(yaw), V(*s))

    actors = []
    # landscape: two collision components side by side
    comps = []
    for x0 in (-6000, 0):
        c = u.LandscapeHeightfieldCollisionComponent("LandscapeHeightfieldCollisionComponent", lo=(x0, -6000, 0),
                                                     hi=(x0 + 6000, 6000, 1200), func=T.ue_height)
        comps.append(c)
    actors.append(u.Landscape("Landscape", Tf(V(-6000, -6000, 0), Q(), V(100, 100, 100)), comps))
    # the flag, turned 90 degrees
    a = u.StaticMeshActor("Flag", at(1000, -1000, yaw=90), [smc(sm_flag)])
    a.components[0].transform = a.transform
    actors.append(a)
    # a stretched wall with a material override
    a = u.StaticMeshActor("Wall", at(-2000, -2000, s=(4, 0.5, 2)), [smc(sm_cube, override_materials=[m_gold])])
    a.components[0].transform = a.transform
    actors.append(a)
    # 20 crates in a row: placed one by one in Unreal, grouped into one instance file
    for k in range(20):
        a = u.StaticMeshActor(f"Crate{k}", at(-5000 + k * 300, 4000), [smc(sm_cube)])
        a.components[0].transform = a.transform
        actors.append(a)
    # a hidden crate and a crate without collision
    a = u.StaticMeshActor("HiddenCrate", at(0, 4500), [smc(sm_cube)], hidden=True)
    actors.append(a)
    a = u.StaticMeshActor("GhostCrate", at(3000, -4000), [smc(sm_cube, collision=u.CollisionEnabled.NO_COLLISION)])
    a.components[0].transform = a.transform
    actors.append(a)
    # foliage: an HISM of stretched bushes
    inst = [at(-3000 + k * 200, 2000, s=(1.5, 1.5, 0.5 + 0.1 * k)) for k in range(10)]
    actors.append(u.Actor("FoliageActor", Tf(), [u.HierarchicalInstancedStaticMeshComponent(
        "HISM_Bush", static_mesh=sm_bush, instances=inst, collision=u.CollisionEnabled.NO_COLLISION)]))
    # lights, sun, camera, player start, fog, sky light
    pl = u.PointLightComponent("LightComponent0", intensity=8.0, light_color=u.Color(255, 200, 150), attenuation_radius=1500)
    a = u.PointLight("Lamp", at(500, 500, dz=300), [pl])
    pl.transform = a.transform
    actors.append(a)
    sun = u.DirectionalLight("Sun", Tf(V(0, 0, 5000), u.Rotator(0, -30, 45).quaternion()),
                             [u.DirectionalLightComponent("Light", intensity=10.0, light_color=u.Color(255, 240, 220))])
    actors.append(sun)
    cam = u.CameraActor("ShotCam", Tf(V(-2000, 3000, 1500), rot(-45)), [u.CameraComponent("CameraComponent")])
    actors.append(cam)
    ps = u.PlayerStart("PlayerStart", at(0, 0, dz=92.0, yaw=90), [u.CapsuleComponent("Capsule")])
    ps.rotator = u.Rotator(0, 0, 90)
    actors.append(ps)
    actors.append(u.ExponentialHeightFog("Fog", Tf(V(0, 0, 100)), [u.ExponentialHeightFogComponent("Fog")]))
    actors.append(u.SkyLight("SkyLight", Tf(), [u.SkyLightComponent("SkyLightComponent0")]))
    return actors


class MockExport(unittest.TestCase):
    def setUp(self):
        self.u = make_unreal()
        sys.modules["unreal"] = self.u
        if "export_level_to_dayfall" in sys.modules:
            del sys.modules["export_level_to_dayfall"]
        import export_level_to_dayfall as E
        self.E = E
        self.u.state.actors = fake_level(self.u)
        self.dir = tempfile.mkdtemp(prefix="dayfall_mock_")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def export(self, *extra):
        self.E.main(["--out", self.dir, "--name", "Mock Level", *extra])
        with open(os.path.join(self.dir, "unreal_export_report.json")) as f:
            rep = json.load(f)
        self.assertNotIn("error", rep, rep.get("error"))
        with open(os.path.join(self.dir, "map.json")) as f:
            return json.load(f), rep

    def test_map(self):
        doc, rep = self.export()
        self.assertEqual(doc["format"], "dayfall-map")
        self.assertEqual(rep["mesh_format"], "gltf")
        self.assertEqual(set(doc["meshes"]), {"SM_Flag", "SM_Cube", "SM_Bush"})
        ids = {o["id"]: o for o in doc["objects"]}
        self.assertIn("Flag", ids)
        self.assertEqual(ids["Wall"]["scale"], [4, 0.5, 2])
        self.assertEqual(ids["Wall"]["materials"], {"MI_Crate": "M_Gold"})
        self.assertEqual(ids["GhostCrate"]["collision"], "none")
        self.assertNotIn("HiddenCrate", ids)
        files = {f["mesh"]: f for f in doc["instance_files"]}
        self.assertEqual(files["SM_Cube"]["count"], 20)
        self.assertEqual(files["SM_Bush"]["layout"], "pos_quat_scale3")
        self.assertEqual(files["SM_Bush"]["collision"], "none")
        m = doc["materials"]
        self.assertEqual(m["MI_Crate"]["textures"]["base"], "textures/T_Crate_BC.png")
        self.assertEqual(m["MI_Crate"]["textures"]["normal"], "textures/T_Crate_N.png")
        self.assertEqual(m["MI_Crate"]["normal_convention"], "directx")
        self.assertEqual(m["M_Leaves"]["model"], "foliage")
        self.assertAlmostEqual(doc["player_start"]["yaw_deg"], -90)
        self.assertAlmostEqual(doc["player_start"]["position"][2], T.ue_height(0, 0) / 100, places=3)
        sun = doc["environment"]["sun"]
        self.assertAlmostEqual(sun["elevation_deg"], 30, places=1)
        # the light heads to Unreal (+x, +y) = engine south-east, so the sun stands north-west: azimuth -45
        self.assertAlmostEqual(sun["azimuth_deg"], -45, places=1)
        self.assertAlmostEqual(doc["lights"][0]["intensity"], 4.0, places=3)
        self.assertIn("ShotCam", doc["cameras"])
        self.assertIn("terrain", doc)
        self.assertTrue(os.path.exists(os.path.join(self.dir, "terrain", "height.png")))
        self.assertIn("fog", rep)

    def test_fbx_fallback(self):
        del self.u.GLTFExportOptions
        doc, rep = self.export()
        self.assertEqual(rep["mesh_format"], "fbx")
        with open(os.path.join(self.dir, "_fbx", "manifest.json")) as f:
            man = json.load(f)
        self.assertEqual(len(man["meshes"]), 3)
        self.assertEqual(man["meshes"][0]["glb"], "../assets/" + man["meshes"][0]["key"] + ".glb")

    @unittest.skipUnless("--engine" in sys.argv, "pass --engine to run against bin/dayfall")
    def test_engine(self):
        doc, rep = self.export()
        probes = {"arm": (700.0, -1000.0), "beside_arm": (1300.0, -1000.0), "wall": (-2000.0, -2000.0),
                  "crate": (-5000.0 + 5 * 300, 4000.0), "ground": (-4000.0, -4000.0), "bump": (2000.0, 3000.0)}
        pts = [[x / 100, -y / 100] for x, y in probes.values()]
        cf = os.path.join(self.dir, "calls.json")
        T.write_file(cf, json.dumps([{"tool": "ground_query", "args": {"points": pts}}, {"tool": "stats"}]))
        cmd = [T.Engine.exe, self.dir, "--headless", "--no-mcp", "--exec", cf]
        if shutil.which("xvfb-run") and os.name != "nt" and not os.environ.get("DISPLAY"):
            cmd = ["xvfb-run", "-a"] + cmd
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        out = "\n".join(l for l in r.stdout.splitlines() if not l.lstrip().startswith("[ ") or l.strip() in ("[", "]"))
        res = json.loads(out[out.index("["):])
        got = dict(zip(probes, res[0]["result"]["points"]))
        h = lambda x, y: T.ue_height(x, y) / 100   # noqa: E731
        self.assertAlmostEqual(got["arm"]["height"], h(1000, -1000) + 6.0, delta=0.05)
        self.assertLess(got["beside_arm"]["height"], h(1300, -1000) + 0.5)
        self.assertAlmostEqual(got["wall"]["height"], h(-2000, -2000) + 2.0, delta=0.05)
        self.assertAlmostEqual(got["crate"]["height"], h(-3500, 4000) + 1.0, delta=0.05)
        self.assertAlmostEqual(got["ground"]["terrain_height"], h(-4000, -4000), delta=0.05)
        self.assertAlmostEqual(got["bump"]["terrain_height"], h(2000, 3000), delta=0.05)
        self.assertEqual(res[1]["result"]["build"]["warnings"], [])


if __name__ == "__main__":
    unittest.main(argv=[a for a in sys.argv if a != "--engine"])
