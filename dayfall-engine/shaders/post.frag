#version 460
#extension GL_GOOGLE_include_directive : require
// Exposure, AgX tone mapping with a look (contrast / saturation), vignette, dither.
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 agx(vec3 v) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float minEv = -12.47393, maxEv = 4.026069;
    v = inset * v;
    v = clamp(log2(max(v, vec3(1e-10))), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    return agxContrast(v);
}
vec3 agxEotf(vec3 v) {
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    return pow(max(outset * v, vec3(0.0)), vec3(2.2));
}
vec3 agxLook(vec3 v) {
    float luma = dot(v, vec3(0.2126, 0.7152, 0.0722));
    v = pow(max(v * frame.tonemap.w, vec3(0.0)), vec3(frame.tonemap.y));
    return luma + frame.tonemap.z * (v - luma);
}

void main() {
    vec3 c = texture(hdrColor, vUv).rgb * frame.tonemap.x;
    c = agxEotf(agxLook(agx(c)));
    vec2 q = vUv * 2.0 - 1.0;
    float vig = 1.0 - smoothstep(0.55, 1.45, length(q * vec2(0.95, 0.8)));
    c *= mix(1.0, vig, frame.post.x);
    // triangular dither against banding in 8-bit output
    float r = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) + fract(sin(dot(gl_FragCoord.xy, vec2(39.3468, 11.135))) * 24634.6345) - 1.0;
    c += r * frame.post.y / 255.0;
    outColor = vec4(c, 1.0);
}
