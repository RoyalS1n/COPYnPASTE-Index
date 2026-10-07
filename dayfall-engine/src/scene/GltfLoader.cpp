#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "scene/GltfLoader.h"
#include "core/Log.h"
#include "core/Error.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace df {
namespace {
struct FileCtx {
    std::filesystem::path dir;
    cgltf_data* data;
    MeshAsset* out;
    std::unordered_map<const cgltf_image*, uint32_t> srgbTex, linTex;
    std::unordered_map<const cgltf_material*, std::string> mats;
    // skinned loads: 4 bone influences per vertex (JOINTS_n / WEIGHTS_n offset by boneBase, or all on rigidBone)
    CharacterAsset* skin = nullptr;
    uint32_t boneBase = 0;
    int rigidBone = -1;
};

uint32_t textureFromImage(FileCtx& ctx, const cgltf_texture_view& view, bool srgb) {
    if (!view.texture || !view.texture->image) return kNoTexture;
    const cgltf_image* img = view.texture->image;
    auto& cache = srgb ? ctx.srgbTex : ctx.linTex;
    if (auto it = cache.find(img); it != cache.end()) return it->second;
    int w = 0, h = 0, n = 0;
    stbi_uc* px = nullptr;
    if (img->buffer_view) {
        const uint8_t* p = (const uint8_t*)img->buffer_view->buffer->data + img->buffer_view->offset;
        px = stbi_load_from_memory(p, (int)img->buffer_view->size, &w, &h, &n, 4);
    } else if (img->uri && std::strncmp(img->uri, "data:", 5) != 0) {
        std::string uri = img->uri;
        cgltf_decode_uri(uri.data());
        uri.resize(std::strlen(uri.c_str()));
        px = stbi_load((ctx.dir / uri).string().c_str(), &w, &h, &n, 4);
    }
    if (!px) { logWarn("texture {} could not be decoded", img->name ? img->name : "?"); cache[img] = kNoTexture; return kNoTexture; }
    Texture t;
    t.width = (uint32_t)w; t.height = (uint32_t)h; t.srgb = srgb;
    t.name = img->name ? img->name : (img->uri ? img->uri : "embedded");
    t.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    uint32_t id = (uint32_t)ctx.out->textures.size();
    ctx.out->textures.push_back(std::move(t));
    cache[img] = id;
    return id;
}

std::string materialFor(FileCtx& ctx, const cgltf_material* m) {
    if (!m) return "default";
    if (auto it = ctx.mats.find(m); it != ctx.mats.end()) return it->second;
    std::string name = m->name ? m->name : std::format("material_{}", (size_t)(m - ctx.data->materials));
    MaterialDef d;
    d.name = name;
    GpuMaterial& g = d.gpu;
    g.h0.x = m->unlit ? ModelUnlit : ModelLit;
    g.c[0] = vec4(1.0f);
    g.p[0] = vec4(1.0f, 1.0f, 0.5f, 1.0f);
    g.p[1].w = 1.0f;
    if (m->has_pbr_metallic_roughness) {
        auto& pbr = m->pbr_metallic_roughness;
        g.c[0] = glm::make_vec4(pbr.base_color_factor);
        g.p[0].x = pbr.roughness_factor;
        g.p[0].y = pbr.metallic_factor;
        g.h0.z = textureFromImage(ctx, pbr.base_color_texture, true);
        g.h1.x = textureFromImage(ctx, pbr.metallic_roughness_texture, false);
        // occlusion is used only when packed into the same image (ORM)
        bool packed = m->occlusion_texture.texture && pbr.metallic_roughness_texture.texture &&
                      m->occlusion_texture.texture->image == pbr.metallic_roughness_texture.texture->image;
        g.p[0].w = packed ? m->occlusion_texture.scale : 0.0f;
    }
    if (m->normal_texture.texture) {
        g.h0.w = textureFromImage(ctx, m->normal_texture, false);
        g.p[1].w = m->normal_texture.scale;
    }
    float es = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
    g.c[1] = vec4(m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2], es);
    g.h1.y = textureFromImage(ctx, m->emissive_texture, true);
    if (m->double_sided) g.h0.y |= MatTwoSided;
    float cutoff = 0.5f;
    if (m->alpha_mode == cgltf_alpha_mode_mask) { g.h0.y |= MatMasked; cutoff = m->alpha_cutoff; }
    if (m->alpha_mode == cgltf_alpha_mode_blend) g.h0.y |= MatBlend;
    std::memcpy(&g.h1.z, &cutoff, 4);
    d.group = groupOf(g);
    ctx.out->materials.push_back(d);
    ctx.mats[m] = name;
    return name;
}

