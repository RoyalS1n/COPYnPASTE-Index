#version 460
#extension GL_GOOGLE_include_directive : require
// Water surfaces: refraction of the opaque scene with depth-based absorption,
// sky reflection with Fresnel, sun glint, animated ripples; duckweed mats on
// shallow marsh water (MAT_DUCKWEED, colour0.r = depth / 2).
#include "common.glsl"
#include "noise.glsl"
#include "sky.glsl"
#include "lighting.glsl"
#include "surface.glsl"
layout(location = 0) in vec3 vPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec4 vUv;
layout(location = 4) in vec4 vColor0;
layout(location = 5) in vec4 vColor1;
layout(location = 6) flat in uint vMaterial;
layout(location = 7) flat in vec4 vInst;
layout(location = 0) out vec4 outColor;

float linearDist(float d) { return frame.water.z / max(d, 1e-7); }   // reversed infinite Z

void main() {
    Material m = materials[vMaterial];
    float time = frame.cameraPos.w;
    vec3 p = vPos;
    // ripples: directional waves, each faded out where a pixel spans a good part of its
    // wavelength, so distant water stays smooth instead of sparkling
    float footprint = length(fwidth(p.xy));
    vec2 g = vec2(0.0);
    const vec4 waves[4] = vec4[4](vec4(0.8, 0.6, 1.9, 0.06), vec4(-0.4, 0.9, 3.7, 0.035), vec4(0.9, -0.3, 7.3, 0.02), vec4(0.2, 1.0, 13.0, 0.012));
    const float speed[4] = float[4](1.3, 2.1, 3.0, 4.2);
    for (int i = 0; i < 4; ++i) {
        float lambda = 6.2831853 / waves[i].z;
        float keep = 1.0 - smoothstep(lambda * 0.08, lambda * 0.4, footprint);
        g += waves[i].xy * cos(dot(p.xy, waves[i].xy) * waves[i].z + time * speed[i]) * waves[i].w * keep;
    }
    g *= m.p[0].z > 0.0 ? m.p[0].z : 1.0;
    vec3 n = normalize(vec3(-g, 1.0));
    vec3 v = normalize(frame.cameraPos.xyz - p);
    float NoV = max(dot(n, v), 1e-3);

    vec2 suv = gl_FragCoord.xy * frame.viewport.zw;
    float fragDist = linearDist(gl_FragCoord.z);
    float thick = max(linearDist(texture(sceneDepth, suv).r) - fragDist, 0.0);
    vec2 ruv = suv + n.xy * 0.04 * clamp(thick, 0.0, 1.0);
    float rthick = linearDist(texture(sceneDepth, ruv).r) - fragDist;
    if (rthick < 0.0) { ruv = suv; rthick = thick; }
    vec3 refr = texture(sceneColor, ruv).rgb;
    vec3 tint = m.c[0].rgb;
    vec3 absorb = exp(-max(rthick, 0.0) * m.p[0].x * (1.0 - tint));
    vec3 R = reflect(-v, n);
    vec3 refl = skyRadiance(normalize(vec3(R.xy, max(R.z, 0.01))));
    vec4 cl = cloudLayer(p, normalize(vec3(R.xy, max(R.z, 0.01))));
    refl = refl * (1.0 - cl.a) + cl.rgb;
    float fx = clamp(1.0 - NoV, 0.0, 1.0);
    float F = 0.02 + 0.98 * fx * fx * fx * fx * fx;
    vec3 L = frame.sunDir.xyz;
    vec3 h = normalize(v + L);
    float a = max(m.p[0].y * m.p[0].y, 0.0015);
    vec3 sunE = frame.sunColor.rgb * frame.sunDir.w;
    float shadow = sampleShadow(p, n, -(frame.view * vec4(p, 1.0)).z);
    vec3 glint = sunE * shadow * max(dot(n, L), 0.0) * D_GGX(max(dot(n, h), 0.0), a) * V_Smith(NoV, max(dot(n, L), 0.0), a) * F;
    vec3 col = mix(refr * absorb, refl, F) + glint;

    if ((m.h0.y & MAT_DUCKWEED) != 0u) {
        float depth = vColor0.r * 2.0;
        float shallow = smoothrange(depth, 0.55, 0.08);
        float patches = smoothrange(fbm(p * 0.16, 4), 0.45, 0.6);
        float speck = smoothrange(voronoiEdge(p.xy * 90.0), 0.0, 0.25);
        float weed = smoothrange(shallow * patches * (0.45 + 0.55 * speck), 0.25, 0.6);
        Surface s = defaultSurface(vec3(0, 0, 1));
        s.albedo = mix(m.c[1].rgb * 0.4, m.c[1].rgb, speck);
        s.rough = 0.55;
        vec3 weedCol = lightSurface(s, p, v, -(frame.view * vec4(p, 1.0)).z);
        col = mix(col, weedCol, weed);
    }
    outColor = vec4(applyHaze(col, p), 1.0);
}
