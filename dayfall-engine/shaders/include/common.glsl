// GPU data contract. Must match src/render/GpuTypes.h exactly (std430 / std140).
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shader_draw_parameters : require

#define MAX_VIEWS 5          // main view + up to 4 shadow cascades
#define GROUP_OPAQUE 0u
#define GROUP_TWOSIDED 1u
#define GROUP_MASKED 2u
#define GROUP_WATER 3u
#define GROUP_BLEND 4u

#define MODEL_LIT 0u
#define MODEL_TERRAIN 1u
#define MODEL_FOLIAGE 2u
#define MODEL_COURSES 3u
#define MODEL_WATER 4u
#define MODEL_EMISSIVE 5u
#define MODEL_UNLIT 6u
#define MODEL_GRASS 7u
#define MODEL_ROCK 8u

#define MAT_TWOSIDED 1u
#define MAT_MASKED 2u
#define MAT_BLEND 4u
#define MAT_WIND 8u
#define MAT_DUCKWEED 16u
#define MAT_VERTEXCOLOR 32u
#define MAT_NORMAL_DX 64u      // DirectX normal map (green down): Unreal exports

#define INST_SHADOW 1u
#define INST_AXIS_SCALE 2u   // flags >> 8 indexes instanceScales

struct Vertex {
    vec4 p0;   // position.xyz, uv0.x
    vec4 p1;   // normal.xyz, uv0.y
    vec4 tangent;
    vec4 p3;   // uv1.xy, color0 (rgba8 bits), color1 (rgba8 bits)
};
struct Instance {
    vec4 posScale;   // world position, uniform scale (the largest axis when INST_AXIS_SCALE)
    vec4 rot;        // quaternion xyzw
    uint mesh;
    uint flags;
    float cullDistance;
    float seed;      // 0..1, per-instance variation
};
struct MeshInfo {
    vec4 bounds;     // local bounding sphere: center, radius
    uint firstLod;
    uint lodCount;
    uint pad0, pad1;
};
struct MeshLod {
    uint firstRef;   // into batchRefs
    uint refCount;
    float maxDistance;
    uint pad;
};
struct Batch {
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint material;
    uint mainOffset;     // into the visible list, main view
    uint shadowOffset;   // into the visible list, each shadow view (relative to its base)
    uint group;
    uint pad;
};
struct Material {
    uvec4 h0;        // model, flags, tex base colour, tex normal
    uvec4 h1;        // tex orm, tex emissive, alphaCutoff (float bits), unused
    vec4 c[8];       // colours (meaning per model)
    vec4 p[4];       // parameters (meaning per model)
};
struct Light {
    vec4 posRange;   // position, range
    vec4 color;      // rgb * intensity (radiant intensity, W/sr equivalent)
};
struct DrawCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 view;
    mat4 proj;
    mat4 invViewProj;
    mat4 cascadeViewProj[4];
    vec4 viewPlanes[MAX_VIEWS * 6];   // culling planes, view 0 = camera, 1..4 = cascades
    vec4 cameraPos;          // xyz, time (s)
    vec4 sunDir;             // xyz towards the sun, illuminance
    vec4 sunColor;           // rgb, angular radius (rad)
    vec4 sh[9];              // sky irradiance, SH L2 (rgb)
    vec4 fogNear;            // haze colour near, amount
    vec4 fogFar;             // haze colour far, start (m)
    vec4 fogParams;          // depth (m), height fog density, height falloff, height fog base
    vec4 cascadeSplits;      // view-space far distance of each cascade
    vec4 shadowParams;       // 1/size, normal bias (m), pcf radius (texels), cascade count
    vec4 viewport;           // w, h, 1/w, 1/h
    vec4 tonemap;            // exposure scale, look power, look saturation, look slope
    vec4 wind;               // dir.xy, strength scale, gust frequency
    vec4 clouds;             // coverage, height (m), scale, enabled
    vec4 cloudColor;         // rgb, shadow amount
    vec4 water;              // water level, unused, camera near plane (m), unused
    vec4 lightGrid;          // origin xyz, cell size
    uvec4 lightGridDims;     // x, y, z, light count
    uvec4 counts;            // instances, batches, views, flags
    vec4 lodBias;            // lod distance scale, cull distance scale, unused, unused
    uvec4 totals;            // visible-list size of the main view, of each shadow view, unused, unused
    vec4 post;               // vignette, dither, unused, unused
    vec4 terrainA;           // origin xy, sample spacing (m), samples per side
    vec4 terrainB;           // height min, height range, snowline, rock slope (deg)
    vec4 terrainC;           // dry amount, enabled, material index (bits), unused
} frame;

layout(std430, set = 0, binding = 1) readonly buffer Vertices { Vertex vertices[]; };
layout(std430, set = 0, binding = 2) readonly buffer Instances { Instance instances[]; };
layout(std430, set = 0, binding = 3) readonly buffer Meshes { MeshInfo meshes[]; };
layout(std430, set = 0, binding = 4) readonly buffer Lods { MeshLod lods[]; };
layout(std430, set = 0, binding = 5) readonly buffer BatchRefs { uint batchRefs[]; };
layout(std430, set = 0, binding = 6) readonly buffer Batches { Batch batches[]; };
layout(std430, set = 0, binding = 7) readonly buffer Materials { Material materials[]; };
layout(std430, set = 0, binding = 10) readonly buffer Lights { Light lights[]; };
layout(std430, set = 0, binding = 11) readonly buffer LightGrid { uint lightGridData[]; };
layout(set = 0, binding = 12) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 13) uniform sampler2D skyLut;
layout(set = 0, binding = 14) uniform sampler2D sceneColor;
layout(set = 0, binding = 15) uniform sampler2D sceneDepth;
layout(set = 0, binding = 16) uniform sampler2D hdrColor;
layout(set = 0, binding = 17) uniform sampler2D terrainHeight;   // R16 unorm: min + v * range
layout(set = 0, binding = 18) uniform sampler2D terrainPaint0;   // dirt, rock, snow, wet (painted)
layout(set = 0, binding = 19) uniform sampler2D terrainPaint1;   // dry, grass, -, -
layout(set = 0, binding = 20) uniform sampler2D terrainPaths;    // path mask, lane mask, lane signed distance (m), -
// binding 21: terrain patches (terrain.vert)
layout(std430, set = 0, binding = 22) readonly buffer InstanceScales { vec4 instanceScales[]; };   // per-axis scales
layout(set = 0, binding = 23) uniform sampler2D textures[];      // must stay the last binding (variable count)

vec3 instanceScale(Instance inst) {
    return (inst.flags & INST_AXIS_SCALE) != 0u ? instanceScales[inst.flags >> 8].xyz : vec3(inst.posScale.w);
}

layout(push_constant) uniform Push {
    uint batchBase;
    uint view;
    uint flags;
    uint pad;
} pc;

const float PI = 3.14159265359;

vec3 quatRotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}
vec4 unpackColor(float bits) { return unpackUnorm4x8(floatBitsToUint(bits)); }
uint shadowBase(uint view) { return frame.totals.x + (view - 1u) * frame.totals.y; }
float hash11(float p) { p = fract(p * 0.1031); p *= p + 33.33; p *= p + p; return fract(p); }