// Appends one primitive's geometry (transformed by `xf`, glTF space) to the asset.
void appendPrimitive(FileCtx& ctx, const cgltf_primitive& prim, const mat4& xf) {
    MeshAsset& out = *ctx.out;
    if (prim.type != cgltf_primitive_type_triangles) return;
    const cgltf_accessor *pos = nullptr, *nrm = nullptr, *tan = nullptr, *uv0 = nullptr, *uv1 = nullptr, *col0 = nullptr, *col1 = nullptr;
    for (size_t i = 0; i < prim.attributes_count; ++i) {
        const auto& a = prim.attributes[i];
        switch (a.type) {
            case cgltf_attribute_type_position: pos = a.data; break;
            case cgltf_attribute_type_normal: nrm = a.data; break;
            case cgltf_attribute_type_tangent: tan = a.data; break;
            case cgltf_attribute_type_texcoord: if (a.index == 0) uv0 = a.data; else if (a.index == 1) uv1 = a.data; break;
            case cgltf_attribute_type_color: if (a.index == 0) col0 = a.data; else if (a.index == 1) col1 = a.data; break;
            default: break;
        }
    }
    if (!pos) return;
    size_t n = pos->count;
    uint32_t first = (uint32_t)out.vertices.size();
    mat3 nxf = glm::transpose(glm::inverse(mat3(xf)));
    auto read = [&](const cgltf_accessor* acc, int comps) {
        std::vector<float> tmp(n * comps, 0.0f);
        if (acc) {
            int c = (int)cgltf_num_components(acc->type);
            std::vector<float> raw(n * c);
            cgltf_accessor_unpack_floats(acc, raw.data(), raw.size());
            for (size_t i = 0; i < n; ++i)
                for (int k = 0; k < std::min(c, comps); ++k) tmp[i * comps + k] = raw[i * c + k];
            if (comps == 4 && c == 3) for (size_t i = 0; i < n; ++i) tmp[i * 4 + 3] = 1.0f;
        }
        return tmp;
    };
    std::vector<float> P = read(pos, 3), N = read(nrm, 3), T = read(tan, 4), U0 = read(uv0, 2), U1 = read(uv1, 2);
    std::vector<float> C0 = read(col0, 4), C1 = read(col1, 4);
    out.vertices.resize(first + n);
    for (size_t i = 0; i < n; ++i) {
        vec3 p = gltfToWorld(vec3(xf * vec4(P[i * 3], P[i * 3 + 1], P[i * 3 + 2], 1.0f)));
        vec3 nn = nrm ? gltfToWorld(glm::normalize(nxf * vec3(N[i * 3], N[i * 3 + 1], N[i * 3 + 2]))) : vec3(0, 0, 1);
        vec4 t = tan ? vec4(gltfToWorld(glm::normalize(mat3(xf) * vec3(T[i * 4], T[i * 4 + 1], T[i * 4 + 2]))), T[i * 4 + 3]) : vec4(0);
        vec4 c0 = col0 ? vec4(C0[i * 4], C0[i * 4 + 1], C0[i * 4 + 2], C0[i * 4 + 3]) : vec4(1.0f);
        vec4 c1 = col1 ? vec4(C1[i * 4], C1[i * 4 + 1], C1[i * 4 + 2], C1[i * 4 + 3]) : vec4(0.0f);
        out.vertices[first + i] = makeVertex(p, nn, uv0 ? vec2(U0[i * 2], U0[i * 2 + 1]) : vec2(0), t, c0, c1,
                                             uv1 ? vec2(U1[i * 2], U1[i * 2 + 1]) : vec2(0));
    }
    MeshPart part;
    part.firstIndex = (uint32_t)out.indices.size();
    if (prim.indices) {
        for (size_t i = 0; i < prim.indices->count; ++i) out.indices.push_back(first + (uint32_t)cgltf_accessor_read_index(prim.indices, i));
    } else {
        for (size_t i = 0; i < n; ++i) out.indices.push_back(first + (uint32_t)i);
    }
    part.indexCount = (uint32_t)out.indices.size() - part.firstIndex;
    // a mirrored transform flips winding
    if (glm::determinant(mat3(xf)) < 0)
        for (uint32_t i = part.firstIndex; i + 2 < out.indices.size(); i += 3) std::swap(out.indices[i + 1], out.indices[i + 2]);
    // smooth normals when missing
    if (!nrm) {
        std::vector<vec3> acc(n, vec3(0));
        for (uint32_t i = part.firstIndex; i + 2 < out.indices.size(); i += 3) {
            uint32_t a = out.indices[i] - first, b = out.indices[i + 1] - first, c = out.indices[i + 2] - first;
            vec3 pa = out.vertices[first + a].p0, pb = out.vertices[first + b].p0, pc = out.vertices[first + c].p0;
            vec3 fn = glm::cross(pb - pa, pc - pa);
            acc[a] += fn; acc[b] += fn; acc[c] += fn;
        }
        for (size_t i = 0; i < n; ++i) {
            vec3 v = glm::length(acc[i]) > 0 ? glm::normalize(acc[i]) : vec3(0, 0, 1);
            out.vertices[first + i].p1 = vec4(v, out.vertices[first + i].p1.w);
        }
    }
    part.material = materialFor(ctx, prim.material);
    if (ctx.skin) {   // the 4 strongest of all JOINTS_n / WEIGHTS_n influences, renormalised
        std::vector<std::pair<const cgltf_accessor*, const cgltf_accessor*>> sets(4, {nullptr, nullptr});
        for (size_t i = 0; i < prim.attributes_count; ++i) {
            const auto& a = prim.attributes[i];
            if (a.index < 0 || a.index >= 4) continue;
            if (a.type == cgltf_attribute_type_joints) sets[a.index].first = a.data;
            if (a.type == cgltf_attribute_type_weights) sets[a.index].second = a.data;
        }
        for (size_t i = 0; i < n; ++i) {
            std::pair<float, uint32_t> inf[16]{};
            int count = 0;
            if (ctx.rigidBone < 0)
                for (auto& [ja, wa] : sets) {
                    if (!ja || !wa) continue;
                    cgltf_uint j[4]{};
                    float w[4]{};
                    cgltf_accessor_read_uint(ja, i, j, 4);
                    cgltf_accessor_read_float(wa, i, w, 4);
                    for (int k = 0; k < 4; ++k) if (w[k] > 0 && count < 16) inf[count++] = {w[k], ctx.boneBase + j[k]};
                }
            std::sort(inf, inf + count, [](auto& a, auto& b) { return a.first > b.first; });
            uvec4 jv(0);
            vec4 wv(0);
            float sum = 0;
            for (int k = 0; k < std::min(count, 4); ++k) { jv[k] = inf[k].second; wv[k] = inf[k].first; sum += inf[k].first; }
            if (sum > 1e-6f) wv /= sum;
            else { jv = uvec4((uint32_t)std::max(ctx.rigidBone, 0), 0, 0, 0); wv = vec4(1, 0, 0, 0); }
            ctx.skin->joints.push_back(jv);
            ctx.skin->weights.push_back(wv);
        }
    }
    // tangents for normal-mapped materials without them
    bool normalMapped = false;
    for (auto& m : out.materials) if (m.name == part.material && m.gpu.h0.w != kNoTexture) normalMapped = true;
    if (!tan && normalMapped) {
        std::vector<vec3> acc(n, vec3(0));
        for (uint32_t i = part.firstIndex; i + 2 < out.indices.size(); i += 3) {
            uint32_t ids[3] = {out.indices[i] - first, out.indices[i + 1] - first, out.indices[i + 2] - first};
            GpuVertex &a = out.vertices[first + ids[0]], &b = out.vertices[first + ids[1]], &c = out.vertices[first + ids[2]];
            vec3 e1 = vec3(b.p0) - vec3(a.p0), e2 = vec3(c.p0) - vec3(a.p0);
            vec2 d1 = vec2(b.p0.w, b.p1.w) - vec2(a.p0.w, a.p1.w), d2 = vec2(c.p0.w, c.p1.w) - vec2(a.p0.w, a.p1.w);
            float r = d1.x * d2.y - d2.x * d1.y;
            if (std::abs(r) < 1e-12f) continue;
            vec3 t = (e1 * d2.y - e2 * d1.y) / r;
            for (uint32_t k : ids) acc[k] += t;
        }
        for (size_t i = 0; i < n; ++i) {
            vec3 nn = out.vertices[first + i].p1;
            vec3 t = acc[i] - nn * glm::dot(nn, acc[i]);
            out.vertices[first + i].tangent = glm::length(t) > 1e-8f ? vec4(glm::normalize(t), 1.0f) : vec4(0);
        }
    }
    out.lods[0].push_back(part);
}

