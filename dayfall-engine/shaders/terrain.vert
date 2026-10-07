#version 460
#extension GL_GOOGLE_include_directive : require
// CDLOD terrain: one instanced draw of a 33x33 grid per patch; vertices morph
// to the next coarser grid before the patch switches LOD, so there are no
// cracks or pops. pc.view 0 = camera, 1..4 = shadow cascades.
#include "common.glsl"
#include "terrain.glsl"
layout(std430, set = 0, binding = 21) readonly buffer Patches { vec4 patchData[]; };   // per patch: (x, y, size, morph start), (morph end, level, -, -)
layout(location = 0) out vec3 vPos;

void main() {
    uint p = gl_InstanceIndex;
    vec4 a = patchData[p * 2u], b = patchData[p * 2u + 1u];
    vec2 g = vec2(float(gl_VertexIndex % 33u), float(gl_VertexIndex / 33u));
    float cell = a.z / 32.0;
    vec2 xy = a.xy + g * cell;
    float h = terrainHeightAt(xy);
    float d = distance(frame.cameraPos.xyz, vec3(xy, h));
    float k = clamp((d - a.w) / max(b.x - a.w, 1e-3), 0.0, 1.0);
    vec2 gm = g - fract(g * 0.5) * 2.0 * k;
    xy = a.xy + gm * cell;
    // keep the grid inside the terrain (patches at the far edge are clipped)
    vec2 lo = frame.terrainA.xy, hi = frame.terrainA.xy + vec2(frame.terrainA.z * (frame.terrainA.w - 1.0));
    xy = clamp(xy, lo, hi);
    vec3 pos = vec3(xy, terrainHeightAt(xy));
    vPos = pos;
    gl_Position = (pc.view == 0u ? frame.viewProj : frame.cascadeViewProj[pc.view - 1u]) * vec4(pos, 1.0);
}
