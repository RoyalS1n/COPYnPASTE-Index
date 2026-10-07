# Builds content/characters/rigged_dummy.glb: a small rigged humanoid (capsule
# limbs, 17 bones, smooth weights at the joints, a visor parented to the head bone) with idle / walk / run / jump /
# fall / land actions, for testing skeletal animation in the engine.
#
#   python tools/make_rigged_dummy.py [out.glb]      (a Python with the `bpy` module, Blender 4.4+)
#
# Blender space: Z up, the character faces -Y (Blender's front; glTF +Z after export),
# its left side is +X. 1.8 m tall, feet at z = 0. Every bone's local X axis is world
# +X in the rest pose, so a rotation about local X swings a limb forward / back
# (positive: the lower end moves back, -Y is forward). Each action stores its
# ground speed as a custom property "speed_mps" (exported as glTF animation extras).
import math
import os
import sys

import bpy  # first: the bpy module makes bmesh and mathutils importable
import bmesh
from mathutils import Matrix, Vector

FPS = 30
OUT = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].endswith(".glb") else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "content", "characters", "rigged_dummy.glb")

# ---------------------------------------------------------------- skeleton: name, parent, head, tail
BONES = [
    ("hips", None, (0, 0, 0.92), (0, 0, 1.04)),
    ("spine", "hips", (0, 0, 1.04), (0, 0, 1.24)),
    ("chest", "spine", (0, 0, 1.24), (0, 0, 1.46)),
    ("neck", "chest", (0, 0, 1.46), (0, 0, 1.56)),
    ("head", "neck", (0, 0, 1.56), (0, 0, 1.80)),
]
for side, s in (("L", 1), ("R", -1)):
    BONES += [
        (f"upper_arm.{side}", "chest", (0.21 * s, 0, 1.43), (0.24 * s, 0, 1.15)),
        (f"forearm.{side}", f"upper_arm.{side}", (0.24 * s, 0, 1.15), (0.26 * s, 0, 0.90)),
        (f"hand.{side}", f"forearm.{side}", (0.26 * s, 0, 0.90), (0.265 * s, 0, 0.80)),
        (f"thigh.{side}", "hips", (0.1 * s, 0, 0.94), (0.1 * s, 0, 0.52)),
        (f"shin.{side}", f"thigh.{side}", (0.1 * s, 0, 0.52), (0.1 * s, 0.01, 0.1)),
        (f"foot.{side}", f"shin.{side}", (0.1 * s, 0.01, 0.1), (0.1 * s, -0.13, 0.03)),
    ]
PARENT = {b[0]: b[1] for b in BONES}
# the bone that continues a limb (weights blend into it near the tail)
CHILD = {"hips": "spine", "spine": "chest", "chest": "neck", "neck": "head"}
for side in "LR":
    CHILD[f"upper_arm.{side}"] = f"forearm.{side}"
    CHILD[f"forearm.{side}"] = f"hand.{side}"
    CHILD[f"thigh.{side}"] = f"shin.{side}"
    CHILD[f"shin.{side}"] = f"foot.{side}"


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = FPS


