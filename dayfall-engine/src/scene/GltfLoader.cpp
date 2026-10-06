#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "scene/GltfLoader.h"
#include "core/Log.h"
#include "core/Error.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace df {
namespace {
struct FileCtx {
    std::filesystem::path dir;
    cgltf_data* data;
    MeshAsset* out;
    std::unordered_map<const cgltf_image*, uint32_t> srgbTex, linTex;
    std::unordered_map<const cgltf_material*, std::string> mats;
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
        p.scale = (sc.x + sc.y + sc.z) / 3.0f;   // non-uniform scale is averaged
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