mat4 nodeMatrix(const cgltf_node* node) {
    float m[16];
    cgltf_node_transform_world(node, m);
    return glm::make_mat4(m);
}

cgltf_data* parse(const std::filesystem::path& file) {
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, file.string().c_str(), &data) != cgltf_result_success) throw Error(std::format("cannot parse {}", file.string()));
    if (cgltf_load_buffers(&opt, data, file.string().c_str()) != cgltf_result_success) {
        cgltf_free(data);
        throw Error(std::format("cannot load buffers of {}", file.string()));
    }
    return data;
}

void finish(MeshAsset& m) {
    m.lodDistances = {1e9f};
    m.computeBounds();
    // merge parts that share a material (fewer draw batches)
    std::vector<MeshPart> merged;
    std::vector<uint32_t> idx;
    std::vector<std::string> order;
    for (auto& p : m.lods[0]) if (std::find(order.begin(), order.end(), p.material) == order.end()) order.push_back(p.material);
    for (auto& mat : order) {
        MeshPart mp;
        mp.material = mat;
        mp.firstIndex = (uint32_t)idx.size();
        for (auto& p : m.lods[0])
            if (p.material == mat) idx.insert(idx.end(), m.indices.begin() + p.firstIndex, m.indices.begin() + p.firstIndex + p.indexCount);
        mp.indexCount = (uint32_t)idx.size() - mp.firstIndex;
        merged.push_back(mp);
    }
    m.indices = std::move(idx);
    m.lods[0] = std::move(merged);
}
}  // namespace