def material(name, color, rough=0.6, emit=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    if emit:
        bsdf.inputs["Emission Color"].default_value = (*emit, 1.0)
        bsdf.inputs["Emission Strength"].default_value = 2.0
    return m


# ---------------------------------------------------------------- geometry
def capsule(bm, a, b, r, mat, seg=16, rings=8):
    """A capsule from point a to point b (sphere centres), radius r; returns its new verts."""
    a, b = Vector(a), Vector(b)
    axis = b - a
    length = axis.length
    ret = bmesh.ops.create_uvsphere(bm, u_segments=seg, v_segments=rings, radius=r)
    verts = ret["verts"]
    for v in verts:   # stretch the upper half into a cylinder
        if v.co.z > 1e-6:
            v.co.z += length
    rot = axis.normalized().to_track_quat("Z", "Y").to_matrix().to_4x4()
    bmesh.ops.transform(bm, matrix=Matrix.Translation(a) @ rot, verts=verts)
    faces = {f for v in verts for f in v.link_faces}
    for f in faces:
        f.material_index = mat
        f.smooth = True
    return verts


def box(bm, center, size, mat, smooth=False):
    ret = bmesh.ops.create_cube(bm, size=1.0)
    verts = ret["verts"]
    bmesh.ops.transform(bm, matrix=Matrix.Translation(Vector(center)) @ Matrix.Diagonal((*size, 1.0)), verts=verts)
    for f in {f for v in verts for f in v.link_faces}:
        f.material_index = mat
        f.smooth = smooth
    return verts


def build_mesh():
    mats = [material("dummy_body", (0.55, 0.6, 0.66), 0.5), material("dummy_joint", (0.12, 0.13, 0.15), 0.4)]
    BODY, JOINT = 0, 1
    bm = bmesh.new()
    parts = []   # (verts, bone)
    parts.append((box(bm, (0, 0, 0.97), (0.3, 0.18, 0.16), JOINT, True), "hips"))
    parts.append((capsule(bm, (0, 0, 1.12), (0, 0, 1.3), 0.15, BODY, 20, 10), "torso"))
    parts.append((box(bm, (0, 0, 1.39), (0.44, 0.22, 0.14), BODY, True), "chest"))
    parts.append((capsule(bm, (0, 0, 1.46), (0, 0, 1.56), 0.05, JOINT), "neck"))
    parts.append((capsule(bm, (0, 0, 1.66), (0, 0, 1.70), 0.11, BODY, 20, 12), "head"))
    for side, s in (("L", 1), ("R", -1)):
        parts.append((capsule(bm, (0.21 * s, 0, 1.40), (0.24 * s, 0, 1.17), 0.055, BODY), f"upper_arm.{side}"))
        parts.append((capsule(bm, (0.24 * s, 0, 1.13), (0.26 * s, 0, 0.92), 0.045, BODY), f"forearm.{side}"))
        parts.append((capsule(bm, (0.262 * s, 0, 0.86), (0.265 * s, 0, 0.82), 0.05, JOINT), f"hand.{side}"))
        parts.append((capsule(bm, (0.1 * s, 0, 0.88), (0.1 * s, 0, 0.56), 0.075, BODY), f"thigh.{side}"))
        parts.append((capsule(bm, (0.1 * s, 0, 0.48), (0.1 * s, 0.005, 0.14), 0.06, BODY), f"shin.{side}"))
        parts.append((box(bm, (0.1 * s, -0.05, 0.04), (0.11, 0.26, 0.08), JOINT, True), f"foot.{side}"))
    me = bpy.data.meshes.new("rigged_dummy")
    for m in mats:
        me.materials.append(m)
    # weights before freeing the bmesh (indices are stable after index_update)
    bm.verts.index_update()
    weights = {}
    for verts, bone in parts:
        for v in verts:
            weights[v.index] = vertex_weights(v.co, bone)
    bm.to_mesh(me)
    bm.free()
    return me, weights


def build_visor(rig):
    """an unskinned prop parented to the head bone (tests rigid attachments); faces -Y: shows the front"""
    bm = bmesh.new()
    box(bm, (0, -0.095, 1.69), (0.15, 0.04, 0.05), 0)
    me = bpy.data.meshes.new("visor")
    me.materials.append(material("dummy_visor", (0.9, 0.45, 0.1), 0.3, emit=(1.0, 0.5, 0.12)))
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new("visor", me)
    bpy.context.scene.collection.objects.link(ob)
    ob.parent = rig
    ob.parent_type = "BONE"
    ob.parent_bone = "head"
    bpy.context.view_layer.update()
    ob.matrix_world = Matrix.Identity(4)   # keep the vertices where they were modelled
    return ob


def along(co, bone):
    """distance along a bone from its head, and the bone length"""
    _, _, head, tail = next(b for b in BONES if b[0] == bone)
    head, tail = Vector(head), Vector(tail)
    d = tail - head
    return (Vector(co) - head).dot(d.normalized()), d.length


def vertex_weights(co, bone, blend=0.07):
    if bone == "torso":   # one capsule over spine and chest: blend at their joint (z 1.24)
        t = max(0.0, min(1.0, (co.z - 1.17) / 0.14))
        w = {"spine": 1 - t, "chest": t}
        if co.z < 1.1:
            h = max(0.0, min(1.0, (1.1 - co.z) / 0.08)) * 0.5
            w = {"hips": h, "spine": 1 - h}
        return w
    s, length = along(co, bone)
    w = {bone: 1.0}
    p, c = PARENT.get(bone), CHILD.get(bone)
    if p and s < blend and not bone.startswith("hand") and not bone.startswith("foot"):
        w[p] = 0.5 * (1 - max(0.0, s) / blend)
    if c and length - s < blend and not c.startswith("hand") and not c.startswith("foot") and bone not in ("hips", "neck"):
        w[c] = 0.5 * (1 - max(0.0, length - s) / blend)
    total = sum(w.values())
    return {k: v / total for k, v in w.items()}


def build_armature():
    arm = bpy.data.armatures.new("rig")
    ob = bpy.data.objects.new("rig", arm)
    bpy.context.scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode="EDIT")
    for name, parent, head, tail in BONES:
        eb = arm.edit_bones.new(name)
        eb.head, eb.tail = Vector(head), Vector(tail)
        y = (eb.tail - eb.head).normalized()
        eb.align_roll(Vector((1, 0, 0)).cross(y))   # local X = world +X
        if parent:
            eb.parent = arm.edit_bones[parent]
            eb.use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")
    for pb in ob.pose.bones:
        pb.rotation_mode = "XYZ"
    return ob


