#version 460
#extension GL_GOOGLE_include_directive : require
// Bloom chain (Jimenez 2014, "Next generation post processing in Call of Duty: Advanced Warfare"; Blender's Glare
// node Bloom works the same way). pc.flags 0: soft threshold + 13-tap downsample of the HDR image, Karis-weighted
// (no fireflies); 1: 13-tap downsample; 2: tent upsample, blended additively onto the level above.
#include "common.glsl"
#include "post.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// what blooms: the brightest channel's excess over the threshold (before exposure) with a soft knee, capped
vec3 highlights(vec3 c) {
    float br = max(c.r, max(c.g, c.b)), k = frame.bloom.z, x = br - frame.bloom.y;
    float s = clamp(x + k, 0.0, 2.0 * k);
    return c * (min(max(s * s / (4.0 * k), x), frame.bloom.w) / max(br, 1e-4));
}

// 3x3 tent over postInput, one input texel wide
vec3 tent(vec2 uv) {
    vec2 t = 1.0 / vec2(textureSize(postInput, 0));
    vec3 s = texture(postInput, uv).rgb * 4.0;
    s += (texture(postInput, uv + vec2(t.x, 0.0)).rgb + texture(postInput, uv - vec2(t.x, 0.0)).rgb +
          texture(postInput, uv + vec2(0.0, t.y)).rgb + texture(postInput, uv - vec2(0.0, t.y)).rgb) * 2.0;
    s += texture(postInput, uv + t).rgb + texture(postInput, uv - t).rgb + texture(postInput, uv + vec2(t.x, -t.y)).rgb +
         texture(postInput, uv + vec2(-t.x, t.y)).rgb;
    return s * (1.0 / 16.0);
}

vec3 tap(vec2 uv) {
    vec3 c = texture(postInput, uv).rgb;
    return pc.flags == 0u ? highlights(c) : c;
}

void main() {
    if (pc.flags == 2u) { outColor = vec4(tent(vUv), 1.0); return; }
    vec2 t = 1.0 / vec2(textureSize(postInput, 0));
    vec3 a = tap(vUv + t * vec2(-2, -2)), b = tap(vUv + t * vec2(0, -2)), c = tap(vUv + t * vec2(2, -2));
    vec3 d = tap(vUv + t * vec2(-1, -1)), e = tap(vUv + t * vec2(1, -1));
    vec3 f = tap(vUv + t * vec2(-2, 0)), g = tap(vUv), h = tap(vUv + t * vec2(2, 0));
    vec3 i = tap(vUv + t * vec2(-1, 1)), j = tap(vUv + t * vec2(1, 1));
    vec3 k = tap(vUv + t * vec2(-2, 2)), l = tap(vUv + t * vec2(0, 2)), m = tap(vUv + t * vec2(2, 2));
    // five overlapping 4x4-texel boxes: the centre one weighs 1/2, the four corner ones 1/8 each
    vec3 box[5] = vec3[](d + e + i + j, a + b + f + g, b + c + g + h, f + g + k + l, g + h + l + m);
    vec3 sum = vec3(0.0);
    float ws = 0.0;
    for (int n = 0; n < 5; ++n) {
        vec3 x = box[n] * 0.25;
        float w = n == 0 ? 0.5 : 0.125;
        if (pc.flags == 0u) w /= 1.0 + dot(x, LUMA);   // Karis average
        sum += x * w;
        ws += w;
    }
    outColor = vec4(sum / ws, 1.0);
}
