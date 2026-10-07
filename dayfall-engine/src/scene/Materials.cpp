#include "scene/Materials.h"
#include "core/Error.h"
#include "scene/GltfLoader.h"
#include <cstring>

namespace df {
using json = nlohmann::json;
namespace {
vec3 v3(const json& j, vec3 def) { return j.is_array() && j.size() >= 3 ? vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>()) : def; }
vec4 v4(const json& j, vec4 def) {
    if (!j.is_array()) return def;
    if (j.size() == 3) return vec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), def.w);
    return j.size() >= 4 ? vec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()) : def;
}
vec4 col(const json& j, const char* k, vec4 def) { return j.contains(k) ? v4(j[k], def) : def; }
float f(const json& j, const char* k, float def) { return j.contains(k) && j[k].is_number() ? j[k].get<float>() : def; }
bool b(const json& j, const char* k, bool def) { return j.contains(k) && j[k].is_boolean() ? j[k].get<bool>() : def; }

uint32_t modelFromName(const std::string& m) {
    if (m == "lit") return ModelLit;
    if (m == "terrain") return ModelTerrain;
    if (m == "foliage") return ModelFoliage;
    if (m == "grass") return ModelGrass;
    if (m == "courses") return ModelCourses;
    if (m == "water") return ModelWater;
    if (m == "emissive") return ModelEmissive;
    if (m == "unlit") return ModelUnlit;
    if (m == "rock") return ModelRock;
    throw Error("unknown material model '" + m + "' (lit, terrain, rock, foliage, grass, courses, water, emissive, unlit)");
}
}  // namespace

