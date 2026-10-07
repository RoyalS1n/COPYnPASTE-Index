#pragma once
// Mirrors shaders/include/common.glsl exactly. Change both together.
#include "core/Math.h"
#include <cstdint>

namespace df {
constexpr uint32_t kMaxViews = 5;     // camera + 4 shadow cascades
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;

enum Group : uint32_t { GroupOpaque = 0, GroupTwoSided = 1, GroupMasked = 2, GroupWater = 3, GroupBlend = 4, GroupCount = 5 };
enum Model : uint32_t { ModelLit = 0, ModelTerrain = 1, ModelFoliage = 2, ModelCourses = 3, ModelWater = 4,
                        ModelEmissive = 5, ModelUnlit = 6, ModelGrass = 7, ModelRock = 8 };
enum MatFlags : uint32_t { MatTwoSided = 1, MatMasked = 2, MatBlend = 4, MatWind = 8, MatDuckweed = 16, MatVertexColor = 32 };
enum InstFlags : uint32_t { InstShadow = 1 };

struct GpuVertex {
    vec4 p0;        // pos.xyz, uv0.x
    vec4 p1;        // normal.xyz, uv0.y
    vec4 tangent;
    vec4 p3;        // uv1.xy, color0 bits, color1 bits
};
static_assert(sizeof(GpuVertex) == 64);

struct GpuInstance {
    vec4 posScale;
    vec4 rot;       // quaternion xyzw
    uint32_t mesh;
    uint32_t flags;
    float cullDistance;
    float seed;
};
static_assert(sizeof(GpuInstance) == 48);

struct GpuMeshInfo { vec4 bounds; uint32_t firstLod, lodCount, pad0, pad1; };
struct GpuMeshLod { uint32_t firstRef, refCount; float maxDistance; uint32_t pad; };
struct GpuBatch { uint32_t indexCount, firstIndex; int32_t vertexOffset; uint32_t material, mainOffset, shadowOffset, group, pad; };
struct GpuMaterial {
    uvec4 h0{ModelLit, 0, kNoTexture, kNoTexture};   // model, flags, tex base, tex normal
    uvec4 h1{kNoTexture, kNoTexture, 0, 0};          // tex orm, tex emissive, alpha cutoff (float bits), -
    vec4 c[8]{};
    vec4 p[4]{};
};
static_assert(sizeof(GpuMaterial) == 224);
struct GpuLight { vec4 posRange; vec4 color; };

struct GpuFrame {
    mat4 viewProj, view, proj, invViewProj;
    mat4 cascadeViewProj[4];
    vec4 viewPlanes[kMaxViews * 6];
    vec4 cameraPos, sunDir, sunColor;
    vec4 sh[9];
    vec4 fogNear, fogFar, fogParams, cascadeSplits, shadowParams, viewport, tonemap, wind, clouds, cloudColor, water, lightGrid;
    uvec4 lightGridDims, counts;
    vec4 lodBias;
    uvec4 totals;
    vec4 post;
};
}  // namespace df