uint32_t groupOf(const GpuMaterial& g) {
    if (g.h0.x == ModelWater) return GroupWater;
    if (g.h0.y & MatBlend) return GroupBlend;
    if (g.h0.y & MatMasked) return GroupMasked;
    if (g.h0.y & MatTwoSided) return GroupTwoSided;
    return GroupOpaque;
}

MeshAsset loadGltfAsset(const std::filesystem::path& file, const std::string& name) {
    cgltf_data* data = parse(file);
    MeshAsset out;
    out.name = name;
    out.lods.resize(1);
    FileCtx ctx{file.parent_path(), data, &out};
    const cgltf_scene* sc = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    std::vector<const cgltf_node*> stack;
    if (sc) for (size_t i = 0; i < sc->nodes_count; ++i) stack.push_back(sc->nodes[i]);
    else for (size_t i = 0; i < data->nodes_count; ++i) if (!data->nodes[i].parent) stack.push_back(&data->nodes[i]);
    while (!stack.empty()) {
        const cgltf_node* node = stack.back();
        stack.pop_back();
        for (size_t i = 0; i < node->children_count; ++i) stack.push_back(node->children[i]);
        if (!node->mesh) continue;
        mat4 xf = nodeMatrix(node);
        for (size_t p = 0; p < node->mesh->primitives_count; ++p) appendPrimitive(ctx, node->mesh->primitives[p], xf);
    }
    cgltf_free(data);
    if (out.vertices.empty()) throw Error(std::format("{} has no triangle meshes", file.string()));
    finish(out);
    return out;
}