MaterialDef parseMaterial(const std::string& name, const json& j, const TextureResolver& textures) {
    MaterialDef d;
    d.name = name;
    GpuMaterial& g = d.gpu;
    uint32_t model = modelFromName(j.value("model", "lit"));
    g.h0.x = model;
    uint32_t flags = 0;
    if (b(j, "two_sided", model == ModelFoliage || model == ModelGrass)) flags |= MatTwoSided;
    std::string alpha = j.value("alpha", "opaque");
    if (alpha == "mask") flags |= MatMasked;
    if (alpha == "blend") flags |= MatBlend;
    if (b(j, "vertex_color", false)) flags |= MatVertexColor;
    std::string nc = j.value("normal_convention", "opengl");
    if (nc == "directx") flags |= MatNormalDirectX;
    else if (nc != "opengl") throw Error("normal_convention must be opengl (glTF, Blender) or directx (Unreal)");
    if (j.contains("wind")) {
        flags |= MatWind;
        const json& w = j["wind"];
        g.p[1] = vec4(f(w, "strength", 0.05f), f(w, "height", 1.0f), f(w, "speed", 1.6f), 0.0f);
    }
    float cutoff = f(j, "alpha_cutoff", 0.5f);
    std::memcpy(&g.h1.z, &cutoff, 4);
    switch (model) {
        case ModelTerrain:
            g.c[0] = col(j, "grass", vec4(0.05f, 0.095f, 0.018f, 1));
            g.c[1] = col(j, "grass_dry", vec4(0.2f, 0.15f, 0.045f, 1));
            g.c[2] = col(j, "soil", vec4(0.085f, 0.06f, 0.038f, 1));
            g.c[3] = col(j, "rock", vec4(0.125f, 0.115f, 0.1f, 1));
            g.c[4] = col(j, "rock_dark", vec4(0.035f, 0.032f, 0.029f, 1));
            g.c[5] = col(j, "moss", vec4(0.055f, 0.085f, 0.02f, 1));
            g.c[6] = col(j, "snow", vec4(0.8f, 0.82f, 0.86f, 1));
            g.p[0] = vec4(f(j, "water_level", -1000.0f), 0, 0, 0);
            g.p[1].x = f(j, "wildflowers", 0.0f);
            break;
        case ModelFoliage:
            g.c[0] = col(j, "color_a", vec4(0.05f, 0.1f, 0.02f, 1));
            g.c[1] = col(j, "color_b", vec4(0.12f, 0.12f, 0.03f, 1));
            g.c[2] = col(j, "tip", vec4(0.32f, 0.36f, 0.06f, 1));
            g.p[0] = vec4(f(j, "roughness", 0.62f), f(j, "translucency", 0.35f), f(j, "variation", 0.6f), 0);
            break;
        case ModelGrass:
            g.c[0] = col(j, "color_a", vec4(0.05f, 0.095f, 0.018f, 1));
            g.c[1] = col(j, "color_b", vec4(0.2f, 0.15f, 0.045f, 1));
            break;
        case ModelCourses:
            g.c[0] = col(j, "color_a", vec4(0.25f, 0.215f, 0.17f, 1));
            g.c[1] = col(j, "color_b", vec4(0.155f, 0.13f, 0.094f, 1));
            g.c[2] = col(j, "mortar_color", vec4(0.14f, 0.12f, 0.09f, 1));
            g.c[3] = col(j, "grime_color", vec4(0.055f, 0.085f, 0.02f, 1));
            g.p[0] = vec4(f(j, "block_width", 0.95f), f(j, "block_height", 0.44f), f(j, "mortar_width", 0.014f), f(j, "row_offset", 0.5f));
            g.p[1] = vec4(f(j, "grime", 0.5f), f(j, "roughness", 0.86f), f(j, "normal_strength", 0.03f), f(j, "specular", 0.3f));
            break;
        case ModelRock:
            g.c[0] = col(j, "color", vec4(0.14f, 0.13f, 0.115f, 1));
            g.c[1] = col(j, "color_dark", vec4(0.05f, 0.046f, 0.04f, 1));
            g.c[2] = col(j, "moss", vec4(0.055f, 0.085f, 0.02f, 1));
            g.c[3] = col(j, "lichen", vec4(0.32f, 0.3f, 0.2f, 1));
            g.p[0] = vec4(f(j, "moss_amount", 0.6f), f(j, "lichen_amount", 0.5f), f(j, "bump", 0.05f), 0);
            break;
        case ModelWater:
            g.c[0] = col(j, "color", vec4(0.55f, 0.6f, 0.55f, 1));
            g.c[1] = col(j, "duckweed_color", vec4(0.08f, 0.17f, 0.025f, 1));
            g.p[0] = vec4(f(j, "absorption", 1.2f), f(j, "roughness", 0.04f), f(j, "ripple_strength", 1.0f), 0);
            if (b(j, "duckweed", false)) flags |= MatDuckweed;
            break;
        default:   // lit, emissive, unlit
            g.c[0] = col(j, "base_color", vec4(0.6f, 0.6f, 0.6f, 1));
            g.c[1] = vec4(v3(j.value("emissive", json()), vec3(model == ModelEmissive ? 1.0f : 0.0f)),
                          f(j, "emissive_strength", model == ModelEmissive ? 1.0f : 0.0f));
            g.p[0] = vec4(f(j, "roughness", 0.8f), f(j, "metallic", 0.0f), f(j, "specular", 0.5f), f(j, "occlusion_strength", 1.0f));
            g.p[1].w = f(j, "normal_scale", 1.0f);
            break;
    }
    if (j.contains("textures") && textures) {
        const json& t = j["textures"];
        if (t.contains("base")) g.h0.z = textures(t["base"].get<std::string>(), true);
        if (t.contains("normal")) g.h0.w = textures(t["normal"].get<std::string>(), false);
        if (t.contains("orm")) g.h1.x = textures(t["orm"].get<std::string>(), false);
        if (t.contains("emissive")) g.h1.y = textures(t["emissive"].get<std::string>(), true);
    }
    g.h0.y = flags;
    d.group = groupOf(g);
    return d;
}

