#version 460
#extension GL_GOOGLE_include_directive : require
// Painterly look (environment.post.painterly): a generalised Kuwahara filter with polynomial sector weights
// (Kyprianidis, Kang, Doellner 2009; Kyprianidis et al. 2010) over the tone-mapped image, blended with it, a
// chroma lift, then ink lines from depth: silhouettes (nearer than a neighbour) and creases (normals rebuilt from
// depth disagree) of solid shapes, none in clutter (grass, leaves), far away or under water; then the vignette and
// dither.
#include "common.glsl"
#include "post.glsl"   // postInput: the tone-mapped image, gamma 2.2 encoded
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

ivec2 lim;
vec3 colorAt(ivec2 p) { return texelFetch(postInput, clamp(p, ivec2(0), lim), 0).rgb; }
float sq(float x) { return x * x; }

// Eight overlapping sectors of a disc. Every sample counts in each sector by a polynomial weight times a radial
// Gaussian (a sample and its mirror share the weights, half a turn apart); the sector means then blend by
// 1 / sigma^8, so the calmest side of an edge wins: edges stay sharp, flat areas turn into strokes.
vec3 kuwahara(ivec2 p, float radius) {
    const float sinE = sin(3.0 * PI / 16.0), cosE = cos(3.0 * PI / 16.0);   // sector envelope: 3/2 pi / 8
    float zeta = 2.0 / radius, eta = (zeta + cosE) / (sinE * sinE);
    vec3 c0 = colorAt(p), m[8], s[8];
    float wsum[4];   // sectors k and k + 4 always collect the same total weight
    for (int k = 0; k < 8; ++k) { m[k] = c0 * 0.125; s[k] = c0 * c0 * 0.125; }
    for (int k = 0; k < 4; ++k) wsum[k] = 0.125;
    int r = int(ceil(radius));
    for (int j = 0; j <= r; ++j)
        for (int i = j == 0 ? 1 : -r; i <= r; ++i) {
            vec2 v = vec2(i, j) / radius;
            float d2 = dot(v, v);
            if (d2 > 1.0) continue;
            vec2 u = vec2(v.x - v.y, v.x + v.y) * 0.70710678;   // v turned 45 degrees: the odd sectors
            vec2 pv = zeta - eta * v * v, pu = zeta - eta * u * u;
            float w[8] = float[](sq(max(v.y + pv.x, 0.0)), sq(max(u.y + pu.x, 0.0)), sq(max(pv.y - v.x, 0.0)), sq(max(pu.y - u.x, 0.0)),
                                 sq(max(pv.x - v.y, 0.0)), sq(max(pu.x - u.y, 0.0)), sq(max(v.x + pv.y, 0.0)), sq(max(u.x + pu.y, 0.0)));
            float g = exp(-PI * d2) / max(w[0] + w[1] + w[2] + w[3] + w[4] + w[5] + w[6] + w[7], 1e-6);
            vec3 a = colorAt(p + ivec2(i, j)), b = colorAt(p - ivec2(i, j));
            for (int k = 0; k < 8; ++k) {
                float x = w[k] * g;
                wsum[k & 3] += x;
                m[k] += a * x;
                s[k] += a * a * x;
                m[(k + 4) & 7] += b * x;
                s[(k + 4) & 7] += b * b * x;
            }
        }
    vec3 sum = vec3(0.0);
    float wt = 0.0;
    for (int k = 0; k < 8; ++k) {
        vec3 mean = m[k] / wsum[k & 3];
        float sigma = dot(sqrt(abs(s[k] / wsum[k & 3] - mean * mean)), vec3(1.0));
        float w = 1.0 / pow(max(sigma, 0.02), 8.0);
        sum += mean * w;
        wt += w;
    }
    return sum / wt;
}

// view-space position from the resolved depth (reversed Z, infinite far: depth = near / distance; sky = 100 km)
vec3 viewPos(ivec2 p) {
    float z = frame.water.z / max(texelFetch(sceneDepth, clamp(p, ivec2(0), lim), 0).r, frame.water.z * 1e-5);
    vec2 ndc = (vec2(p) + 0.5) * frame.viewport.zw * 2.0 - 1.0;
    return vec3(ndc.x / frame.proj[0][0], -ndc.y / frame.proj[1][1], -1.0) * z;
}
// normal from the flatter side along each axis, so it never smears across a silhouette
vec3 normalAt(vec3 c, vec3 l, vec3 r, vec3 u, vec3 d) {
    vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    vec3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    return normalize(cross(dx, dy));
}

