"""Procedural Cycles materials. No image textures, so the project is
self-contained; every material is driven by the preset palette."""
import bpy


def _lin(c):
    return (c[0], c[1], c[2], 1.0)


class NB:
    """Tiny node-builder to keep material code readable."""

    def __init__(self, mat):
        self.nt = mat.node_tree
        self.nt.nodes.clear()
        self.x = 0

    def n(self, kind, **props):
        node = self.nt.nodes.new(kind)
        for k, v in props.items():
            if k.startswith("in_"):
                key = k[3:].replace("__", " ")
                node.inputs[key].default_value = v
            else:
                setattr(node, k, v)
        node.location = (self.x, 0)
        self.x += 180
        return node

    def link(self, a, b):
        self.nt.links.new(a, b)


def _new(name):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    return m


def _ramp(nb, stops):
    r = nb.n("ShaderNodeValToRGB")
    els = r.color_ramp.elements
    els[0].position, els[0].color = stops[0][0], _lin(stops[0][1])
    els[1].position, els[1].color = stops[-1][0], _lin(stops[-1][1])
    for pos, col in stops[1:-1]:
        e = els.new(pos)
        e.color = _lin(col)
    return r


def _mix(nb, fac, a, b, blend="MIX"):
    m = nb.n("ShaderNodeMix", data_type="RGBA", blend_type=blend)
    if isinstance(fac, (int, float)):
        m.inputs["Factor"].default_value = fac
    else:
        nb.link(fac, m.inputs["Factor"])
    for sock, val in ((m.inputs[6], a), (m.inputs[7], b)):
        if isinstance(val, (tuple, list)):
            sock.default_value = _lin(val)
        else:
            nb.link(val, sock)
    return m.outputs[2]


def _attr(nb, name):
    a = nb.n("ShaderNodeAttribute", attribute_name=name, attribute_type="GEOMETRY")
    return a.outputs["Fac"]


def _noise(nb, scale, detail=6.0, rough=0.55, coord=None, dim="3D"):
    t = nb.n("ShaderNodeTexNoise", noise_dimensions=dim)
    t.inputs["Scale"].default_value = scale
    t.inputs["Detail"].default_value = detail
    t.inputs["Roughness"].default_value = rough
    if coord is not None:
        nb.link(coord, t.inputs["Vector"])
    return t


def _math(nb, op, a, b=None, clamp=False):
    m = nb.n("ShaderNodeMath", operation=op, use_clamp=clamp)
    for sock, val in zip(m.inputs, (a, b)):
        if val is None:
            continue
        if isinstance(val, (int, float)):
            sock.default_value = val
        else:
            nb.link(val, sock)
    return m.outputs[0]


def _maprange(nb, val, a, b, c=0.0, d=1.0):
    m = nb.n("ShaderNodeMapRange")
    nb.link(val, m.inputs["Value"])
    m.inputs["From Min"].default_value = a
    m.inputs["From Max"].default_value = b
    m.inputs["To Min"].default_value = c
    m.inputs["To Max"].default_value = d
    return m.outputs["Result"]


def _out(nb, shader, displacement=None):
    o = nb.n("ShaderNodeOutputMaterial")
    nb.link(shader, o.inputs["Surface"])
    if displacement is not None:
        nb.link(displacement, o.inputs["Displacement"])
    return o


def _bump(nb, height, strength, distance=0.1, normal=None):
    b = nb.n("ShaderNodeBump")
    b.inputs["Strength"].default_value = strength
    b.inputs["Distance"].default_value = distance
    nb.link(height, b.inputs["Height"])
    if normal is not None:
        nb.link(normal, b.inputs["Normal"])
    return b.outputs["Normal"]


def _principled(nb, color, rough=0.8, normal=None, sss=0.0, spec=0.5):
    p = nb.n("ShaderNodeBsdfPrincipled")
    if isinstance(color, (tuple, list)):
        p.inputs["Base Color"].default_value = _lin(color)
    else:
        nb.link(color, p.inputs["Base Color"])
    if isinstance(rough, (int, float)):
        p.inputs["Roughness"].default_value = rough
    else:
        nb.link(rough, p.inputs["Roughness"])
    p.inputs["Specular IOR Level"].default_value = spec
    if normal is not None:
        nb.link(normal, p.inputs["Normal"])
    return p


