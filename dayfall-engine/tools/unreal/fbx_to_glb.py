"""Turn the FBX meshes written by export_level_to_dayfall.py into the GLB files DAYFALL loads.

Only needed when Unreal's glTF Exporter plugin is off (the export report then says mesh_format "fbx"):

    "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe" -b --factory-startup
        --python tools/unreal/fbx_to_glb.py -- <map folder>/_fbx/manifest.json

(or `python fbx_to_glb.py <manifest>` with the bpy module). Each FBX is imported and compared with the Unreal
bounds in the manifest. Blender's FBX reader can come out turned or scaled relative to Unreal, so the script
finds the axis permutation and scale that make the bounds agree, votes on one answer for the whole set (a single
mesh's box can be ambiguous; hundreds rarely are), applies it, keeps Unreal's material names, and writes
assets/<mesh>.glb. Results go to <map>/_fbx/convert_report.json. Delete _fbx/ once the map looks right.

Target space: DAYFALL mesh-local metres, i.e. Unreal (x, -y, z) / 100, the same as Unreal's glTF exporter gives.
"""
import itertools
import json
import os
import sys

import bpy
from mathutils import Matrix, Vector


def cli_args():
    a = sys.argv
    return a[a.index("--") + 1:] if "--" in a else a[1:]


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def axis_candidates():
    """Signed axis permutations as 3x3 matrices: identity first, then proper rotations, then mirrors."""
    out = []
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product((1, -1), repeat=3):
            m = Matrix(((0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)))
            for r in range(3):
                m[r][perm[r]] = float(signs[r])
            out.append(m)
    ident = Matrix.Identity(3)
    out.sort(key=lambda m: (m != ident, m.determinant() < 0))
    return out


AXES = axis_candidates()


def snap_scale(s):
    for p in (1.0, 0.01, 100.0, 0.1, 10.0, 0.001, 1000.0, 1 / 2.54, 2.54):
        if abs(s / p - 1.0) < 0.03:
            return p
    return s


def expected_box(entry):
    mn, mx = entry["ue_min_cm"], entry["ue_max_cm"]
    return (Vector((mn[0] / 100, -mx[1] / 100, mn[2] / 100)), Vector((mx[0] / 100, -mn[1] / 100, mx[2] / 100)))


def box_of(points):
    xs, ys, zs = zip(*points)
    return Vector((min(xs), min(ys), min(zs))), Vector((max(xs), max(ys), max(zs)))


def corners(mn, mx):
    return [Vector((x, y, z)) for x in (mn.x, mx.x) for y in (mn.y, mx.y) for z in (mn.z, mx.z)]


def box_error(m, s, got, want):
    tmn, tmx = box_of([(m @ c) * s for c in corners(*got)])
    diag = max((want[1] - want[0]).length, 1e-6)
    return ((tmn - want[0]).length + (tmx - want[1]).length) / diag


def import_fbx(path):
    reset()
    bpy.ops.import_scene.fbx(filepath=path, use_custom_normals=True)
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        raise RuntimeError("no mesh in the FBX")
    for o in meshes:   # bake object transforms (the FBX's unit and axis conversion live there) into the data
        mw = o.matrix_world.copy()
        o.parent = None
        o.data.transform(mw)
        o.matrix_world = Matrix.Identity(4)
    for o in list(bpy.context.scene.objects):
        if o.type != "MESH":
            bpy.data.objects.remove(o, do_unlink=True)
    if len(meshes) > 1:
        bpy.ops.object.select_all(action="DESELECT")
        for o in meshes:
            o.select_set(True)
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active or meshes[0]
    return obj


def best_fit(got, want):
    """(axis matrix, scale, error, unambiguous) for one mesh."""
    s = snap_scale(max((want[1] - want[0]).length, 1e-6) / max((got[1] - got[0]).length, 1e-6))
    errs = sorted(((box_error(m, s, got, want), k) for k, m in enumerate(AXES)), key=lambda e: (e[0], e[1]))
    best, k = errs[0]
    # unambiguous: no other axis choice comes close
    second = next((e for e, j in errs[1:] if AXES[j] != AXES[k]), 1.0)
    return k, s, best, second - best > 0.05


