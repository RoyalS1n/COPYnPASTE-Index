"""Build the reference world in Blender.

Runs either inside Blender or with the `bpy` pip module:

    blender -b -P build_world.py -- --preset golden_valley --render stills --export
    python  build_world.py --preset golden_valley --render stills --export

Outputs go to reference-world/build/<preset>/ (git-ignored):
    <preset>.blend, renders/*.png, renders/flythrough.mp4, exports/...
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import bpy  # noqa: E402

from worldgen import assets, camera, castle, export, lighting, materials, meadow, scatter  # noqa: E402
from worldgen.config import load_preset  # noqa: E402
from worldgen.terrain import Terrain  # noqa: E402


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    p = argparse.ArgumentParser()
    p.add_argument("--preset", default="golden_valley", help="preset name in presets/ or a JSON path")
    p.add_argument("--out", default=os.path.normpath(os.path.join(HERE, "..", "build")))
    p.add_argument("--render", choices=["none", "stills", "video", "all"], default="stills")
    p.add_argument("--export", action="store_true", help="write FBX/heightmap/scatter/manifest for Unreal")
    p.add_argument("--quick", action="store_true", help="lower resolution and instance counts for fast iteration")
    p.add_argument("--frames", default=None, help="video frame range override, e.g. 1-48")
    p.add_argument("--preview", action="store_true", help="--quick plus a single low-res hero still")
    p.add_argument("--stills", default=None, help="comma-separated still names to render (default: all)")
    p.add_argument("--ue-project", default=os.path.normpath(os.path.join(HERE, "..", "unreal", "ReferenceWorld")),
                   help="Unreal project that receives a copy of the exports in WorldData/ ('' to skip)")
    return p.parse_args(argv)


def log(msg, t0=[time.time()]):
    print(f"[worldgen {time.time() - t0[0]:6.1f}s] {msg}", flush=True)


def main():
    args = parse_args()
    cfg = load_preset(args.preset)
    if args.preview:
        args.quick = True
        keep = cfg["camera"].get("preview_stills", ["hero", "castle_tele", "hall_interior", "courtyard"])
        cfg["camera"]["stills"] = [st for st in cfg["camera"]["stills"] if st["name"] in keep]
    if args.quick:
        cfg["terrain"]["resolution"] = 1009
        for k in ("tree_count", "bush_count", "rock_count", "grass_count"):
            cfg["biome"][k] = cfg["biome"][k] // 3
        cfg["render"]["resolution"] = [960, 540]
        cfg["render"]["samples"] = 32
    if args.preview:
        cfg["render"]["resolution"] = [640, 360]
        cfg["render"]["samples"] = 16
    if args.stills:
        wanted = {n.strip() for n in args.stills.split(",")}
        cfg["camera"]["stills"] = [st for st in cfg["camera"]["stills"] if st["name"] in wanted]
    out = os.path.join(args.out, cfg["name"])
    os.makedirs(os.path.join(out, "renders"), exist_ok=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"

    def coll(name, parent=None):
        c = bpy.data.collections.new(name)
        (parent or scene.collection).children.link(c)
        return c

    c_world, c_scatter, c_lib, c_rig = coll("World"), coll("Scatter"), coll("Library"), coll("Rig")

    log("terrain")
    terrain = Terrain(cfg).build()
    mats = materials.build_all(cfg)
    t_ob = bpy.data.objects.new("RW_Terrain", terrain.to_mesh(bpy, "RW_Terrain"))
    t_ob.data.materials.append(mats["terrain"])
    c_world.objects.link(t_ob)
    far_ob = bpy.data.objects.new("RW_Terrain_Far", terrain.skirt_mesh(bpy, "RW_Terrain_Far"))
    far_ob.data.materials.append(mats["terrain"])
    c_world.objects.link(far_ob)
    detail_ob = pond_ob = None
    if getattr(terrain, "patch", None) is not None:
        # high-resolution marsh patch fills the hole cut in the main terrain
        detail_ob = bpy.data.objects.new("RW_TerrainDetail", terrain.patch_mesh(bpy, "RW_TerrainDetail"))
        detail_ob.data.materials.append(mats["terrain"])
        c_world.objects.link(detail_ob)
        pond_ob = bpy.data.objects.new("RW_PondWater", terrain.pond_water_mesh(bpy, "RW_PondWater"))
        pond_ob.data.materials.append(mats["pondwater"])
        c_world.objects.link(pond_ob)
        log(f"  marsh patch {len(detail_ob.data.vertices)} verts, water {len(pond_ob.data.polygons)} faces")
    else:
        import bmesh
        bm = bmesh.new()
        bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=terrain.half)
        wme = bpy.data.meshes.new("RW_Water")
        bm.to_mesh(wme)
        bm.free()
        wme.materials.append(mats["water"])
        w_ob = bpy.data.objects.new("RW_Water", wme)
        w_ob.location.z = cfg["biome"]["water_level_m"]
        c_world.objects.link(w_ob)

    castle_ob = crag_ob = props_ob = None
    castle_info = {}            # features info: lights, cameras, player start
    if cfg.get("castle", {}).get("enabled") and cfg["terrain"].get("mode") != "meadow":
        log("castle")
        castle_ob, castle_info = castle.build(cfg, terrain, mats, c_world)
        crag_ob = castle.build_cliff(cfg, terrain, mats, c_world)
        log(f"  castle {len(castle_ob.data.polygons)} faces, cliff {len(crag_ob.data.polygons)} faces")
    if cfg.get("meadow", {}).get("enabled"):
        log("meadow: fences + abandoned buildings")
        props_ob, castle_info = meadow.build(cfg, terrain, mats, c_world)
        log(f"  props {len(props_ob.data.polygons)} faces")
    if castle_info.get("lights"):
        c_lights = coll("CastleLights")
        for i, L in enumerate(castle_info["lights"]):
            ld = bpy.data.lights.new(f"RW_{L['kind']}_{i:03d}", "POINT")
            ld.energy = L["power"]
            ld.color = L["color"]
            ld.shadow_soft_size = L["radius"]
            lo = bpy.data.objects.new(ld.name, ld)
            lo.location = L["pos"]
            c_lights.objects.link(lo)
        log(f"  {len(castle_info['lights'])} feature lights")

    log("asset library")
    lib = assets.build_library(mats, c_lib, marsh=getattr(terrain, "patch", None) is not None)
    for ob in c_lib.all_objects:
        ob.location.x += 10000  # parked far away; instanced through Geometry Nodes

    log("camera + scatter")
    keys = camera.resolve_keys(cfg, terrain)
    cam = camera.build(cfg, keys, c_rig, scene)
    points = scatter.compute(cfg, terrain, keys)
    for k, v in points.items():
        log(f"  {k}: {len(v['x'])} instances")
    # Collection Info resets child transforms, so the park offset is ignored
    scatter.build_instancers(points, lib, c_scatter)

    lighting.build_world(cfg, scene)
    lighting.build_sun(cfg, c_rig)
    lighting.build_clouds(cfg, mats, c_world)
    lighting.render_settings(cfg, scene)
    lighting.build_compositor(cfg, scene)
    # exclude the library from the view layer: still instanced, never rendered directly
    scene.view_layers[0].layer_collection.children["Library"].exclude = True

    if args.export:
        log("export for Unreal")
        exp_dir = os.path.join(out, "exports")
        lowres = bpy.data.objects.new("SM_Terrain", terrain.to_mesh(bpy, "SM_Terrain", step=2))
        lowres.data.materials.append(mats["terrain"])
        far_exp = bpy.data.objects.new("SM_TerrainFar", far_ob.data)
        tmp = coll("ExportTmp")
        tmp.objects.link(lowres)
        tmp.objects.link(far_exp)
        # library objects must be selectable for FBX export: temporarily include
        scene.view_layers[0].layer_collection.children["Library"].exclude = False
        saved = {ob.name: ob.location.copy() for ob in c_lib.all_objects}
        for ob in c_lib.all_objects:
            ob.location.x -= 10000
        extra = {}
        if detail_ob is not None:
            extra = {"detail_mesh": ("SM_TerrainDetail", detail_ob), "water_mesh": ("SM_PondWater", pond_ob)}
        export.export_all(cfg, terrain, lib, points, camera.keys_for_export(keys), exp_dir, lowres, far_exp,
                          castle_ob, crag_ob, castle_info, props_ob, extra)
        for ob in c_lib.all_objects:
            ob.location = saved[ob.name]
        scene.view_layers[0].layer_collection.children["Library"].exclude = True
        bpy.data.objects.remove(lowres)
        bpy.data.objects.remove(far_exp)
        bpy.data.collections.remove(tmp)
        log(f"  wrote {exp_dir}")
        if args.ue_project:
            dst = os.path.join(args.ue_project, "WorldData", cfg["name"])
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(exp_dir, dst)
            log(f"  copied exports into {dst}")

    blend = os.path.join(out, f"{cfg['name']}.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend, compress=True)
    log(f"saved {blend}")

    if args.render in ("stills", "all"):
        for st in cfg["camera"]["stills"]:
            scene.view_settings.exposure = cfg["lighting"]["exposure"] + st.get("exposure", 0.0)
            if "camera" in st:
                if st["camera"] not in castle_info:
                    log(f"skip still {st['name']}: no {st['camera']} placed")
                    continue
                scene.camera = camera.static_shot(cfg, terrain, st, c_rig, castle_info[st["camera"]])
            elif "from" in st:
                scene.camera = camera.static_shot(cfg, terrain, st, c_rig)
            else:
                scene.camera = cam
                scene.frame_set(st["frame"])
            scene.render.filepath = os.path.join(out, "renders", f"{st['name']}.png")
            scene.render.image_settings.file_format = "PNG"
            log(f"render still {st['name']} ({'frame ' + str(st['frame']) if 'frame' in st else 'static shot'})")
            bpy.ops.render.render(write_still=True)

    if args.render in ("video", "all"):
        scene.camera = cam
        scene.view_settings.exposure = cfg["lighting"]["exposure"]
        lighting.render_settings(cfg, scene, preview=True)
        if args.frames:
            a, b = (int(v) for v in args.frames.split("-"))
            scene.frame_start, scene.frame_end = a, b
        frames_dir = os.path.join(out, "renders", "frames")
        os.makedirs(frames_dir, exist_ok=True)
        scene.render.filepath = os.path.join(frames_dir, "f_")
        scene.render.image_settings.file_format = "PNG"
        log(f"render video frames {scene.frame_start}-{scene.frame_end}")
        bpy.ops.render.render(animation=True)
        if shutil.which("ffmpeg"):
            mp4 = os.path.join(out, "renders", "flythrough.mp4")
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(cfg["camera"]["fps"]),
                            "-start_number", str(scene.frame_start), "-i", os.path.join(frames_dir, "f_%04d.png"),
                            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", mp4], check=True)
            log(f"wrote {mp4}")
    log("done")


if __name__ == "__main__":
    main()