# --------------------------------------------------------------------------
def terrain(pal):
    m = _new("RW_Terrain")
    nb = NB(m)
    tc = nb.n("ShaderNodeTexCoord")
    pos = tc.outputs["Object"]
    grass_a, rock_a, snow_a, wet_a, dry_a = (_attr(nb, k) for k in ("grass", "rock", "snow", "wet", "dry"))

    large = _noise(nb, 0.004, 4, 0.5, pos)
    mid = _noise(nb, 0.05, 8, 0.6, pos)
    fine = _noise(nb, 0.9, 10, 0.6, pos)

    # grass: lush <-> dry, broken up with patchy noise
    dry_f = _math(nb, "ADD", dry_a, _math(nb, "MULTIPLY", _math(nb, "SUBTRACT", large.outputs["Fac"], 0.5), 0.6))
    dry_f = _math(nb, "MULTIPLY", dry_f, 1.0, clamp=True)
    grass_col = _mix(nb, dry_f, pal["grass"], pal["grass_dry"])
    grass_col = _mix(nb, _maprange(nb, mid.outputs["Fac"], 0.35, 0.7), grass_col, pal["soil"], "MIX")

    # rock: large tonal patches, subtle distorted strata, dark cracks
    wave = nb.n("ShaderNodeTexWave", wave_type="BANDS", bands_direction="Z")
    wave.inputs["Scale"].default_value = 0.035
    wave.inputs["Distortion"].default_value = 14.0
    wave.inputs["Detail"].default_value = 6.0
    wave.inputs["Detail Roughness"].default_value = 0.6
    nb.link(pos, wave.inputs["Vector"])
    strata = _ramp(nb, [(0.0, pal["rock_dark"]), (0.35, pal["rock"]),
                        (0.7, [c * 1.15 for c in pal["rock"]]), (1.0, [c * 0.8 for c in pal["rock"]])])
    nb.link(wave.outputs["Fac"], strata.inputs[0])
    tone = _mix(nb, _maprange(nb, large.outputs["Fac"], 0.3, 0.7), pal["rock"], [c * 0.6 for c in pal["rock"]])
    rock_col = _mix(nb, 0.3, tone, strata.outputs[0])
    # fractures: voronoi edges on noise-warped, vertically stretched coords
    warp = _noise(nb, 0.04, 4, 0.5, pos)
    wv = nb.n("ShaderNodeVectorMath", operation="MULTIPLY_ADD")
    nb.link(warp.outputs["Color"], wv.inputs[0])
    wv.inputs[1].default_value = (14.0, 14.0, 14.0)
    nb.link(pos, wv.inputs[2])
    stretch = nb.n("ShaderNodeVectorMath", operation="MULTIPLY")
    nb.link(wv.outputs[0], stretch.inputs[0])
    stretch.inputs[1].default_value = (1.0, 1.0, 0.45)
    vor = nb.n("ShaderNodeTexVoronoi", feature="DISTANCE_TO_EDGE")
    vor.inputs["Scale"].default_value = 0.09
    vor.inputs["Randomness"].default_value = 1.0
    nb.link(stretch.outputs[0], vor.inputs["Vector"])
    crack_mask = _maprange(nb, _noise(nb, 0.02, 3, 0.5, pos).outputs["Fac"], 0.4, 0.6)
    cracks = _math(nb, "MULTIPLY", _maprange(nb, vor.outputs["Distance"], 0.0, 0.035, 1.0, 0.0), crack_mask)
    rock_col = _mix(nb, _math(nb, "MULTIPLY", cracks, 0.45), rock_col, pal["rock_dark"])
    rock_col = _mix(nb, _maprange(nb, fine.outputs["Fac"], 0.3, 0.75, 0.0, 0.5), rock_col, [c * 1.4 for c in pal["rock"]])

    # snow, with a little blue in the shadows comes from the sky light
    snow_col = pal["snow"]

    rock_fac = _math(nb, "ADD", rock_a, _math(nb, "MULTIPLY", _math(nb, "SUBTRACT", mid.outputs["Fac"], 0.5), 0.8))
    rock_fac = _maprange(nb, rock_fac, 0.35, 0.6)
    col = _mix(nb, rock_fac, grass_col, rock_col)
    snow_fac = _maprange(nb, _math(nb, "ADD", snow_a, _math(nb, "MULTIPLY", _math(nb, "SUBTRACT", fine.outputs["Fac"], 0.5), 0.6)), 0.4, 0.55)
    col = _mix(nb, snow_fac, col, snow_col)
    wet_col = _mix(nb, wet_a, col, [c * 0.45 for c in pal["soil"]])

    rough = _math(nb, "SUBTRACT", 0.92, _math(nb, "MULTIPLY", wet_a, 0.2))
    height = _math(nb, "ADD", _math(nb, "MULTIPLY", fine.outputs["Fac"], 1.0),
                   _math(nb, "MULTIPLY", mid.outputs["Fac"], 2.0))
    height = _math(nb, "SUBTRACT", height, _math(nb, "MULTIPLY", _math(nb, "MULTIPLY", cracks, rock_fac), 1.5))
    # large-scale relief on rock (couloirs, ledges) then fine grain on top
    macro = _noise(nb, 0.08, 6, 0.65, pos)
    macro_h = _math(nb, "MULTIPLY", _math(nb, "ADD", macro.outputs["Fac"],
                                          _math(nb, "MULTIPLY", cracks, -0.4)), rock_fac)
    normal = _bump(nb, macro_h, 0.8, 3.0)
    normal = _bump(nb, height, 0.35, 0.25, normal)
    p = _principled(nb, wet_col, rough, normal, spec=0.25)
    _out(nb, p.outputs[0])
    return m