def export_glb(obj, path):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    props = set(bpy.ops.export_scene.gltf.get_rna_type().properties.keys())
    kw = dict(filepath=path, export_format="GLB", use_selection=True, export_apply=True, export_yup=True,
              export_tangents=True, export_normals=True, export_texcoords=True, export_materials="EXPORT",
              export_cameras=False, export_lights=False, export_animations=False)
    bpy.ops.export_scene.gltf(**{k: v for k, v in kw.items() if k in props or k == "filepath"})


def main():
    args = cli_args()
    if not args:
        raise SystemExit("usage: fbx_to_glb.py <map>/_fbx/manifest.json")
    man_path = os.path.abspath(args[0])
    base = os.path.dirname(man_path)
    with open(man_path) as f:
        entries = json.load(f)["meshes"]
    report = {"meshes": {}, "warnings": []}

    # pass 1: bounds as Blender reads them, and each mesh's own best fit
    fits = {}
    for e in entries:
        try:
            obj = import_fbx(os.path.join(base, e["fbx"]))
            got = box_of([v.co for v in obj.data.vertices])
            fits[e["key"]] = (got,) + best_fit(got, expected_box(e))
        except Exception as ex:
            report["meshes"][e["key"]] = {"error": str(ex)}
    votes = {}
    for got, k, s, err, clear in fits.values():
        if clear and err < 0.05:
            votes[(k, s)] = votes.get((k, s), 0) + 1
    if votes:
        (gk, gs), n = max(votes.items(), key=lambda kv: kv[1])
    else:
        gk, gs, n = 0, 0.01, 0
        report["warnings"].append("no mesh had unambiguous bounds; assumed Blender reads Unreal FBX as centimetres in place")
    report["global"] = {"axes": [list(r) for r in AXES[gk]], "scale": gs, "votes": n, "voters": len(fits)}

    # pass 2: convert with the global answer unless a mesh clearly disagrees
    for e in entries:
        key = e["key"]
        if key not in fits:
            continue
        got, k, s, err, clear = fits[key]
        want = expected_box(e)
        use_k, use_s = gk, gs
        gerr = box_error(AXES[gk], gs, got, want)
        if gerr > 0.05 and err < gerr * 0.5:
            use_k, use_s = k, s
            report["warnings"].append(f"{key}: uses its own fit (error {err:.3f}) instead of the global one ({gerr:.3f})")
        try:
            obj = import_fbx(os.path.join(base, e["fbx"]))
            m = AXES[use_k].to_4x4() @ Matrix.Scale(use_s, 4)
            if m != Matrix.Identity(4):
                obj.data.transform(m)
                if AXES[use_k].determinant() < 0:
                    obj.data.flip_normals()
            # Unreal material names, in slot order, when Blender named them differently
            names = [s.material.name if s.material else None for s in obj.material_slots]
            want_names = [n for n in e.get("materials", []) if n]
            if want_names and set(names) != set(want_names):
                if len(names) == len(want_names):
                    for slot, nm in zip(obj.material_slots, want_names):
                        if slot.material is None:
                            slot.material = bpy.data.materials.new(nm)
                        slot.material.name = nm
                    report["warnings"].append(f"{key}: materials {names} renamed in slot order to {want_names}")
                else:
                    report["warnings"].append(f"{key}: materials {names} do not match Unreal's {want_names}")
            out = os.path.normpath(os.path.join(base, e["glb"]))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            export_glb(obj, out)
            final = box_of([v.co for v in obj.data.vertices])
            fe = ((final[0] - want[0]).length + (final[1] - want[1]).length) / max((want[1] - want[0]).length, 1e-6)
            report["meshes"][key] = {"glb": e["glb"], "box_error": round(fe, 4), "triangles": sum(len(p.vertices) - 2 for p in obj.data.polygons)}
            if fe > 0.05:
                report["warnings"].append(f"{key}: bounds still differ from Unreal's by {fe:.0%}; check it in a capture")
        except Exception as ex:
            report["meshes"][key] = {"error": str(ex)}
    with open(os.path.join(base, "convert_report.json"), "w") as f:
        json.dump(report, f, indent=1)
    bad = [k for k, v in report["meshes"].items() if "error" in v]
    print(f"[fbx_to_glb] {len(entries) - len(bad)} of {len(entries)} meshes converted; global axes vote {n}/{len(fits)}; "
          f"{len(report['warnings'])} warnings; see {os.path.join(base, 'convert_report.json')}")
    if bad:
        print(f"[fbx_to_glb] failed: {', '.join(bad)}")


if __name__ == "__main__":
    main()