# ---------------------------------------------------------------- animation
def key(ob, frame, pose):
    """pose: {bone: (rx, ry, rz)} or {bone: ((rx, ry, rz), (lx, ly, lz))}; unlisted bones return to rest"""
    for pb in ob.pose.bones:
        v = pose.get(pb.name, (0, 0, 0))
        rot, loc = (v if isinstance(v[0], (tuple, list)) else (v, (0, 0, 0)))
        pb.rotation_euler = rot
        pb.location = loc
        pb.keyframe_insert("rotation_euler", frame=frame)
        pb.keyframe_insert("location", frame=frame)


def action(ob, name, frames, pose_at, speed=0.0, step=1):
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    ob.animation_data_create()
    ob.animation_data.action = act
    for f in range(0, frames + 1, step):
        key(ob, f, pose_at(f / frames))
    if speed:
        act["speed_mps"] = round(speed, 2)
    act.frame_range = (0, frames)
    act.use_frame_range = True
    return act


def gait(ph, swing, knee, arm, elbow, lean, bob, twist, flight=0.0):
    """one locomotion pose at cycle phase ph (0..1); the left foot strikes the ground at ph = 0"""
    c, s = math.cos(2 * math.pi * ph), math.sin(2 * math.pi * ph)
    pose = {}
    for side, off in (("L", 0.0), ("R", 0.5)):
        q = (ph + off) % 1.0
        cq = math.cos(2 * math.pi * q)
        thigh = -swing * cq                                    # forward at strike, back at toe-off
        swing_phase = max(0.0, math.sin(2 * math.pi * (q - 0.5))) if q >= 0.5 else 0.0
        stance = max(0.0, math.sin(2 * math.pi * q * 2)) if q < 0.5 else 0.0
        k = 0.12 + knee * swing_phase + 0.18 * stance + flight * 0.3
        pose[f"thigh.{side}"] = (thigh, 0, 0)
        pose[f"shin.{side}"] = (k, 0, 0)
        pose[f"foot.{side}"] = (-0.5 * (thigh + k) + 0.25 * swing_phase, 0, 0)
        a = arm * cq                                           # arm swings against its own leg
        sgn = 1 if side == "L" else -1
        pose[f"upper_arm.{side}"] = (a, 0, -0.06 * sgn)
        pose[f"forearm.{side}"] = (-(elbow + 0.25 * elbow * max(0.0, -cq)), 0, 0)
    up = bob * math.cos(4 * math.pi * ph)                      # high at mid-stance, low at strike
    pose["hips"] = ((0, twist * s, 0), (0, up, 0))
    pose["spine"] = (-lean, -twist * s * 0.5, 0)
    pose["chest"] = (-lean * 0.3, -twist * s * 0.8, 0)
    pose["head"] = (lean * 0.8, 0, 0)
    return pose