GltfScene loadGltfScene(const std::filesystem::path& file) {
    cgltf_data* data = parse(file);
    GltfScene gs;
    std::string prefix = file.stem().string() + "#";
    for (size_t m = 0; m < data->meshes_count; ++m) {
        MeshAsset a;
        a.name = prefix + (data->meshes[m].name ? data->meshes[m].name : std::to_string(m));
        a.lods.resize(1);
        FileCtx ctx{file.parent_path(), data, &a};
        for (size_t p = 0; p < data->meshes[m].primitives_count; ++p) appendPrimitive(ctx, data->meshes[m].primitives[p], mat4(1.0f));
        if (!a.vertices.empty()) finish(a);
        gs.meshes.push_back(std::move(a));
    }
    auto place = [&](uint32_t mesh, const mat4& gm) {
        vec3 t, sc, skew;
        vec4 persp;
        quat r;
        glm::decompose(gm, sc, r, t, skew, persp);
        GltfScene::Placement p;
        p.mesh = mesh;
        p.position = gltfToWorld(t);
        p.rotation = gltfToWorld(r);
        p.scale = vec3(sc.x, sc.z, sc.y);   // glTF axes (x, y up, z) -> world (x, y, z up); scale has no sign flip
        gs.placements.push_back(p);
    };
    for (size_t i = 0; i < data->nodes_count; ++i) {
        const cgltf_node* node = &data->nodes[i];
        mat4 gm = nodeMatrix(node);
        if (node->mesh) {
            uint32_t mesh = (uint32_t)(node->mesh - data->meshes);
            if (gs.meshes[mesh].vertices.empty()) continue;
            if (node->has_mesh_gpu_instancing) {
                const auto& gi = node->mesh_gpu_instancing;
                const cgltf_accessor *ta = nullptr, *ra = nullptr, *sa = nullptr;
                for (size_t a = 0; a < gi.attributes_count; ++a) {
                    std::string nm = gi.attributes[a].name;
                    if (nm == "TRANSLATION") ta = gi.attributes[a].data;
                    else if (nm == "ROTATION") ra = gi.attributes[a].data;
                    else if (nm == "SCALE") sa = gi.attributes[a].data;
                }
                size_t count = ta ? ta->count : ra ? ra->count : sa ? sa->count : 0;
                for (size_t k = 0; k < count; ++k) {
                    float tv[3] = {0, 0, 0}, rv[4] = {0, 0, 0, 1}, sv[3] = {1, 1, 1};
                    if (ta) cgltf_accessor_read_float(ta, k, tv, 3);
                    if (ra) cgltf_accessor_read_float(ra, k, rv, 4);
                    if (sa) cgltf_accessor_read_float(sa, k, sv, 3);
                    mat4 local = glm::translate(mat4(1), glm::make_vec3(tv)) * glm::mat4_cast(quat(rv[3], rv[0], rv[1], rv[2])) *
                                 glm::scale(mat4(1), glm::make_vec3(sv));
                    place(mesh, gm * local);
                }
            } else {
                place(mesh, gm);
            }
        }
        if (node->light && node->light->type == cgltf_light_type_point) {
            LightDef l;
            l.position = gltfToWorld(vec3(gm[3]));
            l.color = glm::make_vec3(node->light->color);
            l.intensity = node->light->intensity / (4.0f * 3.14159265f) * 0.01f;   // candela-ish -> engine units
            l.range = node->light->range > 0 ? node->light->range : 15.0f;
            gs.lights.push_back(l);
        }
    }
    logInfo("{}: {} meshes, {} placements, {} lights", file.filename().string(), gs.meshes.size(), gs.placements.size(), gs.lights.size());
    cgltf_free(data);
    return gs;
}

