"""Sky, sun, cloud layer, render settings and the compositor grade.

Sun direction convention (verified against the sky texture):
    d = (sin(az)cos(el), cos(az)cos(el), sin(el))
with az a compass bearing measured clockwise from +Y (down the valley)."""
import math

import bpy
from mathutils import Vector


def sun_vector(cfg):
    lt = cfg["lighting"]
    el = math.radians(lt["sun_elevation_deg"])
    az = math.radians(lt["sun_azimuth_deg"])
    return Vector((math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), math.sin(el)))


def build_world(cfg, scene):
    lt = cfg["lighting"]
    world = bpy.data.worlds.new("RW_World")
    scene.world = world
    nt = world.node_tree
    nt.nodes.clear()
    sky = nt.nodes.new("ShaderNodeTexSky")
    sky.sky_type = lt["sky_type"]
    sky.sun_disc = False  # direct sun comes from the lamp
    sky.sun_elevation = math.radians(lt["sun_elevation_deg"])
    sky.sun_rotation = math.radians(lt["sun_azimuth_deg"])
    sky.air_density = lt["air_density"]
    sky.aerosol_density = lt["aerosol_density"]
    sky.ozone_density = lt["ozone_density"]
    bg = nt.nodes.new("ShaderNodeBackground")
    bg.inputs["Strength"].default_value = lt["sky_strength"]
    out = nt.nodes.new("ShaderNodeOutputWorld")
    nt.links.new(sky.outputs[0], bg.inputs[0])
    nt.links.new(bg.outputs[0], out.inputs["Surface"])
    world.mist_settings.start = lt["mist_start_m"]
    world.mist_settings.depth = lt["mist_depth_m"]
    world.mist_settings.falloff = "QUADRATIC"
    return world


def build_sun(cfg, coll):
    lt = cfg["lighting"]
    data = bpy.data.lights.new("RW_Sun", "SUN")
    data.energy = lt["sun_strength"]
    data.color = lt["sun_color"]
    data.angle = math.radians(lt["sun_angle_deg"])
    ob = bpy.data.objects.new("RW_Sun", data)
    ob.rotation_euler = sun_vector(cfg).to_track_quat("Z", "Y").to_euler()
    coll.objects.link(ob)
    return ob


def build_clouds(cfg, mats, coll):
    if not cfg["lighting"].get("clouds", True):
        return None
    import bmesh
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=25000)
    me = bpy.data.meshes.new("RW_Clouds")
    bm.to_mesh(me)
    bm.free()
    me.materials.append(mats["clouds"])
    ob = bpy.data.objects.new("RW_Clouds", me)
    ob.location = (0, 6000, 1500)
    ob.visible_shadow = False
    ob.visible_diffuse = False
    ob.visible_glossy = False
    coll.objects.link(ob)
    return ob


def render_settings(cfg, scene, preview=False):
    lt = cfg["lighting"]
    r = cfg["render"]
    scene.render.engine = "CYCLES"
    c = scene.cycles
    c.device = "CPU"
    c.samples = r["video_samples"] if preview else r["samples"]
    c.use_adaptive_sampling = True
    c.adaptive_threshold = 0.02
    c.use_denoising = True
    c.denoiser = "OPENIMAGEDENOISE"
    c.max_bounces = 8
    c.diffuse_bounces = 3
    c.glossy_bounces = 3
    c.transmission_bounces = 8
    c.transparent_max_bounces = 12
    c.volume_bounces = 0
    c.caustics_reflective = False
    c.caustics_refractive = False
    c.blur_glossy = 1.0
    scene.render.film_transparent = True  # the sky comes back in the compositor
    # keep BVH/scene data between frames: the flythrough only moves the camera
    scene.render.use_persistent_data = True
    scene.render.use_compositing = True
    scene.render.compositor_device = "CPU"  # works headless; GPU compositing needs EGL
    res = r["video_resolution"] if preview else r["resolution"]
    scene.render.resolution_x, scene.render.resolution_y = res
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "AgX"
    try:
        scene.view_settings.look = lt["look"]
    except TypeError:
        scene.view_settings.look = "None"
    scene.view_settings.exposure = lt["exposure"]
    vl = scene.view_layers[0]
    vl.use_pass_mist = True
    vl.use_pass_environment = True


