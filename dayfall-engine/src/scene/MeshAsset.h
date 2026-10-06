#pragma once
// CPU geometry of one engine mesh, independent of any scene: built by the
// glTF loader, the primitive generators and the terrain, cached by the scene
// builder and appended into a Scene's shared vertex / index arrays.
#include "core/Math.h"
#include "render/GpuTypes.h"
#include <string>
#include <vector>

namespace df {
struct Texture { std::vector<uint8_t> rgba; uint32_t width = 0, height = 0; bool srgb = true; std::string name; };
struct MaterialDef { std::string name; GpuMaterial gpu; uint32_t group = GroupOpaque; };

struct MeshPart { uint32_t firstIndex = 0, indexCount = 0; std::string material; };
struct MeshAsset {
    std::string name;
    std::vector<GpuVertex> vertices;
    std::vector<uint32_t> indices;           // relative to this mesh's first vertex
    std::vector<std::vector<MeshPart>> lods; // lods[0] = full detail; all LODs share the vertices
    std::vector<float> lodDistances;         // max camera distance of each LOD
    vec4 bounds{0, 0, 0, 1};                 // bounding sphere
    vec3 aabbMin{0}, aabbMax{0};
    // materials / textures the source file defines; a map material of the same name wins
    std::vector<MaterialDef> materials;      // texture indices refer to `textures`
    std::vector<Texture> textures;
    size_t triangleCount() const;
    void computeBounds();
};

uint32_t packColor(vec4 c);
GpuVertex makeVertex(vec3 p, vec3 n, vec2 uv, vec4 tangent = vec4(0), vec4 color0 = vec4(1), vec4 color1 = vec4(0),
                     vec2 uv1 = vec2(0));

// Builds a MeshAsset triangle by triangle, grouped by material.
class MeshBuilder {
public:
    uint32_t vertex(const GpuVertex& v) { verts_.push_back(v); return (uint32_t)verts_.size() - 1; }
    uint32_t vertex(vec3 p, vec3 n, vec2 uv, vec4 c0 = vec4(1)) { return vertex(makeVertex(p, n, uv, vec4(0), c0)); }
    void tri(const std::string& material, uint32_t a, uint32_t b, uint32_t c);
    void quad(const std::string& material, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { tri(material, a, b, c); tri(material, a, c, d); }
    // flat-shaded convenience: a planar polygon (counter-clockwise seen from the front)
    void polygon(const std::string& material, const std::vector<vec3>& pts, vec2 uvScale = vec2(1));
    size_t vertexCount() const { return verts_.size(); }
    GpuVertex& at(uint32_t i) { return verts_[i]; }
    MeshAsset build(const std::string& name);
private:
    std::vector<GpuVertex> verts_;
    std::vector<std::pair<std::string, std::vector<uint32_t>>> groups_;
};

struct LodSpec { float ratio = 0.25f; float distance = 1e9f; bool sloppy = false; };
// Extra LODs with meshoptimizer (index-only: the same vertices, fewer triangles).
void generateLods(MeshAsset& m, float lod0Distance, const std::vector<LodSpec>& lods);
// Recomputes smooth vertex normals from the triangles.
void computeNormals(MeshAsset& m);
}  // namespace df