// ------------------------------------------------------------------------------------------ rigged characters
namespace {
std::string nodeName(const cgltf_data* data, const cgltf_node* n) {
    return n->name ? n->name : std::format("node_{}", (size_t)(n - data->nodes));
}

// Every animation of a file whose channels target skeleton nodes. nodeOf maps a
// file node to a skeleton node (-1: not in the skeleton). retarget (clips from
// another file): translation is kept only on the root-motion node, the rest
// of the skeleton keeps the character's own bone lengths.
void readClips(cgltf_data* data, CharacterAsset& c, const std::function<int(const cgltf_node*)>& nodeOf,
               const std::filesystem::path& file, bool retarget) {
    for (size_t ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& a = data->animations[ai];
        AnimClip clip;
        clip.source = file.filename().string();
        clip.name = a.name ? a.name : "";
        if (retarget && (clip.name.empty() || clip.name.find("mixamo.com") != std::string::npos))   // Mixamo: one clip per file
            clip.name = file.stem().string() + (data->animations_count > 1 ? std::format("_{}", ai + 1) : "");
        if (clip.name.empty()) clip.name = std::format("clip_{}", ai + 1);
        std::string base = clip.name;
        for (int k = 2; std::any_of(c.clips.begin(), c.clips.end(), [&](const AnimClip& x) { return x.name == clip.name; }); ++k)
            clip.name = std::format("{}_{}", base, k);
        std::vector<std::string> missing;
        float start = 1e30f, end = 0;
        for (size_t ci = 0; ci < a.channels_count; ++ci) {
            const cgltf_animation_channel& ch = a.channels[ci];
            if (!ch.target_node || !ch.sampler || !ch.sampler->input || !ch.sampler->output) continue;
            uint8_t path = ch.target_path == cgltf_animation_path_type_translation ? 0
                           : ch.target_path == cgltf_animation_path_type_rotation  ? 1
                           : ch.target_path == cgltf_animation_path_type_scale     ? 2 : 255;
            if (path == 255) continue;   // morph weights
            int node = nodeOf(ch.target_node);
            if (node < 0) {
                if (missing.size() < 8) missing.push_back(nodeName(data, ch.target_node));
                continue;
            }
            AnimTrack tr;
            tr.node = (uint32_t)node;
            tr.path = path;
            tr.interp = ch.sampler->interpolation == cgltf_interpolation_type_step ? 1
                        : ch.sampler->interpolation == cgltf_interpolation_type_cubic_spline ? 2 : 0;
            size_t keys = ch.sampler->input->count;
            tr.times.resize(keys);
            cgltf_accessor_unpack_floats(ch.sampler->input, tr.times.data(), keys);
            size_t comps = path == 1 ? 4 : 3, per = tr.interp == 2 ? 3 : 1;   // cubic spline: in-tangent, value, out-tangent
            std::vector<float> raw(ch.sampler->output->count * cgltf_num_components(ch.sampler->output->type));
            cgltf_accessor_unpack_floats(ch.sampler->output, raw.data(), raw.size());
            if (keys == 0 || raw.size() < keys * per * comps) continue;
            tr.values.resize(keys);
            for (size_t k = 0; k < keys; ++k) {
                const float* v = &raw[(k * per + (per == 3 ? 1 : 0)) * comps];
                vec4 q(v[0], v[1], v[2], comps == 4 ? v[3] : 0.0f);
                tr.values[k] = comps == 3 ? q : glm::length(q) > 1e-8f ? glm::normalize(q) : vec4(0, 0, 0, 1);
            }
            start = std::min(start, tr.times.front());
            end = std::max(end, tr.times.back());
            clip.tracks.push_back(std::move(tr));
        }
        if (clip.tracks.empty()) {
            c.warnings.push_back(std::format("{}: clip '{}' animates no node of the skeleton", clip.source, clip.name));
            continue;
        }
        for (auto& tr : clip.tracks) for (float& t : tr.times) t -= start;
        clip.duration = std::max(end - start, 0.0f);
        if (!missing.empty()) {
            std::string list;
            for (auto& m : missing) list += (list.empty() ? "" : ", ") + m;
            c.warnings.push_back(std::format("{}: clip '{}' targets nodes the character lacks: {}", clip.source, clip.name, list));
        }
        if (a.extras.data) {
            nlohmann::json ex = nlohmann::json::parse(a.extras.data, nullptr, false);
            if (ex.is_object() && ex.contains("speed_mps") && ex["speed_mps"].is_number()) clip.speed = ex["speed_mps"].get<float>();
        }
        measureTravel(c.skeleton, clip);
        if (retarget)
            std::erase_if(clip.tracks, [&](const AnimTrack& t) { return t.path == 0 && (int)t.node != clip.rootNode; });
        c.clips.push_back(std::move(clip));
    }
}
}  // namespace