def lerp_pose(a, b, t):
    t = t * t * (3 - 2 * t)
    out = {}
    for k in set(a) | set(b):
        va, vb = a.get(k, (0, 0, 0)), b.get(k, (0, 0, 0))
        if not isinstance(va[0], (tuple, list)):
            va = (va, (0, 0, 0))
        if not isinstance(vb[0], (tuple, list)):
            vb = (vb, (0, 0, 0))
        out[k] = (tuple(x + (y - x) * t for x, y in zip(va[0], vb[0])), tuple(x + (y - x) * t for x, y in zip(va[1], vb[1])))
    return out


def keyed(poses):
    """poses: [(time 0..1, pose)] -> pose_at(t) with smooth steps between them"""
    def at(t):
        for (t0, p0), (t1, p1) in zip(poses, poses[1:]):
            if t <= t1:
                return lerp_pose(p0, p1, (t - t0) / max(t1 - t0, 1e-6))
        return poses[-1][1]
    return at


def idle_pose(ph):
    br = math.sin(2 * math.pi * ph)
    return {"hips": ((0, 0, 0), (0, -0.006 + 0.006 * br, 0)), "spine": (-0.02 * br, 0, 0), "chest": (-0.03 * br, 0, 0),
            "head": (0.03 * br, 0.05 * math.sin(2 * math.pi * ph + 1), 0),
            "upper_arm.L": (0.03 * br, 0, -0.08), "upper_arm.R": (0.03 * br, 0, 0.08),
            "forearm.L": (-0.15 - 0.03 * br, 0, 0), "forearm.R": (-0.15 - 0.03 * br, 0, 0),
            "thigh.L": (-0.03, 0, 0.02), "thigh.R": (-0.03, 0, -0.02), "shin.L": (0.06, 0, 0), "shin.R": (0.06, 0, 0),
            "foot.L": (-0.03, 0, 0), "foot.R": (-0.03, 0, 0)}


CROUCH = {"hips": ((0, 0, 0), (0, -0.2, 0)), "spine": (-0.35, 0, 0), "chest": (-0.1, 0, 0), "head": (0.3, 0, 0),
          "thigh.L": (-0.75, 0, 0), "thigh.R": (-0.75, 0, 0), "shin.L": (1.3, 0, 0), "shin.R": (1.3, 0, 0),
          "foot.L": (-0.55, 0, 0), "foot.R": (-0.55, 0, 0), "upper_arm.L": (0.7, 0, -0.1), "upper_arm.R": (0.7, 0, 0.1),
          "forearm.L": (-0.4, 0, 0), "forearm.R": (-0.4, 0, 0)}
EXTEND = {"hips": ((0, 0, 0), (0, 0.04, 0)), "spine": (0.05, 0, 0), "head": (-0.1, 0, 0),
          "thigh.L": (0.1, 0, 0), "thigh.R": (0.15, 0, 0), "shin.L": (0.05, 0, 0), "shin.R": (0.1, 0, 0),
          "foot.L": (0.5, 0, 0), "foot.R": (0.5, 0, 0), "upper_arm.L": (-2.5, 0, -0.2), "upper_arm.R": (-2.5, 0, 0.2),
          "forearm.L": (-0.2, 0, 0), "forearm.R": (-0.2, 0, 0)}