def build_compositor(cfg, scene):
    """Aerial perspective (mist pass blended to the haze colour), sky
    re-inserted behind geometry, bloom, slight chromatic dispersion and a
    soft vignette."""
    lt = cfg["lighting"]
    ng = bpy.data.node_groups.new("RW_Grade", "CompositorNodeTree")
    ng.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    scene.compositing_node_group = ng
    N, L = ng.nodes, ng.links

    rl = N.new("CompositorNodeRLayers")
    rl.scene = scene

    def math_node(op, a, b):
        m = N.new("ShaderNodeMath")
        m.operation = op
        for s, v in zip(m.inputs, (a, b)):
            if isinstance(v, (int, float)):
                s.default_value = v
            else:
                L.new(v, s)
        return m.outputs[0]

    def mix(fac, a, b, blend="MIX"):
        m = N.new("ShaderNodeMix")
        m.data_type = "RGBA"
        m.blend_type = blend
        if isinstance(fac, (int, float)):
            m.inputs[0].default_value = fac
        else:
            L.new(fac, m.inputs[0])
        for s, v in ((m.inputs[6], a), (m.inputs[7], b)):
            if isinstance(v, (tuple, list)):
                s.default_value = (*v[:3], 1.0)
            else:
                L.new(v, s)
        return m.outputs[2]

    # haze = image blended toward haze colour * coverage (premultiplied)
    haze_rgb = N.new("CompositorNodeRGB")
    haze_rgb.outputs[0].default_value = (*lt["haze_color"], 1.0)
    far = lt.get("haze_far_color", [lt["haze_color"][0] * 0.78, lt["haze_color"][1] * 0.9, lt["haze_color"][2] * 1.18])
    haze_far = N.new("CompositorNodeRGB")
    haze_far.outputs[0].default_value = (*far, 1.0)
    haze_col = mix(math_node("POWER", rl.outputs["Mist"], 1.5), haze_rgb.outputs[0], haze_far.outputs[0])
    haze_premul = mix(rl.outputs["Alpha"], (0, 0, 0), haze_col)  # haze * alpha
    fac = math_node("MULTIPLY", rl.outputs["Mist"], lt["haze_amount"])
    hazed = mix(fac, rl.outputs["Image"], haze_premul)

    # sky behind: env * (1 - alpha) + hazed
    inv_a = math_node("SUBTRACT", 1.0, rl.outputs["Alpha"])
    sky = mix(inv_a, (0, 0, 0), rl.outputs["Environment"])
    comp = mix(1.0, hazed, sky, "ADD")

    glare = N.new("CompositorNodeGlare")
    glare.inputs["Type"].default_value = "Bloom"
    glare.inputs["Threshold"].default_value = 0.8
    glare.inputs["Strength"].default_value = lt["bloom"]
    glare.inputs["Size"].default_value = 0.7
    L.new(comp, glare.inputs["Image"])

    # multiplicative warm gain only: an additive lift offset in scene-linear
    # light turns dark interiors (later brightened by exposure) blue
    grade = N.new("CompositorNodeColorBalance")
    grade.inputs[6].default_value = (1.0, 1.0, 1.0, 1.0)
    grade.inputs[8].default_value = (*lt.get("grade_gain", [1.035, 1.0, 0.955]), 1.0)
    L.new(glare.outputs["Image"], grade.inputs["Image"])
    lens = N.new("CompositorNodeLensdist")
    lens.inputs["Dispersion"].default_value = 0.012
    lens.inputs["Distortion"].default_value = -0.004
    L.new(grade.outputs["Image"], lens.inputs["Image"])

    ell = N.new("CompositorNodeEllipseMask")
    ell.inputs["Size"].default_value = (0.95, 0.8)
    blur = N.new("CompositorNodeBlur")
    blur.inputs["Size"].default_value = (250, 250)
    L.new(ell.outputs["Mask"], blur.inputs["Image"])
    vig = math_node("ADD", math_node("MULTIPLY", blur.outputs["Image"], lt["vignette"]), 1.0 - lt["vignette"])
    graded = mix(1.0, lens.outputs["Image"], vig, "MULTIPLY")

    out = N.new("NodeGroupOutput")
    L.new(graded, out.inputs[0])
    return ng
