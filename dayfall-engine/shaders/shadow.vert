#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "vertex.glsl"
layout(location = 0) out vec2 vUv;
layout(location = 1) flat out uint vMaterial;

void main() {
    Pulled p = pullVertex();
    vUv = p.uv.xy;
    vMaterial = p.material;
    gl_Position = frame.cascadeViewProj[pc.view - 1u] * vec4(p.pos, 1.0);
}