const json& builtinMaterials() {
    // Colours are linear albedo. Tuned against the reference-world Blender looks.
    static const json lib = json::parse(R"({
      "default":      {"model": "lit", "base_color": [0.5, 0.5, 0.5], "roughness": 0.8},
      "blockout":     {"model": "lit", "base_color": [0.42, 0.42, 0.44], "roughness": 0.7},
      "blockout_dark":{"model": "lit", "base_color": [0.16, 0.16, 0.18], "roughness": 0.7},
      "terrain":      {"model": "terrain", "wildflowers": 0.5},
      "stone":        {"model": "courses", "color_a": [0.25, 0.215, 0.17], "color_b": [0.155, 0.13, 0.094],
                       "block_width": 0.95, "block_height": 0.44, "mortar_width": 0.014, "grime": 0.5},
      "cobble":       {"model": "courses", "color_a": [0.2, 0.19, 0.17], "color_b": [0.12, 0.11, 0.1],
                       "block_width": 0.32, "block_height": 0.24, "mortar_width": 0.02, "grime": 0.3, "normal_strength": 0.05},
      "brick":        {"model": "courses", "color_a": [0.32, 0.12, 0.07], "color_b": [0.2, 0.07, 0.04],
                       "mortar_color": [0.35, 0.33, 0.3], "block_width": 0.23, "block_height": 0.075, "mortar_width": 0.01, "grime": 0.3},
      "roof_tiles":   {"model": "courses", "color_a": [0.24, 0.085, 0.045], "color_b": [0.15, 0.05, 0.03],
                       "mortar_color": [0.05, 0.03, 0.02], "block_width": 0.3, "block_height": 0.22, "mortar_width": 0.02,
                       "grime": 0.2, "roughness": 0.7, "normal_strength": 0.06},
      "roof_slate":   {"model": "courses", "color_a": [0.07, 0.075, 0.085], "color_b": [0.04, 0.045, 0.05],
                       "mortar_color": [0.02, 0.02, 0.02], "block_width": 0.3, "block_height": 0.2, "mortar_width": 0.015,
                       "grime": 0.2, "roughness": 0.55, "normal_strength": 0.05},
      "planks":       {"model": "courses", "color_a": [0.2, 0.12, 0.06], "color_b": [0.12, 0.07, 0.035],
                       "mortar_color": [0.04, 0.025, 0.015], "block_width": 2.4, "block_height": 0.2, "mortar_width": 0.008,
                       "row_offset": 0.37, "grime": 0.4, "roughness": 0.75, "normal_strength": 0.02},
      "plaster":      {"model": "lit", "base_color": [0.55, 0.5, 0.42], "roughness": 0.9},
      "wood":         {"model": "lit", "base_color": [0.16, 0.095, 0.05], "roughness": 0.75},
      "wood_dark":    {"model": "lit", "base_color": [0.07, 0.045, 0.028], "roughness": 0.7},
      "bark":         {"model": "lit", "base_color": [0.09, 0.07, 0.055], "roughness": 0.9},
      "iron":         {"model": "lit", "base_color": [0.08, 0.08, 0.085], "roughness": 0.55, "metallic": 1.0},
      "gold":         {"model": "lit", "base_color": [1.0, 0.71, 0.29], "roughness": 0.3, "metallic": 1.0},
      "cloth_red":    {"model": "lit", "base_color": [0.35, 0.04, 0.03], "roughness": 0.95},
      "cloth_blue":   {"model": "lit", "base_color": [0.04, 0.08, 0.25], "roughness": 0.95},
      "hay":          {"model": "lit", "base_color": [0.45, 0.35, 0.12], "roughness": 0.95},
      "dirt":         {"model": "lit", "base_color": [0.12, 0.085, 0.055], "roughness": 0.95},
      "rock":         {"model": "rock", "color": [0.16, 0.15, 0.13], "color_dark": [0.06, 0.055, 0.05], "moss_amount": 0.5},
      "leaves":       {"model": "foliage", "color_a": [0.045, 0.09, 0.018], "color_b": [0.11, 0.12, 0.03], "tip": [0.3, 0.34, 0.06],
                       "wind": {"strength": 0.08, "height": 8.0, "speed": 1.4}},
      "needles":      {"model": "foliage", "color_a": [0.025, 0.055, 0.02], "color_b": [0.05, 0.07, 0.025], "tip": [0.12, 0.17, 0.04],
                       "translucency": 0.2, "wind": {"strength": 0.05, "height": 12.0, "speed": 1.1}},
      "grass_blades": {"model": "grass", "color_a": [0.05, 0.095, 0.018], "color_b": [0.2, 0.15, 0.045],
                       "wind": {"strength": 0.12, "height": 0.6, "speed": 2.2}},
      "water":        {"model": "water", "color": [0.55, 0.62, 0.55], "absorption": 1.0, "roughness": 0.03},
      "glow":         {"model": "emissive", "base_color": [1.0, 0.8, 0.35], "emissive": [1.0, 0.75, 0.3], "emissive_strength": 30.0},
      "glow_blue":    {"model": "emissive", "base_color": [0.4, 0.7, 1.0], "emissive": [0.35, 0.65, 1.0], "emissive_strength": 30.0},
      "window_lit":   {"model": "emissive", "base_color": [1.0, 0.7, 0.35], "emissive": [1.0, 0.62, 0.28], "emissive_strength": 6.0},
      "window_dark":  {"model": "lit", "base_color": [0.02, 0.025, 0.03], "roughness": 0.15, "specular": 0.6},
      "mannequin":    {"model": "lit", "base_color": [0.62, 0.6, 0.56], "roughness": 0.45},
      "mannequin_joint": {"model": "lit", "base_color": [0.12, 0.12, 0.13], "roughness": 0.4}
    })");
    return lib;
}
}  // namespace df
