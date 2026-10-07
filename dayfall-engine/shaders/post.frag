#version 460
#extension GL_GOOGLE_include_directive : require
// Bloom, exposure, grade (gain with the white balance, shadows / highlights gains), AgX tone mapping with a look
// (contrast / saturation), vignette, dither. pc.flags 1: gamma-encoded output for the painterly pass, which adds
// the vignette and dither itself.
#include "common.glsl"
#include "post.glsl"   // postInput: the top of the bloom chain (half resolution, sampled bilinearly)
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
    float luma = dot(v, LUMA);
    v = pow(max(v * frame.tonemap.w, vec3(0.0)), vec3(frame.tonemap.y));
    return luma + frame.tonemap.z * (v - luma);
}

void main() {
    vec3 c = texture(hdrColor, vUv).rgb;
    if (frame.bloom.x > 0.0) c += texture(postInput, vUv).rgb * frame.bloom.x;   // before exposure, like the Blender compositor
    c *= frame.tonemap.x;
    // grade in scene-linear light: gain, then shadows / highlights gains weighted by luminance (Unreal's ranges)
    c *= frame.gain.rgb;
    float l = dot(c, LUMA);
    float ws = 1.0 - smoothstep(0.0, 0.09, l), wh = smoothstep(0.5, 1.0, l);
    c *= frame.shadowsGain.rgb * ws + frame.highlightsGain.rgb * wh + (1.0 - ws - wh);
    c = agxEotf(agxLook(agx(c)));
    outColor = vec4(pc.flags == 1u ? pow(c, vec3(1.0 / 2.2)) : finish(c, vUv), 1.0);
}