CharacterAsset loadGltfCharacter(const std::filesystem::path& file, const std::string& name, const CharacterImport& imp) {
    cgltf_data* data = parse(file);
    CharacterAsset c;
    try {
        if (data->skins_count == 0) throw Error(std::format("{} has no skin (no JOINTS_0 / WEIGHTS_0): it is not rigged", file.filename().string()));
        // skeleton: every joint and its ancestors, parents first
        std::unordered_map<const cgltf_node*, int> idx;
        std::vector<bool> needed(data->nodes_count, false);
        for (size_t s = 0; s < data->skins_count; ++s)
            for (size_t j = 0; j < data->skins[s].joints_count; ++j)
                for (const cgltf_node* n = data->skins[s].joints[j]; n; n = n->parent) needed[n - data->nodes] = true;
        Skeleton& sk = c.skeleton;
        std::function<void(const cgltf_node*)> add = [&](const cgltf_node* n) {
            if (idx.count(n) || !needed[n - data->nodes]) return;
            if (n->parent) add(n->parent);
            idx[n] = (int)sk.names.size();
            sk.names.push_back(nodeName(data, n));
            sk.parent.push_back(n->parent ? idx.at(n->parent) : -1);
            vec3 t(0), s(1);
            quat r(1, 0, 0, 0);
            if (n->has_matrix) {
                vec3 skew;
                vec4 persp;
                glm::decompose(glm::make_mat4(n->matrix), s, r, t, skew, persp);
            } else {
                if (n->has_translation) t = glm::make_vec3(n->translation);
                if (n->has_rotation) r = glm::normalize(quat(n->rotation[3], n->rotation[0], n->rotation[1], n->rotation[2]));
                if (n->has_scale) s = glm::make_vec3(n->scale);
            }
            sk.restT.push_back(t);
            sk.restR.push_back(r);
            sk.restS.push_back(s);
        };
        for (size_t i = 0; i < data->nodes_count; ++i) add(&data->nodes[i]);

        // mesh: skinned primitives in their bind space; other meshes ride on their nearest skeleton ancestor
        c.mesh.name = name;
        c.mesh.lods.resize(1);
        c.bones.push_back({-1, mat4(1)});   // bone 0: static (unrigged parts, missing weights)
        FileCtx ctx{file.parent_path(), data, &c.mesh};
        ctx.skin = &c;
        std::unordered_map<const cgltf_skin*, uint32_t> skinBase;
        const cgltf_scene* sc = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
        std::vector<const cgltf_node*> stack;
        if (sc) for (size_t i = 0; i < sc->nodes_count; ++i) stack.push_back(sc->nodes[i]);
        else for (size_t i = 0; i < data->nodes_count; ++i) if (!data->nodes[i].parent) stack.push_back(&data->nodes[i]);
        while (!stack.empty()) {
            const cgltf_node* node = stack.back();
            stack.pop_back();
            for (size_t i = 0; i < node->children_count; ++i) stack.push_back(node->children[i]);
            if (!node->mesh) continue;
            mat4 xf(1);
            if (node->skin) {   // the skinned node's own transform is ignored (glTF spec)
                auto it = skinBase.find(node->skin);
                if (it == skinBase.end()) {
                    it = skinBase.emplace(node->skin, (uint32_t)c.bones.size()).first;
                    const cgltf_skin& s = *node->skin;
                    for (size_t j = 0; j < s.joints_count; ++j) {
                        float m[16];
                        mat4 ibm(1);
                        if (s.inverse_bind_matrices && cgltf_accessor_read_float(s.inverse_bind_matrices, j, m, 16)) ibm = glm::make_mat4(m);
                        c.bones.push_back({idx.at(s.joints[j]), ibm});
                    }
                }
                ctx.boneBase = it->second;
                ctx.rigidBone = -1;
            } else {
                xf = nodeMatrix(node);
                const cgltf_node* anc = node;
                while (anc && !idx.count(anc)) anc = anc->parent;
                ctx.rigidBone = 0;
                if (anc) {
                    ctx.rigidBone = (int)c.bones.size();
                    c.bones.push_back({idx.at(anc), glm::inverse(sk.restGlobal(idx.at(anc)))});
                }
            }
            for (size_t p = 0; p < node->mesh->primitives_count; ++p) appendPrimitive(ctx, node->mesh->primitives[p], xf);
        }
        if (c.mesh.vertices.empty()) throw Error(std::format("{} has no triangle meshes", file.filename().string()));

        // model space: Z up (appendPrimitive converted the vertices), import scale, turned to face +Y, origin at the feet
        mat4 C(1);
        C[1] = vec4(0, 0, 1, 0);
        C[2] = vec4(0, -1, 0, 0);
        mat4 R = glm::rotate(mat4(1), glm::radians(imp.yawDeg), vec3(0, 0, 1)), A = R * glm::scale(mat4(1), vec3(imp.scale));
        for (auto& v : c.mesh.vertices) {
            v.p0 = vec4(vec3(A * vec4(vec3(v.p0), 1.0f)), v.p0.w);
            v.p1 = vec4(mat3(R) * vec3(v.p1), v.p1.w);
            v.tangent = vec4(mat3(R) * vec3(v.tangent), v.tangent.w);
        }
        c.mesh.computeBounds();
        if (imp.groundOrigin) {
            vec3 off(-(c.mesh.aabbMin.x + c.mesh.aabbMax.x) * 0.5f, -(c.mesh.aabbMin.y + c.mesh.aabbMax.y) * 0.5f, -c.mesh.aabbMin.z);
            mat4 T = glm::translate(mat4(1), off);
            for (auto& v : c.mesh.vertices) v.p0 = vec4(vec3(v.p0) + off, v.p0.w);
            A = T * A;
        }
        sk.base = A * C;
        mat4 inv = glm::inverse(sk.base);
        for (auto& b : c.bones) if (b.node >= 0) b.offset = b.offset * inv;
        finish(c.mesh);
        readClips(data, c, [&](const cgltf_node* n) { auto it = idx.find(n); return it == idx.end() ? -1 : it->second; }, file, false);
    } catch (...) {
        cgltf_free(data);
        throw;
    }
    cgltf_free(data);
    logInfo("{}: rigged character, {} vertices, {} joints, {} clips", file.filename().string(), c.mesh.vertices.size(),
            c.skeleton.names.size(), c.clips.size());
    return c;
}