TUCK = {"hips": ((0, 0, 0), (0, 0.0, 0)), "spine": (-0.2, 0, 0), "head": (0.15, 0, 0),
        "thigh.L": (-1.1, 0, 0), "thigh.R": (-0.6, 0, 0), "shin.L": (1.5, 0, 0), "shin.R": (1.1, 0, 0),
        "foot.L": (-0.2, 0, 0), "foot.R": (-0.2, 0, 0), "upper_arm.L": (-1.4, 0, -0.7), "upper_arm.R": (-1.4, 0, 0.7),
        "forearm.L": (-0.6, 0, 0), "forearm.R": (-0.6, 0, 0)}
STAND = idle_pose(0.0)


def fall_pose(ph):
    s = math.sin(2 * math.pi * ph)
    c = math.cos(2 * math.pi * ph)
    return {"hips": ((0, 0, 0), (0, 0, 0)), "spine": (0.12, 0, 0), "chest": (0.05, 0, 0), "head": (-0.25, 0, 0),
            "upper_arm.L": (-0.4 + 0.3 * s, 0, -1.6 - 0.25 * c), "upper_arm.R": (-0.4 - 0.3 * s, 0, 1.6 + 0.25 * c),
            "forearm.L": (-0.5 - 0.3 * c, 0, 0), "forearm.R": (-0.5 + 0.3 * c, 0, 0),
            "thigh.L": (-0.45 + 0.25 * s, 0, 0.05), "thigh.R": (-0.2 - 0.25 * s, 0, -0.05),
            "shin.L": (0.7 - 0.2 * s, 0, 0), "shin.R": (0.5 + 0.2 * s, 0, 0), "foot.L": (0.2, 0, 0), "foot.R": (0.2, 0, 0)}


def main():
    reset_scene()
    me, weights = build_mesh()
    body = bpy.data.objects.new("rigged_dummy", me)
    bpy.context.scene.collection.objects.link(body)
    rig = build_armature()
    groups = {b[0]: body.vertex_groups.new(name=b[0]) for b in BONES}
    for vi, w in weights.items():
        for bone, val in w.items():
            if val > 1e-4:
                groups[bone].add([vi], val, "REPLACE")
    body.parent = rig
    mod = body.modifiers.new("rig", "ARMATURE")
    mod.object = rig
    build_visor(rig)

    leg = 0.84
    # speed = two steps per cycle; a step is about 2 * leg * sin(swing) (stance + swing)
    walk_frames, walk_swing = 24, 0.42
    run_frames, run_swing = 18, 0.62
    walk_speed = 2 * (2 * leg * math.sin(walk_swing)) / (walk_frames / FPS)
    run_speed = 2 * (2.6 * leg * math.sin(run_swing)) / (run_frames / FPS)
    action(rig, "Idle", 60, idle_pose)
    action(rig, "Walk", walk_frames, lambda p: gait(p, walk_swing, 0.95, 0.4, 0.25, 0.04, 0.025, 0.08), walk_speed)
    action(rig, "Run", run_frames, lambda p: gait(p, run_swing, 1.6, 0.85, 1.25, 0.22, 0.05, 0.14, flight=0.4), run_speed)
    action(rig, "Jump", 18, keyed([(0, STAND), (0.12, CROUCH), (0.35, EXTEND), (1.0, TUCK)]))
    action(rig, "Fall", 24, fall_pose)
    action(rig, "Land", 15, keyed([(0, lerp_pose(TUCK, fall_pose(0), 0.5)), (0.2, CROUCH), (1.0, STAND)]))
    rig.animation_data.action = None
    for pb in rig.pose.bones:
        pb.rotation_euler = (0, 0, 0)
        pb.location = (0, 0, 0)

    os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=os.path.abspath(OUT), export_format="GLB", export_yup=True, export_apply=False,
                              export_animations=True, export_animation_mode="ACTIONS", export_extras=True,
                              export_skins=True, export_def_bones=False, export_force_sampling=True,
                              export_reset_pose_bones=True, export_materials="EXPORT")
    print(f"wrote {os.path.abspath(OUT)}: {len(me.vertices)} vertices, {len(BONES)} bones, "
          f"walk {walk_speed:.2f} m/s, run {run_speed:.2f} m/s")


main()
