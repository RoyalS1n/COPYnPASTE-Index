"""Test fbx_to_glb.py with Blender's Python (the bpy module or `blender -b --python ... --`).

    python tools/unreal/test_fbx_to_glb.py        # with a Python that has bpy

Builds meshes in DAYFALL mesh-local space, writes them as FBX with centimetre scale and several axis settings
(standing in for Unreal's FBX exporter, whose exact output Blender may read turned or scaled), runs the converter,
and checks that every GLB comes back in DAYFALL space with the right materials.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

import bpy
from mathutils import Matrix

HERE = os.path.dirname(os.path.abspath(__file__))


def add_box(name, mn, mx, mat):
    bpy.ops.mesh.primitive_cube_add(size=1)
    o = bpy.context.active_object
    o.name = name
    o.scale = [mx[i] - mn[i] for i in range(3)]
    o.location = [(mx[i] + mn[i]) / 2 for i in range(3)]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    m = bpy.data.materials.get(mat) or bpy.data.materials.new(mat)
    o.data.materials.append(m)
    return o


def build(kind, misread=None):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if kind == "flag":    # pole at the origin, arm towards DAYFALL -y (Unreal +y)
        a = add_box("pole", (-0.2, -0.2, 0), (0.2, 0.2, 6), "M_Pole")
        b = add_box("arm", (-0.2, -4.0, 5), (0.2, -0.2, 6), "M_Arm")
        objs = [a, b]
    elif kind == "ramp":  # asymmetric on every axis
        objs = [add_box("r1", (0, 0, 0), (3, 1, 0.5), "M_Ramp"), add_box("r2", (2, 0, 0.5), (3, 1, 2), "M_Ramp")]
    else:                 # a symmetric crate: its own box cannot tell rotations apart
        objs = [add_box("crate", (-0.5, -0.5, 0), (0.5, 0.5, 1), "M_Crate")]
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    o = bpy.context.active_object
    pts = [v.co.copy() for v in o.data.vertices]
    mn = [min(p[i] for p in pts) for i in range(3)]
    mx = [max(p[i] for p in pts) for i in range(3)]
    mats = [s.material.name for s in o.material_slots]
    if misread is not None:   # the file holds the mesh turned or mirrored, as if read with the wrong axes
        o.data.transform(misread)
        if misread.determinant() < 0:
            o.data.flip_normals()
    return mn, mx, mats


def main():
    d = tempfile.mkdtemp(prefix="fbx2glb_")
    try:
        fbx_dir = os.path.join(d, "_fbx")
        os.makedirs(fbx_dir)
        entries, want = [], {}
        runs = [(("-Z", "Y"), None, ""), (("Y", "Z"), None, ""), (("-Y", "Z"), None, ""), (("X", "Z"), None, ""),
                (("-Z", "Y"), Matrix.Rotation(1.5707963, 4, "Z"), "_turned"),
                (("-Z", "Y"), Matrix.Rotation(-1.5707963, 4, "X"), "_yup"),
                (("-Z", "Y"), Matrix.Scale(-1, 4, (0, 1, 0)), "_mirrored")]
        for axes, misread, tag in runs:
            for kind in ("flag", "ramp", "crate"):
                key = f"SM_{kind}_{axes[0].replace('-', 'n')}{axes[1]}{tag}"
                mn, mx, mats = build(kind, misread)
                bpy.ops.export_scene.fbx(filepath=os.path.join(fbx_dir, key + ".fbx"), use_selection=True,
                                         global_scale=100.0, apply_unit_scale=True, axis_forward=axes[0], axis_up=axes[1])
                # Unreal bounds of the same mesh: DAYFALL (x, y, z) m = Unreal (x, -y, z) / 100
                entries.append({"key": key, "fbx": key + ".fbx", "glb": f"../assets/{key}.glb",
                                "ue_min_cm": [mn[0] * 100, -mx[1] * 100, mn[2] * 100],
                                "ue_max_cm": [mx[0] * 100, -mn[1] * 100, mx[2] * 100], "materials": mats})
                want[key] = (mn, mx, sorted(mats))
            # one forward/up pair per run keeps the vote meaningful: Unreal writes every file the same way
            with open(os.path.join(fbx_dir, "manifest.json"), "w") as f:
                json.dump({"meshes": entries}, f)
            cmd = [sys.executable, os.path.join(HERE, "fbx_to_glb.py"), os.path.join(fbx_dir, "manifest.json")]
            r = subprocess.run(cmd, capture_output=True, text=True)
            print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-2000:])
            for e in entries:
                key = e["key"]
                bpy.ops.wm.read_factory_settings(use_empty=True)
                bpy.ops.import_scene.gltf(filepath=os.path.join(d, "assets", key + ".glb"))
                objs = [o for o in bpy.context.scene.objects if o.type == "MESH"]
                pts = [o.matrix_world @ v.co for o in objs for v in o.data.vertices]
                mn = [min(p[i] for p in pts) for i in range(3)]
                mx = [max(p[i] for p in pts) for i in range(3)]
                mats = sorted({s.material.name for o in objs for s in o.material_slots if s.material})
                wmn, wmx, wm = want[key]
                err = max(max(abs(a - b) for a, b in zip(mn, wmn)), max(abs(a - b) for a, b in zip(mx, wmx)))
                ok = err < 1e-3 and mats == wm
                print(f"{'ok  ' if ok else 'FAIL'} {key}: box error {err:.5f} m, materials {mats}")
                assert ok, key
            entries = []
        print("all FBX conversions match")
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    main()