// how well a, b, c (in a row) lie on one plane: 1/z is linear across a plane in screen space
float planar(vec3 a, vec3 b, vec3 c) { return 1.0 - smoothstep(0.004, 0.02, abs(b.z / a.z - 2.0 + b.z / c.z)); }
// silhouette on the near side: `far` lies behind the plane through b and c, and c, b, a carry one surface on, so a
// line needs a solid shape at least three pixels across: thin grass and twigs get none
float outline(vec3 far, vec3 c, vec3 b, vec3 a) { return max(2.0 - c.z / b.z - c.z / far.z, 0.0) * planar(c, b, a); }
// crease: the normals of c and its neighbour b disagree and the surface bends rather than steps between them:
// 1/z at b extrapolated from c's side and at c from b's side miss alike at a bend, oppositely at a step (one grass
// blade in front of the next); c's side (c, o, o2) must be planar
float crease(vec3 n, vec3 nb, vec3 b2, vec3 b, vec3 c, vec3 o, vec3 o2) {
    float ea = 1.0 / b.z - 2.0 / c.z + 1.0 / o.z, eb = 1.0 / c.z - 2.0 / b.z + 1.0 / b2.z;
    float bends = smoothstep(-0.1, 0.1, ea * eb / (ea * ea + eb * eb + 1e-20));
    return (1.0 - dot(n, nb)) * bends * planar(c, o, o2) * (1.0 - smoothstep(0.05, 0.15, abs(b.z - c.z) / -c.z));
}

// a big depth step between two neighbours (relative to the nearer)
float jump(vec3 a, vec3 b) { return smoothstep(0.08, 0.2, abs(a.z - b.z) / min(-a.z, -b.z)); }

float ink(ivec2 p) {
    vec3 c = viewPos(p), l = viewPos(p + ivec2(-1, 0)), r = viewPos(p + ivec2(1, 0)), u = viewPos(p + ivec2(0, -1)), d = viewPos(p + ivec2(0, 1));
    vec3 ul = viewPos(p + ivec2(-1, -1)), ur = viewPos(p + ivec2(1, -1)), dl = viewPos(p + ivec2(-1, 1)), dr = viewPos(p + ivec2(1, 1));
    vec3 l2 = viewPos(p + ivec2(-2, 0)), r2 = viewPos(p + ivec2(2, 0)), u2 = viewPos(p + ivec2(0, -2)), d2 = viewPos(p + ivec2(0, 2));
    float sil = max(max(outline(l, c, r, r2), outline(r, c, l, l2)), max(outline(u, c, d, d2), outline(d, c, u, u2)));
    vec3 n = normalAt(c, l, r, u, d);
    float bend = max(max(crease(n, normalAt(l, l2, c, ul, dl), l2, l, c, r, r2), crease(n, normalAt(r, c, r2, ur, dr), r2, r, c, l, l2)),
                     max(crease(n, normalAt(u, ul, ur, u2, c), u2, u, c, d, d2), crease(n, normalAt(d, dl, dr, c, d2), d2, d, c, u, u2)));
    // clutter: one outline crosses three or four of these sixteen neighbour pairs, grass and leaves cross many
    float steps = jump(l2, l) + jump(l, c) + jump(c, r) + jump(r, r2) + jump(u2, u) + jump(u, c) + jump(c, d) + jump(d, d2) +
                  jump(ul, u) + jump(u, ur) + jump(dl, d) + jump(d, dr) + jump(ul, l) + jump(l, dl) + jump(ur, r) + jump(r, dr);
    // no ink far away (forests turn to noise), in clutter, or under water (transparent: the depth is the bed)
    vec4 w = frame.invViewProj * vec4(((vec2(p) + 0.5) * frame.viewport.zw * 2.0 - 1.0) * vec2(1.0, -1.0), frame.water.z / -c.z, 1.0);
    float fade = (1.0 - smoothstep(150.0, 600.0, -c.z)) * (1.0 - smoothstep(3.5, 6.0, steps)) * smoothstep(-0.05, 0.05, w.z / w.w - frame.water.x);
    return clamp(sil * frame.painterlyEdges.x + bend * frame.painterlyEdges.y, 0.0, 1.0) * fade * frame.painterly.z;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    lim = textureSize(postInput, 0) - 1;
    vec3 c = colorAt(p);
    if (frame.painterly.x >= 0.5) c = mix(c, kuwahara(p, frame.painterly.x), frame.painterly.y);
    float l = dot(c, LUMA);
    c = max(l + frame.painterly.w * (c - l), 0.0) * (1.0 - (frame.painterly.z > 0.0 ? ink(p) : 0.0));
    outColor = vec4(finish(pow(c, vec3(2.2)), vUv), 1.0);
}