void loadGltfClips(const std::filesystem::path& file, CharacterAsset& c) {
    cgltf_data* data = parse(file);
    size_t before = c.clips.size();
    readClips(data, c, [&](const cgltf_node* n) { return n->name ? c.skeleton.find(n->name) : -1; }, file, true);
    if (data->animations_count == 0) c.warnings.push_back(std::format("{} has no animations", file.filename().string()));
    cgltf_free(data);
    logInfo("{}: {} clips for {}", file.filename().string(), c.clips.size() - before, c.mesh.name);
}

GltfRigInfo inspectGltfRig(const std::filesystem::path& file) {
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, file.string().c_str(), &data) != cgltf_result_success) throw Error(std::format("cannot parse {}", file.string()));
    GltfRigInfo info;
    info.meshes = data->meshes_count;
    for (size_t s = 0; s < data->skins_count; ++s) info.joints += data->skins[s].joints_count;
    for (size_t a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& an = data->animations[a];
        float lo = 1e30f, hi = 0;
        for (size_t ch = 0; ch < an.channels_count; ++ch) {
            const cgltf_accessor* in = an.channels[ch].sampler ? an.channels[ch].sampler->input : nullptr;
            if (in && in->has_min && in->has_max) { lo = std::min(lo, in->min[0]); hi = std::max(hi, in->max[0]); }
        }
        info.clips.push_back({an.name ? an.name : std::format("clip_{}", a + 1), hi > lo ? hi - lo : 0.0f});
    }
    cgltf_free(data);
    return info;
}

Texture loadTextureFile(const std::filesystem::path& file, bool srgb) {
    int w, h, n;
    stbi_uc* px = stbi_load(file.string().c_str(), &w, &h, &n, 4);
    if (!px) throw Error(std::format("cannot load texture {}", file.string()));
    Texture t;
    t.width = (uint32_t)w; t.height = (uint32_t)h; t.srgb = srgb;
    t.name = file.string() + (srgb ? "#s" : "#l");
    t.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return t;
}
}  // namespace df