def water(pal):
    m = _new("RW_Water")
    nb = NB(m)
    tc = nb.n("ShaderNodeTexCoord")
    mapping = nb.n("ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = (1.0, 3.0, 1.0)  # stretch ripples along the flow
    nb.link(tc.outputs["Object"], mapping.inputs["Vector"])
    ripple = _noise(nb, 0.6, 6, 0.55, mapping.outputs[0])
    normal = _bump(nb, ripple.outputs["Fac"], 0.12, 0.05)
    p = nb.n("ShaderNodeBsdfPrincipled")
    p.inputs["Base Color"].default_value = _lin(pal["water"])
    p.inputs["Roughness"].default_value = 0.03
    p.inputs["IOR"].default_value = 1.333
    p.inputs["Transmission Weight"].default_value = 1.0
    nb.link(normal, p.inputs["Normal"])
    vol = nb.n("ShaderNodeVolumeAbsorption")
    vol.inputs["Color"].default_value = _lin([0.35, 0.55, 0.5])
    vol.inputs["Density"].default_value = 0.6
    o = _out(nb, p.outputs[0])
    nb.link(vol.outputs[0], o.inputs["Volume"])
    return m


def foliage(name, base, alt, translucency=0.35, variation=0.6):
    """Leaves / needles / grass. Per-instance random colour variation via
    Object Info > Random, plus translucency so backlit foliage glows."""
    m = _new(name)
    nb = NB(m)
    oi = nb.n("ShaderNodeObjectInfo")
    rnd = _maprange(nb, oi.outputs["Random"], 0.0, 1.0, 0.0, variation)
    col = _mix(nb, rnd, base, alt)
    hsv = nb.n("ShaderNodeHueSaturation")
    nb.link(col, hsv.inputs["Color"])
    nb.link(_maprange(nb, oi.outputs["Random"], 0.0, 1.0, 0.85, 1.15), hsv.inputs["Value"])
    tc = nb.n("ShaderNodeTexCoord")
    n = _noise(nb, 25.0, 4, 0.6, tc.outputs["Object"])
    normal = _bump(nb, n.outputs["Fac"], 0.3, 0.02)
    p = _principled(nb, hsv.outputs[0], 0.62, normal, spec=0.4)
    tr = nb.n("ShaderNodeBsdfTranslucent")
    nb.link(hsv.outputs[0], tr.inputs["Color"])
    nb.link(normal, tr.inputs["Normal"])
    mix = nb.n("ShaderNodeMixShader")
    mix.inputs[0].default_value = translucency
    nb.link(p.outputs[0], mix.inputs[1])
    nb.link(tr.outputs[0], mix.inputs[2])
    _out(nb, mix.outputs[0])
    return m


def grass_blades(pal):
    """Grass: darker at the root, lighter / drier at the tip (UV.y runs
    root->tip on the blade meshes)."""
    m = _new("RW_Grass")
    nb = NB(m)
    tc = nb.n("ShaderNodeTexCoord")
    sep = nb.n("ShaderNodeSeparateXYZ")
    nb.link(tc.outputs["UV"], sep.inputs[0])
    oi = nb.n("ShaderNodeObjectInfo")
    # matches the terrain's large-scale dry patches through instance position
    pos_noise = _noise(nb, 0.004, 4, 0.5, oi.outputs["Location"])
    dry = _maprange(nb, pos_noise.outputs["Fac"], 0.35, 0.65, 0.0, 0.9)
    dry = _math(nb, "ADD", dry, _maprange(nb, oi.outputs["Random"], 0, 1, -0.15, 0.15))
    base = _mix(nb, dry, pal["grass"], pal["grass_dry"])
    root = _mix(nb, 1.0, base, [0.3, 0.3, 0.3], "MULTIPLY")
    tip = _mix(nb, 0.35, base, [1.0, 0.95, 0.6], "MULTIPLY")
    ramp_f = _maprange(nb, sep.outputs["Y"], 0.0, 1.0)
    root_to_tip = _mix(nb, ramp_f, root, base)
    col = _mix(nb, _maprange(nb, sep.outputs["Y"], 0.7, 1.0), root_to_tip, tip)
    p = _principled(nb, col, 0.55, spec=0.45)
    tr = nb.n("ShaderNodeBsdfTranslucent")
    nb.link(col, tr.inputs["Color"])
    mix = nb.n("ShaderNodeMixShader")
    mix.inputs[0].default_value = 0.4
    nb.link(p.outputs[0], mix.inputs[1])
    nb.link(tr.outputs[0], mix.inputs[2])
    _out(nb, mix.outputs[0])
    return m


def bark(pal):
    m = _new("RW_Bark")
    nb = NB(m)
    tc = nb.n("ShaderNodeTexCoord")
    mapping = nb.n("ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = (6.0, 6.0, 0.6)
    nb.link(tc.outputs["Object"], mapping.inputs["Vector"])
    n = _noise(nb, 4.0, 8, 0.65, mapping.outputs[0])
    col = _mix(nb, n.outputs["Fac"], pal["bark"], [c * 1.8 for c in pal["bark"]])
    normal = _bump(nb, n.outputs["Fac"], 0.8, 0.05)
    p = _principled(nb, col, 0.9, normal, spec=0.3)
    _out(nb, p.outputs[0])
    return m


def rock(pal):
    m = _new("RW_Rock")
    nb = NB(m)
    tc = nb.n("ShaderNodeTexCoord")
    geo = nb.n("ShaderNodeNewGeometry")
    oi = nb.n("ShaderNodeObjectInfo")
    vec = nb.n("ShaderNodeVectorMath", operation="ADD")
    nb.link(tc.outputs["Object"], vec.inputs[0])
    nb.link(oi.outputs["Random"], vec.inputs[1])
    n_big = _noise(nb, 1.2, 6, 0.6, vec.outputs[0])
    n_fine = _noise(nb, 9.0, 10, 0.65, vec.outputs[0])
    base = _mix(nb, n_big.outputs["Fac"], pal["rock_dark"], pal["rock"])
    base = _mix(nb, _maprange(nb, n_fine.outputs["Fac"], 0.4, 0.7), base, [c * 1.3 for c in pal["rock"]])
    # cavities darker
    cav = _maprange(nb, geo.outputs["Pointiness"], 0.45, 0.55, 0.0, 1.0)
    base = _mix(nb, cav, [c * 0.5 for c in pal["rock_dark"]], base)
    # moss on upward faces
    sepn = nb.n("ShaderNodeSeparateXYZ")
    nb.link(geo.outputs["Normal"], sepn.inputs[0])
    moss_f = _maprange(nb, _math(nb, "ADD", sepn.outputs["Z"], _math(nb, "MULTIPLY", n_big.outputs["Fac"], 0.5)), 0.85, 1.1)
    col = _mix(nb, moss_f, base, pal["moss"])
    height = _math(nb, "ADD", n_fine.outputs["Fac"], _math(nb, "MULTIPLY", n_big.outputs["Fac"], 0.5))
    normal = _bump(nb, height, 0.6, 0.1)
    p = _principled(nb, col, 0.82, normal, spec=0.35)
    _out(nb, p.outputs[0])
    return m


def flowers(pal):
    m = _new("RW_Flowers")
    nb = NB(m)
    oi = nb.n("ShaderNodeObjectInfo")
    cols = pal["flowers"]
    r = _ramp(nb, [(0.0, cols[0]), (0.5, cols[min(1, len(cols) - 1)]), (1.0, cols[-1])])
    r.color_ramp.interpolation = "CONSTANT"
    nb.link(oi.outputs["Random"], r.inputs[0])
    p = _principled(nb, r.outputs[0], 0.5, sss=0.0)
    _out(nb, p.outputs[0])
    return m


def clouds(cfg):
    """Cirrus/altocumulus layer on a single high plane: cheap compared to
    volumetrics, catches the low sun through translucency."""
    lt = cfg["lighting"]
    m = _new("RW_Clouds")
    nb = NB(m)
    tc = nb.n("ShaderNodeTexCoord")
    mapping = nb.n("ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = (1.0, 2.6, 1.0)
    nb.link(tc.outputs["Object"], mapping.inputs["Vector"])
    warp = _noise(nb, 0.00018, 3, 0.5, mapping.outputs[0])
    n = nb.n("ShaderNodeTexNoise", noise_dimensions="3D")
    n.inputs["Scale"].default_value = 0.0004
    n.inputs["Detail"].default_value = 12.0
    n.inputs["Roughness"].default_value = 0.62
    n.inputs["Distortion"].default_value = 1.2
    nb.link(mapping.outputs[0], n.inputs["Vector"])
    cov = lt["cloud_coverage"]
    dens = _maprange(nb, _math(nb, "ADD", n.outputs["Fac"], _math(nb, "MULTIPLY", warp.outputs["Fac"], 0.25)),
                     0.68 - cov * 0.3, 0.80 - cov * 0.3)
    dens = _math(nb, "POWER", dens, 1.6, clamp=True)
    tr = nb.n("ShaderNodeBsdfTranslucent")
    tr.inputs["Color"].default_value = _lin(lt["cloud_color"])
    em = nb.n("ShaderNodeEmission")
    em.inputs["Color"].default_value = _lin(lt["cloud_color"])
    em.inputs["Strength"].default_value = 0.35 * lt["sky_strength"]
    add = nb.n("ShaderNodeAddShader")
    nb.link(tr.outputs[0], add.inputs[0])
    nb.link(em.outputs[0], add.inputs[1])
    transp = nb.n("ShaderNodeBsdfTransparent")
    mix = nb.n("ShaderNodeMixShader")
    nb.link(dens, mix.inputs[0])
    nb.link(transp.outputs[0], mix.inputs[1])
    nb.link(add.outputs[0], mix.inputs[2])
    _out(nb, mix.outputs[0])
    return m


def build_all(cfg):
    pal = cfg["palette"]
    return {
        "terrain": terrain(pal),
        "water": water(pal),
        "needles": foliage("RW_Needles", pal["needles"], [c * 1.5 for c in pal["needles"]], 0.25),
        "leaves": foliage("RW_Leaves", pal["leaves"], pal["leaves_alt"], 0.4, 0.45),
        "bush": foliage("RW_Bush", [c * 0.85 for c in pal["leaves"]], pal["needles"], 0.3, 0.7),
        "grass": grass_blades(pal),
        "bark": bark(pal),
        "rock": rock(pal),
        "flowers": flowers(pal),
        "clouds": clouds(cfg),
    }
