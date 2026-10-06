#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "noise.glsl"
#include "sky.glsl"
#include "lighting.glsl"
#include "surface.glsl"
layout(constant_id = 0) const uint MASKED = 0u;   // alpha-tested pipeline variant
layout(location = 0) in vec3 vPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec4 vUv;
layout(location = 4) in vec4 vColor0;
layout(location = 5) in vec4 vColor1;
layout(location = 6) flat in uint vMaterial;
layout(location = 7) flat in vec4 vInst;
layout(location = 0) out vec4 outColor;

void main() {
    Material m = materials[vMaterial];
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    vec3 v = normalize(frame.cameraPos.xyz - vPos);
    uint model = m.h0.x;
    Surface s;
    if (model == MODEL_TERRAIN) s = shadeTerrain(m, vPos, n, vColor0, vColor1, vUv.zw);
    else if (model == MODEL_FOLIAGE) s = shadeFoliage(m, n, vUv.xy, vColor0, vInst.w);
    else if (model == MODEL_GRASS) s = shadeGrass(m, n, vUv.xy, vInst);
    else if (model == MODEL_COURSES) s = shadeCourses(m, vPos, n, vUv.xy);
    else s = shadeLit(m, n, vTangent, vUv.xy, vColor0);

    float alpha = 1.0;
    if (MASKED == 1u) {
        float cutoff = uintBitsToFloat(m.h1.z);
        if ((pc.flags & 1u) != 0u) {          // no MSAA: plain alpha test
            if (s.alpha < cutoff) discard;
            alpha = 1.0;
        } else {
            alpha = (s.alpha - cutoff) / max(fwidth(s.alpha), 1e-4) + 0.5;   // sharp alpha-to-coverage
            if (alpha <= 0.0) discard;
        }
    }
    vec3 col;
    if (model == MODEL_UNLIT) col = s.albedo + s.emissive;
    else if (model == MODEL_EMISSIVE) col = s.albedo * 0.05 + s.emissive;
    else {
        float viewDepth = -(frame.view * vec4(vPos, 1.0)).z;
        col = lightSurface(s, vPos, v, viewDepth);
    }
    col = applyHaze(col, vPos);
    outColor = vec4(col, clamp(alpha, 0.0, 1.0));
}
