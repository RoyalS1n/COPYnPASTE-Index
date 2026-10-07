#version 460
#extension GL_GOOGLE_include_directive : require
// Terrain shading: per-pixel normal from the heightmap; biome masks (rock on
// slopes, snow above the snowline, wet near the water, dry patches) computed
// here, combined with painted layers and paths, then the terrain model.
#include "common.glsl"
#include "noise.glsl"
#include "sky.glsl"
#include "lighting.glsl"
#include "surface.glsl"
#include "terrain.glsl"
layout(location = 0) in vec3 vPos;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 p = vPos;
    vec3 n = terrainNormalAt(p.xy);
    vec2 uv = terrainUv(p.xy);
    vec4 p0 = texture(terrainPaint0, uv), p1 = texture(terrainPaint1, uv), pa = texture(terrainPaths, uv);
    Material m = materials[floatBitsToUint(frame.terrainC.z)];
    float slope = degrees(acos(clamp(n.z, -1.0, 1.0)));
    float brk = fbm(vec3(p.xy / 55.0, 0.0), 4) * 2.0 - 1.0;
    float big = fbm(vec3(p.xy / 400.0 + vec2(9.0, -4.0), 0.0), 4) * 2.0 - 1.0;
    float snowLine = frame.terrainB.z + 45.0 * big;
    float snow = smoothstep(snowLine - 25.0, snowLine + 25.0, p.z + 15.0 * brk) * (1.0 - smoothstep(40.0, 56.0, slope + 8.0 * brk));
    snow = max(snow, p0.b);
    float rs = frame.terrainB.w;
    float rock = max(smoothstep(rs - 4.0, rs + 10.0, slope + 9.0 * brk), p0.g) * (1.0 - snow) * (1.0 - p1.g);
    float wl = m.p[0].x;
    float wet = max(1.0 - smoothstep(wl + 0.2, wl + 1.6, p.z), p0.a);
    float dry = max(smoothstep(-0.2, 0.6, big + 0.4 * brk) * frame.terrainC.x, p1.r) * (1.0 - p1.g * 0.7);
    float path = max(pa.r, p0.r);
    rock *= 1.0 - path;
    dry *= 1.0 - path;
    float grass = clamp(1.0 - rock - snow, 0.0, 1.0);
    Surface s = shadeTerrain(m, p, n, vec4(grass, rock, snow, wet), vec4(path, dry, pa.g, 0.0), vec2(pa.b, 0.0));
    vec3 v = normalize(frame.cameraPos.xyz - p);
    float viewDepth = -(frame.view * vec4(p, 1.0)).z;
    vec3 col = lightSurface(s, p, v, viewDepth);
    outColor = vec4(applyHaze(col, p), 1.0);
}
