// Shared by the post passes (bloom.frag, post.frag, painterly.frag). Include after common.glsl.
layout(set = 1, binding = 0) uniform sampler2D postInput;   // the pass's input (Renderer::postSets_)

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// vignette, then a triangular dither against banding in 8-bit output (display-linear colour)
vec3 finish(vec3 c, vec2 uv) {
    vec2 q = uv * 2.0 - 1.0;
    float vig = 1.0 - smoothstep(0.55, 1.45, length(q * vec2(0.95, 0.8)));
    c *= mix(1.0, vig, frame.post.x);
    float r = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) + fract(sin(dot(gl_FragCoord.xy, vec2(39.3468, 11.135))) * 24634.6345) - 1.0;
    return c + r * frame.post.y / 255.0;
}
