"""Fortress materials for DAYFALL from the Unreal project's own material tables.

The FloatingIslet fortress materials (MI_FT_*) are instances of one master, M_FT_Master, built by
Content/Python/import_fortress.py from its tables (LOOKS, DETAIL, WEATHER, EMISSIVE_DETAIL, PAINT) with colour
overrides from tune_fortress_look.py (CFG["MI"]). The engine's `triplanar` lit material is that master ported, so
each MI becomes one engine material. The scripts are read as text (ast), never run: they import `unreal`.

    python tools/unreal/fortress_materials.py --scripts "<FloatingIslet>/Content/Python" --out fortress.json
    python tools/unreal/fortress_materials.py --scripts ... --merge content/library.json

Textures: DETAIL entries "FT:<Name>" use <textures>/T_FT_<Name>_A.jpg and _N.png (made by the project's
make_fort_textures.py; tools/unreal/README in docs/PORTING.md). Other detail textures (the third-party Stone_1 set)
are left out: those materials keep the master's colour, noise and painterly layer without a texture.
Not ported: the distance-field grime (no distance fields in the engine) and the kit's UV0 wind.
"""
import argparse
import ast
import json
import os
import sys

# set in import_fortress.py code rather than a table: the V34 crags' vertex colours are masks, not tints
IGNORE_VC = {"FT_CragRock": 1.0}
DEFAULT_PAINT = {"contact": 0.55, "top": 0.16, "warm": 0.6, "strokes": 0.07}


def _assignments(source):
    """Top-level NAME = <expr> nodes of a module."""
    out = {}
    for node in ast.parse(source).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            out[node.targets[0].id] = node.value
    return out


def _literal(node, name):
    try:
        return ast.literal_eval(node)
    except ValueError as e:
        raise ValueError("%s is not a plain literal: %s" % (name, e))


def _paint(node):
    # PAINT = json.loads(os.environ.get("FORT_PAINT", '{...}')): the default is the second argument
    if node is None:
        return dict(DEFAULT_PAINT)
    for sub in ast.walk(node):
        if isinstance(sub, ast.Call) and len(sub.args) == 2 and isinstance(sub.args[1], ast.Constant):
            return json.loads(sub.args[1].value)
    return _literal(node, "PAINT")


def _dict_key(node, key, name):
    if not isinstance(node, ast.Dict):
        raise ValueError("%s is not a dict literal" % name)
    for k, v in zip(node.keys, node.values):
        if isinstance(k, ast.Constant) and k.value == key:
            return _literal(v, "%s[%r]" % (name, key))
    return {}


def read_tables(fortress_src, tune_src=None):
    a = _assignments(fortress_src)
    for need in ("LOOKS", "DETAIL"):
        if need not in a:
            raise ValueError("import_fortress.py has no %s table" % need)
    t = {"looks": _literal(a["LOOKS"], "LOOKS"), "detail": _literal(a["DETAIL"], "DETAIL"),
         "weather": _literal(a["WEATHER"], "WEATHER") if "WEATHER" in a else {},
         "emissive_detail": _literal(a["EMISSIVE_DETAIL"], "EMISSIVE_DETAIL") if "EMISSIVE_DETAIL" in a else {},
         "paint": _paint(a.get("PAINT")), "mi": {}}
    if tune_src:
        b = _assignments(tune_src)
        if "CFG" in b:
            t["mi"] = _dict_key(b["CFG"], "MI", "CFG")
    return t


def material(name, look, detail=None, weather=(0.0, 0.0), emissive_detail=0.0, paint=None, base_override=None,
             tex_dir="textures/fortress", ignore_vc=0.0):
    """One MI_FT_* as a DAYFALL material. look = (base rgb, roughness, metallic, emissive rgb | None,
    emissive strength, noise); detail = (texture, metres per tile, albedo str, chroma mix, normal str, top only)."""
    paint = paint or DEFAULT_PAINT
    base, rough, metal, emissive, strength, noise = look
    tri = {"vertex_color_scale": 1.6, "noise": noise, "contact_dark": paint["contact"], "top_light": paint["top"],
           "warm": paint["warm"], "strokes": paint["strokes"]}
    m = {"model": "lit", "base_color": [round(c, 4) for c in (base_override or base)], "roughness": rough,
         "metallic": metal, "vertex_color": True, "triplanar": tri}
    if emissive:
        m["emissive"] = list(emissive)
        m["emissive_strength"] = strength
    if detail:
        tex, tile, stra, mix, strn, top = detail
        tri.update({"tile_m": tile, "albedo_strength": stra, "chroma_mix": mix, "normal_strength": strn,
                    "top_only": top})
        if isinstance(tex, str) and tex.startswith("FT:"):
            stem = "%s/T_FT_%s" % (tex_dir, tex[3:])
            m["textures"] = {"base": stem + "_A.jpg", "normal": stem + "_N.png"}
            m["normal_convention"] = "directx"
    if weather[0] or weather[1]:
        tri["streaks"], tri["moss"] = weather
    if emissive_detail:
        tri["emissive_detail"] = emissive_detail
    if ignore_vc:
        tri["ignore_vertex_color"] = ignore_vc
    return m


def fortress_materials(tables, tex_dir="textures/fortress"):
    out = {}
    for name, look in tables["looks"].items():
        out[name] = material(name, look, tables["detail"].get(name), tuple(tables["weather"].get(name, (0.0, 0.0))),
                             tables["emissive_detail"].get(name, 0.0), tables["paint"], tables["mi"].get(name),
                             tex_dir, IGNORE_VC.get(name, 0.0))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--scripts", required=True, help="the Unreal project's Content/Python folder")
    ap.add_argument("--textures", default="textures/fortress", help="texture folder, relative to the content dir")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--out", help="write the materials as a JSON object")
    g.add_argument("--merge", help="add or replace them in this library.json / map.json 'materials'")
    a = ap.parse_args(argv)
    with open(os.path.join(a.scripts, "import_fortress.py"), encoding="utf-8") as f:
        fort = f.read()
    tune_path = os.path.join(a.scripts, "tune_fortress_look.py")
    tune = open(tune_path, encoding="utf-8").read() if os.path.exists(tune_path) else None
    mats = fortress_materials(read_tables(fort, tune), a.textures)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(mats, f, indent=1)
    else:
        with open(a.merge, encoding="utf-8") as f:
            doc = json.load(f)
        doc.setdefault("materials", {}).update(mats)
        with open(a.merge, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")
    print("%d fortress materials (%d textured)" % (len(mats), sum("textures" in m for m in mats.values())))


if __name__ == "__main__":
    sys.exit(main())
