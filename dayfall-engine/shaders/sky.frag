#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "noise.glsl"
#include "sky.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 clip = vec4(vUv * 2.0 - 1.0, 1.0, 1.0);   // the near plane (z = 0 is at infinity: w = 0)
    vec4 wp = frame.invViewProj * clip;
    vec3 dir = normalize(wp.xyz / wp.w - frame.cameraPos.xyz);
    vec3 col = skyRadiance(dir);
    // sun disc
    float cosA = dot(dir, frame.sunDir.xyz);
    float r = max(frame.sunColor.w, 0.0045);
    if (cosA > cos(r)) col += frame.sunColor.rgb * frame.sunDir.w * 40.0;
    vec4 cl = cloudLayer(frame.cameraPos.xyz, dir);
    col = col * (1.0 - cl.a) + cl.rgb;
    outColor = vec4(col, 1.0);
}
